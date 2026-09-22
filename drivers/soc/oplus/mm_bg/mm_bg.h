/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _OPLUS_MM_BG_H
#define _OPLUS_MM_BG_H

#include <linux/types.h>

struct task_struct;

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

/* binder_strategy debug knobs. Defaults match the built-in behavior. */
extern int oplus_binder_async_ux;
extern int oplus_binder_fg_list_async_first;

#endif /* _OPLUS_MM_BG_H */
