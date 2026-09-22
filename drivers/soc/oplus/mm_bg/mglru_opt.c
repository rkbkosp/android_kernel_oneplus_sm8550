// SPDX-License-Identifier: GPL-2.0-only
/*
 * Debug file on top of this tree's page-based LRU_GEN. Read reports the
 * core switch and a sum of lrugen.nr_pages. Write 0/1 calls the existing
 * lru_gen_change_state() path. It does not replace the reclaim algorithm.
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/memcontrol.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/mmzone.h>
#include <linux/nodemask.h>
#include <linux/proc_fs.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "mm_bg.h"

static const char path_mglru[] __used = "/proc/oplus_mem/mglru_opt_debug";

static unsigned long mglru_count_pages(void)
{
	unsigned long sum = 0;
	int nid, gen, type, zone;

	for_each_online_node(nid) {
		struct lruvec *lruvec;

		lruvec = mem_cgroup_lruvec(NULL, NODE_DATA(nid));
		if (!lruvec)
			continue;
		for (gen = 0; gen < MAX_NR_GENS; gen++)
			for (type = 0; type < ANON_AND_FILE; type++)
				for (zone = 0; zone < MAX_NR_ZONES; zone++)
					sum += lruvec->lrugen.nr_pages[gen][type][zone];
	}
	return sum;
}

static ssize_t mglru_read(struct file *file, char __user *buf, size_t len,
			  loff_t *ppos)
{
	char tmp[96];
	int n;

	(void)file;
	n = scnprintf(tmp, sizeof(tmp), "enabled=%d nr_pages=%lu\n",
		      lru_gen_enabled() ? 1 : 0, mglru_count_pages());
	return simple_read_from_buffer(buf, len, ppos, tmp, n);
}

static ssize_t mglru_write(struct file *file, const char __user *buf,
			   size_t len, loff_t *ppos)
{
	char kbuf[16];
	int val;

	(void)file;
	(void)ppos;
	if (!len || len >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, len))
		return -EFAULT;
	kbuf[len] = '\0';
	if (kstrtoint(skip_spaces(kbuf), 0, &val))
		return -EINVAL;
	if (val != 0 && val != 1)
		return -EINVAL;
	lru_gen_set_core_enabled(val);
	return len;
}

static const struct proc_ops mglru_ops = {
	.proc_read = mglru_read,
	.proc_write = mglru_write,
	.proc_lseek = default_llseek,
};

static int __init mglru_opt_init(void)
{
	struct proc_dir_entry *dir = oplus_mem_proc_dir();

	if (!dir)
		return -ENOMEM;
	if (!proc_create("mglru_opt_debug", 0644, dir, &mglru_ops))
		return -ENOMEM;
	pr_info("mglru_opt: %s\n", path_mglru);
	return 0;
}
device_initcall(mglru_opt_init);
