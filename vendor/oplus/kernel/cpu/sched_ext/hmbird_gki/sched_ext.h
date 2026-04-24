#ifndef _OPLUS_SCHED_EXT_H
#define _OPLUS_SCHED_EXT_H
#undef CREATE_TRACE_POINTS
#include <../kernel/sched/sched.h>
#include <linux/llist.h>
#include <../kernel/oplus_cpu/sched/sched_assist/sa_common.h>

/*
 * Dispatch queue (dsq) is a simple FIFO which is used to buffer between the
 * scheduler core and the BPF scheduler. See the documentation for more details.
 */
struct scx_dispatch_q {
	raw_spinlock_t		lock;
	struct list_head	fifo;	/* processed in dispatching order */
	struct rb_root_cached	priq;	/* processed in p->scx.dsq_vtime order */
	u32			nr;
	u64			id;
	struct rhash_head	hash_node;
	struct llist_node	free_node;
	struct rcu_head		rcu;
	u64                     last_consume_at;
	bool                    is_timeout;
};


/* scx_entity.flags */
enum scx_ent_flags {
	SCX_TASK_QUEUED		= 1 << 0, /* on ext runqueue */
	SCX_TASK_BAL_KEEP	= 1 << 1, /* balance decided to keep current */
	SCX_TASK_ENQ_LOCAL	= 1 << 2, /* used by scx_select_cpu_dfl() to set SCX_ENQ_LOCAL */

	SCX_TASK_OPS_PREPPED	= 1 << 8, /* prepared for BPF scheduler enable */
	SCX_TASK_OPS_ENABLED	= 1 << 9, /* task has BPF scheduler enabled */

	SCX_TASK_WATCHDOG_RESET = 1 << 16, /* task watchdog counter should be reset */
	SCX_TASK_DEQD_FOR_SLEEP	= 1 << 17, /* last dequeue was for SLEEP */

	SCX_TASK_CURSOR		= 1 << 31, /* iteration cursor, not a task */
};

/* scx_entity.dsq_flags */
enum scx_ent_dsq_flags {
	SCX_TASK_DSQ_ON_PRIQ	= 1 << 0, /* task is queued on the priority queue of a dsq */
};

enum scx_enq_flags {
	/* expose select ENQUEUE_* flags as enums */
	SCX_ENQ_WAKEUP		= ENQUEUE_WAKEUP,
	SCX_ENQ_HEAD		= ENQUEUE_HEAD,

	/* high 32bits are SCX specific */

	/*
	 * Set the following to trigger preemption when calling
	 * scx_bpf_dispatch() with a local dsq as the target. The slice of the
	 * current task is cleared to zero and the CPU is kicked into the
	 * scheduling path. Implies %SCX_ENQ_HEAD.
	 */
	SCX_ENQ_PREEMPT		= 1LLU << 32,

	/*
	 * The task being enqueued was previously enqueued on the current CPU's
	 * %SCX_DSQ_LOCAL, but was removed from it in a call to the
	 * bpf_scx_reenqueue_local() kfunc. If bpf_scx_reenqueue_local() was
	 * invoked in a ->cpu_release() callback, and the task is again
	 * dispatched back to %SCX_LOCAL_DSQ by this current ->enqueue(), the
	 * task will not be scheduled on the CPU until at least the next invocation
	 * of the ->cpu_acquire() callback.
	 */
	SCX_ENQ_REENQ		= 1LLU << 40,

	/*
	 * The task being enqueued is the only task available for the cpu. By
	 * default, ext core keeps executing such tasks but when
	 * %SCX_OPS_ENQ_LAST is specified, they're ops.enqueue()'d with
	 * %SCX_ENQ_LAST and %SCX_ENQ_LOCAL flags set.
	 *
	 * If the BPF scheduler wants to continue executing the task,
	 * ops.enqueue() should dispatch the task to %SCX_DSQ_LOCAL immediately.
	 * If the task gets queued on a different dsq or the BPF side, the BPF
	 * scheduler is responsible for triggering a follow-up scheduling event.
	 * Otherwise, Execution may stall.
	 */
	SCX_ENQ_LAST		= 1LLU << 41,

	/*
	 * A hint indicating that it's advisable to enqueue the task on the
	 * local dsq of the currently selected CPU. Currently used by
	 * select_cpu_dfl() and together with %SCX_ENQ_LAST.
	 */
	SCX_ENQ_LOCAL		= 1LLU << 42,

	/* high 8 bits are internal */
	__SCX_ENQ_INTERNAL_MASK	= 0xffLLU << 56,

	SCX_ENQ_CLEAR_OPSS	= 1LLU << 56,
	SCX_ENQ_DSQ_PRIQ	= 1LLU << 57,
};

enum scx_deq_flags {
	/* expose select DEQUEUE_* flags as enums */
	SCX_DEQ_SLEEP		= DEQUEUE_SLEEP,

	/* high 32bits are SCX specific */

	/*
	 * The generic core-sched layer decided to execute the task even though
	 * it hasn't been dispatched yet. Dequeue from the BPF side.
	 */
	SCX_DEQ_CORE_SCHED_EXEC	= 1LLU << 32,
};

#define MAX_BPF_DSQS (10)
#define MIN_CGROUP_DL_IDX (5)      /* 8ms */
#define DEFAULT_CGROUP_DL_IDX (8)  /* 64ms */
#define NON_PERIOD_START	(5)
#define NON_PERIOD_END		(MAX_BPF_DSQS)
extern u32 SCX_BPF_DSQS_DEADLINE[MAX_BPF_DSQS];

#define SCHED_PROP_TOP_THREAD_SHIFT (8)
#define SCHED_PROP_TOP_THREAD_MASK  (0xf << SCHED_PROP_TOP_THREAD_SHIFT)
#define SCHED_PROP_DEADLINE_MASK (0xFF) /* deadline for ext sched class */
#define SCHED_PROP_DEADLINE_LEVEL1 (1)  /* 1ms for user-aware audio tasks */
#define SCHED_PROP_DEADLINE_LEVEL2 (2)  /* 2ms for user-aware touch tasks */
#define SCHED_PROP_DEADLINE_LEVEL3 (3)  /* 4ms for user aware dispaly tasks */
#define SCHED_PROP_DEADLINE_LEVEL4 (4)  /* 6ms */
#define SCHED_PROP_DEADLINE_LEVEL5 (5)  /* 8ms */
#define SCHED_PROP_DEADLINE_LEVEL6 (6)  /* 16ms */
#define SCHED_PROP_DEADLINE_LEVEL7 (7)  /* 32ms */
#define SCHED_PROP_DEADLINE_LEVEL8 (8)  /* 64ms */
#define SCHED_PROP_DEADLINE_LEVEL9 (9)  /* 128ms */

static inline int sched_prop_get_top_thread_id(struct task_struct *p)
{
	struct oplus_task_struct *ots = get_oplus_task_struct(p);

	if (!ots) {
		return -EPERM;
	}

	return ((ots->scx.sched_prop & SCHED_PROP_TOP_THREAD_MASK)
					>> SCHED_PROP_TOP_THREAD_SHIFT);
}

static inline int sched_set_sched_prop(struct task_struct *p, unsigned long sp)
{
	struct oplus_task_struct *ots = get_oplus_task_struct(p);

	if (!ots) {
		pr_err("scx_sched_ext: sched_set_sched_prop failed! fn=%s\n", __func__);
		return -EPERM;
	}

	ots->scx.sched_prop = sp;
	return 0;
}

static inline unsigned long sched_get_sched_prop(struct task_struct *p)
{
	struct oplus_task_struct *ots = get_oplus_task_struct(p);

	if (!ots) {
		pr_err("scx_sched_ext: sched_get_sched_prop failed! fn=%s\n", __func__);
		return (unsigned long)-1;
	}
	return ots->scx.sched_prop;
}

#endif /*_OPLUS_SCHED_EXT_H*/
