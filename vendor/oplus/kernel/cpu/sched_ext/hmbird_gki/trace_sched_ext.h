/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2019 MediaTek Inc.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM sched_ext

#if !defined(_TRACE_SCHED_EXT_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_SCHED_EXT_H

#include <linux/sched.h>
#include <linux/types.h>
#include <linux/tracepoint.h>

#include "sched_ext.h"
TRACE_EVENT(scx_update_history,

	TP_PROTO(struct scx_entity *scx, struct rq *rq,
		 struct task_struct *p, u32 runtime, int samples, int event),

	TP_ARGS(scx, rq, p, runtime, samples, event),

	TP_STRUCT__entry(
		__array(char,			comm, TASK_COMM_LEN)
		__field(pid_t,			pid)
		__field(unsigned int,		runtime)
		__field(int,			samples)
		__field(int,	event)
		__field(unsigned int,		demand)
		__array(u32,			hist, RAVG_HIST_SIZE)
		__field(u16,			task_util)
		__field(int,			cpu)),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid		= p->pid;
		__entry->runtime	= runtime;
		__entry->samples	= samples;
		__entry->event		= event;
		__entry->demand		= scx->sts.demand;
		memcpy(__entry->hist, scx->sts.sum_history,
					RAVG_HIST_SIZE * sizeof(u32));
		__entry->task_util		= scx->sts.demand_scaled,
		__entry->cpu		= rq->cpu;),

	TP_printk("comm=%s[%d]: runtime %u samples %d event %d demand %u "
		"(hist: %u %u %u %u %u) task_util %u cpu %d",
		__entry->comm, __entry->pid,
		__entry->runtime, __entry->samples,
		__entry->event,
		__entry->demand,
		__entry->hist[0], __entry->hist[1],
		__entry->hist[2], __entry->hist[3],
		__entry->hist[4],
		__entry->task_util,
		__entry->cpu));

DECLARE_EVENT_CLASS(scx_dispatch_template,

	TP_PROTO(struct scx_entity *scx, struct rq *rq,
			struct task_struct *p, unsigned long cpu_load),

	TP_ARGS(scx, rq, p, cpu_load),

	TP_STRUCT__entry(
		__array(char,			comm, TASK_COMM_LEN)
		__field(pid_t,			pid)
		__field(int,			cpu)
		__field(int,			dsq_idx)
		__field(unsigned long,	cpu_load)
		__field(u16,			task_util)),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid		= p->pid;
		__entry->cpu		= rq->cpu;
		__entry->dsq_idx 	= scx->gdsq_idx;
		__entry->cpu_load	= cpu_load;
		__entry->task_util	= scx->sts.demand_scaled;),

	TP_printk("comm=%s[%d]: cpu=%d[dsq_idx=%d], cpu_load=%llu, task_util=%llu",
		__entry->comm, __entry->pid,
		__entry->cpu, __entry->dsq_idx,
		__entry->cpu_load,
		__entry->task_util));

DEFINE_EVENT(scx_dispatch_template, scx_dispatch_enqueue,
	TP_PROTO(struct scx_entity *scx, struct rq *rq,
			struct task_struct *p, unsigned long cpu_load),
	TP_ARGS(scx, rq, p, cpu_load));

DEFINE_EVENT(scx_dispatch_template, scx_dispatch_dequeue,
	TP_PROTO(struct scx_entity *scx, struct rq *rq,
			struct task_struct *p, unsigned long cpu_load),
	TP_ARGS(scx, rq, p, cpu_load));

TRACE_EVENT(scx_run_window_rollover,

	TP_PROTO(u64 old_window_start, u64 new_window_start),

	TP_ARGS(old_window_start, new_window_start),

	TP_STRUCT__entry(
		__field(u64,			old_window_start)
		__field(u64,			new_window_start)
		__field(int,			cpu)),

	TP_fast_assign(
		__entry->old_window_start	= old_window_start;
		__entry->new_window_start	= new_window_start;
		__entry->cpu 			= raw_smp_processor_id();),

	TP_printk("old_window_start=%llu new_window_start=%llu cpu=%d",
		__entry->old_window_start, __entry->new_window_start,
		__entry->cpu));

TRACE_EVENT(scx_update_dsq_timeout,

	TP_PROTO(struct task_struct *p, struct scx_dispatch_q *dsq, u64 runnable_at,
			u64 deadline, u64 duration, bool force_update),

	TP_ARGS(p, dsq, runnable_at, deadline, duration, force_update),

	TP_STRUCT__entry(
		__array(char,		comm, TASK_COMM_LEN)
		__field(pid_t,		pid)
		__field(u64,		runnable_at)
		__field(u64,		deadline)
		__field(u64,		duration)
		__field(int,		cpu)
		__field(bool,		is_timeout)
		__field(bool,		force)),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid		= p->pid;
		__entry->runnable_at	= runnable_at;
		__entry->deadline	= deadline;
		__entry->duration		= duration;
		__entry->cpu		= dsq->id / MAX_BPF_DSQS;
		__entry->is_timeout		= dsq->is_timeout;
		__entry->force		= force_update;),

	TP_printk("comm=%s[%d]: runnable_at=%llu deadline=%llu, duration=%llu, "
		"cpu=%d, timeout=%d, force=%d",
		__entry->comm, __entry->pid,
		__entry->runnable_at, __entry->deadline,
		__entry->duration, __entry->cpu,
		__entry->is_timeout, __entry->force));


TRACE_EVENT(scx_consume_dsq,

	TP_PROTO(struct rq *rq, struct task_struct *p,
		 struct scx_dispatch_q *dsq, u64 runnable_at, int balance_cpu),

	TP_ARGS(rq, p, dsq, runnable_at, balance_cpu),

	TP_STRUCT__entry(
		__array(char,			comm, TASK_COMM_LEN)
		__field(pid_t,			pid)
		__field(u64,			runnable_at)
		__field(int,			dsq_idx)
		__field(u64,			deadline)
		__field(u64,			duration)
		__field(int,			cpu)
		__field(int,			balance_cpu)
		__field(bool,			is_timeout)),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid		= p->pid;
		__entry->runnable_at	= runnable_at;
		__entry->dsq_idx	= dsq->id % MAX_BPF_DSQS;
		__entry->deadline	= msecs_to_jiffies(
					  SCX_BPF_DSQS_DEADLINE[dsq->id % MAX_BPF_DSQS]);
		__entry->duration	= jiffies - runnable_at;
		__entry->cpu		= rq->cpu;
		__entry->balance_cpu	= balance_cpu;
		__entry->is_timeout	= dsq->is_timeout;),

	TP_printk("comm=%s[%d]: runnable_at=%llu, dsq_idx=%d, deadline=%llu, "
		"duration=%llu, cpu=%d, balance_cpu=%d, timeout=%d",
		__entry->comm, __entry->pid,
		__entry->runnable_at, __entry->dsq_idx,
		__entry->deadline, __entry->duration,
		__entry->cpu, __entry->balance_cpu,
		__entry->is_timeout));

TRACE_EVENT(scx_newidle_balance,

	TP_PROTO(int this_cpu, int this_nr_period_tasks,
			int src_cpu, int src_nr_period_tasks_prev,
			int src_nr_period_tasks_now,
			u64 cpu_load_prev, u64 cpu_load_now,
			struct task_struct *pulled_task),

	TP_ARGS(this_cpu, this_nr_period_tasks, src_cpu,
			src_nr_period_tasks_prev, src_nr_period_tasks_now,
			cpu_load_prev, cpu_load_now, pulled_task),

	TP_STRUCT__entry(
		__field(int, 	this_cpu)
		__field(int, 	this_nr_period_tasks)
		__field(int, 	src_cpu)
		__field(int, 	src_nr_period_tasks_prev)
		__field(int, 	src_nr_period_tasks_now)
		__field(u64, 	cpu_load_prev)
		__field(u64, 	cpu_load_now)
		__array(char,	comm, TASK_COMM_LEN)
		__field(pid_t,	pid)),

	TP_fast_assign(
		__entry->this_cpu			= this_cpu;
		__entry->this_nr_period_tasks		= this_nr_period_tasks;
		__entry->src_cpu			= src_cpu;
		__entry->src_nr_period_tasks_prev	= src_nr_period_tasks_prev;
		__entry->src_nr_period_tasks_now	= src_nr_period_tasks_now;
		__entry->cpu_load_prev			= cpu_load_prev;
		__entry->cpu_load_now			= cpu_load_now;
		memcpy(__entry->comm, pulled_task->comm, TASK_COMM_LEN);
		__entry->pid				= pulled_task->pid;),

	TP_printk("this_cpu=%d, nr_period_tasks=%d, src_cpu=%d, nr_period_tasks_prev=%d,"
			"nr_period_tasks_now=%d, cpu_load_prev=%llu, "
			"cpu_load_now=%llu, pulled_task=%s[%d]",
			__entry->this_cpu, __entry->this_nr_period_tasks, __entry->src_cpu,
			__entry->src_nr_period_tasks_prev, __entry->src_nr_period_tasks_now,
			__entry->cpu_load_prev, __entry->cpu_load_now,
			__entry->comm, __entry->pid));

DECLARE_EVENT_CLASS(scx_find_target_cpu_template,

	TP_PROTO(struct task_struct *p, int best_cpu,
			int fastpath, struct cpumask *allowed_mask,
			int partial_enable, int dsq_idx),

	TP_ARGS(p, best_cpu, fastpath, allowed_mask, partial_enable, dsq_idx),

	TP_STRUCT__entry(
		__array(char,	comm, TASK_COMM_LEN)
		__field(pid_t,	pid)
		__field(int, 	best_cpu)
		__field(int, 	fastpath)
		__field(int, 	task_cpus)
		__field(int, 	allowed_mask)
		__field(int, 	partial_enable)
		__field(int, 	dsq_idx)),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid			= p->pid;
		__entry->best_cpu		= best_cpu;
		__entry->fastpath		= fastpath;
		__entry->task_cpus		= cpumask_bits(p->cpus_ptr)[0];
		__entry->allowed_mask		= cpumask_bits(allowed_mask)[0];
		__entry->partial_enable		= partial_enable;
		__entry->dsq_idx		= dsq_idx;),

	TP_printk("comm=%s[%d], best_cpu=%d, fastpath=%d, cpu_allows=0x%x, "
			"allowed_mask=0x%x, partial_enable=%d, dsq_idx=%d",
			__entry->comm, __entry->pid, __entry->best_cpu,
			__entry->fastpath, __entry->task_cpus,
			__entry->allowed_mask, __entry->partial_enable,
			__entry->dsq_idx));

DEFINE_EVENT(scx_find_target_cpu_template, scx_find_target_cpu_fair,
	TP_PROTO(struct task_struct *p, int best_cpu, int fastpath,
		 struct cpumask *allowed_mask, int partial_enable, int dsq_idx),
	TP_ARGS(p, best_cpu, fastpath, allowed_mask, partial_enable, dsq_idx));

DEFINE_EVENT(scx_find_target_cpu_template, scx_find_target_cpu_rt,
	TP_PROTO(struct task_struct *p, int best_cpu, int fastpath, struct cpumask *allowed_mask,
			int partial_enable, int dsq_idx),
	TP_ARGS(p, best_cpu, fastpath, allowed_mask, partial_enable, dsq_idx));

TRACE_EVENT(scx_cfs_check_preempt_wakeup,

	TP_PROTO(struct task_struct *p, int p_idx,
		struct task_struct *curr, int curr_idx, int reason, int preempt),

	TP_ARGS(p, p_idx, curr, curr_idx, reason, preempt),

	TP_STRUCT__entry(
		__array(char,	comm, TASK_COMM_LEN)
		__field(pid_t,	pid)
		__field(int, 	p_idx)
		__array(char,	curr_comm, TASK_COMM_LEN)
		__field(pid_t,	curr_pid)
		__field(int, 	curr_idx)
		__field(int, 	reason)
		__field(int, 	preempt)),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid			= p->pid;
		__entry->p_idx			= p_idx;
		memcpy(__entry->curr_comm, curr->comm, TASK_COMM_LEN);
		__entry->curr_pid		= curr->pid;
		__entry->curr_idx		= curr_idx;
		__entry->reason			= reason;
		__entry->preempt		= preempt;),

	TP_printk("preempt=%d, reason=%d, curr=%s[%d][idx=%d], p=%s[%d][idx=%d]",
			__entry->preempt, __entry->reason,
			__entry->curr_comm, __entry->curr_pid, __entry->curr_idx,
			__entry->comm, __entry->pid, __entry->p_idx));

#endif /*_TRACE_SCHED_EXT_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH ./hmbird_gki

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace_sched_ext
/* This part must be outside protection */
#include <trace/define_trace.h>
