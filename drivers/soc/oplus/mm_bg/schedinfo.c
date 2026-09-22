// SPDX-License-Identifier: GPL-2.0-only
/*
 * /proc/fg_info/fg_uids
 *
 * Userspace writes the foreground uid list. The file is replaced by each
 * write and read back one uid per line. Existing tasks of a uid that is
 * added get the sched_assist UX bit; tasks of a uid that disappeared lose
 * it. The bit is still the slot chosen in sched_assist.c.
 */

#include <linux/cred.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>

#include "mm_bg.h"
#include "policy.h"

static struct oplus_uid_table fg_uids;
static DEFINE_SPINLOCK(fg_lock);

static const char oplus_fg_uids_path[] __used = "/proc/fg_info/fg_uids";

bool oplus_uid_is_foreground(u32 uid)
{
	unsigned long flags;
	bool has;

	spin_lock_irqsave(&fg_lock, flags);
	has = oplus_uid_has(&fg_uids, uid);
	spin_unlock_irqrestore(&fg_lock, flags);
	return has;
}

static void mark_uid_tasks(u32 uid, bool on)
{
	struct task_struct *p, *t;

	rcu_read_lock();
	for_each_process(p) {
		for_each_thread(p, t) {
			if (from_kuid(&init_user_ns, task_uid(t)) == uid)
				oplus_task_set_ux(t, on);
		}
	}
	rcu_read_unlock();
}

static void apply_fg_delta(const struct oplus_uid_table *old,
			   const struct oplus_uid_table *neu)
{
	int i;

	for (i = 0; i < old->n; i++) {
		if (!oplus_uid_has(neu, old->uid[i]))
			mark_uid_tasks(old->uid[i], false);
	}
	for (i = 0; i < neu->n; i++)
		mark_uid_tasks(neu->uid[i], true);
}

static ssize_t fg_uids_read(struct file *file, char __user *buf,
			     size_t len, loff_t *ppos)
{
	struct oplus_uid_table snap;
	char *page;
	int i, n = 0;
	unsigned long flags;
	ssize_t ret;

	page = (char *)__get_free_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;
	spin_lock_irqsave(&fg_lock, flags);
	snap = fg_uids;
	spin_unlock_irqrestore(&fg_lock, flags);
	for (i = 0; i < snap.n && n < PAGE_SIZE - 16; i++)
		n += scnprintf(page + n, PAGE_SIZE - n, "%u\n", snap.uid[i]);
	ret = simple_read_from_buffer(buf, len, ppos, page, n);
	free_page((unsigned long)page);
	return ret;
}

static ssize_t fg_uids_write(struct file *file, const char __user *buf,
			      size_t len, loff_t *ppos)
{
	char *kbuf;
	u32 parsed[OPLUS_UID_CAP];
	struct oplus_uid_table neu;
	struct oplus_uid_table old;
	int n, i, dups = 0;
	unsigned long flags;

	if (!len || len > PAGE_SIZE)
		return -EINVAL;
	kbuf = kmalloc(len + 1, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;
	if (copy_from_user(kbuf, buf, len)) {
		kfree(kbuf);
		return -EFAULT;
	}
	kbuf[len] = '\0';
	n = oplus_parse_uids(kbuf, parsed, OPLUS_UID_CAP, &dups);
	kfree(kbuf);
	if (n < 0)
		return n;
	oplus_uid_table_reset(&neu);
	for (i = 0; i < n; i++) {
		int rc = oplus_uid_add(&neu, parsed[i]);

		if (rc < 0)
			return rc;
	}
	spin_lock_irqsave(&fg_lock, flags);
	old = fg_uids;
	fg_uids = neu;
	spin_unlock_irqrestore(&fg_lock, flags);
	apply_fg_delta(&old, &neu);
	return len;
}

static const struct proc_ops fg_uids_ops = {
	.proc_read = fg_uids_read,
	.proc_write = fg_uids_write,
	.proc_lseek = default_llseek,
};

static int __init oplus_schedinfo_init(void)
{
	struct proc_dir_entry *dir;

	dir = proc_mkdir("fg_info", NULL);
	if (!dir)
		return -ENOMEM;
	if (!proc_create("fg_uids", 0644, dir, &fg_uids_ops))
		return -ENOMEM;
	pr_info("schedinfo: %s\n", oplus_fg_uids_path);
	return 0;
}
device_initcall(oplus_schedinfo_init);
