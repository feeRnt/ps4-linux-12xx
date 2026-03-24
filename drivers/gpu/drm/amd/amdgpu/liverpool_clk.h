/* SPDX-License-Identifier: MIT */
/**
 * liverpool_clk.h” PS4 Liverpool/Gladius GPU clock force header
 *
 * Copyright (C) rmux <armandas.kvietkus@proton.me>
 *
 * Based on ci_dpm.c (AMD GCN DPM framework).
 * Thanks to fail0verflow for ps4-kexec and the Liverpool register map.
 * Thanks to the ps4-linux community for hardware research and testing.
 *
 * ============================================================
 * Hardware Compatibility
 * ============================================================
 * Confirmed on:
 *   PS4 Original / Fat  ” Liverpool GPU (Aeolia  CXD90025G)
 *   PS4 Slim            ” Liverpool GPU (Belize  CXD90036G, protocol identical)
 *   PS4 Pro             ” Gladius GPU   (Baikal  CXD90042GG, untested)
 *
 * ============================================================
 * Register Map (GCK block, SMC indirect ” bonaire.rai)
 * ============================================================
 * All registers accessed via RREG32_SMC / WREG32_SMC.
 *
 * GCK_SCLK_FUSES         0xc0500004  (RO, fuse-burnt)
 *   bits [6:0]   StartupSClkDid     ” DID applied after GFX soft-reset
 *
 * GCK_SPLL_FUSES         0xc050002c  (RO, fuse-burnt)
 *   bits [6:1]   SPLLFreqIdStartup  ” SPLL startup frequency ID
 *   bits [12:7]  SPLLFreqIdMax      ” SPLL maximum frequency ID
 *
 * CG_SCLK_CNTL           0xc050008c  (RW)
 *   bits [6:0]   SCLKDIVIDER        ” SMU-mediated DID
 *   bit  [8]     SCLKDIRCNTLEN      ” enable direct-control mode
 *   bit  [9]     SCLKDIRCNTLTOG     ” toggle to commit direct-control change
 *   bits [16:10] SCLKDIRCNTLDIVIDER ” direct-control DID
 *
 * CG_SCLK_STATUS         0xc0500090  (RO)
 *   bit [0]  SCLKSTATUS          ” 1 = SMU request complete
 *   bit [1]  SCLKFORCESTATUS     ” 1 = force request complete
 *   bit [3]  SCLKDIRCNTLDONETOG  ” mirrors SCLKDIRCNTLTOG when done
 *
 * CG_SPLL_FUNC_CNTL      0xc0500100  (RW)
 *   bits [26:20] SPLLPDIVA  ” post-divider A (1=A·1=max, 2=A·2, ...)
 *
 * CG_SPLL_FUNC_CNTL_3    0xc0500108  (RW)
 *   bits [25:0]  SPLLFBDIV  ” VCO feedback divider
 *
 * CG_SPLL_STATUS         0xc0500114  (RO)
 *   bit [7]  SPLLUNLOCKSTICKY  ” sticky PLL unlock flag
 *
 * SCLK_STARTUP_DID       0xe000304c  (SMC scratch)
 *   bits [6:0]  ” last startup DID applied by SMC firmware
 */
#ifndef LIVERPOOL_CLK_H
#define LIVERPOOL_CLK_H

struct amdgpu_device;

#ifdef CONFIG_DRM_AMDGPU_CIK
int liverpool_clk_force_max(struct amdgpu_device *adev);
#else
static inline int liverpool_clk_force_max(struct amdgpu_device *adev)
{ return 0; }
#endif

#endif /* LIVERPOOL_CLK_H */
