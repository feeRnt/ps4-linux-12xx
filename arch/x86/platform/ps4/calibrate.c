/*
 * calibrate.c: Sony PS4 TSC/LAPIC calibration
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#define pr_fmt(fmt) "ps4: " fmt

#include <linux/jiffies.h>
#include <asm/io.h>
#include <asm/msr.h>
#include <asm/ps4.h>
#include <asm/delay.h>
#include <asm/apic.h>

/* The PS4 southbridge (Aeolia) has an EMC timer that ticks at 32.768kHz,
 * which seems to be an appropriate clock reference for calibration. Both TSC
 * and the LAPIC timer are based on the core clock frequency and thus can be
 * calibrated together. */
static void __iomem *emc_timer = NULL;

static __init inline u32 emctimer_read32(unsigned int reg)
{
	return ioread32(emc_timer + reg);
}

static __init inline void emctimer_write32(unsigned int reg, u32 val)
{
	iowrite32(val, emc_timer + reg);
}

static __init inline u32 emctimer_read(void)
{
	u32 t1, t2;
	t1 = emctimer_read32(EMC_TIMER_VALUE);
	while (1) {
		t2 = emctimer_read32(EMC_TIMER_VALUE);
		if (t1 == t2)
			return t1;
		t1 = t2;
	}
}

static __init unsigned long ps4_measure_tsc_freq(void)
{
	unsigned long ret = 0;
	u32 t1, t2;
	u64 tsc1, tsc2;

	u32 readout;
	bool is_aeolia_emc = false;
	bool is_baikal_emc = false;

	// This is part of the Aeolia pcie device, but it's too early to
	// do this in a driver.

	// With at least one Belize, this does not crash the system. We don't know if the same happens on Baikal, but we don't need to.
	// Just test with Baikal PCIE EMC base first, then switch to Belize.
	emc_timer = early_ioremap(BPCIE_EMC_TIMER_BASE, 0x100);
	if (!emc_timer)
		goto fail;
	readout = emctimer_read32(0); // read 32 bits from Baikal EMC base address --- Returns 0xffffffff on CUH-1216 Belize B0(0x20200)
	pr_info("ps4-calibrate: emctimer_read32(BPCIE_EMC_TIMER_BASE + 0) = %08x\n", readout);
	readout = emctimer_read32(BPCIE_EMC_TIMER_ON_OFF); // read 32 bits from Baikal EMC On/Off address --- Returns 0xffffffff on CUH-1216 Belize B0(0x20200)
	pr_info("ps4-calibrate: emctimer_read32(BPCIE_EMC_TIMER_BASE + BPCIE_EMC_TIMER_ON_OFF) = %08x\n", readout);
	if (readout != 0xffffffff) {
		goto baikal_emc_init;
	}

	early_iounmap(emc_timer, 0x100);
	emc_timer = NULL;

	//emc_timer = ioremap(EMC_TIMER_BASE, 0x100); //replaced with early_ioremap for early mmapping, maybe needed on newer kernels
	emc_timer = early_ioremap(EMC_TIMER_BASE, 0x100); // TODO: change this to Baikal base

	if (!emc_timer)
		goto fail;

	/* Try reading both Aeolia and Baikal base addresses, and establish is_aeolia_emc or is_baikal_emc from the readouts */
	readout = emctimer_read32(0); // read 32 bits from Aeolia EMC base address
	/* if (readout != 0xffffffff") {
		is_baikal_emc = true;
		goto baikal_emc_init;
	}*/
	pr_info("ps4-calibrate: emctimer_read32(EMC_TIMER_BASE + 0) = %08x\n", readout); // TODO: Remove when done checking values
	readout = emctimer_read32(EMC_TIMER_ON_OFF); // read 32 bits from Aeolia EMC On/Off address
	pr_info("ps4-calibrate: emctimer_read32(EMC_TIMER_BASE + EMC_TIMER_ON_OFF) = %08x\n", readout); // TODO: Remove when done checking values
	goto aeolia_emc_init; // TODO: remove this when done with these tests

	// if Aeolia timer returns non-identifying value, assume Baikal
	if (emc_timer) {
		early_iounmap(emc_timer, 0x100);
		emc_timer = NULL;
	}

	emc_timer = early_ioremap(BPCIE_EMC_TIMER_BASE, 0x100); // TODO: change this to Aeolia base

	if (!emc_timer)
		goto fail;

	readout = emctimer_read32(0); // read 32 bits from Baikal EMC base address
	/* if (readout != 0xffffffff) {
		is_aeolia_emc = true;
		goto is_aeolia_emc;
	}*/
	pr_info("ps4-calibrate: emctimer_read32(EMC_TIMER_BASE + 0) = %08x\n", readout); // TODO: Remove when done checking values

	if (is_aeolia_emc == false && is_baikal_emc == false) {
		pr_err("ps4-calibrate: Could not identify Southbridge EMC at early_setup. Returning ENODEV.\n");
		return -ENODEV;
	}

aeolia_emc_init:
	// reset/start the timer
	emctimer_write32(EMC_TIMER_ON_OFF, emctimer_read32(EMC_TIMER_ON_OFF) & (~0x01));

	// udelay is not calibrated yet, so this is likely wildly off, but good
	// enough to work.
	udelay(300);

	emctimer_write32(0x00, emctimer_read32(0x00) | 0x01);
	emctimer_write32(EMC_TIMER_ON_OFF, emctimer_read32(EMC_TIMER_ON_OFF) | 0x01);
	goto common_emc_init;

baikal_emc_init:
	// reset/start the timer
	emctimer_write32(BPCIE_EMC_TIMER_ON_OFF, emctimer_read32(BPCIE_EMC_TIMER_ON_OFF) & 0xFFFFFFC8 | 0x32);

	// udelay is not calibrated yet, so this is likely wildly off, but good
	// enough to work.
	udelay(300);

	emctimer_write32(BPCIE_EMC_TIMER_RESET_VALUE, emctimer_read32(BPCIE_EMC_TIMER_RESET_VALUE) & 0xFFFFFFE0 | 0x10);
	emctimer_write32(BPCIE_EMC_TIMER_ON_OFF, emctimer_read32(BPCIE_EMC_TIMER_ON_OFF) | 0x33);
	goto common_emc_init;

common_emc_init:
	t1 = emctimer_read();
	tsc1 = tsc2 = rdtsc();

	while (emctimer_read() == t1) {
		// 0.1s timeout should be enough
		tsc2 = rdtsc();
		if ((tsc2 - tsc1) > (PS4_DEFAULT_TSC_FREQ/10)) {
			pr_warn("EMC timer is broken.\n");
			goto fail;
		}
	}
	pr_info("EMC timer started in %lld TSC ticks\n", tsc2 - tsc1);

	// Wait for a tick boundary
	t1 = emctimer_read();
	while ((t2 = emctimer_read()) == t1);
	tsc1 = rdtsc();

	// Wait for 1024 ticks to elapse (31.25ms)
	// We don't need to wait very long, as we are looking for transitions.
	// At this value, a TSC uncertainty of ~50 ticks corresponds to 1ppm of
	// clock accuracy.
	while ((emctimer_read() - t2) < 1024);
	tsc2 = rdtsc();

	// TSC rate is 32 times the elapsed time
	ret = (tsc2 - tsc1) * 32;

	pr_info("Calibrated TSC frequency: %ld kHz\n", ret);
fail:
	if (emc_timer) {
		//iounmap(emc_timer); // replaced with early_iounmap(emc_timer, 0x100); (the mapping-size=0x100 should be explicit in early_iounmap)
		early_iounmap(emc_timer, 0x100);
		emc_timer = NULL;
	}
	return ret;
}

unsigned long __init ps4_calibrate_tsc(void)
{
	unsigned long tsc_freq = ps4_measure_tsc_freq();

	if (!tsc_freq) {
		pr_warn("Unable to measure TSC frequency, assuming default.\n");
		tsc_freq = PS4_DEFAULT_TSC_FREQ;
	}

	lapic_timer_period = (tsc_freq + 8 * HZ) / (16 * HZ);

	return (tsc_freq + 500) / 1000;
}
