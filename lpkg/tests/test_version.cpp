#include <gtest/gtest.h>
#include "../main/src/version.hpp"
#include "../main/src/utils.hpp"

TEST(VersionTest, Comparisons) {
    EXPECT_TRUE(version_compare("1.0", "2.0"));
    EXPECT_FALSE(version_compare("2.0", "1.0"));
    EXPECT_FALSE(version_compare("1.0", "1.0")); // strictly less
    
    EXPECT_TRUE(version_compare("1.0", "1.0.1"));
    EXPECT_TRUE(version_compare("1.0-alpha", "1.0"));
    EXPECT_TRUE(version_compare("1.0-alpha", "1.0-beta"));
    EXPECT_TRUE(version_compare("1.0-beta.1", "1.0-beta.2"));
}

TEST(UtilsTest, ParsePackageFilename) {
    auto p1 = parse_package_filename("glibc-2.38.lpkg");
    EXPECT_EQ(p1.first, "glibc");
    EXPECT_EQ(p1.second, "2.38");

    auto p2 = parse_package_filename("some-lib-1.0.0-r1.lpkg");
    EXPECT_EQ(p2.first, "some-lib");
    EXPECT_EQ(p2.second, "1.0.0-r1");

    EXPECT_THROW(parse_package_filename("invalid_filename.tar.gz"), std::exception);
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
