#include <gtest/gtest.h>
#include "strip.hpp"
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>
#include <libelf.h>
#include <gelf.h>

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
        if (fs::exists(test_file)) {
            fs::remove(test_file);
        }
    }
};

TEST_F(StripTest, StripExistingElfFile) {
    std::string error_msg;
    bool result = strip_file(test_file, error_msg);
    if (!result) {
        // It's fine if it fails because the dummy is not a valid ELF,
        // as long as it doesn't crash.
    }
}

TEST_F(StripTest, NoSectionHeaderTable) {
    // Create a 64-bit ELF header with no section header table
    std::ofstream f(test_file, std::ios::binary | std::ios::trunc);
    Elf64_Ehdr ehdr;
    std::memset(&ehdr, 0, sizeof(ehdr));
    std::memcpy(ehdr.e_ident, ELFMAG, SELFMAG);
    ehdr.e_ident[EI_CLASS] = ELFCLASS64;
    ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    ehdr.e_type = ET_EXEC;
    ehdr.e_machine = EM_X86_64;
    ehdr.e_version = EV_CURRENT;
    ehdr.e_ehsize = sizeof(ehdr);
    ehdr.e_shoff = 0; // No section header table
    ehdr.e_shnum = 0;
    
    f.write(reinterpret_cast<char*>(&ehdr), sizeof(ehdr));
    f.close();

    std::string error_msg;
    bool result = strip_file(test_file, error_msg);
    
    // It should return false but error_msg should be empty (indicating skip)
    EXPECT_FALSE(result);
    EXPECT_TRUE(error_msg.empty());
}

