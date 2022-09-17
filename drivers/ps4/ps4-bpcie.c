#define DEBUG

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/msi.h>
#include <asm/irqdomain.h>
#include <asm/irq_remapping.h>

#include <asm/msi.h>

#include <asm/ps4.h>

#include "baikal.h"

/* #define QEMU_HACK_NO_IOMMU */

#define APCIE_REG_CHIPID_0		0x1104
#define APCIE_REG_CHIPID_1		0x1108
#define APCIE_REG_CHIPREV		0x110c

/* Number of implemented MSI registers per function */
static const int subfuncs_per_func[BAIKAL_NUM_FUNCS] = {
	//4, 4, 4, 4, 31, 2, 2, 4
	2, 1, 1, 1, 31, 2, 3, 3
};

static void bpcie_msi_domain_set_desc(msi_alloc_info_t *arg, struct msi_desc *desc);

/*static inline */u32 glue_read32(struct bpcie_dev *sc, u32 offset) {
	return ioread32(sc->bar2 + offset);
}

/*static inline */void glue_write32(struct bpcie_dev *sc, u32 offset, u32 value) {
	iowrite32(value, sc->bar2 + offset);
}

static u8 get_subfunc(unsigned long hwirq) {
	//u32 func = (hwirq >> 5) & 7;
	return hwirq & 0x1f;
}

static void bpcie_msi_write_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct bpcie_dev *sc = data->chip_data;

	//Linux likes to unconfigure MSIs like this, but since we share the
	//address between subfunctions, we can't do that. The IRQ should be
	//masked via apcie_msi_mask anyway, so just do nothing.
	if (!msg->address_lo) {
		return;
	}

	dev_dbg(data->common->msi_desc->dev, "bpcie_msi_write_msg(%08x, %08x) mask=0x%x irq=%d hwirq=0x%lx %p\n",
	       msg->address_lo, msg->data, data->mask, data->irq, data->hwirq, sc);

	pci_msi_domain_write_msg(data, msg);
}

static void bpcie_msi_unmask(struct irq_data *data)
{
	pci_msi_unmask_irq(data);
	return;
	struct bpcie_dev *sc = data->chip_data;
	u8 subfunc = get_subfunc(data->hwirq);//data->hwirq & 0xff;
	struct msi_desc *desc = irq_data_get_msi_desc(data);
	struct pci_dev *pdev = msi_desc_to_pci_dev(desc);
	int msi_allocated = desc->nvec_used;
	int msi_msgnum = pci_msi_vec_count(pdev);
	u32 msi_mask = desc->masked; //(1LL << msi_msgnum) - 1;

	/*
	if ( msi_alloc > 0 )
  	{
	    int i = 0;
	    do
	    {
	      unsigned int unit_no = subfunc + i++;
	      msi_mask &= rol(-2, unit_no);
	    }
	    while ( i < msi_alloc );
  	}
	*/
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

	dev_dbg(data->common->msi_desc->dev, "bpcie_msi_unmask(msi_mask=0x%X, msi_allocated=0x%X)\n", msi_mask, msi_allocated);
	//msi_mask = 0;
	pci_write_config_dword(pdev, desc->mask_pos,
			       msi_mask);
	desc->masked = msi_mask;

	//this code equals msi_mask = 0;
}

static void bpcie_msi_mask(struct irq_data *data)
{
	pci_msi_mask_irq(data);
	return;
	struct bpcie_dev *sc = data->chip_data;
	u8 subfunc = get_subfunc(data->hwirq);
	struct msi_desc *desc = irq_data_get_msi_desc(data);
	struct pci_dev *pdev = msi_desc_to_pci_dev(desc);
	u32 msi_mask = desc->masked;
	u32 msi_allocated = desc->nvec_used; //pci_msi_vec_count(msi_desc_to_pci_dev(desc)); 32 for bpcie glue
	
	if ( msi_allocated > 0 )
    {
      /*
	  int i = 0;
      do
      {
    	  u16 unit_plus_i = subfunc + i++;
    	  unit_plus_i &= 0x1F;
    	  msi_mask |= 1 << unit_plus_i;
      }
      while ( i < msi_allocated );
      */
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
	
	dev_dbg(data->common->msi_desc->dev, "bpcie_msi_mask(msi_mask=0x%X, msi_allocated=0x%X)\n", msi_mask, msi_allocated);
	//msi_mask = 0;
	pci_write_config_dword(pdev, desc->mask_pos,
			       msi_mask);
	desc->masked = msi_mask;
	//TODO: disable ht. See apcie_bpcie_msi_ht_disable_and_bpcie_set_msi_mask

	//this code equals msi_mask = 0xFFFFFFFF;
}

static void bpcie_msi_calc_mask(struct irq_data *data) {
	//struct bpcie_dev *sc = data->chip_data;
	u8 subfunc = get_subfunc(data->hwirq);//data->hwirq & 0xff;
	data->mask = 1 << subfunc;
	dev_dbg(data->common->msi_desc->dev, "bpcie_msi_calc_mask(0x%X)\n", data->mask);
	
	/*
  num_of_alloc_messages = ivars->cfg.msi.msi_alloc;
  if ( num_of_alloc_messages > 0 )
  {
    // add subfunc to msi_mask
    count = 0;
    do
    {
      unit_no = LOWORD(ivars->conf.pd_unit) + count++;// unit_no is subfunction
      DWORD msi_mask &= __ROL4__(-2, unit_no);
    }
    while ( count < num_of_alloc_messages );
  }
	*/
}

static struct irq_chip bpcie_msi_controller = {
	.name = "Baikal-MSI",
	.irq_unmask = bpcie_msi_unmask,
	.irq_mask = bpcie_msi_mask,
	.irq_ack = irq_chip_ack_parent,
	.irq_set_affinity = msi_domain_set_affinity,
	.irq_retrigger = irq_chip_retrigger_hierarchy,
	.irq_compose_msi_msg = irq_msi_compose_msg,
	.irq_write_msi_msg = bpcie_msi_write_msg,
	.flags = IRQCHIP_SKIP_SET_WAKE,
};

static irq_hw_number_t bpcie_msi_get_hwirq(struct msi_domain_info *info,
					  msi_alloc_info_t *arg)
{
	return arg->msi_hwirq;
}

static void bpcie_handle_edge_irq(struct irq_desc *desc)
{
	//return handle_edge_irq(desc);
	u32 func = (desc->irq_data.hwirq >> 5) & 7;
	u32 initial_hwirq = desc->irq_data.hwirq & ~0x1fLL;
	//sc_dbg("bpcie_handle_edge_irq(hwirq=0x%X, irq=0x%X)\n", vector, desc->irq_data.irq);
	unsigned int vector_to_write;
	unsigned int mask;
	char shift;

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
	struct bpcie_dev *sc = desc->irq_data.chip_data;
	glue_write32(sc, BPCIE_ACK_WRITE, vector_to_write);
	u32 vector_read = glue_read32(sc, BPCIE_ACK_READ);
	raw_spin_unlock(&desc->lock);

	unsigned int subfunc_mask = mask & ~(vector_read >> shift);
	//sc_dbg("subfunc_mask=0x%X, vector_read=0x%X\n", subfunc_mask, vector_read);
	unsigned int i;
	for (i = 0; i < 32; i++) {
		if (subfunc_mask & (1 << i)) { //if (test_bit(vector, used_vectors))
			unsigned int virq = irq_find_mapping(desc->irq_data.domain,
					initial_hwirq + i);
			struct irq_desc *new_desc = irq_to_desc(virq);
			if (new_desc) {
				//dev_dbg(new_desc->irq_common_data.msi_desc->dev, "handle_edge_irq_int(new hwirq=0x%X, irq=0x%X)\n", new_desc->irq_data.hwirq, new_desc->irq_data.irq);
				handle_edge_irq(new_desc);
			}
		}
	}
}

static int bpcie_msi_init(struct irq_domain *domain,
			 struct msi_domain_info *info, unsigned int virq,
			 irq_hw_number_t hwirq, msi_alloc_info_t *arg)
{
	struct irq_data *data;
	pr_devel("bpcie_msi_init(%p, %p, %d, 0x%lx, %p)\n", domain, info, virq, hwirq, arg);

	data = irq_domain_get_irq_data(domain, virq);
	irq_domain_set_info(domain, virq, hwirq, info->chip, info->chip_data,
			bpcie_handle_edge_irq/*handle_edge_irq*/, NULL, "edge");
	//bpcie_msi_calc_mask(data);
	return 0;
}

static void bpcie_msi_free(struct irq_domain *domain,
			  struct msi_domain_info *info, unsigned int virq)
{
	pr_devel("bpcie_msi_free(%d)\n", virq);
}

static int bpcie_msi_prepare(struct irq_domain *domain, struct device *dev,
				  int nvec, msi_alloc_info_t *arg)
{
	memset(arg, 0, sizeof(*arg));
	return 0;
}

static struct msi_domain_ops bpcie_msi_domain_ops = {
	.get_hwirq	= bpcie_msi_get_hwirq,
	.msi_init	= bpcie_msi_init,
	.msi_free	= bpcie_msi_free,
	.set_desc	= bpcie_msi_domain_set_desc,
	.msi_prepare = bpcie_msi_prepare,
};

static struct msi_domain_info bpcie_msi_domain_info = {
	.flags		= MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS, //maybe also | MSI_FLAG_ACTIVATE_EARLY
	.ops		= &bpcie_msi_domain_ops,
	.chip		= &bpcie_msi_controller,
	.handler	= bpcie_handle_edge_irq/*handle_edge_irq*/,
};

static void bpcie_msi_domain_set_desc(msi_alloc_info_t *arg,
				    struct msi_desc *desc)
{
	struct pci_dev *dev = msi_desc_to_pci_dev(desc);
	arg->type = X86_IRQ_ALLOC_TYPE_MSI;
	//IRQs "come from" function 4 as far as the IOMMU/system see
	unsigned int sc_devfn;
	struct pci_dev *sc_dev;
	sc_devfn = (dev->devfn & ~7) | BAIKAL_FUNC_ID_PCIE;
	sc_dev = pci_get_slot(dev->bus, sc_devfn);
	arg->msi_dev = sc_dev;
	pci_dev_put(sc_dev);
	//Our hwirq number is (slot << 8) | (func << 5) plus subfunction.
	// Subfunction is usually 0 and implicitly increments per hwirq,
	//but can also be 0xff to indicate that this is a shared IRQ. 
	arg->msi_hwirq = (PCI_SLOT(dev->devfn) << 8) | (PCI_FUNC(dev->devfn) << 5);

	#ifndef QEMU_HACK_NO_IOMMU
		arg->flags = X86_IRQ_ALLOC_CONTIGUOUS_VECTORS;
		if (!(bpcie_msi_domain_info.flags & MSI_FLAG_MULTI_PCI_MSI)) {
			//desk->nvec = desk->nvec_used = 1;
			arg->msi_hwirq |= 0x1F; // Shared IRQ for all subfunctions
		}
	#endif
}

struct irq_domain *bpcie_create_irq_domain(struct bpcie_dev *sc, struct pci_dev *pdev)//similar to native_setup_msi_irqs
{
	struct irq_domain *parent;
	struct irq_alloc_info info;

	dev_dbg(&pdev->dev, "bpcie_create_irq_domain\n");
	if (x86_vector_domain == NULL) {
		dev_err(&pdev->dev, "bpcie: x86_vector_domain is NULL\n");
		return NULL;
	}

	bpcie_msi_domain_info.chip_data = (void *)sc;

	init_irq_alloc_info(&info, NULL);
	info.type = X86_IRQ_ALLOC_TYPE_MSI;
	info.msi_dev = pdev;
	parent = irq_remapping_get_ir_irq_domain(&info);
	if (parent == NULL) {
		parent = x86_vector_domain;
	} else {
		bpcie_msi_domain_info.flags |= MSI_FLAG_MULTI_PCI_MSI;
		bpcie_msi_controller.name = "IR-Baikal-MSI";
	}

	struct irq_domain *d;
	d = pci_msi_create_irq_domain(NULL, &bpcie_msi_domain_info, parent);
	if (d != NULL)
		dev_set_msi_domain(&pdev->dev, d);
	else
		dev_err(&pdev->dev, "bpcie: failed to create irq domain\n");

	return d;
}

int bpcie_is_compatible_device(struct pci_dev *dev)
{
	if (!dev || dev->vendor != PCI_VENDOR_ID_SONY) {
		return 0;
	}
	return (dev->device == PCI_DEVICE_ID_SONY_BAIKAL_PCIE);
}

int bpcie_assign_irqs(struct pci_dev *dev, int nvec)
{
	int ret;
	unsigned int sc_devfn;
	struct pci_dev *sc_dev;
	struct bpcie_dev *sc;

	sc_devfn = (dev->devfn & ~7) | BAIKAL_FUNC_ID_PCIE;
	sc_dev = pci_get_slot(dev->bus, sc_devfn);

	if (!bpcie_is_compatible_device(sc_dev)) {
		dev_err(&dev->dev, "bpcie: this is not a Baikal device\n");
		ret = -ENODEV;
		goto fail;
	}
	sc = pci_get_drvdata(sc_dev);
	if (!sc) {
		dev_err(&dev->dev, "bpcie: not ready yet, cannot assign IRQs\n");
		ret = -ENODEV;
		goto fail;
	}

	dev_dbg(&dev->dev, "bpcie_assign_irqs(%d)\n", nvec);

#ifndef QEMU_HACK_NO_IOMMU
	if (!(bpcie_msi_domain_info.flags & MSI_FLAG_MULTI_PCI_MSI)) {
		nvec = 1;
		//info.msi_hwirq |= 0xff; // Shared IRQ for all subfunctions
	}
#endif
	if (dev->msi_enabled)
		ret = nvec;
	else
		ret = pci_alloc_irq_vectors(dev, 1, nvec, PCI_IRQ_MSI);

fail:
	dev_dbg(&dev->dev, "bpcie_assign_irqs returning %d\n", ret);
	if (sc_dev)
		pci_dev_put(sc_dev);
	return ret;
}
EXPORT_SYMBOL(bpcie_assign_irqs);

void bpcie_free_irqs(unsigned int virq, unsigned int nr_irqs)
{
	irq_domain_free_irqs(virq, nr_irqs);
	//pci_free_irq_vectors(sc->pdev);
	//TODO: remove irqdomains
}
EXPORT_SYMBOL(bpcie_free_irqs);

static void bpcie_glue_remove(struct bpcie_dev *sc);

static struct pci_dev * get_bpcie_device(struct bpcie_dev *sc, u32 bcpie_func) {
	unsigned int devfn;
	struct pci_dev *sc_dev;

	sc_dev = sc->pdev;
	devfn = (sc_dev->devfn & ~7) | bcpie_func;
	return pci_get_slot(sc_dev->bus, devfn);
}

static void bpcie_create_irq_domains(struct bpcie_dev *sc) {
	int func;
	for (func = 0; func < BAIKAL_NUM_FUNCS; ++func) {
		struct pci_dev * bpcie_pdev = get_bpcie_device(sc, func);
		if (bpcie_pdev) {
			struct irq_domain * domain = bpcie_create_irq_domain(sc, bpcie_pdev);
			if (func == BAIKAL_FUNC_ID_PCIE) sc->irqdomain = domain;
			pci_dev_put(bpcie_pdev);
		} else
			sc_err("cannot find bpcie func %d device", func);
	}
}

static int bpcie_glue_init(struct bpcie_dev *sc)
{
	sc_info("bpcie glue probe\n");
	
	
	if (!request_mem_region(pci_resource_start(sc->pdev, 2), pci_resource_len(sc->pdev, 2),
				"bpcie.glue")) {
		sc_err("Failed to request pcie region\n");
		return -EBUSY;

	}
	
	if (!request_mem_region(pci_resource_start(sc->pdev, 4), pci_resource_len(sc->pdev, 4),
				"bpcie.chipid")) {
		sc_err("Failed to request chipid region\n");
		
		release_mem_region(pci_resource_start(sc->pdev, 2), pci_resource_len(sc->pdev, 2));
	
		return -EBUSY;
	}

	sc_info("Baikal chip revision: %08x:%08x:%08x\n",
		ioread32(sc->bar4 + BPCIE_REG_CHIPID_0),
		ioread32(sc->bar4 + BPCIE_REG_CHIPID_1),
		ioread32(sc->bar4 + BPCIE_REG_CHIPREV));

	//sc->irqdomain = bpcie_create_irq_domain(sc);
	bpcie_create_irq_domains(sc);
	if (!sc->irqdomain) {
		sc_err("Failed to create IRQ domain");
		bpcie_glue_remove(sc);
		return -EIO;
	}

	//sc->nvec = bpcie_assign_irqs(sc->pdev, BPCIE_NUM_SUBFUNC);
	sc->nvec = pci_alloc_irq_vectors(sc->pdev, BPCIE_SUBFUNC_ICC+1, BPCIE_NUM_SUBFUNCS, PCI_IRQ_MSI);
	if (sc->nvec <= 0) {
		sc_err("Failed to assign IRQs");
		bpcie_glue_remove(sc);
		return -EIO;
	}
	sc_dbg("dev->irq=%d\n", sc->pdev->irq);
	
	return 0;
}

static void bpcie_glue_remove(struct bpcie_dev *sc) {
	sc_info("bpcie glue remove\n");

	if (sc->nvec > 0) {
		bpcie_free_irqs(sc->pdev->irq, sc->nvec);
		sc->nvec = 0;
	}
	
	if (sc->irqdomain) {
		irq_domain_remove(sc->irqdomain);//TODO: remove other domains
		sc->irqdomain = NULL;
	}
	
	release_mem_region(pci_resource_start(sc->pdev, 4), pci_resource_len(sc->pdev, 4));
	release_mem_region(pci_resource_start(sc->pdev, 2), pci_resource_len(sc->pdev, 2));
}

#ifdef CONFIG_PM
static int bpcie_glue_suspend(struct bpcie_dev *sc, pm_message_t state) {
	return 0;
}

static int bpcie_glue_resume(struct bpcie_dev *sc) {
	return 0;
}
#endif


int bpcie_uart_init(struct bpcie_dev *sc);
int bpcie_icc_init(struct bpcie_dev *sc);
void bpcie_uart_remove(struct bpcie_dev *sc);
void bpcie_icc_remove(struct bpcie_dev *sc);
#ifdef CONFIG_PM
void bpcie_uart_suspend(struct bpcie_dev *sc, pm_message_t state);
void bpcie_icc_suspend(struct bpcie_dev *sc, pm_message_t state);
void bpcie_uart_resume(struct bpcie_dev *sc);
void bpcie_icc_resume(struct bpcie_dev *sc);
#endif

/* From arch/x86/platform/ps4/ps4.c */
extern bool bpcie_initialized;

static int bpcie_probe(struct pci_dev *dev, const struct pci_device_id *id) {
	struct bpcie_dev *sc;
	int ret;

	dev_dbg(&dev->dev, "bpcie_probe()\n");

	ret = pci_enable_device(dev);
	if (ret) {
		dev_err(&dev->dev,
			"bpcie_probe(): pci_enable_device failed: %d\n", ret);
		return ret;
	}

	sc = kzalloc(sizeof(*sc), GFP_KERNEL);
	if (!sc) {
		dev_err(&dev->dev, "bpcie_probe(): alloc sc failed\n");
		ret = -ENOMEM;
		goto disable_dev;
	}
	sc->pdev = dev;
	pci_set_drvdata(dev, sc);

	// eMMC ... unused?
	sc->bar0 = pci_ioremap_bar(dev, 0);
	// pervasive 0 - misc peripherals
	sc->bar2 = pci_ioremap_bar(dev, 2);
	// pervasive 1
	sc->bar4 = pci_ioremap_bar(dev, 4);

	if (!sc->bar0 || !sc->bar2 || !sc->bar4) {
		sc_err("failed to map some BARs, bailing out\n");
		ret = -EIO;
		goto free_bars;
	}

	if ((ret = bpcie_glue_init(sc)) < 0)
		goto free_bars;
	if ((ret = bpcie_uart_init(sc)) < 0)
		goto remove_glue;
	if ((ret = bpcie_icc_init(sc)) < 0)
		goto remove_uart;

	bpcie_initialized = true;
	return 0;

remove_uart:
	bpcie_uart_remove(sc);
remove_glue:
	bpcie_glue_remove(sc);
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

static void bpcie_remove(struct pci_dev *dev) {
	struct bpcie_dev *sc;
	sc = pci_get_drvdata(dev);

	bpcie_icc_remove(sc);
	bpcie_uart_remove(sc);
	bpcie_glue_remove(sc);

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
static int bpcie_suspend(struct pci_dev *dev, pm_message_t state) {
	struct bpcie_dev *sc;
	sc = pci_get_drvdata(dev);

	bpcie_icc_suspend(sc, state);
	bpcie_uart_suspend(sc, state);
	bpcie_glue_suspend(sc, state);
	return 0;
}

static int bpcie_resume(struct pci_dev *dev) {
	struct bpcie_dev *sc;
	sc = pci_get_drvdata(dev);

	bpcie_icc_resume(sc);
	bpcie_glue_resume(sc);
	bpcie_uart_resume(sc);
	return 0;
}
#endif

static const struct pci_device_id bpcie_pci_tbl[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_SONY, PCI_DEVICE_ID_SONY_BAIKAL_PCIE), },
	{ }
};
MODULE_DEVICE_TABLE(pci, bpcie_pci_tbl);

static struct pci_driver bpcie_driver = {
	.name		= "baikal_pcie",
	.id_table	= bpcie_pci_tbl,
	.probe		= bpcie_probe,
	.remove		= bpcie_remove,
#ifdef CONFIG_PM
	.suspend	= bpcie_suspend,
	.resume		= bpcie_resume,
#endif
};
module_pci_driver(bpcie_driver);
