// SPDX-License-Identifier: GPL-2.0-only
/*
 * Order-0 page pool for tasks marked UX by sched_assist.
 * Disabled until /proc/oplus_mem/ux_page_pool is set.
 *
 * android_vh_rmqueue_bulk_bypass is called from get_populated_pcp_list()
 * with list == &pcp->lists[pindex], the persistent per-cpu freelist, not
 * a list of pages about to be returned. __rmqueue_pcplist() then does
 * pcp->count -= 1 << order, and prep_new_page() calls set_page_refcounted()
 * which requires refcount 0. A pool page from alloc_page() already has
 * refcount 1. Splicing it on would underflow pcp->count and free_pcppages_bulk()
 * can __free_one_page() it later (double-free, possibly the wrong zone).
 * Pool pages are therefore not spliced. They are returned, already prepared,
 * from alloc_pages_reclaim_bypass and alloc_pages_failure_bypass. The bulk
 * hook only counts UX refills. There is no sched_ext and no folio.
 */

#include <linux/atomic.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <trace/hooks/mm.h>

#include "mm_bg.h"

#define UX_POOL_MAX 64

static LIST_HEAD(ux_pool);
static int ux_pool_count;
static int ux_pool_enabled;
static DEFINE_SPINLOCK(ux_pool_lock);
static struct work_struct ux_refill_work;
static atomic_t ux_bulk_hits = ATOMIC_INIT(0);
/* Do not push allocated pages onto the pcp freelist. See file comment. */

static const char path_ux_pool[] __used = "/proc/oplus_mem/ux_page_pool";

static struct page *ux_pool_pop(void)
{
	struct page *page = NULL;
	unsigned long flags;

	spin_lock_irqsave(&ux_pool_lock, flags);
	if (ux_pool_enabled && !list_empty(&ux_pool)) {
		page = list_first_entry(&ux_pool, struct page, lru);
		list_del(&page->lru);
		ux_pool_count--;
	}
	spin_unlock_irqrestore(&ux_pool_lock, flags);
	return page;
}

static void ux_pool_refill(struct work_struct *work)
{
	int guard = 0;

	(void)work;
	while (guard++ < UX_POOL_MAX && READ_ONCE(ux_pool_enabled)) {
		struct page *page;
		unsigned long flags;

		if (READ_ONCE(ux_pool_count) >= UX_POOL_MAX)
			break;
		page = alloc_page(GFP_KERNEL | __GFP_NOWARN);
		if (!page)
			break;
		spin_lock_irqsave(&ux_pool_lock, flags);
		if (!ux_pool_enabled || ux_pool_count >= UX_POOL_MAX) {
			spin_unlock_irqrestore(&ux_pool_lock, flags);
			__free_page(page);
			break;
		}
		list_add(&page->lru, &ux_pool);
		ux_pool_count++;
		spin_unlock_irqrestore(&ux_pool_lock, flags);
	}
}

static void ux_pool_drain(void)
{
	LIST_HEAD(tmp);
	unsigned long flags;

	spin_lock_irqsave(&ux_pool_lock, flags);
	list_splice_init(&ux_pool, &tmp);
	ux_pool_count = 0;
	spin_unlock_irqrestore(&ux_pool_lock, flags);
	while (!list_empty(&tmp)) {
		struct page *page = list_first_entry(&tmp, struct page, lru);

		list_del(&page->lru);
		__free_page(page);
	}
}

/* Same prototype for reclaim bypass and failure bypass. */
static void ux_pool_bypass(void *data, gfp_t gfp_mask, int order,
			   int alloc_flags, int migratetype,
			   struct page **page)
{
	(void)data;
	(void)gfp_mask;
	(void)alloc_flags;
	(void)migratetype;
	if (!READ_ONCE(ux_pool_enabled) || order || !page || *page)
		return;
	if (!oplus_task_is_ux(current))
		return;
	*page = ux_pool_pop();
	if (*page && READ_ONCE(ux_pool_count) < UX_POOL_MAX / 2)
		schedule_work(&ux_refill_work);
}

static void ux_rmqueue_bulk(void *data, unsigned int order,
			    struct per_cpu_pages *pcp, int migratetype,
			    struct list_head *list)
{
	(void)data;
	(void)order;
	(void)pcp;
	(void)migratetype;
	/* list is the pcp freelist. Do not splice pool pages onto it. */
	(void)list;
	if (!READ_ONCE(ux_pool_enabled))
		return;
	if (!oplus_task_is_ux(current))
		return;
	atomic_inc(&ux_bulk_hits);
}

static ssize_t ux_pool_read(struct file *file, char __user *buf, size_t len,
			    loff_t *ppos)
{
	char tmp[64];
	int n;

	(void)file;
	n = scnprintf(tmp, sizeof(tmp), "%d %d %d\n",
		      READ_ONCE(ux_pool_enabled), READ_ONCE(ux_pool_count),
		      atomic_read(&ux_bulk_hits));
	return simple_read_from_buffer(buf, len, ppos, tmp, n);
}

static ssize_t ux_pool_write(struct file *file, const char __user *buf,
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
	WRITE_ONCE(ux_pool_enabled, val);
	if (val)
		schedule_work(&ux_refill_work);
	else
		ux_pool_drain();
	return len;
}

static const struct proc_ops ux_pool_ops = {
	.proc_read = ux_pool_read,
	.proc_write = ux_pool_write,
	.proc_lseek = default_llseek,
};

static int __init uxmem_opt_init(void)
{
	struct proc_dir_entry *dir = oplus_mem_proc_dir();

	if (!dir)
		return -ENOMEM;
	INIT_WORK(&ux_refill_work, ux_pool_refill);
	if (!proc_create("ux_page_pool", 0644, dir, &ux_pool_ops))
		return -ENOMEM;
	WARN_ON(register_trace_android_vh_alloc_pages_reclaim_bypass(
			ux_pool_bypass, NULL));
	WARN_ON(register_trace_android_vh_alloc_pages_failure_bypass(
			ux_pool_bypass, NULL));
	WARN_ON(register_trace_android_vh_rmqueue_bulk_bypass(
			ux_rmqueue_bulk, NULL));
	pr_info("uxmem: %s\n", path_ux_pool);
	return 0;
}
device_initcall(uxmem_opt_init);
