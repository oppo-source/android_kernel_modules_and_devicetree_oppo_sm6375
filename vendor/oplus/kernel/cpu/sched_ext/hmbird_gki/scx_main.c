#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/syscore_ops.h>
#include <linux/sched.h>
#include <linux/sysctl.h>
#include <linux/cgroup.h>
#include <linux/tick.h>
#include <kernel/time/tick-sched.h>
#include <trace/hooks/sched.h>
#include <trace/hooks/cpufreq.h>
#include <asm/processor.h>
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_FRAME_BOOST)
#include <../kernel/oplus_cpu/sched/frame_boost/frame_info.h>
#endif

#include "scx_main.h"
#include "sched_ext.h"

unsigned int scx_stats_trace = false;
unsigned int dump_info = false;
atomic_t scx_enter_count;
unsigned int scene_in;
bool scx_clock_suspended;
u64 scx_clock_last;
int frame_per_sec;

struct scx_iso_masks iso_masks;
int num_iso_clusters = 0;
u32 SCX_BPF_DSQS_DEADLINE[MAX_BPF_DSQS] = {0, 1, 2, 4, 6, 8, 8, 32, 64, 128};
DEFINE_PER_CPU(struct scx_dispatch_q[MAX_BPF_DSQS], gdsqs);

noinline int tracing_mark_write(const char *buf)
{
	trace_printk(buf);
	return 0;
}

static __maybe_unused
void init_dsq(struct scx_dispatch_q *dsq, u64 dsq_id)
{
	memset(dsq, 0, sizeof(*dsq));

	raw_spin_lock_init(&dsq->lock);
	INIT_LIST_HEAD(&dsq->fifo);
	dsq->id = dsq_id;
}

static void init_dsq_at_boot(void)
{
	int dsq_id = 0, i, cpu;

	for_each_cpu(cpu, cpu_possible_mask) {
		for (i = 0; i < MAX_BPF_DSQS; i++) {
			init_dsq(per_cpu_ptr(&gdsqs[i], cpu), dsq_id++);
		}
	}
}

static int cgrp_name_to_idx(struct cgroup *cgrp)
{
	int idx;

	if (!cgrp)
		return -1;

	if (!strcmp(cgrp->kn->name, "display")
					|| !strcmp(cgrp->kn->name, "multimedia")
					|| !strcmp(cgrp->kn->name, "touch"))
		idx = 5; /* 8ms */
	else if (!strcmp(cgrp->kn->name, "top-app")
					|| !strcmp(cgrp->kn->name, "ss-top"))
		idx = 6; /* 16ms */
	else if (!strcmp(cgrp->kn->name, "ssfg")
					|| !strcmp(cgrp->kn->name, "foreground"))
		idx = 7; /* 32ms */
	else if (!strcmp(cgrp->kn->name, "bg")
					|| !strcmp(cgrp->kn->name, "log")
					|| !strcmp(cgrp->kn->name, "dex2oat")
					|| !strcmp(cgrp->kn->name, "background"))
		idx = 9; /* 128ms */
	else
		idx = DEFAULT_CGROUP_DL_IDX; /* 64ms */

	debug_printk("initial %s->scx_deadline_idx = %d\n", cgrp->kn->name, idx);
	return idx;
}

static void init_root_tg(struct cgroup  *cgrp, struct task_group *tg)
{
	if (!cgrp || !tg)
			return;
	tg->scx_deadline_idx = DEFAULT_CGROUP_DL_IDX;
}

static void init_level1_tg(struct cgroup *cgrp, struct task_group *tg)
{
	if (!cgrp || !tg)
			return;

	tg->scx_deadline_idx = cgrp_name_to_idx(cgrp);
	debug_printk("initial set %s->scx_deadline_idx = %d\n",
			cgrp->kn->name, tg->scx_deadline_idx);
}

#define CREATE_DSQ_LEVEL_WITHIN	(1)
/* Should do get/put to guarantee exists,TODO. */
static struct cgroup *cgroup_ancestor_l1(struct cgroup *cgrp)
{
	int i;
	struct cgroup *anc;

	for (i = 0; i < cgrp->level; i++) {
		anc = cgrp->ancestors[i];
		if (CREATE_DSQ_LEVEL_WITHIN != anc->level)
			continue;
		return anc;
	}
	debug_printk("cgroup = %s\n", cgrp->kn->name);
	return NULL;
}

static void init_child_tg(struct cgroup *cgrp, struct task_group *tg)
{
	struct cgroup *l1cgrp;

	if (!cgrp || !tg)
		return;

	l1cgrp = cgroup_ancestor_l1(cgrp);
	if (NULL == l1cgrp) {
		debug_printk("can't find l1 parent cgrp\n");
		tg->scx_deadline_idx = DEFAULT_CGROUP_DL_IDX;
		return;
	}
	tg->scx_deadline_idx = cgrp_name_to_idx(l1cgrp);
}

static void cgrp_dsq_idx_init(struct cgroup *cgrp, struct task_group *tg)
{
	switch (cgrp->level) {
	case 0:
		init_root_tg(cgrp, tg);
		break;
	case 1:
		init_level1_tg(cgrp, tg);
		break;
	default:
		init_child_tg(cgrp, tg);
		break;
	}
}

static void init_cgroup(void)
{
	struct cgroup_subsys_state *css;
	struct task_group *tg;

	css_for_each_descendant_pre(css, &root_task_group.css) {
		tg = css_tg(css);
		cgrp_dsq_idx_init(css->cgroup, tg);
	}
}

int hmbird_enable;

int move_to_same_sched_class(struct task_struct *p, int enable)
{
	struct rq *rq;
	int old_prio, old_static_prio, old_normal_prio;
	unsigned int old_rt_priority;
	int ret = 0;
	struct scx_entity *scx;

	if (READ_ONCE(p->__state) == TASK_DEAD)
		return ret;

	rq = task_rq(p);
	if (rq->stop == p)
		return ret;

	scx = get_oplus_ext_entity(p);
	if (!scx)
		return ret;

	old_prio = p->prio;
	old_static_prio = p->static_prio;
	old_normal_prio = p->normal_prio;
	old_rt_priority = p->rt_priority;

	if (enable == 1) {
		if (p->prio < MAX_RT_PRIO) {
			struct sched_param sp = {
				.sched_priority = 0
			};

			scx->prio_backup = old_prio;
			scx->ext_flags |= EXT_FLAG_RT_CHANGED;

			ret = sched_setscheduler_nocheck(p, SCHED_NORMAL, &sp);
			debug_printk("enbable=%d ret=%d comm=%-12s[%d]"
				" prio(%d->%d) static_prio(%d->%d)"
				" normal_prio(%d->%d) rt_priority(%d->%u)\n",
				hmbird_enable, ret, p->comm, p->pid, old_prio, p->prio,
				old_static_prio, p->static_prio,
				old_normal_prio, p->normal_prio,
				old_rt_priority, p->rt_priority);
		}
	} else if (enable == 0) {
		if (scx->ext_flags & EXT_FLAG_RT_CHANGED) {
			struct sched_param sp = {
				.sched_priority = (MAX_RT_PRIO - 1 - scx->prio_backup)
			};

			ret = sched_setscheduler_nocheck(p, SCHED_FIFO, &sp);
			scx->ext_flags |= ~EXT_FLAG_RT_CHANGED;
			scx->prio_backup = 0;
			debug_printk("enbable=%d ret=%d comm=%-12s[%d]"
				" prio(%d->%d) static_prio(%d->%d)"
				" normal_prio(%d->%d) rt_priority(%d->%u)\n",
				hmbird_enable, ret, p->comm, p->pid, old_prio, p->prio,
				old_static_prio, p->static_prio,
				old_normal_prio, p->normal_prio,
				old_rt_priority, p->rt_priority);
		}
	}

	return ret;
}

void prepare_cfs_tasks(struct task_struct *p, int enable)
{
	struct cfs_rq *cfs_rq;
	struct sched_entity *se;
	struct scx_entity *scx;

	/* deal with cfs tasks */
	if (p == NULL || p->prio < MAX_RT_PRIO)
		return;
	scx = get_oplus_ext_entity(p);
	if (!scx)
		return;

	if (enable == 1) {
		bool queued, running;

		queued = task_on_rq_queued(p);
		/* enqueue to the target deadline array idx, update array's count. */
		if (queued) {
			struct task_group *tg = p->sched_task_group;
			int idx;

			if (tg && tg->css.cgroup) {
				idx = tg->scx_deadline_idx;
				if (idx < 0)
					debug_printk("scx_deadline_idx=%d, "
							"comm=%-12s pid=%d\n",
							idx, p->comm, p->pid);
			}
		}

		running = task_current(task_rq(p), p);
		if (running)
			resched_curr(task_rq(p));

		scx->ext_flags |= EXT_FLAG_CFS_CHANGED;
	} else if (enable == 0) {
		bool queued;

		if (scx->ext_flags & EXT_FLAG_CFS_CHANGED) {
			scx->ext_flags &= ~EXT_FLAG_CFS_CHANGED;
		} else {
			debug_printk("untrack task comm=%-12s pid=%d\n",
								p->comm, p->pid);
		}

		/* should we care about those sleep tasks? */
		queued = task_on_rq_queued(p);
		if (queued) {
			se = &p->se;
			for_each_sched_entity(se) {
				cfs_rq = cfs_rq_of(se);
				se->vruntime = cfs_rq->min_vruntime;
			}
		}
	}
}
/*if oplus_task_struct alloc failed, task will not be sched by sched_ext*/
void scx_init_task_struct(struct task_struct *p)
{
	struct scx_entity *scx = get_oplus_ext_entity(p);
	if (!scx)
		return;
	scx->dsq		= NULL;
	INIT_LIST_HEAD(&scx->dsq_node.fifo);
	RB_CLEAR_NODE(&scx->dsq_node.priq);
	scx->flags		= 0;
	scx->dsq_flags	= 0;
	scx->sticky_cpu	= -1;
	scx->runnable_at	= INITIAL_JIFFIES;
	scx->slice		= SCX_SLICE_DFL;
	scx->ext_flags = 0;
	scx->prio_backup = 0;
	memset(&scx->sts, 0, sizeof(struct scx_task_stats));
}

void scx_task_dump(struct task_struct *p)
{
	struct scx_entity *scx = get_oplus_ext_entity(p);
	if (!scx)
		return;

	printk_deferred("Task: %.16s-%d\n", p->comm, p->pid);
	SCHED_PRINT(READ_ONCE(p->__state));
	SCHED_PRINT(task_thread_info(p)->cpu);
	SCHED_PRINT(p->policy);
	SCHED_PRINT(p->prio);
	SCHED_PRINT(p->on_cpu);
	SCHED_PRINT(p->on_rq);
	SCHED_PRINT(scx->dsq);
	SCHED_PRINT(scx->flags);
	SCHED_PRINT(scx->sticky_cpu);
	SCHED_PRINT(scx->runnable_at);
	SCHED_PRINT(scx->slice);
	SCHED_PRINT(scx->ext_flags);
	SCHED_PRINT(scx->prio_backup);
	SCHED_PRINT(scx->sts.mark_start);
	SCHED_PRINT(scx->sts.window_start);
	SCHED_PRINT(scx->sts.demand);
	SCHED_PRINT(scx->sts.demand_scaled);
}

static inline void scx_task_reinit(struct task_struct *p)
{
	int sticky_cpu;
	struct scx_entity *scx = get_oplus_ext_entity(p);
	if (scx)
		sticky_cpu = scx->sticky_cpu;
	else
		sticky_cpu = task_rq(p)->cpu;
	if (task_on_rq_migrating(p)) {
		if (unlikely(sticky_cpu == -1)) {
			SCX_BUG("requeue_migrating_task err while reinit");
		}
		activate_task(cpu_rq(sticky_cpu), p, ENQUEUE_NOCLOCK);
	}
	scx_init_task_struct(p);
}

void scx_prepare_all_task(void)
{
	int cpu;
	struct task_struct *g, *p, *stop, *next;
	int level = 0;
	struct rq *rq;
	struct scx_entity *scx;
	LIST_HEAD(dead);

	read_lock(&tasklist_lock);

	for_each_possible_cpu(cpu) {
		if (level == 0)
			raw_spin_lock(&cpu_rq(cpu)->__lock);
		else
			raw_spin_lock_nested(&cpu_rq(cpu)->__lock, level);
		level++;
	}

	init_dsq_at_boot();

	for_each_possible_cpu(cpu) {
		rq = cpu_rq(cpu);
		stop = rq->stop;
		WRITE_ONCE(rq->stop, NULL);

		while ((next = pick_migrate_task(rq)) != rq->idle) {
			if (next->thread_group.prev == LIST_POISON2) {
				list_add(&next->thread_group, &dead);
			}
			scx = get_oplus_ext_entity(next);
			if (scx)
				scx->sticky_cpu = cpu;
			deactivate_task(rq, next, DEQUEUE_NOCLOCK);
		}
		rq->stop = stop;
		scx_init_task_struct(rq->idle);
	}

	do_each_thread(g, p) {
		scx_task_reinit(p);
	} while_each_thread(g, p);

	list_for_each_entry(p, &dead, thread_group) {
		scx_task_reinit(p);
		list_del_rcu(&p->thread_group);
	}

	for_each_possible_cpu(cpu) {
		raw_spin_unlock(&cpu_rq(cpu)->__lock);
	}
	read_unlock(&tasklist_lock);
}

int prepare_for_ext_scheduler_switch(void *data)
{
	struct task_struct *p, *g;
	int level, cpu, count0 = 0, count1 = 0;
	int enable = hmbird_enable;
	unsigned long irqflags;

	read_lock(&tasklist_lock);
	/* step1: switch all rt sched class to fair sched class. */
	do_each_thread(g, p) {
		/*warning : When switching scheduling classes, adjusting the pi-chain will enable irq.*/
		local_save_flags(irqflags);
		move_to_same_sched_class(p, enable);
		local_irq_restore(irqflags);
		count0++;
	} while_each_thread(g, p);

	debug_printk("finish move_to_same_sched_class, enable=%d\n", hmbird_enable);
	level = 0;
	for_each_possible_cpu(cpu) {
		if (level == 0)
			raw_spin_lock(&cpu_rq(cpu)->__lock);
		else
			raw_spin_lock_nested(&cpu_rq(cpu)->__lock, level);

		update_rq_clock(cpu_rq(cpu));
		level++;
	}

	do_each_thread(g, p) {
		prepare_cfs_tasks(p, enable);
		count1++;
	} while_each_thread(g, p);
	debug_printk("finish prepare_cfs_tasks, enable=%d\n", hmbird_enable);

	for_each_possible_cpu(cpu) {
		raw_spin_unlock(&cpu_rq(cpu)->__lock);
	}
	read_unlock(&tasklist_lock);

	if (count0 != count1)
		pr_err("[xlj] count0(%d) is not the same as count1(%d)\n",
								count0, count1);

	return 0;
}

static void scx_sched_init_rq(struct rq *rq, bool reinit)
{
	struct scx_sched_rq_stats *srq = &per_cpu(scx_sched_rq_stats, cpu_of(rq));
	int i;

	srq->local_dsq_s.cumulative_runnable_avg_scaled = 0;
	srq->local_dsq_s.nr_period_tasks = 0;
	srq->local_dsq_s.nr_tasks = 0;
	srq->prev_window_size = scx_sched_ravg_window;
	srq->task_exec_scale = 1024;
	for (i = 0; i < num_iso_clusters; i++) {
		if (cpumask_test_cpu(cpu_of(rq), iso_masks.cluster[i])) {
			srq->iso_idx = i;
			break;
		}
	}
	if (i == num_iso_clusters)
		srq->iso_idx = -1;

	if (!reinit) {
		srq->window_start = 0;
	}
	else {
		if (unlikely(!tick_sched_clock)) {
			SCX_BUG("tick_sched_clock should not be 0 while reinit!\n");
		}
		srq->window_start = tick_sched_clock;
	}
}

static int scx_reinit_stop_handler(void *data)
{
	int cpu;
	static bool reinit = false;
	unsigned long flags;

	if (unlikely(atomic_read(&scx_enter_count) != 0 || scx_stats_trace))
		SCX_BUG("scx_reinit while scx_enter_count=%d, scx_stats_trace=%d\n",
							atomic_read(&scx_enter_count), scx_stats_trace);
	init_cgroup();
	hmbird_enable = 1;
	scx_prepare_all_task();
	prepare_for_ext_scheduler_switch(NULL);
	scx_sched_gki_init();
	scx_fixup_window_dep();

	for_each_possible_cpu(cpu) {
		struct rq *rq = cpu_rq(cpu);

		raw_spin_lock_irqsave(&rq->__lock, flags);
		scx_sched_init_rq(rq, reinit);
		raw_spin_unlock_irqrestore(&rq->__lock, flags);
	}
	if (!reinit) {
		reinit = true;
	} else {
		if (unlikely(!tick_sched_clock)) {
			SCX_BUG("tick_sched_clock should not be 0 while reinit!\n");
		}
		atomic64_set(&scx_run_rollover_lastq_ws, tick_sched_clock);
	}
	scx_stats_trace = true;
	return 0;
}

static void scx_reinit(void)
{
#ifdef CONFIG_SCX_GAME_OPT_ENABLE
	scx_init_frame_boost_for_sgame();
#endif
	stop_machine(scx_reinit_stop_handler, NULL, NULL);
}

void walt_switch_to_scx(void)
{
#ifdef CONFIG_DYNAMIC_WALT_SUPPORT
	walt_disable_wait_for_completion();
#endif
	if (READ_ONCE(scx_stats_trace))
		return;

	while (atomic_read(&scx_enter_count))
		cpu_relax();

	scx_reinit();

	while(!READ_ONCE(scx_stats_trace))
		cpu_relax();
}

void scx_switch_to_walt(void)
{
#ifdef CONFIG_DYNAMIC_WALT_SUPPORT
	if (unlikely(!scx_stats_trace))
		SCX_BUG("scx_stats_trace=%d while switch walt!\n", scx_stats_trace);
#endif
	if(!cmpxchg(&scx_stats_trace, true, false))
		pr_warn("scx has already been disabled!\n");
	hmbird_enable = 0;
	while(atomic_read(&scx_enter_count))
		cpu_relax();
#ifdef CONFIG_SCX_GAME_OPT_ENABLE
	scx_exit_frame_boost_for_sgame();
#endif
	stop_machine(prepare_for_ext_scheduler_switch, NULL, NULL);
#ifdef CONFIG_DYNAMIC_WALT_SUPPORT
	walt_enable_wait_for_completion();
#endif
}

static void scx_resume(void)
{
	scx_clock_suspended = false;
}

static int scx_suspend(void)
{
	scx_clock_last = sched_clock();
	scx_clock_suspended = true;
	return 0;
}

static struct syscore_ops scx_syscore_ops = {
	.resume		= scx_resume,
	.suspend	= scx_suspend
};

static DEFINE_MUTEX(switch_mutex);
void clear_cpu_from_all_masks(int cpu)
{
	cpumask_clear_cpu(cpu, iso_masks.partial);
	cpumask_clear_cpu(cpu, iso_masks.exclusive);
	cpumask_clear_cpu(cpu, iso_masks.little);
	cpumask_clear_cpu(cpu, iso_masks.big);
}

int parse_and_set_cpus(const char *input, struct cpumask *mask)
{
	char *token;
	char *input_copy;
	char *cur;
	int start, end, cpu;

	input_copy = kstrdup(input, GFP_KERNEL);
	if (!input_copy)
		return -ENOMEM;

	cur = input_copy;

	while ((token = strsep(&cur, ",")) != NULL) {
		if (sscanf(token, "%d-%d", &start, &end) == 2) {
			for (cpu = start; cpu <= end; cpu++) {
				clear_cpu_from_all_masks(cpu);
				cpumask_set_cpu(cpu, mask);
			}
		} else if (sscanf(token, "%d", &cpu) == 1) {
			clear_cpu_from_all_masks(cpu);
			cpumask_set_cpu(cpu, mask);
		} else {
			kfree(input_copy);
			return -EINVAL;
		}
	}

	kfree(input_copy);
	return 0;
}

static ssize_t cpumask_to_str(struct cpumask *mask, char *buf, size_t buf_size)
{
	int cpu, len = 0;
	bool first = true;

	for_each_cpu(cpu, mask) {
		if (!first)
			len += scnprintf(buf + len, buf_size - len, ",");
		len += scnprintf(buf + len, buf_size - len, "%d", cpu);
		first = false;
	}

	return len;
}

static ssize_t print_cpu_distribution(char *buf, size_t buf_size)
{
	int len = 0, i;
	for (i = 0; i < num_iso_clusters; i++) {
		len += scnprintf(buf + len, buf_size - len, "[");
		len += cpumask_to_str(iso_masks.cluster[i], buf + len, buf_size - len);
		len += scnprintf(buf + len, buf_size - len, "]\n");
	}
	return len;
}

static int iso_mask_proc_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp, loff_t *ppos)
{
	char input[128] = { };
	struct cpumask *mask;
	int ret;
	struct ctl_table tmp = {
		.data	= &input,
		.maxlen	= sizeof(input),
	};
	mask = (struct cpumask *)table->data;
	if (write) {
		ret = proc_dostring(&tmp, write, buffer, lenp, ppos);
		if (ret) {
			pr_err("proc_dostring=%d fail, buffer=%s\n", ret, buffer);
			return -EFAULT;
		}
		mutex_lock(&switch_mutex);
		if (scx_stats_trace) {
			pr_err("iso_mask can not be set while enable!\n");
			mutex_unlock(&switch_mutex);
			return -EINVAL;
		}
		ret = parse_and_set_cpus(input, mask);
		mutex_unlock(&switch_mutex);
		return ret ? ret : *lenp;
	} else {
		ret = print_cpu_distribution(input, sizeof(input));
		if (ret < 0)
			return ret;
		ret = proc_dostring(&tmp, write, buffer, lenp, ppos);
		return ret;
	}
}

static int scx_proc_scx_stats_trace_enable(struct ctl_table *table,
				int write, void __user *buffer, size_t *lenp,
				loff_t *ppos)
{
	int ret = -EPERM;
	int val;

	struct ctl_table tmp = {
		.data	= &val,
		.maxlen	= sizeof(val),
		.mode	= table->mode,
	};

	mutex_lock(&switch_mutex);

	val = scx_stats_trace;
	ret = proc_dointvec(&tmp, write, buffer, lenp, ppos);
	if (ret || !write || (!!val == scx_stats_trace))
		goto unlock;

	if(val) {
		scene_in = val;
        walt_switch_to_scx();
	} else
		scx_switch_to_walt();

unlock:
	mutex_unlock(&switch_mutex);
	return ret;
}

static int scx_proc_sched_ravg_window_update(struct ctl_table *table,
				int write, void __user *buffer, size_t *lenp,
				loff_t *ppos)
{
	int ret = -EPERM;
	int val;
	static DEFINE_MUTEX(mutex);

	struct ctl_table tmp = {
		.data	= &val,
		.maxlen	= sizeof(val),
		.mode	= table->mode,
	};

	mutex_lock(&mutex);

	val = frame_per_sec;
	ret = proc_dointvec(&tmp, write, buffer, lenp, ppos);
	if (ret || !write || (val == frame_per_sec))
		goto unlock;
	frame_per_sec = val;
	sched_ravg_window_change(frame_per_sec);
unlock:
	mutex_unlock(&mutex);
	return ret;
}

struct ctl_table scx_table[] = {
	{
		.procname	= "scx_enable",
		.data		= &scx_stats_trace,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= scx_proc_scx_stats_trace_enable,
	},
	{
		.procname	= "scx_shadow_tick_enable",
		.data		= &sysctl_shadow_tick_enable,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{
		.procname	= "sched_ravg_window_frame_per_sec",
		.data		= &frame_per_sec,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= scx_proc_sched_ravg_window_update,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_INT_MAX,
	},
	{
		.procname   = "cpus_partial",
		.data       = (void *)iso_masks.partial,
		.maxlen     = 64,
		.mode       = 0644,
		.proc_handler = iso_mask_proc_handler,
	},
	{
		.procname   = "cpus_exclusive",
		.data       = (void *)iso_masks.exclusive,
		.maxlen     = 64,
		.mode       = 0644,
		.proc_handler = iso_mask_proc_handler,
	},
	{
		.procname   = "cpus_little",
		.data       = (void *)iso_masks.little,
		.maxlen     = 64,
		.mode       = 0644,
		.proc_handler = iso_mask_proc_handler,
	},
	{
		.procname   = "cpus_big",
		.data       = (void *)iso_masks.big,
		.maxlen     = 64,
		.mode       = 0644,
		.proc_handler = iso_mask_proc_handler,
	},
	{
		.procname	= "scx_idle_ctl_enable",
		.data		= &scx_idle_ctl,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{
		.procname	= "scx_tick_resched_enable",
		.data		= &scx_tick_ctl,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{
		.procname	= "scx_newidle_balance_ctl",
		.data		= &scx_newidle_balance_ctl,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{
		.procname	= "scx_exclusive_sync_enable",
		.data		= &scx_exclusive_sync_ctl,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
#ifdef CONFIG_SCX_GAME_OPT_ENABLE
	{
		.procname	= "scx_boost_enable",
		.data		= &scx_boost_ctl,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{
		.procname	= "pid_unitymain",
		.data		= &pid_unitymain,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_INT_MAX,
	},
	{
		.procname	= "cpu_unitymain",
		.data		= &cpu_unitymain,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_INT_MAX,
	},
	{
		.procname	= "min_freq_ctl",
		.data		= &min_freq_ctl,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_INT_MAX,
	},
#endif
	{ },
};

struct ctl_table scx_base_table[] = {
	{
		.procname	= "oplus_sched_ext",
		.mode		= 0555,
		.child		= scx_table,
	},
	{ },
};

static void init_isolate_cpus(void)
{
	if (!alloc_cpumask_var(&iso_masks.partial, GFP_KERNEL))
		goto err;
	if (!alloc_cpumask_var(&iso_masks.exclusive, GFP_KERNEL))
		goto err_free_partial;
	if (!alloc_cpumask_var(&iso_masks.big, GFP_KERNEL))
		goto err_free_exclusive;
	if (!alloc_cpumask_var(&iso_masks.little, GFP_KERNEL))
		goto err_free_big;
	/*Can be configured dynamically before enable*/
	cpumask_set_cpu(5, iso_masks.partial);
	cpumask_set_cpu(6, iso_masks.partial);
	cpumask_set_cpu(4, iso_masks.exclusive);
	cpumask_set_cpu(7, iso_masks.exclusive);
	cpumask_set_cpu(0, iso_masks.little);
	cpumask_set_cpu(1, iso_masks.little);
	cpumask_set_cpu(2, iso_masks.big);
	cpumask_set_cpu(3, iso_masks.big);
	num_iso_clusters = 4;
	return 0;

err_free_big:
	free_cpumask_var(iso_masks.big);
err_free_exclusive:
	free_cpumask_var(iso_masks.exclusive);
err_free_partial:
	free_cpumask_var(iso_masks.partial);
err:
	return -ENOMEM;
}

int __init scx_init(void)
{
	int ret = 0;
	ret = init_isolate_cpus();
	if (ret < 0) {
		pr_err("init_isolate_cpus fail!\n");
		return ret;
	}
	register_sysctl_table(scx_base_table);
	register_syscore_ops(&scx_syscore_ops);
	init_dsq_at_boot();
	scx_sched_gki_init_early();
	scx_shadow_tick_init();
	scx_cpufreq_init();
#ifdef CONFIG_SCX_GAME_OPT_ENABLE
	scx_game_init_early();
#endif
	return ret;
}

void __exit scx_exit(void)
{
}

