#ifndef _AEOLIA_BAIKAL_H
#define _AEOLIA_BAIKAL_H

#include <linux/io.h>
#include <linux/pci.h>
#include <linux/i2c.h>

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

/* Seconds. Yes, some ICC requests can be slow. */
#define ICC_TIMEOUT 15;

#define sc_err(...) dev_err(&sc->pdev->dev, __VA_ARGS__)
#define sc_warn(...) dev_warn(&sc->pdev->dev, __VA_ARGS__)
#define sc_notice(...) dev_notice(&sc->pdev->dev, __VA_ARGS__)
#define sc_info(...) dev_info(&sc->pdev->dev, __VA_ARGS__)
#define sc_dbg(...) dev_dbg(&sc->pdev->dev, __VA_ARGS__)

struct abpcie_icc_dev {
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

struct abpcie_dev {
	struct pci_dev *pdev;
	struct irq_domain *irqdomain;
	void __iomem *bar0;
	void __iomem *bar2;
	void __iomem *bar4;

	int nvec;
	int serial_line[2];
	struct abpcie_icc_dev icc;
};

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
