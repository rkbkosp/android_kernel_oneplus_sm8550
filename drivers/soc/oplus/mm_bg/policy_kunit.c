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

static struct kunit_case oplus_mm_bg_cases[] = {
	KUNIT_CASE(ux_bit_set_and_clear),
	KUNIT_CASE(uid_parse_dedup),
	KUNIT_CASE(frozen_uid_add_dup_del),
	{}
};

static struct kunit_suite oplus_mm_bg_suite = {
	.name = "oplus_mm_bg_policy",
	.test_cases = oplus_mm_bg_cases,
};

kunit_test_suite(oplus_mm_bg_suite);
