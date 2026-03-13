# PS4 Aeolia sky2 Networking — Research & Fix Document

**Last updated:** Sat Mar 14 2026 — patch v1 deployed, residual storm confirmed, race window identified, patch v2 planned

---

## Repositories

| Repo | Branch | State |
|------|--------|-------|
| https://github.com/rmuxnet/ps4-linux-12xx | ps4-linux-6.15.4-aeolia-belize-crashniels | Active dev — patch v1 merged, v2 pending |
| https://github.com/feeRnt/ps4-linux-12xx | ps4-linux-6.15.4-aeolia-belize-crashniels | Stock unpatched — ground truth |

---

## System Context

| Item | Value |
|------|-------|
| Goal | Headless server (Minecraft + userbot + IPTV forwarder). Network is everything. Display irrelevant. |
| PS4 firmware | 12.52 |
| Kernel | 6.15.4-Strawberry-ThinLTO-g03a1b2e4dc17-dirty (patch v1) |
| Root device | /dev/sda2 ext4 on Kingston SA400S37 120GB SSD via USB-UAS (Ugreen) |
| PS4 HDD | /dev/sdb 500GB — sdb27 dm-crypt → ps4hdd btrfs (internal SCEI USB-SATA bridge) |
| SSH port | 17702 |
| WiFi | mlan0 — mwifiex_sdio — 192.168.1.31/24 — working |
| GbE | enp0s20f1 — sky2 — 0000:00:14.1 |
| ISP | Iliad Italia |

---

## Hardware Architecture

### Aeolia Southbridge (CXD90025G)

Sony custom SoC:
- **EAP** (Execution Application Processor): Marvell PJ4C B0, ARMv7 Cortex-A8, 500MHz — runs Orbis GbE stack under FreeBSD 9
- **EMC**: ARM Cortex-M3 r2p1 (cpuid 412FC231), ~100MHz — power/boot, ICC bridge to x86, runs its own FreeBSD kernel
- Acts as gatekeeper between AMD Jaguar x86 APU and all peripherals

**Critical context — Orbis OS network offload:** In Orbis OS, all network I/O is fully offloaded to the Aeolia EAP. The AMD Jaguar x86 CPU never touches network processing. This is why the hardware deviates from standard Yukon-2 PCIe behaviour: the interrupt and DMA model is the southbridge's own design. Under Linux, the x86 CPU takes over via sky2 with no offload layer, which is why missing ICR mask/unmask is catastrophic — there is nothing to absorb the storm.

### The NIC

- Marvell Yukon-2 **OptimaEEE**, chip type `0xbd` rev 1, model 88EC060-NN82
- Embedded inside Aeolia — not discrete
- PCI address: **`0000:00:14.1`** (AEOLIA_FUNC_ID_GBE = 1)
- Driver: `sky2` (built-in, CONFIG_SKY2=y)
- PHY addr 1, possible L2 switch at addr 2
- BAR0: `c4000000`, 16KB
- IRQ: **24** (IR-Aeolia-MSI 256-edge)
- Single queue, single NAPI instance — no RSS/multi-queue support

### Function 4 — MSI Proxy ("glue")

- PCI: **`0000:00:14.4`** (AEOLIA_FUNC_ID_PCIE = 4)
- Centralised interrupt controller for all 8 Aeolia sub-functions
- BAR4 physical base: `0xd0200000`
- GbE MMIO window mapped: `0xbfa00000`, 16KB (set in `apcie_glue_init`)
- **Ignores standard PCIe MSI affinity writes** — CPU targeting is controlled through BAR4, not APIC MSI message

### MSI Register Layout (BAR4-relative)

```c
#define APCIE_RGN_PCIE_BASE       0x1c8000
#define APCIE_REG_MSI_MASK(func)  (APCIE_RGN_PCIE_BASE + 0x40c + ((func) << 2))
// func=1 (GbE) → 0x1c8410 → confirmed in dmesg: glue_write32(00400000, 001c8410, ...)
```

---

## Register Side-Effect Anomalies (Confirmed)

| Register | Offset | Standard Yukon-2 | Aeolia Actual |
|----------|--------|------------------|---------------|
| B0_Y2_SP_ISRC2 | 0x001C | Read masks IRQs | **No masking** |
| B0_Y2_SP_LISR  | 0x0028 | Read unmasks IRQs | **No unmasking** |
| B0_Y2_SP_EISR  | 0x0024 | Non-destructive read | **Destructive — clears on read** |
| B0_Y2_SP_ICR   | 0x0068 | N/A (upstream offset 0x002c unused) | Works: write 2=mask, 1=unmask |

**ICR offset note:** Upstream `sky2.h` defines `B0_Y2_SP_ICR = 0x002c`. This offset has no effect on Aeolia. The working ICR is at `0x0068`. Defined as `AEOLIA_SP_ICR = 0x0068` under `CONFIG_X86_PS4` in sky2.h.

---

## Why The Storm Happens

`sky2_intr` reads `B0_Y2_SP_ISRC2` expecting it to mask interrupts as a side effect (standard Yukon-2). Aeolia ignores this. Interrupt line stays asserted. CPU exits hard IRQ, LAPIC immediately re-enters. Infinite loop. Root cause is one missing ICR write.

---

## Diagnostic Data

### Diag-1 — Unpatched kernel, earlier session

| Metric | Value |
|--------|-------|
| IRQ 24 count | **398,772,495** on CPU1 |
| CPU1 softirq | ~39% |
| enp0s20f1 | UP, 1000Mbps, no IPv4 |

### Diag-2 — Unpatched kernel, Sun Mar 8 13:43 EDT

**Boot:** 13:38:39. Diag: 13:43:02. **Uptime: ~4.5 minutes.**

| Metric | Value | Notes |
|--------|-------|-------|
| Kernel | 6.15.4-crashnt-3 | **Unpatched** |
| IRQ 24 count | **26,861,075** on CPU1 | ~100k/sec |
| CPU1 softirq | 7379 / ~10k jiffies ≈ **70%** | Pathological |
| enp0s20f1 | UP, 1000Mbps, link detected | PHY working |
| enp0s20f1 IPv4 | None — only fe80:: | NM DHCP cycling |
| enp0s20f1 RX | 13211 bytes / 70 pkts (all mcast) | NAPI runs between re-entries |
| enp0s20f1 TX | 8036 bytes / 50 pkts | TX path functional |
| Lockdep / BUG | None | Clean |
| noirqdebug | Yes | IRQ lockup detection disabled |

**Slab allocator evidence (collected while storm active):**
```
202816  202816  100%  2.00K  12676  16  405632  kmalloc-2k
```
405MB+ in 2KB chunks — sk_buff data payload. Additional ~200MB in kmalloc-512 (sk_buff metadata). Total slab growth reached ~2.74GB before reboot. Not a traditional memory leak — storm backpressure visible in the slab.

**Data summary:** Everything works except the storm. PHY init, MDIO, link negotiation, EISR delivery, RX/TX — all functional on unpatched stock.

### Diag-3 — Patch v1 deployed, Sat Mar 14 06:28 EDT

**Kernel:** 6.15.4-Strawberry-ThinLTO-g03a1b2e4dc17-dirty. **Uptime: 12h23m.**

| Metric | Value | Notes |
|--------|-------|-------|
| IRQ 24 | 319,382,539 → 319,404,105 in 3s | **~7,200/sec** — residual storm |
| IRQ 24 dominant CPU | CPU0 (~313M) | Affinity move worked at kernel level |
| Memory | 920Mi used / 6.7Gi total | **Slab bloat gone** — clean overnight |
| CPU sy idle | ~13% constant | ksoftirqd/1 still pegged |
| ksoftirqd/1 CPU time | **741 minutes** over 12h | Persistent softirq load |
| NET_RX softirq | CPU1 >> all others | Aeolia MSI hardwired to CPU1 |
| Download | ~935–942 Mbps | Consistent near line rate across all servers |
| Upload | 76–672 Mbps | **Server-dependent ISP routing variance** — Iliad Italia direct: ~672 Mbps; third-party servers: 76–107 Mbps. Not a NIC issue. |
| Packet loss | 0.0% | Clean |
| Real traffic at idle | ~30 pkt/sec | Confirmed via /proc/net/dev delta |
| RX FIFO overflow | 16 | Minor, non-critical |

**tcpdump captured 0 packets** during idle period — hardware generating spurious IRQs with no real frames.

**IRQ affinity:** `/proc/irq/24/smp_affinity` write to CPU0 accepted by kernel (`effective_affinity_list: 0`) but Aeolia MSI proxy ignores it at hardware level and keeps firing on CPU1. RPS set to `ff` had minimal effect — sky2's single NAPI instance means softirq processing stays on the CPU where the MSI fires.

---

## Hardware Config (from lspci diag-2)

| Function | PCI | IRQ | BAR | Driver |
|----------|-----|-----|-----|--------|
| ACPI | 00:14.0 | — | fdf8000000 (32M) | — |
| **GbE** | **00:14.1** | **24** | **c4000000 (16K)** | **sky2** |
| SATA AHCI | 00:14.2 | 31 | c8000000 (4K) | ahci |
| SD/MMC | 00:14.3 | 28 | cc000000 (4K) | sdhci-pci |
| PCIe Glue | 00:14.4 | 1 | d0000000/d0100000/d0200000 | aeolia_pcie |
| DMA | 00:14.5 | — | d4000000 (4K) | — |
| DDR3/SPM | 00:14.6 | — | d8000000 / 80000000 (1G) | — |
| xHCI USB3 | 00:14.7 | 25/26/27 | dc000000/dc200000/dc400000 | xhci_aeolia |

---

## Patch History

### Patches 1–7 + two-script minimal approach — Scrapped

See pre-Mar-13 history. All abandoned due to flag-guard issues, EISR destruction, PHY deadlocks.

### Patch v1 — Merged commit 03a1b2e (Fri Mar 13 2026)

**Changes:**
- `sky2.h`: Added `AEOLIA_SP_ICR = 0x0068` under `#ifdef CONFIG_X86_PS4`
- `sky2_intr`: Added `sky2_write32(hw, AEOLIA_SP_ICR, 2)` after `prefetch()`, before `napi_schedule()`
- `sky2_poll`: Added `sky2_write32(hw, AEOLIA_SP_ICR, 1)` after `sky2_read32(hw, B0_Y2_SP_LISR)`

**Result:**
- Slab bloat eliminated — memory stable overnight
- GbE functional, DHCP working, link stable
- IRQ storm **reduced but not eliminated** — ~7,200 spurious IRQs/sec remain at idle

**Why v1 is incomplete — the race window:**

```c
status = sky2_read32(hw, B0_Y2_SP_ISRC2);  /* no mask side-effect on Aeolia */
/* ← Aeolia can re-assert IRQ here, before ICR write */
/* ← each re-assertion queues another sky2_intr entry */
sky2_write32(hw, AEOLIA_SP_ICR, 2);         /* mask — too late */
```

Between the ISRC2 read and the ICR=2 write there is a small window where Aeolia re-asserts the interrupt line. Each re-assertion queues another hard IRQ entry. At ~30 real packets/sec this produces ~7,200 spurious IRQs/sec — roughly 240 spurious firings per real packet.

---

## Patch v2 — Planned

**Single change:** Move the `AEOLIA_SP_ICR` mask write to **before** the ISRC2 read, closing the race window entirely.

```c
static irqreturn_t sky2_intr(int irq, void *dev_id)
{
    struct sky2_hw *hw = dev_id;
    u32 status;

#ifdef CONFIG_X86_PS4
    /* Aeolia: mask immediately on entry before any register read.
     * ISRC2 read has no mask side-effect; closing the race window
     * requires masking before status is sampled. */
    sky2_write32(hw, AEOLIA_SP_ICR, 2);
#endif

    /* Reading this masks interrupts as side effect (standard Yukon-2 only) */
    status = sky2_read32(hw, B0_Y2_SP_ISRC2);
    if (status == 0 || status == ~0) {
        sky2_write32(hw, B0_Y2_SP_ICR, 2);
        return IRQ_NONE;
    }

    prefetch(&hw->st_le[hw->st_idx]);

    napi_schedule(&hw->napi);

    return IRQ_HANDLED;
}
```

`sky2_poll` unmask stays unchanged from v1.

**Expected result:** Zero re-assertions between entry and mask. Spurious IRQ rate should drop to near zero at idle.

---

## Longer-term: CPU1 Affinity (Post-v2)

Even after v2 fixes the residual storm, all NET_RX softirq work will remain on CPU1 because Aeolia's MSI proxy (`00:14.4`) controls interrupt routing through BAR4 and ignores standard PCIe MSI affinity writes. The kernel's `/proc/irq/24/smp_affinity` write is accepted but has no hardware effect.

**Proper fix:** `aeolia_pcie` driver needs an `irq_set_affinity` callback that writes CPU targeting through BAR4 registers. Deferred until after v2 is confirmed working.

---

## What went wrong in prior attempts

| Attempt | Problem |
|---------|---------|
| ICR writes guarded by `SKY2_HW_USE_AEOLIA_MSI` flag | Flag never set → writes never execute |
| Removing flag guard | Correct idea but needs clean implementation |
| EISR drain patches | Destroyed PHY link events (destructive read) |
| sky2_phy_intr from hard IRQ | Deadlock risk with phy_lock |
| sky2_phy_intr from NAPI softirq | DUPLEX_UNKNOWN |
| Shadow register / EISR accumulator | Overcomplicated, never tested |
| Patch v1 ICR mask after ISRC2 read | Race window — ~240 spurious IRQs per real packet remain |

---

## EISR Bit Reference

| Bit | Macro | Meaning |
|-----|-------|---------|
| 31 | Y2_IS_HW_ERR | Hardware error |
| 30 | Y2_IS_STAT_BMU | Status BMU interrupt |
| 12 | Y2_IS_IRQ_PHY2 | PHY 2 link state change |
| 4  | Y2_IS_IRQ_PHY1 | **PHY 1 link state change** ← critical |
| 3  | Y2_IS_IRQ_MAC1 | MAC 1 interrupt |

Y2_IS_ERROR does **not** include bits 4 or 12. Do not drain EISR — stock handles it correctly.

---

## ICC Command Reference

| Major | Minor | Data | Purpose |
|-------|-------|------|---------|
| 1 | 0 | 0x10 | Service init |
| 2 | 6 | — | Get EMC firmware version |
| 4 | 1 | 6-byte cmd | Power off / reboot |
| 5 | 0 | 2=off, 3=on | BT/WLAN power |
| 5 | 0x10 | 0=off, 1=on | USB power |
| 9 | 0x20 | led struct | LED config |

No GbE ICC command exists. GbE is left powered by Orbis OS before kexec. ICC is not a factor.

---
