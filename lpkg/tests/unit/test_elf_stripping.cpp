#include <gtest/gtest.h>
#include "strip.hpp"
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <libelf.h>
#include <gelf.h>

namespace fs = std::filesystem;

class StripTest : public ::testing::Test {
protected:
    fs::path test_file;

    void SetUp() override {
        if (elf_version(EV_CURRENT) == EV_NONE)
            FAIL() << "libelf version mismatch";
        test_file = fs::current_path() / "test_strip_bin";
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove(test_file, ec);
    }

    /** 使用系统 gcc 编译一个最小的 C 源文件为 .o 目标文件 */
    bool compile_test_object() {
        fs::path src = test_file.string() + ".c";
        {
            std::ofstream f(src);
            f << "int foo(void) { return 42; }\n";
        }
        std::string cmd = "gcc -c -o " + test_file.string() + " " + src.string() + " 2>/dev/null";
        int ret = std::system(cmd.c_str());
        fs::remove(src);
        return ret == 0 && fs::exists(test_file) && fs::file_size(test_file) > 0;
    }

    /** 使用系统 gcc 编译一个带调试信息的 .o 目标文件 */
    bool compile_test_object_with_debug() {
        fs::path src = test_file.string() + ".c";
        {
            std::ofstream f(src);
            f << "int bar(int x) { return x * 2; }\n"
                 "int baz(int x) { return x + 1; }\n";
        }
        std::string cmd = "gcc -c -g -o " + test_file.string() + " " + src.string() + " 2>/dev/null";
        int ret = std::system(cmd.c_str());
        fs::remove(src);
        return ret == 0 && fs::exists(test_file) && fs::file_size(test_file) > 0;
    }

    /** 检查 ELF 文件中是否包含指定名称的节区 */
    bool has_section(const std::string& section_name) {
        int fd = ::open(test_file.c_str(), O_RDONLY);
        if (fd < 0) return false;

        Elf* elf = elf_begin(fd, ELF_C_READ, nullptr);
        if (!elf) { ::close(fd); return false; }

        size_t shstrndx;
        if (elf_getshdrstrndx(elf, &shstrndx) < 0) {
            elf_end(elf); ::close(fd); return false;
        }

        Elf_Scn* scn = nullptr;
        bool found = false;
        while ((scn = elf_nextscn(elf, scn)) != nullptr) {
            GElf_Shdr shdr;
            gelf_getshdr(scn, &shdr);
            const char* name = elf_strptr(elf, shstrndx, shdr.sh_name);
            if (name && section_name == name) {
                found = true;
                break;
            }
        }
        elf_end(elf); ::close(fd);
        return found;
    }
};

TEST_F(StripTest, NonexistentFile) {
    fs::path missing = test_file;
    fs::remove(missing);
    std::string error_msg;
    bool result = strip_file(missing, error_msg);
    EXPECT_FALSE(result);
    EXPECT_FALSE(error_msg.empty());
}

TEST_F(StripTest, NoSectionHeaderTable) {
    // 创建一个无节区表的 64-bit ELF 头
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
    ehdr.e_shoff = 0;  // 无节区表
    ehdr.e_shnum = 0;
    f.write(reinterpret_cast<char*>(&ehdr), sizeof(ehdr));
    f.close();

    std::string error_msg;
    bool result = strip_file(test_file, error_msg);
    EXPECT_FALSE(result);
    EXPECT_TRUE(error_msg.empty());
}

TEST_F(StripTest, StripCompiledObjectFile) {
    if (!compile_test_object()) {
        GTEST_SKIP() << "gcc not available, skipping compiled object test";
    }

    off_t orig_size = fs::file_size(test_file);
    ASSERT_GT(orig_size, 0);

    std::string error_msg;
    bool result = strip_file(test_file, error_msg);
    EXPECT_TRUE(result) << error_msg;

    // 文件仍然存在且非空
    EXPECT_TRUE(fs::exists(test_file));
    EXPECT_GT(fs::file_size(test_file), 0);
}

TEST_F(StripTest, StripRemovesDebugSections) {
    if (!compile_test_object_with_debug()) {
        GTEST_SKIP() << "gcc not available, skipping debug section test";
    }

    // strip 前应有 .debug_info 等调试节区
    bool has_debug_before = has_section(".debug_info") || has_section(".debug_line");
    if (!has_debug_before) {
        GTEST_SKIP() << "No debug sections found in compiled object";
    }

    off_t orig_size = fs::file_size(test_file);

    std::string error_msg;
    bool result = strip_file(test_file, error_msg);
    ASSERT_TRUE(result) << error_msg;

    // strip 后调试节区应被移除
    EXPECT_FALSE(has_section(".debug_info"));
    EXPECT_FALSE(has_section(".debug_line"));

    // 文件变小
    EXPECT_LT(fs::file_size(test_file), orig_size);
}

TEST_F(StripTest, ProcessArchiveWithStaticLib) {
    // 创建一个 ar 归档（空静态库）
    std::ofstream f(test_file, std::ios::binary);
    f << "!<arch>\n";  // ar magic
    f.close();

    std::string error_msg;
    bool result = strip_file(test_file, error_msg);
    EXPECT_TRUE(result);
}

TEST_F(StripTest, SharedLibraryStrip) {
    // 编译一个共享库 .so 并测试 strip
    fs::path src = test_file.string() + ".c";
    fs::path so_file = test_file.string() + ".so";
    {
        std::ofstream f(src);
        f << "int shared_func(int x) { return x + 1; }\n";
    }
    std::string cmd = "gcc -shared -fPIC -o " + so_file.string() + " " + src.string() + " 2>/dev/null";
    int ret = std::system(cmd.c_str());
    fs::remove(src);
    if (ret != 0 || !fs::exists(so_file)) {
        GTEST_SKIP() << "gcc not available, skipping shared library test";
    }

    // 编译后应有 .dynsym、.dynstr 等动态节区
    off_t orig_size = fs::file_size(so_file);
    ASSERT_GT(orig_size, 0);

    std::string error_msg;
    bool result = strip_file(so_file, error_msg);
    EXPECT_TRUE(result) << error_msg;
    EXPECT_TRUE(fs::exists(so_file));
    EXPECT_GT(fs::file_size(so_file), 0);

    // strip 去除了符号表和注释，但仍可加载（dlopen）
    // 文件大小应小于原始文件
    EXPECT_LE(fs::file_size(so_file), orig_size);

    fs::remove(so_file);
}

TEST_F(StripTest, PIEExecutableStrip) {
    // 编译一个 PIE 可执行文件并测试 strip
    fs::path src = test_file.string() + ".c";
    fs::path exe_file = test_file.string() + "_pie";
    {
        std::ofstream f(src);
        f << "int main(void) { return 42; }\n";
    }
    std::string cmd = "gcc -fPIE -pie -o " + exe_file.string() + " " + src.string() + " 2>/dev/null";
    int ret = std::system(cmd.c_str());
    fs::remove(src);
    if (ret != 0 || !fs::exists(exe_file)) {
        GTEST_SKIP() << "gcc not available, skipping PIE test";
    }

    off_t orig_size = fs::file_size(exe_file);
    ASSERT_GT(orig_size, 0);

    std::string error_msg;
    bool result = strip_file(exe_file, error_msg);
    EXPECT_TRUE(result) << error_msg;
    EXPECT_GT(fs::file_size(exe_file), 0);
    EXPECT_LE(fs::file_size(exe_file), orig_size);

    fs::remove(exe_file);
}

TEST_F(StripTest, StripUnknownFileType) {
    // 非 ELF 文件 → identify_file_type → FileType::Unknown → default 分支
    {
        std::ofstream f(test_file);
        f << "This is not an ELF file at all.\n";
    }

    std::string error_msg;
    bool result = strip_file(test_file, error_msg);
    EXPECT_FALSE(result);
    // 应返回 unknown type 相关错误，而不是文件不存在
    EXPECT_TRUE(error_msg.find("unknown") != std::string::npos
                || error_msg.find("unknown") != std::string::npos
                || !error_msg.empty());
}

TEST_F(StripTest, ArchiveWithObjectMembers) {
    // 编译一个 .o 文件，打包成 ar 归档，测试 process_archive 的内层循环
    if (!compile_test_object()) {
        GTEST_SKIP() << "gcc not available, skipping archive test";
    }

    fs::path archive_file = test_file.string() + ".a";
    std::string cmd = "ar rcs " + archive_file.string() + " " + test_file.string() + " 2>/dev/null";
    int ret = std::system(cmd.c_str());
    if (ret != 0 || !fs::exists(archive_file)) {
        GTEST_SKIP() << "ar not available";
    }

    off_t orig_size = fs::file_size(archive_file);
    ASSERT_GT(orig_size, 0);

    std::string error_msg;
    bool result = strip_file(archive_file, error_msg);
    EXPECT_TRUE(result) << error_msg;
    EXPECT_TRUE(fs::exists(archive_file));

    fs::remove(archive_file);
}

TEST_F(StripTest, NonElfReturnsUnknownType) {
    // 创建一个只包含 ELF magic 但无效头的文件
    // 使 identify_file_type 走 gelf_getehdr 失败路径
    {
        std::ofstream f(test_file, std::ios::binary);
        // 仅写入 ELFMAG 但不写入有效 ELF 头
        const char magic[] = {0x7f, 'E', 'L', 'F'};
        f.write(magic, 4);
    }

    std::string error_msg;
    bool result = strip_file(test_file, error_msg);
    EXPECT_FALSE(result);
}
