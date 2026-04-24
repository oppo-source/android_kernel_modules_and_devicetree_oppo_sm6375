/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2019 MediaTek Inc.
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM scx_hooks

#if !defined(_TRACE_SCX_HOOKS_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_SCX_HOOKS_H

#include <trace/hooks/vendor_hooks.h>
DECLARE_HOOK(android_vh_scx_select_cpu_dfl,
	TP_PROTO(struct task_struct *p, s32 *cpu),
	TP_ARGS(p, cpu));

DECLARE_HOOK(android_vh_scx_sched_lpm_disallowed_time,
	TP_PROTO(int cpu, int *timeout_allowed),
	TP_ARGS(cpu, timeout_allowed));

DECLARE_HOOK(android_vh_check_preempt_curr_scx,
	TP_PROTO(struct rq *rq, struct task_struct *p, int wake_flags, int *check_result),
	TP_ARGS(rq, p, wake_flags, check_result));

#endif /*_TRACE_SCX_HOOKS_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH ./hmbird_gki

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE scx_hooks
/* This part must be outside protection */
#include <trace/define_trace.h>

