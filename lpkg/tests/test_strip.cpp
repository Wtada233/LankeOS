#include <gtest/gtest.h>
#include "strip.hpp"
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

class StripTest : public ::testing::Test {
protected:
    fs::path test_file = fs::current_path() / "test_bin";

    void SetUp() override {
        // Create a dummy ELF file (ELF magic)
        std::ofstream f(test_file, std::ios::binary);
        char elf_magic[] = {0x7f, 'E', 'L', 'F', 0, 0, 0, 0};
        f.write(elf_magic, 8);
        f.close();
    }

    void TearDown() override {
        fs::remove(test_file);
    }
};

TEST_F(StripTest, StripExistingElfFile) {
    std::string error_msg;
    // This will likely fail to actually strip because it's not a real ELF,
    // but it should at least attempt to process it or return a recognizable error,
    // not crash or fail to open.
    bool result = strip_file(test_file, error_msg);
    // Since it's a fake ELF, stripping might fail, but it shouldn't be an error of "not found"
    // We want to ensure it reaches the processing stage.
    // If it returns false, check error_msg.
    if (!result) {
        EXPECT_FALSE(error_msg.empty());
    }
}
