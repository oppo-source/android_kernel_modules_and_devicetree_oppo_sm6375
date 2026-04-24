#ifndef __HMBIRD_SCHED__
#define __HMBIRD_SCHED__

#include <linux/sched.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/smp.h>
#include <linux/tick.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/irq_work.h>
#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <linux/sched/clock.h>
#include <uapi/linux/sched/types.h>
#include <../kernel/time/tick-sched.h>
#include <../kernel/sched/sched.h>
#include <kernel/time/tick-sched.h>
#include <trace/hooks/sched.h>
#include "es4g_assist.h"
#ifdef CONFIG_OPLUS_SYSTEM_KERNEL_QCOM
#include <../../../kernel/sched/walt/walt.h>
#include <../../../kernel/sched/walt/trace.h>
#endif
#include <../kernel/oplus_cpu/sched/sched_assist/sa_common.h>

#if IS_ENABLED(CONFIG_QCOM_MINIDUMP)
#include <soc/qcom/minidump.h>
#endif

#ifndef HMBIRD_TICK_HIT_BOOST
#define HMBIRD_TICK_HIT_BOOST   BIT(29)
#endif

#define REGISTER_TRACE_VH(vender_hook, handler) \
{ \
	ret = register_trace_##vender_hook(handler, NULL); \
	if (ret) { \
		pr_err("failed to register_trace_"#vender_hook", ret=%d\n", ret); \
	} \
}

#define HMBIRD_PROC_OPS(name, open_func, write_func) \
	static const struct proc_ops name##_proc_ops = { \
		.proc_open = open_func, \
		.proc_write = write_func, \
		.proc_read = seq_read, \
		.proc_lseek = seq_lseek, \
		.proc_release = single_release, \
	}

#define REGISTER_TRACE_RVH		REGISTER_TRACE_VH

extern unsigned int highres_tick_ctrl;
extern unsigned int highres_tick_ctrl_dbg;
extern int scx_sched_ravg_window;
extern int new_scx_sched_ravg_window;

extern int slim_walt_ctrl;
extern int slim_walt_dump;
extern int slim_walt_policy;
extern int sched_ravg_window_frame_per_sec;
extern int slim_gov_debug;
extern int cpu7_tl;
extern int scx_gov_ctrl;
extern atomic64_t scx_irq_work_lastq_ws;
extern spinlock_t new_sched_ravg_window_lock;
extern int scx_cpufreq_init(void);

#define MAX_YIELD_SLEEP		(2000000ULL)
#define MIN_YIELD_SLEEP		(200000ULL)
#define YIELD_DURATION		(5000ULL)
#define DEFAULT_YIELD_SLEEP_TH	(10)

struct sched_yield_state {
	raw_spinlock_t	lock;
	u64				last_yield_time;
	u64				last_update_time;
	u64				sleep_end;
	unsigned long	yield_cnt;
	unsigned long	yield_cnt_after_sleep;
	unsigned long	sleep;
	int sleep_times;
};

struct yield_opt_params {
	int enable;
	int frame_per_sec;
	u64 frame_time_ns;
	int yield_headroom;
};

struct boost_policy_params {
	int enable;
	unsigned int bottom_freq;
	int boost_weight;
};

struct tick_hit_params {
	int enable;
	unsigned long jiffies_num;
	int hit_count_thres;
};

DECLARE_PER_CPU(struct sched_yield_state, ystate);

#ifdef CONFIG_SCX_USE_UTIL_TRACK
DECLARE_PER_CPU_SHARED_ALIGNED(struct scx_sched_rq_stats, scx_sched_rq_stats);

struct scx_sched_rq_stats {
	u64	window_start;
	u64	latest_clock;
	u32	prev_window_size;
	u64	task_exec_scale;
	u64	prev_runnable_sum;
	u64	curr_runnable_sum;
};

enum task_event {
	PUT_PREV_TASK   = 0,
	PICK_NEXT_TASK  = 1,
	TASK_WAKE       = 2,
	TASK_MIGRATE    = 3,
	TASK_UPDATE     = 4,
	IRQ_UPDATE      = 5,
};

void sched_ravg_window_change(int frame_per_sec);
void run_scx_irq_work_rollover(void);
void scx_update_task_ravg(struct task_struct *p, struct rq *rq,
					int event, u64 wallclock);
void scx_sched_init_task(struct task_struct *p);
#endif
void slim_walt_enable(bool enable);
void hmbird_sysctrl_init(void);
void hmbird_misc_init(void);
int scx_shadow_tick_init(void);

#endif /*__HMBIRD_SCHED__*/
