#ifndef _AEOLIA_H
#define _AEOLIA_H

#include <linux/io.h>
#include <linux/pci.h>
#include <linux/i2c.h>

enum aeolia_func_id {
	AEOLIA_FUNC_ID_ACPI = 0,
	AEOLIA_FUNC_ID_GBE,
	AEOLIA_FUNC_ID_AHCI,
	AEOLIA_FUNC_ID_SDHCI,
	AEOLIA_FUNC_ID_PCIE,
	AEOLIA_FUNC_ID_DMAC,
	AEOLIA_FUNC_ID_MEM,
	AEOLIA_FUNC_ID_XHCI,

	AEOLIA_NUM_FUNCS
};

/* Sub-functions, aka MSI vectors */
/* MSI registers for up to 31, but only 23 known. */
enum apcie_subfunc {
	APCIE_SUBFUNC_GLUE	= 0,
	APCIE_SUBFUNC_ICC	= 3,
	APCIE_SUBFUNC_HPET	= 5,
	APCIE_SUBFUNC_SFLASH	= 11,
	APCIE_SUBFUNC_RTC	= 13,
	APCIE_SUBFUNC_UART0	= 19,
	APCIE_SUBFUNC_UART1	= 20,
	APCIE_SUBFUNC_TWSI	= 21,

	APCIE_NUM_SUBFUNCS	= 23
};

enum bpcie_subfuncs_per_func {
	SUBFUNCS_PER_FUNC4 = 31,
	SUBFUNCS_PER_FUNC2 = 1,
	SUBFUNCS_PER_FUNC7 = 3,
	SUBFUNCS_PER_FUNC0 = 2,
	SUBFUNCS_PER_FUNC1 = 1,
	SUBFUNCS_PER_FUNC3 = 1,
	SUBFUNCS_PER_FUNC5 = 2,
	SUBFUNCS_PER_FUNC6 = 3,
};

enum bpcie_subfunc {
	BPCIE_SUBFUNC_GLUE	= 0, //confirmed
	BPCIE_SUBFUNC_ICC	= 3, //confirmed
	BPCIE_SUBFUNC_HPET	= 22, //Baikal Timer/WDT
	BPCIE_SUBFUNC_SFLASH	= 19, //confirmed
	BPCIE_SUBFUNC_RTC	= 21, //confirmed
	BPCIE_SUBFUNC_UART0	= 26, //confirmed
	BPCIE_SUBFUNC_UART1	= 27, //not confirmed
	//APCIE_SUBFUNC_TWSI	= 21,

	BPCIE_SUBFUNC_USB0	= 0, BPCIE_SUBFUNC_USB2 = 2, //confirmed
	BPCIE_SUBFUNC_ACPI= 1,
	BPCIE_SUBFUNC_SPM = 1, //confirmed (Scratch Pad Memory)
	BPCIE_SUBFUNC_DMAC1	= 0, //confirmed
	BPCIE_SUBFUNC_DMAC2	= 1, //confirmed
	BPCIE_NUM_SUBFUNCS	= 32
};

#define APCIE_NR_UARTS 2

/* Relative to BAR2 */
#define APCIE_RGN_CHIPID_BASE		(sc->is_baikal ?  0x4000 : \
						          0x1000)

#define APCIE_RGN_CHIPID_SIZE		(sc->is_baikal ?  0x9000 : \
						     	  0x1000)
#define APCIE_REG_CHIPID_0		0x1104
#define APCIE_REG_CHIPID_1		0x1108
#define APCIE_REG_CHIPREV		0x110c

#define BPCIE_HPET_BASE         0x109000
#define BPCIE_HPET_SIZE         0x400

#define BPCIE_ACK_WRITE 		0x110084
#define BPCIE_ACK_READ  		0x110088

#define APCIE_RGN_RTC_BASE		0x0
#define APCIE_RGN_RTC_SIZE		0x1000

/* Relative to BAR4 */
#define BPCIE_REG_CHIPID_0		0xC020
#define BPCIE_REG_CHIPID_1		0xC024
#define BPCIE_REG_CHIPREV		0x4084

/* Relative to BAR4 */
#define APCIE_RGN_UART_BASE		(sc->is_baikal ? 0x10E000 : 0x140000)
#define APCIE_RGN_UART_SIZE		0x1000

#define APCIE_RGN_PCIE_BASE		0x1c8000
#define APCIE_RGN_PCIE_SIZE		0x1000

#define APCIE_RGN_ICC_BASE		(sc->is_baikal ? (0x108000 - 0x800) : \
							 (0x184000))
#define APCIE_RGN_ICC_SIZE		0x1000

#define APCIE_REG_BAR(x)		(APCIE_RGN_PCIE_BASE + (x))
#define APCIE_REG_BAR_MASK(func, bar)	APCIE_REG_BAR(((func) * 0x30) + \
						((bar) << 3))
#define APCIE_REG_BAR_ADDR(func, bar)	APCIE_REG_BAR(((func) * 0x30) + \
						((bar) << 3) + 0x4)
#define APCIE_REG_MSI(x)		(APCIE_RGN_PCIE_BASE + 0x400 + (x))
#define APCIE_REG_MSI_CONTROL		APCIE_REG_MSI(0x0)
#define APCIE_REG_MSI_MASK(func)	APCIE_REG_MSI(0x4c + ((func) << 2))
#define APCIE_REG_MSI_DATA_HI(func)	APCIE_REG_MSI(0x8c + ((func) << 2))
#define APCIE_REG_MSI_ADDR(func)	APCIE_REG_MSI(0xac + ((func) << 2))
/* This register has non-uniform structure per function, dealt with in code */
#define APCIE_REG_MSI_DATA_LO(off)	APCIE_REG_MSI(0x100 + (off))

/* Not sure what the two individual bits do */
#define APCIE_REG_MSI_CONTROL_ENABLE	0x05

/* Enable for the entire function, 4 is special */
#define APCIE_REG_MSI_MASK_FUNC		0x01000000
#define APCIE_REG_MSI_MASK_FUNC4	0x80000000

#define APCIE_REG_ICC(x)		(APCIE_RGN_ICC_BASE + (x))
#define APCIE_REG_ICC_DOORBELL		APCIE_REG_ICC(0x804)
#define APCIE_REG_ICC_STATUS		APCIE_REG_ICC(0x814)
#define APCIE_REG_ICC_IRQ_MASK		APCIE_REG_ICC(0x824)

/* Apply to both DOORBELL and STATUS */
#define APCIE_ICC_SEND			0x01
#define APCIE_ICC_ACK			0x02

/*USB-related*/
#define BPCIE_USB_BASE			0x180000

/* Relative to func6 BAR5 */
#define APCIE_SPM_ICC_BASE		0x2c000
#define APCIE_SPM_ICC_SIZE		0x1000

/* Boot params passed from southbridge */
#define APCIE_SPM_BP_BASE		0x2f000
#define APCIE_SPM_BP_SIZE		0x20

#define APCIE_SPM_ICC_REQUEST		0x0
#define APCIE_SPM_ICC_REPLY		0x800

#define ICC_REPLY 0x4000
#define ICC_EVENT 0x8000

#define ICC_MAGIC 0x42
#define ICC_EVENT_MAGIC 0x24

struct icc_message_hdr {
	u8 magic;// not magic: it's ID of sender. 0x32=EAP,0x42=SoC(x86/fbsd)
 	u8 major;// service id (destination)
 	u16 minor;// message id (command)
	u16 unknown;
	u16 cookie; //normally monotonic xfer counter, can be set to special values
	u16 length;
	u16 checksum;
} __packed;

#define ICC_HDR_SIZE sizeof(struct icc_message_hdr)
#define ICC_MIN_SIZE 0x20
#define ICC_MAX_SIZE 0x7f0
#define ICC_MIN_PAYLOAD (ICC_MIN_SIZE - ICC_HDR_SIZE)
#define ICC_MAX_PAYLOAD (ICC_MAX_SIZE - ICC_HDR_SIZE)

struct apcie_icc_dev {
	phys_addr_t spm_base;
	void __iomem *spm;

	spinlock_t reply_lock;
	bool reply_pending;

	struct icc_message_hdr request;
	struct icc_message_hdr reply;
	u16 reply_extra_checksum;
	void *reply_buffer;
	int reply_length;
	wait_queue_head_t wq;

	struct i2c_adapter i2c;
	struct input_dev *pwrbutton_dev;
};

struct apcie_dev {
	struct pci_dev *pdev;
	struct irq_domain *irqdomain;

	// These are used to differentiate baikal and aeolia/belize
	bool is_baikal;
	int glue_bar_to_use_num;
	void __iomem *glue_bar_to_use;

	void __iomem *bar0;
	void __iomem *bar2;
	void __iomem *bar4;

    bool irq_chip_inited;
    int8_t irq_map[100];

	int nvec;
	int serial_line[2];

	struct apcie_icc_dev icc;
};

#define sc_err(...) dev_info(&sc->pdev->dev, __VA_ARGS__)
#define sc_warn(...) dev_info(&sc->pdev->dev, __VA_ARGS__)
#define sc_notice(...) dev_info(&sc->pdev->dev, __VA_ARGS__)
#define sc_info(...) dev_info(&sc->pdev->dev, __VA_ARGS__)
#define sc_dbg(...) dev_info(&sc->pdev->dev, __VA_ARGS__)

static inline int apcie_irqnum(struct apcie_dev *sc, int index)
{
	if (sc->nvec > 1) {
		return sc->pdev->irq + index;
	} else {
		return sc->pdev->irq;
	}
}

int apcie_icc_cmd(u8 major, u16 minor, const void *data, u16 length,
	    void *reply, u16 reply_length);

// Baikal specific section
#define CHAR_BIT 8	/* Normally in <limits.h> */
static inline u32 rol (u32 n, unsigned int c) {
	const unsigned int mask = (CHAR_BIT*sizeof(n) - 1);  // assumes width is a power of 2.

	c &= mask;
	return (n<<c) | (n>>( (-c)&mask ));
}

static inline u32 ror (u32 n, unsigned int c) {
	const unsigned int mask = (CHAR_BIT*sizeof(n) - 1);

	c &= mask;
	return (n>>c) | (n<<( (-c)&mask ));
}

static inline void cpu_stop(void)
{
    for (;;)
        asm volatile("cli; hlt;" : : : "memory");
}

static inline void stop_hpet_timers(struct apcie_dev *sc) {
	u64 NUM_TIM_CAP;
	u64 N;

	*(volatile u64 *)(sc->bar2 + BPCIE_HPET_BASE + 0x10) &= ~(1UL << 0);  //General Configuration Register
	NUM_TIM_CAP = *(volatile u64 *)(sc->bar2 + BPCIE_HPET_BASE) & 0x1F00;
	for (N = 0; N <= NUM_TIM_CAP; N++) {
		*(volatile u64 *)(sc->bar2 + BPCIE_HPET_BASE + (0x20*N) + 0x100) &= ~(1UL << 2); //Timer N Configuration and Capabilities Register
	}
	cpu_stop();
}

static inline int pci_pm_stop(struct pci_dev *dev)
{
	u16 csr;

	if (!dev->pm_cap)
		return -ENOTTY;

	pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &csr);
	//if (csr & PCI_PM_CTRL_NO_SOFT_RESET)
	//	return -ENOTTY;

	csr &= ~PCI_PM_CTRL_STATE_MASK;
	csr |= PCI_D3hot;
	pci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, csr);
	//pci_dev_d3_sleep(dev);

	return 0;
}

static inline void pci_pm_stop_all(struct pci_dev *dev)
{
	struct pci_dev *sc_dev;
	unsigned int func;
	for (func = 0; func < 8; ++func) {
		sc_dev = pci_get_slot(pci_find_bus(pci_domain_nr(dev->bus), 0), PCI_DEVFN(20, func));
		pci_pm_stop(sc_dev);
	}
	cpu_stop();
}

// Baikal specific section - End

#define BUF_FULL 0x7f0
#define BUF_EMPTY 0x7f4
#define HDR(x) (offsetof(struct icc_message_hdr, x))

#define ICC_MAJOR	'I'

struct icc_cmd {
	u8 major;
	u16 minor;
	void __user *data;
	u16 length;
	void __user *reply;
	u16 reply_length;
};

#define ICC_IOCTL_CMD _IOWR(ICC_MAJOR, 1, struct icc_cmd)

#endif
