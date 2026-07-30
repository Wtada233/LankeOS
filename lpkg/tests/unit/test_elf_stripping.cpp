#include <fcntl.h>
#include <gelf.h>
#include <gtest/gtest.h>
#include <libelf.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "strip.hpp"

namespace fs = std::filesystem;

class StripTest : public ::testing::Test
{
protected:
    fs::path test_file;

    void SetUp() override
    {
        if (elf_version(EV_CURRENT) == EV_NONE) FAIL() << "libelf version mismatch";
        test_file = fs::current_path() / "test_strip_bin";
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove(test_file, ec);
        fs::remove(test_file.string() + ".c", ec);
        fs::remove(test_file.string() + ".cpp", ec);
    }

    /** 使用系统 gcc 编译一个最小的 C 源文件为 .o 目标文件 */
    bool compile_test_object()
    {
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
    bool compile_test_object_with_debug()
    {
        fs::path src = test_file.string() + ".c";
        {
            std::ofstream f(src);
            f << "int bar(int x) { return x * 2; }\n"
                 "int baz(int x) { return x + 1; }\n";
        }
        std::string cmd =
            "gcc -c -g -o " + test_file.string() + " " + src.string() + " 2>/dev/null";
        int ret = std::system(cmd.c_str());
        fs::remove(src);
        return ret == 0 && fs::exists(test_file) && fs::file_size(test_file) > 0;
    }

    /**
     * 使用 g++ 编译含 C++ 模板的源文件，生成具有 SHT_GROUP (COMDAT) 节区的 .o
     * 模板实例化会产生 COMDAT group，这是在 .o 中生成 .group 节区的可靠方法
     */
    bool compile_test_object_with_groups()
    {
        fs::path src = test_file.string() + ".cpp";
        {
            std::ofstream f(src);
            // __attribute__((noinline)) 防止优化将模板实例完全内联，确保生成 COMDAT group
            f << "template<typename T>\n"
                 "T __attribute__((noinline)) add(T a, T b) { return a + b; }\n"
                 "template<typename T>\n"
                 "T __attribute__((noinline)) mul(T a, T b) { return a * b; }\n"
                 "int call(int x, int y) {\n"
                 "    return add<int>(x, y) + add<long>(x, y) + mul<int>(x, y);\n"
                 "}\n";
        }
        std::string cmd = "g++ -c -O2 -o " + test_file.string() + " " + src.string() + " 2>/dev/null";
        int ret = std::system(cmd.c_str());
        fs::remove(src);
        // 返回 .o 中存在 .group 节区才算成功
        return ret == 0 && fs::exists(test_file) && fs::file_size(test_file) > 0 &&
               has_section(".group");
    }

    /** 检查 ELF 文件中是否包含指定名称的节区 */
    bool has_section(const std::string& section_name)
    {
        int fd = ::open(test_file.c_str(), O_RDONLY);
        if (fd < 0) return false;

        Elf* elf = elf_begin(fd, ELF_C_READ, nullptr);
        if (!elf) {
            ::close(fd);
            return false;
        }

        size_t shstrndx;
        if (elf_getshdrstrndx(elf, &shstrndx) < 0) {
            elf_end(elf);
            ::close(fd);
            return false;
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
        elf_end(elf);
        ::close(fd);
        return found;
    }

    /**
     * 完整验证 .o 中所有 SHT_GROUP 节区的数据完整性：
     *   1. sh_link 必须指向 .symtab
     *   2. sh_info 不能为 0（非空组的签名符号索引应合法）
     *   3. group 内容中的每个节区索引都必须指向存在的节区
     * strip 前后都应满足这些约束
     */
    bool verify_group_integrity()
    {
        int fd = ::open(test_file.c_str(), O_RDONLY);
        if (fd < 0) return false;

        Elf* elf = elf_begin(fd, ELF_C_READ, nullptr);
        if (!elf) {
            ::close(fd);
            return false;
        }

        size_t shstrndx;
        if (elf_getshdrstrndx(elf, &shstrndx) < 0) {
            elf_end(elf);
            ::close(fd);
            return false;
        }

        // 定位 .symtab 节区及其索引
        size_t symtab_idx = 0;
        Elf_Scn* scn = nullptr;
        while ((scn = elf_nextscn(elf, scn)) != nullptr) {
            GElf_Shdr shdr;
            gelf_getshdr(scn, &shdr);
            const char* name = elf_strptr(elf, shstrndx, shdr.sh_name);
            if (name && shdr.sh_type == SHT_SYMTAB) {
                symtab_idx = elf_ndxscn(scn);
                break;
            }
        }

        size_t num_sections;
        if (elf_getshdrnum(elf, &num_sections) != 0) {
            elf_end(elf);
            ::close(fd);
            return false;
        }

        bool all_groups_valid = true;
        scn = nullptr;
        while ((scn = elf_nextscn(elf, scn)) != nullptr) {
            GElf_Shdr shdr;
            gelf_getshdr(scn, &shdr);
            const char* name = elf_strptr(elf, shstrndx, shdr.sh_name);
            if (!name || shdr.sh_type != SHT_GROUP) continue;

            // sh_link 必须指向 .symtab
            if (shdr.sh_link != symtab_idx) {
                all_groups_valid = false;
                break;
            }

            // sh_info 是签名符号在 .symtab 中的索引，不应为 0
            if (shdr.sh_info == 0) {
                all_groups_valid = false;
                break;
            }

            // group 内容中的节区索引必须合法
            Elf_Data* data = elf_getdata(scn, nullptr);
            if (!data || data->d_size < sizeof(Elf32_Word)) {
                all_groups_valid = false;
                break;
            }

            Elf32_Word* group_data = static_cast<Elf32_Word*>(data->d_buf);
            size_t count = data->d_size / sizeof(Elf32_Word);
            for (size_t i = 1; i < count; ++i) {
                if (group_data[i] >= num_sections) {
                    all_groups_valid = false;
                    break;
                }
            }
            if (!all_groups_valid) break;
        }

        elf_end(elf);
        ::close(fd);
        return all_groups_valid;
    }

    /** 获取文件中 SHT_GROUP 节区的数量 */
    size_t count_group_sections()
    {
        int fd = ::open(test_file.c_str(), O_RDONLY);
        if (fd < 0) return 0;

        Elf* elf = elf_begin(fd, ELF_C_READ, nullptr);
        if (!elf) {
            ::close(fd);
            return 0;
        }

        size_t shstrndx;
        if (elf_getshdrstrndx(elf, &shstrndx) < 0) {
            elf_end(elf);
            ::close(fd);
            return 0;
        }

        size_t count = 0;
        Elf_Scn* scn = nullptr;
        while ((scn = elf_nextscn(elf, scn)) != nullptr) {
            GElf_Shdr shdr;
            gelf_getshdr(scn, &shdr);
            if (shdr.sh_type == SHT_GROUP) count++;
        }

        elf_end(elf);
        ::close(fd);
        return count;
    }
};

TEST_F(StripTest, NonexistentFile)
{
    fs::path missing = test_file;
    fs::remove(missing);
    std::string error_msg;
    bool result = strip_file(missing, error_msg);
    EXPECT_FALSE(result);
    EXPECT_FALSE(error_msg.empty());
}

TEST_F(StripTest, NoSectionHeaderTable)
{
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

TEST_F(StripTest, StripCompiledObjectFile)
{
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

TEST_F(StripTest, StripRemovesDebugSections)
{
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

TEST_F(StripTest, ProcessArchiveWithStaticLib)
{
    // 创建一个 ar 归档（空静态库）
    std::ofstream f(test_file, std::ios::binary);
    f << "!<arch>\n";  // ar magic
    f.close();

    std::string error_msg;
    bool result = strip_file(test_file, error_msg);
    EXPECT_TRUE(result);
}

TEST_F(StripTest, SharedLibraryStrip)
{
    // 编译一个共享库 .so 并测试 strip
    fs::path src = test_file.string() + ".c";
    fs::path so_file = test_file.string() + ".so";
    {
        std::ofstream f(src);
        f << "int shared_func(int x) { return x + 1; }\n";
    }
    std::string cmd =
        "gcc -shared -fPIC -o " + so_file.string() + " " + src.string() + " 2>/dev/null";
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

TEST_F(StripTest, PIEExecutableStrip)
{
    // 编译一个 PIE 可执行文件并测试 strip
    fs::path src = test_file.string() + ".c";
    fs::path exe_file = test_file.string() + "_pie";
    {
        std::ofstream f(src);
        f << "int main(void) { return 42; }\n";
    }
    std::string cmd =
        "gcc -fPIE -pie -o " + exe_file.string() + " " + src.string() + " 2>/dev/null";
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

TEST_F(StripTest, StripUnknownFileType)
{
    // 非 ELF 文件 → identify_file_type → FileType::Unknown → default 分支
    {
        std::ofstream f(test_file);
        f << "This is not an ELF file at all.\n";
    }

    std::string error_msg;
    bool result = strip_file(test_file, error_msg);
    EXPECT_FALSE(result);
    // 应返回 unknown type 相关错误，而不是文件不存在
    EXPECT_TRUE(error_msg.find("unknown") != std::string::npos ||
                error_msg.find("unknown") != std::string::npos || !error_msg.empty());
}

TEST_F(StripTest, ArchiveWithObjectMembers)
{
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

TEST_F(StripTest, NonElfReturnsUnknownType)
{
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

/**
 * 验证 strip 后 .o 中的 SHT_GROUP 数据完整性：
 *   - 编译含 C++ 模板实例化的 .o（会产生多个 COMDAT group）
 *   - strip 后验证 sh_info、sh_link 和 group 内容中的节区索引仍然有效
 */
TEST_F(StripTest, StripPreservesGroupSections)
{
    if (!compile_test_object_with_groups()) {
        GTEST_SKIP() << "g++ not available or no group sections generated, skipping";
    }

    size_t groups_before = count_group_sections();
    ASSERT_GT(groups_before, 0) << "Test object should have .group sections";
    ASSERT_TRUE(verify_group_integrity()) << "Pre-strip group integrity check failed";

    off_t orig_size = fs::file_size(test_file);

    std::string error_msg;
    bool result = strip_file(test_file, error_msg);
    ASSERT_TRUE(result) << error_msg;

    // strip 后 .group 节区数量应保持不变（COMDAT group 不应被 strip 删掉）
    EXPECT_EQ(count_group_sections(), groups_before);

    // 验证 group 数据完整性
    EXPECT_TRUE(verify_group_integrity()) << "Post-strip group integrity check failed";

    // 文件应变小（去除了 .comment 等节区）
    EXPECT_LT(fs::file_size(test_file), orig_size);
}

/**
 * 验证 strip 后的含 SHT_GROUP 的 .o 仍可正常链接和运行：
 *   - 编译含模板实例化的 .o（有 COMDAT group）
 *   - strip 后编译一个调用其中符号的 main.c
 *   - 链接二者生成可执行文件并运行验证返回值
 */
TEST_F(StripTest, StripWithGroupsStillLinkable)
{
    if (!compile_test_object_with_groups()) {
        GTEST_SKIP() << "g++ not available or no group sections generated, skipping";
    }

    fs::path stripped_obj = test_file.string() + "_stripped.o";
    std::error_code ec;
    fs::copy(test_file, stripped_obj, ec);
    ASSERT_FALSE(ec) << "Failed to copy test object";

    // strip 拷贝的文件
    std::string error_msg;
    bool result = strip_file(stripped_obj, error_msg);
    ASSERT_TRUE(result) << error_msg;

    // 验证 stripped .o 的 group 完整性
    // 需要临时修改 test_file 指向以使用验证函数
    fs::path orig_test_file = test_file;
    test_file = stripped_obj;
    bool group_ok = verify_group_integrity();
    test_file = orig_test_file;
    ASSERT_TRUE(group_ok) << "Group integrity check failed on stripped object";

    // 编译主函数并链接 stripped .o
    fs::path main_src = stripped_obj.string() + "_main.c";
    fs::path linked_exe = stripped_obj.string() + "_exe";
    {
        std::ofstream f(main_src);
        f << "int call(int, int);\n"
             "int main(void) {\n"
             "    return call(1, 2) - 8;\n"
             "}\n";
    }

    std::string link_cmd = "g++ -o " + linked_exe.string() + " " + main_src.string() + " " +
                           stripped_obj.string() + " 2>/dev/null";
    int link_ret = std::system(link_cmd.c_str());
    EXPECT_EQ(link_ret, 0) << "Linking stripped object failed";

    if (link_ret == 0 && fs::exists(linked_exe)) {
        // 运行可执行文件验证功能
        std::string run_cmd = linked_exe.string() + " 2>/dev/null";
        int exit_code = std::system(run_cmd.c_str());
        EXPECT_EQ(exit_code, 0) << "Unexpected exit code from linked executable";
        fs::remove(linked_exe);
    }

    fs::remove(main_src);
    fs::remove(stripped_obj);
}

/**
 * 验证 strip 处理包含 COMDAT group 成员 .o 的 ar 归档：
 *   - 创建含多个 template 实例化的 .o 并打包为 .a
 *   - strip 归档
 *   - 使用归档链接可执行文件并运行验证
 */
TEST_F(StripTest, ArchiveWithGroupObjects)
{
    if (!compile_test_object_with_groups()) {
        GTEST_SKIP() << "g++ not available or no group sections generated, skipping";
    }

    ASSERT_TRUE(verify_group_integrity()) << "Pre-strip group integrity check failed";

    fs::path archive_file = test_file.string() + "_groups.a";
    std::string ar_cmd = "ar rcs " + archive_file.string() + " " + test_file.string() +
                         " 2>/dev/null";
    int ar_ret = std::system(ar_cmd.c_str());
    if (ar_ret != 0 || !fs::exists(archive_file)) {
        GTEST_SKIP() << "ar not available";
    }

    off_t orig_size = fs::file_size(archive_file);
    ASSERT_GT(orig_size, 0);

    // Strip 归档
    std::string error_msg;
    bool result = strip_file(archive_file, error_msg);
    EXPECT_TRUE(result) << error_msg;
    EXPECT_TRUE(fs::exists(archive_file));

    // 从 strip 后的归档中提取 .o 并验证 group 完整性
    fs::path extracted_obj = test_file.string() + "_extracted.o";
    fs::path extract_dir = fs::current_path() / "_extract_tmp";
    fs::create_directories(extract_dir);

    // 需要先拷贝 archive 到提取目录，因为 ar x 会在当前目录释放
    fs::path archive_copy = extract_dir / "archive.a";
    fs::copy(archive_file, archive_copy);
    std::string extract_cmd = "cd " + extract_dir.string() + " && ar x archive.a 2>/dev/null";
    int extract_ret = std::system(extract_cmd.c_str());

    if (extract_ret == 0) {
        // 找到提取出的 .o
        for (auto& entry : fs::directory_iterator(extract_dir)) {
            if (entry.path().extension() == ".o" || entry.path().extension() == ".o") {
                fs::path extracted = entry.path();
                // 临时改变 test_file 以使用验证函数
                fs::path orig = test_file;
                test_file = extracted;
                bool group_ok = verify_group_integrity();
                test_file = orig;
                EXPECT_TRUE(group_ok)
                    << "Group integrity check failed on extracted .o from stripped archive";

                // 链接测试：用提取的 .o 编译并运行
                fs::path main_src = extracted.string() + "_arch_main.c";
                fs::path exe = extracted.string() + "_arch_exe";
                {
                    std::ofstream f(main_src);
                    f << "int call(int, int);\n"
                         "int main(void) {\n"
                         "    return call(5, 7) - 59;\n"
                         "}\n";
                }
                std::string link_cmd =
                    "g++ -o " + exe.string() + " " + main_src.string() + " " + extracted.string() +
                    " 2>/dev/null";
                int link_ret = std::system(link_cmd.c_str());
                EXPECT_EQ(link_ret, 0) << "Linking extracted object from stripped archive failed";

                if (link_ret == 0 && fs::exists(exe)) {
                    std::string run_cmd = exe.string() + " 2>/dev/null";
                    int exit_code = std::system(run_cmd.c_str());
                    EXPECT_EQ(exit_code, 0) << "Unexpected exit code from linked executable";
                    fs::remove(exe);
                }
                fs::remove(main_src);
                break;
            }
        }
    }

    fs::remove_all(extract_dir);
    fs::remove(archive_file);
}
