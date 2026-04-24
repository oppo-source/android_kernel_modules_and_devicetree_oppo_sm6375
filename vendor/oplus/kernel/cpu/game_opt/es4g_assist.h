// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Oplus. All rights reserved.
 */

#ifndef __ES4G_ASSIST_H__
#define __ES4G_ASSIST_H__

#include <linux/sched.h>
#include <linux/sched/cputime.h>
#include <kernel/sched/sched.h>

/* define for debug trace */
#define DEBUG_SYSTRACE				(1 << 0)
#define DEBUG_FTRACE				(1 << 1)

/* define for sched ext */
#define MAX_NR_CPUS					(1 << 3)
#ifdef MAX_TASK_NR
#define MAX_KEY_THREAD_RECORD		((MAX_TASK_NR + 1) >> 1)
#else
#define MAX_KEY_THREAD_RECORD		MAX_NR_CPUS
#endif /* MAX_TASK_NR */
#define MAX_KEY_THREAD_PRIORITY		(0)
#define MAX_KEY_THREAD_PRIORITY_US	(MAX_KEY_THREAD_PRIORITY + 1)
#define MIN_KEY_THREAD_PRIORITY		(8)
#define KEY_THREAD_PRIORITY_COUNT	(MIN_KEY_THREAD_PRIORITY - MAX_KEY_THREAD_PRIORITY + 1)
#define ONE_PAGE_SIZE				(1 << 5)
#define KEY_THREAD_FLAG				(1 << 3)
#define TOP_TASK_SHIFT				(8)
#define TOP_TASK_MAX				(1 << TOP_TASK_SHIFT)
#ifndef TOP_TASK_BITS_MASK
#define	TOP_TASK_BITS_MASK			(TOP_TASK_MAX - 1)
#endif /* TOP_TASK_BITS_MASK */

enum es4g_isolate_type
{
	ES4G_ISOLATE_STRICT,
	ES4G_ISOLATE_PIPELINE,
	ES4G_ISOLATE_WEAK,
};

/**
 * task prop:
 *
 * bit0~8: top task
 * bit9~: for specific thread
 *
 * specific type:
 *
 * 0: common thread, unused
 * 1: pipeline thread
 * 2: debug or logging thread, which is the least critical
 * 3: temporary thread but high-load
 * 4: io related, such as preload
 * 5: network related, such as XXX_NETWORK
 * 6: periodic thread, not waken by critical thread
 * 7: periodic thread, waken by critical thread, such as core thread
 * 8: the most critical but transient thread, such as gc
 * 9: pipeline thread and pipeline cpu is isolated
 *
 */
enum es4g_task_prop_type
{
	ES4G_TASK_PROP_COMMON,
	ES4G_TASK_PROP_PIPELINE,
	ES4G_TASK_PROP_DEBUG_OR_LOG,
	ES4G_TASK_PROP_HIGH_LOAD,
	ES4G_TASK_PROP_IO,
	ES4G_TASK_PROP_NETWORK,
	ES4G_TASK_PROP_PERIODIC,
	ES4G_TASK_PROP_PERIODIC_AND_CRITICAL,
	ES4G_TASK_PROP_TRANSIENT_AND_CRITICAL,
	ES4G_TASK_PROP_ISOLATE,
	ES4G_TASK_PROP_MAX,
};

enum es4g_ctrl_cmd_id
{
	ES4G_FIRST_ID, /* reserved word */
	ES4G_COMMON_CTRL,
	ES4G_SET_CRITICAL_TASK,
	ES4G_SELECT_CPU_LIST,
	ES4G_SET_ISOLATE_CPUS,
	ES4G_MAX_ID,
};

enum es4g_ctrl_common_cmd_id
{
	ES4G_COMMON_CTRL_DEBUG_LEVEL,
	ES4G_COMMON_CTRL_PREEMPT_TYPE,
	ES4G_COMMON_CTRL_SET_SCHED_PROP,
	ES4G_COMMON_CTRL_UNSET_SCHED_PROP,
};

struct es4g_ctrl_info
{
	s64 data[ONE_PAGE_SIZE];
	size_t size;
};

#define ES4G_MAGIC 0xE0
#define CMD_ID_ES4G_COMMON_CTRL \
	_IOWR(ES4G_MAGIC, ES4G_COMMON_CTRL, struct es4g_ctrl_info)
#define CMD_ID_ES4G_SET_CRITICAL_TASK \
	_IOWR(ES4G_MAGIC, ES4G_SET_CRITICAL_TASK, struct es4g_ctrl_info)
#define CMD_ID_ES4G_SELECT_CPU_LIST \
	_IOWR(ES4G_MAGIC, ES4G_SELECT_CPU_LIST, struct es4g_ctrl_info)
#define CMD_ID_ES4G_SET_ISOLATE_CPUS \
	_IOWR(ES4G_MAGIC, ES4G_SET_ISOLATE_CPUS, struct es4g_ctrl_info)

enum es4g_preempt_policy_id
{
	ES4G_PREEMPT_POLICY_NONE,
	ES4G_PREEMPT_POLICY_PRIO_BASED,
};

int es4g_assist_init(void);
void es4g_assist_exit(void);
bool get_hmbird_cpu_exclusive(int cpu);

#endif /* __ES4G_ASSIST_H__ */
