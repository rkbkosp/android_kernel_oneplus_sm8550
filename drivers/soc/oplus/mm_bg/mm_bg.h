/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _OPLUS_MM_BG_H
#define _OPLUS_MM_BG_H

#include <linux/types.h>

struct task_struct;
struct proc_dir_entry;

struct proc_dir_entry *oplus_mem_proc_dir(void);

/*
 * UX bit lives in android_vendor_data1[OPLUS_UX_VENDOR_SLOT].
 * Slot 0 is the start of struct walt_task_struct and is not free.
 */
#define OPLUS_UX_VENDOR_SLOT 49

bool oplus_sched_assist_enabled(void);
void oplus_task_set_ux(struct task_struct *p, bool on);
bool oplus_task_is_ux(struct task_struct *p);

bool oplus_uid_is_foreground(u32 uid);
bool oplus_uid_is_frozen(u32 uid);
/*
 * True when the target process must be treated as frozen: the binder driver's
 * own freeze flag (set by the framework through BINDER_FREEZE when it freezes
 * an app) or the frozen uid table. Defined in binder_strategy.c so the event
 * hooks and the spawn strategy cannot drift apart.
 */
struct binder_proc;
bool oplus_proc_is_frozen(struct binder_proc *proc);

/* binder_strategy debug knobs. Defaults match the built-in behavior. */
extern int oplus_binder_async_ux;
extern int oplus_binder_fg_list_async_first;

#endif /* _OPLUS_MM_BG_H */
