/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _OPLUS_MM_BG_POLICY_H
#define _OPLUS_MM_BG_POLICY_H

#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/types.h>

/*
 * Pure decisions shared by the built-in and by kunit. No binder, no CMA,
 * no task_struct. Callers supply the storage.
 */

#define OPLUS_UID_CAP		128
#define OPLUS_IOLIMIT_CLEAR	0x5aULL

struct oplus_uid_table {
	u32 uid[OPLUS_UID_CAP];
	int n;
};

struct oplus_swap_buckets {
	unsigned int high_pct;
	unsigned int low_pct;
	int sw_high;
	int sw_mid;
	int sw_low;
};

static inline void oplus_ux_word_set(u64 *word, bool on)
{
	if (!word)
		return;
	if (on)
		*word |= BIT_ULL(0);
	else
		*word &= ~BIT_ULL(0);
}

static inline bool oplus_ux_word_test(u64 word)
{
	return !!(word & BIT_ULL(0));
}

static inline void oplus_uid_table_reset(struct oplus_uid_table *t)
{
	if (!t)
		return;
	t->n = 0;
}

/* 0 added, 1 already present, -ENOSPC if the table is full. */
static inline int oplus_uid_add(struct oplus_uid_table *t, u32 uid)
{
	int i;

	if (!t)
		return -EINVAL;
	for (i = 0; i < t->n; i++) {
		if (t->uid[i] == uid)
			return 1;
	}
	if (t->n >= OPLUS_UID_CAP)
		return -ENOSPC;
	t->uid[t->n++] = uid;
	return 0;
}

/* 0 removed, -ENOENT if the uid was not present. */
static inline int oplus_uid_del(struct oplus_uid_table *t, u32 uid)
{
	int i;

	if (!t)
		return -EINVAL;
	for (i = 0; i < t->n; i++) {
		if (t->uid[i] != uid)
			continue;
		t->uid[i] = t->uid[t->n - 1];
		t->n--;
		return 0;
	}
	return -ENOENT;
}

static inline bool oplus_uid_has(const struct oplus_uid_table *t, u32 uid)
{
	int i;

	if (!t)
		return false;
	for (i = 0; i < t->n; i++) {
		if (t->uid[i] == uid)
			return true;
	}
	return false;
}

/*
 * Parse decimal uids separated by anything that is not a digit.
 * Duplicates inside this buffer are counted in *ndup and stored once.
 * Returns the number of unique uids, or a negative errno.
 */
static inline int oplus_parse_uids(const char *s, u32 *out, int cap, int *ndup)
{
	int n = 0;
	int dups = 0;

	if (!s || !out || cap <= 0)
		return -EINVAL;
	while (*s) {
		u64 v = 0;
		int i;
		bool seen = false;

		if (*s < '0' || *s > '9') {
			s++;
			continue;
		}
		while (*s >= '0' && *s <= '9') {
			v = v * 10 + (*s - '0');
			if (v > 0xffffffffULL)
				return -ERANGE;
			s++;
		}
		for (i = 0; i < n; i++) {
			if (out[i] == (u32)v) {
				seen = true;
				break;
			}
		}
		if (seen) {
			dups++;
			continue;
		}
		if (n >= cap)
			return -ENOSPC;
		out[n++] = (u32)v;
	}
	if (ndup)
		*ndup = dups;
	return n;
}

/*
 * Pick a swappiness from remaining memory. pct is avail/total in
 * percent. At or above high_pct the high (less aggressive) value is
 * used; at or above low_pct the middle value; otherwise the low value.
 */
static inline int oplus_swappiness_for_avail(unsigned long avail,
					     unsigned long total,
					     const struct oplus_swap_buckets *b,
					     int fallback)
{
	unsigned int pct;

	if (!b || !total)
		return fallback;
	if (b->high_pct < b->low_pct)
		return fallback;
	pct = (unsigned int)((avail * 100UL) / total);
	if (pct > 100)
		pct = 100;
	if (pct >= b->high_pct)
		return b->sw_high;
	if (pct >= b->low_pct)
		return b->sw_mid;
	return b->sw_low;
}

/* 0 and 0x5A both mean "do not throttle". 0x5A is the explicit clear. */
static inline bool oplus_iolimit_is_clear(u64 limit)
{
	return limit == OPLUS_IOLIMIT_CLEAR;
}

static inline bool oplus_iolimit_over(u64 written, u64 limit)
{
	if (limit == 0 || limit == OPLUS_IOLIMIT_CLEAR)
		return false;
	return written > limit;
}

/*
 * Charge bytes and report whether this write should stall.
 * No trace hook: 0 and 0x5A do not charge and do not stall.
 * Stall only once written is past the limit.
 */
static inline bool oplus_iolimit_charge(u64 *written, u64 limit, u64 bytes)
{
	if (!written || !limit || oplus_iolimit_is_clear(limit))
		return false;
	*written += bytes;
	return oplus_iolimit_over(*written, limit);
}

#endif /* _OPLUS_MM_BG_POLICY_H */
