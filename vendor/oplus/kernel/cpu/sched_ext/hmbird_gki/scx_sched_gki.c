#include <trace/hooks/sched.h>
#include "scx_main.h"

#define CREATE_TRACE_POINTS
#include "trace_sched_ext.h"
#include "scx_hooks.h"


#include <linux/tracepoint.h>

/*0(0-31),1(32-63),2(64-95),3(95-99)*/
#define RT_PRIO_TO_IDX(prio)		(prio >> 5)

/*10 * scx_sched_ravg_window / 1024 ~= 80us*/
u16 balance_small_task_th = 10;
u32 balance_small_task_th_runtime;
u16 scx_init_load_windows_scaled;
u32 scx_init_load_windows;

static int cpuctrl_high_ratio = 70;
static int cpuctrl_low_ratio = 40;

static int cpuctrl_high_util;
static int cpuctrl_low_util;

static cpumask_t __maybe_unused scx_cpumask_full = CPU_MASK_ALL;

unsigned int scx_idle_ctl = true;
unsigned int scx_tick_ctl = true;
unsigned int scx_newidle_balance_ctl = true;
unsigned int scx_exclusive_sync_ctl = false;
unsigned int sysctl_shadow_tick_enable = true;

int partial_enable;

static void partial_backup_systrace_c(int partial_enable)
{
	char buf[256];
	snprintf(buf, sizeof(buf), "C|9999|partial_enable|%d\n", partial_enable);
	tracing_mark_write(buf);
}

static DEFINE_PER_CPU(int, prev_tick_gran_state);
void tick_gran_state_systrace_c(unsigned int cpu, int tick_gran_state)
{
	if (per_cpu(prev_tick_gran_state, cpu) != tick_gran_state) {
		char buf[256];

		snprintf(buf, sizeof(buf), "C|9999|Cpu%d_tick_gran_state|%d\n",
				cpu, tick_gran_state);
		tracing_mark_write(buf);
		per_cpu(prev_tick_gran_state, cpu) = tick_gran_state;
	}
}

void scx_nohz_balancer_kick(struct rq *rq, unsigned int *flags, int *done)
{
	if (!scx_stats_trace)
		return;
	*done = 1;
}

void scx_sched_rebalance_domains(void *unused, struct rq *rq, int *continue_balancing)
{
	if (!scx_stats_trace)
		return;

	*continue_balancing = 0;
}

int find_idx_from_task(struct task_struct *p)
{
	int idx;
	int sp_dl;
	struct task_group *tg = p->sched_task_group;
	struct scx_entity *scx = get_oplus_ext_entity(p);

	if (scx) {
		sp_dl = scx->sched_prop & SCHED_PROP_DEADLINE_MASK;
		if (sp_dl) {
			idx = sp_dl;
			goto done;
		}
		if (scx->ext_flags & EXT_FLAG_RT_CHANGED) {
			idx = RT_PRIO_TO_IDX(scx->prio_backup);
			goto done;
		}
	}

	if (tg && tg->css.cgroup) {
		idx = tg->scx_deadline_idx;
	} else {
		idx = DEFAULT_CGROUP_DL_IDX;
		debug_printk("hmbird : task %s tg->css.cgroup = NULL\n", p->comm);
	}
done:
	if (idx < 0 || idx >= MAX_BPF_DSQS) {
		debug_printk("idx error, idx = %d-----\n", idx);
		idx = DEFAULT_CGROUP_DL_IDX;
	}
	return idx;
}

static inline struct task_struct *first_dsq_task_fifo(struct scx_dispatch_q *dsq)
{
	if (!list_empty(&dsq->fifo))
		return ots_to_ts(list_first_entry(&dsq->fifo,
				struct oplus_task_struct, scx.dsq_node.fifo));

	return NULL;
}

static inline struct scx_entity *first_dsq_entity_fifo(struct scx_dispatch_q *dsq)
{
	if (!list_empty(&dsq->fifo))
		return list_first_entry(&dsq->fifo,
						struct scx_entity, dsq_node.fifo);

	return NULL;
}

bool update_dsq_timeout(struct scx_dispatch_q *dsq, int idx, bool force_update)
{
	u64 duration, deadline, deadline_jiffies;
	struct task_struct *first;
	unsigned long flags;
	struct scx_entity *scx;
	/*PERIOD dsq do not care timeout*/
	if (idx < NON_PERIOD_START)
		return false;

	deadline = SCX_BPF_DSQS_DEADLINE[idx];
	raw_spin_lock_irqsave(&dsq->lock, flags);

	if (list_empty(&dsq->fifo)) {
		dsq->is_timeout = false;
		raw_spin_unlock_irqrestore(&dsq->lock, flags);
		return false;
	}

	if (dsq->is_timeout && !force_update) {
		raw_spin_unlock_irqrestore(&dsq->lock, flags);
		return true;
	}

	first = first_dsq_task_fifo(dsq);
	scx = get_oplus_ext_entity(first);

	duration = jiffies - scx->runnable_at;
	deadline_jiffies = msecs_to_jiffies(deadline);

	dsq->is_timeout = (duration <= deadline_jiffies) ? false : true;
	raw_spin_unlock_irqrestore(&dsq->lock, flags);

	trace_scx_update_dsq_timeout(first, dsq, scx->runnable_at,
				     deadline_jiffies, duration, force_update);
	return dsq->is_timeout;
}

void __maybe_unused
scx_watchdog_scan(struct scx_dispatch_q *dsq, int cpu)
{
	struct task_struct *first;
	struct scx_entity *scx;
	u64 duration;
	unsigned long flags;
	raw_spin_lock_irqsave(&dsq->lock, flags);
	if (list_empty(&dsq->fifo)) {
		raw_spin_unlock_irqrestore(&dsq->lock, flags);
		return;
	}

	first = first_dsq_task_fifo(dsq);
	scx = get_oplus_ext_entity(first);
	duration = jiffies - scx->runnable_at;

	if (duration > 10) {
		pr_err("%s[%d], long runnable runnable_at=%llu,"
			" duration=%llu, detected in cpu=%d, dsq=%d\n",
			first->comm, first->pid, scx->runnable_at,
			duration, cpu, scx->gdsq_idx);
	}
	raw_spin_unlock_irqrestore(&dsq->lock, flags);
}

static atomic_t in_scanning;
static u64 scx_lastscan_jiffies;
void scx_scan_timeout(void)
{
	int i, cpu;
	struct scx_dispatch_q *dsq;

	if (!scx_enabled_enter())
		return;

	if (atomic_cmpxchg(&in_scanning, 0, 1))
		goto exit;

	if (jiffies <= scx_lastscan_jiffies) {
		atomic_set(&in_scanning, 0);
		goto exit;
	}
	scx_lastscan_jiffies = jiffies;

	for_each_cpu(cpu, cpu_possible_mask) {
		for (i = NON_PERIOD_START; i < NON_PERIOD_END; i++) {
			dsq = per_cpu_ptr(&gdsqs[i], cpu);
			update_dsq_timeout(dsq, i, false);
		}
	}
	atomic_set(&in_scanning, 0);
exit:
	scx_enabled_exit();
}

static struct scx_dispatch_q* find_dsq_from_task(
			struct scx_entity *scx, struct task_struct *p, int cpu)
{
	int idx;
	struct scx_dispatch_q *dsq;

	idx = find_idx_from_task(p);
	dsq = per_cpu_ptr(&gdsqs[idx], cpu);
	scx->gdsq_idx = idx;
	return dsq;
}

static bool scx_dsq_priq_less(struct rb_node *node_a,
			      const struct rb_node *node_b)
{
	const struct scx_entity *a =
		container_of(node_a, struct scx_entity, dsq_node.priq);
	const struct scx_entity *b =
		container_of(node_b, struct scx_entity, dsq_node.priq);

	return time_before64(a->dsq_vtime, b->dsq_vtime);
}

static void dispatch_enqueue(struct scx_entity *scx,
			     struct scx_dispatch_q *dsq,
			     struct task_struct *p,
			     u64 enq_flags, struct rq* rq)
{
	unsigned long flags;

	scx_assert_rq_lock(rq);
	if (scx->dsq || !list_empty(&scx->dsq_node.fifo)) {
		WARN_ON_ONCE(1);
		return;
	}

	raw_spin_lock_irqsave(&dsq->lock, flags);

	if (enq_flags & SCX_ENQ_DSQ_PRIQ) {
		scx->dsq_flags |= SCX_TASK_DSQ_ON_PRIQ;
		rb_add_cached(&scx->dsq_node.priq, &dsq->priq,
			      scx_dsq_priq_less);
	} else {
		if (enq_flags & (SCX_ENQ_HEAD | SCX_ENQ_PREEMPT)) {
			struct task_struct *first = first_dsq_task_fifo(dsq);
			if (first) {
				struct scx_entity *scx_f = get_oplus_ext_entity(first);
				if (unlikely(!scx_f)) {
					SCX_BUG("task queued in dsq must has scx_entity!\n");
					scx->runnable_at = jiffies;
				} else
					scx->runnable_at = scx_f->runnable_at;
			} else
				scx->runnable_at = jiffies;
			list_add(&scx->dsq_node.fifo, &dsq->fifo);
		}
		else {
			scx->runnable_at = jiffies;
			list_add_tail(&scx->dsq_node.fifo, &dsq->fifo);
		}
	}
	dsq->nr++;
	scx->dsq = dsq;
	scx->sts.sdsq = NULL;
	raw_spin_unlock_irqrestore(&dsq->lock, flags);
	scx_trace_dispatch_enqueue(scx, p, rq);
}

static void do_enqueue_task(struct scx_entity *scx,
			    struct rq *rq, struct task_struct *p, u64 enq_flags)
{
	struct scx_dispatch_q* d;
	scx_assert_rq_lock(rq);

	d = find_dsq_from_task(scx, p, rq->cpu);
	if (!(enq_flags & SCX_ENQ_HEAD) || !scx->slice)
		scx->slice = SCX_SLICE_DFL;
	dispatch_enqueue(scx, d, p, enq_flags, rq);
}

static inline bool scx_task_fair(struct task_struct *p)
{
	return p->prio >= MAX_RT_PRIO && !is_idle_task(p);
}

void enqueue_task_scx(struct rq *rq, struct task_struct *p, int enq_flags)
{
	struct scx_entity *scx;
	struct scx_sched_rq_stats *srq = &per_cpu(scx_sched_rq_stats, cpu_of(rq));
	if (p->cpus_ptr == &scx_cpumask_full) {
		scx_assert_spin_held(&p->pi_lock);
		p->cpus_ptr = &p->cpus_mask;
	}

	if (!scx_enabled_enter())
		return;

	if (!scx_task_fair(p))
		goto exit;
	scx_assert_rq_lock(rq);
	/*ots is NULL*/
	scx = get_oplus_ext_entity(p);
	if (!scx)
		goto exit;

	if (unlikely(scx->flags & SCX_TASK_QUEUED)) {
		scx_task_dump(current);
		scx_task_dump(p);
		SCX_BUG("double enqueue detect!\n");
		goto exit;
	}

	scx->flags |= SCX_TASK_QUEUED;
	srq->local_dsq_s.nr_tasks++;

	if (!scx->sts.demand_scaled) {
		int i;
		/*new task*/
		scx->sts.demand_scaled = scx_init_load_windows_scaled;
		scx->sts.demand = scx_init_load_windows;
		for (i = 0; i < RAVG_HIST_SIZE; ++i)
			scx->sts.sum_history[i] = scx_init_load_windows;
	}

	/*set_user_nice -> dequeue && enqueue when p is on_cpu*/
	if (!task_on_cpu(rq, p))
		do_enqueue_task(scx, rq, p, enq_flags);
exit:
	scx_enabled_exit();
}

static bool task_linked_on_dsq(struct scx_entity *scx)
{
	return !list_empty(&scx->dsq_node.fifo) ||
		!RB_EMPTY_NODE(&scx->dsq_node.priq);
}

static void task_unlink_from_dsq(struct scx_entity *scx,
				 struct scx_dispatch_q *dsq)
{
	if (scx->dsq_flags & SCX_TASK_DSQ_ON_PRIQ) {
		rb_erase_cached(&scx->dsq_node.priq, &dsq->priq);
		RB_CLEAR_NODE(&scx->dsq_node.priq);
		scx->dsq_flags &= ~SCX_TASK_DSQ_ON_PRIQ;
	} else {
		list_del_init(&scx->dsq_node.fifo);
	}
}

static void dispatch_dequeue(struct scx_entity *scx,
			     struct rq *rq, struct task_struct *p)
{
	struct scx_dispatch_q *dsq = scx->dsq;
	bool update_timeout = false;
	int idx = scx->gdsq_idx;
	unsigned long flags;

	scx_assert_rq_lock(rq);

	if (!dsq) {
		WARN_ON_ONCE(task_linked_on_dsq(scx));
		return;
	}
	raw_spin_lock_irqsave(&dsq->lock, flags);

	if (dsq->is_timeout && (scx == first_dsq_entity_fifo(dsq)))
		update_timeout = true;

	WARN_ON_ONCE(!task_linked_on_dsq(scx));
	task_unlink_from_dsq(scx, dsq);
	dsq->nr--;
	scx->dsq = NULL;
	raw_spin_unlock_irqrestore(&dsq->lock, flags);

	if (update_timeout)
		update_dsq_timeout(dsq, idx, true);

	scx_trace_dispatch_dequeue(scx, p, rq);
}

void dequeue_task_scx(struct rq *rq, struct task_struct *p, int deq_flags)
{
	struct scx_sched_rq_stats *srq = &per_cpu(scx_sched_rq_stats, cpu_of(rq));
	struct scx_entity *scx;
	if (!scx_enabled_enter())
		return;
	scx = get_oplus_ext_entity(p);
	if (!scx)
		goto exit;

	if (!(scx->flags & SCX_TASK_QUEUED)) {
		goto exit;
	}

	scx->flags &= ~SCX_TASK_QUEUED;
	if (unlikely(srq->local_dsq_s.nr_tasks <= 0))
		SCX_BUG("nr_tasks less than 0! nr_tasks=%d\n", srq->local_dsq_s.nr_tasks);

	srq->local_dsq_s.nr_tasks--;

	dispatch_dequeue(scx, rq, p);
exit:
	scx_enabled_exit();
}

static void
scx_update_task_runtime(void *unused,
			struct task_struct *curr, u64 delta_exec, u64 vruntime)
{
	struct scx_entity *curr_scx;
	if (!scx_enabled_enter())
		return;
	curr_scx = get_oplus_ext_entity(curr);
	if (!curr_scx)
		goto exit;

	if (curr_scx->slice != SCX_SLICE_INF)
		curr_scx->slice -= min(curr_scx->slice, delta_exec);
exit:
	scx_enabled_exit();
}

bool consume_dispatch_q(struct rq *rq, struct scx_dispatch_q *dsq,
			struct task_struct **next, int balance_cpu)
{
	struct task_struct *p;
	struct oplus_task_struct *ots;
	bool found = false;

	scx_assert_rq_lock(rq);

	if (list_empty(&dsq->fifo) && !rb_first_cached(&dsq->priq))
		return false;

	list_for_each_entry(ots, &dsq->fifo, scx.dsq_node.fifo) {
		p = ots_to_ts(ots);
		if (balance_cpu != -1 && (p->nr_cpus_allowed == 1
				|| p->migration_disabled ||
				ots->scx.sts.sdsq == (void *)1)) {
			continue;
		}
		*next = p;
		found = true;
		ots->scx.sts.sdsq = (void *)1;
		break;
	}

	if (found)
		trace_scx_consume_dsq(rq, p, dsq, ots->scx.runnable_at, balance_cpu);

	return found;
}

static int consume_period_dsq(struct rq *rq, struct task_struct **next, int balance_cpu)
{
	int i;

	for (i = 0; i < NON_PERIOD_START; i++) {
		if (consume_dispatch_q(rq, per_cpu_ptr(&gdsqs[i],
					rq->cpu), next, balance_cpu)) {
			return 1;
		}
	}
	return 0;
}

static int consume_non_period_dsq(struct rq *rq, struct task_struct **next, int balance_cpu)
{
	bool is_timeout;
	unsigned long flags;
	int i;
	struct scx_dispatch_q *dsq;

	for(i = NON_PERIOD_START; i < NON_PERIOD_END; i++) {
		dsq = per_cpu_ptr(&gdsqs[i], rq->cpu);
		raw_spin_lock_irqsave(&dsq->lock, flags);
		is_timeout = dsq->is_timeout;
		raw_spin_unlock_irqrestore(&dsq->lock, flags);
		if (is_timeout) {
			if (consume_dispatch_q(rq, dsq, next, balance_cpu))
				return 1;
		}
	}

	return 0;
}

static int scx_pick_next_task(struct rq *rq, struct task_struct **next, int balance_cpu)
{
	scx_assert_rq_lock(rq);
	if (consume_non_period_dsq(rq, next, balance_cpu))
		return 1;

	if (consume_period_dsq(rq, next, balance_cpu))
		return 1;

	return 0;
}

void scx_replace_deadline_task_fair(struct rq *rq, struct task_struct **p,
				    struct sched_entity **se, bool *repick, bool simple)
{
	struct task_struct *next = NULL;

	if (!rq || !p || !se)
		return;
retry:
	if (scx_pick_next_task(rq, &next, -1)) {
		/*
		 * new task cpu must equals to this cpu, or is_same_group return null,
		 * it will cause stability issue in pick_next_task_fair()
		 */
		if (unlikely(task_cpu(next) != rq->cpu)) {
			pr_err("scx_sched_ext:cpu%d replace task failed, "
				"task cpu%d\n", cpu_of(rq), task_cpu(next));
			dequeue_task_scx(rq, next, 0);
			goto retry;
		}
		*p = next;
		*se = &next->se;
		*repick = true;
	}
}

static inline void put_prev_task_scx(struct scx_entity *prev_scx,
				     struct rq *rq, struct task_struct *prev)
{
	if (prev_scx->flags & SCX_TASK_QUEUED) {
		do_enqueue_task(prev_scx,
				rq, prev, prev_scx->slice ? SCX_ENQ_HEAD : 0);
	}
}

static inline void set_next_task_scx(struct scx_entity *next_scx,
				     struct rq *rq, struct task_struct *next)
{
	if (next_scx->flags & SCX_TASK_QUEUED) {
		dispatch_dequeue(next_scx, rq, next);
	}
}

static DEFINE_PER_CPU(int, prev_sched_state);
void scx_sched_state_systrace_c(unsigned int cpu, struct task_struct *p)
{
	int idx = find_idx_from_task(p);

	if (per_cpu(prev_sched_state, cpu) != idx) {
		char buf[256];

		snprintf(buf, sizeof(buf), "C|9999|Cpu%d_sched_prop|%d\n", cpu, idx);
		tracing_mark_write(buf);
		per_cpu(prev_sched_state, cpu) = idx;
	}
}

void scx_schedule(struct task_struct *prev, struct task_struct *next, struct rq *rq)
{
	u64 wallclock;
	struct scx_entity *prev_scx, *next_scx;
	if(!scx_enabled_enter())
		return;

	wallclock = scx_rq_clock(rq);
	prev_scx = get_oplus_ext_entity(prev);
	if (likely(prev != next)) {
		next_scx = get_oplus_ext_entity(next);
#ifdef CONFIG_SCX_GAME_OPT_ENABLE
		if (scene_in == SGAME)
			scx_update_frame_state(prev, next, false);
#endif
		if (prev_scx) {
			scx_update_task_ravg(prev_scx,
					     prev, rq, PUT_PREV_TASK, wallclock);
			put_prev_task_scx(prev_scx, rq, prev);
		}

		if (next_scx) {
			set_next_task_scx(next_scx, rq, next);
			scx_update_task_ravg(next_scx,
					     next, rq, PICK_NEXT_TASK, wallclock);
		}
		if (dump_info & SCX_DEBUG_SYSTRACE) {
			scx_sched_state_systrace_c(rq->cpu, next);
		}
	} else if (prev_scx) {
		scx_update_task_ravg(prev_scx, prev, rq, TASK_UPDATE, wallclock);
	}
	scx_enabled_exit();
}

void scx_tick_entry(struct rq *rq)
{
	struct scx_entity *curr_scx;
	if(!scx_enabled_enter())
		return;
	curr_scx = get_oplus_ext_entity(rq->curr);
	if (curr_scx)
		scx_update_task_ravg(curr_scx, rq->curr, rq, TASK_UPDATE, scx_rq_clock(rq));

	if (!scx_cpu_exclusive(rq->cpu))
		scx_scan_timeout();
	if (dump_info & SCX_DEBUG_SYSTRACE)
		tick_gran_state_systrace_c(rq->cpu, 0);
	scx_enabled_exit();
}

extern void set_next_entity(struct cfs_rq *cfs_rq, struct sched_entity *se);

void scx_replace_next_task_fair(struct rq *rq, struct task_struct **p,
				struct sched_entity **se, bool *repick,
				bool simple, struct task_struct *prev)
{
	if (!scx_enabled_enter())
		return;

	scx_replace_deadline_task_fair(rq, p, se, repick, simple);
	/*
	* NOTE:
	* Because the following code is not merged in kernel-5.15,
	* set_next_entity() will no longer be called to remove the
	* task from the red-black tree when pick_next_task_fair(),
	* so we remove the picked task here.
	*
	* https://android-review.googlesource.com/c/kernel/common/+/1667002
	*/
	if (simple && true == *repick) {
		for_each_sched_entity((*se)) {
			struct cfs_rq *cfs_rq = cfs_rq_of(*se);
			set_next_entity(cfs_rq, *se);
		}
	}
	scx_enabled_exit();
}

static inline bool scx_is_tiny_task(struct task_struct *p)
{
	struct scx_entity *scx;
	if (!p)
		return false;
	scx = get_oplus_ext_entity(p);
	if (!scx)
		return false;

	return scx->sts.demand_scaled <= balance_small_task_th;
}

static DEFINE_PER_CPU(call_single_data_t, scx_nib_csd);

void scx_smp_call_newidle_balance(int cpu)
{
	call_single_data_t *csd = &per_cpu(scx_nib_csd, cpu);

	if (!scx_enabled_enter())
		return;

	smp_call_function_single_async(cpu, csd);
	scx_enabled_exit();
}

/*newidle balance*/
void scx_newidle_balance(struct rq *this_rq,
  			 struct rq_flags *rf, int *pulled_task,
			int *done, bool partial_force)
{
	cpumask_t *cluster = NULL;
	struct scx_sched_rq_stats *src_srq, *this_srq;
	struct scx_entity *scx;
	struct rq *src_rq;
	struct task_struct *pull_task = NULL, *p;
	int src_cpu, this_cpu = this_rq->cpu;

	int src_nr_period_tasks_debug_prev = -1,
		src_nr_period_tasks_debug_now = -1, this_nr_period_tasks_debug = -1;
	u64 src_cpu_load_debug_prev = 0, src_cpu_load_debug_now = 0;

	if (!scx_enabled_enter())
		return;
	*done = 1;
	*pulled_task = 0;

	if (!scx_newidle_balance_ctl)
		goto exit;

	if (scx_cpu_exclusive(this_cpu) || (scx_cpu_partial(this_cpu) && !partial_force))
		goto exit;

	this_srq = &per_cpu(scx_sched_rq_stats, this_cpu);

	if (partial_force) {
		if (!cpumask_empty(iso_masks.big))
			cluster = scx_cpu_iso_cluster(cpumask_first(iso_masks.big));
	} else
		cluster = scx_cpu_iso_cluster(this_cpu);

	if (!cluster)
		goto exit;

	rq_unpin_lock(this_rq, rf);

	for_each_cpu(src_cpu, cluster) {
		if (src_cpu == this_cpu)
			continue;

		src_rq = cpu_rq(src_cpu);
		if (src_rq->nr_running < 2 ||
			(src_rq->nr_running == 2 && scx_is_tiny_task(src_rq->curr)))
			continue;

		src_srq = &per_cpu(scx_sched_rq_stats, src_cpu);

		double_lock_balance(this_rq, src_rq);
		/*
		 * Since we have released rq_lock, check nr_running again,
		 * there may be a task enqueued. If there is a task that
		 * can be repicked at this time, we need to set pulled_task,
		 * otherwise the enqueued task may not be scheduled
		 */
		if (this_rq->nr_running) {
			if (this_rq->cfs.h_nr_running)
				*pulled_task = 1;

			if (this_rq->nr_running != this_rq->cfs.h_nr_running)
				*pulled_task = -1;
			double_unlock_balance(this_rq, src_rq);
			goto repin;
		}

		if (trace_scx_newidle_balance_enabled()) {
			src_nr_period_tasks_debug_prev = src_srq->local_dsq_s.nr_period_tasks;
			src_cpu_load_debug_prev = scx_cpu_load(src_cpu);
		}

		if (scx_pick_next_task(src_rq, &pull_task, this_cpu)) {
			goto pull;
		}

		if (partial_force) {
			double_unlock_balance(this_rq, src_rq);
			continue;
		}

		list_for_each_entry_reverse(p, &src_rq->cfs_tasks, se.group_node) {
			scx = get_oplus_ext_entity(p);

			if (!scx || p->nr_cpus_allowed == 1
				 || p->migration_disabled
				 || task_on_cpu(src_rq, p)
				 || scx->gdsq_idx < NON_PERIOD_START)
				continue;

			if (scx->sts.sdsq)
				continue;

			pull_task = p;
			break;
		}
		if (!pull_task) {
			double_unlock_balance(this_rq, src_rq);
			continue;
		}

pull:
		deactivate_task(src_rq, pull_task, 0);
		set_task_cpu(pull_task, this_cpu);
		activate_task(this_rq, pull_task, 0);

		if (trace_scx_newidle_balance_enabled()) {
			src_cpu_load_debug_now = scx_cpu_load(src_cpu);
			src_nr_period_tasks_debug_now =
					src_srq->local_dsq_s.nr_period_tasks;
			this_nr_period_tasks_debug =
					this_srq->local_dsq_s.nr_period_tasks;
		}

		double_unlock_balance(this_rq, src_rq);
		*pulled_task = 1;
		break;
	}

repin:
	if (*pulled_task)
		this_rq->idle_stamp = 0;
	rq_repin_lock(this_rq, rf);

	if (pull_task)
		trace_scx_newidle_balance(this_cpu,
				this_nr_period_tasks_debug, src_cpu,
				src_nr_period_tasks_debug_prev,
				src_nr_period_tasks_debug_now,
				src_cpu_load_debug_prev,
				src_cpu_load_debug_now, pull_task);
exit:
	scx_enabled_exit();
}

void scx_smp_newidle_balance(void *ignored)
{
	int cpu = raw_smp_processor_id();
	struct rq *rq = cpu_rq(cpu);
	struct rq_flags rf;
	int pulled_task;
	int done = 0;

	rq_lock(rq, &rf);
	update_rq_clock(rq);
	scx_newidle_balance(rq, &rf, &pulled_task, &done, true);
	resched_curr(rq);
	rq_unlock(rq, &rf);
}

enum fastpaths {
	NONE = 0,
	SYNC_WAKEUP,
	PREV_CPU_FASTPATH,
	PIPELINE_FASTPATH,
    CPU_AFFINITY_ONE,
    CPU_AFFINITY_EXCLUSIVE,
    CPU_AFFINITY_PARTIAL,
	CPU_AFFINITY_LITTLE,
    SELECT_IDLE,
    SELECT_NR_LEAST
};

static void scx_select_aware_wake_cpu_nrrunning(struct task_struct *task,
						struct cpumask *target_mask,
						int *best_cpu,
						struct cpumask *target_mask2,
						bool period_task)
{
	int least_nr_cpu = -1, i, nr;
	unsigned int cpu_rq_runnable_cnt = UINT_MAX;
	/*TODO : if can use global percpu cpumask buf*/
	struct cpumask allowed_mask = { CPU_BITS_NONE };
	if (!target_mask2)
		cpumask_and(&allowed_mask, task->cpus_ptr, target_mask);
	else
		cpumask_and(&allowed_mask, target_mask2, target_mask);

	for_each_cpu(i, &allowed_mask) {
		if (available_idle_cpu(i)) {
			*best_cpu = i;
			return;
		}
		if (!period_task) {
			if (cpu_rq(i)->nr_running < cpu_rq_runnable_cnt) {
				cpu_rq_runnable_cnt = cpu_rq(i)->nr_running;
				least_nr_cpu = i;
			} else if (cpu_rq(i)->nr_running == cpu_rq_runnable_cnt) {
				if (nr_period_tasks(i) < nr_period_tasks(least_nr_cpu)) {
					least_nr_cpu = i;
				}
			}
		} else {
			nr = nr_period_tasks(i);
			if (nr < cpu_rq_runnable_cnt) {
				cpu_rq_runnable_cnt = nr;
				least_nr_cpu = i;
			} else if (nr == cpu_rq_runnable_cnt) {
				if (cpu_rq(i)->nr_running <
					cpu_rq(least_nr_cpu)->nr_running) {
					least_nr_cpu = i;
				}
			}
		}
	}

	if (least_nr_cpu != -1)
		*best_cpu = least_nr_cpu;
}

static inline int scx_get_task_pipline_cpu(struct task_struct *p)
{
	int cpu = -1;
	trace_android_vh_scx_select_cpu_dfl(p, &cpu);
	return cpu;
}
EXPORT_TRACEPOINT_SYMBOL_GPL(android_vh_scx_select_cpu_dfl);

static DEFINE_PER_CPU(cpumask_t, energy_cpus);
int scx_find_energy_efficient_cpu(struct task_struct *p, int prev_cpu,
		int sync)
{
	int fastpath = NONE;
	int skip_min = false;
	int best_energy_cpu = prev_cpu, cpu = smp_processor_id();
	int pipeline_cpu = -1;
	int dsq_idx = find_idx_from_task(p);
	struct cpumask *allowed_mask = this_cpu_ptr(&energy_cpus);

	rcu_read_lock();

	skip_min = (dsq_idx < NON_PERIOD_START);

	pipeline_cpu = scx_get_task_pipline_cpu(p);
	if (pipeline_cpu != -1) {
		if (cpumask_test_cpu(pipeline_cpu, p->cpus_ptr) &&
				cpu_active(pipeline_cpu)) {
			best_energy_cpu = pipeline_cpu;
			fastpath = PIPELINE_FASTPATH;
			goto out;
		}
	}

	if (sync && ((skip_min && scx_cpu_little(cpu)) ||
				(scx_exclusive_sync_ctl && scx_cpu_exclusive(cpu))))
		sync = 0;

	if (sync && !(scx_cpu_partial(cpu) && !partial_enable)) {
		best_energy_cpu = cpu;
		fastpath = SYNC_WAKEUP;
		goto out;
	}

	if (p->nr_cpus_allowed == 1) {
		best_energy_cpu = cpumask_any(p->cpus_ptr);
		fastpath = CPU_AFFINITY_ONE;
		goto out;
	}

	if (available_idle_cpu(prev_cpu) && !(scx_cpu_partial(prev_cpu)
			&& !partial_enable) && !scx_cpu_exclusive(prev_cpu)
			&& !(skip_min && scx_cpu_little(prev_cpu))
			&& cpu_active(prev_cpu)) {
		best_energy_cpu = prev_cpu;
		fastpath = PREV_CPU_FASTPATH;
		goto out;
	}

	cpumask_andnot(allowed_mask, p->cpus_ptr, iso_masks.exclusive);

	if (cpumask_empty(allowed_mask)) {
		scx_select_aware_wake_cpu_nrrunning(p,
					iso_masks.exclusive,
					&best_energy_cpu, NULL, skip_min);
		fastpath = CPU_AFFINITY_EXCLUSIVE;
		goto out;
	}

	if (skip_min) {
		cpumask_andnot(allowed_mask, allowed_mask, iso_masks.little);
		if (cpumask_empty(allowed_mask)) {
			scx_select_aware_wake_cpu_nrrunning(p,
				iso_masks.little, &best_energy_cpu, NULL, skip_min);
			fastpath = CPU_AFFINITY_LITTLE;
			goto out;
		}
	}

	if (!partial_enable) {
		cpumask_andnot(allowed_mask, allowed_mask, iso_masks.partial);
		if (cpumask_empty(allowed_mask)) {
			scx_select_aware_wake_cpu_nrrunning(p,
				iso_masks.partial, &best_energy_cpu, NULL, skip_min);
			fastpath = CPU_AFFINITY_PARTIAL;
			goto out;
		}
	}

	scx_select_aware_wake_cpu_nrrunning(p, allowed_mask,
					&best_energy_cpu, NULL, skip_min);
	fastpath = SELECT_NR_LEAST;

out:
	rcu_read_unlock();
	trace_scx_find_target_cpu_fair(p, best_energy_cpu,
				fastpath, allowed_mask, partial_enable, dsq_idx);
	if (best_energy_cpu < 0 || best_energy_cpu >= nr_cpu_ids)
		best_energy_cpu = prev_cpu;

	return best_energy_cpu;
}

void scx_select_task_rq_fair(struct task_struct *p,
				int *target_cpu, int wake_flags, int prev_cpu)
{
	int sync;

	if (!scx_enabled_enter())
		return;

	if ((wake_flags & (WF_TTWU | WF_FORK)) && (p->cpus_ptr == &p->cpus_mask)
				&& (p->nr_cpus_allowed > 1)) {
		p->cpus_ptr = &scx_cpumask_full;
	}

	sync = (wake_flags & WF_SYNC) && !(current->flags & PF_EXITING);

	*target_cpu = scx_find_energy_efficient_cpu(p, prev_cpu, sync);
	scx_enabled_exit();
}

bool scx_should_honor_rt_sync(struct rq *rq, struct task_struct *p)
{
	/*TODO*/
	if (cpumask_test_cpu(rq->cpu, iso_masks.partial) && !partial_enable)
		return false;

	if (cpumask_test_cpu(rq->cpu, iso_masks.exclusive))
		return false;

	return true;
}

void scx_rt_find_lowest_rq(struct task_struct *task,
				   struct cpumask *lowest_mask, int ret, int *best_cpu)
{
	/*TODO : if can use global percpu cpumask buf*/
	struct cpumask allowed_mask = { CPU_BITS_NONE };
	int reason, target = -1;
	int dsq_idx, skip_min;
	if (!ret)
		return;

	if (!scx_enabled_enter())
		return;

	dsq_idx = find_idx_from_task(task);
	skip_min = dsq_idx < NON_PERIOD_START;
	cpumask_andnot(&allowed_mask, lowest_mask, iso_masks.exclusive);

	if (cpumask_empty(&allowed_mask)) {
		scx_select_aware_wake_cpu_nrrunning(task,
			iso_masks.exclusive, &target, lowest_mask, skip_min);
		reason = CPU_AFFINITY_EXCLUSIVE;
		goto out;
	}

	if (!partial_enable) {
		cpumask_andnot(&allowed_mask, &allowed_mask, iso_masks.partial);
		if (cpumask_empty(&allowed_mask)) {
			scx_select_aware_wake_cpu_nrrunning(task,
				iso_masks.partial, &target, lowest_mask, skip_min);
			reason = CPU_AFFINITY_PARTIAL;
			goto out;
		}
	}

	scx_select_aware_wake_cpu_nrrunning(task,
				&allowed_mask, &target, NULL, skip_min);
	reason = SELECT_NR_LEAST;
out:
	trace_scx_find_target_cpu_rt(task, target,
				reason, &allowed_mask, partial_enable, dsq_idx);
	if (target < nr_cpu_ids && target != -1)
		*best_cpu = target;
	scx_enabled_exit();
}

#ifdef CONFIG_UCLAMP_TASK
static inline bool scx_rt_task_fits_capacity(struct task_struct *p, int cpu)
{
	unsigned int min_cap;
	unsigned int max_cap;
	unsigned int cpu_cap;

	min_cap = uclamp_eff_value(p, UCLAMP_MIN);
	max_cap = uclamp_eff_value(p, UCLAMP_MAX);

	cpu_cap = capacity_orig_of(cpu);

	return cpu_cap >= min(min_cap, max_cap);
}
#else
static inline bool scx_rt_task_fits_capacity(struct task_struct *p, int cpu)
{
	return true;
}
#endif

static DEFINE_PER_CPU(cpumask_var_t, scx_local_cpu_mask);
void scx_select_task_rq_rt(struct task_struct *task, int cpu,
					int sd_flag, int wake_flags, int *new_cpu)
{
	bool sync = !!(wake_flags & WF_SYNC);
	struct rq *rq, *this_cpu_rq;
	int this_cpu;
	struct task_struct *curr;
	struct cpumask *lowest_mask = NULL;

	int ret, reason, target = -1;

	if (!scx_enabled_enter())
		return;

	/* For anything but wake ups, just return the task_cpu */
	if (sd_flag != SD_BALANCE_WAKE && sd_flag != SD_BALANCE_FORK) {
		reason = NONE;
		goto out;
	}

	if ((task->cpus_ptr == &task->cpus_mask)
				&& (task->nr_cpus_allowed > 1)) {
		task->cpus_ptr = &scx_cpumask_full;
	}
	rq = cpu_rq(cpu);

	rcu_read_lock();
	curr = READ_ONCE(rq->curr);
	this_cpu = raw_smp_processor_id();
	this_cpu_rq = cpu_rq(this_cpu);

	if (cpu_active(this_cpu) && cpumask_test_cpu(this_cpu, task->cpus_ptr)
		&& sync && scx_should_honor_rt_sync(this_cpu_rq, task)) {
		reason = SYNC_WAKEUP;
		*new_cpu = this_cpu;
		goto out;
	}

	*new_cpu = cpu;
	lowest_mask = this_cpu_cpumask_var_ptr(scx_local_cpu_mask);

	ret = cpupri_find_fitness(&task_rq(task)->rd->cpupri,
				task, lowest_mask, scx_rt_task_fits_capacity);

	if (!ret) {
		reason = NONE;
		goto out;
	}

	scx_rt_find_lowest_rq(task, lowest_mask, ret, &target);

	if (target != -1)
		*new_cpu = target;
out:
	rcu_read_unlock();
	scx_enabled_exit();
}

enum {
	USR_HINT,
	TOP_GAME_THREAD,
	HIGH_PERIOD_TASK,
	TIMEOUT,
};

void scx_cfs_check_preempt_wakeup(struct rq *rq,
			struct task_struct *p, bool *preempt, bool *nopreempt)
{
	int reason = -1, p_idx = -1, curr_idx = -1, check_result = -1;

	if (!scx_enabled_enter())
		return;
	trace_android_vh_check_preempt_curr_scx(rq, p, 0, &check_result);

	if (check_result > 0) {
		*preempt = true;
		reason = USR_HINT;
		goto preempt;
	} else if (!check_result) {
		*nopreempt = true;
		reason = USR_HINT;
		goto nopreempt;
	}

#ifdef CONFIG_SCX_GAME_OPT_ENABLE
	if (sched_prop_get_top_thread_id(p)) {
		*preempt = true;
		reason = TOP_GAME_THREAD;
		goto preempt;
	}
#endif
	curr_idx = find_idx_from_task(rq->curr);
	p_idx = find_idx_from_task(p);

	if ((curr_idx < NON_PERIOD_START) || curr_idx < p_idx) {
		*nopreempt = true;
		reason = HIGH_PERIOD_TASK;
		goto nopreempt;
	}

	if (per_cpu_ptr(&gdsqs[curr_idx], rq->cpu)->is_timeout) {
		*nopreempt = true;
		reason = TIMEOUT;
		goto nopreempt;
	}

	if (p_idx < NON_PERIOD_START && curr_idx >= NON_PERIOD_START) {
		*preempt = true;
		reason = HIGH_PERIOD_TASK;
		goto preempt;
	}
	scx_enabled_exit();
	return;
nopreempt:
	trace_scx_cfs_check_preempt_wakeup(p, p_idx, rq->curr, curr_idx, reason, 0);
	scx_enabled_exit();
	return;
preempt:
	trace_scx_cfs_check_preempt_wakeup(p, p_idx, rq->curr, curr_idx, reason, 1);
	scx_enabled_exit();
}
EXPORT_TRACEPOINT_SYMBOL_GPL(android_vh_check_preempt_curr_scx);

int scx_sched_lpm_disallowed_time(int cpu, u64 *timeout)
{
	int timeout_allowed = 0;
	if(!scx_stats_trace || !scx_idle_ctl)
		return -EAGAIN;

	trace_android_vh_scx_sched_lpm_disallowed_time(cpu, &timeout_allowed);
	if (timeout_allowed)
		goto disallow;

	if (!cpumask_test_cpu(cpu, iso_masks.exclusive))
		return -EAGAIN;

disallow:
	*timeout = 0;
	return 0;
}
EXPORT_TRACEPOINT_SYMBOL_GPL(android_vh_scx_sched_lpm_disallowed_time);

/*Called when iso.big cluster window rollover && Prevent repeated detection of the same window*/
void partial_backup_ctrl(void)
{
	int cpu;
	u64 util;
	u64 max = 0;
	if (cpumask_empty(iso_masks.big))
		return;

	for_each_cpu(cpu, iso_masks.big) {
		util = scx_cpu_util(cpu);
		if (util > max)
			max = util;
	}

	if (!partial_enable && (max >= cpuctrl_high_util)) {
		partial_enable = true;
		if (dump_info & SCX_DEBUG_SYSTRACE)
			partial_backup_systrace_c(1);
	} else if (partial_enable && (max <= cpuctrl_low_util)) {
		partial_enable = false;
		if (dump_info & SCX_DEBUG_SYSTRACE)
			partial_backup_systrace_c(0);
	} else {}
}

/*Called when iso.big cpu load update*/
void partial_load_ctrl(struct rq *rq)
{
	u64 load;
	if (cpumask_test_cpu(rq->cpu, iso_masks.big)) {
		load = scx_cpu_load(rq->cpu);

		if (!partial_enable && (load > arch_scale_cpu_capacity(rq->cpu))) {
			partial_enable = true;
			if (dump_info & SCX_DEBUG_SYSTRACE)
				partial_backup_systrace_c(1);
		}
	}
}

void scx_scheduler_tick(void)
{
	int cpu = smp_processor_id();
	struct rq *rq = cpu_rq(cpu);
	struct scx_sched_rq_stats *srq;
	struct scx_entity *scx_curr = NULL;
	if(!scx_enabled_enter())
		return;

	if(unlikely(!tick_sched_clock)) {
		/*
		 * Let the window begin 20us prior to the tick,
		 * that way we are guaranteed a rollover when the tick occurs.
		 * Use rq->clock directly instead of rq_clock() since
		 * we do not have the rq lock and
		 * rq->clock was updated in the tick callpath.
		 */
		if (cmpxchg64(&tick_sched_clock, 0, rq->clock - 20000))
			goto exit;
		atomic64_set(&scx_run_rollover_lastq_ws, tick_sched_clock);
		for_each_possible_cpu(cpu) {
			srq = &per_cpu(scx_sched_rq_stats, cpu);
			srq->window_start = tick_sched_clock;
		}
	}
	raw_spin_lock(&rq->__lock);
	if (likely(rq->curr))
		scx_curr = get_oplus_ext_entity(rq->curr);
	if (scx_curr && (scx_curr->flags & SCX_TASK_QUEUED) && rq->nr_running > 1) {
		if (scx_tick_ctl && !scx_curr->slice) {
			resched_curr(rq);
		}
	}
	raw_spin_unlock(&rq->__lock);
#ifdef CONFIG_SCX_GAME_OPT_ENABLE
	if (scene_in == SGAME)
		scx_sgame_tick_update_boost(rq);
#endif
	if (dump_info & SCX_DEBUG_SYSTRACE)
		tick_gran_state_systrace_c(smp_processor_id(), 2);
exit:
	scx_enabled_exit();
}


static void scx_scheduler_tick_handler(struct rq *rq)
{
	if (scx_stats_trace && sysctl_shadow_tick_enable)
		start_shadow_tick_timer();
	scx_scheduler_tick();
}

struct scx_sched_gki_ops scx_ops = {
	.newidle_balance = scx_newidle_balance,
	.replace_next_task_fair = scx_replace_next_task_fair,
	.schedule = scx_schedule,
	.enqueue_task = enqueue_task_scx,
	.dequeue_task = dequeue_task_scx,
	.select_task_rq_rt = scx_select_task_rq_rt,
	.rt_find_lowest_rq = scx_rt_find_lowest_rq,
	.select_task_rq_fair = scx_select_task_rq_fair,
	.scheduler_tick = scx_scheduler_tick_handler,
	.tick_entry = scx_tick_entry,
	.sched_lpm_disallowed_time = scx_sched_lpm_disallowed_time,
	.nohz_balancer_kick = scx_nohz_balancer_kick,
	.cfs_check_preempt_wakeup = scx_cfs_check_preempt_wakeup,
};

int scx_sched_gki_init_early(void)
{
	int ret = 0;
	int cpu;
	cpumask_bits(&scx_cpumask_full)[0] = 0xff;
	register_scx_sched_gki_ops(&scx_ops);

	for_each_cpu(cpu, cpu_possible_mask) {
		call_single_data_t *csd;

		csd = &per_cpu(scx_nib_csd, cpu);
		INIT_CSD(csd, scx_smp_newidle_balance, (void *)(unsigned long)cpu);
		if(!(zalloc_cpumask_var_node(&per_cpu(scx_local_cpu_mask, cpu),
						GFP_KERNEL, cpu_to_node(cpu)))) {
			pr_err("scx_local_cpu_mask alloc failed for cpu%d\n", cpu);
			return -1;
		}
	}

	REGISTER_TRACE(android_rvh_sched_rebalance_domains,
				scx_sched_rebalance_domains, NULL, out);
	REGISTER_TRACE(sched_stat_runtime, scx_update_task_runtime, NULL, out);

out:
	return ret;
}

void scx_sched_gki_init(void)
{
	/*iso_masks can be configured dynamically*/
	if (!cpumask_empty(iso_masks.big)) {
		cpuctrl_high_util = arch_scale_cpu_capacity(cpumask_first(iso_masks.big))
								* cpuctrl_high_ratio / 100;
		cpuctrl_low_util = arch_scale_cpu_capacity(cpumask_first(iso_masks.big))
								* cpuctrl_low_ratio / 100;
	}
}
