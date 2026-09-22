// SPDX-License-Identifier: GPL-2.0-only
/*
 * /proc/iolimit/pid and /proc/iolimit/write_bytes_limit.
 *
 * android_rvh_ctl_dirty_rate fires from balance_dirty_pages_ratelimited(),
 * once per dirtied page. Over-limit tasks sleep with io_schedule_timeout(),
 * the same wait 5.15 blk-throttle uses. gki_defconfig does not enable
 * CONFIG_BLK_DEV_THROTTLING and that code has no per-pid entry point, so
 * the stall stays on this writeback hook. 0x5A clears a limit.
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <trace/hooks/mm.h>

#include "policy.h"

#define IOLIMIT_CAP 64

struct iolimit_ent {
	pid_t pid;
	u64 limit;
	u64 written;
};

static struct iolimit_ent iolimits[IOLIMIT_CAP];
static int iolimit_n;
static pid_t iolimit_selected;
static DEFINE_SPINLOCK(iolimit_lock);

static const char path_pid[] __used = "/proc/iolimit/pid";
static const char path_limit[] __used = "/proc/iolimit/write_bytes_limit";

static struct iolimit_ent *iolimit_find(pid_t pid)
{
	int i;

	for (i = 0; i < iolimit_n; i++) {
		if (iolimits[i].pid == pid)
			return &iolimits[i];
	}
	return NULL;
}

static struct iolimit_ent *iolimit_get(pid_t pid)
{
	struct iolimit_ent *e = iolimit_find(pid);

	if (e)
		return e;
	if (iolimit_n >= IOLIMIT_CAP)
		return NULL;
	e = &iolimits[iolimit_n++];
	e->pid = pid;
	e->limit = 0;
	e->written = 0;
	return e;
}

static void iolimit_clear_pid(pid_t pid)
{
	int i;

	for (i = 0; i < iolimit_n; i++) {
		if (iolimits[i].pid != pid)
			continue;
		iolimits[i] = iolimits[iolimit_n - 1];
		iolimit_n--;
		return;
	}
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
	e = iolimit_find(current->pid);
	if (e && !oplus_iolimit_is_clear(e->limit) && e->limit) {
		e->written += PAGE_SIZE;
		over = oplus_iolimit_over(e->written, e->limit);
	}
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
	iolimit_selected = pid;
	if (!iolimit_get(pid)) {
		spin_unlock_irqrestore(&iolimit_lock, flags);
		return -ENOSPC;
	}
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
		iolimit_clear_pid(pid);
		spin_unlock_irqrestore(&iolimit_lock, flags);
		return len;
	}
	e = iolimit_get(pid);
	if (!e) {
		spin_unlock_irqrestore(&iolimit_lock, flags);
		return -ENOSPC;
	}
	e->limit = limit;
	e->written = 0;
	iolimit_selected = pid;
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

	dir = proc_mkdir("iolimit", NULL);
	if (!dir)
		return -ENOMEM;
	if (!proc_create("pid", 0644, dir, &pid_ops))
		return -ENOMEM;
	if (!proc_create("write_bytes_limit", 0644, dir, &limit_ops))
		return -ENOMEM;
	WARN_ON(register_trace_android_rvh_ctl_dirty_rate(iolimit_dirty, NULL));
	pr_info("iolimit: %s %s\n", path_pid, path_limit);
	return 0;
}
device_initcall(iolimit_init);
