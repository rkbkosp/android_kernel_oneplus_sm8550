// SPDX-License-Identifier: GPL-2.0-only
/*
 * /dev/osvelte and /sys/kernel/oplus_mm/osvelte/common/compact_memory.
 *
 * Swap-out is not implemented. There is no hybrid swap and no nandswap.
 * Writing compact_memory calls sysctl_compaction_handler(), the same path
 * as /proc/sys/vm/compact_memory (compact_nodes()).
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sysctl.h>
#include <linux/sysfs.h>
#include <linux/compaction.h>

static const char path_dev[] __used = "/dev/osvelte";
static const char path_compact[] __used =
	"/sys/kernel/oplus_mm/osvelte/common/compact_memory";

static int osvelte_open(struct inode *inode, struct file *file)
{
	(void)inode;
	(void)file;
	return 0;
}

static const struct file_operations osvelte_fops = {
	.owner = THIS_MODULE,
	.open = osvelte_open,
	.llseek = noop_llseek,
};

static struct miscdevice osvelte_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "osvelte",
	.fops = &osvelte_fops,
};

static ssize_t compact_memory_store(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buf, size_t count)
{
	(void)kobj;
	(void)attr;
	(void)buf;
	sysctl_compaction_handler(NULL, 1, NULL, NULL, NULL);
	return count;
}

static struct kobj_attribute compact_memory_attr =
	__ATTR(compact_memory, 0200, NULL, compact_memory_store);

static struct kobject *oplus_mm_kobj;
static struct kobject *osvelte_kobj;
static struct kobject *common_kobj;

static int __init osvelte_init(void)
{
	int ret;

	ret = misc_register(&osvelte_misc);
	if (ret)
		return ret;
	oplus_mm_kobj = kobject_create_and_add("oplus_mm", kernel_kobj);
	if (!oplus_mm_kobj)
		return -ENOMEM;
	osvelte_kobj = kobject_create_and_add("osvelte", mm_kobj);
	if (!osvelte_kobj)
		return -ENOMEM;
	common_kobj = kobject_create_and_add("common", osvelte_kobj);
	if (!common_kobj)
		return -ENOMEM;
	ret = sysfs_create_file(common_kobj, &compact_memory_attr.attr);
	if (ret)
		return ret;
	pr_info("osvelte: %s %s (swap-out is not implemented)\n",
		path_dev, path_compact);
	return 0;
}
device_initcall(osvelte_init);
