/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_CPUFREQ_H
#define _LINUX_SCHED_CPUFREQ_H

#include <linux/types.h>

/*
 * Interface between cpufreq drivers and the scheduler:
 */

#define SCHED_CPUFREQ_IOWAIT	(1U << 0)

#ifdef CONFIG_CPU_FREQ
struct cpufreq_policy;

struct update_util_data {
       void (*func)(struct update_util_data *data, u64 time, unsigned int flags);
};

void cpufreq_add_update_util_hook(int cpu, struct update_util_data *data,
                       void (*func)(struct update_util_data *data, u64 time,
				    unsigned int flags));
void cpufreq_remove_update_util_hook(int cpu);
bool cpufreq_this_cpu_can_update(struct cpufreq_policy *policy);

static inline unsigned long map_util_freq(unsigned long util,
					unsigned long freq, unsigned long cap)
{
	return freq * util / cap;
}

static inline unsigned long map_util_perf(unsigned long util)
{
	return util + (util >> 2);
}
/* Exported helpers for external cpufreq governors (e.g. loadable modules) */
unsigned long cpufreq_get_capacity_ref_freq(struct cpufreq_policy *policy);
unsigned long sugov_effective_cpu_perf(int cpu, unsigned long actual,
				       unsigned long min, unsigned long max);
void cpufreq_get_effective_util(int cpu, unsigned long boost,
				unsigned long *out_util,
				unsigned long *out_bw_min);
bool cpufreq_cpu_dl_bw_exceeded(int cpu, unsigned long bw_min);
bool cpufreq_cpu_uclamp_capped(int cpu);
bool cpufreq_scx_switched_all(void);
#endif /* CONFIG_CPU_FREQ */

#endif /* _LINUX_SCHED_CPUFREQ_H */
