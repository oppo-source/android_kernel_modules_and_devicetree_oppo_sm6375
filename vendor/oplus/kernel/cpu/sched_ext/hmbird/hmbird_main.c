#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>

#include "hmbird_sched.h"
extern atomic_t ext_module_loaded;
int __init sched_ext_init(void)
{
	if (NULL == current->scx)
		return 0;

	scx_shadow_tick_init();
	scx_cpufreq_init();
	hmbird_sysctrl_init();
	hmbird_misc_init();
	atomic_set(&ext_module_loaded, 1);

	return 0;
}


void __exit sched_ext_exit(void)
{
}

