// SPDX-License-Identifier: GPL-2.0-only
/*
 * oplus_bsp_aizerofs
 *
 * Stores /sys/module/oplus_bsp_aizerofs/parameters/enabled and opens
 * /dev/aizerofs. The cache is not implemented. There is no read-only
 * compression and no address_space rewrite.
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

static int enabled;

module_param(enabled, int, 0644);
MODULE_PARM_DESC(enabled, "stored and readable; cache is not implemented");

static const char path_enabled[] __used =
	"/sys/module/oplus_bsp_aizerofs/parameters/enabled";
static const char path_dev[] __used = "/dev/aizerofs";

static int aizerofs_open(struct inode *inode, struct file *file)
{
	(void)inode;
	(void)file;
	return 0;
}

static const struct file_operations aizerofs_fops = {
	.owner = THIS_MODULE,
	.open = aizerofs_open,
	.llseek = noop_llseek,
};

static struct miscdevice aizerofs_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "aizerofs",
	.fops = &aizerofs_fops,
};

static int __init aizerofs_init(void)
{
	int ret;

	ret = misc_register(&aizerofs_misc);
	if (ret)
		return ret;
	pr_info("aizerofs: %s %s (cache is not implemented)\n",
		path_enabled, path_dev);
	return 0;
}
device_initcall(aizerofs_init);
