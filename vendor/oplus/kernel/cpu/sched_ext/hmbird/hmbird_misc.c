#include <linux/delay.h>
#include "hmbird_sched.h"

struct yield_opt_params yield_opt_params = {
	.enable = 0,
	.frame_per_sec = 120,
	.frame_time_ns = NSEC_PER_SEC / 120,
	.yield_headroom = 10,
};

DEFINE_PER_CPU(struct sched_yield_state, ystate);

static noinline bool check_scx_enabled(void)
{
	return atomic_read(&__scx_ops_enabled);
}

static inline void hmbird_yield_state_update(struct sched_yield_state *ys)
{
	int yield_headroom = yield_opt_params.yield_headroom;

	if (!raw_spin_is_locked(&ys->lock))
		return;

	if (ys->yield_cnt >= DEFAULT_YIELD_SLEEP_TH || ys->sleep_times > 1
						|| ys->yield_cnt_after_sleep > yield_headroom) {
		ys->sleep = min(ys->sleep + yield_headroom * YIELD_DURATION, MAX_YIELD_SLEEP);
	} else if (!ys->yield_cnt && (ys->sleep_times == 1) && !ys->yield_cnt_after_sleep) {
		ys->sleep = max(ys->sleep - yield_headroom * YIELD_DURATION, MIN_YIELD_SLEEP);
	}
	ys->yield_cnt = 0;
	ys->sleep_times = 0;
	ys->yield_cnt_after_sleep = 0;
}

void hmbird_skip_yield_handler(void *unused, long *skip)
{
	unsigned long flags, sleep_now = 0;
	struct sched_yield_state *ys;
	int cpu = raw_smp_processor_id(), cont_yield, new_frame;
	int frame_time_ns = yield_opt_params.frame_time_ns, yield_headroom = yield_opt_params.yield_headroom;
	u64 wc;

	if (!check_scx_enabled() || !yield_opt_params.enable)
		return;

	if (get_hmbird_cpu_exclusive(cpu) && !(*skip)) {
		wc = sched_clock();
		ys = &per_cpu(ystate, cpu);
		raw_spin_lock_irqsave(&ys->lock, flags);

		cont_yield = (wc - ys->last_yield_time) < MIN_YIELD_SLEEP;
		new_frame = (wc - ys->last_update_time) > (frame_time_ns >> 1);

		if (!cont_yield && new_frame) {
			hmbird_yield_state_update(ys);
			ys->last_update_time = wc;
			ys->sleep_end = ys->last_yield_time + frame_time_ns - yield_headroom * YIELD_DURATION;
		}

		if (ys->sleep > MIN_YIELD_SLEEP || ys->yield_cnt >= DEFAULT_YIELD_SLEEP_TH) {
			*skip = true;

			sleep_now = ys->sleep_times ?
							max(ys->sleep >> ys->sleep_times, (unsigned long)MIN_YIELD_SLEEP):ys->sleep;
			if (wc + sleep_now > ys->sleep_end) {
				u64 delta = ys->sleep_end - wc;
				if (ys->sleep_end > wc && delta > 3 * YIELD_DURATION)
					sleep_now = delta;
				else
					sleep_now = 0;
			}
			raw_spin_unlock_irqrestore(&ys->lock, flags);
			if (sleep_now) {
				sleep_now = div64_u64(sleep_now, 1000);
				usleep_range_state(sleep_now, sleep_now, TASK_IDLE);
			}
			ys->sleep_times++;
			ys->last_yield_time = sched_clock();
			return;
		}
		if (ys->sleep_times)
			ys->yield_cnt_after_sleep++;
		else
			(ys->yield_cnt)++;
		ys->last_yield_time = wc;
		raw_spin_unlock_irqrestore(&ys->lock, flags);
	}
}

#ifdef CONFIG_ARCH_MEDIATEK
#if IS_ENABLED(CONFIG_MTK_AEE_IPANIC)
//#include "../../../drivers/misc/mediatek/aee/mrdump/mrdump_mini.h"
extern void oplus_mrdump_mini_add_misc(unsigned long addr, unsigned long size,
		unsigned long start, char *name);
#endif
#endif /* CONFIG_ARCH_MEDIATEK */

static void android_vh_hmbird_update_load_handler(
					void *unused, struct task_struct *p,
					struct rq *rq, int event, u64 wallclock)
{
#ifdef CONFIG_SCX_USE_UTIL_TRACK
	/*rq lock musted be held here*/
	struct scx_sched_rq_stats *srq;
	if (!(rq->clock_update_flags & RQCF_UPDATED))
		update_rq_clock(rq);
	srq = &per_cpu(scx_sched_rq_stats, cpu_of(rq));
	scx_update_task_ravg(p, rq, event, max(rq_clock(rq), srq->latest_clock));
#endif
}

static void android_vh_hmbird_init_task_handler(
					void *unused, struct task_struct *p)
{
#ifdef CONFIG_SCX_USE_UTIL_TRACK
	scx_sched_init_task(p);
#endif
}

#ifdef CONFIG_OPLUS_SYSTEM_KERNEL_QCOM
extern void walt_disable_wait_for_completion(void);
extern void walt_enable_wait_for_completion(void);
#endif
static void android_rvh_hmbird_update_load_enable_handler(
					void *unused, bool enable)
{
#ifdef CONFIG_SCX_USE_UTIL_TRACK
	slim_walt_enable(enable);
#ifdef CONFIG_OPLUS_SYSTEM_KERNEL_QCOM
	preempt_enable();
	if (enable)
		walt_disable_wait_for_completion();
	else
		walt_enable_wait_for_completion();
	preempt_disable();
#endif
#endif
}

static void android_vh_get_util_handler(
			void *unused, int cpu, struct task_struct *p, u64 *util)
{
#ifdef CONFIG_OPLUS_SYSTEM_KERNEL_QCOM
	struct walt_task_struct *wts;
	struct walt_rq *wrq;
	u64 prev_runnable_sum;

	if ((cpu < 0) && NULL == p)
		return;

	if (p) {
		wts = (struct walt_task_struct *) p->android_vendor_data1;
		*util = wts->demand_scaled;
	} else {
		wrq = &per_cpu(walt_rq, cpu);
		prev_runnable_sum = wrq->prev_runnable_sum_fixed;
		do_div(prev_runnable_sum, wrq->prev_window_size >> SCHED_CAPACITY_SHIFT);
		*util = prev_runnable_sum;
	}
#else
	if ((cpu < 0) && NULL == p)
		return;
	if (p) {
		*util = p->scx->sts.demand_scaled;
	} else {
		*util = scx_cpu_util(cpu);
	}
#endif /*CONFIG_OPLUS_SYSTEM_KERNEL_QCOM*/
}

struct hmbird_ops_t {
	bool (*task_is_scx)(struct task_struct *p);
};
struct walt_ops_t {
	bool (*scx_enable)(void);
	bool (*check_non_task)(void);
};
extern void register_hmbird_sched_ops(struct hmbird_ops_t *ops);
#ifdef CONFIG_OPLUS_SYSTEM_KERNEL_QCOM
extern void register_walt_ops(struct walt_ops_t *ops);
#endif /*CONFIG_OPLUS_SYSTEM_KERNEL_QCOM*/
/* Ops must global variables */
static struct hmbird_ops_t hops;
#ifdef CONFIG_OPLUS_SYSTEM_KERNEL_QCOM
static struct walt_ops_t wops;

static noinline bool check_non_ext_task(void)
{
	return  atomic_read(&non_ext_task);
}
#endif /*CONFIG_OPLUS_SYSTEM_KERNEL_QCOM*/

static void register_helper_ops(void)
{
	hops.task_is_scx = task_is_scx;
#ifdef CONFIG_OPLUS_SYSTEM_KERNEL_QCOM
	wops.scx_enable = check_scx_enabled;
	wops.check_non_task = check_non_ext_task;
#endif /*CONFIG_OPLUS_SYSTEM_KERNEL_QCOM*/
	register_hmbird_sched_ops(&hops);
#ifdef CONFIG_OPLUS_SYSTEM_KERNEL_QCOM
	register_walt_ops(&wops);
#endif /*CONFIG_OPLUS_SYSTEM_KERNEL_QCOM*/
}

static void register_hooks(void)
{
	int ret;

	REGISTER_TRACE_VH(android_vh_hmbird_update_load,
				android_vh_hmbird_update_load_handler);
	REGISTER_TRACE_VH(android_vh_hmbird_init_task,
				android_vh_hmbird_init_task_handler);
#ifdef CONFIG_OPLUS_SYSTEM_KERNEL_QCOM
	REGISTER_TRACE_RVH(android_vh_hmbird_update_load_enable,
				android_rvh_hmbird_update_load_enable_handler);
#else
	REGISTER_TRACE_RVH(android_rvh_hmbird_update_load_enable,
				android_rvh_hmbird_update_load_enable_handler);
#endif
	REGISTER_TRACE_VH(android_vh_get_util,
				android_vh_get_util_handler);
	REGISTER_TRACE_VH(android_rvh_before_do_sched_yield,
				hmbird_skip_yield_handler);
	register_helper_ops();
}

#ifdef CONFIG_ARCH_MEDIATEK
#if IS_ENABLED(CONFIG_MTK_AEE_IPANIC)
static void hmbird_minidump_init(void)
{
	unsigned long vaddr = 0;
	unsigned long size = 0;

	scx_get_md_info(&vaddr, &size);
	if (vaddr) {
		oplus_mrdump_mini_add_misc(vaddr, size, 0, "load");
	}

	pr_info("hmbird_minidump_init.\n");
}
#else
static void hmbird_minidump_init(void) {};
#endif /* CONFIG_MTK_AEE_IPANIC */
#else
static void hmbird_minidump_init(void) {};
#endif /* CONFIG_ARCH_MEDIATEK */

#if IS_ENABLED(CONFIG_QCOM_MINIDUMP)
static void hmbird_md_register(void)
{
	unsigned long vaddr, size;
	struct md_region hmbird_entry;

	scx_get_md_info(&vaddr, &size);
	if (!vaddr) {
		WARN_ON(1);
		return;
	}
	strlcpy(hmbird_entry.name, "HMBIRD", sizeof(hmbird_entry.name));
	hmbird_entry.virt_addr = (uintptr_t)vaddr;
	hmbird_entry.phys_addr = virt_to_phys((unsigned long*)vaddr);
	hmbird_entry.size = size;
	if (msm_minidump_add_region(&hmbird_entry) < 0)
		WARN_ON(1);
}
#endif

void hmbird_misc_init(void)
{
	int cpu;
	for_each_possible_cpu(cpu) {
		struct sched_yield_state *ys = &per_cpu(ystate, cpu);
		raw_spin_lock_init(&ys->lock);
	}

	hmbird_minidump_init();

	register_hooks();
#if IS_ENABLED(CONFIG_QCOM_MINIDUMP)
	hmbird_md_register();
#endif
}
