// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  ahci.c - AHCI SATA support
 *
 *  Maintained by:  Tejun Heo <tj@kernel.org>
 *    		    Please ALWAYS copy linux-ide@vger.kernel.org
 *		    on emails.
 *
 *  Copyright 2004-2005 Red Hat, Inc.
 *
 * libata documentation is available via 'make {ps|pdf}docs',
 * as Documentation/driver-api/libata.rst
 *
 * AHCI hardware documentation:
 * http://www.intel.com/technology/serialata/pdf/rev1_0.pdf
 * http://www.intel.com/technology/serialata/pdf/rev1_1.pdf
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/gfp.h>
#include <linux/msi.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_cmnd.h>
#include <linux/libata.h>
#include <linux/ahci-remap.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include "ahci.h"

#ifdef CONFIG_X86_PS4
#include <asm/ps4.h>
#endif

#define DRV_NAME	"ahci"
#define DRV_VERSION	"3.0"

enum {
	AHCI_PCI_BAR_STA2X11	= 0,
	AHCI_PCI_BAR_CAVIUM	= 0,
	AHCI_PCI_BAR_LOONGSON	= 0,
	AHCI_PCI_BAR_ENMOTUS	= 2,
	AHCI_PCI_BAR_CAVIUM_GEN5	= 4,
	AHCI_PCI_BAR_STANDARD	= 5,
	AHCI_PCI_BAR0_BAIKAL = 0 //PS4 Baikal
};

enum board_ids {
	/* board IDs by feature in alphabetical order */
	board_ahci,
	board_ahci_ign_iferr,
	board_ahci_mobile,
	board_ahci_nomsi,
	board_ahci_noncq,
	board_ahci_nosntf,
	board_ahci_yes_fbs,

	/* board IDs for specific chipsets in alphabetical order */
	board_ahci_al,
	board_ahci_avn,
	board_ahci_mcp65,
	board_ahci_mcp77,
	board_ahci_mcp89,
	board_ahci_mv,
	board_ahci_sb600,
	board_ahci_sb700,	/* for SB700 and SB800 */
	board_ahci_vt8251,

	/*
	 * board IDs for Intel chipsets that support more than 6 ports
	 * *and* end up needing the PCS quirk.
	 */
	board_ahci_pcs7,

	/* aliases */
	board_ahci_mcp_linux	= board_ahci_mcp65,
	board_ahci_mcp67	= board_ahci_mcp65,
	board_ahci_mcp73	= board_ahci_mcp65,
	board_ahci_mcp79	= board_ahci_mcp77,
};

static int ahci_init_one(struct pci_dev *pdev, const struct pci_device_id *ent);
static void ahci_remove_one(struct pci_dev *dev);
static void ahci_shutdown_one(struct pci_dev *dev);
static int ahci_vt8251_hardreset(struct ata_link *link, unsigned int *class,
				 unsigned long deadline);
static int ahci_avn_hardreset(struct ata_link *link, unsigned int *class,
			      unsigned long deadline);
static void ahci_mcp89_apple_enable(struct pci_dev *pdev);
static bool is_mcp89_apple(struct pci_dev *pdev);
static int ahci_p5wdh_hardreset(struct ata_link *link, unsigned int *class,
				unsigned long deadline);
#ifdef CONFIG_PM
static int ahci_pci_device_runtime_suspend(struct device *dev);
static int ahci_pci_device_runtime_resume(struct device *dev);
#ifdef CONFIG_PM_SLEEP
static int ahci_pci_device_suspend(struct device *dev);
static int ahci_pci_device_resume(struct device *dev);
#endif
#endif /* CONFIG_PM */

static struct scsi_host_template ahci_sht = {
	AHCI_SHT("ahci"),
};

static struct ata_port_operations ahci_vt8251_ops = {
	.inherits		= &ahci_ops,
	.hardreset		= ahci_vt8251_hardreset,
};

static struct ata_port_operations ahci_p5wdh_ops = {
	.inherits		= &ahci_ops,
	.hardreset		= ahci_p5wdh_hardreset,
};

static struct ata_port_operations ahci_avn_ops = {
	.inherits		= &ahci_ops,
	.hardreset		= ahci_avn_hardreset,
};

static const struct ata_port_info ahci_port_info[] = {
	/* by features */
	[board_ahci] = {
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_ops,
	},
	[board_ahci_ign_iferr] = {
		AHCI_HFLAGS	(AHCI_HFLAG_IGN_IRQ_IF_ERR),
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_ops,
	},
	[board_ahci_mobile] = {
		AHCI_HFLAGS	(AHCI_HFLAG_IS_MOBILE),
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_ops,
	},
	[board_ahci_nomsi] = {
		AHCI_HFLAGS	(AHCI_HFLAG_NO_MSI),
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_ops,
	},
	[board_ahci_noncq] = {
		AHCI_HFLAGS	(AHCI_HFLAG_NO_NCQ),
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_ops,
	},
	[board_ahci_nosntf] = {
		AHCI_HFLAGS	(AHCI_HFLAG_NO_SNTF),
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_ops,
	},
	[board_ahci_yes_fbs] = {
		AHCI_HFLAGS	(AHCI_HFLAG_YES_FBS),
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_ops,
	},
	/* by chipsets */
	[board_ahci_al] = {
		AHCI_HFLAGS	(AHCI_HFLAG_NO_PMP | AHCI_HFLAG_NO_MSI),
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_ops,
	},
	[board_ahci_avn] = {
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_avn_ops,
	},
	[board_ahci_mcp65] = {
		AHCI_HFLAGS	(AHCI_HFLAG_NO_FPDMA_AA | AHCI_HFLAG_NO_PMP |
				 AHCI_HFLAG_YES_NCQ),
		.flags		= AHCI_FLAG_COMMON | ATA_FLAG_NO_DIPM,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_ops,
	},
	[board_ahci_mcp77] = {
		AHCI_HFLAGS	(AHCI_HFLAG_NO_FPDMA_AA | AHCI_HFLAG_NO_PMP),
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_ops,
	},
	[board_ahci_mcp89] = {
		AHCI_HFLAGS	(AHCI_HFLAG_NO_FPDMA_AA),
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_ops,
	},
	[board_ahci_mv] = {
		AHCI_HFLAGS	(AHCI_HFLAG_NO_NCQ | AHCI_HFLAG_NO_MSI |
				 AHCI_HFLAG_MV_PATA | AHCI_HFLAG_NO_PMP),
		.flags		= ATA_FLAG_SATA | ATA_FLAG_PIO_DMA,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_ops,
	},
	[board_ahci_sb600] = {
		AHCI_HFLAGS	(AHCI_HFLAG_IGN_SERR_INTERNAL |
				 AHCI_HFLAG_NO_MSI | AHCI_HFLAG_SECT255 |
				 AHCI_HFLAG_32BIT_ONLY),
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_pmp_retry_srst_ops,
	},
	[board_ahci_sb700] = {	/* for SB700 and SB800 */
		AHCI_HFLAGS	(AHCI_HFLAG_IGN_SERR_INTERNAL),
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_pmp_retry_srst_ops,
	},
	[board_ahci_vt8251] = {
		AHCI_HFLAGS	(AHCI_HFLAG_NO_NCQ | AHCI_HFLAG_NO_PMP),
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_vt8251_ops,
	},
	[board_ahci_pcs7] = {
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_ops,
	},
};

static const struct pci_device_id ahci_pci_tbl[] = {
	/* Intel */
	{ PCI_VDEVICE(INTEL, 0x06d6), board_ahci }, /* Comet Lake PCH-H RAID */
	{ PCI_VDEVICE(INTEL, 0x2652), board_ahci }, /* ICH6 */
	{ PCI_VDEVICE(INTEL, 0x2653), board_ahci }, /* ICH6M */
	{ PCI_VDEVICE(INTEL, 0x27c1), board_ahci }, /* ICH7 */
	{ PCI_VDEVICE(INTEL, 0x27c5), board_ahci }, /* ICH7M */
	{ PCI_VDEVICE(INTEL, 0x27c3), board_ahci }, /* ICH7R */
	{ PCI_VDEVICE(AL, 0x5288), board_ahci_ign_iferr }, /* ULi M5288 */
	{ PCI_VDEVICE(INTEL, 0x2681), board_ahci }, /* ESB2 */
	{ PCI_VDEVICE(INTEL, 0x2682), board_ahci }, /* ESB2 */
	{ PCI_VDEVICE(INTEL, 0x2683), board_ahci }, /* ESB2 */
	{ PCI_VDEVICE(INTEL, 0x27c6), board_ahci }, /* ICH7-M DH */
	{ PCI_VDEVICE(INTEL, 0x2821), board_ahci }, /* ICH8 */
	{ PCI_VDEVICE(INTEL, 0x2822), board_ahci_nosntf }, /* ICH8 */
	{ PCI_VDEVICE(INTEL, 0x2824), board_ahci }, /* ICH8 */
	{ PCI_VDEVICE(INTEL, 0x2829), board_ahci }, /* ICH8M */
	{ PCI_VDEVICE(INTEL, 0x282a), board_ahci }, /* ICH8M */
	{ PCI_VDEVICE(INTEL, 0x2922), board_ahci }, /* ICH9 */
	{ PCI_VDEVICE(INTEL, 0x2923), board_ahci }, /* ICH9 */
	{ PCI_VDEVICE(INTEL, 0x2924), board_ahci }, /* ICH9 */
	{ PCI_VDEVICE(INTEL, 0x2925), board_ahci }, /* ICH9 */
	{ PCI_VDEVICE(INTEL, 0x2927), board_ahci }, /* ICH9 */
	{ PCI_VDEVICE(INTEL, 0x2929), board_ahci_mobile }, /* ICH9M */
	{ PCI_VDEVICE(INTEL, 0x292a), board_ahci_mobile }, /* ICH9M */
	{ PCI_VDEVICE(INTEL, 0x292b), board_ahci_mobile }, /* ICH9M */
	{ PCI_VDEVICE(INTEL, 0x292c), board_ahci_mobile }, /* ICH9M */
	{ PCI_VDEVICE(INTEL, 0x292f), board_ahci_mobile }, /* ICH9M */
	{ PCI_VDEVICE(INTEL, 0x294d), board_ahci }, /* ICH9 */
	{ PCI_VDEVICE(INTEL, 0x294e), board_ahci_mobile }, /* ICH9M */
	{ PCI_VDEVICE(INTEL, 0x502a), board_ahci }, /* Tolapai */
	{ PCI_VDEVICE(INTEL, 0x502b), board_ahci }, /* Tolapai */
	{ PCI_VDEVICE(INTEL, 0x3a05), board_ahci }, /* ICH10 */
	{ PCI_VDEVICE(INTEL, 0x3a22), board_ahci }, /* ICH10 */
	{ PCI_VDEVICE(INTEL, 0x3a25), board_ahci }, /* ICH10 */
	{ PCI_VDEVICE(INTEL, 0x3b22), board_ahci }, /* PCH AHCI */
	{ PCI_VDEVICE(INTEL, 0x3b23), board_ahci }, /* PCH AHCI */
	{ PCI_VDEVICE(INTEL, 0x3b24), board_ahci }, /* PCH RAID */
	{ PCI_VDEVICE(INTEL, 0x3b25), board_ahci }, /* PCH RAID */
	{ PCI_VDEVICE(INTEL, 0x3b29), board_ahci_mobile }, /* PCH M AHCI */
	{ PCI_VDEVICE(INTEL, 0x3b2b), board_ahci }, /* PCH RAID */
	{ PCI_VDEVICE(INTEL, 0x3b2c), board_ahci_mobile }, /* PCH M RAID */
	{ PCI_VDEVICE(INTEL, 0x3b2f), board_ahci }, /* PCH AHCI */
	{ PCI_VDEVICE(INTEL, 0x19b0), board_ahci_pcs7 }, /* DNV AHCI */
	{ PCI_VDEVICE(INTEL, 0x19b1), board_ahci_pcs7 }, /* DNV AHCI */
	{ PCI_VDEVICE(INTEL, 0x19b2), board_ahci_pcs7 }, /* DNV AHCI */
	{ PCI_VDEVICE(INTEL, 0x19b3), board_ahci_pcs7 }, /* DNV AHCI */
	{ PCI_VDEVICE(INTEL, 0x19b4), board_ahci_pcs7 }, /* DNV AHCI */
	{ PCI_VDEVICE(INTEL, 0x19b5), board_ahci_pcs7 }, /* DNV AHCI */
	{ PCI_VDEVICE(INTEL, 0x19b6), board_ahci_pcs7 }, /* DNV AHCI */
	{ PCI_VDEVICE(INTEL, 0x19b7), board_ahci_pcs7 }, /* DNV AHCI */
	{ PCI_VDEVICE(INTEL, 0x19bE), board_ahci_pcs7 }, /* DNV AHCI */
	{ PCI_VDEVICE(INTEL, 0x19bF), board_ahci_pcs7 }, /* DNV AHCI */
	{ PCI_VDEVICE(INTEL, 0x19c0), board_ahci_pcs7 }, /* DNV AHCI */
	{ PCI_VDEVICE(INTEL, 0x19c1), board_ahci_pcs7 }, /* DNV AHCI */
	{ PCI_VDEVICE(INTEL, 0x19c2), board_ahci_pcs7 }, /* DNV AHCI */
	{ PCI_VDEVICE(INTEL, 0x19c3), board_ahci_pcs7 }, /* DNV AHCI */
	{ PCI_VDEVICE(INTEL, 0x19c4), board_ahci_pcs7 }, /* DNV AHCI */
	{ PCI_VDEVICE(INTEL, 0x19c5), board_ahci_pcs7 }, /* DNV AHCI */
	{ PCI_VDEVICE(INTEL, 0x19c6), board_ahci_pcs7 }, /* DNV AHCI */
	{ PCI_VDEVICE(INTEL, 0x19c7), board_ahci_pcs7 }, /* DNV AHCI */
	{ PCI_VDEVICE(INTEL, 0x19cE), board_ahci_pcs7 }, /* DNV AHCI */
	{ PCI_VDEVICE(INTEL, 0x19cF), board_ahci_pcs7 }, /* DNV AHCI */
	{ PCI_VDEVICE(INTEL, 0x1c02), board_ahci }, /* CPT AHCI */
	{ PCI_VDEVICE(INTEL, 0x1c03), board_ahci_mobile }, /* CPT M AHCI */
	{ PCI_VDEVICE(INTEL, 0x1c04), board_ahci }, /* CPT RAID */
	{ PCI_VDEVICE(INTEL, 0x1c05), board_ahci_mobile }, /* CPT M RAID */
	{ PCI_VDEVICE(INTEL, 0x1c06), board_ahci }, /* CPT RAID */
	{ PCI_VDEVICE(INTEL, 0x1c07), board_ahci }, /* CPT RAID */
	{ PCI_VDEVICE(INTEL, 0x1d02), board_ahci }, /* PBG AHCI */
	{ PCI_VDEVICE(INTEL, 0x1d04), board_ahci }, /* PBG RAID */
	{ PCI_VDEVICE(INTEL, 0x1d06), board_ahci }, /* PBG RAID */
	{ PCI_VDEVICE(INTEL, 0x2826), board_ahci }, /* PBG RAID */
	{ PCI_VDEVICE(INTEL, 0x2323), board_ahci }, /* DH89xxCC AHCI */
	{ PCI_VDEVICE(INTEL, 0x1e02), board_ahci }, /* Panther Point AHCI */
	{ PCI_VDEVICE(INTEL, 0x1e03), board_ahci_mobile }, /* Panther M AHCI */
	{ PCI_VDEVICE(INTEL, 0x1e04), board_ahci }, /* Panther Point RAID */
	{ PCI_VDEVICE(INTEL, 0x1e05), board_ahci }, /* Panther Point RAID */
	{ PCI_VDEVICE(INTEL, 0x1e06), board_ahci }, /* Panther Point RAID */
	{ PCI_VDEVICE(INTEL, 0x1e07), board_ahci_mobile }, /* Panther M RAID */
	{ PCI_VDEVICE(INTEL, 0x1e0e), board_ahci }, /* Panther Point RAID */
	{ PCI_VDEVICE(INTEL, 0x8c02), board_ahci }, /* Lynx Point AHCI */
	{ PCI_VDEVICE(INTEL, 0x8c03), board_ahci_mobile }, /* Lynx M AHCI */
	{ PCI_VDEVICE(INTEL, 0x8c04), board_ahci }, /* Lynx Point RAID */
	{ PCI_VDEVICE(INTEL, 0x8c05), board_ahci_mobile }, /* Lynx M RAID */
	{ PCI_VDEVICE(INTEL, 0x8c06), board_ahci }, /* Lynx Point RAID */
	{ PCI_VDEVICE(INTEL, 0x8c07), board_ahci_mobile }, /* Lynx M RAID */
	{ PCI_VDEVICE(INTEL, 0x8c0e), board_ahci }, /* Lynx Point RAID */
	{ PCI_VDEVICE(INTEL, 0x8c0f), board_ahci_mobile }, /* Lynx M RAID */
	{ PCI_VDEVICE(INTEL, 0x9c02), board_ahci_mobile }, /* Lynx LP AHCI */
	{ PCI_VDEVICE(INTEL, 0x9c03), board_ahci_mobile }, /* Lynx LP AHCI */
	{ PCI_VDEVICE(INTEL, 0x9c04), board_ahci_mobile }, /* Lynx LP RAID */
	{ PCI_VDEVICE(INTEL, 0x9c05), board_ahci_mobile }, /* Lynx LP RAID */
	{ PCI_VDEVICE(INTEL, 0x9c06), board_ahci_mobile }, /* Lynx LP RAID */
	{ PCI_VDEVICE(INTEL, 0x9c07), board_ahci_mobile }, /* Lynx LP RAID */
	{ PCI_VDEVICE(INTEL, 0x9c0e), board_ahci_mobile }, /* Lynx LP RAID */
	{ PCI_VDEVICE(INTEL, 0x9c0f), board_ahci_mobile }, /* Lynx LP RAID */
	{ PCI_VDEVICE(INTEL, 0x9dd3), board_ahci_mobile }, /* Cannon Lake PCH-LP AHCI */
	{ PCI_VDEVICE(INTEL, 0x1f22), board_ahci }, /* Avoton AHCI */
	{ PCI_VDEVICE(INTEL, 0x1f23), board_ahci }, /* Avoton AHCI */
	{ PCI_VDEVICE(INTEL, 0x1f24), board_ahci }, /* Avoton RAID */
	{ PCI_VDEVICE(INTEL, 0x1f25), board_ahci }, /* Avoton RAID */
	{ PCI_VDEVICE(INTEL, 0x1f26), board_ahci }, /* Avoton RAID */
	{ PCI_VDEVICE(INTEL, 0x1f27), board_ahci }, /* Avoton RAID */
	{ PCI_VDEVICE(INTEL, 0x1f2e), board_ahci }, /* Avoton RAID */
	{ PCI_VDEVICE(INTEL, 0x1f2f), board_ahci }, /* Avoton RAID */
	{ PCI_VDEVICE(INTEL, 0x1f32), board_ahci_avn }, /* Avoton AHCI */
	{ PCI_VDEVICE(INTEL, 0x1f33), board_ahci_avn }, /* Avoton AHCI */
	{ PCI_VDEVICE(INTEL, 0x1f34), board_ahci_avn }, /* Avoton RAID */
	{ PCI_VDEVICE(INTEL, 0x1f35), board_ahci_avn }, /* Avoton RAID */
	{ PCI_VDEVICE(INTEL, 0x1f36), board_ahci_avn }, /* Avoton RAID */
	{ PCI_VDEVICE(INTEL, 0x1f37), board_ahci_avn }, /* Avoton RAID */
	{ PCI_VDEVICE(INTEL, 0x1f3e), board_ahci_avn }, /* Avoton RAID */
	{ PCI_VDEVICE(INTEL, 0x1f3f), board_ahci_avn }, /* Avoton RAID */
	{ PCI_VDEVICE(INTEL, 0x2823), board_ahci }, /* Wellsburg RAID */
	{ PCI_VDEVICE(INTEL, 0x2827), board_ahci }, /* Wellsburg RAID */
	{ PCI_VDEVICE(INTEL, 0x43d4), board_ahci }, /* Rocket Lake PCH-H RAID */
	{ PCI_VDEVICE(INTEL, 0x43d5), board_ahci }, /* Rocket Lake PCH-H RAID */
	{ PCI_VDEVICE(INTEL, 0x43d6), board_ahci }, /* Rocket Lake PCH-H RAID */
	{ PCI_VDEVICE(INTEL, 0x43d7), board_ahci }, /* Rocket Lake PCH-H RAID */
	{ PCI_VDEVICE(INTEL, 0x8d02), board_ahci }, /* Wellsburg AHCI */
	{ PCI_VDEVICE(INTEL, 0x8d04), board_ahci }, /* Wellsburg RAID */
	{ PCI_VDEVICE(INTEL, 0x8d06), board_ahci }, /* Wellsburg RAID */
	{ PCI_VDEVICE(INTEL, 0x8d0e), board_ahci }, /* Wellsburg RAID */
	{ PCI_VDEVICE(INTEL, 0x8d62), board_ahci }, /* Wellsburg AHCI */
	{ PCI_VDEVICE(INTEL, 0x8d64), board_ahci }, /* Wellsburg RAID */
	{ PCI_VDEVICE(INTEL, 0x8d66), board_ahci }, /* Wellsburg RAID */
	{ PCI_VDEVICE(INTEL, 0x8d6e), board_ahci }, /* Wellsburg RAID */
	{ PCI_VDEVICE(INTEL, 0x23a3), board_ahci }, /* Coleto Creek AHCI */
	{ PCI_VDEVICE(INTEL, 0x9c83), board_ahci_mobile }, /* Wildcat LP AHCI */
	{ PCI_VDEVICE(INTEL, 0x9c85), board_ahci_mobile }, /* Wildcat LP RAID */
	{ PCI_VDEVICE(INTEL, 0x9c87), board_ahci_mobile }, /* Wildcat LP RAID */
	{ PCI_VDEVICE(INTEL, 0x9c8f), board_ahci_mobile }, /* Wildcat LP RAID */
	{ PCI_VDEVICE(INTEL, 0x8c82), board_ahci }, /* 9 Series AHCI */
	{ PCI_VDEVICE(INTEL, 0x8c83), board_ahci_mobile }, /* 9 Series M AHCI */
	{ PCI_VDEVICE(INTEL, 0x8c84), board_ahci }, /* 9 Series RAID */
	{ PCI_VDEVICE(INTEL, 0x8c85), board_ahci_mobile }, /* 9 Series M RAID */
	{ PCI_VDEVICE(INTEL, 0x8c86), board_ahci }, /* 9 Series RAID */
	{ PCI_VDEVICE(INTEL, 0x8c87), board_ahci_mobile }, /* 9 Series M RAID */
	{ PCI_VDEVICE(INTEL, 0x8c8e), board_ahci }, /* 9 Series RAID */
	{ PCI_VDEVICE(INTEL, 0x8c8f), board_ahci_mobile }, /* 9 Series M RAID */
	{ PCI_VDEVICE(INTEL, 0x9d03), board_ahci_mobile }, /* Sunrise LP AHCI */
	{ PCI_VDEVICE(INTEL, 0x9d05), board_ahci_mobile }, /* Sunrise LP RAID */
	{ PCI_VDEVICE(INTEL, 0x9d07), board_ahci_mobile }, /* Sunrise LP RAID */
	{ PCI_VDEVICE(INTEL, 0xa102), board_ahci }, /* Sunrise Point-H AHCI */
	{ PCI_VDEVICE(INTEL, 0xa103), board_ahci_mobile }, /* Sunrise M AHCI */
	{ PCI_VDEVICE(INTEL, 0xa105), board_ahci }, /* Sunrise Point-H RAID */
	{ PCI_VDEVICE(INTEL, 0xa106), board_ahci }, /* Sunrise Point-H RAID */
	{ PCI_VDEVICE(INTEL, 0xa107), board_ahci_mobile }, /* Sunrise M RAID */
	{ PCI_VDEVICE(INTEL, 0xa10f), board_ahci }, /* Sunrise Point-H RAID */
	{ PCI_VDEVICE(INTEL, 0x2822), board_ahci }, /* Lewisburg RAID*/
	{ PCI_VDEVICE(INTEL, 0x2823), board_ahci }, /* Lewisburg AHCI*/
	{ PCI_VDEVICE(INTEL, 0x2826), board_ahci }, /* Lewisburg RAID*/
	{ PCI_VDEVICE(INTEL, 0x2827), board_ahci }, /* Lewisburg RAID*/
	{ PCI_VDEVICE(INTEL, 0xa182), board_ahci }, /* Lewisburg AHCI*/
	{ PCI_VDEVICE(INTEL, 0xa186), board_ahci }, /* Lewisburg RAID*/
	{ PCI_VDEVICE(INTEL, 0xa1d2), board_ahci }, /* Lewisburg RAID*/
	{ PCI_VDEVICE(INTEL, 0xa1d6), board_ahci }, /* Lewisburg RAID*/
	{ PCI_VDEVICE(INTEL, 0xa202), board_ahci }, /* Lewisburg AHCI*/
	{ PCI_VDEVICE(INTEL, 0xa206), board_ahci }, /* Lewisburg RAID*/
	{ PCI_VDEVICE(INTEL, 0xa252), board_ahci }, /* Lewisburg RAID*/
	{ PCI_VDEVICE(INTEL, 0xa256), board_ahci }, /* Lewisburg RAID*/
	{ PCI_VDEVICE(INTEL, 0xa356), board_ahci }, /* Cannon Lake PCH-H RAID */
	{ PCI_VDEVICE(INTEL, 0x06d7), board_ahci }, /* Comet Lake-H RAID */
	{ PCI_VDEVICE(INTEL, 0xa386), board_ahci }, /* Comet Lake PCH-V RAID */
	{ PCI_VDEVICE(INTEL, 0x0f22), board_ahci_mobile }, /* Bay Trail AHCI */
	{ PCI_VDEVICE(INTEL, 0x0f23), board_ahci_mobile }, /* Bay Trail AHCI */
	{ PCI_VDEVICE(INTEL, 0x22a3), board_ahci_mobile }, /* Cherry Tr. AHCI */
	{ PCI_VDEVICE(INTEL, 0x5ae3), board_ahci_mobile }, /* ApolloLake AHCI */
	{ PCI_VDEVICE(INTEL, 0x34d3), board_ahci_mobile }, /* Ice Lake LP AHCI */
	{ PCI_VDEVICE(INTEL, 0x02d3), board_ahci_mobile }, /* Comet Lake PCH-U AHCI */
	{ PCI_VDEVICE(INTEL, 0x02d7), board_ahci_mobile }, /* Comet Lake PCH RAID */

	/* JMicron 360/1/3/5/6, match class to avoid IDE function */
	{ PCI_VENDOR_ID_JMICRON, PCI_ANY_ID, PCI_ANY_ID, PCI_ANY_ID,
	  PCI_CLASS_STORAGE_SATA_AHCI, 0xffffff, board_ahci_ign_iferr },
	/* JMicron 362B and 362C have an AHCI function with IDE class code */
	{ PCI_VDEVICE(JMICRON, 0x2362), board_ahci_ign_iferr },
	{ PCI_VDEVICE(JMICRON, 0x236f), board_ahci_ign_iferr },
	/* May need to update quirk_jmicron_async_suspend() for additions */

	/* ATI */
	{ PCI_VDEVICE(ATI, 0x4380), board_ahci_sb600 }, /* ATI SB600 */
	{ PCI_VDEVICE(ATI, 0x4390), board_ahci_sb700 }, /* ATI SB700/800 */
	{ PCI_VDEVICE(ATI, 0x4391), board_ahci_sb700 }, /* ATI SB700/800 */
	{ PCI_VDEVICE(ATI, 0x4392), board_ahci_sb700 }, /* ATI SB700/800 */
	{ PCI_VDEVICE(ATI, 0x4393), board_ahci_sb700 }, /* ATI SB700/800 */
	{ PCI_VDEVICE(ATI, 0x4394), board_ahci_sb700 }, /* ATI SB700/800 */
	{ PCI_VDEVICE(ATI, 0x4395), board_ahci_sb700 }, /* ATI SB700/800 */

	/* Amazon's Annapurna Labs support */
	{ PCI_DEVICE(PCI_VENDOR_ID_AMAZON_ANNAPURNA_LABS, 0x0031),
		.class = PCI_CLASS_STORAGE_SATA_AHCI,
		.class_mask = 0xffffff,
		board_ahci_al },
	/* AMD */
	{ PCI_VDEVICE(AMD, 0x7800), board_ahci }, /* AMD Hudson-2 */
	{ PCI_VDEVICE(AMD, 0x7900), board_ahci }, /* AMD CZ */
	{ PCI_VDEVICE(AMD, 0x7901), board_ahci_mobile }, /* AMD Green Sardine */
	/* AMD is using RAID class only for ahci controllers */
	{ PCI_VENDOR_ID_AMD, PCI_ANY_ID, PCI_ANY_ID, PCI_ANY_ID,
	  PCI_CLASS_STORAGE_RAID << 8, 0xffffff, board_ahci },

	/* Dell S140/S150 */
	{ PCI_VENDOR_ID_INTEL, PCI_ANY_ID, PCI_SUBVENDOR_ID_DELL, PCI_ANY_ID,
	  PCI_CLASS_STORAGE_RAID << 8, 0xffffff, board_ahci },

	/* VIA */
	{ PCI_VDEVICE(VIA, 0x3349), board_ahci_vt8251 }, /* VIA VT8251 */
	{ PCI_VDEVICE(VIA, 0x6287), board_ahci_vt8251 }, /* VIA VT8251 */

	/* NVIDIA */
	{ PCI_VDEVICE(NVIDIA, 0x044c), board_ahci_mcp65 },	/* MCP65 */
	{ PCI_VDEVICE(NVIDIA, 0x044d), board_ahci_mcp65 },	/* MCP65 */
	{ PCI_VDEVICE(NVIDIA, 0x044e), board_ahci_mcp65 },	/* MCP65 */
	{ PCI_VDEVICE(NVIDIA, 0x044f), board_ahci_mcp65 },	/* MCP65 */
	{ PCI_VDEVICE(NVIDIA, 0x045c), board_ahci_mcp65 },	/* MCP65 */
	{ PCI_VDEVICE(NVIDIA, 0x045d), board_ahci_mcp65 },	/* MCP65 */
	{ PCI_VDEVICE(NVIDIA, 0x045e), board_ahci_mcp65 },	/* MCP65 */
	{ PCI_VDEVICE(NVIDIA, 0x045f), board_ahci_mcp65 },	/* MCP65 */
	{ PCI_VDEVICE(NVIDIA, 0x0550), board_ahci_mcp67 },	/* MCP67 */
	{ PCI_VDEVICE(NVIDIA, 0x0551), board_ahci_mcp67 },	/* MCP67 */
	{ PCI_VDEVICE(NVIDIA, 0x0552), board_ahci_mcp67 },	/* MCP67 */
	{ PCI_VDEVICE(NVIDIA, 0x0553), board_ahci_mcp67 },	/* MCP67 */
	{ PCI_VDEVICE(NVIDIA, 0x0554), board_ahci_mcp67 },	/* MCP67 */
	{ PCI_VDEVICE(NVIDIA, 0x0555), board_ahci_mcp67 },	/* MCP67 */
	{ PCI_VDEVICE(NVIDIA, 0x0556), board_ahci_mcp67 },	/* MCP67 */
	{ PCI_VDEVICE(NVIDIA, 0x0557), board_ahci_mcp67 },	/* MCP67 */
	{ PCI_VDEVICE(NVIDIA, 0x0558), board_ahci_mcp67 },	/* MCP67 */
	{ PCI_VDEVICE(NVIDIA, 0x0559), board_ahci_mcp67 },	/* MCP67 */
	{ PCI_VDEVICE(NVIDIA, 0x055a), board_ahci_mcp67 },	/* MCP67 */
	{ PCI_VDEVICE(NVIDIA, 0x055b), board_ahci_mcp67 },	/* MCP67 */
	{ PCI_VDEVICE(NVIDIA, 0x0580), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x0581), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x0582), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x0583), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x0584), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x0585), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x0586), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x0587), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x0588), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x0589), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x058a), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x058b), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x058c), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x058d), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x058e), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x058f), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x07f0), board_ahci_mcp73 },	/* MCP73 */
	{ PCI_VDEVICE(NVIDIA, 0x07f1), board_ahci_mcp73 },	/* MCP73 */
	{ PCI_VDEVICE(NVIDIA, 0x07f2), board_ahci_mcp73 },	/* MCP73 */
	{ PCI_VDEVICE(NVIDIA, 0x07f3), board_ahci_mcp73 },	/* MCP73 */
	{ PCI_VDEVICE(NVIDIA, 0x07f4), board_ahci_mcp73 },	/* MCP73 */
	{ PCI_VDEVICE(NVIDIA, 0x07f5), board_ahci_mcp73 },	/* MCP73 */
	{ PCI_VDEVICE(NVIDIA, 0x07f6), board_ahci_mcp73 },	/* MCP73 */
	{ PCI_VDEVICE(NVIDIA, 0x07f7), board_ahci_mcp73 },	/* MCP73 */
	{ PCI_VDEVICE(NVIDIA, 0x07f8), board_ahci_mcp73 },	/* MCP73 */
	{ PCI_VDEVICE(NVIDIA, 0x07f9), board_ahci_mcp73 },	/* MCP73 */
	{ PCI_VDEVICE(NVIDIA, 0x07fa), board_ahci_mcp73 },	/* MCP73 */
	{ PCI_VDEVICE(NVIDIA, 0x07fb), board_ahci_mcp73 },	/* MCP73 */
	{ PCI_VDEVICE(NVIDIA, 0x0ad0), board_ahci_mcp77 },	/* MCP77 */
	{ PCI_VDEVICE(NVIDIA, 0x0ad1), board_ahci_mcp77 },	/* MCP77 */
	{ PCI_VDEVICE(NVIDIA, 0x0ad2), board_ahci_mcp77 },	/* MCP77 */
	{ PCI_VDEVICE(NVIDIA, 0x0ad3), board_ahci_mcp77 },	/* MCP77 */
	{ PCI_VDEVICE(NVIDIA, 0x0ad4), board_ahci_mcp77 },	/* MCP77 */
	{ PCI_VDEVICE(NVIDIA, 0x0ad5), board_ahci_mcp77 },	/* MCP77 */
	{ PCI_VDEVICE(NVIDIA, 0x0ad6), board_ahci_mcp77 },	/* MCP77 */
	{ PCI_VDEVICE(NVIDIA, 0x0ad7), board_ahci_mcp77 },	/* MCP77 */
	{ PCI_VDEVICE(NVIDIA, 0x0ad8), board_ahci_mcp77 },	/* MCP77 */
	{ PCI_VDEVICE(NVIDIA, 0x0ad9), board_ahci_mcp77 },	/* MCP77 */
	{ PCI_VDEVICE(NVIDIA, 0x0ada), board_ahci_mcp77 },	/* MCP77 */
	{ PCI_VDEVICE(NVIDIA, 0x0adb), board_ahci_mcp77 },	/* MCP77 */
	{ PCI_VDEVICE(NVIDIA, 0x0ab4), board_ahci_mcp79 },	/* MCP79 */
	{ PCI_VDEVICE(NVIDIA, 0x0ab5), board_ahci_mcp79 },	/* MCP79 */
	{ PCI_VDEVICE(NVIDIA, 0x0ab6), board_ahci_mcp79 },	/* MCP79 */
	{ PCI_VDEVICE(NVIDIA, 0x0ab7), board_ahci_mcp79 },	/* MCP79 */
	{ PCI_VDEVICE(NVIDIA, 0x0ab8), board_ahci_mcp79 },	/* MCP79 */
	{ PCI_VDEVICE(NVIDIA, 0x0ab9), board_ahci_mcp79 },	/* MCP79 */
	{ PCI_VDEVICE(NVIDIA, 0x0aba), board_ahci_mcp79 },	/* MCP79 */
	{ PCI_VDEVICE(NVIDIA, 0x0abb), board_ahci_mcp79 },	/* MCP79 */
	{ PCI_VDEVICE(NVIDIA, 0x0abc), board_ahci_mcp79 },	/* MCP79 */
	{ PCI_VDEVICE(NVIDIA, 0x0abd), board_ahci_mcp79 },	/* MCP79 */
	{ PCI_VDEVICE(NVIDIA, 0x0abe), board_ahci_mcp79 },	/* MCP79 */
	{ PCI_VDEVICE(NVIDIA, 0x0abf), board_ahci_mcp79 },	/* MCP79 */
	{ PCI_VDEVICE(NVIDIA, 0x0d84), board_ahci_mcp89 },	/* MCP89 */
	{ PCI_VDEVICE(NVIDIA, 0x0d85), board_ahci_mcp89 },	/* MCP89 */
	{ PCI_VDEVICE(NVIDIA, 0x0d86), board_ahci_mcp89 },	/* MCP89 */
	{ PCI_VDEVICE(NVIDIA, 0x0d87), board_ahci_mcp89 },	/* MCP89 */
	{ PCI_VDEVICE(NVIDIA, 0x0d88), board_ahci_mcp89 },	/* MCP89 */
	{ PCI_VDEVICE(NVIDIA, 0x0d89), board_ahci_mcp89 },	/* MCP89 */
	{ PCI_VDEVICE(NVIDIA, 0x0d8a), board_ahci_mcp89 },	/* MCP89 */
	{ PCI_VDEVICE(NVIDIA, 0x0d8b), board_ahci_mcp89 },	/* MCP89 */
	{ PCI_VDEVICE(NVIDIA, 0x0d8c), board_ahci_mcp89 },	/* MCP89 */
	{ PCI_VDEVICE(NVIDIA, 0x0d8d), board_ahci_mcp89 },	/* MCP89 */
	{ PCI_VDEVICE(NVIDIA, 0x0d8e), board_ahci_mcp89 },	/* MCP89 */
	{ PCI_VDEVICE(NVIDIA, 0x0d8f), board_ahci_mcp89 },	/* MCP89 */

	/* SiS */
	{ PCI_VDEVICE(SI, 0x1184), board_ahci },		/* SiS 966 */
	{ PCI_VDEVICE(SI, 0x1185), board_ahci },		/* SiS 968 */
	{ PCI_VDEVICE(SI, 0x0186), board_ahci },		/* SiS 968 */

	/* ST Microelectronics */
	{ PCI_VDEVICE(STMICRO, 0xCC06), board_ahci },		/* ST ConneXt */

	/* Marvell */
	{ PCI_VDEVICE(MARVELL, 0x6145), board_ahci_mv },	/* 6145 */
	{ PCI_VDEVICE(MARVELL, 0x6121), board_ahci_mv },	/* 6121 */
	{ PCI_DEVICE(PCI_VENDOR_ID_MARVELL_EXT, 0x9123),
	  .class = PCI_CLASS_STORAGE_SATA_AHCI,
	  .class_mask = 0xffffff,
	  .driver_data = board_ahci_yes_fbs },			/* 88se9128 */
	{ PCI_DEVICE(PCI_VENDOR_ID_MARVELL_EXT, 0x9125),
	  .driver_data = board_ahci_yes_fbs },			/* 88se9125 */
	{ PCI_DEVICE_SUB(PCI_VENDOR_ID_MARVELL_EXT, 0x9178,
			 PCI_VENDOR_ID_MARVELL_EXT, 0x9170),
	  .driver_data = board_ahci_yes_fbs },			/* 88se9170 */
	{ PCI_DEVICE(PCI_VENDOR_ID_MARVELL_EXT, 0x917a),
	  .driver_data = board_ahci_yes_fbs },			/* 88se9172 */
	{ PCI_DEVICE(PCI_VENDOR_ID_MARVELL_EXT, 0x9172),
	  .driver_data = board_ahci_yes_fbs },			/* 88se9182 */
	{ PCI_DEVICE(PCI_VENDOR_ID_MARVELL_EXT, 0x9182),
	  .driver_data = board_ahci_yes_fbs },			/* 88se9172 */
	{ PCI_DEVICE(PCI_VENDOR_ID_MARVELL_EXT, 0x9192),
	  .driver_data = board_ahci_yes_fbs },			/* 88se9172 on some Gigabyte */
	{ PCI_DEVICE(PCI_VENDOR_ID_MARVELL_EXT, 0x91a0),
	  .driver_data = board_ahci_yes_fbs },
	{ PCI_DEVICE(PCI_VENDOR_ID_MARVELL_EXT, 0x91a2), 	/* 88se91a2 */
	  .driver_data = board_ahci_yes_fbs },
	{ PCI_DEVICE(PCI_VENDOR_ID_MARVELL_EXT, 0x91a3),
	  .driver_data = board_ahci_yes_fbs },
	{ PCI_DEVICE(PCI_VENDOR_ID_MARVELL_EXT, 0x9230),
	  .driver_data = board_ahci_yes_fbs },
	{ PCI_DEVICE(PCI_VENDOR_ID_TTI, 0x0642), /* highpoint rocketraid 642L */
	  .driver_data = board_ahci_yes_fbs },
	{ PCI_DEVICE(PCI_VENDOR_ID_TTI, 0x0645), /* highpoint rocketraid 644L */
	  .driver_data = board_ahci_yes_fbs },

	/* Promise */
	{ PCI_VDEVICE(PROMISE, 0x3f20), board_ahci },	/* PDC42819 */
	{ PCI_VDEVICE(PROMISE, 0x3781), board_ahci },   /* FastTrak TX8660 ahci-mode */

	/* Asmedia */
	{ PCI_VDEVICE(ASMEDIA, 0x0601), board_ahci },	/* ASM1060 */
	{ PCI_VDEVICE(ASMEDIA, 0x0602), board_ahci },	/* ASM1060 */
	{ PCI_VDEVICE(ASMEDIA, 0x0611), board_ahci },	/* ASM1061 */
	{ PCI_VDEVICE(ASMEDIA, 0x0612), board_ahci },	/* ASM1062 */
	{ PCI_VDEVICE(ASMEDIA, 0x0621), board_ahci },   /* ASM1061R */
	{ PCI_VDEVICE(ASMEDIA, 0x0622), board_ahci },   /* ASM1062R */

	/*
	 * Samsung SSDs found on some macbooks.  NCQ times out if MSI is
	 * enabled.  https://bugzilla.kernel.org/show_bug.cgi?id=60731
	 */
	{ PCI_VDEVICE(SAMSUNG, 0x1600), board_ahci_nomsi },
	{ PCI_VDEVICE(SAMSUNG, 0xa800), board_ahci_nomsi },

	/* Enmotus */
	{ PCI_DEVICE(0x1c44, 0x8000), board_ahci },

	/* Sony (PS4) */
	{ PCI_VDEVICE(SONY, PCI_DEVICE_ID_SONY_AEOLIA_AHCI), board_ahci },
	{ PCI_VDEVICE(SONY, PCI_DEVICE_ID_SONY_BELIZE_AHCI), board_ahci },
	{ PCI_VDEVICE(SONY, PCI_DEVICE_ID_SONY_BAIKAL_AHCI), board_ahci },

	/* Loongson */
	{ PCI_VDEVICE(LOONGSON, 0x7a08), board_ahci },

	/* Generic, PCI class code for AHCI */
	{ PCI_ANY_ID, PCI_ANY_ID, PCI_ANY_ID, PCI_ANY_ID,
	  PCI_CLASS_STORAGE_SATA_AHCI, 0xffffff, board_ahci },

	{ }	/* terminate list */
};

static const struct dev_pm_ops ahci_pci_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(ahci_pci_device_suspend, ahci_pci_device_resume)
	SET_RUNTIME_PM_OPS(ahci_pci_device_runtime_suspend,
			   ahci_pci_device_runtime_resume, NULL)
};

static struct pci_driver ahci_pci_driver = {
	.name			= DRV_NAME,
	.id_table		= ahci_pci_tbl,
	.probe			= ahci_init_one,
	.remove			= ahci_remove_one,
	.shutdown		= ahci_shutdown_one,
	.driver = {
		.pm		= &ahci_pci_pm_ops,
	},
};

#if IS_ENABLED(CONFIG_PATA_MARVELL)
static int marvell_enable;
#else
static int marvell_enable = 1;
#endif
module_param(marvell_enable, int, 0644);
MODULE_PARM_DESC(marvell_enable, "Marvell SATA via AHCI (1 = enabled)");

static int mobile_lpm_policy = -1;
module_param(mobile_lpm_policy, int, 0644);
MODULE_PARM_DESC(mobile_lpm_policy, "Default LPM policy for mobile chipsets");

static void ahci_pci_save_initial_config(struct pci_dev *pdev,
					 struct ahci_host_priv *hpriv)
{
	if (pdev->vendor == PCI_VENDOR_ID_JMICRON && pdev->device == 0x2361) {
		dev_info(&pdev->dev, "JMB361 has only one port\n");
		hpriv->force_port_map = 1;
	}

	/*
	 * Temporary Marvell 6145 hack: PATA port presence
	 * is asserted through the standard AHCI port
	 * presence register, as bit 4 (counting from 0)
	 */
	if (hpriv->flags & AHCI_HFLAG_MV_PATA) {
		if (pdev->device == 0x6121)
			hpriv->mask_port_map = 0x3;
		else
			hpriv->mask_port_map = 0xf;
		dev_info(&pdev->dev,
			  "Disabling your PATA port. Use the boot option 'ahci.marvell_enable=0' to avoid this.\n");
	}

	ahci_save_initial_config(&pdev->dev, hpriv);
}

static void ahci_pci_init_controller(struct ata_host *host)
{
	struct ahci_host_priv *hpriv = host->private_data;
	struct pci_dev *pdev = to_pci_dev(host->dev);
	void __iomem *port_mmio;
	u32 tmp;
	int mv;

	if (hpriv->flags & AHCI_HFLAG_MV_PATA) {
		if (pdev->device == 0x6121)
			mv = 2;
		else
			mv = 4;
		port_mmio = __ahci_port_base(host, mv);

		writel(0, port_mmio + PORT_IRQ_MASK);

		/* clear port IRQ */
		tmp = readl(port_mmio + PORT_IRQ_STAT);
		VPRINTK("PORT_IRQ_STAT 0x%x\n", tmp);
		if (tmp)
			writel(tmp, port_mmio + PORT_IRQ_STAT);
	}

	ahci_init_controller(host);
}

static int ahci_vt8251_hardreset(struct ata_link *link, unsigned int *class,
				 unsigned long deadline)
{
	struct ata_port *ap = link->ap;
	struct ahci_host_priv *hpriv = ap->host->private_data;
	bool online;
	int rc;

	DPRINTK("ENTER\n");

	hpriv->stop_engine(ap);

	rc = sata_link_hardreset(link, sata_ehc_deb_timing(&link->eh_context),
				 deadline, &online, NULL);

	hpriv->start_engine(ap);

	DPRINTK("EXIT, rc=%d, class=%u\n", rc, *class);

	/* vt8251 doesn't clear BSY on signature FIS reception,
	 * request follow-up softreset.
	 */
	return online ? -EAGAIN : rc;
}

static int ahci_p5wdh_hardreset(struct ata_link *link, unsigned int *class,
				unsigned long deadline)
{
	struct ata_port *ap = link->ap;
	struct ahci_port_priv *pp = ap->private_data;
	struct ahci_host_priv *hpriv = ap->host->private_data;
	u8 *d2h_fis = pp->rx_fis + RX_FIS_D2H_REG;
	struct ata_taskfile tf;
	bool online;
	int rc;

	hpriv->stop_engine(ap);

	/* clear D2H reception area to properly wait for D2H FIS */
	ata_tf_init(link->device, &tf);
	tf.command = ATA_BUSY;
	ata_tf_to_fis(&tf, 0, 0, d2h_fis);

	rc = sata_link_hardreset(link, sata_ehc_deb_timing(&link->eh_context),
				 deadline, &online, NULL);

	hpriv->start_engine(ap);

	/* The pseudo configuration device on SIMG4726 attached to
	 * ASUS P5W-DH Deluxe doesn't send signature FIS after
	 * hardreset if no device is attached to the first downstream
	 * port && the pseudo device locks up on SRST w/ PMP==0.  To
	 * work around this, wait for !BSY only briefly.  If BSY isn't
	 * cleared, perform CLO and proceed to IDENTIFY (achieved by
	 * ATA_LFLAG_NO_SRST and ATA_LFLAG_ASSUME_ATA).
	 *
	 * Wait for two seconds.  Devices attached to downstream port
	 * which can't process the following IDENTIFY after this will
	 * have to be reset again.  For most cases, this should
	 * suffice while making probing snappish enough.
	 */
	if (online) {
		rc = ata_wait_after_reset(link, jiffies + 2 * HZ,
					  ahci_check_ready);
		if (rc)
			ahci_kick_engine(ap);
	}
	return rc;
}

/*
 * ahci_avn_hardreset - attempt more aggressive recovery of Avoton ports.
 *
 * It has been observed with some SSDs that the timing of events in the
 * link synchronization phase can leave the port in a state that can not
 * be recovered by a SATA-hard-reset alone.  The failing signature is
 * SStatus.DET stuck at 1 ("Device presence detected but Phy
 * communication not established").  It was found that unloading and
 * reloading the driver when this problem occurs allows the drive
 * connection to be recovered (DET advanced to 0x3).  The critical
 * component of reloading the driver is that the port state machines are
 * reset by bouncing "port enable" in the AHCI PCS configuration
 * register.  So, reproduce that effect by bouncing a port whenever we
 * see DET==1 after a reset.
 */
static int ahci_avn_hardreset(struct ata_link *link, unsigned int *class,
			      unsigned long deadline)
{
	const unsigned long *timing = sata_ehc_deb_timing(&link->eh_context);
	struct ata_port *ap = link->ap;
	struct ahci_port_priv *pp = ap->private_data;
	struct ahci_host_priv *hpriv = ap->host->private_data;
	u8 *d2h_fis = pp->rx_fis + RX_FIS_D2H_REG;
	unsigned long tmo = deadline - jiffies;
	struct ata_taskfile tf;
	bool online;
	int rc, i;

	DPRINTK("ENTER\n");

	hpriv->stop_engine(ap);

	for (i = 0; i < 2; i++) {
		u16 val;
		u32 sstatus;
		int port = ap->port_no;
		struct ata_host *host = ap->host;
		struct pci_dev *pdev = to_pci_dev(host->dev);

		/* clear D2H reception area to properly wait for D2H FIS */
		ata_tf_init(link->device, &tf);
		tf.command = ATA_BUSY;
		ata_tf_to_fis(&tf, 0, 0, d2h_fis);

		rc = sata_link_hardreset(link, timing, deadline, &online,
				ahci_check_ready);

		if (sata_scr_read(link, SCR_STATUS, &sstatus) != 0 ||
				(sstatus & 0xf) != 1)
			break;

		ata_link_info(link,  "avn bounce port%d\n", port);

		pci_read_config_word(pdev, 0x92, &val);
		val &= ~(1 << port);
		pci_write_config_word(pdev, 0x92, val);
		ata_msleep(ap, 1000);
		val |= 1 << port;
		pci_write_config_word(pdev, 0x92, val);
		deadline += tmo;
	}

	hpriv->start_engine(ap);

	if (online)
		*class = ahci_dev_classify(ap);

	DPRINTK("EXIT, rc=%d, class=%u\n", rc, *class);
	return rc;
}


#ifdef CONFIG_PM
static void ahci_pci_disable_interrupts(struct ata_host *host)
{
	struct ahci_host_priv *hpriv = host->private_data;
	void __iomem *mmio = hpriv->mmio;
	u32 ctl;

	/* AHCI spec rev1.1 section 8.3.3:
	 * Software must disable interrupts prior to requesting a
	 * transition of the HBA to D3 state.
	 */
	ctl = readl(mmio + HOST_CTL);
	ctl &= ~HOST_IRQ_EN;
	writel(ctl, mmio + HOST_CTL);
	readl(mmio + HOST_CTL); /* flush */
}

static int ahci_pci_device_runtime_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct ata_host *host = pci_get_drvdata(pdev);

	ahci_pci_disable_interrupts(host);
	return 0;
}

static int ahci_pci_device_runtime_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct ata_host *host = pci_get_drvdata(pdev);
	int rc;

	rc = ahci_reset_controller(host);
	if (rc)
		return rc;
	ahci_pci_init_controller(host);
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int ahci_pci_device_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct ata_host *host = pci_get_drvdata(pdev);
	struct ahci_host_priv *hpriv = host->private_data;

	if (hpriv->flags & AHCI_HFLAG_NO_SUSPEND) {
		dev_err(&pdev->dev,
			"BIOS update required for suspend/resume\n");
		return -EIO;
	}

	ahci_pci_disable_interrupts(host);
	return ata_host_suspend(host, PMSG_SUSPEND);
}

static int ahci_pci_device_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct ata_host *host = pci_get_drvdata(pdev);
	int rc;

	/* Apple BIOS helpfully mangles the registers on resume */
	if (is_mcp89_apple(pdev))
		ahci_mcp89_apple_enable(pdev);

	if (pdev->dev.power.power_state.event == PM_EVENT_SUSPEND) {
		rc = ahci_reset_controller(host);
		if (rc)
			return rc;

		ahci_pci_init_controller(host);
	}

	ata_host_resume(host);

	return 0;
}
#endif

#endif /* CONFIG_PM */

static int ahci_configure_dma_masks(struct pci_dev *pdev, int using_dac)
{
	const int dma_bits = using_dac ? 64 : 32;
	int rc;

	/*
	 * If the device fixup already set the dma_mask to some non-standard
	 * value, don't extend it here. This happens on STA2X11, for example.
	 *
	 * XXX: manipulating the DMA mask from platform code is completely
	 * bogus, platform code should use dev->bus_dma_limit instead..
	 */
	if (pdev->dma_mask && pdev->dma_mask < DMA_BIT_MASK(32))
		return 0;

	#ifdef CONFIG_X86_PS4
	if (pdev->vendor == PCI_VENDOR_ID_SONY) {
		rc = dma_set_mask(&pdev->dev, DMA_BIT_MASK(31));
		if (rc) {
			dev_err(&pdev->dev, "31-bit DMA enable failed\n");
			return rc;
		}
		rc = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(31));
		if (rc) {
			dev_err(&pdev->dev,
				"31-bit consistent DMA enable failed\n");
			return rc;
		}
		return 0;
	}
	#endif

	rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(dma_bits));
	if (rc)
		dev_err(&pdev->dev, "DMA enable failed\n");
	return rc;
}

static void ahci_pci_print_info(struct ata_host *host)
{
	struct pci_dev *pdev = to_pci_dev(host->dev);
	u16 cc;
	const char *scc_s;

	pci_read_config_word(pdev, 0x0a, &cc);
	if (cc == PCI_CLASS_STORAGE_IDE)
		scc_s = "IDE";
	else if (cc == PCI_CLASS_STORAGE_SATA)
		scc_s = "SATA";
	else if (cc == PCI_CLASS_STORAGE_RAID)
		scc_s = "RAID";
	else
		scc_s = "unknown";

	ahci_print_info(host, scc_s);
}

/* On ASUS P5W DH Deluxe, the second port of PCI device 00:1f.2 is
 * hardwired to on-board SIMG 4726.  The chipset is ICH8 and doesn't
 * support PMP and the 4726 either directly exports the device
 * attached to the first downstream port or acts as a hardware storage
 * controller and emulate a single ATA device (can be RAID 0/1 or some
 * other configuration).
 *
 * When there's no device attached to the first downstream port of the
 * 4726, "Config Disk" appears, which is a pseudo ATA device to
 * configure the 4726.  However, ATA emulation of the device is very
 * lame.  It doesn't send signature D2H Reg FIS after the initial
 * hardreset, pukes on SRST w/ PMP==0 and has bunch of other issues.
 *
 * The following function works around the problem by always using
 * hardreset on the port and not depending on receiving signature FIS
 * afterward.  If signature FIS isn't received soon, ATA class is
 * assumed without follow-up softreset.
 */
static void ahci_p5wdh_workaround(struct ata_host *host)
{
	static const struct dmi_system_id sysids[] = {
		{
			.ident = "P5W DH Deluxe",
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR,
					  "ASUSTEK COMPUTER INC"),
				DMI_MATCH(DMI_PRODUCT_NAME, "P5W DH Deluxe"),
			},
		},
		{ }
	};
	struct pci_dev *pdev = to_pci_dev(host->dev);

	if (pdev->bus->number == 0 && pdev->devfn == PCI_DEVFN(0x1f, 2) &&
	    dmi_check_system(sysids)) {
		struct ata_port *ap = host->ports[1];

		dev_info(&pdev->dev,
			 "enabling ASUS P5W DH Deluxe on-board SIMG4726 workaround\n");

		ap->ops = &ahci_p5wdh_ops;
		ap->link.flags |= ATA_LFLAG_NO_SRST | ATA_LFLAG_ASSUME_ATA;
	}
}

/*
 * Macbook7,1 firmware forcibly disables MCP89 AHCI and changes PCI ID when
 * booting in BIOS compatibility mode.  We restore the registers but not ID.
 */
static void ahci_mcp89_apple_enable(struct pci_dev *pdev)
{
	u32 val;

	printk(KERN_INFO "ahci: enabling MCP89 AHCI mode\n");

	pci_read_config_dword(pdev, 0xf8, &val);
	val |= 1 << 0x1b;
	/* the following changes the device ID, but appears not to affect function */
	/* val = (val & ~0xf0000000) | 0x80000000; */
	pci_write_config_dword(pdev, 0xf8, val);

	pci_read_config_dword(pdev, 0x54c, &val);
	val |= 1 << 0xc;
	pci_write_config_dword(pdev, 0x54c, val);

	pci_read_config_dword(pdev, 0x4a4, &val);
	val &= 0xff;
	val |= 0x01060100;
	pci_write_config_dword(pdev, 0x4a4, val);

	pci_read_config_dword(pdev, 0x54c, &val);
	val &= ~(1 << 0xc);
	pci_write_config_dword(pdev, 0x54c, val);

	pci_read_config_dword(pdev, 0xf8, &val);
	val &= ~(1 << 0x1b);
	pci_write_config_dword(pdev, 0xf8, val);
}

static bool is_mcp89_apple(struct pci_dev *pdev)
{
	return pdev->vendor == PCI_VENDOR_ID_NVIDIA &&
		pdev->device == PCI_DEVICE_ID_NVIDIA_NFORCE_MCP89_SATA &&
		pdev->subsystem_vendor == PCI_VENDOR_ID_APPLE &&
		pdev->subsystem_device == 0xcb89;
}

/* only some SB600 ahci controllers can do 64bit DMA */
static bool ahci_sb600_enable_64bit(struct pci_dev *pdev)
{
	static const struct dmi_system_id sysids[] = {
		/*
		 * The oldest version known to be broken is 0901 and
		 * working is 1501 which was released on 2007-10-26.
		 * Enable 64bit DMA on 1501 and anything newer.
		 *
		 * Please read bko#9412 for more info.
		 */
		{
			.ident = "ASUS M2A-VM",
			.matches = {
				DMI_MATCH(DMI_BOARD_VENDOR,
					  "ASUSTeK Computer INC."),
				DMI_MATCH(DMI_BOARD_NAME, "M2A-VM"),
			},
			.driver_data = "20071026",	/* yyyymmdd */
		},
		/*
		 * All BIOS versions for the MSI K9A2 Platinum (MS-7376)
		 * support 64bit DMA.
		 *
		 * BIOS versions earlier than 1.5 had the Manufacturer DMI
		 * fields as "MICRO-STAR INTERANTIONAL CO.,LTD".
		 * This spelling mistake was fixed in BIOS version 1.5, so
		 * 1.5 and later have the Manufacturer as
		 * "MICRO-STAR INTERNATIONAL CO.,LTD".
		 * So try to match on DMI_BOARD_VENDOR of "MICRO-STAR INTER".
		 *
		 * BIOS versions earlier than 1.9 had a Board Product Name
		 * DMI field of "MS-7376". This was changed to be
		 * "K9A2 Platinum (MS-7376)" in version 1.9, but we can still
		 * match on DMI_BOARD_NAME of "MS-7376".
		 */
		{
			.ident = "MSI K9A2 Platinum",
			.matches = {
				DMI_MATCH(DMI_BOARD_VENDOR,
					  "MICRO-STAR INTER"),
				DMI_MATCH(DMI_BOARD_NAME, "MS-7376"),
			},
		},
		/*
		 * All BIOS versions for the MSI K9AGM2 (MS-7327) support
		 * 64bit DMA.
		 *
		 * This board also had the typo mentioned above in the
		 * Manufacturer DMI field (fixed in BIOS version 1.5), so
		 * match on DMI_BOARD_VENDOR of "MICRO-STAR INTER" again.
		 */
		{
			.ident = "MSI K9AGM2",
			.matches = {
				DMI_MATCH(DMI_BOARD_VENDOR,
					  "MICRO-STAR INTER"),
				DMI_MATCH(DMI_BOARD_NAME, "MS-7327"),
			},
		},
		/*
		 * All BIOS versions for the Asus M3A support 64bit DMA.
		 * (all release versions from 0301 to 1206 were tested)
		 */
		{
			.ident = "ASUS M3A",
			.matches = {
				DMI_MATCH(DMI_BOARD_VENDOR,
					  "ASUSTeK Computer INC."),
				DMI_MATCH(DMI_BOARD_NAME, "M3A"),
			},
		},
		{ }
	};
	const struct dmi_system_id *match;
	int year, month, date;
	char buf[9];

	match = dmi_first_match(sysids);
	if (pdev->bus->number != 0 || pdev->devfn != PCI_DEVFN(0x12, 0) ||
	    !match)
		return false;

	if (!match->driver_data)
		goto enable_64bit;

	dmi_get_date(DMI_BIOS_DATE, &year, &month, &date);
	snprintf(buf, sizeof(buf), "%04d%02d%02d", year, month, date);

	if (strcmp(buf, match->driver_data) >= 0)
		goto enable_64bit;
	else {
		dev_warn(&pdev->dev,
			 "%s: BIOS too old, forcing 32bit DMA, update BIOS\n",
			 match->ident);
		return false;
	}

enable_64bit:
	dev_warn(&pdev->dev, "%s: enabling 64bit DMA\n", match->ident);
	return true;
}

static bool ahci_broken_system_poweroff(struct pci_dev *pdev)
{
	static const struct dmi_system_id broken_systems[] = {
		{
			.ident = "HP Compaq nx6310",
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR, "Hewlett-Packard"),
				DMI_MATCH(DMI_PRODUCT_NAME, "HP Compaq nx6310"),
			},
			/* PCI slot number of the controller */
			.driver_data = (void *)0x1FUL,
		},
		{
			.ident = "HP Compaq 6720s",
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR, "Hewlett-Packard"),
				DMI_MATCH(DMI_PRODUCT_NAME, "HP Compaq 6720s"),
			},
			/* PCI slot number of the controller */
			.driver_data = (void *)0x1FUL,
		},

		{ }	/* terminate list */
	};
	const struct dmi_system_id *dmi = dmi_first_match(broken_systems);

	if (dmi) {
		unsigned long slot = (unsigned long)dmi->driver_data;
		/* apply the quirk only to on-board controllers */
		return slot == PCI_SLOT(pdev->devfn);
	}

	return false;
}

static bool ahci_broken_suspend(struct pci_dev *pdev)
{
	static const struct dmi_system_id sysids[] = {
		/*
		 * On HP dv[4-6] and HDX18 with earlier BIOSen, link
		 * to the harddisk doesn't become online after
		 * resuming from STR.  Warn and fail suspend.
		 *
		 * http://bugzilla.kernel.org/show_bug.cgi?id=12276
		 *
		 * Use dates instead of versions to match as HP is
		 * apparently recycling both product and version
		 * strings.
		 *
		 * http://bugzilla.kernel.org/show_bug.cgi?id=15462
		 */
		{
			.ident = "dv4",
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR, "Hewlett-Packard"),
				DMI_MATCH(DMI_PRODUCT_NAME,
					  "HP Pavilion dv4 Notebook PC"),
			},
			.driver_data = "20090105",	/* F.30 */
		},
		{
			.ident = "dv5",
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR, "Hewlett-Packard"),
				DMI_MATCH(DMI_PRODUCT_NAME,
					  "HP Pavilion dv5 Notebook PC"),
			},
			.driver_data = "20090506",	/* F.16 */
		},
		{
			.ident = "dv6",
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR, "Hewlett-Packard"),
				DMI_MATCH(DMI_PRODUCT_NAME,
					  "HP Pavilion dv6 Notebook PC"),
			},
			.driver_data = "20090423",	/* F.21 */
		},
		{
			.ident = "HDX18",
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR, "Hewlett-Packard"),
				DMI_MATCH(DMI_PRODUCT_NAME,
					  "HP HDX18 Notebook PC"),
			},
			.driver_data = "20090430",	/* F.23 */
		},
		/*
		 * Acer eMachines G725 has the same problem.  BIOS
		 * V1.03 is known to be broken.  V3.04 is known to
		 * work.  Between, there are V1.06, V2.06 and V3.03
		 * that we don't have much idea about.  For now,
		 * blacklist anything older than V3.04.
		 *
		 * http://bugzilla.kernel.org/show_bug.cgi?id=15104
		 */
		{
			.ident = "G725",
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR, "eMachines"),
				DMI_MATCH(DMI_PRODUCT_NAME, "eMachines G725"),
			},
			.driver_data = "20091216",	/* V3.04 */
		},
		{ }	/* terminate list */
	};
	const struct dmi_system_id *dmi = dmi_first_match(sysids);
	int year, month, date;
	char buf[9];

	if (!dmi || pdev->bus->number || pdev->devfn != PCI_DEVFN(0x1f, 2))
		return false;

	dmi_get_date(DMI_BIOS_DATE, &year, &month, &date);
	snprintf(buf, sizeof(buf), "%04d%02d%02d", year, month, date);

	return strcmp(buf, dmi->driver_data) < 0;
}

static bool ahci_broken_lpm(struct pci_dev *pdev)
{
	static const struct dmi_system_id sysids[] = {
		/* Various Lenovo 50 series have LPM issues with older BIOSen */
		{
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
				DMI_MATCH(DMI_PRODUCT_VERSION, "ThinkPad X250"),
			},
			.driver_data = "20180406", /* 1.31 */
		},
		{
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
				DMI_MATCH(DMI_PRODUCT_VERSION, "ThinkPad L450"),
			},
			.driver_data = "20180420", /* 1.28 */
		},
		{
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
				DMI_MATCH(DMI_PRODUCT_VERSION, "ThinkPad T450s"),
			},
			.driver_data = "20180315", /* 1.33 */
		},
		{
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
				DMI_MATCH(DMI_PRODUCT_VERSION, "ThinkPad W541"),
			},
			/*
			 * Note date based on release notes, 2.35 has been
			 * reported to be good, but I've been unable to get
			 * a hold of the reporter to get the DMI BIOS date.
			 * TODO: fix this.
			 */
			.driver_data = "20180310", /* 2.35 */
		},
		{ }	/* terminate list */
	};
	const struct dmi_system_id *dmi = dmi_first_match(sysids);
	int year, month, date;
	char buf[9];

	if (!dmi)
		return false;

	dmi_get_date(DMI_BIOS_DATE, &year, &month, &date);
	snprintf(buf, sizeof(buf), "%04d%02d%02d", year, month, date);

	return strcmp(buf, dmi->driver_data) < 0;
}

static bool ahci_broken_online(struct pci_dev *pdev)
{
#define ENCODE_BUSDEVFN(bus, slot, func)			\
	(void *)(unsigned long)(((bus) << 8) | PCI_DEVFN((slot), (func)))
	static const struct dmi_system_id sysids[] = {
		/*
		 * There are several gigabyte boards which use
		 * SIMG5723s configured as hardware RAID.  Certain
		 * 5723 firmware revisions shipped there keep the link
		 * online but fail to answer properly to SRST or
		 * IDENTIFY when no device is attached downstream
		 * causing libata to retry quite a few times leading
		 * to excessive detection delay.
		 *
		 * As these firmwares respond to the second reset try
		 * with invalid device signature, considering unknown
		 * sig as offline works around the problem acceptably.
		 */
		{
			.ident = "EP45-DQ6",
			.matches = {
				DMI_MATCH(DMI_BOARD_VENDOR,
					  "Gigabyte Technology Co., Ltd."),
				DMI_MATCH(DMI_BOARD_NAME, "EP45-DQ6"),
			},
			.driver_data = ENCODE_BUSDEVFN(0x0a, 0x00, 0),
		},
		{
			.ident = "EP45-DS5",
			.matches = {
				DMI_MATCH(DMI_BOARD_VENDOR,
					  "Gigabyte Technology Co., Ltd."),
				DMI_MATCH(DMI_BOARD_NAME, "EP45-DS5"),
			},
			.driver_data = ENCODE_BUSDEVFN(0x03, 0x00, 0),
		},
		{ }	/* terminate list */
	};
#undef ENCODE_BUSDEVFN
	const struct dmi_system_id *dmi = dmi_first_match(sysids);
	unsigned int val;

	if (!dmi)
		return false;

	val = (unsigned long)dmi->driver_data;

	return pdev->bus->number == (val >> 8) && pdev->devfn == (val & 0xff);
}

static bool ahci_broken_devslp(struct pci_dev *pdev)
{
	/* device with broken DEVSLP but still showing SDS capability */
	static const struct pci_device_id ids[] = {
		{ PCI_VDEVICE(INTEL, 0x0f23)}, /* Valleyview SoC */
		{}
	};

	return pci_match_id(ids, pdev);
}

#ifdef CONFIG_ATA_ACPI
static void ahci_gtf_filter_workaround(struct ata_host *host)
{
	static const struct dmi_system_id sysids[] = {
		/*
		 * Aspire 3810T issues a bunch of SATA enable commands
		 * via _GTF including an invalid one and one which is
		 * rejected by the device.  Among the successful ones
		 * is FPDMA non-zero offset enable which when enabled
		 * only on the drive side leads to NCQ command
		 * failures.  Filter it out.
		 */
		{
			.ident = "Aspire 3810T",
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
				DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 3810T"),
			},
			.driver_data = (void *)ATA_ACPI_FILTER_FPDMA_OFFSET,
		},
		{ }
	};
	const struct dmi_system_id *dmi = dmi_first_match(sysids);
	unsigned int filter;
	int i;

	if (!dmi)
		return;

	filter = (unsigned long)dmi->driver_data;
	dev_info(host->dev, "applying extra ACPI _GTF filter 0x%x for %s\n",
		 filter, dmi->ident);

	for (i = 0; i < host->n_ports; i++) {
		struct ata_port *ap = host->ports[i];
		struct ata_link *link;
		struct ata_device *dev;

		ata_for_each_link(link, ap, EDGE)
			ata_for_each_dev(dev, link, ALL)
				dev->gtf_filter |= filter;
	}
}
#else
static inline void ahci_gtf_filter_workaround(struct ata_host *host)
{}
#endif

/*
 * On the Acer Aspire Switch Alpha 12, sometimes all SATA ports are detected
 * as DUMMY, or detected but eventually get a "link down" and never get up
 * again. When this happens, CAP.NP may hold a value of 0x00 or 0x01, and the
 * port_map may hold a value of 0x00.
 *
 * Overriding CAP.NP to 0x02 and the port_map to 0x7 will reveal all 3 ports
 * and can significantly reduce the occurrence of the problem.
 *
 * https://bugzilla.kernel.org/show_bug.cgi?id=189471
 */
static void acer_sa5_271_workaround(struct ahci_host_priv *hpriv,
				    struct pci_dev *pdev)
{
	static const struct dmi_system_id sysids[] = {
		{
			.ident = "Acer Switch Alpha 12",
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
				DMI_MATCH(DMI_PRODUCT_NAME, "Switch SA5-271")
			},
		},
		{ }
	};

	if (dmi_check_system(sysids)) {
		dev_info(&pdev->dev, "enabling Acer Switch Alpha 12 workaround\n");
		if ((hpriv->saved_cap & 0xC734FF00) == 0xC734FF00) {
			hpriv->port_map = 0x7;
			hpriv->cap = 0xC734FF02;
		}
	}
}

#ifdef CONFIG_ARM64
/*
 * Due to ERRATA#22536, ThunderX needs to handle HOST_IRQ_STAT differently.
 * Workaround is to make sure all pending IRQs are served before leaving
 * handler.
 */
static irqreturn_t ahci_thunderx_irq_handler(int irq, void *dev_instance)
{
	struct ata_host *host = dev_instance;
	struct ahci_host_priv *hpriv;
	unsigned int rc = 0;
	void __iomem *mmio;
	u32 irq_stat, irq_masked;
	unsigned int handled = 1;

	VPRINTK("ENTER\n");
	hpriv = host->private_data;
	mmio = hpriv->mmio;
	irq_stat = readl(mmio + HOST_IRQ_STAT);
	if (!irq_stat)
		return IRQ_NONE;

	do {
		irq_masked = irq_stat & hpriv->port_map;
		spin_lock(&host->lock);
		rc = ahci_handle_port_intr(host, irq_masked);
		if (!rc)
			handled = 0;
		writel(irq_stat, mmio + HOST_IRQ_STAT);
		irq_stat = readl(mmio + HOST_IRQ_STAT);
		spin_unlock(&host->lock);
	} while (irq_stat);
	VPRINTK("EXIT\n");

	return IRQ_RETVAL(handled);
}
#endif

static void ahci_remap_check(struct pci_dev *pdev, int bar,
		struct ahci_host_priv *hpriv)
{
	int i;
	u32 cap;

	/*
	 * Check if this device might have remapped nvme devices.
	 */
	if (pdev->vendor != PCI_VENDOR_ID_INTEL ||
	    pci_resource_len(pdev, bar) < SZ_512K ||
	    bar != AHCI_PCI_BAR_STANDARD ||
	    !(readl(hpriv->mmio + AHCI_VSCAP) & 1))
		return;

	cap = readq(hpriv->mmio + AHCI_REMAP_CAP);
	for (i = 0; i < AHCI_MAX_REMAP; i++) {
		if ((cap & (1 << i)) == 0)
			continue;
		if (readl(hpriv->mmio + ahci_remap_dcc(i))
				!= PCI_CLASS_STORAGE_EXPRESS)
			continue;

		/* We've found a remapped device */
		hpriv->remapped_nvme++;
	}

	if (!hpriv->remapped_nvme)
		return;

	dev_warn(&pdev->dev, "Found %u remapped NVMe devices.\n",
		 hpriv->remapped_nvme);
	dev_warn(&pdev->dev,
		 "Switch your BIOS from RAID to AHCI mode to use them.\n");

	/*
	 * Don't rely on the msi-x capability in the remap case,
	 * share the legacy interrupt across ahci and remapped devices.
	 */
	hpriv->flags |= AHCI_HFLAG_NO_MSI;
}

static int ahci_get_irq_vector(struct ata_host *host, int port)
{
	return pci_irq_vector(to_pci_dev(host->dev), port);
}

static int ahci_init_msi(struct pci_dev *pdev, unsigned int n_ports,
			struct ahci_host_priv *hpriv)
{
	int nvec;

	#ifdef CONFIG_X86_PS4
	if ((pdev->vendor == PCI_VENDOR_ID_SONY) &&
	    ((pdev->device == PCI_DEVICE_ID_SONY_BELIZE_AHCI) ||
	    (pdev->device == PCI_DEVICE_ID_SONY_AEOLIA_AHCI))) {
		return apcie_assign_irqs(pdev, n_ports);
	}
	#endif

	if (hpriv->flags & AHCI_HFLAG_NO_MSI)
		return -ENODEV;

	/*
	 * If number of MSIs is less than number of ports then Sharing Last
	 * Message mode could be enforced. In this case assume that advantage
	 * of multipe MSIs is negated and use single MSI mode instead.
	 */
	if (n_ports > 1) {
		nvec = pci_alloc_irq_vectors(pdev, n_ports, INT_MAX,
				PCI_IRQ_MSIX | PCI_IRQ_MSI);
		if (nvec > 0) {
			if (!(readl(hpriv->mmio + HOST_CTL) & HOST_MRSM)) {
				hpriv->get_irq_vector = ahci_get_irq_vector;
				hpriv->flags |= AHCI_HFLAG_MULTI_MSI;
				return nvec;
			}

			/*
			 * Fallback to single MSI mode if the controller
			 * enforced MRSM mode.
			 */
			printk(KERN_INFO
				"ahci: MRSM is on, fallback to single MSI\n");
			pci_free_irq_vectors(pdev);
		}
	}

	/*
	 * If the host is not capable of supporting per-port vectors, fall
	 * back to single MSI before finally attempting single MSI-X.
	 */
	nvec = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
	if (nvec == 1)
		return nvec;
	return pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSIX);
}

static void ahci_update_initial_lpm_policy(struct ata_port *ap,
					   struct ahci_host_priv *hpriv)
{
	int policy = CONFIG_SATA_MOBILE_LPM_POLICY;


	/* Ignore processing for non mobile platforms */
	if (!(hpriv->flags & AHCI_HFLAG_IS_MOBILE))
		return;

	/* user modified policy via module param */
	if (mobile_lpm_policy != -1) {
		policy = mobile_lpm_policy;
		goto update_policy;
	}

#ifdef CONFIG_ACPI
	if (policy > ATA_LPM_MED_POWER &&
	    (acpi_gbl_FADT.flags & ACPI_FADT_LOW_POWER_S0)) {
		if (hpriv->cap & HOST_CAP_PART)
			policy = ATA_LPM_MIN_POWER_WITH_PARTIAL;
		else if (hpriv->cap & HOST_CAP_SSC)
			policy = ATA_LPM_MIN_POWER;
	}
#endif

update_policy:
	if (policy >= ATA_LPM_UNKNOWN && policy <= ATA_LPM_MIN_POWER)
		ap->target_lpm_policy = policy;
}

static void ahci_intel_pcs_quirk(struct pci_dev *pdev, struct ahci_host_priv *hpriv)
{
	const struct pci_device_id *id = pci_match_id(ahci_pci_tbl, pdev);
	u16 tmp16;

	/*
	 * Only apply the 6-port PCS quirk for known legacy platforms.
	 */
	if (!id || id->vendor != PCI_VENDOR_ID_INTEL)
		return;

	/* Skip applying the quirk on Denverton and beyond */
	if (((enum board_ids) id->driver_data) >= board_ahci_pcs7)
		return;

	/*
	 * port_map is determined from PORTS_IMPL PCI register which is
	 * implemented as write or write-once register.  If the register
	 * isn't programmed, ahci automatically generates it from number
	 * of ports, which is good enough for PCS programming. It is
	 * otherwise expected that platform firmware enables the ports
	 * before the OS boots.
	 */
	pci_read_config_word(pdev, PCS_6, &tmp16);
	if ((tmp16 & hpriv->port_map) != hpriv->port_map) {
		tmp16 |= hpriv->port_map;
		pci_write_config_word(pdev, PCS_6, tmp16);
	}
}

static ssize_t remapped_nvme_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	struct ata_host *host = dev_get_drvdata(dev);
	struct ahci_host_priv *hpriv = host->private_data;

	return sprintf(buf, "%u\n", hpriv->remapped_nvme);
}

static DEVICE_ATTR_RO(remapped_nvme);

static int ahci_init_one(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	unsigned int board_id = ent->driver_data;
	struct ata_port_info pi = ahci_port_info[board_id];
	const struct ata_port_info *ppi[] = { &pi, NULL };
	struct device *dev = &pdev->dev;
	struct ahci_host_priv *hpriv;
	struct ata_host *host;
	int n_ports, i, rc;
	int ahci_pci_bar = AHCI_PCI_BAR_STANDARD;

	VPRINTK("ENTER\n");

	WARN_ON((int)ATA_MAX_QUEUE > AHCI_MAX_CMDS);

	
	#ifdef CONFIG_X86_PS4
	/* This will return negative on non-PS4 platforms */
	if (apcie_status() == 0)
		return -EPROBE_DEFER;
	#endif

	ata_print_version_once(&pdev->dev, DRV_VERSION);

	/* The AHCI driver can only drive the SATA ports, the PATA driver
	   can drive them all so if both drivers are selected make sure
	   AHCI stays out of the way */
	if (pdev->vendor == PCI_VENDOR_ID_MARVELL && !marvell_enable)
		return -ENODEV;

	/* Apple BIOS on MCP89 prevents us using AHCI */
	if (is_mcp89_apple(pdev))
		ahci_mcp89_apple_enable(pdev);

	/* Promise's PDC42819 is a SAS/SATA controller that has an AHCI mode.
	 * At the moment, we can only use the AHCI mode. Let the users know
	 * that for SAS drives they're out of luck.
	 */
	if (pdev->vendor == PCI_VENDOR_ID_PROMISE)
		dev_info(&pdev->dev,
			 "PDC42819 can only drive SATA devices with this driver\n");

	/* Some devices use non-standard BARs */
	if (pdev->vendor == PCI_VENDOR_ID_STMICRO && pdev->device == 0xCC06)
		ahci_pci_bar = AHCI_PCI_BAR_STA2X11;
	else if (pdev->vendor == 0x1c44 && pdev->device == 0x8000)
		ahci_pci_bar = AHCI_PCI_BAR_ENMOTUS;
	else if (pdev->vendor == PCI_VENDOR_ID_CAVIUM) {
		if (pdev->device == 0xa01c)
			ahci_pci_bar = AHCI_PCI_BAR_CAVIUM;
		if (pdev->device == 0xa084)
			ahci_pci_bar = AHCI_PCI_BAR_CAVIUM_GEN5;
	} else if (pdev->vendor == PCI_VENDOR_ID_LOONGSON) {
		if (pdev->device == 0x7a08)
			ahci_pci_bar = AHCI_PCI_BAR_LOONGSON;
	} else if (pdev->vendor == PCI_VENDOR_ID_SONY && pdev->device == PCI_DEVICE_ID_SONY_BAIKAL_AHCI)
		ahci_pci_bar = AHCI_PCI_BAR0_BAIKAL;

	/* acquire resources */
	rc = pcim_enable_device(pdev);
	if (rc)
		return rc;

	if (pdev->vendor == PCI_VENDOR_ID_INTEL &&
	    (pdev->device == 0x2652 || pdev->device == 0x2653)) {
		u8 map;

		/* ICH6s share the same PCI ID for both piix and ahci
		 * modes.  Enabling ahci mode while MAP indicates
		 * combined mode is a bad idea.  Yield to ata_piix.
		 */
		pci_read_config_byte(pdev, ICH_MAP, &map);
		if (map & 0x3) {
			dev_info(&pdev->dev,
				 "controller is in combined mode, can't enable AHCI mode\n");
			return -ENODEV;
		}
	}

	/* AHCI controllers often implement SFF compatible interface.
	 * Grab all PCI BARs just in case.
	 */
	rc = pcim_iomap_regions_request_all(pdev, 1 << ahci_pci_bar, DRV_NAME);
	if (rc == -EBUSY)
		pcim_pin_device(pdev);
	if (rc)
		return rc;

	hpriv = devm_kzalloc(dev, sizeof(*hpriv), GFP_KERNEL);
	if (!hpriv)
		return -ENOMEM;
	hpriv->flags |= (unsigned long)pi.private_data;

	/* MCP65 revision A1 and A2 can't do MSI */
	if (board_id == board_ahci_mcp65 &&
	    (pdev->revision == 0xa1 || pdev->revision == 0xa2))
		hpriv->flags |= AHCI_HFLAG_NO_MSI;

	/* SB800 does NOT need the workaround to ignore SERR_INTERNAL */
	if (board_id == board_ahci_sb700 && pdev->revision >= 0x40)
		hpriv->flags &= ~AHCI_HFLAG_IGN_SERR_INTERNAL;

	/* only some SB600s can do 64bit DMA */
	if (ahci_sb600_enable_64bit(pdev))
		hpriv->flags &= ~AHCI_HFLAG_32BIT_ONLY;

	hpriv->mmio = pcim_iomap_table(pdev)[ahci_pci_bar];

	/* detect remapped nvme devices */
	ahci_remap_check(pdev, ahci_pci_bar, hpriv);

	sysfs_add_file_to_group(&pdev->dev.kobj,
				&dev_attr_remapped_nvme.attr,
				NULL);

	/* must set flag prior to save config in order to take effect */
	if (ahci_broken_devslp(pdev))
		hpriv->flags |= AHCI_HFLAG_NO_DEVSLP;

#ifdef CONFIG_ARM64
	if (pdev->vendor == PCI_VENDOR_ID_HUAWEI &&
	    pdev->device == 0xa235 &&
	    pdev->revision < 0x30)
		hpriv->flags |= AHCI_HFLAG_NO_SXS;

	if (pdev->vendor == 0x177d && pdev->device == 0xa01c)
		hpriv->irq_handler = ahci_thunderx_irq_handler;
#endif

	/* save initial config */
	ahci_pci_save_initial_config(pdev, hpriv);

	/*
	 * If platform firmware failed to enable ports, try to enable
	 * them here.
	 */
	ahci_intel_pcs_quirk(pdev, hpriv);

	/* prepare host */
	if (hpriv->cap & HOST_CAP_NCQ) {
		pi.flags |= ATA_FLAG_NCQ;
		/*
		 * Auto-activate optimization is supposed to be
		 * supported on all AHCI controllers indicating NCQ
		 * capability, but it seems to be broken on some
		 * chipsets including NVIDIAs.
		 */
		if (!(hpriv->flags & AHCI_HFLAG_NO_FPDMA_AA))
			pi.flags |= ATA_FLAG_FPDMA_AA;

		/*
		 * All AHCI controllers should be forward-compatible
		 * with the new auxiliary field. This code should be
		 * conditionalized if any buggy AHCI controllers are
		 * encountered.
		 */
		pi.flags |= ATA_FLAG_FPDMA_AUX;
	}

	if (hpriv->cap & HOST_CAP_PMP)
		pi.flags |= ATA_FLAG_PMP;

	ahci_set_em_messages(hpriv, &pi);

	if (ahci_broken_system_poweroff(pdev)) {
		pi.flags |= ATA_FLAG_NO_POWEROFF_SPINDOWN;
		dev_info(&pdev->dev,
			"quirky BIOS, skipping spindown on poweroff\n");
	}

	if (ahci_broken_lpm(pdev)) {
		pi.flags |= ATA_FLAG_NO_LPM;
		dev_warn(&pdev->dev,
			 "BIOS update required for Link Power Management support\n");
	}

	if (ahci_broken_suspend(pdev)) {
		hpriv->flags |= AHCI_HFLAG_NO_SUSPEND;
		dev_warn(&pdev->dev,
			 "BIOS update required for suspend/resume\n");
	}

	if (ahci_broken_online(pdev)) {
		hpriv->flags |= AHCI_HFLAG_SRST_TOUT_IS_OFFLINE;
		dev_info(&pdev->dev,
			 "online status unreliable, applying workaround\n");
	}


	/* Acer SA5-271 workaround modifies private_data */
	acer_sa5_271_workaround(hpriv, pdev);

	/* CAP.NP sometimes indicate the index of the last enabled
	 * port, at other times, that of the last possible port, so
	 * determining the maximum port number requires looking at
	 * both CAP.NP and port_map.
	 */
	n_ports = max(ahci_nr_ports(hpriv->cap), fls(hpriv->port_map));

	host = ata_host_alloc_pinfo(&pdev->dev, ppi, n_ports);
	if (!host)
		return -ENOMEM;
	host->private_data = hpriv;

	if (ahci_init_msi(pdev, n_ports, hpriv) < 0) {
		/* legacy intx interrupts */
		pci_intx(pdev, 1);
	}
	hpriv->irq = pci_irq_vector(pdev, 0);

	if (!(hpriv->cap & HOST_CAP_SSS) || ahci_ignore_sss)
		host->flags |= ATA_HOST_PARALLEL_SCAN;
	else
		dev_info(&pdev->dev, "SSS flag set, parallel bus scan disabled\n");

	if (pi.flags & ATA_FLAG_EM)
		ahci_reset_em(host);

	for (i = 0; i < host->n_ports; i++) {
		struct ata_port *ap = host->ports[i];

		ata_port_pbar_desc(ap, ahci_pci_bar, -1, "abar");
		ata_port_pbar_desc(ap, ahci_pci_bar,
				   0x100 + ap->port_no * 0x80, "port");

		/* set enclosure management message type */
		if (ap->flags & ATA_FLAG_EM)
			ap->em_message_type = hpriv->em_msg_type;

		ahci_update_initial_lpm_policy(ap, hpriv);

		/* disabled/not-implemented port */
		if (!(hpriv->port_map & (1 << i)))
			ap->ops = &ata_dummy_port_ops;
	}

	/* apply workaround for ASUS P5W DH Deluxe mainboard */
	ahci_p5wdh_workaround(host);

	/* apply gtf filter quirk */
	ahci_gtf_filter_workaround(host);

	/* initialize adapter */
	rc = ahci_configure_dma_masks(pdev, hpriv->cap & HOST_CAP_64);
	if (rc)
		return rc;

	rc = ahci_reset_controller(host);
	if (rc)
		return rc;

	ahci_pci_init_controller(host);
	ahci_pci_print_info(host);

	pci_set_master(pdev);

	rc = ahci_host_activate(host, &ahci_sht);
	if (rc)
		return rc;

	pm_runtime_put_noidle(&pdev->dev);
	return 0;
}

static void ahci_shutdown_one(struct pci_dev *pdev)
{
	ata_pci_shutdown_one(pdev);
}

static void ahci_remove_one(struct pci_dev *pdev)
{
	sysfs_remove_file_from_group(&pdev->dev.kobj,
				     &dev_attr_remapped_nvme.attr,
				     NULL);
	pm_runtime_get_noresume(&pdev->dev);
	ata_pci_remove_one(pdev);

	#ifdef CONFIG_X86_PS4
	if (pdev->vendor == PCI_VENDOR_ID_SONY) {
		apcie_free_irqs(pdev);
	}
	#endif
}

#ifdef CONFIG_X86_PS4
void baikal_pcie_sata_phy_init(struct device *dev, struct ahci_controller *ctlr)
{
	struct ahci_controller *ctlr_; // r15
	__int64 v6; // rax
	void (**v7)(void *, void *, _QWORD, int *); // rbx
	int dev_id; // er12
	__int64 v9; // rdi
	unsigned __int32 v10; // er13
	unsigned __int32 v11; // eax
	int v12; // er9
	int v13; // ecx
	int v14; // edx
	int v15; // er8
	signed __int64 v16; // r10
	signed __int64 v17; // rbx
	struct f_resource *r_mem; // rax
	__int64 is_mem; // rdi
	__int64 bar_addr; // rdx
	unsigned __int32 *bar_and_offset_0x20A0; // rax
	unsigned __int32 v22; // eax
	struct f_resource *v23; // rcx
	_BOOL8 bpci_usb_ahci; // rsi
	unsigned __int32 *v25; // rdx
	struct f_resource *v26; // rax
	__int64 v27; // rcx
	unsigned int *v28; // rdx
	unsigned int v29; // eax
	unsigned __int32 v30; // eax
	struct f_resource *v31; // rcx
	struct f_resource *v32; // rax
	__int64 v33; // rdi
	__int64 v34; // rdx
	unsigned __int32 *v35; // rax
	unsigned __int32 v36; // eax
	struct f_resource *v37; // rcx
	unsigned __int32 *v38; // rdx
	struct f_resource *v39; // rcx
	__int64 v40; // rax
	unsigned int *v41; // rdx
	unsigned int v42; // eax
	unsigned __int32 v43; // eax
	struct f_resource *v44; // rcx
	struct f_resource *v45; // rax
	__int64 v46; // rdi
	__int64 v47; // rdx
	unsigned __int32 *v48; // rax
	unsigned __int32 v49; // eax
	struct f_resource *v50; // rcx
	unsigned int *v51; // rdx
	unsigned int v52; // ecx
	int bpcie_buffer; // eax
	unsigned int trace_length; // ebx
	unsigned int v55; // ecx
	int v56; // edx
	unsigned int v57; // eax
	struct f_resource *v58; // rax
	unsigned int *v59; // rdx
	unsigned int v60; // eax
	struct f_resource *v61; // rax
	unsigned int *v62; // rdx
	unsigned int v63; // eax
	unsigned __int32 v64; // eax
	struct f_resource *v65; // rcx
	struct f_resource *v66; // rax
	unsigned int *v67; // rdx
	unsigned int v68; // eax
	unsigned __int32 v69; // eax
	struct f_resource *v70; // rcx
	struct f_resource *v71; // rax
	unsigned int *v72; // rdx
	unsigned int v73; // eax
	unsigned __int32 v74; // eax
	struct f_resource *v75; // rcx
	struct f_resource *v76; // rax
	unsigned int *v77; // rdx
	unsigned int v78; // eax
	unsigned __int32 v79; // eax
	struct f_resource *v80; // rcx
	struct f_resource *v81; // rax
	unsigned int *v82; // rdx
	unsigned int v83; // eax
	unsigned __int32 v84; // eax
	struct f_resource *v85; // rcx
	struct f_resource *v86; // rax
	unsigned int *v87; // rdx
	unsigned int v88; // eax
	unsigned __int32 v89; // eax
	struct f_resource *v90; // rcx
	struct f_resource *v91; // rax
	unsigned int *v92; // rdx
	unsigned int v93; // eax
	unsigned __int32 v94; // eax
	struct f_resource *v95; // rcx
	struct f_resource *v96; // rax
	unsigned int *v97; // rdx
	unsigned int v98; // eax
	unsigned __int32 v99; // eax
	struct f_resource *v100; // rcx
	struct f_resource *v101; // rax
	unsigned int *v102; // rdx
	unsigned int v103; // eax
	unsigned __int32 v104; // eax
	struct f_resource *v105; // rcx
	struct f_resource *v106; // rax
	unsigned int *v107; // rdx
	unsigned int v108; // eax
	unsigned __int32 v109; // eax
	struct f_resource *v110; // rcx
	struct f_resource *v111; // rax
	unsigned int *v112; // rdx
	unsigned int v113; // eax
	unsigned __int32 v114; // eax
	struct f_resource *v115; // rcx
	struct f_resource *v116; // rax
	unsigned int *v117; // rdx
	unsigned __int32 v118; // eax
	struct f_resource *v119; // rcx
	unsigned int v120; // eax
	struct f_resource *v121; // rax
	unsigned int *v122; // rdx
	unsigned int v123; // eax
	struct f_resource *v124; // rax
	unsigned int *v125; // rdx
	unsigned int v126; // eax
	unsigned __int32 v127; // eax
	struct f_resource *v128; // rcx
	struct f_resource *v129; // rax
	unsigned int *v130; // rdx
	unsigned int v131; // eax
	unsigned __int32 v132; // eax
	struct f_resource *v133; // rcx
	struct f_resource *v134; // rax
	unsigned int *v135; // rdx
	unsigned int v136; // eax
	unsigned __int32 v137; // eax
	struct f_resource *v138; // rcx
	struct f_resource *v139; // rax
	unsigned int *v140; // rdx
	unsigned int v141; // eax
	unsigned __int32 v142; // eax
	struct f_resource *v143; // rcx
	struct f_resource *v144; // rax
	unsigned int *v145; // rdx
	unsigned int v146; // eax
	unsigned __int32 v147; // eax
	struct f_resource *v148; // rcx
	struct f_resource *v149; // rax
	unsigned int *v150; // rdx
	unsigned int v151; // eax
	unsigned __int32 v152; // eax
	struct f_resource *v153; // rcx
	struct f_resource *v154; // rax
	unsigned int *v155; // rdx
	unsigned int v156; // eax
	unsigned __int32 v157; // eax
	struct f_resource *v158; // rcx
	struct f_resource *v159; // rax
	unsigned int *v160; // rdx
	unsigned int v161; // eax
	unsigned __int32 v162; // eax
	struct f_resource *v163; // rcx
	struct f_resource *v164; // rax
	unsigned int *v165; // rdx
	unsigned int v166; // eax
	unsigned __int32 v167; // eax
	struct f_resource *v168; // rcx
	struct f_resource *v169; // rax
	unsigned int *v170; // rdx
	unsigned int v171; // eax
	unsigned __int32 v172; // eax
	struct f_resource *v173; // rcx
	struct f_resource *v174; // rax
	unsigned int *v175; // rdx
	unsigned int v176; // eax
	unsigned __int32 v177; // eax
	struct f_resource *v178; // rcx
	struct f_resource *v179; // rax
	struct f_resource *v180; // rcx
	__int64 v181; // rax
	unsigned int *v182; // rdx
	unsigned int v183; // eax
	unsigned __int32 v184; // eax
	struct f_resource *v185; // rcx
	struct f_resource *v186; // rax
	__int64 v187; // rcx
	unsigned int *v188; // rdx
	unsigned int v189; // eax
	unsigned __int32 v190; // eax
	struct f_resource *v191; // rcx
	struct f_resource *v192; // rax
	__int64 v193; // rcx
	unsigned int *v194; // rdx
	unsigned int v195; // eax
	unsigned __int32 v196; // eax
	struct f_resource *v197; // rcx
	struct f_resource *v198; // rax
	__int64 v199; // rcx
	unsigned int *v200; // rdx
	unsigned int v201; // eax
	unsigned __int32 v202; // eax
	struct f_resource *v203; // rcx
	struct f_resource *v204; // rax
	__int64 v205; // rcx
	unsigned int *v206; // rdx
	unsigned int v207; // eax
	unsigned __int32 v208; // eax
	struct f_resource *v209; // rcx
	struct f_resource *v210; // rcx
	__int64 v211; // rax
	unsigned int *v212; // rdx
	unsigned int v213; // eax
	unsigned __int32 v214; // eax
	struct f_resource *v215; // rcx
	struct f_resource *v216; // rax
	__int64 v217; // rcx
	unsigned int *v218; // rdx
	unsigned int v219; // eax
	unsigned __int32 v220; // eax
	struct f_resource *v221; // rcx
	struct f_resource *v222; // rax
	__int64 v223; // rcx
	unsigned int *v224; // rdx
	unsigned int v225; // eax
	unsigned __int32 v226; // eax
	struct f_resource *v227; // rcx
	struct f_resource *v228; // rcx
	__int64 v229; // rax
	unsigned int *v230; // rdx
	unsigned int v231; // eax
	unsigned __int32 v232; // eax
	struct f_resource *v233; // rcx
	__int64 v234; // rdi
	unsigned int v235; // ebx
	struct f_resource *v236; // rax
	_DWORD *v237; // rdx
	unsigned __int32 v238; // eax
	struct f_resource *v239; // rax
	unsigned int *v240; // rdx
	unsigned int v241; // eax
	unsigned __int32 v242; // eax
	struct f_resource *v243; // rcx
	struct f_resource *v244; // rax
	_DWORD *v245; // rdx
	struct f_resource *v246; // rax
	__int64 v247; // rcx
	unsigned int *v248; // rdx
	unsigned int v249; // eax
	unsigned __int32 v250; // eax
	struct f_resource *v251; // rcx
	struct f_resource *v252; // rax
	__int64 v253; // rcx
	unsigned int *v254; // rdx
	unsigned int v255; // eax
	unsigned __int32 v256; // eax
	struct f_resource *v257; // rcx
	unsigned __int32 v258; // eax
	struct f_resource *v259; // rcx
	unsigned __int32 v260; // eax
	struct f_resource *v261; // rcx
	struct f_resource *v262; // rax
	unsigned int *v263; // rdx
	unsigned int v264; // eax
	unsigned __int32 v265; // eax
	struct f_resource *v266; // rcx
	struct f_resource *v267; // rax
	unsigned int *v268; // rdx
	unsigned int v269; // eax
	unsigned __int32 v270; // eax
	struct f_resource *v271; // rcx
	struct f_resource *v272; // rax
	unsigned int *v273; // rdx
	unsigned int v274; // eax
	unsigned __int32 v275; // eax
	struct f_resource *v276; // rcx
	struct f_resource *v277; // rax
	unsigned int *v278; // rdx
	unsigned int v279; // eax
	unsigned __int32 v280; // eax
	struct f_resource *v281; // rcx
	struct f_resource *v282; // rax
	unsigned int *v283; // rdx
	unsigned int v284; // eax
	unsigned __int32 v285; // eax
	struct f_resource *v286; // rcx
	struct f_resource *v287; // rax
	unsigned int *v288; // rdx
	unsigned int v289; // eax
	unsigned __int32 v290; // eax
	struct f_resource *v291; // rcx
	struct f_resource *v292; // rax
	unsigned int *v293; // rdx
	unsigned int v294; // eax
	unsigned __int32 v295; // eax
	struct f_resource *v296; // rcx
	struct f_resource *v297; // rax
	unsigned int *v298; // rdx
	unsigned int v299; // eax
	unsigned __int32 v300; // eax
	struct f_resource *v301; // rcx
	struct f_resource *v302; // rax
	unsigned int *v303; // rdx
	unsigned int v304; // eax
	unsigned __int32 v305; // eax
	struct f_resource *v306; // rcx
	struct f_resource *v307; // rax
	unsigned int *v308; // rdx
	unsigned int v309; // eax
	unsigned __int32 v310; // eax
	struct f_resource *v311; // rcx
	struct f_resource *v312; // rax
	unsigned int *v313; // rdx
	unsigned int v314; // eax
	unsigned __int32 v315; // eax
	struct f_resource *v316; // rcx
	struct f_resource *v317; // rax
	unsigned int *v318; // rdx
	unsigned int v319; // eax
	unsigned __int32 v320; // eax
	struct f_resource *v321; // rcx
	struct f_resource *v322; // rax
	unsigned int *v323; // rdx
	unsigned int v324; // eax
	unsigned __int32 v325; // eax
	struct f_resource *v326; // rcx
	struct f_resource *v327; // rax
	unsigned int *v328; // rdx
	unsigned int v329; // eax
	unsigned __int32 v330; // eax
	struct f_resource *v331; // rcx
	struct f_resource *v332; // rax
	unsigned int *v333; // rdx
	unsigned int v334; // eax
	unsigned __int32 v335; // eax
	struct f_resource *v336; // rcx
	struct f_resource *v337; // rax
	unsigned int *v338; // rdx
	unsigned int v339; // eax
	unsigned __int32 v340; // eax
	struct f_resource *v341; // rcx
	struct f_resource *v342; // rax
	unsigned __int32 v343; // eax
	struct f_resource *v344; // rcx
	struct f_resource *v345; // rax
	unsigned int *v346; // rdx
	unsigned int v347; // eax
	unsigned __int32 v348; // eax
	struct f_resource *v349; // rcx
	unsigned __int32 v350; // eax
	struct f_resource *v351; // rcx
	struct f_resource *v352; // rax
	unsigned int *v353; // rdx
	unsigned int v354; // eax
	unsigned __int32 v355; // eax
	struct f_resource *v356; // rcx
	struct f_resource *v357; // rax
	unsigned __int32 v358; // eax
	struct f_resource *v359; // rcx
	struct f_resource *v360; // rax
	unsigned int *v361; // rdx
	unsigned int v362; // eax
	unsigned __int32 v363; // eax
	struct f_resource *v364; // rcx
	struct f_resource *v365; // rax
	unsigned int *v366; // rdx
	unsigned int v367; // eax
	unsigned __int32 v368; // eax
	struct f_resource *v369; // rcx
	struct f_resource *v370; // rax
	unsigned int *v371; // rdx
	unsigned int v372; // eax
	unsigned __int32 v373; // eax
	struct f_resource *v374; // rcx
	struct f_resource *v375; // rax
	unsigned int *v376; // rdx
	unsigned int v377; // eax
	unsigned __int32 v378; // eax
	struct f_resource *v379; // rcx
	struct f_resource *v380; // rax
	unsigned int *v381; // rdx
	unsigned int v382; // eax
	unsigned __int32 v383; // eax
	struct f_resource *v384; // rcx
	struct f_resource *v385; // rax
	unsigned int *v386; // rdx
	unsigned int v387; // eax
	unsigned __int32 v388; // eax
	struct f_resource *v389; // rcx
	struct f_resource *v390; // rax
	unsigned int *v391; // rdx
	unsigned int v392; // eax
	unsigned __int32 v393; // eax
	struct f_resource *v394; // rcx
	struct f_resource *v395; // rax
	unsigned int *v396; // rdx
	unsigned int v397; // eax
	unsigned __int32 v398; // eax
	struct f_resource *v399; // rcx
	struct f_resource *v400; // rax
	unsigned int *v401; // rdx
	unsigned int v402; // eax
	unsigned __int32 v403; // eax
	struct f_resource *v404; // rcx
	struct f_resource *v405; // rax
	unsigned int *v406; // rdx
	unsigned int v407; // eax
	unsigned __int32 v408; // eax
	struct f_resource *v409; // rcx
	struct f_resource *v410; // rax
	unsigned int *v411; // rdx
	unsigned int v412; // eax
	unsigned __int32 v413; // eax
	struct f_resource *v414; // rcx
	struct f_resource *v415; // rax
	unsigned __int32 v416; // eax
	struct f_resource *v417; // rcx
	int ivar_out; // [rsp+8h] [rbp-48h]
	int v419; // [rsp+Ch] [rbp-44h]
	int v420; // [rsp+10h] [rbp-40h]
	int v421; // [rsp+14h] [rbp-3Ch]
	int v422; // [rsp+18h] [rbp-38h]
	int v423; // [rsp+1Ch] [rbp-34h]

	dev_id = ctlr->dev_id;
	ctlr_ = ctlr;
	dev_info(dev, "Baikal SATA PHY init\n");
	struct pci_dev* sc_dev = pci_get_device(PCI_VENDOR_ID_SONY, PCI_DEVICE_ID_SONY_BAIKAL_PCIE, NULL);
	if (!sc_dev) {
		dev_err(dev, "bpcie glue: not device found\n");
	}

	sc = pci_get_drvdata(sc_dev);
	if (!sc) {
		dev_err(dev, "bpcie glue: not ready yet\n");
		return;
	}

	if ( dev_id == 0x90D9104D )
	{
		bpcie_write_to_bar2_and_0x180000_and_offset(108LL, 1u);
		bpcie_write_to_bar2_and_0x180000_and_offset(44LL, 1u);
		v9 = 108LL;
	}
	else
	{
		bpcie_write_to_bar2_and_0x180000_and_offset(112LL, 1u);
		bpcie_write_to_bar2_and_0x180000_and_offset(48LL, 1u);
		v9 = 112LL;
	}
	bpcie_write_to_bar2_and_0x180000_and_offset(v9, 0);
	v10 = bpcie_read_from_bar4_and_0xC000_and_offset(72LL);
	v11 = bpcie_read_from_bar4_and_0xC000_and_offset(108LL);
	pci_dev_put(sc_dev);
	v12 = 16;
	v13 = 40;
	v14 = 40;
	v15 = 16;
	v16 = 16LL;
	if ( v11 & 0x40000 )
	{
		v15 = (v10 >> 6) & 0x1F;
		v14 = v10 & 0x3F;
		v16 = (unsigned __int16)v10 >> 11;
	}
	v17 = 16LL;
	v422 = v14;
	v420 = v15;
	ivar_out = v16;
	if ( v11 & 0x4000000 )
	{
		v13 = (v10 >> 16) & 0x3F;
		v12 = (v10 >> 22) & 0x1F;
		v17 = v10 >> 27;
	}
	v423 = v13;
	v421 = v12;
	v419 = v17;
	dev_info(dev, "Baikal SATA EFUSE VALUE: 0x%02x:0x%02x:0x%02x:0x%02x:0x%02x:0x%02x\n", v16, v17, 0, 0, 0, 0);
	r_mem = ctlr->r_mem;
	is_mem = r_mem->r_bustag;
	bar_addr = r_mem->r_bushandle;
	bar_and_offset_0x20A0 = (unsigned __int32 *)(bar_addr + 0x20A0);
	v22 = bpcie_ahci_read(ctlr->r_mem, 0x20A0);

	bpci_usb_ahci = dev_id != 0x90D9104D;
	bpcie_ahci_write(ctlr->r_mem, 0x20A0, (v22 & 0xFBFF03FF) | ((bpci_usb_ahci ? v423 : v422) << 10) | 0x4000000);

	v29 = bpcie_ahci_read(ctlr->r_mem, 0x2014);
	bpcie_ahci_write(ctlr->r_mem, 0x2014, v29 | 0x100000);

	v36 = bpcie_ahci_read(ctlr->r_mem, 0x2054);
	bpcie_ahci_write(ctlr->r_mem, 0x2054, v36 & (0xFFFFF07F | ((bpci_usb_ahci ? v421 : v420) << 7)));

	v42 = bpcie_ahci_read(ctlr->r_mem, 0x201C);
	bpcie_ahci_write(ctlr->r_mem, 0x201C, v42 | 4);

	v49 = bpcie_ahci_read(ctlr->r_mem, 0x2078);
	bpcie_ahci_write(ctlr->r_mem, 0x2078, v49 & (0xFFFFFE0F | 16 * ((bpci_usb_ahci ? v419 : ivar_out))));

	bpcie_buffer = (int)ctlr_->apcie_bpcie_buffer;
	if ( bpcie_buffer )
	{
		trace_length = 6;
		v55 = (unsigned __int8)bpcie_buffer >> 5;
		v56 = v55 - 2;
		if ( v55 <= 2 )
			v56 = 0;
		v57 = v56 + ((_QWORD)ctlr_->apcie_bpcie_buffer & 0x1F);
		if ( v57 <= 0x12 )
			trace_length = v56 + ((_QWORD)ctlr_->apcie_bpcie_buffer & 0x1F);
		dev_info(dev, "Baikal SATA PHY Trace length : %d\n", trace_length);
		switch ( trace_length )
		{
		case 0u:
		case 1u:
		case 2u:
			v123 = bpcie_ahci_read(ctlr->r_mem, 0x204C);
			bpcie_ahci_write(ctlr->r_mem, 0x204C, (v123 & 0xFFFFC0FF) | 0x1D00);

			v264 = bpcie_ahci_read(ctlr->r_mem, 0x2054);
			bpcie_ahci_write(ctlr->r_mem, 0x2054, (v264 & 0xFFFF9FFF) | 0x4000);

			v272 = ctlr_->r_mem;
			v273 = (unsigned int *)(v272->r_bushandle + 0x207C);
			if ( v272->r_bustag )
			{
				v274 = *v273 & 0xFFFFFFC0 | 0x20;
			LABEL_246:
				*v273 = v274;
				goto LABEL_247;
			}
			v280 = __indword((unsigned __int16)v273);
			v281 = ctlr_->r_mem;
			v274 = v280 & 0xFFFFFFC0 | 0x20;
			v273 = (unsigned int *)(v281->r_bushandle + 0x207C);
			if ( v281->r_bustag )
				goto LABEL_246;
			__outdword((unsigned __int16)v273, v274);
		LABEL_247:
			v282 = ctlr_->r_mem;
			v283 = (unsigned int *)(v282->r_bushandle + 0x205C);
			if ( v282->r_bustag )
			{
				v284 = *v283 & 0xCFFFFFFF | 0x20000000;
			LABEL_254:
				*v283 = v284;
				goto LABEL_255;
			}
			v290 = __indword((unsigned __int16)v283);
			v291 = ctlr_->r_mem;
			v284 = v290 & 0xCFFFFFFF | 0x20000000;
			v283 = (unsigned int *)(v291->r_bushandle + 0x205C);
			if ( v291->r_bustag )
				goto LABEL_254;
			__outdword((unsigned __int16)v283, v284);
		LABEL_255:
			v292 = ctlr_->r_mem;
			v293 = (unsigned int *)(v292->r_bushandle + 0x2080);
			if ( v292->r_bustag )
			{
				v294 = *v293 & 0xFFFFF03F | 0x880;
			LABEL_262:
				*v293 = v294;
				goto LABEL_263;
			}
			v300 = __indword((unsigned __int16)v293);
			v301 = ctlr_->r_mem;
			v294 = v300 & 0xFFFFF03F | 0x880;
			v293 = (unsigned int *)(v301->r_bushandle + 0x2080);
			if ( v301->r_bustag )
				goto LABEL_262;
			__outdword((unsigned __int16)v293, v294);
		LABEL_263:
			v302 = ctlr_->r_mem;
			v303 = (unsigned int *)(v302->r_bushandle + 0x2080);
			if ( v302->r_bustag )
			{
				v304 = *v303 & 0xFFFC0FFF | 0x3000;
			LABEL_270:
				*v303 = v304;
				goto LABEL_271;
			}
			v310 = __indword((unsigned __int16)v303);
			v311 = ctlr_->r_mem;
			v304 = v310 & 0xFFFC0FFF | 0x3000;
			v303 = (unsigned int *)(v311->r_bushandle + 0x2080);
			if ( v311->r_bustag )
				goto LABEL_270;
			__outdword((unsigned __int16)v303, v304);
		LABEL_271:
			v312 = ctlr_->r_mem;
			v313 = (unsigned int *)(v312->r_bushandle + 0x205C);
			if ( v312->r_bustag )
			{
				v314 = *v313 & 0x3FFFFFFF | 0x40000000;
			LABEL_278:
				*v313 = v314;
				goto LABEL_279;
			}
			v320 = __indword((unsigned __int16)v313);
			v321 = ctlr_->r_mem;
			v314 = v320 & 0x3FFFFFFF | 0x40000000;
			v313 = (unsigned int *)(v321->r_bushandle + 0x205C);
			if ( v321->r_bustag )
				goto LABEL_278;
			__outdword((unsigned __int16)v313, v314);
		LABEL_279:
			v322 = ctlr_->r_mem;
			v323 = (unsigned int *)(v322->r_bushandle + 0x204C);
			if ( v322->r_bustag )
			{
				v324 = *v323 & 0xFFFFFFF0 | 3;
			LABEL_286:
				*v323 = v324;
				goto LABEL_287;
			}
			v330 = __indword((unsigned __int16)v323);
			v331 = ctlr_->r_mem;
			v324 = v330 & 0xFFFFFFF0 | 3;
			v323 = (unsigned int *)(v331->r_bushandle + 0x204C);
			if ( v331->r_bustag )
				goto LABEL_286;
			__outdword((unsigned __int16)v323, v324);
		LABEL_287:
			v332 = ctlr_->r_mem;
			v333 = (unsigned int *)(v332->r_bushandle + 0x206C);
			if ( v332->r_bustag )
			{
				v334 = *v333 & 0xFFFFF0FF | 0x100;
			LABEL_294:
				*v333 = v334;
				goto LABEL_295;
			}
			v340 = __indword((unsigned __int16)v333);
			v341 = ctlr_->r_mem;
			v334 = v340 & 0xFFFFF0FF | 0x100;
			v333 = (unsigned int *)(v341->r_bushandle + 0x206C);
			if ( v341->r_bustag )
				goto LABEL_294;
			__outdword((unsigned __int16)v333, v334);
		LABEL_295:
			v342 = ctlr_->r_mem;
			v117 = (unsigned int *)(v342->r_bushandle + 0x2084);
			if ( v342->r_bustag )
			{
				v120 = *v117 & 0xFFFFFF00 | 0x32;
				goto LABEL_153;
			}
			v348 = __indword((unsigned __int16)v117);
			v349 = ctlr_->r_mem;
			v120 = v348 & 0xFFFFFF00 | 0x32;
			v117 = (unsigned int *)(v349->r_bushandle + 0x2084);
			if ( v349->r_bustag )
				goto LABEL_153;
			__outdword((unsigned __int16)v117, v120);
			goto LABEL_154;
		case 3u:
		case 4u:
		case 5u:
			v124 = ctlr_->r_mem;
			v125 = (unsigned int *)(v124->r_bushandle + 0x204C);
			if ( v124->r_bustag )
			{
				v126 = *v125 & 0xFFC0FFFF | 0x1E0000;
			LABEL_234:
				*v125 = v126;
				goto LABEL_235;
			}
			v265 = __indword((unsigned __int16)v125);
			v266 = ctlr_->r_mem;
			v126 = v265 & 0xFFC0FFFF | 0x1E0000;
			v125 = (unsigned int *)(v266->r_bushandle + 0x204C);
			if ( v266->r_bustag )
				goto LABEL_234;
			__outdword((unsigned __int16)v125, v126);
		LABEL_235:
			v267 = ctlr_->r_mem;
			v268 = (unsigned int *)(v267->r_bushandle + 0x204C);
			if ( v267->r_bustag )
			{
				v269 = *v268 & 0xC0FFFFFF;
			LABEL_242:
				*v268 = v269;
				goto LABEL_243;
			}
			v275 = __indword((unsigned __int16)v268);
			v276 = ctlr_->r_mem;
			v269 = v275 & 0xC0FFFFFF;
			v268 = (unsigned int *)(v276->r_bushandle + 0x204C);
			if ( v276->r_bustag )
				goto LABEL_242;
			__outdword((unsigned __int16)v268, v269);
		LABEL_243:
			v277 = ctlr_->r_mem;
			v278 = (unsigned int *)(v277->r_bushandle + 0x2054);
			if ( v277->r_bustag )
			{
				v279 = *v278 & 0xFFFF9FFF | 0x2000;
			LABEL_250:
				*v278 = v279;
				goto LABEL_251;
			}
			v285 = __indword((unsigned __int16)v278);
			v286 = ctlr_->r_mem;
			v279 = v285 & 0xFFFF9FFF | 0x2000;
			v278 = (unsigned int *)(v286->r_bushandle + 0x2054);
			if ( v286->r_bustag )
				goto LABEL_250;
			__outdword((unsigned __int16)v278, v279);
		LABEL_251:
			v287 = ctlr_->r_mem;
			v288 = (unsigned int *)(v287->r_bushandle + 0x207C);
			if ( v287->r_bustag )
			{
				v289 = *v288 & 0xFFFFF03F | 0x840;
			LABEL_258:
				*v288 = v289;
				goto LABEL_259;
			}
			v295 = __indword((unsigned __int16)v288);
			v296 = ctlr_->r_mem;
			v289 = v295 & 0xFFFFF03F | 0x840;
			v288 = (unsigned int *)(v296->r_bushandle + 0x207C);
			if ( v296->r_bustag )
				goto LABEL_258;
			__outdword((unsigned __int16)v288, v289);
		LABEL_259:
			v297 = ctlr_->r_mem;
			v298 = (unsigned int *)(v297->r_bushandle + 0x207C);
			if ( v297->r_bustag )
			{
				v299 = *v298 & 0xFFFC0FFF | 0x2000;
			LABEL_266:
				*v298 = v299;
				goto LABEL_267;
			}
			v305 = __indword((unsigned __int16)v298);
			v306 = ctlr_->r_mem;
			v299 = v305 & 0xFFFC0FFF | 0x2000;
			v298 = (unsigned int *)(v306->r_bushandle + 0x207C);
			if ( v306->r_bustag )
				goto LABEL_266;
			__outdword((unsigned __int16)v298, v299);
		LABEL_267:
			v307 = ctlr_->r_mem;
			v308 = (unsigned int *)(v307->r_bushandle + 0x205C);
			if ( v307->r_bustag )
			{
				v309 = *v308 & 0xCFFFFFFF | 0x10000000;
			LABEL_274:
				*v308 = v309;
				goto LABEL_275;
			}
			v315 = __indword((unsigned __int16)v308);
			v316 = ctlr_->r_mem;
			v309 = v315 & 0xCFFFFFFF | 0x10000000;
			v308 = (unsigned int *)(v316->r_bushandle + 0x205C);
			if ( v316->r_bustag )
				goto LABEL_274;
			__outdword((unsigned __int16)v308, v309);
		LABEL_275:
			v317 = ctlr_->r_mem;
			v318 = (unsigned int *)(v317->r_bushandle + 0x2080);
			if ( v317->r_bustag )
			{
				v319 = *v318 & 0xFFFFF03F | 0x8C0;
			LABEL_282:
				*v318 = v319;
				goto LABEL_283;
			}
			v325 = __indword((unsigned __int16)v318);
			v326 = ctlr_->r_mem;
			v319 = v325 & 0xFFFFF03F | 0x8C0;
			v318 = (unsigned int *)(v326->r_bushandle + 0x2080);
			if ( v326->r_bustag )
				goto LABEL_282;
			__outdword((unsigned __int16)v318, v319);
		LABEL_283:
			v327 = ctlr_->r_mem;
			v328 = (unsigned int *)(v327->r_bushandle + 0x2080);
			if ( v327->r_bustag )
			{
				v329 = *v328 & 0xFFFC0FFF | 0x7000;
			LABEL_290:
				*v328 = v329;
				goto LABEL_291;
			}
			v335 = __indword((unsigned __int16)v328);
			v336 = ctlr_->r_mem;
			v329 = v335 & 0xFFFC0FFF | 0x7000;
			v328 = (unsigned int *)(v336->r_bushandle + 0x2080);
			if ( v336->r_bustag )
				goto LABEL_290;
			__outdword((unsigned __int16)v328, v329);
		LABEL_291:
			v337 = ctlr_->r_mem;
			v338 = (unsigned int *)(v337->r_bushandle + 0x205C);
			if ( v337->r_bustag )
			{
				v339 = *v338 & 0x3FFFFFFF | 0x40000000;
			LABEL_298:
				*v338 = v339;
				goto LABEL_299;
			}
			v343 = __indword((unsigned __int16)v338);
			v344 = ctlr_->r_mem;
			v339 = v343 & 0x3FFFFFFF | 0x40000000;
			v338 = (unsigned int *)(v344->r_bushandle + 0x205C);
			if ( v344->r_bustag )
				goto LABEL_298;
			__outdword((unsigned __int16)v338, v339);
		LABEL_299:
			v345 = ctlr_->r_mem;
			v346 = (unsigned int *)(v345->r_bushandle + 8268);
			if ( v345->r_bustag )
			{
				v347 = *v346 & 0xFFFFFFF0 | 3;
			LABEL_304:
				*v346 = v347;
				goto LABEL_305;
			}
			v350 = __indword((unsigned __int16)v346);
			v351 = ctlr_->r_mem;
			v347 = v350 & 0xFFFFFFF0 | 3;
			v346 = (unsigned int *)(v351->r_bushandle + 8268);
			if ( v351->r_bustag )
				goto LABEL_304;
			__outdword((unsigned __int16)v346, v347);
		LABEL_305:
			v352 = ctlr_->r_mem;
			v353 = (unsigned int *)(v352->r_bushandle + 8300);
			if ( v352->r_bustag )
			{
				v354 = *v353 & 0xFFFFF0FF | 0x100;
			LABEL_308:
				*v353 = v354;
				goto LABEL_309;
			}
			v355 = __indword((unsigned __int16)v353);
			v356 = ctlr_->r_mem;
			v354 = v355 & 0xFFFFF0FF | 0x100;
			v353 = (unsigned int *)(v356->r_bushandle + 8300);
			if ( v356->r_bustag )
				goto LABEL_308;
			__outdword((unsigned __int16)v353, v354);
		LABEL_309:
			v357 = ctlr_->r_mem;
			v117 = (unsigned int *)(v357->r_bushandle + 8324);
			if ( v357->r_bustag )
			{
				v120 = *v117 & 0xFFFFFF00 | 0x43;
				goto LABEL_153;
			}
			v358 = __indword((unsigned __int16)v117);
			v359 = ctlr_->r_mem;
			v120 = v358 & 0xFFFFFF00 | 0x43;
			v117 = (unsigned int *)(v359->r_bushandle + 8324);
			if ( v359->r_bustag )
				goto LABEL_153;
			__outdword((unsigned __int16)v117, v120);
			goto LABEL_154;
		case 6u:
		case 7u:
		case 8u:
			goto LABEL_45;
		case 9u:
		case 0xAu:
		case 0xBu:
		case 0xCu:
			v58 = ctlr_->r_mem;
			v59 = (unsigned int *)(v58->r_bushandle + 8268);
			if ( v58->r_bustag )
			{
				v60 = *v59 & 0xFFC0FFFF | 0x240000;
			LABEL_110:
				*v59 = v60;
				goto LABEL_111;
			}
			v127 = __indword((unsigned __int16)v59);
			v128 = ctlr_->r_mem;
			v60 = v127 & 0xFFC0FFFF | 0x240000;
			v59 = (unsigned int *)(v128->r_bushandle + 8268);
			if ( v128->r_bustag )
				goto LABEL_110;
			__outdword((unsigned __int16)v59, v60);
		LABEL_111:
			v129 = ctlr_->r_mem;
			v130 = (unsigned int *)(v129->r_bushandle + 8268);
			if ( v129->r_bustag )
			{
				v131 = *v130 & 0xC0FFFFFF | 0x4000000;
			LABEL_114:
				*v130 = v131;
				goto LABEL_115;
			}
			v132 = __indword((unsigned __int16)v130);
			v133 = ctlr_->r_mem;
			v131 = v132 & 0xC0FFFFFF | 0x4000000;
			v130 = (unsigned int *)(v133->r_bushandle + 8268);
			if ( v133->r_bustag )
				goto LABEL_114;
			__outdword((unsigned __int16)v130, v131);
		LABEL_115:
			v134 = ctlr_->r_mem;
			v135 = (unsigned int *)(v134->r_bushandle + 8276);
			if ( v134->r_bustag )
			{
				v136 = *v135 & 0xFFFF9FFF | 0x2000;
			LABEL_118:
				*v135 = v136;
				goto LABEL_119;
			}
			v137 = __indword((unsigned __int16)v135);
			v138 = ctlr_->r_mem;
			v136 = v137 & 0xFFFF9FFF | 0x2000;
			v135 = (unsigned int *)(v138->r_bushandle + 8276);
			if ( v138->r_bustag )
				goto LABEL_118;
			__outdword((unsigned __int16)v135, v136);
		LABEL_119:
			v139 = ctlr_->r_mem;
			v140 = (unsigned int *)(v139->r_bushandle + 8316);
			if ( v139->r_bustag )
			{
				v141 = *v140 & 0xFFFFF03F | 0x880;
			LABEL_122:
				*v140 = v141;
				goto LABEL_123;
			}
			v142 = __indword((unsigned __int16)v140);
			v143 = ctlr_->r_mem;
			v141 = v142 & 0xFFFFF03F | 0x880;
			v140 = (unsigned int *)(v143->r_bushandle + 8316);
			if ( v143->r_bustag )
				goto LABEL_122;
			__outdword((unsigned __int16)v140, v141);
		LABEL_123:
			v144 = ctlr_->r_mem;
			v145 = (unsigned int *)(v144->r_bushandle + 8316);
			if ( v144->r_bustag )
			{
				v146 = *v145 & 0xFFFC0FFF | 0x6000;
			LABEL_126:
				*v145 = v146;
				goto LABEL_127;
			}
			v147 = __indword((unsigned __int16)v145);
			v148 = ctlr_->r_mem;
			v146 = v147 & 0xFFFC0FFF | 0x6000;
			v145 = (unsigned int *)(v148->r_bushandle + 8316);
			if ( v148->r_bustag )
				goto LABEL_126;
			__outdword((unsigned __int16)v145, v146);
		LABEL_127:
			v149 = ctlr_->r_mem;
			v150 = (unsigned int *)(v149->r_bushandle + 8284);
			if ( v149->r_bustag )
			{
				v151 = *v150 & 0xCFFFFFFF | 0x10000000;
			LABEL_130:
				*v150 = v151;
				goto LABEL_131;
			}
			v152 = __indword((unsigned __int16)v150);
			v153 = ctlr_->r_mem;
			v151 = v152 & 0xCFFFFFFF | 0x10000000;
			v150 = (unsigned int *)(v153->r_bushandle + 8284);
			if ( v153->r_bustag )
				goto LABEL_130;
			__outdword((unsigned __int16)v150, v151);
		LABEL_131:
			v154 = ctlr_->r_mem;
			v155 = (unsigned int *)(v154->r_bushandle + 8320);
			if ( v154->r_bustag )
			{
				v156 = *v155 & 0xFFFFF03F | 0x900;
			LABEL_134:
				*v155 = v156;
				goto LABEL_135;
			}
			v157 = __indword((unsigned __int16)v155);
			v158 = ctlr_->r_mem;
			v156 = v157 & 0xFFFFF03F | 0x900;
			v155 = (unsigned int *)(v158->r_bushandle + 8320);
			if ( v158->r_bustag )
				goto LABEL_134;
			__outdword((unsigned __int16)v155, v156);
		LABEL_135:
			v159 = ctlr_->r_mem;
			v160 = (unsigned int *)(v159->r_bushandle + 8320);
			if ( v159->r_bustag )
			{
				v161 = *v160 & 0xFFFC0FFF | 0xF000;
			LABEL_138:
				*v160 = v161;
				goto LABEL_139;
			}
			v162 = __indword((unsigned __int16)v160);
			v163 = ctlr_->r_mem;
			v161 = v162 & 0xFFFC0FFF | 0xF000;
			v160 = (unsigned int *)(v163->r_bushandle + 8320);
			if ( v163->r_bustag )
				goto LABEL_138;
			__outdword((unsigned __int16)v160, v161);
		LABEL_139:
			v164 = ctlr_->r_mem;
			v165 = (unsigned int *)(v164->r_bushandle + 8284);
			if ( v164->r_bustag )
			{
				v166 = *v165 & 0x3FFFFFFF | 0x40000000;
			LABEL_142:
				*v165 = v166;
				goto LABEL_143;
			}
			v167 = __indword((unsigned __int16)v165);
			v168 = ctlr_->r_mem;
			v166 = v167 & 0x3FFFFFFF | 0x40000000;
			v165 = (unsigned int *)(v168->r_bushandle + 8284);
			if ( v168->r_bustag )
				goto LABEL_142;
			__outdword((unsigned __int16)v165, v166);
		LABEL_143:
			v169 = ctlr_->r_mem;
			v170 = (unsigned int *)(v169->r_bushandle + 8268);
			if ( v169->r_bustag )
			{
				v171 = *v170 & 0xFFFFFFF0 | 5;
			LABEL_146:
				*v170 = v171;
				goto LABEL_147;
			}
			v172 = __indword((unsigned __int16)v170);
			v173 = ctlr_->r_mem;
			v171 = v172 & 0xFFFFFFF0 | 5;
			v170 = (unsigned int *)(v173->r_bushandle + 8268);
			if ( v173->r_bustag )
				goto LABEL_146;
			__outdword((unsigned __int16)v170, v171);
		LABEL_147:
			v174 = ctlr_->r_mem;
			v175 = (unsigned int *)(v174->r_bushandle + 8300);
			if ( v174->r_bustag )
			{
				v176 = *v175 & 0xFFFFF0FF | 0x200;
			LABEL_150:
				*v175 = v176;
				goto LABEL_151;
			}
			v177 = __indword((unsigned __int16)v175);
			v178 = ctlr_->r_mem;
			v176 = v177 & 0xFFFFF0FF | 0x200;
			v175 = (unsigned int *)(v178->r_bushandle + 8300);
			if ( v178->r_bustag )
				goto LABEL_150;
			__outdword((unsigned __int16)v175, v176);
		LABEL_151:
			v179 = ctlr_->r_mem;
			v117 = (unsigned int *)(v179->r_bushandle + 8324);
			if ( v179->r_bustag )
				goto LABEL_152;
			v258 = __indword((unsigned __int16)v117);
			v259 = ctlr_->r_mem;
			v120 = v258 & 0xFFFFFF00 | 0x55;
			v117 = (unsigned int *)(v259->r_bushandle + 8324);
			if ( v259->r_bustag )
				goto LABEL_153;
			__outdword((unsigned __int16)v117, v120);
			goto LABEL_154;
		default:
			v360 = ctlr_->r_mem;
			v361 = (unsigned int *)(v360->r_bushandle + 8268);
			if ( v360->r_bustag )
			{
				v362 = *v361 & 0xFFC0FFFF | 0x260000;
			LABEL_347:
				*v361 = v362;
				goto LABEL_348;
			}
			v363 = __indword((unsigned __int16)v361);
			v364 = ctlr_->r_mem;
			v362 = v363 & 0xFFC0FFFF | 0x260000;
			v361 = (unsigned int *)(v364->r_bushandle + 8268);
			if ( v364->r_bustag )
				goto LABEL_347;
			__outdword((unsigned __int16)v361, v362);
		LABEL_348:
			v365 = ctlr_->r_mem;
			v366 = (unsigned int *)(v365->r_bushandle + 8268);
			if ( v365->r_bustag )
			{
				v367 = *v366 & 0xC0FFFFFF | 0x7000000;
			LABEL_351:
				*v366 = v367;
				goto LABEL_352;
			}
			v368 = __indword((unsigned __int16)v366);
			v369 = ctlr_->r_mem;
			v367 = v368 & 0xC0FFFFFF | 0x7000000;
			v366 = (unsigned int *)(v369->r_bushandle + 8268);
			if ( v369->r_bustag )
				goto LABEL_351;
			__outdword((unsigned __int16)v366, v367);
		LABEL_352:
			v370 = ctlr_->r_mem;
			v371 = (unsigned int *)(v370->r_bushandle + 8276);
			if ( v370->r_bustag )
			{
				v372 = *v371 & 0xFFFF9FFF | 0x2000;
			LABEL_355:
				*v371 = v372;
				goto LABEL_356;
			}
			v373 = __indword((unsigned __int16)v371);
			v374 = ctlr_->r_mem;
			v372 = v373 & 0xFFFF9FFF | 0x2000;
			v371 = (unsigned int *)(v374->r_bushandle + 8276);
			if ( v374->r_bustag )
				goto LABEL_355;
			__outdword((unsigned __int16)v371, v372);
		LABEL_356:
			v375 = ctlr_->r_mem;
			v376 = (unsigned int *)(v375->r_bushandle + 8316);
			if ( v375->r_bustag )
			{
				v377 = *v376 & 0xFFFFF03F | 0x880;
			LABEL_359:
				*v376 = v377;
				goto LABEL_360;
			}
			v378 = __indword((unsigned __int16)v376);
			v379 = ctlr_->r_mem;
			v377 = v378 & 0xFFFFF03F | 0x880;
			v376 = (unsigned int *)(v379->r_bushandle + 8316);
			if ( v379->r_bustag )
				goto LABEL_359;
			__outdword((unsigned __int16)v376, v377);
		LABEL_360:
			v380 = ctlr_->r_mem;
			v381 = (unsigned int *)(v380->r_bushandle + 8316);
			if ( v380->r_bustag )
			{
				v382 = *v381 & 0xFFFC0FFF | 0x6000;
			LABEL_363:
				*v381 = v382;
				goto LABEL_364;
			}
			v383 = __indword((unsigned __int16)v381);
			v384 = ctlr_->r_mem;
			v382 = v383 & 0xFFFC0FFF | 0x6000;
			v381 = (unsigned int *)(v384->r_bushandle + 8316);
			if ( v384->r_bustag )
				goto LABEL_363;
			__outdword((unsigned __int16)v381, v382);
		LABEL_364:
			v385 = ctlr_->r_mem;
			v386 = (unsigned int *)(v385->r_bushandle + 8284);
			if ( v385->r_bustag )
			{
				v387 = *v386 & 0xCFFFFFFF | 0x10000000;
			LABEL_367:
				*v386 = v387;
				goto LABEL_368;
			}
			v388 = __indword((unsigned __int16)v386);
			v389 = ctlr_->r_mem;
			v387 = v388 & 0xCFFFFFFF | 0x10000000;
			v386 = (unsigned int *)(v389->r_bushandle + 8284);
			if ( v389->r_bustag )
				goto LABEL_367;
			__outdword((unsigned __int16)v386, v387);
		LABEL_368:
			v390 = ctlr_->r_mem;
			v391 = (unsigned int *)(v390->r_bushandle + 8320);
			if ( v390->r_bustag )
			{
				v392 = *v391 & 0xFFFFF03F | 0x900;
			LABEL_371:
				*v391 = v392;
				goto LABEL_372;
			}
			v393 = __indword((unsigned __int16)v391);
			v394 = ctlr_->r_mem;
			v392 = v393 & 0xFFFFF03F | 0x900;
			v391 = (unsigned int *)(v394->r_bushandle + 8320);
			if ( v394->r_bustag )
				goto LABEL_371;
			__outdword((unsigned __int16)v391, v392);
		LABEL_372:
			v395 = ctlr_->r_mem;
			v396 = (unsigned int *)(v395->r_bushandle + 8320);
			if ( v395->r_bustag )
			{
				v397 = *v396 & 0xFFFC0FFF | 0xF000;
			LABEL_375:
				*v396 = v397;
				goto LABEL_376;
			}
			v398 = __indword((unsigned __int16)v396);
			v399 = ctlr_->r_mem;
			v397 = v398 & 0xFFFC0FFF | 0xF000;
			v396 = (unsigned int *)(v399->r_bushandle + 8320);
			if ( v399->r_bustag )
				goto LABEL_375;
			__outdword((unsigned __int16)v396, v397);
		LABEL_376:
			v400 = ctlr_->r_mem;
			v401 = (unsigned int *)(v400->r_bushandle + 8284);
			if ( v400->r_bustag )
			{
				v402 = *v401 & 0x3FFFFFFF | 0x40000000;
			LABEL_379:
				*v401 = v402;
				goto LABEL_380;
			}
			v403 = __indword((unsigned __int16)v401);
			v404 = ctlr_->r_mem;
			v402 = v403 & 0x3FFFFFFF | 0x40000000;
			v401 = (unsigned int *)(v404->r_bushandle + 8284);
			if ( v404->r_bustag )
				goto LABEL_379;
			__outdword((unsigned __int16)v401, v402);
		LABEL_380:
			v405 = ctlr_->r_mem;
			v406 = (unsigned int *)(v405->r_bushandle + 8268);
			if ( v405->r_bustag )
			{
				v407 = *v406 & 0xFFFFFFF0 | 5;
			LABEL_383:
				*v406 = v407;
				goto LABEL_384;
			}
			v408 = __indword((unsigned __int16)v406);
			v409 = ctlr_->r_mem;
			v407 = v408 & 0xFFFFFFF0 | 5;
			v406 = (unsigned int *)(v409->r_bushandle + 8268);
			if ( v409->r_bustag )
				goto LABEL_383;
			__outdword((unsigned __int16)v406, v407);
		LABEL_384:
			v410 = ctlr_->r_mem;
			v411 = (unsigned int *)(v410->r_bushandle + 8300);
			if ( v410->r_bustag )
			{
				v412 = *v411 & 0xFFFFF0FF | 0x200;
			LABEL_387:
				*v411 = v412;
				goto LABEL_388;
			}
			v413 = __indword((unsigned __int16)v411);
			v414 = ctlr_->r_mem;
			v412 = v413 & 0xFFFFF0FF | 0x200;
			v411 = (unsigned int *)(v414->r_bushandle + 8300);
			if ( v414->r_bustag )
				goto LABEL_387;
			__outdword((unsigned __int16)v411, v412);
		LABEL_388:
			v415 = ctlr_->r_mem;
			v117 = (unsigned int *)(v415->r_bushandle + 8324);
			if ( v415->r_bustag )
				goto LABEL_152;
			v416 = __indword((unsigned __int16)v117);
			v417 = ctlr_->r_mem;
			v120 = v416 & 0xFFFFFF00 | 0x55;
			v117 = (unsigned int *)(v417->r_bushandle + 8324);
			if ( v417->r_bustag )
				goto LABEL_153;
			__outdword((unsigned __int16)v117, v120);
			break;
		}
		goto LABEL_154;
	}
	dev_info(dev, "Baikal SATA PHY Trace length : %d\n", 6LL);
LABEL_45:
	v61 = ctlr_->r_mem;
	v62 = (unsigned int *)(v61->r_bushandle + 8268);
	if ( v61->r_bustag )
	{
		v63 = *v62 & 0xFFC0FFFF | 0x200000;
	LABEL_48:
		*v62 = v63;
		goto LABEL_49;
	}
	v64 = __indword((unsigned __int16)v62);
	v65 = ctlr_->r_mem;
	v63 = v64 & 0xFFC0FFFF | 0x200000;
	v62 = (unsigned int *)(v65->r_bushandle + 8268);
	if ( v65->r_bustag )
		goto LABEL_48;
	__outdword((unsigned __int16)v62, v63);
LABEL_49:
	v66 = ctlr_->r_mem;
	v67 = (unsigned int *)(v66->r_bushandle + 8268);
	if ( v66->r_bustag )
	{
		v68 = *v67 & 0xC0FFFFFF | 0x1000000;
	LABEL_52:
		*v67 = v68;
		goto LABEL_53;
	}
	v69 = __indword((unsigned __int16)v67);
	v70 = ctlr_->r_mem;
	v68 = v69 & 0xC0FFFFFF | 0x1000000;
	v67 = (unsigned int *)(v70->r_bushandle + 8268);
	if ( v70->r_bustag )
		goto LABEL_52;
	__outdword((unsigned __int16)v67, v68);
LABEL_53:
	v71 = ctlr_->r_mem;
	v72 = (unsigned int *)(v71->r_bushandle + 8276);
	if ( v71->r_bustag )
	{
		v73 = *v72 & 0xFFFF9FFF | 0x2000;
	LABEL_56:
		*v72 = v73;
		goto LABEL_57;
	}
	v74 = __indword((unsigned __int16)v72);
	v75 = ctlr_->r_mem;
	v73 = v74 & 0xFFFF9FFF | 0x2000;
	v72 = (unsigned int *)(v75->r_bushandle + 8276);
	if ( v75->r_bustag )
		goto LABEL_56;
	__outdword((unsigned __int16)v72, v73);
LABEL_57:
	v76 = ctlr_->r_mem;
	v77 = (unsigned int *)(v76->r_bushandle + 8316);
	if ( v76->r_bustag )
	{
		v78 = *v77 & 0xFFFFF03F | 0x880;
	LABEL_60:
		*v77 = v78;
		goto LABEL_61;
	}
	v79 = __indword((unsigned __int16)v77);
	v80 = ctlr_->r_mem;
	v78 = v79 & 0xFFFFF03F | 0x880;
	v77 = (unsigned int *)(v80->r_bushandle + 8316);
	if ( v80->r_bustag )
		goto LABEL_60;
	__outdword((unsigned __int16)v77, v78);
LABEL_61:
	v81 = ctlr_->r_mem;
	v82 = (unsigned int *)(v81->r_bushandle + 8316);
	if ( v81->r_bustag )
	{
		v83 = *v82 & 0xFFFC0FFF | 0x6000;
	LABEL_64:
		*v82 = v83;
		goto LABEL_65;
	}
	v84 = __indword((unsigned __int16)v82);
	v85 = ctlr_->r_mem;
	v83 = v84 & 0xFFFC0FFF | 0x6000;
	v82 = (unsigned int *)(v85->r_bushandle + 8316);
	if ( v85->r_bustag )
		goto LABEL_64;
	__outdword((unsigned __int16)v82, v83);
LABEL_65:
	v86 = ctlr_->r_mem;
	v87 = (unsigned int *)(v86->r_bushandle + 8284);
	if ( v86->r_bustag )
	{
		v88 = *v87 & 0xCFFFFFFF | 0x10000000;
	LABEL_68:
		*v87 = v88;
		goto LABEL_69;
	}
	v89 = __indword((unsigned __int16)v87);
	v90 = ctlr_->r_mem;
	v88 = v89 & 0xCFFFFFFF | 0x10000000;
	v87 = (unsigned int *)(v90->r_bushandle + 8284);
	if ( v90->r_bustag )
		goto LABEL_68;
	__outdword((unsigned __int16)v87, v88);
LABEL_69:
	v91 = ctlr_->r_mem;
	v92 = (unsigned int *)(v91->r_bushandle + 8320);
	if ( v91->r_bustag )
	{
		v93 = *v92 & 0xFFFFF03F | 0x900;
	LABEL_72:
		*v92 = v93;
		goto LABEL_73;
	}
	v94 = __indword((unsigned __int16)v92);
	v95 = ctlr_->r_mem;
	v93 = v94 & 0xFFFFF03F | 0x900;
	v92 = (unsigned int *)(v95->r_bushandle + 8320);
	if ( v95->r_bustag )
		goto LABEL_72;
	__outdword((unsigned __int16)v92, v93);
LABEL_73:
	v96 = ctlr_->r_mem;
	v97 = (unsigned int *)(v96->r_bushandle + 8320);
	if ( v96->r_bustag )
	{
		v98 = *v97 & 0xFFFC0FFF | 0xF000;
	LABEL_76:
		*v97 = v98;
		goto LABEL_77;
	}
	v99 = __indword((unsigned __int16)v97);
	v100 = ctlr_->r_mem;
	v98 = v99 & 0xFFFC0FFF | 0xF000;
	v97 = (unsigned int *)(v100->r_bushandle + 8320);
	if ( v100->r_bustag )
		goto LABEL_76;
	__outdword((unsigned __int16)v97, v98);
LABEL_77:
	v101 = ctlr_->r_mem;
	v102 = (unsigned int *)(v101->r_bushandle + 8284);
	if ( v101->r_bustag )
	{
		v103 = *v102 & 0x3FFFFFFF | 0x40000000;
	LABEL_80:
		*v102 = v103;
		goto LABEL_81;
	}
	v104 = __indword((unsigned __int16)v102);
	v105 = ctlr_->r_mem;
	v103 = v104 & 0x3FFFFFFF | 0x40000000;
	v102 = (unsigned int *)(v105->r_bushandle + 8284);
	if ( v105->r_bustag )
		goto LABEL_80;
	__outdword((unsigned __int16)v102, v103);
LABEL_81:
	v106 = ctlr_->r_mem;
	v107 = (unsigned int *)(v106->r_bushandle + 8268);
	if ( v106->r_bustag )
	{
		v108 = *v107 & 0xFFFFFFF0 | 5;
	LABEL_84:
		*v107 = v108;
		goto LABEL_85;
	}
	v109 = __indword((unsigned __int16)v107);
	v110 = ctlr_->r_mem;
	v108 = v109 & 0xFFFFFFF0 | 5;
	v107 = (unsigned int *)(v110->r_bushandle + 8268);
	if ( v110->r_bustag )
		goto LABEL_84;
	__outdword((unsigned __int16)v107, v108);
LABEL_85:
	v111 = ctlr_->r_mem;
	v112 = (unsigned int *)(v111->r_bushandle + 8300);
	if ( v111->r_bustag )
	{
		v113 = *v112 & 0xFFFFF0FF | 0x200;
	}
	else
	{
		v114 = __indword((unsigned __int16)v112);
		v115 = ctlr_->r_mem;
		v113 = v114 & 0xFFFFF0FF | 0x200;
		v112 = (unsigned int *)(v115->r_bushandle + 8300);
		if ( !v115->r_bustag )
		{
			__outdword((unsigned __int16)v112, v113);
			goto LABEL_89;
		}
	}
	*v112 = v113;
LABEL_89:
	v116 = ctlr_->r_mem;
	v117 = (unsigned int *)(v116->r_bushandle + 8324);
	if ( v116->r_bustag )
	{
	LABEL_152:
		v120 = *v117 & 0xFFFFFF00 | 0x55;
	}
	else
	{
		v118 = __indword((unsigned __int16)v117);
		v119 = ctlr_->r_mem;
		v120 = v118 & 0xFFFFFF00 | 0x55;
		v117 = (unsigned int *)(v119->r_bushandle + 8324);
		if ( !v119->r_bustag )
		{
			__outdword((unsigned __int16)v117, v120);
			goto LABEL_154;
		}
	}
LABEL_153:
	*v117 = v120;
LABEL_154:
	v180 = ctlr_->r_mem;
	v181 = v180->r_bushandle;
	v182 = (unsigned int *)(v181 + 8256);
	if ( v180->r_bustag )
	{
		v183 = *(_DWORD *)(v181 + 8256) & 0xFFFFFFE0 | 0x12;
	LABEL_157:
		*v182 = v183;
		goto LABEL_158;
	}
	v184 = __indword((unsigned __int16)v182);
	v185 = ctlr_->r_mem;
	v183 = v184 & 0xFFFFFFE0 | 0x12;
	v182 = (unsigned int *)(v185->r_bushandle + 8256);
	if ( v185->r_bustag )
		goto LABEL_157;
	__outdword((unsigned __int16)v182, v183);
LABEL_158:
	v186 = ctlr_->r_mem;
	v187 = v186->r_bushandle;
	v188 = (unsigned int *)(v187 + 8256);
	if ( v186->r_bustag )
	{
		v189 = *(_DWORD *)(v187 + 8256) & 0xFFFFC0FF | 0x3100;
	LABEL_161:
		*v188 = v189;
		goto LABEL_162;
	}
	v190 = __indword((unsigned __int16)v188);
	v191 = ctlr_->r_mem;
	v189 = v190 & 0xFFFFC0FF | 0x3100;
	v188 = (unsigned int *)(v191->r_bushandle + 8256);
	if ( v191->r_bustag )
		goto LABEL_161;
	__outdword((unsigned __int16)v188, v189);
LABEL_162:
	v192 = ctlr_->r_mem;
	v193 = v192->r_bushandle;
	v194 = (unsigned int *)(v193 + 8256);
	if ( v192->r_bustag )
	{
		v195 = *(_DWORD *)(v193 + 8256) & 0xFFE0FFFF | 0xE0000;
	LABEL_165:
		*v194 = v195;
		goto LABEL_166;
	}
	v196 = __indword((unsigned __int16)v194);
	v197 = ctlr_->r_mem;
	v195 = v196 & 0xFFE0FFFF | 0xE0000;
	v194 = (unsigned int *)(v197->r_bushandle + 8256);
	if ( v197->r_bustag )
		goto LABEL_165;
	__outdword((unsigned __int16)v194, v195);
LABEL_166:
	v198 = ctlr_->r_mem;
	v199 = v198->r_bushandle;
	v200 = (unsigned int *)(v199 + 8256);
	if ( v198->r_bustag )
	{
		v201 = *(_DWORD *)(v199 + 8256) & 0xFFFFFF1F | 0x80;
	LABEL_169:
		*v200 = v201;
		goto LABEL_170;
	}
	v202 = __indword((unsigned __int16)v200);
	v203 = ctlr_->r_mem;
	v201 = v202 & 0xFFFFFF1F | 0x80;
	v200 = (unsigned int *)(v203->r_bushandle + 8256);
	if ( v203->r_bustag )
		goto LABEL_169;
	__outdword((unsigned __int16)v200, v201);
LABEL_170:
	if ( get_subsys_id() != 0x30100 )
		goto LABEL_179;
	v204 = ctlr_->r_mem;
	v205 = v204->r_bushandle;
	v206 = (unsigned int *)(v205 + 8232);
	if ( v204->r_bustag )
	{
		v207 = *(_DWORD *)(v205 + 8232) & 0xFDFFFFFF;
	LABEL_174:
		*v206 = v207;
		goto LABEL_175;
	}
	v208 = __indword((unsigned __int16)v206);
	v209 = ctlr_->r_mem;
	v207 = v208 & 0xFDFFFFFF;
	v206 = (unsigned int *)(v209->r_bushandle + 8232);
	if ( v209->r_bustag )
		goto LABEL_174;
	__outdword((unsigned __int16)v206, v207);
LABEL_175:
	v210 = ctlr_->r_mem;
	v211 = v210->r_bushandle;
	v212 = (unsigned int *)(v211 + 8260);
	if ( v210->r_bustag )
	{
		v213 = *(_DWORD *)(v211 + 8260) & 0xFFFFFF80 | 0x1C;
	LABEL_178:
		*v212 = v213;
		goto LABEL_179;
	}
	v214 = __indword((unsigned __int16)v212);
	v215 = ctlr_->r_mem;
	v213 = v214 & 0xFFFFFF80 | 0x1C;
	v212 = (unsigned int *)(v215->r_bushandle + 8260);
	if ( v215->r_bustag )
		goto LABEL_178;
	__outdword((unsigned __int16)v212, v213);
LABEL_179:
	v216 = ctlr_->r_mem;
	v217 = v216->r_bushandle;
	v218 = (unsigned int *)(v217 + 8220);
	if ( v216->r_bustag )
	{
		v219 = *(_DWORD *)(v217 + 8220) & 0xFF0FFFFF | 0x200000;
	LABEL_182:
		*v218 = v219;
		goto LABEL_183;
	}
	v220 = __indword((unsigned __int16)v218);
	v221 = ctlr_->r_mem;
	v219 = v220 & 0xFF0FFFFF | 0x200000;
	v218 = (unsigned int *)(v221->r_bushandle + 8220);
	if ( v221->r_bustag )
		goto LABEL_182;
	__outdword((unsigned __int16)v218, v219);
LABEL_183:
	v222 = ctlr_->r_mem;
	v223 = v222->r_bushandle;
	v224 = (unsigned int *)(v223 + 8412);
	if ( v222->r_bustag )
	{
		v225 = *(_DWORD *)(v223 + 8412) & 0xFFFFE0FF | 0x400;
	LABEL_186:
		*v224 = v225;
		goto LABEL_187;
	}
	v226 = __indword((unsigned __int16)v224);
	v227 = ctlr_->r_mem;
	v225 = v226 & 0xFFFFE0FF | 0x400;
	v224 = (unsigned int *)(v227->r_bushandle + 8412);
	if ( v227->r_bustag )
		goto LABEL_186;
	__outdword((unsigned __int16)v224, v225);
LABEL_187:
	v228 = ctlr_->r_mem;
	v229 = v228->r_bushandle;
	v230 = (unsigned int *)(v229 + 8228);
	if ( v228->r_bustag )
	{
		v231 = *(_DWORD *)(v229 + 8228) | 0x30;
	}
	else
	{
		v232 = __indword((unsigned __int16)v230);
		v233 = ctlr_->r_mem;
		v231 = v232 | 0x30;
		v230 = (unsigned int *)(v233->r_bushandle + 8228);
		if ( !v233->r_bustag )
		{
			__outdword((unsigned __int16)v230, v231);
			if ( dev_id != -1864822707 )
				goto LABEL_191;
		LABEL_199:
			v234 = 44LL;
			goto LABEL_200;
		}
	}
	*v230 = v231;
	if ( dev_id == 0x90D9104D )
		goto LABEL_199;
LABEL_191:
	v234 = 48LL;
LABEL_200:
	bpcie_write_to_bar2_and_0x180000_and_offset(v234, 0);
	v235 = 0;
	do
	{
		v236 = ctlr_->r_mem;
		v237 = (_DWORD *)(v236->r_bushandle + 220);
		if ( v236->r_bustag )
		{
			if ( *v237 & 1 )
				break;
		}
		else
		{
			v238 = __indword((unsigned __int16)v237);
			if ( v238 & 1 )
				break;
		}
		delay(10);
		++v235;
	}
	while ( v235 < 0x64 );
	v239 = ctlr_->r_mem;
	v240 = (unsigned int *)v239->r_bushandle;
	if ( v239->r_bustag )
	{
		v241 = *v240 & 0xE7FFFFFF;
	LABEL_209:
		*v240 = v241;
		goto LABEL_210;
	}
	v242 = __indword((unsigned __int16)v240);
	v243 = ctlr_->r_mem;
	v241 = v242 & 0xE7FFFFFF;
	v240 = (unsigned int *)v243->r_bushandle;
	if ( v243->r_bustag )
		goto LABEL_209;
	__outdword((unsigned __int16)v240, v241);
LABEL_210:
	v244 = ctlr_->r_mem;
	v245 = (_DWORD *)(v244->r_bushandle + 12);
	if ( v244->r_bustag )
		*v245 = 1;
	else
		__outdword((unsigned __int16)v245, 1u);
	v246 = ctlr_->r_mem;
	v247 = v246->r_bushandle;
	v248 = (unsigned int *)(v247 + 184);
	if ( v246->r_bustag )
	{
		v249 = *(_DWORD *)(v247 + 184) & 0xFFFDFFFF;
	LABEL_216:
		*v248 = v249;
		goto LABEL_217;
	}
	v250 = __indword((unsigned __int16)v248);
	v251 = ctlr_->r_mem;
	v249 = v250 & 0xFFFDFFFF;
	v248 = (unsigned int *)(v251->r_bushandle + 184);
	if ( v251->r_bustag )
		goto LABEL_216;
	__outdword((unsigned __int16)v248, v249);
LABEL_217:
	v252 = ctlr_->r_mem;
	v253 = v252->r_bushandle;
	v254 = (unsigned int *)(v253 + 0x118);
	if ( v252->r_bustag )
	{
		v255 = *(_DWORD *)(v253 + 0x118) & 0xFFE3FFFF | 0x40000;
	LABEL_220:
		*v254 = v255;
		return;
	}
	v256 = __indword((unsigned __int16)v254);
	v257 = ctlr_->r_mem;
	v255 = v256 & 0xFFE3FFFF | 0x40000;
	v254 = (unsigned int *)(v257->r_bushandle + 0x118);
	if ( v257->r_bustag )
		goto LABEL_220;
	__outdword((unsigned __int16)v254, v255);
}
EXPORT_SYMBOL_GPL(baikal_pcie_sata_phy_init);

void belize_pcie_sata_phy_init(struct device *dev, struct ahci_controller *ctlr)
{
	int i;
	u32 v;
	u32 v2;
	bool is_phy_gen_3;
	unsigned int trace_length;

  	dev_info(dev, "Belize SATA PHY init\n");

	for (i = 0; i < 100; i++) {
		udelay(10000);
	}

 	//step 1
  	bpcie_ahci_write(ctlr->r_mem, 0x178, 0x81);

	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	bpcie_ahci_write(ctlr->r_mem, 0x17c, (v & 0xffffff00) | 1);

	bpcie_ahci_write(ctlr->r_mem, 0x178, 0xa5);

	//step 2
	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);

	if (ctlr->dev_id == 0x90ca104d || ctlr->dev_id == 0x909f104d || ctlr->dev_id == 0x90d9104d) {
		is_phy_gen_3 = false;
		bpcie_ahci_write(ctlr->r_mem, 0x17c, v & 0xfffff3ff);
	} else {
		u32 v2 = bpcie_ahci_read(ctlr->r_mem, 0);
	  	if ((v2 & 0xf00000) != 0x300000) {
	    		bpcie_ahci_write(ctlr->r_mem, 0x17c, (v & 0xfffff3ff) | 0x400);
	    		is_phy_gen_3 = false;
	  	} else {
	    		 dev_info(dev,"PHY SET GEN3\n");
	    		 bpcie_ahci_write(ctlr->r_mem, 0x17c, (v & 0xfffff3ff) | 0x800);
	    		 is_phy_gen_3 = true;
	  	}
	}

	//--- step 3 ---
	if (ctlr->trace_len == 0) {
	  	trace_length = 6;
	} else {
	  	trace_length = ctlr->trace_len & 0x1f;
	  	if (trace_length >= 0x13) {
	    		trace_length = 6;
	  	}
	}

	dev_info(dev,"Belize SATA PHY Trace length : %d\n",trace_length);

	bpcie_ahci_write(ctlr->r_mem, 0x178, 0xa3);

	//--- step 4 ---
	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	bpcie_ahci_write(ctlr->r_mem, 0x17c, (v & 0xfffff3ff) | 0x800);
	bpcie_ahci_write(ctlr->r_mem, 0x178, 0xd0);

	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	bpcie_ahci_write(ctlr->r_mem, 0x17c, (v & 0xfffff800) | 0x441);
	bpcie_ahci_write(ctlr->r_mem, 0x178, 0xe1);

	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	bpcie_ahci_write(ctlr->r_mem, 0x17c, (v & 0xffffff8f) | 0x60);
	bpcie_ahci_write(ctlr->r_mem, 0x178, 0xd0);

	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	bpcie_ahci_write(ctlr->r_mem, 0x17c, (v & 0xffff3fff) | 0x8000);
	bpcie_ahci_write(ctlr->r_mem, 0x178, 0xf1);

	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	bpcie_ahci_write(ctlr->r_mem, 0x17c, (v & 0xfffff3ff) | 0x400);
	bpcie_ahci_write(ctlr->r_mem, 0x178, 0x48);

	//--- step 5 ---
	bpcie_ahci_write(ctlr->r_mem, 0x17c, 0x62d8);
	bpcie_ahci_write(ctlr->r_mem, 0x178, 0xe);

	//--- step 6 ---
	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	bpcie_ahci_write(ctlr->r_mem, 0x17c, v & 0xffffdfff);
	bpcie_ahci_write(ctlr->r_mem, 0x178, 0x8d);

	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	bpcie_ahci_write(ctlr->r_mem, 0x17c, v | 1);
	bpcie_ahci_write(ctlr->r_mem, 0x178, 0x8f);

	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	bpcie_ahci_write(ctlr->r_mem, 0x17c, v | 1);
	bpcie_ahci_write(ctlr->r_mem, 0x178, 0x91);

	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	bpcie_ahci_write(ctlr->r_mem, 0x17c, v | 1);
	bpcie_ahci_write(ctlr->r_mem, 0x178, 0x8d);

	//--- step 7 ---
	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	if (trace_length >= 0xe) {
		bpcie_ahci_write(ctlr->r_mem, 0x17c, (v & 0xffffffc1) | 0x28);
	} else {
		if (trace_length < 0xb) {
	    		bpcie_ahci_write(ctlr->r_mem, 0x17c, (v & 0xffffffc1) | 8);
	  	} else {
	    		bpcie_ahci_write(ctlr->r_mem, 0x17c, (v & 0xffffffc1) | 0x10);
	  	}
	}

	bpcie_ahci_write(ctlr->r_mem, 0x178, 0x8f);
	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	bpcie_ahci_write(ctlr->r_mem, 0x17c, (v & 0xffffffc1) | 0x12);

	//--- step 8 ---
	bpcie_ahci_write(ctlr->r_mem, 0x178, 0x91);
	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	if (trace_length < 5) {
		bpcie_ahci_write(ctlr->r_mem, 0x17c, (v & 0xffffffc1) | 0x24);
	} else {
	  	bpcie_ahci_write(ctlr->r_mem, 0x17c, (v & 0xffffffc1) | 0x2a);
	}

	//--- step 9 ---
	bpcie_ahci_write(ctlr->r_mem, 0x178, 0xff);
	bpcie_ahci_write(ctlr->r_mem, 0x17c, 1);

	bpcie_ahci_write(ctlr->r_mem, 0x178, 0x97);

	//--- step 10 ---
	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	if (is_phy_gen_3) {
		bpcie_ahci_write(ctlr->r_mem, 0x17c, (v & 0xffffff80) | 0xd3);
	} else {
	  	bpcie_ahci_write(ctlr->r_mem, 0x17c, (v & 0xffffff80) | 0xee);
	}

	bpcie_ahci_write(ctlr->r_mem, 0x178, 0x95);
	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	bpcie_ahci_write(ctlr->r_mem, 0x17c, (v & 0xffffff00) | 0xee);

	//--- step 11 ---
	bpcie_ahci_write(ctlr->r_mem, 0x178, 0xff);
	bpcie_ahci_write(ctlr->r_mem, 0x17c, 0);

	bpcie_ahci_write(ctlr->r_mem, 0x178, 0x8d);

	//--- step 12 ---
	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	if (trace_length < 0xe) {
		bpcie_ahci_write(ctlr->r_mem, 0x17c, (v & 0xfffff87f) | 0x900);
	} else {
		bpcie_ahci_write(ctlr->r_mem, 0x17c, (v & 0xfffff87f) | 0xe00);
	}

	//--- step 13 ---
	bpcie_ahci_write(ctlr->r_mem, 0x178, 0x8f);
	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	bpcie_ahci_write(ctlr->r_mem, 0x17c, (v & 0xfffff07f) | 0x900);

	bpcie_ahci_write(ctlr->r_mem, 0x178, 0x91);

	//--- step 14 ---
	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	if (is_phy_gen_3) {
		bpcie_ahci_write(ctlr->r_mem, 0x17c, (v & 0xfffff87f) | 0xd00);
	} else {
		bpcie_ahci_write(ctlr->r_mem, 0x17c, (v & 0xfffff87f) | 0xe80);
	}

	//--- step 15 ---
	bpcie_ahci_write(ctlr->r_mem, 0x178, 200);
	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	bpcie_ahci_write(ctlr->r_mem, 0x17c, v & 0xffffefff);

	bpcie_ahci_write(ctlr->r_mem, 0x178, 10);
	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	bpcie_ahci_write(ctlr->r_mem, 0x17c, v & 0xffffefff);

	bpcie_ahci_write(ctlr->r_mem, 0x178, 0x82);
	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	bpcie_ahci_write(ctlr->r_mem, 0x17c, v | 0x1000);

	bpcie_ahci_write(ctlr->r_mem, 0x178, 0xc9);
	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	bpcie_ahci_write(ctlr->r_mem, 0x17c, v | 0x1000);

	bpcie_ahci_write(ctlr->r_mem, 0x178, 0x84);
	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	bpcie_ahci_write(ctlr->r_mem, 0x17c, (v & 0xffffffc0) | 0xc);

	bpcie_ahci_write(ctlr->r_mem, 0x178, 0x8d);

	//--- step 16 ---
	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	v = v & 0xffff0fff;
	if (trace_length < 0xe) {
		bpcie_ahci_write(ctlr->r_mem, 0x17c, v | 0x8000);
	} else {
		bpcie_ahci_write(ctlr->r_mem, 0x17c, v);
	}

	//--- step 17 ---
	bpcie_ahci_write(ctlr->r_mem, 0x178, 0x8f);
	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);
	bpcie_ahci_write(ctlr->r_mem, 0x17c, (v & 0xffff0fff) | 0x8000);

	//--- step 18 ---
	bpcie_ahci_write(ctlr->r_mem, 0xa0, 0);
	v = bpcie_ahci_read(ctlr->r_mem, 0xa4);
	bpcie_ahci_write(ctlr->r_mem, 0xa4, v | 0x40);

	//--- step 19 ---
	if (ctlr->dev_id != 0x909f104d && ctlr->dev_id != 0x90ca104d && ctlr->dev_id != 0x90d9104d) {
		v2 = 0x73000000;
	} else {
		v2 = 0x30000000;
	}

	//--- step 20 ---
	bpcie_ahci_write(ctlr->r_mem, 0xa0, 4);
	v = bpcie_ahci_read(ctlr->r_mem, 0xa4);
	bpcie_ahci_write(ctlr->r_mem, 0xa4, (v & 0x88ffffff) | v2);

	//--- step 21 ---
	v = bpcie_ahci_read(ctlr->r_mem, 0xa4);

	//--- step 22 ---
	for (i = 0; i < 100; i++) {
		udelay(10000);
	}

	//--- step 23 ---
	bpcie_ahci_write(ctlr->r_mem, 0x178, 0xf9);
	v = bpcie_ahci_read(ctlr->r_mem, 0x17c);

	if (is_phy_gen_3) {
		dev_info(dev,"Align 90=0x%02x\n",v & 0x7f);
	}
	//done!
}
EXPORT_SYMBOL_GPL(belize_pcie_sata_phy_init);
#endif

module_pci_driver(ahci_pci_driver);

MODULE_AUTHOR("Jeff Garzik");
MODULE_DESCRIPTION("AHCI SATA low-level driver");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(pci, ahci_pci_tbl);
MODULE_VERSION(DRV_VERSION);
