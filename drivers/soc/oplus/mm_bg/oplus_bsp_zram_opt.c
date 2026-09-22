// SPDX-License-Identifier: GPL-2.0-only
/*
 * Built-in swappiness. The object name is oplus_bsp_zram_opt so module
 * parameters show up at /sys/module/oplus_bsp_zram_opt/parameters/.
 * Compression stays the zstd backend this tree already accepts. There is
 * no oplus_bsp_zstdn.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <trace/hooks/vmscan.h>

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
	BUILD_BUG_ON(sizeof(zram_algo) < 5);
	pr_info("zram_opt: %s %s %s algo=%s\n",
		path_vm_swappiness, path_high_thr, path_low_thr, zram_algo);
	WARN_ON(register_trace_android_vh_tune_swappiness(zram_tune_swappiness,
							  NULL));
	return 0;
}
device_initcall(zram_opt_init);
