#ifdef CONFIG_SCX_GAME_OPT_ENABLE
#include "scx_main.h"
#include <../kernel/oplus_cpu/sched/frame_boost/frame_group.h>
#include <../kernel/oplus_cpu/sched/frame_boost/frame_info.h>

static DEFINE_PER_CPU(struct freq_qos_request, scx_qos_min);
static DEFINE_PER_CPU(struct freq_qos_request *, gpa_qos_max);
static DEFINE_PER_CPU(struct cpu_freq_status *, gpa_cpu_stats);
static struct irq_work scx_frame_irq_work;
#define DEFAULT_CPU_UNITY_MAIN				7

int pid_unitymain = -1;
int cpu_unitymain = DEFAULT_CPU_UNITY_MAIN;
static unsigned int scx_use_freqqos = true;
unsigned int scx_boost_ctl = true;

struct cpu_freq_status {
	unsigned int min;
	unsigned int max;
};

bool is_unitymain(struct task_struct *p)
{
	return p->pid == pid_unitymain;
}

void scx_gpa_register_qos_max(int cpu, struct freq_qos_request *req_max,
					struct cpu_freq_status *game_cpu_stats)
{
	per_cpu(gpa_qos_max, cpu) = req_max;
	per_cpu(gpa_cpu_stats, cpu) = game_cpu_stats;
}
EXPORT_SYMBOL_GPL(scx_gpa_register_qos_max);

/*just used in boost strategy*/
static inline bool is_CoreThread(struct task_struct *p)
{
	if (6 == sched_prop_get_top_thread_id(p))
		return true;

	if ((find_idx_from_task(p) == SCHED_PROP_DEADLINE_LEVEL3)
				&& !strcmp(p->comm, "CoreThread")) {
		sched_set_sched_prop(p, SCHED_PROP_DEADLINE_LEVEL3
				| (6 << SCHED_PROP_TOP_THREAD_SHIFT));
		return true;
	}
	return false;
}

static inline void scx_freq_qos_request_exit(void)
{
	struct freq_qos_request *req;
	int cpu;
	for_each_present_cpu(cpu) {
		req = &per_cpu(scx_qos_min, cpu);
		if (req && freq_qos_request_active(req))
			freq_qos_remove_request(req);
	}
}

static int scx_freq_qos_request_init(void)
{
	unsigned int cpu;
	int ret;

	struct cpufreq_policy *policy;
	struct freq_qos_request *req;

	for_each_present_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy) {
			pr_err("%s: Failed to get cpufreq policy for cpu%d\n",
				__func__, cpu);
			ret = -EINVAL;
			goto cleanup;
		}

		req = &per_cpu(scx_qos_min, cpu);
		ret = freq_qos_add_request(&policy->constraints, req,
			FREQ_QOS_MIN, FREQ_QOS_MIN_DEFAULT_VALUE);
		if (ret < 0) {
			pr_err("%s: Failed to add min freq constraint (%d)\n",
				__func__, ret);
			cpufreq_cpu_put(policy);
			goto cleanup;
		}

		cpufreq_cpu_put(policy);
	}
	return 0;

cleanup:
	scx_freq_qos_request_exit();
	return ret;
}

unsigned int min_freq_ctl = 1478400;
static void __maybe_unused
scx_boost_cpu_minfreq(struct cpufreq_policy *policy, unsigned int min_freq)
{
	struct freq_qos_request *req, *req_gpa_max;
	struct cpu_freq_status *game_cpu_stats;

	req = &per_cpu(scx_qos_min, policy->cpu);
	req_gpa_max = per_cpu(gpa_qos_max, policy->cpu);
	game_cpu_stats = per_cpu(gpa_cpu_stats, policy->cpu);

	if (req_gpa_max && game_cpu_stats) {
		if (min_freq != FREQ_QOS_MIN_DEFAULT_VALUE
					&& min_freq > req_gpa_max->pnode.prio)
			freq_qos_update_request(req_gpa_max, min_freq);

		if (min_freq == FREQ_QOS_MIN_DEFAULT_VALUE) {
			/*reset gpa max freq*/
			freq_qos_update_request(req_gpa_max, game_cpu_stats->max);
		}
	}

	if (freq_qos_update_request(req, min_freq) < 0) {
		scx_use_freqqos = 0;
		pr_err("scx boost err\n");
	}
}

static int prev_frame_state;
static inline void scx_frame_systrace_c(int frame_state)
{
	if (prev_frame_state != frame_state) {
		char buf[256];
		snprintf(buf, sizeof(buf), "C|9999|frame|%d\n", frame_state);
		tracing_mark_write(buf);
		prev_frame_state = frame_state;
	}
}
static u64 boost_at;
static u64 should_boost;
static int frame_state;

static void scx_apply_boost(int boost)
{
	struct cpufreq_policy *policy;

	policy = cpufreq_cpu_get_raw(cpu_unitymain);
	if (scx_use_freqqos) {
		scx_boost_cpu_minfreq(policy, boost ? min_freq_ctl
					: FREQ_QOS_MIN_DEFAULT_VALUE);
	}

	debug_trace_printk("boost=%d minfreq=%u on cpu=%d\n",
			boost, min_freq_ctl, raw_smp_processor_id());
}

bool scx_gpa_could_limits_max(int cpu, unsigned int max_freq)
{
	struct cpufreq_policy *policy1, *policy2;
	if(!scx_stats_trace)
		return true;
	policy1 = cpufreq_cpu_get_raw(cpu);
	policy2 = cpufreq_cpu_get_raw(cpu_unitymain);

	return !(policy1 == policy2 && boost_at && (max_freq < min_freq_ctl));
}
EXPORT_SYMBOL_GPL(scx_gpa_could_limits_max);

static void scx_frame_irq_work_fn(struct irq_work *irq_work)
{
	u64 boost_at_tmp;
	if (frame_state) {
		if (!boost_at && should_boost) {
			if (jiffies - should_boost >= 3)
				should_boost = 0;
			else {
				boost_at = jiffies;
				goto boost;
			}
		}
	} else {
		boost_at_tmp = boost_at;
		if (boost_at_tmp && cmpxchg64(&boost_at, boost_at_tmp, 0)) {
			should_boost = 0;
			goto boost;
		}
	}
	return;
boost:
	scx_apply_boost(boost_at);
}

static struct hrtimer scx_frametimer;
static u64 timer_f;
#define SCX_VIRTUAL_FRAME_ON_120	(8300000ULL)
#define SCX_VIRTUAL_FRAME_OFF_120	(200000ULL)

static inline void scx_frametimer_start(u64 time_ns)
{
	if (!hrtimer_active(&scx_frametimer))
		hrtimer_start(&scx_frametimer, ns_to_ktime(time_ns), HRTIMER_MODE_REL);
}

static inline void scx_frametimer_cancel(void)
{
	if (!hrtimer_active(&scx_frametimer))
		return;

	hrtimer_cancel(&scx_frametimer);
}

#define __NR_NANOSLEEP 101

enum hrtimer_restart scx_update_frame_state(struct task_struct *prev,
					struct task_struct *next, bool virtual)
{
	struct pt_regs *regs;
	enum hrtimer_restart ret = HRTIMER_NORESTART;
	bool queue_work = true;
	bool rate = (120 == get_frame_rate(SF_FRAME_GROUP_ID));
	if (!rate) {
		/*The statistics collection in mode 120Hz*/
		if (cmpxchg(&frame_state, 1, 0))
			goto update;
	} else {
		if (virtual) {
			if (((timer_f == SCX_VIRTUAL_FRAME_ON_120)
					&& cmpxchg(&frame_state, 1, 0)) ||
					((timer_f == SCX_VIRTUAL_FRAME_OFF_120)
					&& !cmpxchg(&frame_state, 0, 1)))
				goto update;
		} else {
			if (is_unitymain(prev)) {
				regs = task_pt_regs(prev);
				if (regs->syscallno == __NR_NANOSLEEP) {
					if (!cmpxchg(&frame_state, 1, 0))
						queue_work = false;
					goto update;
				}
			} else if (is_unitymain(next)) {
				regs = task_pt_regs(next);
				if (regs->syscallno == __NR_NANOSLEEP) {
					if (cmpxchg(&frame_state, 0, 1))
						queue_work = false;
					goto update;
				}
			}
		}
	}
	return ret;
update:
	if (!virtual) {
		scx_frametimer_cancel();
		if (frame_state) {
			timer_f = SCX_VIRTUAL_FRAME_ON_120;
			scx_frametimer_start(timer_f);
		}
	} else {
		if (rate) {
			timer_f = frame_state ? SCX_VIRTUAL_FRAME_ON_120
						: SCX_VIRTUAL_FRAME_OFF_120;
			hrtimer_forward_now(&scx_frametimer, timer_f);
			ret = HRTIMER_RESTART;
		}
	}

	if (queue_work) {
		int cpu = cpumask_first(iso_masks.partial);
		if (cpu > nr_cpu_ids) {
			irq_work_queue(&scx_frame_irq_work);
		} else {
			irq_work_queue_on(&scx_frame_irq_work, cpu);
		}
	}
	if (dump_info & SCX_DEBUG_SYSTRACE)
		scx_frame_systrace_c((virtual && !frame_state) ? -1 : frame_state);
	return ret;
}

static enum hrtimer_restart frametimer_fn(struct hrtimer *h)
{
	return scx_update_frame_state(NULL, NULL, true);
}

void scx_init_frame_boost_for_sgame(void)
{
	boost_at = 0;
	should_boost = 0;
	frame_state = 0;
	prev_frame_state = 0;
	scx_freq_qos_request_init();
}

void scx_exit_frame_boost_for_sgame(void)
{
	scx_freq_qos_request_exit();
	scx_frametimer_cancel();
}

void scx_sgame_tick_update_boost(struct rq *rq)
{
	u64 boost_at_tmp;
	struct scx_entity *scx;
	if (unlikely(!rq->curr))
		return;
	scx = get_oplus_ext_entity(rq->curr);
	if (!scx)
		return;
	if (scx_boost_ctl && (scx->flags & SCX_TASK_QUEUED)
				&& !scx->slice && scx->gdsq_idx == 3
				&& is_CoreThread(rq->curr)) {
		if (!should_boost) {
			/*boost cpu 7*/
			should_boost = jiffies;
		}
	}
	boost_at_tmp = boost_at;
	if (boost_at_tmp && (jiffies - boost_at_tmp >= 3)) {
		if (cmpxchg64(&boost_at, boost_at_tmp, 0)) {
			should_boost = 0;
			scx_apply_boost(boost_at);
		}
	}
}
void scx_game_init_early(void)
{
	hrtimer_init(&scx_frametimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	scx_frametimer.function = frametimer_fn;
	init_irq_work(&scx_frame_irq_work, scx_frame_irq_work_fn);
}

#endif
