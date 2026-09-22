// SPDX-License-Identifier: GPL-2.0-only
/*
 * /proc/oplus_mem/fragmentation_index
 * Read calls fragmentation_index() per populated zone.
 * Write sets sysctl_compaction_proactiveness and wakes kcompactd the
 * same way compaction_proactiveness_sysctl_handler() does.
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/nodemask.h>
#include <linux/proc_fs.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/compaction.h>

#include "mm_bg.h"

static int frag_order = 4;
static const char path_frag[] __used = "/proc/oplus_mem/fragmentation_index";

static ssize_t frag_read(struct file *file, char __user *buf, size_t len,
			 loff_t *ppos)
{
	char *page;
	int nid, n = 0;
	ssize_t ret;

	(void)file;
	page = (char *)__get_free_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;
	for_each_online_node(nid) {
		pg_data_t *pgdat = NODE_DATA(nid);
		int zid;

		if (!pgdat)
			continue;
		for (zid = 0; zid < MAX_NR_ZONES && n < PAGE_SIZE - 64; zid++) {
			struct zone *zone = &pgdat->node_zones[zid];

			if (!populated_zone(zone))
				continue;
			n += scnprintf(page + n, PAGE_SIZE - n,
				       "node %d zone %s order %d index %d proactiveness %u\n",
				       nid, zone->name, frag_order,
				       fragmentation_index(zone, frag_order),
				       sysctl_compaction_proactiveness);
		}
	}
	ret = simple_read_from_buffer(buf, len, ppos, page, n);
	free_page((unsigned long)page);
	return ret;
}

static void wake_proactive(void)
{
	int nid;

	for_each_online_node(nid) {
		pg_data_t *pgdat = NODE_DATA(nid);

		if (!pgdat || pgdat->proactive_compact_trigger)
			continue;
		pgdat->proactive_compact_trigger = true;
		wake_up_interruptible(&pgdat->kcompactd_wait);
	}
}

static ssize_t frag_write(struct file *file, const char __user *buf,
			  size_t len, loff_t *ppos)
{
	char kbuf[32];
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
	if (val < 0 || val > 100)
		return -EINVAL;
	sysctl_compaction_proactiveness = val;
	if (val)
		wake_proactive();
	return len;
}

static const struct proc_ops frag_ops = {
	.proc_read = frag_read,
	.proc_write = frag_write,
	.proc_lseek = default_llseek,
};

static int __init proactive_compact_init(void)
{
	struct proc_dir_entry *dir = oplus_mem_proc_dir();

	if (!dir)
		return -ENOMEM;
	if (!proc_create("fragmentation_index", 0644, dir, &frag_ops))
		return -ENOMEM;
	pr_info("proactive_compact: %s\n", path_frag);
	return 0;
}
device_initcall(proactive_compact_init);
