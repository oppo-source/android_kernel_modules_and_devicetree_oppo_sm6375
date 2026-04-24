/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2022 Oplus. All rights reserved.
 */

#include <linux/kmemleak.h>
#include <linux/cpufreq.h>
#include <linux/sched.h>
#include <../kernel/sched/sched.h>
#include <trace/hooks/sched.h>

#ifdef CONFIG_HMBIRD_SCHED
#include "./hmbird/hmbird_sched.h"
#else
#include "./hmbird_gki/scx_main.h"
#endif

#ifndef CONFIG_HMBIRD_SCHED
unsigned int sysctl_scx_gov_debug;
static int cpufreq_gov_debug(void) {return sysctl_scx_gov_debug;}
#else
extern struct scx_iso_masks iso_masks;
extern int slim_gov_debug;
static int cpufreq_gov_debug(void) {return slim_gov_debug;}
#endif

/*debug level for scx_gov*/
#define DEBUG_SYSTRACE (1 << 0)
#define DEBUG_FTRACE   (1 << 1)
#define DEBUG_KMSG     (1 << 2)

#define scx_gov_debug(fmt, ...) \
	pr_info("[scx_gov][%s] "fmt, __func__, ##__VA_ARGS__)

#define scx_gov_err(fmt, ...) \
	pr_err("[scx_gov][%s] "fmt, __func__, ##__VA_ARGS__)

#define gov_trace_printk(fmt, args...)	\
do {							\
	if (cpufreq_gov_debug() & DEBUG_FTRACE)		\
		trace_printk("[scx_gov] "fmt, args);	\
} while (0)

#define DEFAULT_TARGET_LOAD 80

#define MAX_CLUSTERS 4
static int gov_flag[MAX_CLUSTERS] = {0};
struct proc_dir_entry *dir;
struct scx_sched_cluster {
	struct list_head	list;
	struct cpumask	cpus;
	int id;
};
__read_mostly int num_sched_clusters;
struct scx_sched_cluster *scx_cluster[MAX_CLUSTERS];
struct list_head cluster_head;
bool scx_gov_boost_cluster[MAX_CLUSTERS] = {false};
bool scx_gov_boost_cluster_last[MAX_CLUSTERS] = {false};
#define for_each_sched_cluster(cluster) \
	list_for_each_entry_rcu(cluster, &cluster_head, list)


static struct irq_work scx_cpufreq_irq_work;

#define MAX_TL_SECS	(3)
#define GOV_MAX_UTIL	(0)
#define GOV_AVG_UTIL	(1)

struct scx_gov_tunables {
	struct cpufreq_policy		*policy;
	struct gov_attr_set		attr_set;
	int				soft_freq_max;
	int				soft_freq_min;
	bool				apply_freq_immediately;

	int                             target_loads[MAX_TL_SECS];
	int                             tl_thres[MAX_TL_SECS];
	int				cpu_util_policy;
};

struct scx_gov_policy {
	struct cpufreq_policy	*policy;

	struct scx_gov_tunables	*tunables;
	struct list_head	tunables_hook;

	raw_spinlock_t		update_lock;	/* For shared policies */
	unsigned int		next_freq;
	/* The next fields are only needed if fast switch cannot be used: */
	struct kthread_work	work;
	struct mutex		work_lock;
	struct kthread_worker	worker;
	struct task_struct	*thread;
	bool			work_in_progress;
	unsigned int	target_load;
	bool		backup_efficiencies_available;
};

struct scx_gov_cpu {
#ifndef CONFIG_SCX_USE_UTIL_TRACK
	struct waltgov_callback	cb;
#else
	struct update_util_data	update_util;
#endif
	unsigned int		reasons;
	struct scx_gov_policy	*sg_policy;
	unsigned int		cpu;

	unsigned long		util;
	unsigned int		flags;
};

static DEFINE_PER_CPU(struct scx_gov_cpu, scx_gov_cpu);
static DEFINE_PER_CPU(struct scx_gov_tunables *, cached_tunables);
static DEFINE_MUTEX(global_tunables_lock);
static struct scx_gov_tunables *global_tunables;

struct boost_policy_params boost_policy_params = {
	.enable = 0,
	.bottom_freq = 1200000,
	.boost_weight = 120,
};

#ifdef CONFIG_ARCH_MEDIATEK
static inline void scx_boost_systrace_c(int cpu, unsigned int next_f)
{
	char buf[256];
	snprintf(buf, sizeof(buf), "C|9999|Cpu%d_boost_freq|%u\n",
			cpu, next_f);
	tracing_mark_write(buf);
}
#endif

static void scx_gov_work(struct kthread_work *work)
{
	struct scx_gov_policy *sg_policy =
				container_of(work, struct scx_gov_policy, work);
	unsigned int freq;
	unsigned long flags;

	/*
	 * Hold sg_policy->update_lock shortly to handle the case where:
	 * incase sg_policy->next_freq is read here, and then updated by
	 * scx_gov_deferred_update() just before work_in_progress is set to false
	 * here, we may miss queueing the new update.
	 *
	 * Note: If a work was queued after the update_lock is released,
	 * scx_gov_work() will just be called again by kthread_work code; and the
	 * request will be proceed before the scx_gov thread sleeps.
	 */
	raw_spin_lock_irqsave(&sg_policy->update_lock, flags);
	freq = sg_policy->next_freq;
	raw_spin_unlock_irqrestore(&sg_policy->update_lock, flags);

	mutex_lock(&sg_policy->work_lock);
	__cpufreq_driver_target(sg_policy->policy, freq, CPUFREQ_RELATION_L);
	mutex_unlock(&sg_policy->work_lock);
}

static inline void scx_irq_work_queue(struct irq_work *work)
{
	if (likely(cpu_online(raw_smp_processor_id())))
		irq_work_queue(work);
	else
		irq_work_queue_on(work, cpumask_any(cpu_online_mask));
}

void run_scx_irq_work_rollover(void)
{
	scx_irq_work_queue(&scx_cpufreq_irq_work);
}

#define MAX_POSSIBLE_FREQ	(6000000)
#define MIN_POSSIBLE_FREQ	(0)
#define MAX_POSSIBLE_TL		(100)
#define MIN_POSSIBLE_TL		(1)

static bool is_valid_tl(int tl)
{
	return tl <= MAX_POSSIBLE_TL && tl >= MIN_POSSIBLE_TL;
}

static bool is_valid_freq(int freq)
{
	return freq <= MAX_POSSIBLE_FREQ && freq >= MIN_POSSIBLE_FREQ;
}
/* next_freq = (max_freq * scale_time* 100) /
		(window_size * TL * arch_scale_cpu_capacity) */
#define DIV64_U64_ROUNDUP(X, Y) div64_u64((X) + (Y - 1), Y)
static unsigned int get_next_freq(struct scx_gov_policy *sg_policy,
							u64 prev_runnable_sum)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int freq = policy->cpuinfo.max_freq, raw_next_f, next_f;
	unsigned int cluster_tl = 0;
	u64 divisor;
	int cpu = cpumask_first(policy->cpus);

#ifdef CONFIG_SCX_USE_UTIL_TRACK
	divisor = DIV64_U64_ROUNDUP(scx_sched_ravg_window * arch_scale_cpu_capacity(cpu), freq);
#else
	divisor = DIV64_U64_ROUNDUP(sched_ravg_window * arch_scale_cpu_capacity(cpu), freq);
#endif

	raw_next_f = DIV64_U64_ROUNDUP(prev_runnable_sum << SCHED_CAPACITY_SHIFT, divisor);
	if (sg_policy->tunables) {
		int i = MAX_TL_SECS;

		while(--i && raw_next_f < sg_policy->tunables->tl_thres[i]);
		cluster_tl = sg_policy->tunables->target_loads[i];
	}
	if (!is_valid_tl(cluster_tl))
		cluster_tl = DEFAULT_TARGET_LOAD;

	next_f = mult_frac(raw_next_f, 100, cluster_tl);

	gov_trace_printk("cluster[%d] tl[%d] max_freq[%d]"
			"cpu_cap[%lu] divisor[%llu] raw_next_f[%d] next_f[%d]\n",
			cpu, cluster_tl, freq,
			arch_scale_cpu_capacity(cpu),
			divisor, raw_next_f, next_f);
	return next_f;
}

static unsigned int soft_freq_clamp(struct scx_gov_policy *sg_policy, unsigned int target_freq)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	int soft_freq_max = sg_policy->tunables->soft_freq_max;
	int soft_freq_min = sg_policy->tunables->soft_freq_min;

	if (soft_freq_min >= 0 && soft_freq_min > target_freq) {
		target_freq = soft_freq_min;
	}
	if (soft_freq_max >= 0 && soft_freq_max < target_freq) {
		target_freq = soft_freq_max;
	}

	gov_trace_printk("cluster[%d] max_freq[%d] min_freq[%d] freq[%d]\n",
			policy->cpu, soft_freq_max, soft_freq_min, target_freq);

	return target_freq;
}

void scx_gov_update_cpufreq(struct cpufreq_policy *policy, u64 prev_runnable_sum)
{
	unsigned int next_f;
	struct scx_gov_policy *sg_policy = policy->governor_data;
	unsigned long irq_flags;

	raw_spin_lock_irqsave(&sg_policy->update_lock, irq_flags);

	next_f = get_next_freq(sg_policy, prev_runnable_sum);
	next_f = soft_freq_clamp(sg_policy, next_f);
	next_f = cpufreq_driver_resolve_freq(policy, next_f);
	if (sg_policy->next_freq == next_f)
		goto unlock;
	sg_policy->next_freq = next_f;
	gov_trace_printk("cluster[%d] freq[%d] fast[%d]\n",
			policy->cpu, next_f, policy->fast_switch_enabled);
	if (policy->fast_switch_enabled)
		cpufreq_driver_fast_switch(policy, next_f);
	else
		kthread_queue_work(&sg_policy->worker, &sg_policy->work);

unlock:
	raw_spin_unlock_irqrestore(&sg_policy->update_lock, irq_flags);
}

void scx_gov_update_soft_limit_cpufreq(struct scx_gov_policy *sg_policy)
{
	unsigned int next_f;
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned long irq_flags;

	raw_spin_lock_irqsave(&sg_policy->update_lock, irq_flags);

	next_f = soft_freq_clamp(sg_policy, sg_policy->next_freq);
	next_f = cpufreq_driver_resolve_freq(policy, next_f);
	if (sg_policy->next_freq == next_f)
		goto unlock;
	sg_policy->next_freq = next_f;
	gov_trace_printk("cluster[%d] freq[%d] fast[%d]\n",
			policy->cpu, next_f, policy->fast_switch_enabled);
	if (policy->fast_switch_enabled)
		cpufreq_driver_fast_switch(policy, next_f);
	else
		kthread_queue_work(&sg_policy->worker, &sg_policy->work);

unlock:
	raw_spin_unlock_irqrestore(&sg_policy->update_lock, irq_flags);
}

/************************** sysfs interface ************************/
static inline struct scx_gov_tunables *to_scx_gov_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct scx_gov_tunables, attr_set);
}

static DEFINE_MUTEX(min_rate_lock);


static ssize_t target_loads_show(struct gov_attr_set *attr_set, char *buf)
{
	struct scx_gov_tunables *tunables = to_scx_gov_tunables(attr_set);
	char tmp[256];
	int i, idx = 0;

	if (!tunables)
		return -EFAULT;

	for (i = 0; i < MAX_TL_SECS; i++) {
		idx += sprintf(&tmp[idx], "(%d,%d)",
			tunables->tl_thres[i], tunables->target_loads[i]);
	}
	tmp[idx] = '\0';

	return sprintf(buf, "%s\n", tmp);
}
/*
 * 1. must follow format strictly:(thres1,tl1)(thres2,tl2)(thres3,tl3)
 * 2. thres must be sorted in ascending order.
 * 3. thres or target load must within a acceptable range.
 * e.g. echo "(0,80)(800000,85)(1200000,90)" >
 *      /sys/devices/system/cpu/cpufreq/policy0/scx/target_loads
 */
static ssize_t target_loads_store(struct gov_attr_set *attr_set, const char *buf,
					size_t count)
{
	struct scx_gov_tunables *tunables = to_scx_gov_tunables(attr_set);
	int tl[MAX_TL_SECS];
	int tl_thres[MAX_TL_SECS];
	int i, idx = 0;
	int last = 0;
	char tmp[256];

	if (!tunables)
		return -EFAULT;

	for (i = 0; i < MAX_TL_SECS; i++) {
		if (idx < 0 || idx >= count)
			return -EINVAL;
		if (sscanf(&buf[idx], "(%d,%d)", &tl_thres[i], &tl[i]) != 2)
			return -EINVAL;

		idx +=  sprintf(tmp, "(%d,%d)", tl_thres[i], tl[i]);
	}
	/* Verify params */
	for (i = 0; i < MAX_TL_SECS; i++) {
		/* tl thres must be sorted in ascending order.*/
		if (tl_thres[i] < last)
			return -EINVAL;

		last = tl_thres[i];

		if (!is_valid_freq(tl_thres[i]))
			return -EINVAL;

		if (!is_valid_tl(tl[i]))
			return -EINVAL;
	}

	for (i = 0; i < MAX_TL_SECS; i++) {
		tunables->tl_thres[i] = tl_thres[i];
                tunables->target_loads[i] = tl[i];
	}

	return count;
}
#ifdef CONFIG_HMBIRD_SCHED
static ssize_t cpu_util_policy_show(struct gov_attr_set *attr_set, char *buf)
{
	struct scx_gov_tunables *tunables = to_scx_gov_tunables(attr_set);
	if (!tunables)
		return -EFAULT;

	return sprintf(buf, "%s\n", !tunables->cpu_util_policy ?
				"0(GOV_MAX_UTIL)" : "1(GOV_AVG_UTIL)");
}

static ssize_t cpu_util_policy_store(struct gov_attr_set *attr_set, const char *buf,
                                        size_t count)
{
	struct scx_gov_tunables *tunables = to_scx_gov_tunables(attr_set);
	int new_policy;
	int cpu;

	if (!tunables || !tunables->policy)
		return -EFAULT;

	if (kstrtoint(buf, 10, &new_policy))
		return -EINVAL;

	if (new_policy < GOV_MAX_UTIL || new_policy > GOV_AVG_UTIL)
		return -EINVAL;

	if (new_policy == GOV_AVG_UTIL) {
		for_each_cpu(cpu, tunables->policy->cpus) {
			if (cpumask_test_cpu(cpu, iso_masks.partial)
					|| cpumask_test_cpu(cpu, iso_masks.ex_free)
					|| cpumask_test_cpu(cpu, iso_masks.exclusive))
				return -EINVAL;
		}
	}

	tunables->cpu_util_policy = new_policy;

	return count;
}
#endif

static ssize_t soft_freq_max_show(struct gov_attr_set *attr_set, char *buf)
{
	struct scx_gov_tunables *tunables = to_scx_gov_tunables(attr_set);
	int soft_freq_max = tunables->soft_freq_max;

	if (soft_freq_max < 0) {
		return sprintf(buf, "max\n");
	} else {
		return sprintf(buf, "%d\n", soft_freq_max);
	}
}

static ssize_t soft_freq_max_store(struct gov_attr_set *attr_set, const char *buf, size_t count)
{
	struct scx_gov_tunables *tunables = to_scx_gov_tunables(attr_set);
	struct scx_gov_policy *sg_policy = list_first_entry(&attr_set->policy_list, struct scx_gov_policy, tunables_hook);
	int new_soft_freq_max = -1;

	if (kstrtoint(buf, 10, &new_soft_freq_max))
		return -EINVAL;

	if (tunables->soft_freq_max == new_soft_freq_max) {
		return count;
	}

	tunables->soft_freq_max = new_soft_freq_max;
	if (tunables->apply_freq_immediately) {
		scx_gov_update_soft_limit_cpufreq(sg_policy);
	}

	return count;
}

static ssize_t soft_freq_min_show(struct gov_attr_set *attr_set, char *buf)
{
	struct scx_gov_tunables *tunables = to_scx_gov_tunables(attr_set);
	int soft_freq_min = tunables->soft_freq_min;

	if (soft_freq_min < 0) {
		return sprintf(buf, "0\n");
	} else {
		return sprintf(buf, "%d\n", soft_freq_min);
	}
}

static ssize_t soft_freq_min_store(struct gov_attr_set *attr_set, const char *buf, size_t count)
{
	struct scx_gov_tunables *tunables = to_scx_gov_tunables(attr_set);
	struct scx_gov_policy *sg_policy = list_first_entry(&attr_set->policy_list, struct scx_gov_policy, tunables_hook);
	int new_soft_freq_min = -1;

	if (kstrtoint(buf, 10, &new_soft_freq_min))
		return -EINVAL;

	if (tunables->soft_freq_min == new_soft_freq_min) {
		return count;
	}

	tunables->soft_freq_min = new_soft_freq_min;
	if (tunables->apply_freq_immediately) {
		scx_gov_update_soft_limit_cpufreq(sg_policy);
	}

	return count;
}

static ssize_t soft_freq_cur_show(struct gov_attr_set *attr_set __maybe_unused, char *buf)
{
	return sprintf(buf, "none\n");
}

static ssize_t soft_freq_cur_store(struct gov_attr_set *attr_set, const char *buf, size_t count)
{
	struct scx_gov_tunables *tunables = to_scx_gov_tunables(attr_set);
	struct scx_gov_policy *sg_policy = list_first_entry(&attr_set->policy_list, struct scx_gov_policy, tunables_hook);
	int new_soft_freq_cur = -1;

	if (kstrtoint(buf, 10, &new_soft_freq_cur))
		return -EINVAL;

	if (tunables->soft_freq_max == new_soft_freq_cur && tunables->soft_freq_min == new_soft_freq_cur) {
		return count;
	}

	tunables->soft_freq_max = new_soft_freq_cur;
	tunables->soft_freq_min = new_soft_freq_cur;
	if (tunables->apply_freq_immediately) {
		scx_gov_update_soft_limit_cpufreq(sg_policy);
	}

	return count;
}

static ssize_t apply_freq_immediately_show(struct gov_attr_set *attr_set, char *buf)
{
	struct scx_gov_tunables *tunables = to_scx_gov_tunables(attr_set);
	return sprintf(buf, "%d\n", (int)tunables->apply_freq_immediately);
}

static ssize_t apply_freq_immediately_store(struct gov_attr_set *attr_set, const char *buf, size_t count)
{
	struct scx_gov_tunables *tunables = to_scx_gov_tunables(attr_set);
	int new_apply_freq_immediately = 0;

	if (kstrtoint(buf, 10, &new_apply_freq_immediately))
		return -EINVAL;

	tunables->apply_freq_immediately = new_apply_freq_immediately > 0;
	return count;
}

ssize_t set_sugov_tl_scx(unsigned int cpu, char *buf)
{
	struct cpufreq_policy *policy;
	struct scx_gov_policy *sg_policy;
	struct scx_gov_tunables *tunables;
	struct gov_attr_set *attr_set;
	size_t count;

	if (!buf)
		return -EFAULT;

	policy = cpufreq_cpu_get(cpu);
	if (!policy)
		return -ENODEV;

	sg_policy = policy->governor_data;
	if (!sg_policy)
		return -EINVAL;

	tunables = sg_policy->tunables;
	if (!tunables)
		return -ENOMEM;

	attr_set = &tunables->attr_set;
	count = strlen(buf);

	return target_loads_store(attr_set, buf, count);
}
EXPORT_SYMBOL_GPL(set_sugov_tl_scx);

static struct governor_attr target_loads =
	__ATTR(target_loads, 0664, target_loads_show, target_loads_store);

#ifdef CONFIG_HMBIRD_SCHED
static struct governor_attr cpu_util_policy =
	__ATTR(cpu_util_policy, 0664, cpu_util_policy_show, cpu_util_policy_store);
#endif

static struct governor_attr soft_freq_max =
	__ATTR(soft_freq_max, 0664, soft_freq_max_show, soft_freq_max_store);

static struct governor_attr soft_freq_min =
	__ATTR(soft_freq_min, 0664, soft_freq_min_show, soft_freq_min_store);

static struct governor_attr soft_freq_cur =
	__ATTR(soft_freq_cur, 0664, soft_freq_cur_show, soft_freq_cur_store);

static struct governor_attr apply_freq_immediately =
	__ATTR(apply_freq_immediately, 0664, apply_freq_immediately_show, apply_freq_immediately_store);

static struct attribute *scx_gov_attrs[] = {
	&target_loads.attr,
#ifdef CONFIG_HMBIRD_SCHED
	&cpu_util_policy.attr,
#endif
	&soft_freq_max.attr,
	&soft_freq_min.attr,
	&soft_freq_cur.attr,
	&apply_freq_immediately.attr,
	NULL
};
ATTRIBUTE_GROUPS(scx_gov);

static struct kobj_type scx_gov_tunables_ktype = {
	.default_groups = scx_gov_groups,
	.sysfs_ops = &governor_sysfs_ops,
};

/********************** cpufreq governor interface *********************/

struct cpufreq_governor cpufreq_scx_gov;

static struct scx_gov_policy *scx_gov_policy_alloc(struct cpufreq_policy *policy)
{
	struct scx_gov_policy *sg_policy;

	sg_policy = kzalloc(sizeof(*sg_policy), GFP_KERNEL);
	if (!sg_policy)
		return NULL;

	sg_policy->policy = policy;
	raw_spin_lock_init(&sg_policy->update_lock);
	return sg_policy;
}

static inline void scx_gov_cpu_reset(struct scx_gov_policy *sg_policy)
{
	unsigned int cpu;

	for_each_cpu(cpu, sg_policy->policy->cpus) {
		struct scx_gov_cpu *sg_cpu = &per_cpu(scx_gov_cpu, cpu);

		sg_cpu->sg_policy = NULL;
	}
}

static void scx_gov_policy_free(struct scx_gov_policy *sg_policy)
{
	kfree(sg_policy);
}

static void scx_irq_work(struct irq_work *irq_work)
{
#ifdef CONFIG_SCX_USE_UTIL_TRACK
	cpumask_t lock_cpus;
	struct scx_sched_cluster *cluster;
	struct cpufreq_policy *policy;
	struct scx_gov_policy *sg_policy;
	struct scx_sched_rq_stats *srq;
	struct rq *rq;
	int cpu;
	int level = 0;
	u64 wc;
	unsigned long flags;

	cpumask_copy(&lock_cpus, cpu_possible_mask);

	for_each_cpu(cpu, &lock_cpus) {
		if (level == 0)
			raw_spin_lock(&cpu_rq(cpu)->__lock);
		else
			raw_spin_lock_nested(&cpu_rq(cpu)->__lock, level);
		level++;
	}

#ifdef CONFIG_HMBIRD_SCHED
	wc = sched_clock();
#else
	wc = scx_sched_clock();
#endif

	for_each_sched_cluster(cluster) {
		cpumask_t cluster_online_cpus;
		u64 prev_runnable_sum = 0;
		u64 pmax = 0, psum = 0;

		if (gov_flag[cluster->id] == 0)
			continue;
		if (scx_gov_boost_cluster[cluster->id]) {
			scx_gov_boost_cluster[cluster->id] = false;
			scx_gov_boost_cluster_last[cluster->id] = true;
			continue;
		} else if (scx_gov_boost_cluster_last[cluster->id]) {
			scx_gov_boost_cluster_last[cluster->id] = false;
			scx_boost_systrace_c(cpumask_first(&cluster->cpus), 0);
		}
		cpumask_and(&cluster_online_cpus, &cluster->cpus, cpu_online_mask);
		for_each_cpu(cpu, &cluster_online_cpus) {
			rq = cpu_rq(cpu);
#ifdef CONFIG_HMBIRD_SCHED
			scx_update_task_ravg(rq->curr,
					rq, TASK_UPDATE, wc);
#else
			struct scx_entity *scx =
					get_oplus_ext_entity(rq->curr);
			if (scx)
				scx_update_task_ravg(scx,
					rq->curr, rq, TASK_UPDATE, wc);
#endif
			srq = &per_cpu(scx_sched_rq_stats, cpu);
			gov_trace_printk("cpu[%d] prev_runnable_sum[%llu]\n",
					cpu, srq->prev_runnable_sum);
			psum += srq->prev_runnable_sum;
			pmax = max(pmax, srq->prev_runnable_sum);
		}


		policy = cpufreq_cpu_get_raw(cpumask_first(&cluster_online_cpus));
		if (policy == NULL)
			scx_gov_err("NULL policy [%d]\n",
					cpumask_first(&cluster_online_cpus));

		sg_policy = policy->governor_data;
		if (GOV_MAX_UTIL == sg_policy->tunables->cpu_util_policy)
			prev_runnable_sum = pmax;
		else
			prev_runnable_sum = psum / cpumask_weight(&cluster_online_cpus);

		scx_gov_update_cpufreq(policy, prev_runnable_sum);
	}

	spin_lock_irqsave(&new_sched_ravg_window_lock, flags);
	if (unlikely(new_scx_sched_ravg_window != scx_sched_ravg_window)) {
		srq = &per_cpu(scx_sched_rq_stats, smp_processor_id());
		if (wc < srq->window_start + new_scx_sched_ravg_window) {
			scx_sched_ravg_window = new_scx_sched_ravg_window;
#ifdef CONFIG_HMBIRD_SCHED_GKI
			scx_fixup_window_dep();
#endif
		}
	}
	spin_unlock_irqrestore(&new_sched_ravg_window_lock, flags);

	for_each_cpu(cpu, &lock_cpus) {
		raw_spin_unlock(&cpu_rq(cpu)->__lock);
	}
#endif
}

#ifndef CONFIG_SCX_USE_UTIL_TRACK
static void scxgov_update_freq(struct waltgov_callback *cb, u64 time, unsigned int flags)
{
	struct scx_gov_cpu *sg_cpu = container_of(cb, struct scx_gov_cpu, cb);
	struct cpufreq_policy *policy;
	u64 sum = 0, prev_runnable_sum = 0;
	int cpu;

	if ((flags & WALT_CPUFREQ_ROLLOVER) && !(flags & WALT_CPUFREQ_CONTINUE)) {
		policy = sg_cpu->sg_policy->policy;
		for_each_cpu(cpu, policy->cpus) {
			struct walt_rq *wrq = &per_cpu(walt_rq, cpu);
			sum = wrq->prev_runnable_sum_fixed;
			prev_runnable_sum = max(prev_runnable_sum, sum);
		}
		scx_gov_update_cpufreq(policy,  prev_runnable_sum);
	}
}
#else
#ifdef CONFIG_ARCH_MEDIATEK
static unsigned int scx_boost_get_cpufreq_tick_hit(struct cpufreq_policy *policy, struct scx_gov_policy *sg_policy)
{
	unsigned int next_f;
	if (sg_policy->next_freq <= boost_policy_params.bottom_freq) {
		next_f = boost_policy_params.bottom_freq;
	} else {
		next_f = mult_frac(sg_policy->next_freq, boost_policy_params.boost_weight, 100);
	}
	next_f = soft_freq_clamp(sg_policy, next_f);
	next_f = cpufreq_driver_resolve_freq(policy, next_f);

	return next_f;
}

static void scx_boost_update_cpufreq(struct update_util_data *update_util, u64 time, unsigned int flags)
{
	struct scx_gov_cpu *sg_cpu = container_of(update_util, struct scx_gov_cpu, update_util);
	struct cpufreq_policy *policy;
	struct scx_gov_policy *sg_policy;
	unsigned long irq_flags;
	unsigned int next_f = 0;
	int cluster_id;

	if ((flags & HMBIRD_TICK_HIT_BOOST) || (flags & HMBIRD_EXIT_PHASE_BOOST)) {
		cluster_id = topology_cluster_id(sg_cpu->cpu);
		if (gov_flag[cluster_id] == 0)
			return;
		policy = cpufreq_cpu_get_raw(sg_cpu->cpu);
		if (!policy)
			return;
		sg_policy = policy->governor_data;
		if (!sg_policy)
			return;
		raw_spin_lock_irqsave(&sg_policy->update_lock, irq_flags);
		if (flags & HMBIRD_TICK_HIT_BOOST) {
			next_f = scx_boost_get_cpufreq_tick_hit(policy, sg_policy);
		} else if (flags & HMBIRD_EXIT_PHASE_BOOST) {
			next_f = cpufreq_driver_resolve_freq(policy, policy->max);
		} else {
			goto unlock;
		}
		if (sg_policy->next_freq == next_f)
			goto unlock;
		sg_policy->next_freq = next_f;
		scx_boost_systrace_c(policy->cpu, next_f);
		gov_trace_printk("cluster[%d] freq[%d] fast[%d]\n",
				policy->cpu, next_f, policy->fast_switch_enabled);
		scx_gov_boost_cluster[cluster_id] = true;
		if (policy->fast_switch_enabled)
			cpufreq_driver_fast_switch(policy, next_f);
		else
			kthread_queue_work(&sg_policy->worker, &sg_policy->work);

unlock:
		raw_spin_unlock_irqrestore(&sg_policy->update_lock, irq_flags);
	}
}
#endif
#endif

static int scx_gov_kthread_create(struct scx_gov_policy *sg_policy)
{
	struct task_struct *thread;
	struct sched_attr attr = {
		.size		= sizeof(struct sched_attr),
		.sched_policy	= SCHED_DEADLINE,
		.sched_flags	= SCHED_FLAG_SUGOV,
		.sched_nice	= 0,
		.sched_priority	= 0,
		/*
		 * Fake (unused) bandwidth; workaround to "fix"
		 * priority inheritance.
		 */
		.sched_runtime	=  1000000,
		.sched_deadline = 10000000,
		.sched_period	= 10000000,
	};
	struct cpufreq_policy *policy = sg_policy->policy;
	int ret;

	/* kthread only required for slow path */
	if (policy->fast_switch_enabled)
		return 0;

	kthread_init_work(&sg_policy->work, scx_gov_work);
	kthread_init_worker(&sg_policy->worker);
	thread = kthread_create(kthread_worker_fn, &sg_policy->worker,
				"scx_gov:%d",
				cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("failed to create scx_gov thread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	ret = sched_setattr_nocheck(thread, &attr);
	if (ret) {
		kthread_stop(thread);
		pr_warn("%s: failed to set SCHED_DEADLINE\n", __func__);
		return ret;
	}

	sg_policy->thread = thread;
	kthread_bind_mask(thread, policy->related_cpus);
	mutex_init(&sg_policy->work_lock);

	wake_up_process(thread);

	return 0;
}

static void scx_gov_kthread_stop(struct scx_gov_policy *sg_policy)
{
	/* kthread only required for slow path */
	if (sg_policy->policy->fast_switch_enabled)
		return;

	kthread_flush_worker(&sg_policy->worker);
	kthread_stop(sg_policy->thread);
	mutex_destroy(&sg_policy->work_lock);
}

static struct scx_gov_tunables *scx_gov_tunables_alloc(struct scx_gov_policy *sg_policy)
{
	struct scx_gov_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (tunables) {
		gov_attr_set_init(&tunables->attr_set, &sg_policy->tunables_hook);
		if (!have_governor_per_policy())
			global_tunables = tunables;
	}
	return tunables;
}

static void scx_gov_tunables_free(struct scx_gov_tunables *tunables)
{
	if (!have_governor_per_policy())
		global_tunables = NULL;

	kfree(tunables);
}

#define DEFAULT_HISPEED_LOAD 90
static void scx_gov_tunables_save(struct cpufreq_policy *policy,
		struct scx_gov_tunables *tunables)
{
	int cpu;
	struct scx_gov_tunables *cached = per_cpu(cached_tunables, policy->cpu);

	if (!cached) {
		cached = kzalloc(sizeof(*tunables), GFP_KERNEL);
		if (!cached)
			return;

		for_each_cpu(cpu, policy->related_cpus)
			per_cpu(cached_tunables, cpu) = cached;
	}
}

/*********************************
 * rebuild scx cluster
 *********************************/

static inline void move_list(struct list_head *dst, struct list_head *src)
{
	struct list_head *first, *last;

	first = src->next;
	last = src->prev;

	first->prev = dst;
	dst->prev = last;
	last->next = dst;

	/* Ensure list sanity before making the head visible to all CPUs. */
	smp_mb();
	dst->next = first;
}

static void get_possible_siblings(int cpuid, struct cpumask *cluster_cpus)
{
	int cpu;
	struct cpu_topology *cpu_topo, *cpuid_topo = &cpu_topology[cpuid];

	if (cpuid_topo->cluster_id == -1)
		return;

	for_each_possible_cpu(cpu) {
		cpu_topo = &cpu_topology[cpu];

		if (cpuid_topo->cluster_id != cpu_topo->cluster_id)
			continue;
		cpumask_set_cpu(cpu, cluster_cpus);
	}
}

static void insert_cluster(struct scx_sched_cluster *cluster,
					struct list_head *head)
{
	struct scx_sched_cluster *tmp;
	struct list_head *iter = head;

	list_for_each_entry(tmp, head, list) {
		if (arch_scale_cpu_capacity(cpumask_first(&cluster->cpus))
			< arch_scale_cpu_capacity(cpumask_first(&tmp->cpus)))
			break;
		iter = &tmp->list;
	}

	list_add(&cluster->list, iter);
}

static void cleanup_clusters(struct list_head *head)
{
	struct scx_sched_cluster *cluster, *tmp;

	list_for_each_entry_safe(cluster, tmp, head, list) {
		list_del(&cluster->list);
		num_sched_clusters--;
		kfree(cluster);
	}
}

static struct scx_sched_cluster *alloc_new_cluster(const struct cpumask *cpus)
{
	struct scx_sched_cluster *cluster = NULL;

	cluster = kzalloc(sizeof(struct scx_sched_cluster), GFP_ATOMIC);
	WARN_ON(!cluster);

	INIT_LIST_HEAD(&cluster->list);
	cluster->cpus = *cpus;

	return cluster;
}

static inline void add_cluster(const struct cpumask *cpus,
					struct list_head *head)
{
	struct scx_sched_cluster *cluster = NULL;

	cluster = alloc_new_cluster(cpus);
	insert_cluster(cluster, head);

	scx_cluster[num_sched_clusters] = cluster;

	num_sched_clusters++;
}

static inline void assign_cluster_ids(struct list_head *head)
{
	struct scx_sched_cluster *cluster;
	unsigned int cpu;

	list_for_each_entry(cluster, head, list) {
		cpu = cpumask_first(&cluster->cpus);
		cluster->id = topology_cluster_id(cpu);
		scx_gov_debug("assign cluster[%d] cluster_id[%d]\n",
							cpu, cluster->id);
	}
}

static bool scx_build_clusters(void)
{
	struct cpumask cpus = *cpu_possible_mask;
	struct cpumask cluster_cpus;
	struct list_head new_head;
	int i;

	INIT_LIST_HEAD(&cluster_head);
	INIT_LIST_HEAD(&new_head);

	/* 
	 * If this work failed, 
	 * our cluster_head can still used with only one cluster struct
	 */
	for_each_cpu(i, &cpus) {
		cpumask_clear(&cluster_cpus);
		get_possible_siblings(i, &cluster_cpus);
		if (cpumask_empty(&cluster_cpus)) {
			cleanup_clusters(&new_head);
			return false;
		}
		cpumask_andnot(&cpus, &cpus, &cluster_cpus);
		add_cluster(&cluster_cpus, &new_head);
	}

	assign_cluster_ids(&new_head);
	move_list(&cluster_head, &new_head);
	return true;
}
/*********************************
 * rebuild scx cluster done
 *********************************/

static int scx_gov_init(struct cpufreq_policy *policy)
{
	struct scx_gov_policy *sg_policy;
	struct scx_gov_tunables *tunables;
	int ret = 0;
	int i;

	/* State should be equivalent to EXIT */
	if (policy->governor_data)
		return -EBUSY;

	cpufreq_enable_fast_switch(policy);

	sg_policy = scx_gov_policy_alloc(policy);
	if (!sg_policy) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}

	ret = scx_gov_kthread_create(sg_policy);
	if (ret)
		goto free_sg_policy;

	mutex_lock(&global_tunables_lock);

	if (global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto stop_kthread;
		}
		policy->governor_data = sg_policy;
		sg_policy->tunables = global_tunables;

		gov_attr_set_get(&global_tunables->attr_set,
						&sg_policy->tunables_hook);
		goto out;
	}

	tunables = scx_gov_tunables_alloc(sg_policy);
	if (!tunables) {
		ret = -ENOMEM;
		goto stop_kthread;
	}
	for (i = 0; i < MAX_TL_SECS; i++) {
		tunables->target_loads[i] = DEFAULT_TARGET_LOAD;
		tunables->tl_thres[i] = MIN_POSSIBLE_FREQ;
	}
	tunables->policy = policy;
	tunables->cpu_util_policy = GOV_MAX_UTIL;
	tunables->soft_freq_max = -1;
	tunables->soft_freq_min = -1;
	tunables->apply_freq_immediately = true;

	policy->governor_data = sg_policy;
	sg_policy->tunables = tunables;

	ret = kobject_init_and_add(&tunables->attr_set.kobj,
					&scx_gov_tunables_ktype,
				   	get_governor_parent_kobj(policy), "%s",
				   	cpufreq_scx_gov.name);
	if (ret)
		goto fail;

	policy->dvfs_possible_from_any_cpu = 1;

out:
	mutex_unlock(&global_tunables_lock);

	scx_gov_debug("init cluster[%d] done.\n", cpumask_first(policy->related_cpus));
	return 0;

fail:
	kobject_put(&tunables->attr_set.kobj);
	policy->governor_data = NULL;
	scx_gov_tunables_free(tunables);

stop_kthread:
	scx_gov_kthread_stop(sg_policy);
	mutex_unlock(&global_tunables_lock);

free_sg_policy:
	scx_gov_policy_free(sg_policy);

disable_fast_switch:
	cpufreq_disable_fast_switch(policy);

	pr_err("initialization failed (error %d)\n", ret);
	return ret;
}

static void scx_gov_exit(struct cpufreq_policy *policy)
{
	struct scx_gov_policy *sg_policy = policy->governor_data;
	struct scx_gov_tunables *tunables = sg_policy->tunables;
	unsigned int count;

	mutex_lock(&global_tunables_lock);

	count = gov_attr_set_put(&tunables->attr_set,
				&sg_policy->tunables_hook);
	policy->governor_data = NULL;
	if (!count) {
		scx_gov_tunables_save(policy, tunables);
		scx_gov_tunables_free(tunables);
	}

	mutex_unlock(&global_tunables_lock);

	scx_gov_kthread_stop(sg_policy);
	scx_gov_cpu_reset(sg_policy);
	scx_gov_policy_free(sg_policy);
	cpufreq_disable_fast_switch(policy);

	scx_gov_debug("exit cluster[%d] done.\n", cpumask_first(policy->related_cpus));
}

static int scx_gov_start(struct cpufreq_policy *policy)
{
	struct scx_gov_policy *sg_policy = policy->governor_data;
	unsigned int cpu, cluster_id;

	sg_policy->next_freq = 0;

	for_each_cpu(cpu, policy->cpus) {
		struct scx_gov_cpu *sg_cpu = &per_cpu(scx_gov_cpu, cpu);

		memset(sg_cpu, 0, sizeof(*sg_cpu));
		sg_cpu->cpu			= cpu;
		sg_cpu->sg_policy		= sg_policy;
#ifndef CONFIG_SCX_USE_UTIL_TRACK
		waltgov_add_callback(cpu, &sg_cpu->cb, scxgov_update_freq);
#else
		cpufreq_add_update_util_hook(cpu, &sg_cpu->update_util, scx_boost_update_cpufreq);
#endif
	}

	cpu = cpumask_first(policy->related_cpus);
	cluster_id = topology_cluster_id(cpu);

	/* backup efficiencies_available, set scx efficiencies_available is false*/
	sg_policy->backup_efficiencies_available = policy->efficiencies_available;
	policy->efficiencies_available = false;

	if (cluster_id < MAX_CLUSTERS) {
		gov_flag[cluster_id] = 1;
		scx_gov_boost_cluster[cluster_id] = false;
		scx_gov_boost_cluster_last[cluster_id] = false;
	}

	scx_gov_debug("start cluster[%d] cluster_id[%d] gov\n", cpu, cluster_id);
	return 0;
}

static void scx_gov_stop(struct cpufreq_policy *policy)
{
	struct scx_gov_policy *sg_policy = policy->governor_data;
	unsigned int cpu, cluster_id;

	for_each_cpu(cpu, policy->cpus) {
#ifndef CONFIG_SCX_USE_UTIL_TRACK
		waltgov_remove_callback(cpu);
#else
		cpufreq_remove_update_util_hook(cpu);
#endif
	}

	if (!policy->fast_switch_enabled) {
		irq_work_sync(&scx_cpufreq_irq_work);
		kthread_cancel_work_sync(&sg_policy->work);
	}

	/* restore efficiencies_available */
	policy->efficiencies_available = sg_policy->backup_efficiencies_available;

	cpu = cpumask_first(policy->related_cpus);
	cluster_id = topology_cluster_id(cpu);
	if (cluster_id < MAX_CLUSTERS) {
		gov_flag[cluster_id] = 0;
#ifdef CONFIG_ARCH_MEDIATEK
		if (scx_gov_boost_cluster[cluster_id] || scx_gov_boost_cluster_last[cluster_id]) {
			scx_boost_systrace_c(cpu, 0);
		}
#endif
	}

	scx_gov_debug("stop cluster[%d] cluster_id[%d] gov\n", cpu, cluster_id);
	synchronize_rcu();
}

static void scx_gov_limits(struct cpufreq_policy *policy)
{
	struct scx_gov_policy *sg_policy = policy->governor_data;
	unsigned long flags, now;
	unsigned int freq, final_freq;

	if (!policy->fast_switch_enabled) {
		mutex_lock(&sg_policy->work_lock);
		cpufreq_policy_apply_limits(policy);
		mutex_unlock(&sg_policy->work_lock);
	} else {
		raw_spin_lock_irqsave(&sg_policy->update_lock, flags);
		freq = policy->cur;
		now = ktime_get_ns();

		final_freq = cpufreq_driver_resolve_freq(policy, freq);
		cpufreq_driver_fast_switch(policy, final_freq);

		raw_spin_unlock_irqrestore(&sg_policy->update_lock, flags);
	}
}

struct cpufreq_governor cpufreq_scx_gov = {
	.name			= "scx",
	.owner			= THIS_MODULE,
	.flags			= CPUFREQ_GOV_DYNAMIC_SWITCHING,
	.init			= scx_gov_init,
	.exit			= scx_gov_exit,
	.start			= scx_gov_start,
	.stop			= scx_gov_stop,
	.limits			= scx_gov_limits,
};

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_SCX
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &cpufreq_scx_gov;
}
#endif

#ifdef CONFIG_HMBIRD_SCHED_GKI
struct ctl_table scx_gov_table[] = {
	{
		.procname	= "scx_gov_debug",
		.data		= &sysctl_scx_gov_debug,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0666,
		.proc_handler	= proc_dointvec,
	},
	{ }
};

struct ctl_table scx_gov_base_table[] = {
	{
		.procname	= "scx_gov",
		.mode		= 0555,
		.child		= scx_gov_table,
	},
	{ },
};

void scx_gov_sysctl_init(void)
{
	struct ctl_table_header *hdr;

	sysctl_scx_gov_debug = 0;
	hdr = register_sysctl_table(scx_gov_base_table);
	kmemleak_not_leak(hdr);
}
#endif

int scx_cpufreq_init(void)
{
	int ret = 0;
	struct scx_sched_cluster *cluster = NULL;
#ifdef CONFIG_HMBIRD_SCHED_GKI
	scx_gov_sysctl_init();
#endif
	ret = cpufreq_register_governor(&cpufreq_scx_gov);
	if (ret)
		return ret;

	if (!scx_build_clusters()) {
		ret = -1;
		scx_gov_err("failed to build sched cluster\n");
		goto out;
	}

	for_each_sched_cluster(cluster)
		scx_gov_debug("num_cluster=%d id=%d"
			      "cpumask=%*pbl capacity=%lu num_cpus=%d\n",
			      num_sched_clusters, cluster->id,
			      cpumask_pr_args(&cluster->cpus),
			      arch_scale_cpu_capacity(cpumask_first(&cluster->cpus)),
			      num_possible_cpus());

	init_irq_work(&scx_cpufreq_irq_work, scx_irq_work);
	return ret;

out:
	cpufreq_unregister_governor(&cpufreq_scx_gov);
	return ret;
}
