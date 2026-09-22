// SPDX-License-Identifier: GPL-2.0-only
/*
 * oplus_hans generic netlink family.
 *
 * The family name is fixed. Command numbers, the uid attribute and the
 * multicast group name are NOT a verified ColorOS ABI. The donor .ko is
 * not on this machine. Defaults below are module parameters so a device
 * log can override them. Commands that are not one of those ids return
 * an error and log the command id.
 *
 * Only commands 1..OPLUS_HANS_CMD_SLOTS are registered. Genetlink rejects
 * anything outside that range before this callback runs.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/ratelimit.h>
#include <linux/spinlock.h>
#include <net/genetlink.h>

#include "mm_bg.h"
#include "policy.h"

#define OPLUS_HANS_CMD_SLOTS 32
#define OPLUS_HANS_ATTR_MAX 8

/* 待真机日志核对: placeholder command and attribute numbers. */
static unsigned int hans_cmd_add_uid = 1;
static unsigned int hans_cmd_del_uid = 2;
static unsigned int hans_attr_uid = 1;
static unsigned int hans_cmd_event = 3;

module_param(hans_cmd_add_uid, uint, 0644);
MODULE_PARM_DESC(hans_cmd_add_uid, "frozen-uid add command (待真机日志核对)");
module_param(hans_cmd_del_uid, uint, 0644);
MODULE_PARM_DESC(hans_cmd_del_uid, "frozen-uid delete command (待真机日志核对)");
module_param(hans_attr_uid, uint, 0644);
MODULE_PARM_DESC(hans_attr_uid, "uid attribute id 1..8 (待真机日志核对)");
module_param(hans_cmd_event, uint, 0644);
MODULE_PARM_DESC(hans_cmd_event, "event command id (待真机日志核对)");

static const char oplus_hans_family_name[] __used = "oplus_hans";

static struct oplus_uid_table frozen_uids;
static DEFINE_SPINLOCK(frozen_lock);

bool oplus_uid_is_frozen(u32 uid)
{
	unsigned long flags;
	bool has;

	spin_lock_irqsave(&frozen_lock, flags);
	has = oplus_uid_has(&frozen_uids, uid);
	spin_unlock_irqrestore(&frozen_lock, flags);
	return has;
}

static int hans_uid_attr(struct genl_info *info, u32 *uid)
{
	struct nlattr *nla;
	unsigned int id = READ_ONCE(hans_attr_uid);

	if (!info || !uid || id < 1 || id > OPLUS_HANS_ATTR_MAX)
		return -EINVAL;
	nla = info->attrs[id];
	if (!nla || nla_len(nla) < (int)sizeof(u32))
		return -EINVAL;
	*uid = nla_get_u32(nla);
	return 0;
}

static int hans_add_uid(u32 uid)
{
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&frozen_lock, flags);
	rc = oplus_uid_add(&frozen_uids, uid);
	spin_unlock_irqrestore(&frozen_lock, flags);
	return rc < 0 ? rc : 0;
}

static int hans_del_uid(u32 uid)
{
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&frozen_lock, flags);
	rc = oplus_uid_del(&frozen_uids, uid);
	spin_unlock_irqrestore(&frozen_lock, flags);
	return rc;
}

static const struct nla_policy hans_policy[OPLUS_HANS_ATTR_MAX + 1] = {
	[1] = { .type = NLA_BINARY, .len = sizeof(u32) },
	[2] = { .type = NLA_BINARY, .len = sizeof(u32) },
	[3] = { .type = NLA_BINARY, .len = sizeof(u32) },
	[4] = { .type = NLA_BINARY, .len = sizeof(u32) },
	[5] = { .type = NLA_BINARY, .len = sizeof(u32) },
	[6] = { .type = NLA_BINARY, .len = sizeof(u32) },
	[7] = { .type = NLA_BINARY, .len = sizeof(u32) },
	[8] = { .type = NLA_BINARY, .len = sizeof(u32) },
};

/* Group name is unverified. Userspace that only looks up the family still
 * sees id of "oplus_hans". 待真机日志核对.
 */
static const struct genl_multicast_group hans_mcgrps[] = {
	{ .name = "events" },
};

static int hans_doit(struct sk_buff *skb, struct genl_info *info)
{
	u8 cmd = info->genlhdr->cmd;
	u32 uid;
	int rc;

	if (cmd == READ_ONCE(hans_cmd_add_uid)) {
		rc = hans_uid_attr(info, &uid);
		if (rc)
			return rc;
		return hans_add_uid(uid);
	}
	if (cmd == READ_ONCE(hans_cmd_del_uid) &&
	    cmd != READ_ONCE(hans_cmd_add_uid)) {
		rc = hans_uid_attr(info, &uid);
		if (rc)
			return rc;
		return hans_del_uid(uid);
	}
	pr_warn_ratelimited("oplus_hans: unknown command %u\n", cmd);
	return -EOPNOTSUPP;
}

#define HANS_OP(n) {							\
	.cmd = (n),							\
	.doit = hans_doit,						\
	.flags = GENL_ADMIN_PERM,					\
	.validate = GENL_DONT_VALIDATE_STRICT,				\
}

static const struct genl_small_ops hans_ops[] = {
	HANS_OP(1),  HANS_OP(2),  HANS_OP(3),  HANS_OP(4),
	HANS_OP(5),  HANS_OP(6),  HANS_OP(7),  HANS_OP(8),
	HANS_OP(9),  HANS_OP(10), HANS_OP(11), HANS_OP(12),
	HANS_OP(13), HANS_OP(14), HANS_OP(15), HANS_OP(16),
	HANS_OP(17), HANS_OP(18), HANS_OP(19), HANS_OP(20),
	HANS_OP(21), HANS_OP(22), HANS_OP(23), HANS_OP(24),
	HANS_OP(25), HANS_OP(26), HANS_OP(27), HANS_OP(28),
	HANS_OP(29), HANS_OP(30), HANS_OP(31), HANS_OP(32),
};

static struct genl_family oplus_hans_family __ro_after_init = {
	.name = "oplus_hans",
	.version = 1,
	.maxattr = OPLUS_HANS_ATTR_MAX,
	.policy = hans_policy,
	.small_ops = hans_ops,
	.n_small_ops = ARRAY_SIZE(hans_ops),
	.mcgrps = hans_mcgrps,
	.n_mcgrps = ARRAY_SIZE(hans_mcgrps),
	.parallel_ops = true,
};

static int __init oplus_hans_init(void)
{
	int ret;

	BUILD_BUG_ON(ARRAY_SIZE(hans_ops) != OPLUS_HANS_CMD_SLOTS);
	ret = genl_register_family(&oplus_hans_family);
	if (ret) {
		pr_err("oplus_hans: register failed %d\n", ret);
		return ret;
	}
	pr_info("oplus_hans: family id %d name %s\n",
		oplus_hans_family.id, oplus_hans_family_name);
	return 0;
}
device_initcall(oplus_hans_init);
