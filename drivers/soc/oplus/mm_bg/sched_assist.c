// SPDX-License-Identifier: GPL-2.0-only
/*
 * Foreground UX mark for the RAM and background-freeze port.
 *
 * WALT casts task_struct::android_vendor_data1 to struct walt_task_struct.
 * That overlay starts at slot 0. On this tree the struct is 392 bytes with
 * NR_CPUS=32, which is 49 u64 slots (0..48). The UX bit is bit 0 of slot 49,
 * the first slot past the overlay. This file does not read or write WALT
 * fields and does not change WALT task selection.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "mm_bg.h"
#include "policy.h"

#if IS_ENABLED(CONFIG_SCHED_WALT)
#include <linux/sched/walt.h>
#endif

static bool sched_assist_enabled = true;

static const char oplus_sched_assist_path[] __used =
	"/proc/oplus_scheduler/sched_assist/sched_assist_enabled";

bool oplus_sched_assist_enabled(void)
{
	return READ_ONCE(sched_assist_enabled);
}

static u64 *oplus_ux_word(struct task_struct *p)
{
#ifdef CONFIG_ANDROID_VENDOR_OEM_DATA
	if (!p)
		return NULL;
	return &p->android_vendor_data1[OPLUS_UX_VENDOR_SLOT];
#else
	return NULL;
#endif
}

void oplus_task_set_ux(struct task_struct *p, bool on)
{
	u64 *word = oplus_ux_word(p);
	unsigned long flags;
	u64 cur;

	if (!word)
		return;
	local_irq_save(flags);
	cur = READ_ONCE(*word);
	oplus_ux_word_set(&cur, on);
	WRITE_ONCE(*word, cur);
	local_irq_restore(flags);
}

bool oplus_task_is_ux(struct task_struct *p)
{
	u64 *word;

	if (!READ_ONCE(sched_assist_enabled))
		return false;
	word = oplus_ux_word(p);
	if (!word)
		return false;
	return oplus_ux_word_test(READ_ONCE(*word));
}

static ssize_t sched_assist_read(struct file *file, char __user *buf,
				  size_t len, loff_t *ppos)
{
	char tmp[8];
	int n;

	n = scnprintf(tmp, sizeof(tmp), "%d\n",
		      READ_ONCE(sched_assist_enabled) ? 1 : 0);
	return simple_read_from_buffer(buf, len, ppos, tmp, n);
}

static ssize_t sched_assist_write(struct file *file, const char __user *buf,
				   size_t len, loff_t *ppos)
{
	char kbuf[16];
	int val;

	if (!len || len >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, len))
		return -EFAULT;
	kbuf[len] = '\0';
	if (kstrtoint(skip_spaces(kbuf), 0, &val))
		return -EINVAL;
	if (val != 0 && val != 1)
		return -EINVAL;
	WRITE_ONCE(sched_assist_enabled, val != 0);
	return len;
}

static const struct proc_ops sched_assist_ops = {
	.proc_read = sched_assist_read,
	.proc_write = sched_assist_write,
	.proc_lseek = default_llseek,
};

static void __init oplus_ux_slot_ok(void)
{
#if IS_ENABLED(CONFIG_SCHED_WALT)
	BUILD_BUG_ON(sizeof(struct walt_task_struct) >
		     OPLUS_UX_VENDOR_SLOT * sizeof(u64));
#endif
}

static int __init oplus_sched_assist_init(void)
{
	struct proc_dir_entry *parent, *dir;

	oplus_ux_slot_ok();
	parent = proc_mkdir("oplus_scheduler", NULL);
	if (!parent)
		return -ENOMEM;
	dir = proc_mkdir("sched_assist", parent);
	if (!dir)
		return -ENOMEM;
	if (!proc_create("sched_assist_enabled", 0644, dir, &sched_assist_ops))
		return -ENOMEM;
	pr_info("sched_assist: %s ux_slot=%d\n",
		oplus_sched_assist_path, OPLUS_UX_VENDOR_SLOT);
	return 0;
}
device_initcall(oplus_sched_assist_init);
