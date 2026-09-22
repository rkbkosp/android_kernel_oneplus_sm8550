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

#include <linux/cred.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/ratelimit.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/uidgid.h>
#include <net/genetlink.h>
#include <trace/hooks/binder.h>
#include <trace/hooks/signal.h>

#include "binder_internal.h"
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

static struct genl_family oplus_hans_family;

static DEFINE_RATELIMIT_STATE(hans_evt_rs, HZ, 64);

/*
 * Event attribute numbers and the string names are placeholders.
 * 待真机日志核对. "FROZEN_TRANS" and "signal-freeze" are the names
 * KERNEL_MODULES.md records for this module. "packet" is not.
 */
static void hans_send_event(const char *event, u32 uid, int sig)
{
	struct sk_buff *skb;
	void *hdr;
	int rc;

	if (!__ratelimit(&hans_evt_rs))
		return;
	skb = genlmsg_new(NLMSG_GOODSIZE, GFP_ATOMIC);
	if (!skb)
		return;
	hdr = genlmsg_put(skb, 0, 0, &oplus_hans_family, 0,
			  READ_ONCE(hans_cmd_event));
	if (!hdr)
		goto drop;
	if (nla_put_string(skb, 2, event) ||
	    nla_put_u32(skb, 1, uid) ||
	    (sig >= 0 && nla_put_s32(skb, 3, sig)))
		goto drop;
	genlmsg_end(skb, hdr);
	rc = genlmsg_multicast(&oplus_hans_family, skb, 0, 0, GFP_ATOMIC);
	if (rc == -ESRCH)
		return;
	if (rc)
		pr_warn_ratelimited("oplus_hans: event %s uid %u rc %d\n",
				    event, uid, rc);
	return;
drop:
	nlmsg_free(skb);
}

static u32 hans_proc_uid(struct binder_proc *proc)
{
	if (!proc || !proc->tsk)
		return (u32)-1;
	return from_kuid(&init_user_ns, task_uid(proc->tsk));
}

static void hans_binder_hit(struct binder_proc *target)
{
	u32 uid = hans_proc_uid(target);

	if (uid == (u32)-1 || !oplus_uid_is_frozen(uid))
		return;
	hans_send_event("FROZEN_TRANS", uid, -1);
}

static void hans_on_trans(void *data, struct binder_proc *target_proc,
			  struct binder_proc *proc, struct binder_thread *thread,
			  struct binder_transaction_data *tr)
{
	(void)data;
	(void)proc;
	(void)thread;
	(void)tr;
	hans_binder_hit(target_proc);
}

static void hans_on_reply(void *data, struct binder_proc *target_proc,
			  struct binder_proc *proc, struct binder_thread *thread,
			  struct binder_transaction_data *tr)
{
	(void)data;
	(void)proc;
	(void)thread;
	(void)tr;
	hans_binder_hit(target_proc);
}

static void hans_on_preset(void *data, struct hlist_head *hhead,
			   struct mutex *lock)
{
	struct binder_proc *proc;

	(void)data;
	if (!hhead || !lock)
		return;
	mutex_lock(lock);
	hlist_for_each_entry(proc, hhead, proc_node)
		hans_binder_hit(proc);
	mutex_unlock(lock);
}

static void hans_on_sig(void *data, int sig, struct task_struct *killer,
			struct task_struct *dst)
{
	u32 uid;

	(void)data;
	(void)killer;
	if (!dst)
		return;
	uid = from_kuid(&init_user_ns, task_uid(dst));
	if (!oplus_uid_is_frozen(uid))
		return;
	hans_send_event("signal-freeze", uid, sig);
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

	(void)skb;
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
	WARN_ON(register_trace_android_vh_binder_trans(hans_on_trans, NULL));
	WARN_ON(register_trace_android_vh_binder_reply(hans_on_reply, NULL));
	WARN_ON(register_trace_android_vh_binder_preset(hans_on_preset, NULL));
	WARN_ON(register_trace_android_vh_do_send_sig_info(hans_on_sig, NULL));
	return 0;
}
device_initcall(oplus_hans_init);
