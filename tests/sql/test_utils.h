#pragma once

#include "sql/token.h"
#include <gtest/gtest.h>
using namespace letty;

inline void assert_tokens_equal(const std::vector<Token>& actual, const std::vector<Token>& expected) {
    ASSERT_EQ(actual.size(), expected.size());
    for (size_t i = 0; i < actual.size(); ++i) {
        EXPECT_EQ(actual[i].type, expected[i].type) << "Token " << i << " type mismatch.";
        EXPECT_EQ(actual[i].text, expected[i].text) << "Token " << i << " text mismatch.";
    }
}