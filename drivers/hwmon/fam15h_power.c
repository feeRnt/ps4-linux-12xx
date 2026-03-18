// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * fam15h_power.c - AMD Family 15h processor power monitoring
 *
 * Copyright (c) 2011-2016 Advanced Micro Devices, Inc.
 * Author: Andreas Herrmann <herrmann.der.user@googlemail.com>
 */

#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/bitops.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/time.h>
#include <linux/sched.h>
#include <linux/topology.h>
#include <asm/processor.h>
#include <asm/msr.h>

MODULE_DESCRIPTION("AMD Family 15h CPU processor power monitor");
MODULE_AUTHOR("Andreas Herrmann <herrmann.der.user@googlemail.com>");
MODULE_LICENSE("GPL");

/* D18F3 */
#define REG_NORTHBRIDGE_CAP		0xe8

/* D18F4 */
#ifdef CONFIG_X86_PS4
#define REG_PROCESSOR_TDP		0xf4

#else
#define REG_PROCESSOR_TDP		0x1b8

#endif

/* D18F5 */
/* On PS4 AMD Liverpool Systems with Jaguar Architecture,
 * the TDP_RUNNING_AVERAGE and TDP_LIMIT3 seem to be offset
 * by one 16 byte row for some reason, for PCI function 5.
 * They also are not very correctly aligned.. Maybe this is not right */
#ifdef CONFIG_X86_PS4
#define REG_TDP_RUNNING_AVERAGE		0xf0
#define REG_TDP_LIMIT3			0x88 /*0xf4*/

#else

#define REG_TDP_RUNNING_AVERAGE		0xe0
#define REG_TDP_LIMIT3			0xe8
#endif

#define FAM15H_MIN_NUM_ATTRS		2
#define FAM15H_NUM_GROUPS		2
#define MAX_CUS				8

/* set maximum interval as 1 second */
#define MAX_INTERVAL			1000

#define PCI_DEVICE_ID_AMD_15H_M70H_NB_F4 0x15b4

struct fam15h_power_data {
	struct pci_dev *pdev;
	unsigned int tdp_to_watts; //Multiplication factor
	unsigned int base_tdp; //REG_PROCESSOR_TDP
	unsigned int processor_pwr_watts; //Max power?
	unsigned int cpu_pwr_sample_ratio;
	const struct attribute_group *groups[FAM15H_NUM_GROUPS];
	struct attribute_group group;
	/* maximum accumulated power of a compute unit */
	u64 max_cu_acc_power;
	/* accumulated power of the compute units */
	u64 cu_acc_power[MAX_CUS];
	/* performance timestamp counter */
	u64 cpu_sw_pwr_ptsc[MAX_CUS];
	/* online/offline status of current compute unit */
	int cu_on[MAX_CUS];
	unsigned long power_period;
};

static bool is_carrizo_or_later(void)
{
	return boot_cpu_data.x86 == 0x15 && boot_cpu_data.x86_model >= 0x60;
}

static ssize_t power1_input_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	u32 val, tdp_limit, running_avg_range;
	s32 running_avg_capture;
	u64 curr_pwr_watts;
	struct fam15h_power_data *data = dev_get_drvdata(dev);
	struct pci_dev *f4 = data->pdev;

	pci_bus_read_config_dword(f4->bus, PCI_DEVFN(PCI_SLOT(f4->devfn), 5),
				  REG_TDP_RUNNING_AVERAGE, &val);
	pr_info("fam15h_power: REG_TDP_RUNNING_AVERAGE raw value = %#x in %s.\n",
			val, __func__);

	/*
	 * On Carrizo and later platforms, TdpRunAvgAccCap bit field
	 * is extended to 4:31 from 4:25.
	 */
	if (is_carrizo_or_later()) {
		pr_info("fam15h_power: is_carrizo_or_later hit! In %s.\n", __func__); //shouldn't be
		running_avg_capture = val >> 4;
		running_avg_capture = sign_extend32(running_avg_capture, 27);
	} else {
		running_avg_capture = (val >> 4) & 0x3fffff;
		running_avg_capture = sign_extend32(running_avg_capture, 21);
	}

	pr_info("fam15h_power: Current running_avg_capture = %d in %s .\n",
			running_avg_capture, __func__);
	running_avg_range = (val & 0xf) + 1;

	pr_info("fam15h_power: Current running_avg_range = %d in %s .\n",
			running_avg_range, __func__);

	pci_bus_read_config_dword(f4->bus, PCI_DEVFN(PCI_SLOT(f4->devfn), 5),
				  REG_TDP_LIMIT3, &val);
	pr_info("fam15h_power: REG_TDP_LIMIT3 raw value = %#x in %s.\n",
			val, __func__);

	/*
	 * On Carrizo and later platforms, ApmTdpLimit bit field
	 * is extended to 16:31 from 16:28.
	 */
	if (is_carrizo_or_later()) {
		tdp_limit = val >> 16;
		pr_info("fam15h_power: is_carrizo_or_later hit! In %s.\n", __func__); //shouldn't be
	}
	else
		tdp_limit = (val >> 16) & 0x1fff;

	pr_info("fam15h_power: Current tdp_limit= %u in %s .\n",
			tdp_limit, __func__);

	curr_pwr_watts = ((u64)(tdp_limit +
				data->base_tdp)) << running_avg_range;
	curr_pwr_watts -= running_avg_capture;
	curr_pwr_watts *= data->tdp_to_watts;

	pr_info("fam15h_power: curr_pwr_watts = %llu in %s.\n", (unsigned long long)curr_pwr_watts, __func__);
	/*
	 * Convert to microWatt
	 *
	 * power is in Watt provided as fixed point integer with
	 * scaling factor 1/(2^16).  For conversion we use
	 * (10^6)/(2^16) = 15625/(2^10)
	 */
	curr_pwr_watts = (curr_pwr_watts * 15625) >> (10 + running_avg_range);
	return sprintf(buf, "%u\n", (unsigned int) curr_pwr_watts);
}
static DEVICE_ATTR_RO(power1_input);

static ssize_t power1_crit_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct fam15h_power_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", data->processor_pwr_watts);
}
static DEVICE_ATTR_RO(power1_crit);

static void do_read_registers_on_cu(void *_data)
{
	struct fam15h_power_data *data = _data;
	int cu;

	/*
	 * With the new x86 topology modelling, cpu core id actually
	 * is compute unit id.
	 */
	cu = topology_core_id(smp_processor_id());
	pr_info("fam15h_power: Current cu = %d in %s.\n", cu, __func__);

	rdmsrl_safe(MSR_F15H_CU_PWR_ACCUMULATOR, &data->cu_acc_power[cu]);
	pr_info("fam15h_power: Set cu = %d's acc_power to %llu in %s.\n",
			cu, (unsigned long long)(data->cu_acc_power[cu]), __func__);

	rdmsrl_safe(MSR_F15H_PTSC, &data->cpu_sw_pwr_ptsc[cu]);
	pr_info("fam15h_power: Set cu = %d's sw_pwr_ptsc to %llu in %s.\n",
			cu, (unsigned long long)(data->cpu_sw_pwr_ptsc[cu]), __func__);

	data->cu_on[cu] = 1;
}

/*
 * This function is only able to be called when CPUID
 * Fn8000_0007:EDX[12] is set.
 */
static int read_registers(struct fam15h_power_data *data)
{
	int core, this_core;
	cpumask_var_t mask;
	int ret, cpu;

	ret = zalloc_cpumask_var(&mask, GFP_KERNEL);
	if (!ret) {
		pr_info("fam15h_power: returning ENOMEM!\n");
		return -ENOMEM;
	}

	memset(data->cu_on, 0, sizeof(int) * MAX_CUS);

	cpus_read_lock();

	/*
	 * Choose the first online core of each compute unit, and then
	 * read their MSR value of power and ptsc in a single IPI,
	 * because the MSR value of CPU core represent the compute
	 * unit's.
	 */
	core = -1;

	for_each_online_cpu(cpu) {
		this_core = topology_core_id(cpu);

		pr_info("fam15h_power: iter cpu=%d, this_core=%d, core=%d\n", cpu, this_core, core);

		if (this_core == core) {
			pr_info("fam15h_power: skip cpu=%d (duplicate core)\n", cpu);
			continue;
		}
		core = this_core;

		pr_info("fam15h_power: Accepting cpu=%d and its sisters for mask in %s\n",
				cpu, __func__);
		/* get any CPU on this compute unit */
		cpumask_set_cpu(cpumask_any(topology_sibling_cpumask(cpu)), mask);
	}

	pr_info("fam15h_power: read_registers_on_cu on cpu=%d\n", cpu);
	on_each_cpu_mask(mask, do_read_registers_on_cu, data, true);

	cpus_read_unlock();
	free_cpumask_var(mask);

	return 0;
}

static ssize_t power1_average_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct fam15h_power_data *data = dev_get_drvdata(dev);
	u64 prev_cu_acc_power[MAX_CUS], prev_ptsc[MAX_CUS],
	    jdelta[MAX_CUS];
	u64 tdelta, avg_acc;
	int cu, cu_num, ret;
	signed long leftover;

	/*
	 * With the new x86 topology modelling, x86_max_cores is the
	 * compute unit number.
	 */
	cu_num = topology_num_cores_per_package();

	ret = read_registers(data);
	if (ret)
		return 0;

	for (cu = 0; cu < cu_num; cu++) {
		prev_cu_acc_power[cu] = data->cu_acc_power[cu];
		prev_ptsc[cu] = data->cpu_sw_pwr_ptsc[cu];
	}

	leftover = schedule_timeout_interruptible(msecs_to_jiffies(data->power_period));
	if (leftover)
		return 0;

	ret = read_registers(data);
	if (ret)
		return 0;

	for (cu = 0, avg_acc = 0; cu < cu_num; cu++) {
		/* check if current compute unit is online */
		if (data->cu_on[cu] == 0)
			continue;

		if (data->cu_acc_power[cu] < prev_cu_acc_power[cu]) {
			jdelta[cu] = data->max_cu_acc_power + data->cu_acc_power[cu];
			jdelta[cu] -= prev_cu_acc_power[cu];
		} else {
			jdelta[cu] = data->cu_acc_power[cu] - prev_cu_acc_power[cu];
		}
		tdelta = data->cpu_sw_pwr_ptsc[cu] - prev_ptsc[cu];
		jdelta[cu] *= data->cpu_pwr_sample_ratio * 1000;
		do_div(jdelta[cu], tdelta);

		/* the unit is microWatt */
		avg_acc += jdelta[cu];
	}

	return sprintf(buf, "%llu\n", (unsigned long long)avg_acc);
}
static DEVICE_ATTR_RO(power1_average);

static ssize_t power1_average_interval_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct fam15h_power_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "%lu\n", data->power_period);
}

static ssize_t power1_average_interval_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	struct fam15h_power_data *data = dev_get_drvdata(dev);
	unsigned long temp;
	int ret;

	ret = kstrtoul(buf, 10, &temp);
	if (ret)
		return ret;

	if (temp > MAX_INTERVAL)
		return -EINVAL;

	/* the interval value should be greater than 0 */
	if (temp <= 0)
		return -EINVAL;

	data->power_period = temp;

	return count;
}
static DEVICE_ATTR_RW(power1_average_interval);

static int fam15h_power_init_attrs(struct pci_dev *pdev,
				   struct fam15h_power_data *data)
{
	int n = FAM15H_MIN_NUM_ATTRS;
	struct attribute **fam15h_power_attrs;
	struct cpuinfo_x86 *c = &boot_cpu_data;

	/* PS4 Phat/Liverpool Family is 0x16, and models seem to start from 0x10; selected 0x25 as somewhat
	 * arbitrary end point*/
	if ((c->x86 == 0x15 &&
	    (c->x86_model <= 0xf ||
	     (c->x86_model >= 0x60 && c->x86_model <= 0x7f))) ||
	   (c->x86 == 0x99 && (c->x86_model >= 0x10 && c->x86_model <= 0x25))) { //keep ps4 check off for now
		n += 1;
		pr_info("fam15h_power: x86 == 0x15 && x86_model == %#x in %s.\n",
				c->x86_model, __func__);
	}

	/* check if processor supports accumulated power */
	if (boot_cpu_has(X86_FEATURE_ACC_POWER)) {
		n += 2;
		pr_info("fam15h_power: boot_cpu_has accumulated power feature in %s.\n",
				__func__);
	}

	fam15h_power_attrs = devm_kcalloc(&pdev->dev, n,
					  sizeof(*fam15h_power_attrs),
					  GFP_KERNEL);

	if (!fam15h_power_attrs)
		return -ENOMEM;

	n = 0; //why are we setting this to 0? what on Earth? Memory shortage test?? Oh the kcalloc
	fam15h_power_attrs[n++] = &dev_attr_power1_crit.attr;

	/* We can't rely on REG_PROCESSOR_TDP only, as that seems non existent on PS4 Liverpool (0x16).
	 * Rely on power1_input functions to get the missing values */
	if ((c->x86 == 0x15 &&
	    (c->x86_model <= 0xf ||
	     (c->x86_model >= 0x60 && c->x86_model <= 0x7f))) ||
	   (c->x86 == 0x99 && (c->x86_model >= 0x10 && c->x86_model <= 0x25))) {
		fam15h_power_attrs[n++] = &dev_attr_power1_input.attr;
		pr_info("fam15h_power: x86 == 0x15 && x86_model == %#x in %s.\n"
		"Setting power1_input attributes as our fam15h_power_attrs now.\n",
		c->x86_model, __func__);
	}

	if (boot_cpu_has(X86_FEATURE_ACC_POWER)) {
		fam15h_power_attrs[n++] = &dev_attr_power1_average.attr;
		fam15h_power_attrs[n++] = &dev_attr_power1_average_interval.attr;
		pr_info("fam15h_power: boot_cpu_has accumulated power feature in %s.\n",
		__func__);
	}

	data->group.attrs = fam15h_power_attrs;

	pr_info("fam15h_power: Returning 0 in in %s.\n", __func__);
	return 0;
}

static bool should_load_on_this_node(struct pci_dev *f4)
{
	u32 val;

	pci_bus_read_config_dword(f4->bus, PCI_DEVFN(PCI_SLOT(f4->devfn), 3),
				  REG_NORTHBRIDGE_CAP, &val);
	if ((val & BIT(29)) && ((val >> 30) & 3))
		return false;

	return true;
}

/*
 * Newer BKDG versions have an updated recommendation on how to properly
 * initialize the running average range (was: 0xE, now: 0x9). This avoids
 * counter saturations resulting in bogus power readings.
 * We correct this value ourselves to cope with older BIOSes.
 */
static const struct pci_device_id affected_device[] = {
	{ PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_15H_NB_F4) },
	{ 0 }
};

static void tweak_runavg_range(struct pci_dev *pdev)
{
	u32 val;

	/*
	 * let this quirk apply only to the current version of the
	 * northbridge, since future versions may change the behavior
	 */
	// we just return here; we're not the affected device
	//maybe need to be ?? PS4's is AMD_16H_M41H_F4 & F3(temp)
	if (!pci_match_id(affected_device, pdev)) {
		pr_info("fam15h_power: Returning from %s; did not match quirk affected device.\n",
				__func__);
		return;
	}
	pr_info("fam15h_power: Continuing in %s; check me!\n",
				__func__);


	pci_bus_read_config_dword(pdev->bus,
		PCI_DEVFN(PCI_SLOT(pdev->devfn), 5),
		REG_TDP_RUNNING_AVERAGE, &val);
	if ((val & 0xf) != 0xe)
		return;

	val &= ~0xf;
	val |=  0x9;
	pci_bus_write_config_dword(pdev->bus,
		PCI_DEVFN(PCI_SLOT(pdev->devfn), 5),
		REG_TDP_RUNNING_AVERAGE, val);
}

#ifdef CONFIG_PM
static int fam15h_power_resume(struct pci_dev *pdev)
{
	tweak_runavg_range(pdev);
	return 0;
}
#else
#define fam15h_power_resume NULL
#endif

static int fam15h_power_init_data(struct pci_dev *f4,
				  struct fam15h_power_data *data)
{
	u32 val;
	u64 tmp;
	int ret;

#ifdef CONFIG_X86_PS4
	pci_bus_read_config_dword(f4->bus, PCI_DEVFN(PCI_SLOT(f4->devfn), 5),
				REG_PROCESSOR_TDP, &val); //on PS4s, the REG processor TDP seems to live on function 5 though?
#else
	pci_read_config_dword(f4, REG_PROCESSOR_TDP, &val); //pci_dev points to function 4, as this func owns the fam15h_power driver
#endif
	pr_info("fam15h_power: REG_PROCESSOR_TDP raw value = %#x in %s.\n", val, __func__);
	data->base_tdp = val >> 16;
	pr_info("fam15h_power: Set power_data->base_tdp = %#x in %s.\n", data->base_tdp, __func__);
	tmp = val & 0xffff;

	pci_bus_read_config_dword(f4->bus, PCI_DEVFN(PCI_SLOT(f4->devfn), 5),
				  REG_TDP_LIMIT3, &val);
	pr_info("fam15h_power: REG_TDP_LIMIT3 raw value from fn5 = %#x in %s.\n", val, __func__);

	data->tdp_to_watts = ((val & 0x3ff) << 6) | ((val >> 10) & 0x3f);
	pr_info("fam15h_power: Set power_data->tdp_to_watts = %#x in %s.\n", data->tdp_to_watts, __func__);
	tmp *= data->tdp_to_watts;

	/* result not allowed to be >= 256W */
	if ((tmp >> 16) >= 256)
		dev_warn(&f4->dev,
			 "Bogus value for ProcessorPwrWatts (processor_pwr_watts>=%u)\n",
			 (unsigned int) (tmp >> 16));
	
	pr_info("fam15h_power: Current Wattage = %u in %s.\n", (unsigned int) (tmp >> 16), __func__);

	/* convert to microWatt */
	data->processor_pwr_watts = (tmp * 15625) >> 10;

	ret = fam15h_power_init_attrs(f4, data);
	if (ret) {
		pr_info("fam15h_power: return = %d from fam15h_power_init_attrs; exiting function.\n");
		return ret;
	}

	/* CPUID Fn8000_0007:EDX[12] indicates to support accumulated power */
	if (!boot_cpu_has(X86_FEATURE_ACC_POWER)) {
		pr_info("fam15h_power: !boot_cpu_has accumulated power feature in %s, returning 0.\n",
				__func__);
		return 0;
	}

	/*
	 * determine the ratio of the compute unit power accumulator
	 * sample period to the PTSC counter period by executing CPUID
	 * Fn8000_0007:ECX
	 */
	data->cpu_pwr_sample_ratio = cpuid_ecx(0x80000007);

	if (rdmsrl_safe(MSR_F15H_CU_MAX_PWR_ACCUMULATOR, &tmp)) {
		pr_err("Failed to read max compute unit power accumulator MSR\n");
		return -ENODEV;
	}

	pr_info("fam15h_power: Setting power_data->max_cu_acc_power to %llu in %s.\n",
			(unsigned long long)tmp, __func__);
	data->max_cu_acc_power = tmp;

	/*
	 * Milliseconds are a reasonable interval for the measurement.
	 * But it shouldn't set too long here, because several seconds
	 * would cause the read function to hang. So set default
	 * interval as 10 ms.
	 */
	data->power_period = 10;

	return read_registers(data);
}

static int fam15h_power_probe(struct pci_dev *pdev,
			      const struct pci_device_id *id)
{
	struct fam15h_power_data *data;
	struct device *dev = &pdev->dev;
	struct device *hwmon_dev;
	int ret;

	/*
	 * though we ignore every other northbridge, we still have to
	 * do the tweaking on _each_ node in MCM processors as the counters
	 * are working hand-in-hand
	 */
	tweak_runavg_range(pdev);

	if (!should_load_on_this_node(pdev)) {
		pr_info("fam15h_power: Returning ENODEV in %s.\n", __func__);
		return -ENODEV;
	}

	data = devm_kzalloc(dev, sizeof(struct fam15h_power_data), GFP_KERNEL);
	if (!data) {
		pr_info("fam15h_power: Returning ENOMEM in %s.\n", __func__);
		return -ENOMEM;
	}

	ret = fam15h_power_init_data(pdev, data);
	if (ret) {
		pr_info("fam15h_power: Return as expected from fam15h_power_init; exiting func.\n");
		return ret;
	}

	data->pdev = pdev;

	data->groups[0] = &data->group;

	hwmon_dev = devm_hwmon_device_register_with_groups(dev, "fam15h_power",
							   data,
							   &data->groups[0]);
	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct pci_device_id fam15h_power_id_table[] = {
	{ PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_15H_NB_F4) },
	{ PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_15H_M30H_NB_F4) },
	{ PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_15H_M60H_NB_F4) },
	{ PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_15H_M70H_NB_F4) },
	{ PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_16H_NB_F4) },
	{ PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_16H_M30H_NB_F4) },
	{ PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_16H_M41H_F4) },
	{}
};
MODULE_DEVICE_TABLE(pci, fam15h_power_id_table);

static struct pci_driver fam15h_power_driver = {
	.name = "fam15h_power",
	.id_table = fam15h_power_id_table,
	.probe = fam15h_power_probe,
	.resume = fam15h_power_resume,
};

module_pci_driver(fam15h_power_driver);
