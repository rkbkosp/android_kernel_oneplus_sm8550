// SPDX-License-Identifier: GPL-2.0-only
/*
 * /proc/iolimit/pid and /proc/iolimit/write_bytes_limit.
 *
 * android_rvh_ctl_dirty_rate fires from balance_dirty_pages_ratelimited(),
 * once per dirtied page. Over-limit tasks sleep with io_schedule_timeout().
 *
 * block/blk-throttle.c throttles bios per cgroup (struct throtl_grp,
 * blk_throtl_bio, rbps/wbps/riops/wiops). It has no per-pid entry and no
 * cumulative byte cap that 0x5A clears, so it cannot express these proc
 * files. CONFIG_BLK_DEV_THROTTLING stays off. This file does not call it.
 * The stall decision is oplus_iolimit_charge(), tested without this hook.
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <trace/events/sched.h>
#include <trace/hooks/mm.h>

#include "policy.h"

#define IOLIMIT_CAP 64

struct iolimit_ent {
	pid_t pid;
	u64 start_time;
	u64 limit;
	u64 written;
};

static struct iolimit_ent iolimits[IOLIMIT_CAP];
static int iolimit_n;
static pid_t iolimit_selected;
static u64 iolimit_selected_start_time;
static DEFINE_SPINLOCK(iolimit_lock);

static const char path_pid[] __used = "/proc/iolimit/pid";
static const char path_limit[] __used = "/proc/iolimit/write_bytes_limit";

static struct iolimit_ent *iolimit_find(pid_t pid, u64 start_time)
{
	int i;

	for (i = 0; i < iolimit_n; i++) {
		if (iolimits[i].pid == pid &&
		    iolimits[i].start_time == start_time)
			return &iolimits[i];
	}
	return NULL;
}

static struct iolimit_ent *iolimit_get(pid_t pid, u64 start_time)
{
	struct iolimit_ent *e = iolimit_find(pid, start_time);
	int i;

	if (e)
		return e;
	/* A numeric PID may now name a different task. Reuse its old slot. */
	for (i = 0; i < iolimit_n; i++) {
		if (iolimits[i].pid == pid) {
			e = &iolimits[i];
			goto reset;
		}
	}
	if (iolimit_n >= IOLIMIT_CAP)
		return NULL;
	e = &iolimits[iolimit_n++];

reset:
	e->pid = pid;
	e->start_time = start_time;
	e->limit = 0;
	e->written = 0;
	return e;
}

static void iolimit_clear_pid(pid_t pid, u64 start_time)
{
	int i;

	for (i = 0; i < iolimit_n; i++) {
		if (iolimits[i].pid != pid ||
		    (start_time && iolimits[i].start_time != start_time))
			continue;
		iolimits[i] = iolimits[iolimit_n - 1];
		iolimit_n--;
		return;
	}
}

/* Called before the task releases its PID, so the slot is reusable on exit. */
static void iolimit_exit(void *data, struct task_struct *task)
{
	unsigned long flags;

	(void)data;
	spin_lock_irqsave(&iolimit_lock, flags);
	iolimit_clear_pid(task->pid, task->start_time);
	if (iolimit_selected == task->pid &&
	    iolimit_selected_start_time == task->start_time) {
		iolimit_selected = 0;
		iolimit_selected_start_time = 0;
	}
	spin_unlock_irqrestore(&iolimit_lock, flags);
}

/* Caller holds iolimit_lock; the RCU read protects the task lookup. */
static bool iolimit_task_start_time(pid_t pid, u64 *start_time)
{
	struct task_struct *task;
	bool live = false;

	rcu_read_lock();
	task = find_task_by_vpid(pid);
	if (task && !(READ_ONCE(task->flags) & PF_EXITING)) {
		*start_time = task->start_time;
		live = true;
	}
	rcu_read_unlock();
	return live;
}

static void iolimit_dirty(void *data, void *unused)
{
	struct iolimit_ent *e;
	bool over = false;
	unsigned long flags;

	(void)data;
	(void)unused;
	if (!current->pid)
		return;
	spin_lock_irqsave(&iolimit_lock, flags);
	e = iolimit_find(current->pid, current->start_time);
	if (e)
		over = oplus_iolimit_charge(&e->written, e->limit, PAGE_SIZE);
	spin_unlock_irqrestore(&iolimit_lock, flags);
	if (over)
		io_schedule_timeout(HZ / 50);
}

static ssize_t pid_read(struct file *file, char __user *buf, size_t len,
			loff_t *ppos)
{
	char *page;
	int i, n = 0;
	unsigned long flags;
	ssize_t ret;

	(void)file;
	page = (char *)__get_free_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;
	spin_lock_irqsave(&iolimit_lock, flags);
	n += scnprintf(page + n, PAGE_SIZE - n, "selected %d\n", iolimit_selected);
	for (i = 0; i < iolimit_n && n < PAGE_SIZE - 32; i++)
		n += scnprintf(page + n, PAGE_SIZE - n, "%d\n", iolimits[i].pid);
	spin_unlock_irqrestore(&iolimit_lock, flags);
	ret = simple_read_from_buffer(buf, len, ppos, page, n);
	free_page((unsigned long)page);
	return ret;
}

static ssize_t pid_write(struct file *file, const char __user *buf,
			 size_t len, loff_t *ppos)
{
	char kbuf[32];
	int pid;
	u64 start_time;
	unsigned long flags;

	(void)file;
	(void)ppos;
	if (!len || len >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, len))
		return -EFAULT;
	kbuf[len] = '\0';
	if (kstrtoint(skip_spaces(kbuf), 0, &pid) || pid <= 0)
		return -EINVAL;
	spin_lock_irqsave(&iolimit_lock, flags);
	if (!iolimit_task_start_time(pid, &start_time)) {
		spin_unlock_irqrestore(&iolimit_lock, flags);
		return -ESRCH;
	}
	if (!iolimit_get(pid, start_time)) {
		spin_unlock_irqrestore(&iolimit_lock, flags);
		return -ENOSPC;
	}
	iolimit_selected = pid;
	iolimit_selected_start_time = start_time;
	spin_unlock_irqrestore(&iolimit_lock, flags);
	return len;
}

static ssize_t limit_read(struct file *file, char __user *buf, size_t len,
			  loff_t *ppos)
{
	char *page;
	int i, n = 0;
	unsigned long flags;
	ssize_t ret;

	(void)file;
	page = (char *)__get_free_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;
	spin_lock_irqsave(&iolimit_lock, flags);
	for (i = 0; i < iolimit_n && n < PAGE_SIZE - 80; i++)
		n += scnprintf(page + n, PAGE_SIZE - n, "%d %llu %llu\n",
			       iolimits[i].pid, iolimits[i].limit,
			       iolimits[i].written);
	spin_unlock_irqrestore(&iolimit_lock, flags);
	ret = simple_read_from_buffer(buf, len, ppos, page, n);
	free_page((unsigned long)page);
	return ret;
}

static ssize_t limit_write(struct file *file, const char __user *buf,
			   size_t len, loff_t *ppos)
{
	char kbuf[64];
	unsigned long long limit;
	int pid = 0;
	int n;
	unsigned long flags;
	u64 start_time;
	struct iolimit_ent *e;

	(void)file;
	(void)ppos;
	if (!len || len >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, len))
		return -EFAULT;
	kbuf[len] = '\0';
	n = sscanf(skip_spaces(kbuf), "%d %llu", &pid, &limit);
	if (n == 1) {
		limit = (unsigned long long)pid;
		pid = 0;
	} else if (n != 2) {
		return -EINVAL;
	}
	spin_lock_irqsave(&iolimit_lock, flags);
	if (pid <= 0)
		pid = iolimit_selected;
	if (pid <= 0) {
		spin_unlock_irqrestore(&iolimit_lock, flags);
		return -EINVAL;
	}
	if (limit == OPLUS_IOLIMIT_CLEAR) {
		iolimit_clear_pid(pid, 0);
		if (iolimit_selected == pid) {
			iolimit_selected = 0;
			iolimit_selected_start_time = 0;
		}
		spin_unlock_irqrestore(&iolimit_lock, flags);
		return len;
	}
	if (!iolimit_task_start_time(pid, &start_time)) {
		spin_unlock_irqrestore(&iolimit_lock, flags);
		return -ESRCH;
	}
	if (n == 1 && start_time != iolimit_selected_start_time) {
		spin_unlock_irqrestore(&iolimit_lock, flags);
		return -ESRCH;
	}
	e = iolimit_get(pid, start_time);
	if (!e) {
		spin_unlock_irqrestore(&iolimit_lock, flags);
		return -ENOSPC;
	}
	e->limit = limit;
	e->written = 0;
	iolimit_selected = pid;
	iolimit_selected_start_time = start_time;
	spin_unlock_irqrestore(&iolimit_lock, flags);
	return len;
}

static const struct proc_ops pid_ops = {
	.proc_read = pid_read,
	.proc_write = pid_write,
	.proc_lseek = default_llseek,
};
static const struct proc_ops limit_ops = {
	.proc_read = limit_read,
	.proc_write = limit_write,
	.proc_lseek = default_llseek,
};

static int __init iolimit_init(void)
{
	struct proc_dir_entry *dir;
	int ret;

	ret = register_trace_sched_process_exit(iolimit_exit, NULL);
	if (ret)
		return ret;

	dir = proc_mkdir("iolimit", NULL);
	if (!dir) {
		unregister_trace_sched_process_exit(iolimit_exit, NULL);
		return -ENOMEM;
	}
	if (!proc_create("pid", 0644, dir, &pid_ops)) {
		remove_proc_subtree("iolimit", NULL);
		unregister_trace_sched_process_exit(iolimit_exit, NULL);
		return -ENOMEM;
	}
	if (!proc_create("write_bytes_limit", 0644, dir, &limit_ops)) {
		remove_proc_subtree("iolimit", NULL);
		unregister_trace_sched_process_exit(iolimit_exit, NULL);
		return -ENOMEM;
	}
	WARN_ON(register_trace_android_rvh_ctl_dirty_rate(iolimit_dirty, NULL));
	pr_info("iolimit: %s %s\n", path_pid, path_limit);
	return 0;
}
device_initcall(iolimit_init);
