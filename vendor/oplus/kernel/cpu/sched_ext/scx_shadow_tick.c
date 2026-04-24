#include <linux/tick.h>
#include <kernel/time/tick-sched.h>
#include <trace/hooks/sched.h>

#ifdef CONFIG_HMBIRD_SCHED
#include "./hmbird/hmbird_sched.h"
#else
#include "./hmbird_gki/scx_main.h"
#endif

#define HIGHRES_WATCH_CPU       0
#ifdef CONFIG_HMBIRD_SCHED
extern unsigned int highres_tick_ctrl;
extern unsigned int highres_tick_ctrl_dbg;
static bool shadow_tick_enable(void) {return highres_tick_ctrl;}
static bool shadow_tick_dbg_enable(void) {return highres_tick_ctrl_dbg;}
#else
static bool shadow_tick_enable(void) {return true;}
static bool shadow_tick_dbg_enable(void) {return false;}
#endif

#define shadow_tick_printk(fmt, args...)	\
do {							\
	int cpu = smp_processor_id();			\
	if (shadow_tick_dbg_enable() && cpu == HIGHRES_WATCH_CPU)	\
		trace_printk("hmbird shadow tick :"fmt, args);	\
} while (0)


#define REGISTER_TRACE_VH(vender_hook, handler) \
{ \
	ret = register_trace_##vender_hook(handler, NULL); \
	if (ret) { \
		pr_err("failed to register_trace_"#vender_hook", ret=%d\n", ret); \
	} \
}

#define NUM_SHADOW_TICK_TIMER (3)
DEFINE_PER_CPU(struct hrtimer[NUM_SHADOW_TICK_TIMER], stt);
#define shadow_tick_timer(cpu, id) (&per_cpu(stt[id], (cpu)))

#define STOP_IDLE_TRIGGER     (1)
#define PERIODIC_TICK_TRIGGER (2)
static DEFINE_PER_CPU(u8, trigger_event);

static void sched_switch_handler(void *data, bool preempt, struct task_struct *prev,
		struct task_struct *next, unsigned int prev_state)
{
	int i, cpu = smp_processor_id();

	if (shadow_tick_enable() && (cpu_rq(cpu)->idle == prev)) {
		this_cpu_write(trigger_event, STOP_IDLE_TRIGGER);
		for (i = 0; i < NUM_SHADOW_TICK_TIMER; i++) {
			if (!hrtimer_active(shadow_tick_timer(cpu, i)))
				hrtimer_start(shadow_tick_timer(cpu, i),
					ns_to_ktime(1000000ULL * (i + 1)), HRTIMER_MODE_REL_PINNED);
		}
		if (shadow_tick_dbg_enable() && cpu == HIGHRES_WATCH_CPU)
			trace_printk("hmbird_sched : enter tick triggered by stop_idle events\n");
	}
}

#ifdef CONFIG_ARCH_MEDIATEK
struct tick_hit_params tick_hit_params = {
	.enable = 0,
	.jiffies_num = 2,
	.hit_count_thres = 6,
};

static void tick_hit_critical_task(struct task_struct *curr, struct rq *rq)
{
	struct oplus_task_struct *ots = get_oplus_task_struct(curr);

	if (!tick_hit_params.enable || IS_ERR_OR_NULL(ots))
		return;

	if ((!task_is_top_task(curr) || curr->pid != curr->tgid) && (curr->pid != scx_systemui_pid))
		return;

	if (ots->tick_hit_count == 0) {
		if (ots->start_jiffies == 0) {
			ots->start_jiffies = jiffies;
			ots->tick_hit_count = 1;
		} else {
			pr_err("hmbird: tick_hit_critical_task error!\n");
			ots->start_jiffies = 0;       //status reset
			ots->tick_hit_count = 0;
		}
	} else {
		if (time_before_eq(jiffies, ots->start_jiffies + tick_hit_params.jiffies_num)) {
			ots->tick_hit_count++;
			if (ots->tick_hit_count >= tick_hit_params.hit_count_thres) {
				cpufreq_update_util(rq, HMBIRD_TICK_HIT_BOOST);
			}
		} else {
			ots->tick_hit_count = 1;
			ots->start_jiffies = jiffies;
		}
	}
}
#endif

enum hrtimer_restart scheduler_tick_no_balance(struct hrtimer *timer)
{
	int cpu = smp_processor_id();
	struct rq *rq = cpu_rq(cpu);
	struct task_struct *curr = rq->curr;
	struct rq_flags rf;

	rq_lock(rq, &rf);
	update_rq_clock(rq);
#ifdef CONFIG_HMBIRD_SCHED_GKI
	scx_tick_entry(rq);
#endif
	curr->sched_class->task_tick(rq, curr, 0);
	shadow_tick_printk("enter 1ms tick on cpu%d \n", HIGHRES_WATCH_CPU);
#ifdef CONFIG_ARCH_MEDIATEK
	tick_hit_critical_task(curr, rq);
#endif
	rq_unlock(rq, &rf);
#ifdef CONFIG_HMBIRD_SCHED_GKI
	scx_scheduler_tick();
#endif
	return HRTIMER_NORESTART;
}

void shadow_tick_timer_init(void)
{
	int i, cpu;

	for_each_possible_cpu(cpu) {
		for (i = 0; i < NUM_SHADOW_TICK_TIMER; i++) {
			hrtimer_init(shadow_tick_timer(cpu, i),
					CLOCK_MONOTONIC, HRTIMER_MODE_REL);
			shadow_tick_timer(cpu, i)->function =
					&scheduler_tick_no_balance;
		}
	}
}

void start_shadow_tick_timer(void)
{
	int i, cpu = smp_processor_id();

	if (shadow_tick_enable()) {
		if (this_cpu_read(trigger_event) == STOP_IDLE_TRIGGER) {
			for (i = 0; i < NUM_SHADOW_TICK_TIMER; i++)
				hrtimer_cancel(shadow_tick_timer(cpu, i));
		}

		this_cpu_write(trigger_event, PERIODIC_TICK_TRIGGER);

		for (i = 0; i < NUM_SHADOW_TICK_TIMER; i++) {
			if (!hrtimer_active(shadow_tick_timer(cpu, i)))
				hrtimer_start(shadow_tick_timer(cpu, i),
						ns_to_ktime(1000000ULL * (i + 1)),
						HRTIMER_MODE_REL_PINNED);
			shadow_tick_printk("restart 1ms tick on cpu%d \n",
							HIGHRES_WATCH_CPU);
		}
	}
}

static void stop_shadow_tick_timer(void)
{
	int i, cpu = smp_processor_id();

	this_cpu_write(trigger_event, 0);
	for (i = 0; i < NUM_SHADOW_TICK_TIMER; i++)
		hrtimer_cancel(shadow_tick_timer(cpu, i));
	shadow_tick_printk("stop 1ms tick on cpu%d \n", HIGHRES_WATCH_CPU);
}

void android_vh_tick_nohz_idle_stop_tick_handler(void *unused, void *data)
{
	stop_shadow_tick_timer();
}

#ifdef CONFIG_HMBIRD_SCHED
static void scheduler_tick_handler(void *unused, struct rq *rq)
{
	start_shadow_tick_timer();
}
#endif


int scx_shadow_tick_init(void)
{
	int ret = 0;
	shadow_tick_timer_init();

	REGISTER_TRACE_VH(android_vh_tick_nohz_idle_stop_tick,
				android_vh_tick_nohz_idle_stop_tick_handler);
#ifdef CONFIG_HMBIRD_SCHED
	REGISTER_TRACE_VH(android_vh_scheduler_tick,
				scheduler_tick_handler);
#endif
	REGISTER_TRACE_VH(sched_switch, sched_switch_handler);
	return ret;
}
