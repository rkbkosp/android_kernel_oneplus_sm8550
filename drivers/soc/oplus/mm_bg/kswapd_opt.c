// SPDX-License-Identifier: GPL-2.0-only
/*
 * Stored values userspace writes. Commit follow-up applies them.
 * /proc/oplus_mem/kswapd_load_stat is the node oplus_kswapd_profile.sh writes.
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "mm_bg.h"

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
	return len;
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
	return 0;
}
device_initcall(kswapd_opt_init);
