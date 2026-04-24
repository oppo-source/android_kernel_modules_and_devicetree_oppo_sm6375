#include <linux/sched.h>
#include <../kernel/sched/sched.h>

#include "trace_sched_ext.h"

#include "scx_main.h"
/*Sysctl related interface*/
#define WINDOW_STATS_RECENT		0
#define WINDOW_STATS_MAX		1
#define WINDOW_STATS_MAX_RECENT_AVG	2
#define WINDOW_STATS_AVG		3
#define WINDOW_STATS_INVALID_POLICY	4

#define SCX_SCHED_CAPACITY_SHIFT  10
#define SCHED_ACCOUNT_WAIT_TIME 0

DEFINE_PER_CPU(struct scx_sched_rq_stats, scx_sched_rq_stats);
__read_mostly int scx_sched_ravg_window = 8000000;
int new_scx_sched_ravg_window = 8000000;
DEFINE_SPINLOCK(new_sched_ravg_window_lock);

__read_mostly unsigned int scx_scale_demand_divisor;

atomic64_t scx_run_rollover_lastq_ws;
u64 tick_sched_clock;

static int sched_window_stats_policy = WINDOW_STATS_MAX_RECENT_AVG;

static inline void
fixup_cumulative_runnable_avg(struct scx_entity *scx,
			      struct scx_dsq_stats *stats,
			      s64 demand_scaled_delta)
{
	struct scx_task_stats *sts = &scx->sts;
	s64 cumulative_runnable_avg_scaled =
		stats->cumulative_runnable_avg_scaled + demand_scaled_delta;

	if (cumulative_runnable_avg_scaled < 0) {
		SCX_BUG("on CPU %d task ds=%lu is higher than cra=%llu\n",
			 raw_smp_processor_id(), (unsigned long)sts->demand_scaled,
			 stats->cumulative_runnable_avg_scaled);
		cumulative_runnable_avg_scaled = 0;
	}
	stats->cumulative_runnable_avg_scaled = (u64)cumulative_runnable_avg_scaled;
}

static void __maybe_unused
scx_inc_cumulative_runnable_avg(struct scx_entity *scx,
				struct task_struct *p, struct scx_dsq_stats *sds)
{
	struct scx_task_stats *sts = &scx->sts;

	fixup_cumulative_runnable_avg(scx, sds, sts->demand_scaled);
}

static void
scx_dec_cumulative_runnable_avg(struct scx_entity *scx,
				struct task_struct *p, struct scx_dsq_stats *sds)
{
	struct scx_task_stats *sts = &scx->sts;

	fixup_cumulative_runnable_avg(scx, sds, -(s64)sts->demand_scaled);
}

static void	__maybe_unused
cpu_load_systrace_c(int cpu, u64 cpu_load)
{
	char buf[256];
	snprintf(buf, sizeof(buf), "C|9999|cpu%d_load|%llu\n", cpu, cpu_load);
	tracing_mark_write(buf);
}


void scx_trace_dispatch_enqueue(struct scx_entity *scx,
				struct task_struct *p, struct rq *rq)
{
	struct scx_sched_rq_stats *srq = &per_cpu(scx_sched_rq_stats, cpu_of(rq));
	struct scx_dsq_stats *sds = &srq->local_dsq_s;
	unsigned long cpu_load = 0;

	scx_inc_cumulative_runnable_avg(scx, p, sds);
	if (scx->gdsq_idx < NON_PERIOD_START)
		sds->nr_period_tasks++;

	partial_load_ctrl(rq);

	if (trace_scx_dispatch_enqueue_enabled())
		cpu_load = scx_cpu_load(rq->cpu);

	trace_scx_dispatch_enqueue(scx, rq, p, cpu_load);
}

void scx_trace_dispatch_dequeue(struct scx_entity *scx,
				struct task_struct *p, struct rq *rq)
{
	struct scx_sched_rq_stats *srq = &per_cpu(scx_sched_rq_stats, cpu_of(rq));
	struct scx_dsq_stats *sds = &srq->local_dsq_s;
	unsigned long cpu_load = 0;

	scx_dec_cumulative_runnable_avg(scx, p, sds);

	if (scx->gdsq_idx < NON_PERIOD_START)
		sds->nr_period_tasks--;

	if (trace_scx_dispatch_dequeue_enabled())
		cpu_load = scx_cpu_load(rq->cpu);

	trace_scx_dispatch_dequeue(scx, rq, p, cpu_load);
}

static inline u64 scale_exec_time(u64 delta, struct rq *rq)
{
	struct scx_sched_rq_stats *srq = &per_cpu(scx_sched_rq_stats, cpu_of(rq));

	return (delta * srq->task_exec_scale) >> SCX_SCHED_CAPACITY_SHIFT;
}

static u64 add_to_task_demand(struct scx_entity *scx,
			      struct rq *rq, struct task_struct *p, u64 delta)
{
	struct scx_task_stats *sts = &scx->sts;

	delta = scale_exec_time(delta, rq);
	sts->sum += delta;
	if (unlikely(sts->sum > scx_sched_ravg_window))
		sts->sum = scx_sched_ravg_window;

	return delta;
}


static int
account_busy_for_task_demand(struct rq *rq, struct task_struct *p, int event)
{
	/*
	 * No need to bother updating task demand for the idle task.
	 */
	if (is_idle_task(p))
		return 0;

	/*
	 * When a task is waking up it is completing a segment of non-busy
	 * time. Likewise, if wait time is not treated as busy time, then
	 * when a task begins to run or is migrated, it is not running and
	 * is completing a segment of non-busy time.
	 */
	if (event == TASK_WAKE || (!SCHED_ACCOUNT_WAIT_TIME &&
			 (event == PICK_NEXT_TASK || event == TASK_MIGRATE)))
		return 0;

	/*
	 * The idle exit time is not accounted for the first task _picked_ up to
	 * run on the idle CPU.
	 */
	if (event == PICK_NEXT_TASK && rq->curr == rq->idle)
		return 0;

	/*
	 * TASK_UPDATE can be called on sleeping task, when its moved between
	 * related groups
	 */
	if (event == TASK_UPDATE) {
		if (rq->curr == p)
			return 1;

		return p->on_rq ? SCHED_ACCOUNT_WAIT_TIME : 0;
	}

	return 1;
}

static void rollover_cpu_window(struct rq *rq, bool full_window)
{
	struct scx_sched_rq_stats *srq = &per_cpu(scx_sched_rq_stats, cpu_of(rq));
	u64 curr_sum = srq->curr_runnable_sum;

	if (unlikely(full_window))
		curr_sum = 0;

	srq->prev_runnable_sum = curr_sum;
	srq->curr_runnable_sum = 0;
}

static u64
update_window_start(struct rq *rq, u64 wallclock, int event)
{
	s64 delta;
	int nr_windows;
	bool full_window;

	struct scx_sched_rq_stats *srq = &per_cpu(scx_sched_rq_stats, cpu_of(rq));
	u64 old_window_start = srq->window_start;

	if (wallclock < srq->latest_clock) {
		SCX_BUG("on CPU%d; wallclock=%llu(0x%llx) is lesser "
			"than latest_clock=%llu(0x%llx)",
			rq->cpu, wallclock, wallclock, srq->latest_clock,
			srq->latest_clock);
		wallclock = srq->latest_clock;
	}
	delta = wallclock - srq->window_start;
	if (delta < 0) {
		SCX_BUG("on CPU%d; wallclock=%llu(0x%llx) is lesser "
			"than window_start=%llu(0x%llx)",
			rq->cpu, wallclock, wallclock,
			srq->window_start, srq->window_start);
		delta = 0;
		wallclock = srq->window_start;
	}
	srq->latest_clock = wallclock;
	if (delta < scx_sched_ravg_window)
		return old_window_start;

	nr_windows = div64_u64(delta, scx_sched_ravg_window);
	srq->window_start += (u64)nr_windows * (u64)scx_sched_ravg_window;

	srq->prev_window_size = scx_sched_ravg_window;
	full_window = nr_windows > 1;
	rollover_cpu_window(rq, full_window);

	return old_window_start;
}

#define DIV64_U64_ROUNDUP(X, Y) div64_u64((X) + (Y - 1), Y)

static inline unsigned int get_max_freq(unsigned int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get_raw(cpu);

	return (policy == NULL) ? 0 : policy->cpuinfo.max_freq;
}

static inline unsigned int cpu_cur_freq(int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get_raw(cpu);

	return (policy == NULL) ? 0 : policy->cur;
}

static void
update_task_rq_cpu_cycles(struct task_struct *p, struct rq *rq, int event,
			  u64 wallclock)
{
	int cpu = cpu_of(rq);
	struct scx_sched_rq_stats *srq = &per_cpu(scx_sched_rq_stats, cpu_of(rq));

	srq->task_exec_scale = DIV64_U64_ROUNDUP(cpu_cur_freq(cpu) *
				arch_scale_cpu_capacity(cpu), get_max_freq(cpu));
}

/*real_runtime = sum_task_util * window_size / task_exec_scale*/
static inline u64 __maybe_unused
calc_load_to_time(u64 task_exec_scale, u64 load)
{
	load = load * scx_sched_ravg_window;
	do_div(load, task_exec_scale);
	return load;
}

static inline void __maybe_unused
task_util_update_systrace_c(struct scx_entity *scx, struct task_struct *p)
{
	char buf[256];
	struct scx_task_stats *sts = &scx->sts;
	snprintf(buf, sizeof(buf), "C|%d|Task%d_util|%u\n",
			p->pid, p->pid, sts->demand_scaled);
	tracing_mark_write(buf);
}

/*
 * Called when new window is starting for a task, to record cpu usage over
 * recently concluded window(s). Normally 'samples' should be 1. It can be > 1
 * when, say, a real-time task runs without preemption for several windows at a
 * stretch.
 */
static void update_history(struct scx_entity *scx, struct rq *rq, struct task_struct *p,
			 u32 runtime, int samples, int event)
{
	struct scx_task_stats *sts = &scx->sts;
	u32 *hist = &sts->sum_history[0];
	int i;
	u32 max = 0, avg, demand;
	u64 sum = 0;
	u16 demand_scaled;
	int samples_old = samples;

	/* Ignore windows where task had no activity */
	if (!runtime || is_idle_task(p) || !samples)
		goto done;

	/* Push new 'runtime' value onto stack */
	for (; samples > 0; samples--) {
		hist[sts->cidx] = runtime;
		sts->cidx = ++(sts->cidx) % RAVG_HIST_SIZE;
	}

	for (i = 0; i < RAVG_HIST_SIZE; i++) {
		sum += hist[i];
		if (hist[i] > max)
			max = hist[i];
	}

	sts->sum = 0;
	avg = div64_u64(sum, RAVG_HIST_SIZE);

	switch (sched_window_stats_policy) {
	case WINDOW_STATS_RECENT:
		demand = runtime;
		break;
	case WINDOW_STATS_MAX:
		demand = max;
		break;
	case WINDOW_STATS_AVG:
		demand = avg;
		break;
	default:
		demand = max(avg, runtime);
	}

	demand_scaled = scx_scale_time_to_util(demand);

	sts->demand = demand;
	sts->demand_scaled = demand_scaled;

done:
	trace_scx_update_history(scx, rq, p, runtime, samples_old, event);
	return;
}


static u64
update_task_demand(struct scx_entity *scx, struct task_struct *p, struct rq *rq,
			       int event, u64 wallclock)
{
	struct scx_task_stats *sts = &scx->sts;

	u64 mark_start = sts->mark_start;
	struct scx_sched_rq_stats *srq = &per_cpu(scx_sched_rq_stats, cpu_of(rq));


	u64 delta, window_start = srq->window_start;
	int new_window, nr_full_windows;
	u32 window_size = scx_sched_ravg_window;
	u64 runtime;

	new_window = mark_start < window_start;
	if (!account_busy_for_task_demand(rq, p, event)) {
		if (new_window)
			/*
			 * If the time accounted isn't being accounted as
			 * busy time, and a new window started, only the
			 * previous window need be closed out with the
			 * pre-existing demand. Multiple windows may have
			 * elapsed, but since empty windows are dropped,
			 * it is not necessary to account those.
			 */
			update_history(scx, rq, p, sts->sum, 1, event);
		return 0;
	}

	if (!new_window) {
		/*
		 * The simple case - busy time contained within the existing
		 * window.
		 */
		return add_to_task_demand(scx, rq, p, wallclock - mark_start);
	}

	/*
	 * Busy time spans at least two windows. Temporarily rewind
	 * window_start to first window boundary after mark_start.
	 */
	delta = window_start - mark_start;
	nr_full_windows = div64_u64(delta, window_size);
	window_start -= (u64)nr_full_windows * (u64)window_size;

	/* Process (window_start - mark_start) first */
	runtime = add_to_task_demand(scx, rq, p, window_start - mark_start);

	/* Push new sample(s) into task's demand history */
	update_history(scx, rq, p, sts->sum, 1, event);
	if (nr_full_windows) {
		u64 scaled_window = scale_exec_time(window_size, rq);

		update_history(scx, rq, p, scaled_window, nr_full_windows, event);
		runtime += nr_full_windows * scaled_window;
	}

	/*
	 * Roll window_start back to current to process any remainder
	 * in current window.
	 */
	window_start += (u64)nr_full_windows * (u64)window_size;

	/* Process (wallclock - window_start) next */
	mark_start = window_start;
	runtime += add_to_task_demand(scx, rq, p, wallclock - mark_start);

	return runtime;
}

static DEFINE_PER_CPU(u16, prev_cpu_util);
static inline void cpu_util_update_systrace_c(int cpu)
{
	char buf[256];
	u16 cpu_util = scx_cpu_util(cpu);

	if(cpu_util != per_cpu(prev_cpu_util, cpu)) {
		snprintf(buf, sizeof(buf), "C|9999|Cpu%d_util|%u\n",
						cpu, cpu_util);
		tracing_mark_write(buf);
		per_cpu(prev_cpu_util, cpu) = cpu_util;
	}
}

static inline int account_busy_for_cpu_time(struct rq *rq, struct task_struct *p,
				     int event)
{
	return !is_idle_task(p) && (event == PUT_PREV_TASK || event == TASK_UPDATE);
}


static void update_cpu_busy_time(struct scx_entity *scx,
				 struct task_struct *p, struct rq *rq,
				 int event, u64 wallclock)
{
	int new_window, full_window = 0;
	struct scx_task_stats *sts = &scx->sts;
	u64 mark_start = sts->mark_start;
	struct scx_sched_rq_stats *srq = &per_cpu(scx_sched_rq_stats, cpu_of(rq));
	u64 window_start = srq->window_start;
	u32 window_size = srq->prev_window_size;
	u64 delta;
	u64 *curr_runnable_sum = &srq->curr_runnable_sum;
	u64 *prev_runnable_sum = &srq->prev_runnable_sum;

	/*TODO : assert rq->__lock*/
	new_window = mark_start < window_start;
	if (new_window)
		full_window = (window_start - mark_start) >= window_size;


	if (!account_busy_for_cpu_time(rq, p, event))
		goto done;


	if (!new_window) {
		/*
		 * account_busy_for_cpu_time() = 1 so busy time needs
		 * to be accounted to the current window. No rollover
		 * since we didn't start a new window. An example of this is
		 * when a task starts execution and then sleeps within the
		 * same window.
		 */
		delta = wallclock - mark_start;

		delta = scale_exec_time(delta, rq);
		*curr_runnable_sum += delta;

		goto done;
	}

	/*
	 * situations below this need window rollover,
	 * Rollover of cpu counters (curr/prev_runnable_sum) should have already be done
	 * in update_window_start()
	 *
	 * For task counters curr/prev_window[_cpu] are rolled over in the early part of
	 * this function. If full_window(s) have expired and time since last update needs
	 * to be accounted as busy time, set the prev to a complete window size time, else
	 * add the prev window portion.
	 *
	 * For task curr counters a new window has begun, always assign
	 */

	/*
	 * account_busy_for_cpu_time() = 1 so busy time needs
	 * to be accounted to the current window. A new window
	 * must have been started in udpate_window_start()
	 * If any of these three above conditions are true
	 * then this busy time can't be accounted as irqtime.
	 *
	 * Busy time for the idle task need not be accounted.
	 *
	 * An example of this would be a task that starts execution
	 * and then sleeps once a new window has begun.
	 */

	/*
	 * A full window hasn't elapsed, account partial
	 * contribution to previous completed window.
	 */


	delta = full_window ? scale_exec_time(window_size, rq) :
				scale_exec_time(window_start - mark_start, rq);

	*prev_runnable_sum += delta;

	/* Account piece of busy time in the current window. */
	delta = scale_exec_time(wallclock - window_start, rq);
	*curr_runnable_sum += delta;

done:
	if((dump_info & SCX_DEBUG_SYSTRACE) && new_window)
		cpu_util_update_systrace_c(rq->cpu);
}

static inline void window_rollover_systrace_c(void)
{
	char buf[256];
	static unsigned long window_count;

	window_count += 1;

	snprintf(buf, sizeof(buf), "C|9999|scx_window_rollover|%lu\n", window_count % 2);
	tracing_mark_write(buf);
}

void scx_window_rollover_run_once(u64 old_window_start, struct rq *rq)
{
	u64 result;
	struct scx_sched_rq_stats *srq = &per_cpu(scx_sched_rq_stats, cpu_of(rq));
	u64 new_window_start = srq->window_start;

	if (old_window_start == new_window_start)
		return;

	result = atomic64_cmpxchg(&scx_run_rollover_lastq_ws,
					old_window_start, new_window_start);

	if (result != old_window_start)
		return;
	run_scx_irq_work_rollover();
	partial_backup_ctrl();
	trace_scx_run_window_rollover(old_window_start, new_window_start);
	if (dump_info & SCX_DEBUG_SYSTRACE)
		window_rollover_systrace_c();
}

void scx_update_task_ravg(struct scx_entity *scx, struct task_struct *p,
					struct rq *rq, int event, u64 wallclock)
{
	struct scx_task_stats *sts = &scx->sts;
	struct scx_sched_rq_stats *srq = &per_cpu(scx_sched_rq_stats, cpu_of(rq));
	u64 old_window_start;

	if(!scx_enabled_enter())
		return;

	if(!srq->window_start || sts->mark_start == wallclock)
		goto exit;

	scx_assert_rq_lock(rq);

	old_window_start = update_window_start(rq, wallclock, event);

	if(!sts->window_start)
		sts->window_start = srq->window_start;

	if(!sts->mark_start)
		goto done;

	update_task_rq_cpu_cycles(p, rq, event, wallclock);
	update_task_demand(scx, p, rq, event, wallclock);
	update_cpu_busy_time(scx, p, rq, event, wallclock);

	sts->window_start = srq->window_start;

done:
	sts->mark_start = wallclock;

	if (sts->mark_start > (sts->window_start + scx_sched_ravg_window))
		SCX_BUG("CPU%d: %s task %s(%d)'s ms=%llu is ahead of ws=%llu "
			"by more than 1 window on rq=%d event=%d\n",
			raw_smp_processor_id(), __func__, p->comm, p->pid,
			sts->mark_start, sts->window_start, rq->cpu, event);

	scx_window_rollover_run_once(old_window_start, rq);
exit:
	scx_enabled_exit();
}

u16 scx_cpu_util(int cpu)
{
	u64 prev_runnable_sum;
	struct scx_sched_rq_stats *srq = &per_cpu(scx_sched_rq_stats, cpu);

	prev_runnable_sum = srq->prev_runnable_sum;
	do_div(prev_runnable_sum, srq->prev_window_size >> SCX_SCHED_CAPACITY_SHIFT);

	return (u16)prev_runnable_sum;
}

void sched_ravg_window_change(int frame_per_sec)
{
	unsigned long flags;

	spin_lock_irqsave(&new_sched_ravg_window_lock, flags);
	new_scx_sched_ravg_window = NSEC_PER_SEC / frame_per_sec;
	spin_unlock_irqrestore(&new_sched_ravg_window_lock, flags);
}

