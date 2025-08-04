#define DEBUG

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/msi.h>
#include <asm/apic.h>
#include <asm/irqdomain.h>
#include <asm/irq_remapping.h>

#include <asm/msi.h>

#include <asm/ps4.h>

#include "aeolia.h"

/* #define QEMU_HACK_NO_IOMMU */

void apcie_set_desc(msi_alloc_info_t *arg, struct msi_desc *desc);

/* Number of implemented MSI registers per function */
static const int aeolia_belize_subfuncs_per_func[AEOLIA_NUM_FUNCS] = {
	4, 4, 4, 4, 31, 2, 2, 4
};

static const int baikal_subfuncs_per_func[AEOLIA_NUM_FUNCS] = {
	2, 1, 1, 1, 31, 2, 3, 3
};

static inline u32 glue_read32(struct apcie_dev *sc, u32 offset) {
	return ioread32((void __iomem *) sc->glue_bar_to_use + offset);
}

static inline void glue_write32(struct apcie_dev *sc, u32 offset, u32 value) {
	iowrite32(value,(void __iomem *) sc->glue_bar_to_use + offset);
}

static inline void glue_set_region(struct apcie_dev *sc, u32 func, u32 bar,
			    u32 base, u32 mask) {
	glue_write32(sc, APCIE_REG_BAR_MASK(func, bar), mask);
	glue_write32(sc, APCIE_REG_BAR_ADDR(func, bar), base);
}

static inline void glue_set_mask(struct apcie_dev *sc, u32 offset, u32 mask) {
	void __iomem *ptr = sc->glue_bar_to_use + offset;
	iowrite32(ioread32(ptr) | mask, ptr);
}

static inline void glue_clear_mask(struct apcie_dev *sc, u32 offset, u32 mask) {
	void __iomem *ptr = sc->glue_bar_to_use + offset;
	iowrite32(ioread32(ptr) & ~mask, ptr);
}

static inline void glue_mask_and_set(struct apcie_dev *sc, u32 offset, u32 mask, u32 set) {
	void __iomem *ptr = sc->glue_bar_to_use + offset;
	iowrite32((ioread32(ptr) & ~mask) | set, ptr);
}

static void apcie_config_msi(struct apcie_dev *sc, u32 func, u32 subfunc,
			     u32 addr, u32 data) {
	u32 offset;

	sc_dbg("apcie_config_msi: func: %u, subfunc: %u, addr %08x data: 0x%08x (%u)\n",
		func, subfunc, addr, data, data);

	glue_clear_mask(sc, APCIE_REG_MSI_CONTROL, APCIE_REG_MSI_CONTROL_ENABLE);
	/* Unknown */
	glue_write32(sc, APCIE_REG_MSI(0x8), 0xffffffff);
	/* Unknown */
	glue_write32(sc, APCIE_REG_MSI(0xc + (func << 2)), 0xB7FFFF00 + func * 16);
	glue_write32(sc, APCIE_REG_MSI_ADDR(func), addr);
	/* Unknown */
	glue_write32(sc, APCIE_REG_MSI(0xcc + (func << 2)), 0);
	glue_write32(sc, APCIE_REG_MSI_DATA_HI(func), data & 0xffe0);

	if (func < 4) {
		/* First 4 functions have 4 IRQs/subfuncs each */
		offset = (func << 4) | (subfunc << 2);
	} else if (func == 4) {
		/* Function 4 gets 24 consecutive slots,
		 * then 7 more at the end. */
		if (subfunc < 24)
			offset = 0x40 + (subfunc << 2);
		else
			offset = 0xe0 + ((subfunc - 24) << 2);
	} else {
		offset = 0xa0 + ((func - 5) << 4) + (subfunc << 2);
	}

	glue_write32(sc, APCIE_REG_MSI_DATA_LO(offset), data & 0x1f);

	if (func == AEOLIA_FUNC_ID_PCIE)
		glue_set_mask(sc, APCIE_REG_MSI_MASK(func), APCIE_REG_MSI_MASK_FUNC4);
	else
		glue_set_mask(sc, APCIE_REG_MSI_MASK(func), APCIE_REG_MSI_MASK_FUNC);

	glue_set_mask(sc, APCIE_REG_MSI_CONTROL, APCIE_REG_MSI_CONTROL_ENABLE);
}

static void baikal_msi_write_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct apcie_dev *sc = data->chip_data;

	/* Linux likes to unconfigure MSIs like this, but since we share the
	 * address between subfunctions, we can't do that. The IRQ should be
	 * masked via apcie_msi_mask anyway, so just do nothing. */
	if (!msg->address_lo) {
		return;
	}

	dev_info(data->common->msi_desc->dev, "baikal_msi_write_msg(%08x, %08x) mask=0x%x irq=%d hwirq=0x%lx %p\n",
		msg->address_lo, msg->data, data->mask, data->irq, data->hwirq, sc);


	pci_msi_domain_write_msg(data, msg);
}

static void apcie_msi_write_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct apcie_dev *sc = data->chip_data;
	u32 func = data->hwirq >> 8;
        //u32 subfunc = data->hwirq & 0xff;
	u32 subfunc = data->hwirq & 0x1f; // Was this a typo? seems important

	/* Linux likes to unconfigure MSIs like this, but since we share the
	 * address between subfunctions, we can't do that. The IRQ should be
	 * masked via apcie_msi_mask anyway, so just do nothing. */
	if (!msg->address_lo) {
		return;
	}

	sc_dbg("apcie_msi_write_msg(%08x, %08x) mask=0x%x irq=%d hwirq=0x%lx %p\n",
	       msg->address_lo, msg->data, data->mask, data->irq, data->hwirq,
	       sc);

	if (subfunc == 0x1f) {
		int i;
		for (i = 0; i < aeolia_belize_subfuncs_per_func[func];
		     i++)
			apcie_config_msi(sc, func, i, msg->address_lo,
					 msg->data);
	} else {
		apcie_config_msi(sc, func, subfunc, msg->address_lo,
				 msg->data);
	}
}


static void baikal_pcie_msi_unmask(struct irq_data *data)
{
	pci_msi_unmask_irq(data);
	return;
	struct apcie_dev *sc = data->chip_data;
	u8 subfunc = data->hwirq & 0x1f;
	struct msi_desc *desc = irq_data_get_msi_desc(data);
	struct pci_dev *pdev = msi_desc_to_pci_dev(desc);
	int msi_allocated = desc->nvec_used;
	int msi_msgnum = pci_msi_vec_count(pdev);
	u32 msi_mask = desc->msi_mask; //(1LL << msi_msgnum) - 1;

	u32 result;
	asm volatile(".intel_syntax noprefix;"
		     "mov 	 eax, %[amsi_allocated];"
		     "mov 	 edx, %[asubfunc];"
		     "mov 	 ebx, %[amsi_mask];"
		     "mov 	 esi, 0;"  //i = 0
		     "loop2:  lea  ecx, [rdx+rsi];"
		     "mov	 edi, 0x0FFFFFFFE;" //-2
		     "inc     esi;"
		     "rol     edi, cl;"
		     "and     ebx, edi;"
		     "cmp     esi, eax;"
		     "jl      short loop2;"
		     "mov 	 %[aResult], ebx;"  //msi_mask
		     ".att_syntax prefix;"
		     : [aResult] "=r" (result)
		     : [amsi_allocated] "r" (msi_allocated), [asubfunc] "r" ((u32)subfunc), [amsi_mask] "r" (msi_mask)
		     : "eax", "ebx", "edx");
	msi_mask = result;

	dev_info(data->common->msi_desc->dev, "bpcie_msi_unmask(msi_mask=0x%X, msi_allocated=0x%X)\n", msi_mask, msi_allocated);
	//msi_mask = 0;
	pci_write_config_dword(pdev, desc->mask_pos,
			       msi_mask);
	desc->msi_mask = msi_mask;

	//this code equals msi_mask = 0;
}

static void baikal_pcie_msi_mask(struct irq_data *data)
{
	pci_msi_mask_irq(data);
	return;
	struct apcie_dev *sc = data->chip_data;
	u8 subfunc = data->hwirq & 0x1f;
	struct msi_desc *desc = irq_data_get_msi_desc(data);
	struct pci_dev *pdev = msi_desc_to_pci_dev(desc);
	u32 msi_mask = desc->msi_mask;
	u32 msi_allocated = desc->nvec_used; //pci_msi_vec_count(msi_desc_to_pci_dev(desc)); 32 for bpcie glue

	if (msi_allocated > 0)
	{
		u32 result;
		asm volatile(".intel_syntax noprefix;"
			     "mov 	 eax, %[amsi_allocated];"
			     "mov 	 edx, %[asubfunc];"
			     "mov 	 ebx, %[amsi_mask];"
			     "mov 	 esi, 0;"  //i = 0
			     "loop:   lea  ecx, [rdx+rsi];"
			     "mov	 edi, 1;"
			     "inc     esi;"
			     "shl     edi, cl;"
			     "or      ebx, edi;"
			     "cmp     esi, eax;"
			     "jl      short loop;"
			     "mov 	 %[aResult], ebx;"  //msi_mask
			     ".att_syntax prefix;"
			     : [aResult] "=r" (result)
			     : [amsi_allocated] "r" (msi_allocated), [asubfunc] "r" ((u32)subfunc), [amsi_mask] "r" (msi_mask)
			     : "eax", "ebx", "edx");
		msi_mask = result;
	}

	dev_info(data->common->msi_desc->dev, "bpcie_msi_mask(msi_mask=0x%X, msi_allocated=0x%X)\n", msi_mask, msi_allocated);
	//msi_mask = 0;
	pci_write_config_dword(pdev, desc->mask_pos,
			       msi_mask);
	desc->msi_mask = msi_mask;
	//TODO: disable ht. See apcie_bpcie_msi_ht_disable_and_bpcie_set_msi_mask

	//this code equals msi_mask = 0xFFFFFFFF;
}

static void apcie_msi_unmask(struct irq_data *data)
{
	struct apcie_dev *sc = data->chip_data;
	u32 func = data->hwirq >> 8;
	struct msi_desc *desc = irq_data_get_msi_desc(data);

	if(desc) {
		desc->msi_mask |= data->mask;
	} else {
		pr_debug("no msi_desc at apcie_msi_unmask\n");
	}

	glue_set_mask(sc, APCIE_REG_MSI_MASK(func), data->mask);
}

static void apcie_msi_mask(struct irq_data *data)
{
	struct apcie_dev *sc = data->chip_data;
	u32 func = data->hwirq >> 8;

	struct msi_desc *desc = irq_data_get_msi_desc(data);

	if (desc) {
		desc->msi_mask &= ~data->mask;
	} else {
		pr_debug("no msi_desc at apcie_msi_mask\n");
	}

	glue_clear_mask(sc, APCIE_REG_MSI_MASK(func), data->mask);
}

static void apcie_msi_calc_mask(struct irq_data *data) {
	u32 func = data->hwirq >> 8;
	u32 subfunc = data->hwirq & 0x1f; // Same as previous mention

	if (subfunc == 0x1f) {
		data->mask =
			(1 << aeolia_belize_subfuncs_per_func[func]) - 1;
	} else {
		data->mask = 1 << subfunc;
	}
}

// TODO (ps4patches): This is awesome, make this for aeolia as well
static void baikal_handle_edge_irq(struct irq_desc *desc)
{
	u32 vector_read;
	unsigned int vector_to_write;
	unsigned int mask;
	char shift;

	//return handle_edge_irq(desc);
	u32 func = (desc->irq_data.hwirq >> 5) & 7;
	u32 initial_hwirq = desc->irq_data.hwirq & ~0x1fLL;
	pr_info("bpcie_handle_edge_irq(hwirq=0x%X, irq=0x%X)\n", desc->irq_data.hwirq,
 	 desc->irq_data.irq);

	if (func == 4)          // Baikal Glue, 5 bits for subfunctions
	{
		vector_to_write = 2;
		mask = -1;
		shift = 0;
	}
	else if (func == 7)     // Baikal USB 3.0 xHCI Host Controller
	{
		vector_to_write = 3;
		mask = 7;
		shift = 0x10;
	}
	else if (func == 5)        // Baikal DMA Controller
	{
		mask = 3;
		vector_to_write = 3;
		shift = 0;
	} else {
		handle_edge_irq(desc);
		return;
	}

	raw_spin_lock(&desc->lock); //TODO: try it
	struct apcie_dev *sc = desc->irq_data.chip_data;
	glue_write32(sc, BPCIE_ACK_WRITE, vector_to_write);
	vector_read = glue_read32(sc, BPCIE_ACK_READ);
	raw_spin_unlock(&desc->lock);

	int new_desc_found = 0;
	unsigned int subfunc_mask = mask & ~(vector_read >> shift);
	sc_info("subfunc_mask=0x%X, vector_to_write=0x%X, vector_read=0x%X\n", subfunc_mask, vector_to_write, vector_read);
	unsigned int i;
	for (i = 0; i < 32; i++) {
		if (subfunc_mask & (1 << i)) { //if (test_bit(vector, used_vectors))
			unsigned int virq = irq_find_mapping(desc->irq_data.domain,
							     initial_hwirq + i);
			struct irq_desc *new_desc = irq_to_desc(virq);
			if (new_desc) {
				//dev_info(new_desc->irq_common_data.msi_desc->dev, "handle_edge_irq_int(new hwirq=0x%X, irq=0x%X)\n", new_desc->irq_data.hwirq, new_desc->irq_data.irq);
				handle_edge_irq(new_desc);
			}
		}
	}
}

int apcie_is_compatible_device(struct pci_dev *dev)
{
	if (!dev || dev->vendor != PCI_VENDOR_ID_SONY) {
		return 0;
	}
	return (dev->device == PCI_DEVICE_ID_SONY_AEOLIA_PCIE ||
		dev->device == PCI_DEVICE_ID_SONY_BELIZE_PCIE ||
		dev->device == PCI_DEVICE_ID_SONY_BAIKAL_PCIE);
}

static void apcie_irq_msi_compose_msg(struct irq_data *data,
				       struct msi_msg *msg)
{
	struct irq_cfg *cfg = irqd_cfg(data);
	struct apcie_dev *sc = data->chip_data;
	int i;

	memset(msg, 0, sizeof(*msg));
	msg->address_hi = X86_MSI_BASE_ADDRESS_HIGH;
	msg->address_lo = 0xfee00000;// Just do it like this for now

	if (sc) {
		for (i = 0; i < 100; i++) {
			if (sc->irq_map[i] == data->irq) {
				msg->data = i;
				break;
			}
		}
	} else {
		pr_info("apcie_irq_msi_compose_msg SC null\n");
		msg->data = data->irq - 1;
	}

	pr_info("apcie_irq_msi_compose_msg %x %x\n", msg->data, cfg->vector);
}

static struct irq_chip apcie_msi_controller = {
	.name = "Aeolia-MSI",
	.irq_unmask = apcie_msi_unmask,
	.irq_mask = apcie_msi_mask,
	.irq_ack = irq_chip_ack_parent,
	.irq_set_affinity = msi_domain_set_affinity,
	.irq_retrigger = irq_chip_retrigger_hierarchy,
	.irq_compose_msi_msg = apcie_irq_msi_compose_msg,
	.irq_write_msi_msg = apcie_msi_write_msg,
	.flags = IRQCHIP_SKIP_SET_WAKE | IRQCHIP_AFFINITY_PRE_STARTUP,
};

// We still call it Aeolia-MSI so vector.c can identify it as a ps4 southbridge
static struct irq_chip baikal_pcie_msi_controller = {
	.name = "Aeolia-MSI",
	.irq_unmask = baikal_pcie_msi_unmask,
	.irq_mask = baikal_pcie_msi_mask,
	.irq_ack = irq_chip_ack_parent,
	.irq_set_affinity = msi_domain_set_affinity,
	.irq_retrigger = irq_chip_retrigger_hierarchy,
	.irq_compose_msi_msg = apcie_irq_msi_compose_msg,
	.irq_write_msi_msg = baikal_msi_write_msg,
	.flags = IRQCHIP_SKIP_SET_WAKE,
};

static irq_hw_number_t apcie_msi_get_hwirq(struct msi_domain_info *info,
					  msi_alloc_info_t *arg)
{
	return arg->hwirq;
}

static int apcie_msi_init(struct irq_domain *domain,
			 struct msi_domain_info *info, unsigned int virq,
			 irq_hw_number_t hwirq, msi_alloc_info_t *arg)
{
	int i;
	struct irq_data *data;
	struct apcie_dev *sc = info->chip_data;
	pr_err("apcie_msi_init(%p, %p, %d, 0x%lx, %p)\n", domain, info, virq, hwirq, arg);

	data = irq_domain_get_irq_data(domain, virq);

	if(((struct apcie_dev*) info->chip_data)->is_baikal) {
		irq_domain_set_info(domain, virq, hwirq, info->chip, info->chip_data,
				    baikal_handle_edge_irq, NULL, "edge");
	} else {
		irq_domain_set_info(domain, virq, hwirq, info->chip, info->chip_data,
				    handle_edge_irq, NULL, "edge");

		apcie_msi_calc_mask(data);
	}

	if (sc) {
		for (i = 0; i < 100; i++) {
			if (sc->irq_map[i] == -1) {
				sc->irq_map[i] = (int8_t)virq;
				break;
			}
		}
	}

	return 0;
}

static void apcie_msi_free(struct irq_domain *domain,
			  struct msi_domain_info *info, unsigned int virq)
{
	int i;

	struct apcie_dev *sc = info->chip_data;

	pr_err("apcie_msi_free(%d)\n", virq);


	if (sc) {
		// TODO (ps4patches): not sure what's supposed to happen here
		for (i = 0; i < 100; i++) {
			if (sc->irq_map[i] == virq) {
				sc->irq_map[i] = -1;
				break;
			}
		}
	}
}

static struct msi_domain_ops apcie_msi_domain_ops = {
	.get_hwirq	= apcie_msi_get_hwirq,
	.set_desc       = apcie_set_desc,
	.msi_init	= apcie_msi_init,
	.msi_free	= apcie_msi_free,
};

static struct msi_domain_info apcie_msi_domain_info = {
	.flags		= MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS,
	.ops		= &apcie_msi_domain_ops,
	.chip		= &apcie_msi_controller,
	.handler	= handle_edge_irq,
	.handler_name	= "edge"
};

void apcie_set_desc(msi_alloc_info_t *arg, struct msi_desc *desc)
{
	//IRQs "come from" function 4 as far as the IOMMU/system see
	unsigned int sc_devfn = 0;
	struct pci_dev *sc_dev = NULL;
	struct apcie_dev *sc = NULL;
	struct pci_dev *dev = msi_desc_to_pci_dev(desc);

	arg->desc = desc;

	if (dev) {
		sc_devfn = (dev->devfn & ~7) | AEOLIA_FUNC_ID_PCIE;
		sc_dev = pci_get_slot(dev->bus, sc_devfn);

		sc = pci_get_drvdata(sc_dev);
		arg->devid = pci_dev_id(sc_dev);

		arg->type = X86_IRQ_ALLOC_TYPE_PCI_MSI;

		// !irq_chip_inited is so it can set a hwirq for all the irq_chip devices
		if ((arg->desc->dev != &sc_dev->dev) || (!sc->irq_chip_inited)) {
			pr_info("Modifying HWIRQ: %x\n", arg->hwirq);

			if (sc_dev->device == PCI_DEVICE_ID_SONY_BAIKAL_PCIE) {
				//Our hwirq number is (slot << 8) | (func << 5) plus subfunction.
				// Subfunction is usually 0 and implicitly increments per hwirq,
				//but can also be 0xff to indicate that this is a shared IRQ.'
				arg->hwirq = (PCI_SLOT(dev->devfn) << 8) |
					(PCI_FUNC(dev->devfn) << 5);
			} else {
				//Our hwirq number is (slot << 8) plus subfunction.
				// Subfunction is usually 0 and implicitly increments per hwirq,
				//but can also be 0xff to indicate that this is a shared IRQ.'
				arg->hwirq = (PCI_FUNC(dev->devfn) << 8);
			}

#ifndef QEMU_HACK_NO_IOMMU
			arg->flags = X86_IRQ_ALLOC_CONTIGUOUS_VECTORS;
			if (!(apcie_msi_domain_info.flags & MSI_FLAG_MULTI_PCI_MSI)) {
				arg->hwirq |= 0x1F; // Shared IRQ for all subfunctions
			}
#endif

			// Reassign the device,
			// because all interrupts come from the AEOLIA/BELIZE/BAIKAL chip
			if (arg->desc->dev != &sc_dev->dev) {
				pr_info("Reassigning device\n", arg->hwirq);

				arg->desc->dev = &sc_dev->dev;
			}
		}
	}

	pci_dev_put(sc_dev);

	pr_info("set_desc HWIRQ: %x\n", arg->hwirq);
}

static struct irq_domain *apcie_create_irq_domain(struct apcie_dev *sc, struct pci_dev *pdev)
{
	struct irq_domain *domain, *parent;
	struct fwnode_handle *fn;
	struct irq_fwspec fwspec;

	sc_dbg("apcie_create_irq_domain\n");
	if (x86_vector_domain == NULL)
		return NULL;

	apcie_msi_domain_info.chip_data = (void *)sc;

	if(sc->is_baikal) {
		apcie_msi_domain_info.handler = baikal_handle_edge_irq;
		apcie_msi_domain_info.chip = &baikal_pcie_msi_controller;
	}

	fn = irq_domain_alloc_named_id_fwnode("Aeolia-MSI", pci_dev_id(pdev));
	if (!fn) {
		return NULL;
	}

	sc_dbg("devid = %d\n", pci_dev_id(pdev));

	fwspec.fwnode = fn;
	fwspec.param_count = 1;

	// It should be correct to put the pci device id in here
	fwspec.param[0] = pci_dev_id(pdev);

	parent = irq_find_matching_fwspec(&fwspec, DOMAIN_BUS_ANY);
	if (!parent) {
		sc_dbg("no parent \n");
		parent = x86_vector_domain;
	} else if (parent == x86_vector_domain) {
		sc_dbg("x86_vector_domain parent \n");
	} else {
		apcie_msi_domain_info.flags |= MSI_FLAG_MULTI_PCI_MSI;
		apcie_msi_controller.name = "IR-Aeolia-MSI";
	}

	domain = pci_msi_create_irq_domain(fn, &apcie_msi_domain_info, parent);
	if (domain) {
		dev_set_msi_domain(&pdev->dev, domain);
	} else {
		irq_domain_free_fwnode(fn);
		pr_warn("Failed to initialize Aeolia-MSI irqdomain.\n");
	}

	return domain;
}

// This should assign the PCIE irq domain to every pcie device under the
// AEOLIA/BAIKAL/BELIZE chip
static void assignDomains(struct apcie_dev *sc)
{
	int func;
	struct irq_domain *domain;
	struct pci_dev *pdev;

	unsigned int devfn;
	struct pci_dev *sc_dev;

	// First make a PCIE domain so we can assign the rest
	sc_dev = sc->pdev;
	devfn = (sc_dev->devfn & ~7) | AEOLIA_FUNC_ID_PCIE;
	pdev = pci_get_slot(sc_dev->bus, devfn);

	if (pdev) {
		domain = apcie_create_irq_domain(sc, pdev);
		sc->irqdomain = domain;
		pci_dev_put(pdev);
	} else {
		sc_err("cannot create pcie irqdomain!");
		return;
	}

	// Now we can use the PCIE domain everywhere else
	for (func = 0; func < AEOLIA_NUM_FUNCS; ++func) {
		if (func != AEOLIA_FUNC_ID_PCIE) {
			devfn = (sc_dev->devfn & ~7) | func;
			pdev = pci_get_slot(sc_dev->bus, devfn);

			if (pdev) {
				// Domain per subdevice
				apcie_create_irq_domain(sc, pdev);

				pci_dev_put(pdev);
			} else
				sc_err("cannot find apcie func %d device",
						func);
		}
	}
}

int msi_capability_init(struct pci_dev *dev, int nvec,
        struct irq_affinity *affd);
int apcie_assign_irqs(struct pci_dev *dev, int nvec)
{
	int ret = 0;
	unsigned int sc_devfn;
	struct pci_dev *sc_dev;
	struct apcie_dev *sc;
	struct irq_alloc_info info;
	struct msi_desc *desc;

	sc_devfn = (dev->devfn & ~7) | AEOLIA_FUNC_ID_PCIE;
	sc_dev = pci_get_slot(dev->bus, sc_devfn);

	if (!apcie_is_compatible_device(sc_dev)) {
		dev_err(&dev->dev, "apcie: this is not an Aeolia device\n");
		ret = -ENODEV;
		goto fail;
	}
	sc = pci_get_drvdata(sc_dev);
	if (!sc) {
		dev_err(&dev->dev,
				"apcie: not ready yet, cannot assign IRQs\n");
		ret = -ENODEV;
		goto fail;
	}

	if (!dev->msi_enabled) {
#ifndef QEMU_HACK_NO_IOMMU
		if (!(apcie_msi_domain_info.flags &
					MSI_FLAG_MULTI_PCI_MSI)) {
			nvec = 1;
		}
#endif

		if (sc->is_baikal) {
			ret = pci_alloc_irq_vectors(dev, 1, nvec, PCI_IRQ_MSI);
		} else {
			ret = msi_capability_init(dev, nvec, NULL);
			if (ret == 0) {
				ret = nvec;
			}
		}
	} else {
		ret = nvec;
	}

fail:
	dev_info(&dev->dev, "apcie_assign_irqs returning %d\n", ret);
	if (sc_dev)
		pci_dev_put(sc_dev);
	return ret;
}
EXPORT_SYMBOL(apcie_assign_irqs);

void apcie_free_irqs(struct pci_dev* dev)
{
	if (dev != NULL &&
	    dev->irq != NULL &&
	    dev_get_msi_domain(&dev->dev) != NULL) {
		// TODO (ps4patches): This might be duplicate and already handled nowadays
		__msi_domain_free_irqs(dev_get_msi_domain(&dev->dev), &dev->dev);
	}
}
EXPORT_SYMBOL(apcie_free_irqs);

static void apcie_glue_remove(struct apcie_dev *sc);

static int apcie_glue_init(struct apcie_dev *sc)
{
	int i;

	sc_info("apcie glue probe\n");

	if(!sc->is_baikal) {
		if (!request_mem_region(pci_resource_start(sc->pdev, 4) +
				    																	 APCIE_RGN_PCIE_BASE, APCIE_RGN_PCIE_SIZE, 
																							 "apcie.glue")) {
			sc_err("Failed to request pcie region\n");
			return -EBUSY;
		}
	} else {
		if (!request_mem_region(pci_resource_start(sc->pdev, 2), pci_resource_len(sc->pdev, 2),
					"bpcie.glue")) {
			sc_err("Failed to request pcie region\n");
			return -EBUSY;

		}
	}

	if (!sc->is_baikal) {
		if (!request_mem_region(pci_resource_start(sc->pdev, 2) + 
		    APCIE_RGN_CHIPID_BASE, APCIE_RGN_CHIPID_SIZE,
		    "apcie.chipid")) {
			sc_err("Failed to request chipid region\n");
			release_mem_region(pci_resource_start(sc->pdev, 4) + APCIE_RGN_PCIE_BASE, APCIE_RGN_PCIE_SIZE);
			return -EBUSY;
		}
	} else {
		if (!request_mem_region(pci_resource_start(sc->pdev, 4), pci_resource_len(sc->pdev, 4),
		     "bpcie.chipid")) {

			sc_err("Failed to request chipid region\n");
			release_mem_region(pci_resource_start(sc->pdev, 2), pci_resource_len(sc->pdev, 2));
			return -EBUSY;
		}
	}

	if (!sc->is_baikal) {
		// Apparently baikal doesn't do this
		glue_set_region(sc, AEOLIA_FUNC_ID_PCIE, 2, 0xbf018000, 0x7fff);

		sc_info("Aeolia chip revision: %08x:%08x:%08x\n",
			ioread32(sc->bar2 + APCIE_REG_CHIPID_0),
			ioread32(sc->bar2 + APCIE_REG_CHIPID_1),
			ioread32(sc->bar2 + APCIE_REG_CHIPREV));
	} else {
		sc_info("Baikal chip revision: %08x:%08x:%08x\n",
			ioread32(sc->bar4 + BPCIE_REG_CHIPID_0),
			ioread32(sc->bar4 + BPCIE_REG_CHIPID_1),
			ioread32(sc->bar4 + BPCIE_REG_CHIPREV));
	}

	if (!sc->is_baikal) {
		/* Mask all MSIs first, to avoid spurious IRQs */
		for (i = 0; i < AEOLIA_NUM_FUNCS; i++) {
			glue_write32(sc, APCIE_REG_MSI_MASK(i), 0);
			glue_write32(sc, APCIE_REG_MSI_ADDR(i), 0);
			glue_write32(sc, APCIE_REG_MSI_DATA_HI(i), 0);
		}

		for (i = 0; i < 0xfc; i += 4)
			glue_write32(sc, APCIE_REG_MSI_DATA_LO(i), 0);

		glue_set_region(sc, AEOLIA_FUNC_ID_GBE, 0, 0xbfa00000, 0x3fff);
		glue_set_region(sc, AEOLIA_FUNC_ID_AHCI, 5, 0xbfa04000, 0xfff);
		glue_set_region(sc, AEOLIA_FUNC_ID_SDHCI, 0, 0xbfa80000, 0xfff);
		glue_set_region(sc, AEOLIA_FUNC_ID_SDHCI, 1, 0, 0);
		glue_set_region(sc, AEOLIA_FUNC_ID_DMAC, 0, 0xbfa05000, 0xfff);
		glue_set_region(sc, AEOLIA_FUNC_ID_DMAC, 1, 0, 0);
		glue_set_region(sc, AEOLIA_FUNC_ID_DMAC, 2, 0xbfa06000, 0xfff);
		glue_set_region(sc, AEOLIA_FUNC_ID_DMAC, 3, 0, 0);
		glue_set_region(sc, AEOLIA_FUNC_ID_MEM, 2, 0xc0000000,
				0x3fffffff);
		glue_set_region(sc, AEOLIA_FUNC_ID_MEM, 3, 0, 0);
		glue_set_region(sc, AEOLIA_FUNC_ID_XHCI, 0, 0xbf400000,
				0x1fffff);
		glue_set_region(sc, AEOLIA_FUNC_ID_XHCI, 1, 0, 0);
		glue_set_region(sc, AEOLIA_FUNC_ID_XHCI, 2, 0xbf600000,
				0x1fffff);
		glue_set_region(sc, AEOLIA_FUNC_ID_XHCI, 3, 0, 0);
		glue_set_region(sc, AEOLIA_FUNC_ID_XHCI, 4, 0xbf800000,
				0x1fffff);
		glue_set_region(sc, AEOLIA_FUNC_ID_XHCI, 5, 0, 0);
	}

	assignDomains(sc);

	if (!sc->irqdomain) {
		sc_err("Failed to create IRQ domain");
		apcie_glue_remove(sc);
		return -EIO;
	}

	if (sc->is_baikal)
		sc->nvec =
			pci_alloc_irq_vectors(sc->pdev, BPCIE_SUBFUNC_ICC + 1,
					      BPCIE_NUM_SUBFUNCS, PCI_IRQ_MSI);
	else
		sc->nvec = apcie_assign_irqs(sc->pdev, APCIE_NUM_SUBFUNCS);

	if (sc->nvec <= 0) {
		sc_err("Failed to assign IRQs");
		apcie_glue_remove(sc);
		return -EIO;
	}

	sc->irq_chip_inited = true;

	sc_dbg("dev->irq=%d\n", sc->pdev->irq);

	return 0;
}

static void apcie_glue_remove(struct apcie_dev *sc) {
	sc_info("apcie glue remove\n");

	if (sc->nvec > 0) {
		apcie_free_irqs(sc->pdev);
		sc->nvec = 0;
	}
	if (sc->irqdomain) {
		irq_domain_remove(sc->irqdomain);
		sc->irqdomain = NULL;
	}

	// TODO (ps4patches): What does this resource length mean?
	if(sc->is_baikal) {
		release_mem_region(pci_resource_start(sc->pdev, 4),
				   pci_resource_len(sc->pdev, 4));
		release_mem_region(pci_resource_start(sc->pdev, 2),
				   pci_resource_len(sc->pdev, 2));
	} else {
		release_mem_region(pci_resource_start(sc->pdev, 2) +
					   APCIE_RGN_CHIPID_BASE,
				   APCIE_RGN_CHIPID_SIZE);
		release_mem_region(pci_resource_start(sc->pdev, 4) +
					   APCIE_RGN_PCIE_BASE,
				   APCIE_RGN_PCIE_SIZE);
	}
}

#ifdef CONFIG_PM
static int apcie_glue_suspend(struct apcie_dev *sc, pm_message_t state) {
	return 0;
}

static int apcie_glue_resume(struct apcie_dev *sc) {
	return 0;
}
#endif


int apcie_uart_init(struct apcie_dev *sc);
int apcie_icc_init(struct apcie_dev *sc);
void apcie_uart_remove(struct apcie_dev *sc);
void apcie_icc_remove(struct apcie_dev *sc);
#ifdef CONFIG_PM
void apcie_uart_suspend(struct apcie_dev *sc, pm_message_t state);
void apcie_icc_suspend(struct apcie_dev *sc, pm_message_t state);
void apcie_uart_resume(struct apcie_dev *sc);
void apcie_icc_resume(struct apcie_dev *sc);
#endif

/* From arch/x86/platform/ps4/ps4.c */
extern bool apcie_initialized;

static int apcie_probe(struct pci_dev *dev, const struct pci_device_id *id) {
	struct apcie_dev *sc;
	int ret;

	dev_info(&dev->dev, "apcie_probe()\n");

	ret = pci_enable_device(dev);
	if (ret) {
		dev_err(&dev->dev,
			"apcie_probe(): pci_enable_device failed: %d\n", ret);
		return ret;
	}

	sc = kzalloc(sizeof(*sc), GFP_KERNEL);
	if (!sc) {
		dev_err(&dev->dev, "apcie_probe(): alloc sc failed\n");
		ret = -ENOMEM;
		goto disable_dev;
	}
	sc->pdev = dev;

	pci_set_drvdata(dev, sc);
	// eMMC ... unused?
	sc->bar0 = pci_ioremap_bar(dev, 0);
	// pervasive 0 - misc peripherals (baikal)
	sc->bar2 = pci_ioremap_bar(dev, 2);
	// pervasive 1 - misc peripherals (aeolia/belize)
	sc->bar4 = pci_ioremap_bar(dev, 4);

	if (!sc->bar0 || !sc->bar2 || !sc->bar4) {
		sc_err("failed to map some BARs, bailing out\n");
		ret = -EIO;
		goto free_bars;
	}

	sc->irq_chip_inited = false;
	memset(sc->irq_map, -1, 100);

	sc->is_baikal = sc->pdev->device == PCI_DEVICE_ID_SONY_BAIKAL_PCIE;
	if(sc->is_baikal) {
		sc->glue_bar_to_use = sc->bar2;
		sc->glue_bar_to_use_num = 2;
	} else {
		sc->glue_bar_to_use = sc->bar4;
		sc->glue_bar_to_use_num = 4;
	}

	if ((ret = apcie_glue_init(sc)) < 0)
		goto free_bars;
	// TODO (ps4patches): figure out why this dies a horrible and painful death.
	//if ((ret = apcie_uart_init(sc)) < 0)
	//	goto remove_glue;
	if ((ret = apcie_icc_init(sc)) < 0)
		goto remove_glue;

	apcie_initialized = true;
	return 0;

remove_uart:
	apcie_uart_remove(sc);
remove_glue:
	apcie_glue_remove(sc);
free_bars:
	if (sc->bar0)
		iounmap(sc->bar0);
	if (sc->bar2)
		iounmap(sc->bar2);
	if (sc->bar4)
		iounmap(sc->bar4);
	kfree(sc);
disable_dev:
	pci_disable_device(dev);
	return ret;
}

static void apcie_remove(struct pci_dev *dev) {
	struct apcie_dev *sc;
	sc = pci_get_drvdata(dev);

	apcie_icc_remove(sc);
	apcie_uart_remove(sc);
	apcie_glue_remove(sc);

	if (sc->bar0)
		iounmap(sc->bar0);
	if (sc->bar2)
		iounmap(sc->bar2);
	if (sc->bar4)
		iounmap(sc->bar4);
	kfree(sc);
	pci_disable_device(dev);
}

#ifdef CONFIG_PM
static int apcie_suspend(struct pci_dev *dev, pm_message_t state) {
	struct apcie_dev *sc;
	sc = pci_get_drvdata(dev);

	apcie_icc_suspend(sc, state);
	apcie_uart_suspend(sc, state);
	apcie_glue_suspend(sc, state);
	return 0;
}

static int apcie_resume(struct pci_dev *dev) {
	struct apcie_dev *sc;
	sc = pci_get_drvdata(dev);

	apcie_icc_resume(sc);
	apcie_glue_resume(sc);
	apcie_uart_resume(sc);
	return 0;
}
#endif

static const struct pci_device_id apcie_pci_tbl[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_SONY, PCI_DEVICE_ID_SONY_AEOLIA_PCIE), },
	{ PCI_DEVICE(PCI_VENDOR_ID_SONY, PCI_DEVICE_ID_SONY_BELIZE_PCIE), },
	{ PCI_DEVICE(PCI_VENDOR_ID_SONY, PCI_DEVICE_ID_SONY_BAIKAL_PCIE), },
	{ }
};
MODULE_DEVICE_TABLE(pci, apcie_pci_tbl);

static struct pci_driver apcie_driver = {
	.name		= "aeolia_pcie",
	.id_table	= apcie_pci_tbl,
	.probe		= apcie_probe,
	.remove		= apcie_remove,
#ifdef CONFIG_PM
	.suspend	= apcie_suspend,
	.resume		= apcie_resume,
#endif
};
module_pci_driver(apcie_driver);
