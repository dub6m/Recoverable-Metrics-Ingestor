#include <gtest/gtest.h>

#include <cstdlib>

// Proves the test command fails when a check fails. Normally passes; set
// LAB_FORCE_FAIL=1 to make it fail on purpose (scripts/check_all.sh, step forced-failure-fails).
TEST(Harness, FailsWhenForced)
{
    EXPECT_EQ(std::getenv("LAB_FORCE_FAIL"), nullptr)
        << "LAB_FORCE_FAIL is set: this failure is deliberate";
}
