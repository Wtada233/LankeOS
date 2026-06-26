#include <gtest/gtest.h>
#include "../main/src/vercmp/version.hpp"

TEST(VersionTest, Comparisons) {
    EXPECT_TRUE(version_compare("1.0", "2.0"));
    EXPECT_FALSE(version_compare("2.0", "1.0"));
    EXPECT_FALSE(version_compare("1.0", "1.0")); // strictly less
    
    EXPECT_TRUE(version_compare("1.0", "1.0.1"));
    EXPECT_TRUE(version_compare("1.0-alpha", "1.0"));
    EXPECT_TRUE(version_compare("1.0-alpha", "1.0-beta"));
    EXPECT_TRUE(version_compare("1.0-beta.1", "1.0-beta.2"));
}

TEST(VersionTest, Satisfaction) {
    EXPECT_TRUE(version_satisfies("1.0", ">=", "1.0"));
    EXPECT_TRUE(version_satisfies("2.0", ">=", "1.0"));
    EXPECT_FALSE(version_satisfies("1.0", ">=", "2.0"));
    EXPECT_TRUE(version_satisfies("1.0", "<", "2.0"));
    EXPECT_FALSE(version_satisfies("2.0", "<", "1.0"));
    EXPECT_TRUE(version_satisfies("1.0", "=", "1.0"));
    EXPECT_FALSE(version_satisfies("1.0", "!=", "1.0"));
}
