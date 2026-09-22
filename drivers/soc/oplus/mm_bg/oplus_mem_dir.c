// SPDX-License-Identifier: GPL-2.0-only
#include <linux/init.h>
#include <linux/proc_fs.h>

#include "mm_bg.h"

static struct proc_dir_entry *oplus_mem_dir;

struct proc_dir_entry *oplus_mem_proc_dir(void)
{
	if (!oplus_mem_dir)
		oplus_mem_dir = proc_mkdir("oplus_mem", NULL);
	return oplus_mem_dir;
}

static int __init oplus_mem_dir_init(void)
{
	return oplus_mem_proc_dir() ? 0 : -ENOMEM;
}
device_initcall(oplus_mem_dir_init);
