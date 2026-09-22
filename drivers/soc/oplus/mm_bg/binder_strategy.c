// SPDX-License-Identifier: GPL-2.0-only
/*
 * Binder UX hint and priority behavior on the hooks this tree already has.
 *
 * binder_transaction::android_vendor_data1 is owned by WALT: it stores
 * wts->boost in binder_set_priority_hook and restores it later. The UX
 * hint is a side table keyed by the transaction pointer, filled from
 * android_vh_alloc_oem_binder_struct and dropped from
 * android_vh_free_oem_binder_struct. This file does not write that word
 * and does not change WALT task selection.
 *
 * Debug proc names seen for the donor module:
 *   /proc/d_oplus_binder/async_ux
 *   /proc/d_oplus_binder/fg_list_async_first
 * Both default to 1, which is the behavior below.
 */

#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <uapi/linux/android/binder.h>
#include <trace/hooks/binder.h>

#include "binder_internal.h"
#include "mm_bg.h"

#define OPLUS_TXN_HINTS 64

struct oplus_txn_hint {
	struct binder_transaction *txn;
	bool ux_caller;
	bool sync_txn;
};

static struct oplus_txn_hint txn_hints[OPLUS_TXN_HINTS];
static DEFINE_SPINLOCK(hint_lock);

int oplus_binder_async_ux = 1;
int oplus_binder_fg_list_async_first = 1;

static const char oplus_binder_async_ux_path[] __used =
	"/proc/d_oplus_binder/async_ux";
static const char oplus_binder_fg_async_path[] __used =
	"/proc/d_oplus_binder/fg_list_async_first";

static void hint_put(struct binder_transaction *txn, bool ux, bool sync)
{
	int i, slot = -1;
	unsigned long flags;

	if (!txn)
		return;
	spin_lock_irqsave(&hint_lock, flags);
	for (i = 0; i < OPLUS_TXN_HINTS; i++) {
		if (txn_hints[i].txn == txn) {
			txn_hints[i].ux_caller = ux;
			txn_hints[i].sync_txn = sync;
			spin_unlock_irqrestore(&hint_lock, flags);
			return;
		}
		if (!txn_hints[i].txn && slot < 0)
			slot = i;
	}
	if (slot >= 0) {
		txn_hints[slot].txn = txn;
		txn_hints[slot].ux_caller = ux;
		txn_hints[slot].sync_txn = sync;
	}
	spin_unlock_irqrestore(&hint_lock, flags);
}

static bool hint_is_ux(struct binder_transaction *txn)
{
	int i;
	unsigned long flags;
	bool ux = false;

	if (!txn)
		return false;
	spin_lock_irqsave(&hint_lock, flags);
	for (i = 0; i < OPLUS_TXN_HINTS; i++) {
		if (txn_hints[i].txn == txn) {
			ux = txn_hints[i].ux_caller;
			break;
		}
	}
	spin_unlock_irqrestore(&hint_lock, flags);
	return ux;
}

static void hint_drop(struct binder_transaction *txn)
{
	int i;
	unsigned long flags;

	if (!txn)
		return;
	spin_lock_irqsave(&hint_lock, flags);
	for (i = 0; i < OPLUS_TXN_HINTS; i++) {
		if (txn_hints[i].txn == txn)
			txn_hints[i].txn = NULL;
	}
	spin_unlock_irqrestore(&hint_lock, flags);
}

static void bs_alloc_oem(void *data, struct binder_transaction_data *tr,
			 struct binder_transaction *t, struct binder_proc *proc)
{
	bool sync;

	(void)data;
	(void)proc;
	if (!tr || !t)
		return;
	sync = !(tr->flags & TF_ONE_WAY);
	hint_put(t, oplus_task_is_ux(current), sync);
}

static void bs_free_oem(void *data, struct binder_transaction *t)
{
	(void)data;
	hint_drop(t);
}

static bool bs_prefer_ux(struct binder_transaction *t, bool sync)
{
	if (!hint_is_ux(t))
		return false;
	if (sync)
		return READ_ONCE(oplus_binder_async_ux) != 0;
	return READ_ONCE(oplus_binder_fg_list_async_first) != 0;
}

/* Prefer a UX caller by inserting at the head. enqueue uses list_add_tail
 * and only runs when *special_task stays true, so clear it after list_add.
 */
static void bs_special_task(void *data, struct binder_transaction *t,
			    struct binder_proc *proc, struct binder_thread *thread,
			    struct binder_work *w, struct list_head *head,
			    bool sync, bool *special_task)
{
	(void)data;
	(void)proc;
	(void)thread;
	if (!t || !w || !head || !special_task)
		return;
	if (!bs_prefer_ux(t, sync))
		return;
	if (w->entry.next && !list_empty(&w->entry))
		return;
	list_add(&w->entry, head);
	*special_task = false;
}

static void bs_has_special(void *data, struct binder_thread *thread,
			   bool do_proc_work, bool *has_work)
{
	struct binder_work *w;

	(void)data;
	if (!thread || !has_work || !do_proc_work)
		return;
	if (!READ_ONCE(oplus_binder_async_ux))
		return;
	list_for_each_entry(w, &thread->proc->todo, entry) {
		struct binder_transaction *t;

		if (w->type != BINDER_WORK_TRANSACTION)
			continue;
		t = container_of(w, struct binder_transaction, work);
		if (hint_is_ux(t)) {
			*has_work = true;
			return;
		}
	}
}

/* Sync UX caller: raise the target to the caller's nice. binder restores
 * the saved priority after android_vh_binder_restore_priority.
 */
static void bs_set_priority(void *data, struct binder_transaction *t,
			    struct task_struct *task)
{
	int from, to;

	(void)data;
	if (!t || !task || !READ_ONCE(oplus_binder_async_ux))
		return;
	if (t->flags & TF_ONE_WAY)
		return;
	if (!hint_is_ux(t) && !oplus_task_is_ux(current))
		return;
	from = task_nice(current);
	to = task_nice(task);
	if (from < to)
		set_user_nice(task, from);
}

static void bs_restore_priority(void *data, struct binder_transaction *t,
				struct task_struct *task)
{
	(void)data;
	(void)t;
	(void)task;
	/* binder_restore_priority() runs after this hook and puts the
	 * saved policy back. Nothing extra to undo.
	 */
}

static bool proc_is_frozen(struct binder_proc *proc)
{
	u32 uid;

	if (!proc)
		return false;
	if (proc->is_frozen)
		return true;
	if (!proc->tsk)
		return false;
	uid = from_kuid(&init_user_ns, task_uid(proc->tsk));
	return oplus_uid_is_frozen(uid);
}

static void bs_spawn(void *data, struct binder_thread *thread,
		     struct binder_proc *proc, bool *force_spawn)
{
	struct binder_work *w;

	(void)data;
	(void)thread;
	if (!proc || !force_spawn || !READ_ONCE(oplus_binder_async_ux))
		return;
	if (!proc_is_frozen(proc))
		return;
	list_for_each_entry(w, &proc->todo, entry) {
		struct binder_transaction *t;

		if (w->type != BINDER_WORK_TRANSACTION)
			continue;
		t = container_of(w, struct binder_transaction, work);
		if (hint_is_ux(t)) {
			*force_spawn = true;
			return;
		}
	}
}

static ssize_t knob_read(struct file *file, char __user *buf, size_t len,
			  loff_t *ppos)
{
	char tmp[8];
	int *val = PDE_DATA(file_inode(file));
	int n;

	if (!val)
		return -EINVAL;
	n = scnprintf(tmp, sizeof(tmp), "%d\n", READ_ONCE(*val) ? 1 : 0);
	return simple_read_from_buffer(buf, len, ppos, tmp, n);
}

static ssize_t knob_write(struct file *file, const char __user *buf,
			   size_t len, loff_t *ppos)
{
	char kbuf[16];
	int *val = PDE_DATA(file_inode(file));
	int parsed;

	if (!val || !len || len >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, len))
		return -EFAULT;
	kbuf[len] = '\0';
	if (kstrtoint(skip_spaces(kbuf), 0, &parsed))
		return -EINVAL;
	if (parsed != 0 && parsed != 1)
		return -EINVAL;
	WRITE_ONCE(*val, parsed);
	return len;
}

static const struct proc_ops knob_ops = {
	.proc_read = knob_read,
	.proc_write = knob_write,
	.proc_lseek = default_llseek,
};

static int __init oplus_binder_strategy_init(void)
{
	struct proc_dir_entry *dir;

	if (WARN_ON(register_trace_android_vh_alloc_oem_binder_struct(bs_alloc_oem, NULL)))
		return -EINVAL;
	if (WARN_ON(register_trace_android_vh_free_oem_binder_struct(bs_free_oem, NULL)))
		return -EINVAL;
	if (WARN_ON(register_trace_android_vh_binder_special_task(bs_special_task, NULL)))
		return -EINVAL;
	if (WARN_ON(register_trace_android_vh_binder_has_special_work_ilocked(bs_has_special, NULL)))
		return -EINVAL;
	if (WARN_ON(register_trace_android_vh_binder_set_priority(bs_set_priority, NULL)))
		return -EINVAL;
	if (WARN_ON(register_trace_android_vh_binder_restore_priority(bs_restore_priority, NULL)))
		return -EINVAL;
	if (WARN_ON(register_trace_android_vh_binder_spawn_new_thread(bs_spawn, NULL)))
		return -EINVAL;

	dir = proc_mkdir("d_oplus_binder", NULL);
	if (!dir)
		return -ENOMEM;
	if (!proc_create_data("async_ux", 0644, dir, &knob_ops,
			      &oplus_binder_async_ux))
		return -ENOMEM;
	if (!proc_create_data("fg_list_async_first", 0644, dir, &knob_ops,
			      &oplus_binder_fg_list_async_first))
		return -ENOMEM;
	pr_info("binder_strategy: %s %s defaults %d %d\n",
		oplus_binder_async_ux_path, oplus_binder_fg_async_path,
		oplus_binder_async_ux, oplus_binder_fg_list_async_first);
	return 0;
}
device_initcall(oplus_binder_strategy_init);
