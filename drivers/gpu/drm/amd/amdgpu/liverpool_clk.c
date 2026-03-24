// SPDX-License-Identifier: MIT
/**
 * liverpool_clk.c” PS4 Liverpool/Gladius GPU clock force driver
 *
 * Copyright (C) rmux <armandas.kvietkus@proton.me>
 *
 * Based on ci_dpm.c (AMD GCN DPM framework).
 * Thanks to fail0verflow for ps4-kexec and the Liverpool register map.
 * Thanks to the ps4-linux community for hardware research and testing.
 *
 * ============================================================
 * Root Cause
 * ============================================================
 * ps4-kexec performs a GFX soft-reset (RCU_GFX_STRAP |= 0x10003)
 * before handing off to Linux. On GCN hardware a GFX soft-reset
 * causes the SCLK divider (DID) to revert to the fuse-burnt startup
 * value in GCK_SCLK_FUSES.StartupSClkDid, which corresponds to the
 * strap/boot frequency (~200-300 MHz on Liverpool).
 *
 * pp_smu_ip_block is present in the IP block list for CHIP_LIVERPOOL
 * and CHIP_GLADIUS but produces no functional DPM table and has never
 * raised the clock across any ps4-linux rebase. The GPU runs at strap
 * speed permanently.
 *
 * ============================================================
 * Fix
 * ============================================================
 * Use the CG_SCLK_CNTL direct-control path (SCLKDIRCNTLEN +
 * SCLKDIRCNTLTOG) to force SCLKDIRCNTLDIVIDER = 1 (DID 1, divide-
 * by-1, full SPLL output) from cik_common_hw_init(), before any GFX
 * ring or shader work begins.
 *
 * SCLKDIRCNTLEN is kept set permanently. This prevents any dormant
 * DPM path from lowering the clock autonomously.
 *
 * ============================================================
 * SPLL State After Soft-Reset
 * ============================================================
 * A GFX soft-reset does not reconfigure the GCK PLL block (separate
 * power domain managed by SMC firmware). The SPLL remains locked to
 * the operating frequency (~800 MHz Liverpool, ~911 MHz Gladius) set
 * by the BIOS/hypervisor before kexec. Only the post-divider (DID)
 * is reset. Forcing DID=1 restores the full SPLL output.
 *
 * If SPLL PDIVA > 4 at init time the SPLL may have been reconfigured
 * by the soft-reset. The function logs PDIVA and FBDIV in that case
 * for follow-up SPLL reprogramming support.
 *
 * ============================================================
 * Voltage
 * ============================================================
 * The BIOS/hypervisor that configured the SPLL also set VID rails to
 * operational levels before kexec. A GFX soft-reset does not touch
 * the power plane.
 *
 * The EMC (Aeolia/Belize/Baikal, ARM Cortex-M3) provides an
 * independent hardware thermal kill at 72 C (GPU domain, ICC command
 * 0x0B/0x05 domain 2) and 97 C (APU shutdown) regardless of what
 * Linux writes into the clock registers.
 *
 * ============================================================
 * Register Reference
 * ============================================================
 * All registers accessed via RREG32_SMC / WREG32_SMC (SMC indirect
 * at mmSMC_IND_INDEX_0 / mmSMC_IND_DATA_0, initialised in
 * cik_common_early_init before this function is called).
 * Source: bonaire.rai, GCK block. See liverpool_clk.h for field map.
 */

#include <linux/delay.h>
#include "amdgpu.h"
#include "liverpool_clk.h"

#define GCK_SCLK_FUSES                  0xc0500004
#define   STARTUP_SCLK_DID_SHIFT          0
#define   STARTUP_SCLK_DID_MASK           (0x7f << 0)

#define GCK_SPLL_FUSES                  0xc050002c
#define   SPLL_FREQ_ID_STARTUP_SHIFT      1
#define   SPLL_FREQ_ID_STARTUP_MASK       (0x3f << 1)
#define   SPLL_FREQ_ID_MAX_SHIFT          7
#define   SPLL_FREQ_ID_MAX_MASK           (0x3f << 7)

#define CG_SCLK_CNTL                    0xc050008c
#define   SCLK_DIVIDER_SHIFT              0
#define   SCLK_DIVIDER_MASK               (0x7f << 0)
#define   SCLK_DIRCNTL_EN                 BIT(8)
#define   SCLK_DIRCNTL_TOG                BIT(9)
#define   SCLK_DIRCNTL_DIV_SHIFT          10
#define   SCLK_DIRCNTL_DIV_MASK           (0x7f << 10)

#define CG_SCLK_STATUS                  0xc0500090
#define   SCLK_STATUS_DONE                BIT(0)
#define   SCLK_FORCE_STATUS_DONE          BIT(1)
#define   SCLK_DIRCNTL_DONE_TOG           BIT(3)

#define CG_SPLL_FUNC_CNTL               0xc0500100
#define   SPLL_PDIVA_SHIFT                20
#define   SPLL_PDIVA_MASK                 (0x7f << 20)

#define CG_SPLL_FUNC_CNTL_3             0xc0500108
#define   SPLL_FB_DIV_SHIFT               0
#define   SPLL_FB_DIV_MASK                (0x3ffffff << 0)

#define CG_SPLL_STATUS                  0xc0500114
#define   SPLL_UNLOCK_STICKY              BIT(7)

#define SCLK_STARTUP_DID                0xe000304c
#define   SCLKSTARTUPDID_MASK             0x7f

#define LIVERPOOL_TARGET_SCLK_DID       1
#define LIVERPOOL_CLK_TIMEOUT_US        10
#define LIVERPOOL_CLK_TIMEOUT_ITER      1000

/* ============================================================
 * liverpool_clk_force_max - Force GPU SCLK to maximum
 * ============================================================
 * @adev: amdgpu device pointer (CHIP_LIVERPOOL or CHIP_GLADIUS)
 *
 * Called from cik_common_hw_init() before any GFX or SDMA ring
 * work. Uses CG_SCLK_CNTL direct-control mode so the change takes
 * effect without going through the SMU message protocol.
 *
 * Logs SPLL state (PDIVA, FBDIV) at init time. A PDIVA > 4 warning
 * means the SPLL may need reprogramming â€” report the values.
 *
 * Returns 0 on success, -ETIMEDOUT if hardware does not respond.
 */
int liverpool_clk_force_max(struct amdgpu_device *adev)
{
	u32 spll_fuses, sclk_fuses, spll_cntl, spll_fb;
	u32 spll_freq_id_startup, spll_freq_id_max;
	u32 startup_did, current_did;
	u32 spll_pdiva, spll_fbdiv;
	u32 cntl, status;
	int i;

	spll_fuses = RREG32_SMC(GCK_SPLL_FUSES);
	sclk_fuses = RREG32_SMC(GCK_SCLK_FUSES);
	spll_cntl  = RREG32_SMC(CG_SPLL_FUNC_CNTL);
	spll_fb    = RREG32_SMC(CG_SPLL_FUNC_CNTL_3);

	spll_freq_id_startup = (spll_fuses & SPLL_FREQ_ID_STARTUP_MASK) >> SPLL_FREQ_ID_STARTUP_SHIFT;
	spll_freq_id_max     = (spll_fuses & SPLL_FREQ_ID_MAX_MASK)     >> SPLL_FREQ_ID_MAX_SHIFT;
	startup_did          = (sclk_fuses & STARTUP_SCLK_DID_MASK)     >> STARTUP_SCLK_DID_SHIFT;
	current_did          = RREG32_SMC(SCLK_STARTUP_DID) & SCLKSTARTUPDID_MASK;
	spll_pdiva           = (spll_cntl & SPLL_PDIVA_MASK)            >> SPLL_PDIVA_SHIFT;
	spll_fbdiv           = (spll_fb  & SPLL_FB_DIV_MASK)            >> SPLL_FB_DIV_SHIFT;

	dev_info(adev->dev,
		 "Liverpool CLK: SPLL startup_freq_id=%u max_freq_id=%u "
		 "SCLK startup_DID=%u current_DID=%u\n",
		 spll_freq_id_startup, spll_freq_id_max, startup_did, current_did);
	dev_info(adev->dev,
		 "Liverpool CLK: SPLL PDIVA=%u FBDIV=0x%x\n",
		 spll_pdiva, spll_fbdiv);

	if (spll_pdiva > 4)
		dev_warn(adev->dev,
			 "Liverpool CLK: SPLL PDIVA=%u > 4 â€” SPLL may have been "
			 "reconfigured by soft-reset. Report PDIVA+FBDIV.\n",
			 spll_pdiva);

	status = RREG32_SMC(CG_SPLL_STATUS);
	if (status & SPLL_UNLOCK_STICKY)
		dev_warn(adev->dev,
			 "Liverpool CLK: SPLL unlock sticky set (0x%08x)\n",
			 status);

	cntl = RREG32_SMC(CG_SCLK_CNTL);
	dev_info(adev->dev,
		 "Liverpool CLK: CG_SCLK_CNTL=0x%08x DIVIDER=%u DIRCNTLEN=%u DIRCNTLDIV=%u\n",
		 cntl,
		 (cntl & SCLK_DIVIDER_MASK)     >> SCLK_DIVIDER_SHIFT,
		 !!(cntl & SCLK_DIRCNTL_EN),
		 (cntl & SCLK_DIRCNTL_DIV_MASK) >> SCLK_DIRCNTL_DIV_SHIFT);

	if ((cntl & SCLK_DIRCNTL_EN) &&
	    ((cntl & SCLK_DIRCNTL_DIV_MASK) >> SCLK_DIRCNTL_DIV_SHIFT) == LIVERPOOL_TARGET_SCLK_DID) {
		dev_info(adev->dev, "Liverpool CLK: already at DID=1, no change.\n");
		return 0;
	}

	for (i = 0; i < LIVERPOOL_CLK_TIMEOUT_ITER; i++) {
		if (RREG32_SMC(CG_SCLK_STATUS) & SCLK_STATUS_DONE)
			break;
		udelay(LIVERPOOL_CLK_TIMEOUT_US);
	}
	if (i == LIVERPOOL_CLK_TIMEOUT_ITER) {
		dev_err(adev->dev,
			"Liverpool CLK: timed out waiting for SCLK idle (0x%08x)\n",
			RREG32_SMC(CG_SCLK_STATUS));
		return -ETIMEDOUT;
	}

	cntl &= ~SCLK_DIRCNTL_DIV_MASK;
	cntl |= (LIVERPOOL_TARGET_SCLK_DID << SCLK_DIRCNTL_DIV_SHIFT) & SCLK_DIRCNTL_DIV_MASK;
	cntl |= SCLK_DIRCNTL_EN;
	cntl ^= SCLK_DIRCNTL_TOG;
	WREG32_SMC(CG_SCLK_CNTL, cntl);

	for (i = 0; i < LIVERPOOL_CLK_TIMEOUT_ITER; i++) {
		status = RREG32_SMC(CG_SCLK_STATUS);
		if (status & (SCLK_DIRCNTL_DONE_TOG | SCLK_FORCE_STATUS_DONE))
			break;
		udelay(LIVERPOOL_CLK_TIMEOUT_US);
	}
	if (i == LIVERPOOL_CLK_TIMEOUT_ITER) {
		dev_err(adev->dev,
			"Liverpool CLK: timed out waiting for force done (0x%08x)\n",
			RREG32_SMC(CG_SCLK_STATUS));
		return -ETIMEDOUT;
	}

	cntl   = RREG32_SMC(CG_SCLK_CNTL);
	status = RREG32_SMC(CG_SCLK_STATUS);
	dev_info(adev->dev,
		 "Liverpool CLK: force done. CG_SCLK_CNTL=0x%08x DIRCNTLDIV=%u status=0x%08x\n",
		 cntl,
		 (cntl & SCLK_DIRCNTL_DIV_MASK) >> SCLK_DIRCNTL_DIV_SHIFT,
		 status);

	if (((cntl & SCLK_DIRCNTL_DIV_MASK) >> SCLK_DIRCNTL_DIV_SHIFT) != LIVERPOOL_TARGET_SCLK_DID)
		dev_warn(adev->dev,
			 "Liverpool CLK: readback DID mismatch â€” expected %u got %u\n",
			 LIVERPOOL_TARGET_SCLK_DID,
			 (cntl & SCLK_DIRCNTL_DIV_MASK) >> SCLK_DIRCNTL_DIV_SHIFT);

	return 0;
}
