// SPDX-License-Identifier: GPL-2.0-only
/*
 * Stored values userspace writes. Commit follow-up applies them.
 * /proc/oplus_mem/kswapd_load_stat is the node oplus_kswapd_profile.sh writes.
 */

#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/nodemask.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <trace/hooks/mm.h>
#include <trace/hooks/vmscan.h>

#include "mm_bg.h"

static void apply_kswapd_nice(int nice);

static int kswapd_debug;
static int kswapd_nice;
static int kswapd_load_stat;
static int alloc_adjust_ctrl;

static const char path_kswapd_debug[] __used = "/proc/oplus_mem/kswapd_debug";
static const char path_alloc_adjust[] __used = "/proc/oplus_mem/alloc_adjust_ctrl";
static const char path_kswapd_nice[] __used = "/proc/oplus_mem/kswapd_nice";
static const char path_kswapd_load[] __used = "/proc/oplus_mem/kswapd_load_stat";

static ssize_t kint_read(struct file *file, char __user *buf, size_t len,
			 loff_t *ppos)
{
	char tmp[32];
	int *val = PDE_DATA(file_inode(file));
	int n;

	if (!val)
		return -EINVAL;
	n = scnprintf(tmp, sizeof(tmp), "%d\n", READ_ONCE(*val));
	return simple_read_from_buffer(buf, len, ppos, tmp, n);
}

static ssize_t kint_write(struct file *file, const char __user *buf,
			   size_t len, loff_t *ppos)
{
	char kbuf[32];
	int *val = PDE_DATA(file_inode(file));
	int parsed;

	if (!val || !len || len >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, len))
		return -EFAULT;
	kbuf[len] = '\0';
	if (kstrtoint(skip_spaces(kbuf), 0, &parsed))
		return -EINVAL;
	WRITE_ONCE(*val, parsed);
	if (val == &kswapd_nice)
		apply_kswapd_nice(parsed);
	return len;
}

static atomic_t slowpath_end_count = ATOMIC_INIT(0);

static void apply_kswapd_nice(int nice)
{
	int nid, i;

	if (nice < MIN_NICE || nice > MAX_NICE)
		return;
	for_each_online_node(nid) {
		pg_data_t *pgdat = NODE_DATA(nid);

		if (!pgdat)
			continue;
		if (pgdat->kswapd)
			set_user_nice(pgdat->kswapd, nice);
		for (i = 0; i < MAX_KSWAPD_THREADS; i++) {
			if (pgdat->mkswapd[i])
				set_user_nice(pgdat->mkswapd[i], nice);
		}
	}
}

static void kswapd_on_done(void *data, int node_id, unsigned int highest,
			   unsigned int alloc_order, unsigned int reclaim_order)
{
	int nice = READ_ONCE(kswapd_nice);

	(void)data;
	(void)node_id;
	(void)highest;
	(void)alloc_order;
	(void)reclaim_order;
	if (nice >= MIN_NICE && nice <= MAX_NICE && task_nice(current) != nice)
		set_user_nice(current, nice);
}

static void kswapd_kvmalloc(void *data, unsigned int order, gfp_t *flags)
{
	int ctrl = READ_ONCE(alloc_adjust_ctrl);

	(void)data;
	(void)order;
	if (flags && ctrl)
		*flags |= (gfp_t)ctrl;
}

static void kswapd_slow_end(void *data, gfp_t *gfp_mask, unsigned int order,
			    unsigned long alloc_start, u64 stime,
			    unsigned long did_some_progress,
			    unsigned long pages_reclaimed, int retry_loop_count)
{
	(void)data;
	(void)gfp_mask;
	(void)order;
	(void)alloc_start;
	(void)stime;
	(void)did_some_progress;
	(void)pages_reclaimed;
	(void)retry_loop_count;
	atomic_inc(&slowpath_end_count);
}

static const struct proc_ops kint_ops = {
	.proc_read = kint_read,
	.proc_write = kint_write,
	.proc_lseek = default_llseek,
};

static int __init kswapd_opt_init(void)
{
	struct proc_dir_entry *dir = oplus_mem_proc_dir();

	if (!dir)
		return -ENOMEM;
	if (!proc_create_data("kswapd_debug", 0644, dir, &kint_ops, &kswapd_debug))
		return -ENOMEM;
	if (!proc_create_data("alloc_adjust_ctrl", 0644, dir, &kint_ops,
			      &alloc_adjust_ctrl))
		return -ENOMEM;
	if (!proc_create_data("kswapd_nice", 0644, dir, &kint_ops, &kswapd_nice))
		return -ENOMEM;
	if (!proc_create_data("kswapd_load_stat", 0644, dir, &kint_ops,
			      &kswapd_load_stat))
		return -ENOMEM;
	pr_info("kswapd_opt: %s %s %s %s\n", path_kswapd_debug, path_alloc_adjust,
		path_kswapd_nice, path_kswapd_load);
	WARN_ON(register_trace_android_vh_vmscan_kswapd_done(kswapd_on_done, NULL));
	WARN_ON(register_trace_android_vh_adjust_kvmalloc_flags(kswapd_kvmalloc, NULL));
	WARN_ON(register_trace_android_vh_alloc_pages_slowpath_end(kswapd_slow_end, NULL));
	return 0;
}
device_initcall(kswapd_opt_init);
