#include <linux/sched.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>

#if defined(CONFIG_HMBIRD_SCHED)
extern int sched_ext_init(void);
extern void sched_ext_exit(void);
static int __init common_init(void) {return sched_ext_init();}
static void __exit common_exit(void) {return sched_ext_exit();}
#elif defined(CONFIG_HMBIRD_SCHED_GKI)
extern int scx_init(void);
extern void scx_exit(void);
static int __init common_init(void) {return scx_init();}
static void __exit common_exit(void) {return scx_exit();}
#else
static int __init common_init(void) {return 0;}
static void __exit common_exit(void) {}
#endif



static int __init hmbird_common_init(void)
{
	return common_init();
}

static void __exit hmbird_common_exit(void)
{
	common_exit();
}

module_init(hmbird_common_init);
module_exit(hmbird_common_exit);
MODULE_LICENSE("GPL v2");

