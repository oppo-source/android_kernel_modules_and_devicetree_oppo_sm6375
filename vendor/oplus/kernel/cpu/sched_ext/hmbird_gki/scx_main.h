// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020-2023 Oplus. All rights reserved.
 */

#ifndef _SCX_SE_H_
#define _SCX_SE_H_

#include <linux/sched.h>
#include <linux/list.h>
#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/cgroup-defs.h>
#include <../kernel/sched/sched.h>
#include <../../../kernel/sched/walt/walt.h>
#include <../kernel/oplus_cpu/sched/sched_assist/sa_common.h>
#include "sched_ext.h"

DECLARE_PER_CPU(struct scx_dispatch_q[MAX_BPF_DSQS], gdsqs);
/*sysctl*/
extern unsigned int dump_info;

#define SCX_DEBUG_FTRACE		(1 << 0)
#define SCX_DEBUG_SYSTRACE		(1 << 1)
#define SCX_DEBUG_PRINTK		(1 << 2)
#define SCX_DEBUG_PANIC			(1 << 3)

#define scx_trace_printk(fmt, ...)	\
do {										\
		trace_printk("scx_sched_ext :"fmt, ##__VA_ARGS__);	\
} while (0)

#define debug_trace_printk(fmt, ...)	\
do {										\
	if (dump_info & SCX_DEBUG_FTRACE)			\
		trace_printk("scx_sched_ext :"fmt, ##__VA_ARGS__);	\
} while (0)

#define debug_printk(fmt, ...)	\
{							\
	if (dump_info & SCX_DEBUG_PRINTK)	\
		printk_deferred("scx_sched_ext[%s]: "fmt, __func__, ##__VA_ARGS__); \
}

#define scx_assert_rq_lock(rq)	\
do {			\
	if (unlikely(!raw_spin_is_locked(&rq->__lock))) { \
		printk_deferred("on CPU%d: %s task %s(%d) unlocked access for" \
			"cpu=%d stack[%pS <== %pS <== %pS]\n",		\
			raw_smp_processor_id(), __func__,		\
			current->comm, current->pid, rq->cpu,             \
			(void *)CALLER_ADDR0, (void *)CALLER_ADDR1,	\
			(void *)CALLER_ADDR2);          \
		BUG_ON(-1);					\
	}	\
} while (0)

#define scx_assert_spin_held(lock)	\
do {			\
	if (unlikely(!raw_spin_is_locked(lock))) { \
		printk_deferred("on CPU%d: %s task %s(%d) unlocked access for" \
			"lock=%s stack[%pS <== %pS <== %pS]\n", \
			raw_smp_processor_id(), __func__,	\
			current->comm, current->pid, #lock,             \
			(void *)CALLER_ADDR0, (void *)CALLER_ADDR1,	\
			(void *)CALLER_ADDR2);          \
		BUG_ON(-1);					\
	}	\
} while (0)

#define SCX_BUG(fmt, ...)		\
do {										\
	printk_deferred("scx_sched_ext[%s]:"fmt, __func__, ##__VA_ARGS__);	\
	if (dump_info & SCX_DEBUG_PANIC)			\
		BUG_ON(-1);								\
} while (0)

#define SCHED_PRINT(arg)	printk_deferred("%s=%llu", #arg, arg)
void scx_task_dump(struct task_struct *p);

#define REGISTER_TRACE(vendor_hook, handler, data, err)	\
do {								\
	ret = register_trace_##vendor_hook(handler, data);	\
	if (ret) {						\
		pr_err("scx_sched_ext:failed to register_trace_"#vendor_hook   \
							", ret = %d\n", ret);  \
		goto err;					\
	}							\
} while (0)

#define UNREGISTER_TRACE(vendor_hook, handler, data)	\
do {								\
	unregister_trace_##vendor_hook(handler, data);				\
} while (0)


#ifdef CONFIG_FAIR_GROUP_SCHED
/* Walk up scheduling entities hierarchy */
#define for_each_sched_entity(se) \
		for (; se; se = se->parent)
#else
#define for_each_sched_entity(se) \
		for (; se; se = NULL)
#endif

enum scene {
	UNKNOWN = 0,
	SGAME = 1,
	GENSHIN = 2,
	WECHAT = 3,
};
extern unsigned int scene_in;

struct scx_dsq_stats {
	u64			cumulative_runnable_avg_scaled;
	int			nr_period_tasks;
	int 			nr_tasks;
};

struct scx_sched_rq_stats {
	u64			window_start;
	u64			latest_clock;
	u32			prev_window_size;
	u64			task_exec_scale;
	u64			prev_runnable_sum;
	u64			curr_runnable_sum;
	int			iso_idx;
	struct scx_dsq_stats 	local_dsq_s;
};

struct scx_iso_masks {
	union {
		struct {
			cpumask_var_t	little;
			cpumask_var_t	big;
			cpumask_var_t	partial;
			cpumask_var_t	exclusive;
		};
		cpumask_var_t	cluster[4];
	};
};
extern struct scx_iso_masks iso_masks;
extern int num_iso_clusters;
DECLARE_PER_CPU(struct scx_sched_rq_stats, scx_sched_rq_stats);
static inline cpumask_t *scx_cpu_iso_cluster(int cpu)
{
	struct scx_sched_rq_stats *srq = &per_cpu(scx_sched_rq_stats, cpu);
	if (srq->iso_idx < 0 || srq->iso_idx >= num_iso_clusters)
		return NULL;

	return iso_masks.cluster[srq->iso_idx];
}

static inline bool scx_cpu_partial(int cpu)
{
	return cpumask_test_cpu(cpu, iso_masks.partial);
}

static inline bool scx_cpu_exclusive(int cpu)
{
	return cpumask_test_cpu(cpu, iso_masks.exclusive);
}

static inline bool scx_cpu_little(int cpu)
{
	return cpumask_test_cpu(cpu, iso_masks.little);
}

static inline bool scx_cpu_big(int cpu)
{
	return cpumask_test_cpu(cpu, iso_masks.big);
}

static inline struct scx_entity *get_oplus_ext_entity(struct task_struct *p)
{
	struct oplus_task_struct *ots = get_oplus_task_struct(p);
	if (!ots) {
		WARN_ONCE(1, "scx_sched_ext:get_oplus_ext_entity NULL!");
		return NULL;
	}
	return &ots->scx;
}

extern atomic_t scx_enter_count;
extern unsigned int scx_stats_trace;
extern void scx_reinit_queue_work(void);
#define SCX_ENABLE_PENDING			(-1)

static inline bool scx_enabled_enter(void)
{
	bool ret = scx_stats_trace;
	if (ret)
		atomic_inc(&scx_enter_count);
	return ret;
}

static inline void scx_enabled_exit(void)
{
	atomic_dec(&scx_enter_count);
}

extern bool scx_clock_suspended;
extern u64 scx_clock_last;
static inline u64 scx_sched_clock(void)
{
	if (unlikely(scx_clock_suspended))
		return scx_clock_last;
	return sched_clock();
}

static inline u64 scx_rq_clock(struct rq *rq)
{
	struct scx_sched_rq_stats *srq = &per_cpu(scx_sched_rq_stats, cpu_of(rq));

	if (unlikely(scx_clock_suspended))
		return scx_clock_last;

	scx_assert_rq_lock(rq);

	if (!(rq->clock_update_flags & RQCF_UPDATED))
		update_rq_clock(rq);

	return max(rq_clock(rq), srq->latest_clock);
}

extern noinline int tracing_mark_write(const char *buf);

/*scx_util_trace*/
extern int scx_sched_ravg_window;
extern int new_scx_sched_ravg_window;
extern spinlock_t new_sched_ravg_window_lock;
extern unsigned int scx_scale_demand_divisor;
extern u64 tick_sched_clock;
extern atomic64_t scx_run_rollover_lastq_ws;
extern u16 balance_small_task_th;
extern u32 balance_small_task_th_runtime;
extern u16 scx_init_load_windows_scaled;
extern u32 scx_init_load_windows;

/*util = runtime * 1024 / window_size */
static inline u64 scx_scale_time_to_util(u64 d)
{
	do_div(d, scx_scale_demand_divisor);
	return d;
}

static inline u32 scx_scale_util_to_time(u16 util)
{
	return util * scx_scale_demand_divisor;
}

/*called while scx_sched_ravg_window changed or init*/
static inline void scx_fixup_window_dep(void)
{
	scx_scale_demand_divisor = scx_sched_ravg_window >> SCHED_CAPACITY_SHIFT;
	balance_small_task_th_runtime = scx_scale_util_to_time(balance_small_task_th);
	scx_init_load_windows_scaled = balance_small_task_th + 1;
	scx_init_load_windows = balance_small_task_th_runtime + 1;
}

u16 scx_cpu_util(int cpu);
static inline unsigned long scx_cpu_load(int cpu)
{
	struct scx_sched_rq_stats *srq = &per_cpu(scx_sched_rq_stats, cpu);
	struct scx_entity *curr_scx = NULL;
	u64 curr_load;
	if (cpu_rq(cpu)->curr) {
		curr_scx = get_oplus_ext_entity(cpu_rq(cpu)->curr);
	}

	curr_load = curr_scx ? curr_scx->sts.demand_scaled : 0;

	return srq->local_dsq_s.cumulative_runnable_avg_scaled + curr_load;
}

static inline int nr_period_tasks(int cpu)
{
	struct scx_sched_rq_stats *srq = &per_cpu(scx_sched_rq_stats, cpu);
	struct scx_entity *curr_scx = NULL;

	if (cpu_rq(cpu)->curr) {
		curr_scx = get_oplus_ext_entity(cpu_rq(cpu)->curr);
	}

	return (curr_scx && (curr_scx->gdsq_idx < NON_PERIOD_START)) ?
				(srq->local_dsq_s.nr_period_tasks + 1)
				: srq->local_dsq_s.nr_period_tasks;
}

/*scx_game*/
#ifdef CONFIG_SCX_GAME_OPT_ENABLE
extern unsigned int scx_boost_ctl;
extern int pid_unitymain;
extern int cpu_unitymain;
extern unsigned int min_freq_ctl;
enum hrtimer_restart scx_update_frame_state(struct task_struct *prev,
					struct task_struct *next, bool virtual);
void scx_init_frame_boost_for_sgame(void);
void scx_exit_frame_boost_for_sgame(void);
void scx_game_init_early(void);
void scx_sgame_tick_update_boost(struct rq *rq);
#endif

/*util_track*/
void scx_update_task_ravg(struct scx_entity *scx,
				struct task_struct *p, struct rq *rq,
				int event, u64 wallclock);
void scx_trace_dispatch_enqueue(struct scx_entity *scx,
				struct task_struct *p, struct rq *rq);
void scx_trace_dispatch_dequeue(struct scx_entity *scx,
				struct task_struct *p, struct rq *rq);
void sched_ravg_window_change(int frame_per_sec);
/*scx_sched_gki*/
extern int partial_enable;
extern unsigned int scx_idle_ctl;
extern unsigned int scx_tick_ctl;
extern unsigned int scx_newidle_balance_ctl;
extern unsigned int scx_exclusive_sync_ctl;

void partial_backup_ctrl(void);
int scx_sched_gki_init_early(void);
void scx_sched_gki_init(void);
void scx_tick_entry(struct rq *rq);
void scx_scheduler_tick(void);
void partial_backup_ctrl(void);
void partial_load_ctrl(struct rq *rq);
int find_idx_from_task(struct task_struct *p);
void scx_smp_call_newidle_balance(int cpu);

/*cpufreq_gov*/
int scx_cpufreq_init(void);
void run_scx_irq_work_rollover(void);
void scx_gov_update_cpufreq(struct cpufreq_policy *policy, u64 prev_runnable_sum);

/*shadow_tick*/
extern unsigned int sysctl_shadow_tick_enable;
int scx_shadow_tick_init(void);
void start_shadow_tick_timer(void);
#endif /* _SCX_SE_H_ */
