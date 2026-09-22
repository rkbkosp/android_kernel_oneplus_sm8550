// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "policy.h"

static void ux_bit_set_and_clear(struct kunit *test)
{
	u64 word = 0;

	oplus_ux_word_set(&word, true);
	KUNIT_EXPECT_TRUE(test, oplus_ux_word_test(word));
	KUNIT_EXPECT_EQ(test, word & ~BIT_ULL(0), 0ULL);
	word |= BIT_ULL(3);
	oplus_ux_word_set(&word, false);
	KUNIT_EXPECT_FALSE(test, oplus_ux_word_test(word));
	KUNIT_EXPECT_EQ(test, word, BIT_ULL(3));
	oplus_ux_word_set(NULL, true);
}

static void uid_parse_dedup(struct kunit *test)
{
	u32 out[8];
	int dups = -1;
	int n;

	n = oplus_parse_uids("10 10\n11,10 12", out, 8, &dups);
	KUNIT_EXPECT_EQ(test, n, 3);
	KUNIT_EXPECT_EQ(test, (int)dups, 2);
	KUNIT_EXPECT_EQ(test, out[0], 10U);
	KUNIT_EXPECT_EQ(test, out[1], 11U);
	KUNIT_EXPECT_EQ(test, out[2], 12U);
	KUNIT_EXPECT_EQ(test, oplus_parse_uids("99999999999", out, 8, NULL),
			-ERANGE);
	KUNIT_EXPECT_LT(test, oplus_parse_uids(NULL, out, 8, NULL), 0);
}

static void frozen_uid_add_dup_del(struct kunit *test)
{
	struct oplus_uid_table t;
	int rc;

	oplus_uid_table_reset(&t);
	rc = oplus_uid_add(&t, 1000);
	KUNIT_EXPECT_EQ(test, rc, 0);
	KUNIT_EXPECT_TRUE(test, oplus_uid_has(&t, 1000));
	rc = oplus_uid_add(&t, 1000);
	KUNIT_EXPECT_EQ(test, rc, 1);
	KUNIT_EXPECT_EQ(test, t.n, 1);
	rc = oplus_uid_del(&t, 1000);
	KUNIT_EXPECT_EQ(test, rc, 0);
	KUNIT_EXPECT_FALSE(test, oplus_uid_has(&t, 1000));
	KUNIT_EXPECT_EQ(test, oplus_uid_del(&t, 1000), -ENOENT);
}

static void iolimit_over_and_clear(struct kunit *test)
{
	u64 written;

	KUNIT_EXPECT_EQ(test, OPLUS_IOLIMIT_CLEAR, 0x5aULL);
	KUNIT_EXPECT_FALSE(test, oplus_iolimit_over(100, 200));
	KUNIT_EXPECT_FALSE(test, oplus_iolimit_over(200, 200));
	KUNIT_EXPECT_TRUE(test, oplus_iolimit_over(201, 200));
	KUNIT_EXPECT_FALSE(test, oplus_iolimit_over(1, 0));
	KUNIT_EXPECT_TRUE(test, oplus_iolimit_is_clear(OPLUS_IOLIMIT_CLEAR));
	KUNIT_EXPECT_FALSE(test, oplus_iolimit_is_clear(0));
	KUNIT_EXPECT_FALSE(test, oplus_iolimit_is_clear(200));
	KUNIT_EXPECT_FALSE(test, oplus_iolimit_over(9999, OPLUS_IOLIMIT_CLEAR));
	KUNIT_EXPECT_FALSE(test, oplus_iolimit_over(0, 10));

	/* Under and exactly at the limit: charged, not a stall. */
	written = 0;
	KUNIT_EXPECT_FALSE(test, oplus_iolimit_charge(&written, 200, 100));
	KUNIT_EXPECT_EQ(test, written, 100ULL);
	KUNIT_EXPECT_FALSE(test, oplus_iolimit_charge(&written, 200, 100));
	KUNIT_EXPECT_EQ(test, written, 200ULL);
	/* Past the limit: stall. */
	KUNIT_EXPECT_TRUE(test, oplus_iolimit_charge(&written, 200, 1));
	KUNIT_EXPECT_EQ(test, written, 201ULL);

	/* 0x5A clears: no charge and no stall, even if already over. */
	written = 9999;
	KUNIT_EXPECT_FALSE(test,
			   oplus_iolimit_charge(&written, OPLUS_IOLIMIT_CLEAR, 4096));
	KUNIT_EXPECT_EQ(test, written, 9999ULL);
	KUNIT_EXPECT_FALSE(test, oplus_iolimit_charge(&written, 0, 4096));
	KUNIT_EXPECT_EQ(test, written, 9999ULL);
	KUNIT_EXPECT_FALSE(test, oplus_iolimit_charge(NULL, 10, 1));
}

static void swappiness_bucket_select(struct kunit *test)
{
	struct oplus_swap_buckets b = {
		.high_pct = 50,
		.low_pct = 20,
		.sw_high = 10,
		.sw_mid = 50,
		.sw_low = 100,
	};

	KUNIT_EXPECT_EQ(test, oplus_swappiness_for_avail(80, 100, &b, 1), 10);
	KUNIT_EXPECT_EQ(test, oplus_swappiness_for_avail(50, 100, &b, 1), 10);
	KUNIT_EXPECT_EQ(test, oplus_swappiness_for_avail(30, 100, &b, 1), 50);
	KUNIT_EXPECT_EQ(test, oplus_swappiness_for_avail(19, 100, &b, 1), 100);
	KUNIT_EXPECT_EQ(test, oplus_swappiness_for_avail(1, 0, &b, 7), 7);
	b.high_pct = 10;
	b.low_pct = 40;
	KUNIT_EXPECT_EQ(test, oplus_swappiness_for_avail(5, 100, &b, 7), 7);
}

static struct kunit_case oplus_mm_bg_cases[] = {
	KUNIT_CASE(ux_bit_set_and_clear),
	KUNIT_CASE(uid_parse_dedup),
	KUNIT_CASE(frozen_uid_add_dup_del),
	KUNIT_CASE(swappiness_bucket_select),
	KUNIT_CASE(iolimit_over_and_clear),
	{}
};

static struct kunit_suite oplus_mm_bg_suite = {
	.name = "oplus_mm_bg_policy",
	.test_cases = oplus_mm_bg_cases,
};

kunit_test_suite(oplus_mm_bg_suite);
