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

static struct kunit_case oplus_mm_bg_cases[] = {
	KUNIT_CASE(ux_bit_set_and_clear),
	{}
};

static struct kunit_suite oplus_mm_bg_suite = {
	.name = "oplus_mm_bg_policy",
	.test_cases = oplus_mm_bg_cases,
};

kunit_test_suite(oplus_mm_bg_suite);
