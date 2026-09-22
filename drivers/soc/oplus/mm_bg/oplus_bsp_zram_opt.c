// SPDX-License-Identifier: GPL-2.0-only
/*
 * Built-in swappiness. The object name is oplus_bsp_zram_opt so module
 * parameters show up at /sys/module/oplus_bsp_zram_opt/parameters/.
 * Compression stays the zstd backend this tree already accepts. There is
 * no oplus_bsp_zstdn.
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <trace/hooks/vmscan.h>

#include "mm_bg.h"

#include "policy.h"

static int vm_swappiness = 100;
static int swappiness_high_threshold = 50;
static int swappiness_low_threshold = 20;
static int tune_count;

module_param(vm_swappiness, int, 0644);
MODULE_PARM_DESC(vm_swappiness, "base swappiness written by userspace");
module_param(swappiness_high_threshold, int, 0644);
MODULE_PARM_DESC(swappiness_high_threshold, "avail percent for the high bucket");
module_param(swappiness_low_threshold, int, 0644);
MODULE_PARM_DESC(swappiness_low_threshold, "avail percent for the low bucket");
module_param(tune_count, int, 0444);
MODULE_PARM_DESC(tune_count, "android_vh_tune_swappiness hits");

static const char path_vm_swappiness[] __used =
	"/sys/module/oplus_bsp_zram_opt/parameters/vm_swappiness";
static const char path_high_thr[] __used =
	"/sys/module/oplus_bsp_zram_opt/parameters/swappiness_high_threshold";
static const char path_low_thr[] __used =
	"/sys/module/oplus_bsp_zram_opt/parameters/swappiness_low_threshold";

static int dynamic_swappiness;
static int direct_swappiness;
static struct oplus_swap_buckets kswapd_buckets = {
	.high_pct = 50,
	.low_pct = 20,
	.sw_high = 60,
	.sw_mid = 100,
	.sw_low = 160,
};
static struct oplus_swap_buckets direct_buckets = {
	.high_pct = 40,
	.low_pct = 15,
	.sw_high = 80,
	.sw_mid = 120,
	.sw_low = 180,
};
static char zram_algo[16] = "zstd";

static const char path_dyn[] __used = "/proc/oplus_mem/dynamic_swappiness";
static const char path_direct[] __used = "/proc/oplus_mem/dynamic_direct_swappiness";
static const char path_para[] __used = "/proc/oplus_mem/swappiness_para";

static int parse_bucket_line(const char *s, int *enable,
			     struct oplus_swap_buckets *b)
{
	unsigned int high = b->high_pct, low = b->low_pct;
	int en = *enable, hi = b->sw_high, mid = b->sw_mid, lo = b->sw_low;
	int n;

	n = sscanf(s, "%d %u %u %d %d %d", &en, &high, &low, &hi, &mid, &lo);
	if (n < 1)
		return -EINVAL;
	if (en != 0 && en != 1)
		return -EINVAL;
	if (n >= 3 && high < low)
		return -EINVAL;
	*enable = en;
	if (n >= 2)
		b->high_pct = high;
	if (n >= 3)
		b->low_pct = low;
	if (n >= 4)
		b->sw_high = hi;
	if (n >= 5)
		b->sw_mid = mid;
	if (n >= 6)
		b->sw_low = lo;
	return 0;
}

static ssize_t bucket_read(char __user *buf, size_t len, loff_t *ppos,
			   int enable, const struct oplus_swap_buckets *b)
{
	char tmp[96];
	int n = scnprintf(tmp, sizeof(tmp), "%d %u %u %d %d %d\n",
			  enable, b->high_pct, b->low_pct,
			  b->sw_high, b->sw_mid, b->sw_low);

	return simple_read_from_buffer(buf, len, ppos, tmp, n);
}

static ssize_t dyn_read(struct file *file, char __user *buf, size_t len,
			loff_t *ppos)
{
	return bucket_read(buf, len, ppos, READ_ONCE(dynamic_swappiness),
			   &kswapd_buckets);
}

static ssize_t dyn_write(struct file *file, const char __user *buf,
			 size_t len, loff_t *ppos)
{
	char kbuf[96];
	int en = READ_ONCE(dynamic_swappiness);

	(void)file;
	(void)ppos;
	if (!len || len >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, len))
		return -EFAULT;
	kbuf[len] = '\0';
	if (parse_bucket_line(skip_spaces(kbuf), &en, &kswapd_buckets))
		return -EINVAL;
	WRITE_ONCE(dynamic_swappiness, en);
	return len;
}

static ssize_t direct_read(struct file *file, char __user *buf, size_t len,
			   loff_t *ppos)
{
	return bucket_read(buf, len, ppos, READ_ONCE(direct_swappiness),
			   &direct_buckets);
}

static ssize_t direct_write(struct file *file, const char __user *buf,
			    size_t len, loff_t *ppos)
{
	char kbuf[96];
	int en = READ_ONCE(direct_swappiness);

	(void)file;
	(void)ppos;
	if (!len || len >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, len))
		return -EFAULT;
	kbuf[len] = '\0';
	if (parse_bucket_line(skip_spaces(kbuf), &en, &direct_buckets))
		return -EINVAL;
	WRITE_ONCE(direct_swappiness, en);
	return len;
}

static ssize_t para_read(struct file *file, char __user *buf, size_t len,
			 loff_t *ppos)
{
	char tmp[64];
	int n;

	(void)file;
	n = scnprintf(tmp, sizeof(tmp), "%s\n", zram_algo);
	return simple_read_from_buffer(buf, len, ppos, tmp, n);
}

static ssize_t para_write(struct file *file, const char __user *buf,
			  size_t len, loff_t *ppos)
{
	char kbuf[64];
	char *tok;

	(void)file;
	(void)ppos;
	if (!len || len >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, len))
		return -EFAULT;
	kbuf[len] = '\0';
	tok = skip_spaces(kbuf);
	if (strncmp(tok, "zstd", 4) != 0)
		return -EINVAL;
	if (tok[4] != '\0' && tok[4] != '\n' && tok[4] != ' ')
		return -EINVAL;
	strscpy(zram_algo, "zstd", sizeof(zram_algo));
	return len;
}

static const struct proc_ops dyn_ops = {
	.proc_read = dyn_read,
	.proc_write = dyn_write,
	.proc_lseek = default_llseek,
};
static const struct proc_ops direct_ops = {
	.proc_read = direct_read,
	.proc_write = direct_write,
	.proc_lseek = default_llseek,
};
static const struct proc_ops para_ops = {
	.proc_read = para_read,
	.proc_write = para_write,
	.proc_lseek = default_llseek,
};

static void zram_tune_swappiness(void *data, int *swappiness)
{
	unsigned long total = totalram_pages();
	unsigned long avail = si_mem_available();
	const struct oplus_swap_buckets *b;
	int chosen;

	(void)data;
	WRITE_ONCE(tune_count, READ_ONCE(tune_count) + 1);
	if (!swappiness)
		return;
	if (current->flags & PF_KSWAPD) {
		if (!READ_ONCE(dynamic_swappiness)) {
			*swappiness = READ_ONCE(vm_swappiness);
			return;
		}
		b = &kswapd_buckets;
	} else if (READ_ONCE(direct_swappiness)) {
		b = &direct_buckets;
	} else {
		*swappiness = READ_ONCE(vm_swappiness);
		return;
	}
	chosen = oplus_swappiness_for_avail(avail, total, b,
					    READ_ONCE(vm_swappiness));
	if (chosen < 0)
		chosen = 0;
	if (chosen > 200)
		chosen = 200;
	*swappiness = chosen;
}

static int __init zram_opt_init(void)
{
	struct proc_dir_entry *dir;

	BUILD_BUG_ON(sizeof(zram_algo) < 5);
	dir = oplus_mem_proc_dir();

	if (!dir)
		return -ENOMEM;
	if (!proc_create("dynamic_swappiness", 0644, dir, &dyn_ops))
		return -ENOMEM;
	if (!proc_create("dynamic_direct_swappiness", 0644, dir, &direct_ops))
		return -ENOMEM;
	if (!proc_create("swappiness_para", 0644, dir, &para_ops))
		return -ENOMEM;
	pr_info("zram_opt: %s %s %s algo=%s\n",
		path_dyn, path_direct, path_para, zram_algo);
	WARN_ON(register_trace_android_vh_tune_swappiness(zram_tune_swappiness,
							  NULL));
	return 0;
}
device_initcall(zram_opt_init);
