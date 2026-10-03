#include <endian.h>
#include <fcntl.h>
#include <gelf.h>
#include <gtest/gtest.h>
#include <libelf.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "i18n/localization.hpp"
#include "lib_utils.hpp"
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
        fs::remove_all(fs::current_path() / "strip_two_members", ec);
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
        std::string cmd =
            "g++ -c -O2 -o " + test_file.string() + " " + src.string() + " 2>/dev/null";
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

    const auto read_all = [](const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    const std::string before = read_all(test_file);

    std::string error_msg;
    bool result = strip_file(test_file, error_msg);
    EXPECT_FALSE(result);
    // **静默是这里的有意语义**（2026-10-03 复核后**保留**，一度改成告警，已撤回）：
    // 没有节区表 = **没什么可剥**，不是失败。被完整 strip 过的二进制就长这样 —— 本仓库
    // 自己的 `make docker` 产物 `build/lpkg-docker` 实测 `readelf -h` 就是
    // `Number of section headers: 0`（UPX 打包的二进制同理）。`strip_binary` 是**尽力而为**
    // 的步骤：只有 `error_msg` 非空才打一条 per-file 告警（且**从不**演成错误、不影响同包
    // 其它文件），空串 = 什么都不说。对"没什么可剥"出声 = 每次构建都刷一行无意义告警。
    EXPECT_TRUE(error_msg.empty()) << "没有节区表是正常形态（本仓库自己的二进制就是），不该出声";
    // **绝不改动剥不了的文件**（这是本条真正的风险点）：`process_elf` 只在 `strip_elf_data`
    // **成功之后**才用 `trunc` 打开原文件写回；失败路径上它只被 O_RDONLY 打开过。逐字节比对钉死。
    EXPECT_EQ(read_all(test_file), before) << "剥不了的文件必须逐字节原样保留";
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

TEST_F(StripTest, MalformedArchiveIsPreservedNotEmptied)
{
    // **回归（2026-10-02）**：ar magic 之后是垃圾 —— libelf 仍认它是 ar（只看 8 字节 magic），
    // libarchive 却读不出任何成员。旧实现
    //   `while (archive_read_next_header(...) == ARCHIVE_OK)`
    // 在**首成员**处就结束循环 → 写出一个 **8 字节空归档** → rename 覆盖掉原文件 → 返回 true
    // （实测端到端复现：`lpkg build` 报成功，`.lpkg` 里的 `.a` 只剩 `!<arch>\n`）。
    // 现在：任何非 OK ⇒ 丢弃临时文件、**原库一字不改**、返回 false。
    const fs::path lib = test_file.string() + "_bad.a";
    const std::string bad = "!<arch>\nGARBAGE_NOT_A_VALID_ARCHIVE_MEMBER____________________";
    {
        std::ofstream f(lib, std::ios::binary);
        f.write(bad.data(), static_cast<std::streamsize>(bad.size()));
    }
    const auto before = fs::file_size(lib);

    std::string error_msg;
    EXPECT_FALSE(strip_file(lib, error_msg))
        << "畸形归档必须判失败（保留原库），而不是产出空归档还报成功";

    ASSERT_TRUE(fs::exists(lib)) << "原库被删掉了";
    EXPECT_EQ(fs::file_size(lib), before) << "原库被截断/改写了";
    {
        std::ifstream f(lib, std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
        EXPECT_EQ(content, bad) << "原库内容被改动了";
    }

    std::error_code ec;
    fs::remove(lib, ec);
    fs::remove(lib.string() + ".tmp", ec);
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
    // 文件**必须真的变小**（.symtab/.strtab/.comment 都有数据，删掉即缩小）。订正 2026-10-03：
    // 这里原为 `EXPECT_LE(..., orig_size)` —— strip 变 no-op（大小相等）也照样绿，等于什么都没
    // 证明；收紧成 `EXPECT_LT` 才真正断言"strip 动了这个文件"。
    EXPECT_LT(fs::file_size(so_file), orig_size);

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
    // 同上（订正 2026-10-03）：`EXPECT_LE` 对"strip 变 no-op"恒真，收紧成 `EXPECT_LT`。
    EXPECT_LT(fs::file_size(exe_file), orig_size);

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

namespace
{
struct ArProbe {
    bool has_index = false;                ///< 归档里有符号索引成员 `/`
    bool has_entries = false;              ///< 索引里至少有一个符号条目
    bool stale = false;                    ///< 索引里有偏移**没有**落在成员头上（= 索引已失效）
    std::vector<uint64_t> member_offsets;  ///< 非索引成员的头偏移（按文件顺序）
    std::vector<std::pair<std::string, uint64_t>> symbols;  ///< 索引条目：符号名 → 它指的偏移
};

/** 解析 ar 归档，检查符号索引（成员 `/`）里的每个偏移是否都落在某个成员头的位置上。
 *
 *  索引里存的是**符号名**不是成员名，所以"偏移指不指得到成员头"是唯一能直接判的。
 *  但那个判据在**多成员**时不够：指到**另一个**成员同样是"指向某个成员头"。所以还要拿
 *  `symbols` 按"哪个成员定义哪个符号"去钉指对了没有 —— 那才是能骗过 stale 的形态。
 *  （GNU ar 格式：8 字节 magic 后是 60 字节一个的成员头；size 在偏移 48、10 字节。） */
ArProbe probe_ar_index(const fs::path& p)
{
    ArProbe r;
    std::ifstream f(p, std::ios::binary);
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
    if (d.size() < 8 || std::memcmp(d.data(), "!<arch>\n", 8) != 0) return r;

    const auto fld = [&](size_t off, size_t len) {
        std::string s(reinterpret_cast<const char*>(d.data()) + off, len);
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    };

    std::vector<uint64_t> starts;
    uint64_t idx_off = 0, idx_size = 0;
    size_t pos = 8;
    while (pos + 60 <= d.size()) {
        if (d[pos + 58] != '`' || d[pos + 59] != '\n') break;
        const std::string name = fld(pos, 16);
        uint64_t size = 0;
        try {
            size = std::stoull(fld(pos + 48, 10));
        } catch (...) {
            break;
        }
        const size_t body = pos + 60;
        if (body + size > d.size()) break;
        if (name == "/") {
            r.has_index = true;
            idx_off = body;
            idx_size = size;
        } else if (name != "//" && name != "__.SYMDEF" && name != "__.SYMDEF SORTED") {
            // 只把**内容成员**算进"成员头位置"：`//` 长名表与 BSD ranlib 表都不是内容成员，
            // 索引条目也绝不该指向它们（指向了就是 stale）。
            starts.push_back(pos);
        }
        pos = body + size + (size & 1);
    }
    if (!r.has_index || idx_size < 4) return r;

    const auto be32 = [&](size_t o) {
        return (static_cast<uint32_t>(d[o]) << 24) | (static_cast<uint32_t>(d[o + 1]) << 16) |
               (static_cast<uint32_t>(d[o + 2]) << 8) | static_cast<uint32_t>(d[o + 3]);
    };
    r.member_offsets = starts;
    const uint32_t n = be32(idx_off);
    r.has_entries = n > 0;
    if (idx_size < 4ull + 4ull * n) {
        r.stale = true;
        return r;
    }
    for (uint32_t i = 0; i < n; ++i) {
        const uint64_t off = be32(idx_off + 4 + 4 * i);
        if (std::find(starts.begin(), starts.end(), off) == starts.end()) r.stale = true;
    }
    // 符号名紧跟偏移数组（NUL 分隔）—— 解析出来好按"哪个成员定义哪个符号"去钉指对了没有
    size_t at = idx_off + 4 + 4ull * n;
    const size_t idx_end = idx_off + idx_size;
    for (uint32_t i = 0; i < n && at < idx_end; ++i) {
        const size_t start = at;
        while (at < idx_end && d[at] != 0) ++at;
        r.symbols.emplace_back(
            std::string(reinterpret_cast<const char*>(d.data()) + start, at - start),
            be32(idx_off + 4 + 4 * i));
        ++at;
    }
    return r;
}
}  // namespace

/**
 * **回归（2026-10-02）**：ar 的符号索引成员 `/` 记录的是一串"符号 → 成员起始偏移"，而
 * strip 会让成员**变短** —— 其后每个成员的偏移都跟着前移，**照抄的索引于是全部失效**。
 * 实测后果不是"少个优化"而是链接**硬失败**：
 * `ld: error adding symbols: no more archived files`（本机 `bison` / `nspr` / `gcc`
 * 三个包的 `.a` 就是这样被写坏的）。
 *
 * ⚠️ **单成员的归档测不出这件事** —— 第一个成员的偏移不会变，所以既有的
 * `ArchiveWithObjectMembers` 一直绿着。这里必须**两个**成员，且都要带 `-g`（这样两个
 * 都会在 strip 后变短，第二个成员的偏移必然前移）。
 */
TEST_F(StripTest, ArchiveWithTwoMembersKeepsIndexConsistent)
{
    // ⚠️ 成员名必须 ≤15 字节：再长就落进 GNU/SVR4 ar 的 `//` 长名表，而 libarchive 的 ar
    // 写入器写不了长名（`archive_strip_scan` 会**有意放弃**整个库）—— 那样根本测不到本缺陷。
    const fs::path dir = fs::current_path() / "strip_two_members";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    const auto compile = [&](const char* base, const char* body) {
        const fs::path obj = dir / (std::string(base) + ".o");
        const fs::path src = dir / (std::string(base) + ".c");
        {
            std::ofstream f(src);
            f << body;
        }
        const std::string cmd =
            "gcc -c -g -o " + obj.string() + " " + src.string() + " 2>/dev/null";
        const int rc = std::system(cmd.c_str());
        ec.clear();
        fs::remove(src, ec);
        return rc == 0 && fs::exists(obj) && fs::file_size(obj) > 0;
    };
    if (!compile("a", "int sym_a(void) { return 1; }\n") ||
        !compile("b", "int sym_b(void) { return 2; }\n")) {
        fs::remove_all(dir, ec);
        GTEST_SKIP() << "gcc not available";
    }

    const fs::path archive_file = dir / "lib.a";
    // 必须 `cd` 进去用**相对**短名调用 ar —— GNU ar 把命令行上给的路径原样记进成员名，
    // 传绝对路径会把成员名变成一长串（于是又落进"长名放弃"那条路）。
    const std::string ar_cmd = "cd " + dir.string() + " && ar rcs lib.a a.o b.o 2>/dev/null";
    if (std::system(ar_cmd.c_str()) != 0 || !fs::exists(archive_file)) {
        fs::remove_all(dir, ec);
        GTEST_SKIP() << "ar not available";
    }
    const auto orig_size = fs::file_size(archive_file);

    // 前置条件：`ar rcs` 给的必须是一份**有效**索引，否则下面的断言没有区分力
    const ArProbe before = probe_ar_index(archive_file);
    ASSERT_TRUE(before.has_index) << "ar rcs 应当写出符号索引";
    ASSERT_TRUE(before.has_entries) << "索引里应当有符号条目";
    ASSERT_FALSE(before.stale) << "前置条件：strip 之前索引必须是有效的";

    std::string error_msg;
    EXPECT_TRUE(strip_file(archive_file, error_msg)) << error_msg;

    // 后置条件（**订正 2026-10-03**）：索引必须**保留**并被重算到成员的新位置。
    // 此前写的是"索引要么被丢掉、要么仍逐个指向成员头"——**丢掉那一半是错的**：那是把 `.a`
    // 降级成非正常形态（链接器只能退化成顺序扫描），而 binutils 在 `--strip-debug` / `-g` /
    // `--strip-unneeded` 档位都**保留并重算**（`binutils/objcopy.c`：只有
    // `strip_symbols == STRIP_ALL` 才 `has_armap = false`）——lpkg 剥的正是"调试信息那一档"，
    // 契约与它一致。实测（binutils 2.47）：同一套判据跑两边产物，索引**逐条一致**。
    const ArProbe after = probe_ar_index(archive_file);
    ASSERT_TRUE(after.has_index) << "重写后符号索引必须仍在（丢索引 = 把 .a 降级成非正常形态）";
    EXPECT_FALSE(after.stale) << "重写后索引里有偏移指不到成员头（索引已失效）";
    EXPECT_LT(fs::file_size(archive_file), orig_size) << "两个带 -g 的成员应当被剥离变小";
    ASSERT_EQ(after.member_offsets.size(), 2u);

    // **精确判据**：sym_a / sym_b 必须各自指到**自己那个成员**（a.o 在前、b.o 在后）。
    // 只判"指向某个成员头"不够 —— 指到另一个成员同样是合法的成员头，但链接器会取错成员。
    // b.o 那条是承重的：它在 strip 后**必然前移**，正是本回归要钉的位置。
    const auto offset_of = [&](const std::string& sym) -> uint64_t {
        for (const auto& [s, off] : after.symbols) {
            if (s == sym) return off;
        }
        return UINT64_MAX;
    };
    EXPECT_EQ(offset_of("sym_a"), after.member_offsets[0]) << "sym_a 应指向 a.o";
    EXPECT_EQ(offset_of("sym_b"), after.member_offsets[1]) << "sym_b 应指向 b.o";

    // **端到端判据：拿真正的 ld 做一次最终链接**（入口点直接指到 use，所以不需要 crt/libc）。
    // ⚠️ **判据先验过有区分力**：`ld -r`（可重定位链接）**允许未定义符号** —— 什么都不喂也
    // rc=0（实测），拿它当闸门等于恒真废话；`ld -r --no-undefined` 同样不设防。只有**最终
    // 链接**会真的进归档里找符号：索引错位时实测 `error adding symbols: malformed archive`
    // ——那正是 `bison` / `nspr` / `gcc` 三个包的现场。
    if (std::system("ld --version >/dev/null 2>&1") == 0) {
        const fs::path use_obj = dir / "use.o";
        {
            std::ofstream f(dir / "use.c");
            f << "int sym_a(void); int sym_b(void);\nint use(void) { return sym_a() + sym_b(); }\n";
        }
        const std::string ccmd = "cd " + dir.string() + " && gcc -c use.c -o use.o 2>/dev/null";
        if (std::system(ccmd.c_str()) == 0 && fs::exists(use_obj)) {
            const std::string lcmd =
                "cd " + dir.string() + " && ld -e use -o link.out use.o lib.a > link.log 2>&1";
            const int lrc = std::system(lcmd.c_str());
            std::ifstream lf(dir / "link.log");
            const std::string llog((std::istreambuf_iterator<char>(lf)),
                                   std::istreambuf_iterator<char>());
            EXPECT_EQ(lrc, 0) << "ld 解不出剥离后索引里的符号: " << llog;
        }
    }

    fs::remove_all(dir, ec);
}

namespace
{
/** 手工构造 GNU ar 归档的一个成员（名字 ≤16 字节；特殊名 `/`、`//`、`__.SYMDEF` 原样写）。 */
struct ArMember {
    std::string name;
    std::string data;
};

/** 把成员拼成一个 ar 归档（8 字节 magic + 逐个 60 字节头 + 内容 + 偶数字节对齐）。 */
std::string build_ar(const std::vector<ArMember>& members)
{
    std::string out = "!<arch>\n";
    for (const auto& m : members) {
        std::string hdr(60, ' ');
        if (m.name.size() > 16) return {};
        std::memcpy(hdr.data(), m.name.data(), m.name.size());
        const std::string sz = std::to_string(m.data.size());
        std::memcpy(hdr.data() + 48, sz.data(), sz.size());
        hdr[58] = '`';
        hdr[59] = '\n';
        out += hdr;
        out += m.data;
        if (m.data.size() & 1) out += '\n';
    }
    return out;
}

/** 每个成员头相对归档起点的偏移（用于手工构造索引里指向成员的偏移值）。 */
std::vector<uint64_t> ar_member_offsets(const std::vector<ArMember>& members)
{
    std::vector<uint64_t> offs;
    uint64_t pos = 8;
    for (const auto& m : members) {
        offs.push_back(pos);
        pos += 60 + m.data.size() + (m.data.size() & 1);
    }
    return offs;
}

/** GNU `/` 符号索引体：`<n:4BE>` + n 个 `<成员偏移:4BE>` + 符号名（NUL 分隔）。 */
std::string make_ar_index(uint64_t first_member_off)
{
    std::string d;
    const auto be32 = [&](uint32_t v) {
        d.push_back(static_cast<char>((v >> 24) & 0xff));
        d.push_back(static_cast<char>((v >> 16) & 0xff));
        d.push_back(static_cast<char>((v >> 8) & 0xff));
        d.push_back(static_cast<char>(v & 0xff));
    };
    be32(1);
    be32(static_cast<uint32_t>(first_member_off));
    d += "sym_a";
    d.push_back('\0');
    return d;
}

void write_file(const fs::path& p, const std::string& data)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

/** 归档里是否存在名为 want 的成员（按 GNU 规则去掉名字字段的尾 `/` 与填充空格）。 */
bool ar_has_member(const fs::path& p, std::string_view want)
{
    std::ifstream f(p, std::ios::binary);
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
    if (d.size() < 8 || std::memcmp(d.data(), "!<arch>\n", 8) != 0) return false;
    size_t pos = 8;
    while (pos + 60 <= d.size()) {
        if (d[pos + 58] != '`' || d[pos + 59] != '\n') break;
        std::string name(reinterpret_cast<const char*>(d.data()) + pos, 16);
        while (!name.empty() && name.back() == ' ') name.pop_back();
        if (!name.empty() && name[0] != '/' && name.back() == '/') name.pop_back();
        if (name == want) return true;
        uint64_t size = 0;
        try {
            size = std::stoull(std::string(reinterpret_cast<const char*>(d.data()) + pos + 48, 10));
        } catch (...) {
            break;
        }
        if (pos + 60 + size > d.size()) break;
        pos += 60 + size + (size & 1);
    }
    return false;
}
}  // namespace

/**
 * **回归（2026-10-03）**：`//` 长名表与归档符号索引必须**同一判据**。
 *
 * 旧判据只认"有 `.o` 成员在 strip 后变短"（`scan == 1`）才处理索引，却漏掉了**另一种会让
 * 偏移改变的情形**：`//` 长名表在重写时恒定被丢弃（libarchive 已把它的 size 归零，照抄只会
 * 写出一张空表 → 读回 "Invalid string table"），其后成员整体前移。于是一个"带 `//`、成员
 * **没有**变短"的归档会被判成"索引仍然有效"而照抄索引 —— 但成员已经移位，偏移全错
 * （子审计逐字节复刻：索引 stale 0 → 2）。真实 GNU `ar` 只在有 >15 字节名字时才产 `//`，
 * 那种库会被"长名放弃"整库跳过；但**外来/构造归档**可以带一张没人引用的 `//`，正是这里手工
 * 构造的形态。
 *
 * 处置是**重算偏移后保留索引**（丢掉会把 `.a` 降级成非正常形态，见
 * `ArchiveWithTwoMembersKeepsIndexConsistent`）。
 */
TEST_F(StripTest, ArchiveWithUnreferencedLongNameTableIsNotLeftWithStaleIndex)
{
    // 成员：[`/` 索引] [（没人引用的）`//` 长名表] [非 ELF 的 a.o —— strip 不了 → 不变短]
    std::vector<ArMember> members = {ArMember{"/", ""}, ArMember{"//", "unused.o/\n"},
                                     ArMember{"a.o", "this is not an ELF object file"}};
    members[0].data = make_ar_index(0);  // 占位：长度与真实索引一致（4+4+6）
    const auto offs = ar_member_offsets(members);
    members[0].data = make_ar_index(offs[2]);  // 索引指向 a.o 的成员头

    const fs::path archive_file = test_file.string() + "_lnt.a";
    write_file(archive_file, build_ar(members));

    const ArProbe before = probe_ar_index(archive_file);
    ASSERT_TRUE(before.has_index) << "前置条件：构造的归档里应有 `/` 索引";
    ASSERT_TRUE(before.has_entries) << "前置条件：索引里应有符号条目";
    ASSERT_FALSE(before.stale) << "前置条件：strip 之前索引必须有效";

    std::string error_msg;
    EXPECT_TRUE(strip_file(archive_file, error_msg)) << error_msg;

    const ArProbe after = probe_ar_index(archive_file);
    ASSERT_TRUE(after.has_index) << "`/` 索引成员必须保留（丢索引 = 把 .a 降级成非正常形态）";
    EXPECT_FALSE(after.stale)
        << "归档里有被丢弃的 `//` 长名表 → 成员已前移；照抄的索引偏移必然全部错位"
           "（ld: error adding symbols: no more archived files）";
    EXPECT_FALSE(ar_has_member(archive_file, "//")) << "`//` 长名表必须被丢弃（size 已被归零）";

    // **精确判据**：索引条目必须指到 a.o **移动之后**的位置。
    ASSERT_EQ(after.member_offsets.size(), 1u);
    ASSERT_EQ(after.symbols.size(), 1u);
    EXPECT_EQ(after.symbols[0].first, "sym_a");
    EXPECT_EQ(after.symbols[0].second, after.member_offsets[0]) << "索引没有指向 a.o 的新位置";
    // 且新位置必须**与重写前不同** —— 否则上面那条断言没有区分力（成员压根没动）
    ASSERT_EQ(before.member_offsets.size(), 1u);
    EXPECT_NE(after.member_offsets[0], before.member_offsets[0])
        << "前置条件：丢掉 `//` 应当让 a.o 前移（否则这条用例测不到重映射）";

    std::error_code ec;
    fs::remove(archive_file, ec);
}

/**
 * **回归（2026-10-03）**：BSD `__.SYMDEF`（ranlib 表）**认不出来就整个库不剥** ——
 * 维护者定的原则是"lpkg 不产出错误产物"。它记录的也是"符号 → 字节偏移"，成员一变短就失效；
 * 而它的数值字段是**宿主字节序**（bfd 自己的注释："Probably we're using the wrong byte
 * ordering"），跨平台没有可靠判据，GNU ar 在 Linux/ELF 上也不产它。
 *
 * 于是**不能**照抄（偏移错位 ⇒ `malformed archive`），也**不该**丢掉（把 `.a` 降级成非正常
 * 形态、且没人能保证后续工具都愿意顺序扫描）——只能原样留着并告警。此前（含我这个会话
 * 早些时候的版本）处置是"丢掉"。
 *
 * GNU `ar` 不产 `__.SYMDEF`，故用构造归档：一个带 `-g`、strip 后会变短的 `.o` 成员把重写
 * 推进"索引失效"分支，再断言整份归档**一个字节都没动**。
 */
TEST_F(StripTest, ArchiveWithBsdSymdefIsLeftUntouchedRatherThanMisindexed)
{
    init_localization();  // 下面要断"报错点名了是哪种索引"，没初始化只会拿到 [MISSING_STRING]

    const fs::path dir = fs::current_path() / "strip_symdef";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path obj = dir / "a.o";
    const fs::path src = dir / "a.c";
    {
        std::ofstream f(src);
        f << "int sym_a(void) { return 1; }\n";
    }
    const std::string cmd = "gcc -c -g -o " + obj.string() + " " + src.string() + " 2>/dev/null";
    const int rc = std::system(cmd.c_str());
    std::error_code e2;
    fs::remove(src, e2);
    if (rc != 0 || !fs::exists(obj)) {
        fs::remove_all(dir, e2);
        GTEST_SKIP() << "gcc 不可用";
    }
    std::ifstream in(obj, std::ios::binary);
    const std::string obj_data((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());

    const fs::path archive_file = dir / "lib.a";
    std::vector<ArMember> members = {ArMember{"a.o", obj_data},
                                     ArMember{"__.SYMDEF", "symdef-payload"}};
    const std::string original = build_ar(members);
    write_file(archive_file, original);

    ASSERT_TRUE(ar_has_member(archive_file, "__.SYMDEF")) << "前置条件：构造的归档含 __.SYMDEF";

    std::string error_msg;
    // 认不出索引 ⇒ **整份归档不剥**（返回失败让调用方 `strip_binary` 打 warning）
    EXPECT_FALSE(strip_file(archive_file, error_msg)) << "带 BSD ranlib 表的库不该被重写";
    EXPECT_NE(error_msg.find("__.SYMDEF"), std::string::npos)
        << "拒绝必须点名是哪种索引（报错要能定位），实际: " << error_msg;

    // 原文件**一个字节都没动**：索引照旧有效、链接照常（宁可少剥一个库）
    std::ifstream rf(archive_file, std::ios::binary);
    const std::string now((std::istreambuf_iterator<char>(rf)), std::istreambuf_iterator<char>());
    EXPECT_EQ(now, original) << "拒绝时不许动原文件";
    EXPECT_TRUE(ar_has_member(archive_file, "__.SYMDEF"));

    fs::remove_all(dir, ec);
}

/**
 * **回归（2026-10-03）**：**畸形的 GNU 索引**（条目指向的不是成员头）同样"不产出错误产物"。
 *
 * 判据是"每一条偏移都必须**正好**落在某个成员头上"，任一条对不上就说明这份索引不是我们能
 * 重算的东西：照抄会把偏移写错 —— 只能整份归档原样留着 + 告警（不产出错误产物）。
 *
 * 构造：`[`/` 索引（条目指向一个根本不存在的位置）]` + `[带 -g、strip 后会变短的 a.o]`。
 */
TEST_F(StripTest, ArchiveWithUnmappableIndexIsLeftUntouchedRatherThanMisindexed)
{
    init_localization();  // 要断"报错点名了索引成员名"（没初始化只会拿到 [MISSING_STRING]）

    const fs::path dir = fs::current_path() / "strip_bad_index";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path obj = dir / "a.o";
    const fs::path src = dir / "a.c";
    {
        std::ofstream f(src);
        f << "int sym_a(void) { return 1; }\n";
    }
    const std::string cmd = "gcc -c -g -o " + obj.string() + " " + src.string() + " 2>/dev/null";
    const int rc = std::system(cmd.c_str());
    std::error_code e2;
    fs::remove(src, e2);
    if (rc != 0 || !fs::exists(obj)) {
        fs::remove_all(dir, e2);
        GTEST_SKIP() << "gcc 不可用";
    }
    std::ifstream in(obj, std::ios::binary);
    const std::string obj_data((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());

    const fs::path archive_file = dir / "lib.a";
    std::vector<ArMember> members = {ArMember{"/", ""}, ArMember{"a.o", obj_data}};
    members[0].data = make_ar_index(0);         // 先按真实长度占位
    members[0].data = make_ar_index(0x00ffff);  // 条目指向一个不存在的位置（文件远没有这么长）
    const std::string original = build_ar(members);
    write_file(archive_file, original);

    std::string error_msg;
    EXPECT_FALSE(strip_file(archive_file, error_msg)) << "索引条目指不到成员头 ⇒ 不该重写";
    EXPECT_NE(error_msg.find('/'), std::string::npos)
        << "拒绝必须点名是哪张索引（报错要能定位），实际: " << error_msg;

    std::ifstream rf(archive_file, std::ios::binary);
    const std::string now((std::istreambuf_iterator<char>(rf)), std::istreambuf_iterator<char>());
    EXPECT_EQ(now, original) << "拒绝时不许动原文件";

    fs::remove_all(dir, ec);
}

/**
 * **回归（2026-10-03）**：`apply_soname_links` 生成的 SONAME 链接必须指向**真实库文件**，且
 * 结果**不依赖目录遍历顺序**（可复现构建）。
 *
 * 旧实现：链接目标取"被遍历条目自己的文件名"，而第一遍用**跟随**语义，所以会处理符号链接
 * 条目 —— 若包发 `liborder.so -> liborder.so.1.2.3` 却不发 `liborder.so.1`，一旦 readdir
 * 先给出 `liborder.so`，就会建出链式目标 `liborder.so.1 -> liborder.so`；先给出实体文件则建出
 * `liborder.so.1 -> liborder.so.1.2.3` —— 同一份内容在不同机器上产出**不同**的链接。修法：
 * 先收集条目、实体文件优先、按文件名定序，链接一律指向实体文件。
 */
TEST_F(StripTest, SonameLinkPointsAtRealLibraryNotAnotherSymlink)
{
    const fs::path dir = fs::current_path() / "soname_order_dir";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    const fs::path src = dir / "liborder.c";
    {
        std::ofstream f(src);
        f << "int order_func(void) { return 7; }\n";
    }
    const fs::path real_lib = dir / "liborder.so.1.2.3";
    const std::string cmd = "gcc -shared -fPIC -Wl,-soname,liborder.so.1 -o " + real_lib.string() +
                            " " + src.string() + " 2>/dev/null";
    const int rc = std::system(cmd.c_str());
    std::error_code e2;
    fs::remove(src, e2);
    if (rc != 0 || !fs::exists(real_lib)) {
        fs::remove_all(dir, e2);
        GTEST_SKIP() << "gcc 不可用";
    }

    // 包自带的链接：liborder.so -> liborder.so.1.2.3（**没有** liborder.so.1）
    fs::create_symlink("liborder.so.1.2.3", dir / "liborder.so");

    apply_soname_links(dir);

    const fs::path link = dir / "liborder.so.1";
    ASSERT_TRUE(fs::is_symlink(link)) << "SONAME 链接 liborder.so.1 没有被建立";
    const fs::path target = fs::read_symlink(link);
    EXPECT_EQ(target.filename().string(), "liborder.so.1.2.3")
        << "链接指向了包自带的另一条符号链接（链式目标），而不是真实库文件；旧实现的结果还随 "
           "readdir 顺序漂移（不可复现构建）";
    EXPECT_FALSE(fs::is_symlink(dir / target.filename())) << "链接目标本身又是个符号链接";

    fs::remove_all(dir, e2);
}

/**
 * **回归（2026-10-02）**：`e_ehsize` 是**输入可控**的字段，而 `write_ehdr` 会无条件写入
 * **整个** `Elf32_Ehdr` / `Elf64_Ehdr`（52 / 64 字节）；唯一那道守卫取自
 * `max(e_phoff + e_phnum*phsize, e_ehsize)` —— 把 `e_ehsize` 改成 4 就能让它通过，于是
 * 往只有几十字节的输出缓冲区里写整个 ELF 头（实测 ASan：`heap-buffer-overflow WRITE of
 * size 2`；非 ASan 下是堆损坏 → SIGABRT，而目标文件已被 `process_elf` 截断成 0 字节）。
 *
 * 修后：**明确拒绝**（返回 false 且点名原因），绝不写。
 *
 * 说明：另一个同类守卫（`e_shnum` 截断）**在这套 libelf 上不可达** —— elfutils 会拒绝
 * `e_shnum >= SHN_LORESERVE` 的输入，而没有 SHN_XINDEX 扩展编号时节区数最多 65535，
 * 恰好装得下 16 位字段。那条守卫留作纵深防御，因此不为它写用例（见 CLAUDE.md §7.4：
 * 断言一条走不到的分支没有意义）。
 */
TEST_F(StripTest, MalformedEhsizeIsRefusedNotOverflowed)
{
    // 用**真实**的可执行文件，只改 `e_ehsize` 一处 —— 这样测的正是那道守卫，不是别的失败
    {
        const fs::path src = test_file.string() + ".c";
        {
            std::ofstream f(src);
            f << "int main(void) { return 0; }\n";
        }
        const std::string cmd =
            "gcc -o " + test_file.string() + " " + src.string() + " 2>/dev/null";
        const int rc = std::system(cmd.c_str());
        std::error_code e;
        fs::remove(src, e);
        if (rc != 0 || !fs::exists(test_file)) GTEST_SKIP() << "gcc not available";
    }

    {
        std::fstream f(test_file, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.is_open());
        unsigned char ident[16] = {};
        f.seekg(0);
        f.read(reinterpret_cast<char*>(ident), sizeof ident);
        ASSERT_EQ(ident[0], 0x7f);
        ASSERT_TRUE(ident[4] == 1 || ident[4] == 2) << "EI_CLASS 只能是 ELF32/ELF64";
        const std::streamoff ehsize_off = (ident[4] == 2) ? 52 : 40;  // ELF64 / ELF32
        const unsigned char bad[2] = {4, 0};                          // e_ehsize = 4（小端）
        f.seekp(ehsize_off);
        f.write(reinterpret_cast<const char*>(bad), 2);
    }

    std::string error_msg;
    EXPECT_FALSE(strip_file(test_file, error_msg)) << "畸形 e_ehsize 必须被拒绝，而不是去写";
    EXPECT_FALSE(error_msg.empty()) << "拒绝必须说得出原因（否则又是一次静默失败）";
    EXPECT_TRUE(fs::exists(test_file)) << "拒绝时不能把原文件弄丢";
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
    std::string ar_cmd =
        "ar rcs " + archive_file.string() + " " + test_file.string() + " 2>/dev/null";
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

    // 找到提取出的目标文件：**按文件名匹配，不按扩展名**。归档成员名就是 `test_file`
    // 的 basename（`ar rcs <archive> <test_file>`），而 `test_file` 没有扩展名
    // （`test_strip_bin`）—— 此前判据是 `extension() == ".o"`（且同一条件写重两遍），
    // 于是这段"strip 后重新链接并运行"的验证**从未执行**、用例恒绿（2026-10-03 订正）。
    EXPECT_EQ(extract_ret, 0) << "ar x failed on the stripped archive";
    const std::string member_name = test_file.filename().string();
    bool member_verified = false;
    if (extract_ret == 0) {
        for (auto& entry : fs::directory_iterator(extract_dir)) {
            const fs::path& extracted = entry.path();
            if (extracted.filename() != member_name) continue;
            member_verified = true;
            // 临时改变 test_file 以使用验证函数
            fs::path orig = test_file;
            test_file = extracted;
            bool group_ok = verify_group_integrity();
            test_file = orig;
            EXPECT_TRUE(group_ok)
                << "Group integrity check failed on extracted object from stripped archive";

            // 链接测试：用提取的目标文件编译并运行
            fs::path main_src = extracted.string() + "_arch_main.c";
            fs::path exe = extracted.string() + "_arch_exe";
            {
                std::ofstream f(main_src);
                f << "int call(int, int);\n"
                     "int main(void) {\n"
                     "    return call(5, 7) - 59;\n"
                     "}\n";
            }
            std::string link_cmd = "g++ -o " + exe.string() + " " + main_src.string() + " " +
                                   extracted.string() + " 2>/dev/null";
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
    EXPECT_TRUE(member_verified)
        << "ar x did not extract the expected member '" << member_name
        << "'; the post-strip verification was skipped (this case used to pass vacuously)";

    fs::remove_all(extract_dir);
    fs::remove(archive_file);
}

// ============================================================================
// 畸形 ELF 的边界/计数校验（历史 TODO.md X1）
//
// strip 处理的是**上游构建产物**（不可信输入）。修复前的三处缺陷都实测过：
//   ① `memcpy(output, input, min(headers_size, input.size()))` 只按输入大小夹紧，
//      e_phoff 很大时向输出缓冲越界写（ASan：128 字节区域写 4320）
//   ② 节区源区间检查用 `off + size <= limit`，uint64 回绕后通过 → 越界 memcpy
//   ③ `sh_size / sh_entsize`，sh_entsize == 0 时 SIGFPE
// 修法：共用 elf_range_within()（饱和比较）与 elf_section_entry_count()，校验不过
// 一律返回 false（调用方告警并保留原文件），绝不部分拷贝出损坏产物。
// ============================================================================

class CraftedElfTest : public StripTest
{
protected:
    /** 手搓最小 ELF64：一个可指定区间/类型的节区 + 一个（未被保留的）附加节区 */
    void write_crafted_elf(uint64_t sh_offset, uint64_t sh_size, uint32_t sh_type,
                           uint64_t sh_flags, uint16_t e_phoff_lo, uint16_t e_phnum,
                           size_t file_size)
    {
        std::vector<uint8_t> buf(file_size, 0);
        Elf64_Ehdr ehdr{};
        std::memcpy(ehdr.e_ident, ELFMAG, SELFMAG);
        ehdr.e_ident[EI_CLASS] = ELFCLASS64;
        ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
        ehdr.e_ident[EI_VERSION] = EV_CURRENT;
        ehdr.e_type = ET_EXEC;
        ehdr.e_machine = EM_X86_64;
        ehdr.e_version = EV_CURRENT;
        ehdr.e_ehsize = sizeof(Elf64_Ehdr);
        ehdr.e_phoff = e_phoff_lo;
        ehdr.e_phnum = e_phnum;
        ehdr.e_shoff = sizeof(Elf64_Ehdr);
        ehdr.e_shnum = 2;
        ehdr.e_shentsize = sizeof(Elf64_Shdr);
        ehdr.e_shstrndx = 0;  // 无节区名表（名字全空，不影响本组断言）
        std::memcpy(buf.data(), &ehdr, sizeof(ehdr));

        Elf64_Shdr sh[2]{};
        sh[0] = Elf64_Shdr{};
        sh[1].sh_type = sh_type;
        sh[1].sh_flags = sh_flags;
        sh[1].sh_offset = sh_offset;
        sh[1].sh_size = sh_size;
        sh[1].sh_entsize = 0;  // ① 与 ② 的场景都需要它（SHT_DYNAMIC 走安全计数）
        std::memcpy(buf.data() + sizeof(Elf64_Ehdr), sh, sizeof(sh));

        std::ofstream f(test_file, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(buf.data()),
                static_cast<std::streamsize>(buf.size()));
    }
};

TEST_F(CraftedElfTest, HugeProgramHeaderOffsetIsRejectedInsteadOfHeapOverflow)
{
    // e_phoff = 0x1000 + 4 个 phdr ⇒ headers_size = 4320。旧实现的输出缓冲只按
    // `e_shoff + e_shnum*shentsize` 算（~176 字节），于是"复制 4320 字节的头/程序头"会越界写。
    // 当年的处置是**拒绝这个输入**，本用例钉的就是那个拒绝。
    //
    // **订正 2026-10-03**：给节区落点补上 `max(…, headers_size)` 这道下界之后（见 strip.cpp 里
    // `layout_base` 的说明 —— 那处同源缺陷由 `tests/fuzz/elf_strip_fuzz.cpp` 实测抓到），输出
    // 缓冲已被**正确放大**到装得下整个头 + 程序头，越界不再可能，于是这个输入变成"安全地接受"
    // （产出 4448 字节：头与 phdr 原样保留、节区表排在 phdr 之后）。
    // 所以本用例改钉**不变量**而不是那个中间机制：要么拒绝（且带原因），要么接受、但产物必须
    // 仍是 libelf 能解析的 ELF。越界本身由 `make test-sanitize`（ASan）那套保证。
    write_crafted_elf(/*sh_offset=*/0x40, /*sh_size=*/16, SHT_PROGBITS, SHF_ALLOC,
                      /*e_phoff_lo=*/0x1000, /*e_phnum=*/4, /*file_size=*/8192);

    std::string error_msg;
    const bool ok = strip_file(test_file, error_msg);

    std::ifstream in(test_file, std::ios::binary);
    ASSERT_TRUE(in.good()) << "strip 之后文件不该消失";
    std::vector<uint8_t> out{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    ASSERT_GE(out.size(), static_cast<size_t>(EI_NIDENT));
    EXPECT_EQ(std::memcmp(out.data(), ELFMAG, SELFMAG), 0) << "产物必须仍是 ELF";

    Elf* elf = elf_memory(reinterpret_cast<char*>(out.data()), out.size());
    ASSERT_NE(elf, nullptr);
    GElf_Ehdr got{};
    EXPECT_NE(gelf_getehdr(elf, &got), nullptr) << "产物必须能被 libelf 解析";
    elf_end(elf);

    if (!ok) {
        EXPECT_FALSE(error_msg.empty()) << "拒绝必须带上原因 —— strip_binary 的告警判据是 "
                                           "`!strip_file(...) && !error_msg.empty()`，"
                                           "空串 = 检测到了也一声不响（静默跳过）";
    }
}

TEST_F(CraftedElfTest, OverflowingSectionRangeIsRejectedInsteadOfOutOfBoundsCopy)
{
    // sh_offset + sh_size 回绕成 0：旧检查 `off + size <= limit` 因此通过
    constexpr uint64_t kOff = 0x1000;
    const uint64_t kSize = ~uint64_t{0} - kOff + 1;  // 2^64 - 0x1000
    write_crafted_elf(kOff, kSize, SHT_PROGBITS, SHF_ALLOC, /*e_phoff_lo=*/0, /*e_phnum=*/0,
                      /*file_size=*/8192);

    std::string error_msg;
    EXPECT_FALSE(strip_file(test_file, error_msg))
        << "回绕的节区区间应被拒绝，而不是做 2^64 级别的 memcpy";
    EXPECT_FALSE(error_msg.empty()) << "拒绝必须带上原因（空串 = 调用方静默跳过）";
}

TEST_F(CraftedElfTest, ZeroEntsizeDynamicSectionDoesNotDivideByZero)
{
    // SHT_DYNAMIC 且 sh_entsize == 0：旧代码 `sh_size / sh_entsize` 直接 SIGFPE
    // （identify_file_type 的 SONAME 扫描 与 get_elf_soname 两处都是）
    write_crafted_elf(/*sh_offset=*/192, /*sh_size=*/16, SHT_DYNAMIC, /*sh_flags=*/0,
                      /*e_phoff_lo=*/0, /*e_phnum=*/0, /*file_size=*/208);

    // 能走到下一行就说明没有除零崩溃
    EXPECT_EQ(get_elf_soname(test_file), "") << "畸形 SHT_DYNAMIC 不应产出 SONAME";

    std::string error_msg;
    EXPECT_NO_THROW(strip_file(test_file, error_msg))
        << "strip 路径（identify_file_type 的 SONAME 扫描）也不得除零崩溃";
}

TEST_F(CraftedElfTest, SectionTableNeverOverwritesTheElfHeader)
{
    // 触发条件：既没有 SHF_ALLOC 数据（max_alloc_end == 0）、也没有非 ALLOC 载荷 ⇒ 重排算出的
    // `e_shoff` 落到 0。而 `kept_sections` **永远含节区 0**（null section，内容全零），
    // `write_shdr(0)` 于是正好写在第 0 字节 —— **把 ELF 头覆盖成全零**，函数却返回 true
    // （`process_elf` 据此把损坏内容写回原文件，`strip_binary` 还当成功、一声不响）。
    // 由 fuzz harness（tests/fuzz/elf_strip_fuzz.cpp）实测抓到。修复前用本用例这条输入跑
    // 独立复现：返回 true + 输出 128 字节全零；修复后：返回 true + 192 字节合法 ELF。
    write_crafted_elf(/*sh_offset=*/0, /*sh_size=*/0, SHT_PROGBITS, SHF_ALLOC,
                      /*e_phoff_lo=*/0, /*e_phnum=*/0, /*file_size=*/192);

    std::string error_msg;
    const bool ok = strip_file(test_file, error_msg);

    // 不变量（与 fuzz oracle 同一条）：无论接受还是拒绝，盘上留下的都必须是**能解析的 ELF**。
    // strip 的语义是"尽力而为；失败就保留原文件"，绝不允许"报成功却写出损坏产物"。
    std::ifstream in(test_file, std::ios::binary);
    ASSERT_TRUE(in.good()) << "strip 之后文件不该消失";
    std::vector<uint8_t> out{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    ASSERT_GE(out.size(), static_cast<size_t>(EI_NIDENT));
    EXPECT_EQ(std::memcmp(out.data(), ELFMAG, SELFMAG), 0)
        << "ELF 魔数被覆盖了（修复前：节区表写在偏移 0，把 ELF 头整块清零）";

    Elf* elf = elf_memory(reinterpret_cast<char*>(out.data()), out.size());
    ASSERT_NE(elf, nullptr);
    GElf_Ehdr got{};
    EXPECT_NE(gelf_getehdr(elf, &got), nullptr) << "strip 的产物必须仍能被 libelf 解析";
    elf_end(elf);

    if (!ok) {
        EXPECT_FALSE(error_msg.empty()) << "拒绝必须带上原因（空串 = 调用方静默跳过）";
    }
}

TEST_F(CraftedElfTest, RelSectionSizeBeyondFileIsRejectedInsteadOfRunawayAllocation)
{
    // 触发条件：**ET_REL** 的某个带数据节区声明了远超文件大小的 `sh_size`。
    // `strip_elf_rel_object` 会把节区头（含 `sh_size`）原样写进输出 ELF，再交给 `elf_update`
    // 去布局 —— libelf 会**按这些尺寸创建/分配空间**。由 fuzz harness 抓到、再最小复现：
    // **1224 字节**的输入声明了 3 个节区共约 480 GB，让 shmem 一路涨到容器的 cgroup 上限
    // （实测容器被自己的 30 GiB 限制杀掉；在还没给容器设限时它会直接打死宿主桌面）。
    //
    // 本用例用 **1 GiB** 而不是 192 GB：判据是同一条（"带数据的节区必须整段落在文件内"），
    // 而**万一将来这道判据被改坏**，1 GiB 只会让测试失败，不会把跑测试的机器打爆。
    std::vector<uint8_t> buf(256, 0);
    Elf64_Ehdr ehdr{};
    std::memcpy(ehdr.e_ident, ELFMAG, SELFMAG);
    ehdr.e_ident[EI_CLASS] = ELFCLASS64;
    ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    ehdr.e_type = ET_REL;  // ← 关键：走 strip_elf_rel_object（修复前正是这条路没有量级封顶）
    ehdr.e_machine = EM_X86_64;
    ehdr.e_version = EV_CURRENT;
    ehdr.e_ehsize = sizeof(Elf64_Ehdr);
    ehdr.e_shoff = sizeof(Elf64_Ehdr);
    ehdr.e_shnum = 2;
    ehdr.e_shentsize = sizeof(Elf64_Shdr);
    ehdr.e_shstrndx = 0;
    std::memcpy(buf.data(), &ehdr, sizeof(ehdr));

    Elf64_Shdr sh[2]{};
    sh[1].sh_type = SHT_PROGBITS;
    sh[1].sh_offset = 128;
    sh[1].sh_size = 1ull << 30;  // 1 GiB，而整个文件只有 256 字节
    std::memcpy(buf.data() + sizeof(Elf64_Ehdr), sh, sizeof(sh));
    {
        std::ofstream f(test_file, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(buf.data()),
                static_cast<std::streamsize>(buf.size()));
    }

    std::string error_msg;
    EXPECT_FALSE(strip_file(test_file, error_msg))
        << "节区数据超出文件范围的 ET_REL 必须被拒绝，而不是让 elf_update 去申请上 GiB";
    EXPECT_FALSE(error_msg.empty()) << "拒绝必须带上原因（空串 = 调用方静默跳过）";
}

TEST_F(CraftedElfTest, RelSectionWithHugeAddralignIsRejectedInsteadOfRunawayAllocation)
{
    // 触发条件：ET_REL 的某个节区声明了**天文数字的 `sh_addralign`**。
    // `strip_elf_rel_object` 把它原样写进输出 ELF，而 `elf_update` **按它对齐节区落点** ——
    // 输出偏移随之间断到该对齐值，memfd 被 `ftruncate` 到那么大并**真实占掉几十 GB**。
    // 由 fuzz harness 抓到、再用"只改这一个字段"的最小复现钉死：取一个正常的 gcc `.o`，
    // **只**把某节区的 `sh_addralign` 改成 2^56，宿主内存 7 秒掉 48 GB（进程自身 RSS 仅 0.4 GB）。
    // 本用例用 **2^32（4 GiB）** 而不是 2^56：判据相同，而万一判据将来被改坏，测试只会**失败**，
    // 不会把跑测试的机器打爆。
    std::vector<uint8_t> buf(256, 0);
    Elf64_Ehdr ehdr{};
    std::memcpy(ehdr.e_ident, ELFMAG, SELFMAG);
    ehdr.e_ident[EI_CLASS] = ELFCLASS64;
    ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    ehdr.e_type = ET_REL;
    ehdr.e_machine = EM_X86_64;
    ehdr.e_version = EV_CURRENT;
    ehdr.e_ehsize = sizeof(Elf64_Ehdr);
    ehdr.e_shoff = sizeof(Elf64_Ehdr);
    ehdr.e_shnum = 2;
    ehdr.e_shentsize = sizeof(Elf64_Shdr);
    ehdr.e_shstrndx = 0;
    std::memcpy(buf.data(), &ehdr, sizeof(ehdr));

    Elf64_Shdr sh[2]{};
    sh[1].sh_type = SHT_PROGBITS;
    sh[1].sh_offset = 128;
    sh[1].sh_size = 16;               // 数据本身在文件内（这是关键：只把**对齐**写大）
    sh[1].sh_addralign = 1ull << 32;  // ← 4 GiB 对齐，而整个文件只有 256 字节
    std::memcpy(buf.data() + sizeof(Elf64_Ehdr), sh, sizeof(sh));
    {
        std::ofstream f(test_file, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(buf.data()),
                static_cast<std::streamsize>(buf.size()));
    }

    std::string error_msg;
    EXPECT_FALSE(strip_file(test_file, error_msg))
        << "sh_addralign 远大于整个文件的 ET_REL 必须被拒绝，而不是让 elf_update 去铺一个 4 GiB+ "
           "的文件";
    EXPECT_FALSE(error_msg.empty()) << "拒绝必须带上原因（空串 = 调用方静默跳过）";
}

TEST_F(CraftedElfTest, RealSharedLibraryStillStripsAndKeepsSoname)
{
    // 正向对照：正常产物必须仍然能被 strip 且 SONAME 可读（防止校验过严把好文件拒了）
    const fs::path src = test_file.string() + ".c";
    {
        std::ofstream f(src);
        f << "int crafted_test_func(int x) { return x + 1; }\n";
    }
    const std::string cmd = "gcc -shared -fPIC -Wl,-soname,libtest.so.1 -o " + test_file.string() +
                            " " + src.string() + " 2>/dev/null";
    const int ret = std::system(cmd.c_str());
    fs::remove(src);
    if (ret != 0 || !fs::exists(test_file)) GTEST_SKIP() << "gcc 不可用";

    EXPECT_EQ(get_elf_soname(test_file), "libtest.so.1");
    std::string error_msg;
    EXPECT_TRUE(strip_file(test_file, error_msg)) << error_msg;
    EXPECT_EQ(get_elf_soname(test_file), "libtest.so.1") << "strip 后 SONAME 丢失";
}

TEST_F(StripTest, StripObjectWithBssSection)
{
    // **回归（我引入的）**：`.bss` 是 SHT_NOBITS —— libelf 给它的 Elf_Data.d_buf 为 NULL
    // 而 d_size > 0。曾经为"不就地改写调用方输入"而按 d_size 复制缓冲，从 NULL 构造
    // vector 是 UB（bad_alloc/崩溃）→ 整个 strip 失败，且**在此之前所有 strip 测试都是
    // 无 .bss 的 .o，全绿也照样漏掉**（LLVM 的 .o 带 .bss，实测把 llvm 卡在 package 阶段）。
    const fs::path src = test_file.string() + ".c";
    {
        std::ofstream f(src);
        f << "int uninit_global[4096];\nint foo(void){ return uninit_global[0]; }\n";
    }
    const std::string cmd = "gcc -c -o " + test_file.string() + " " + src.string() + " 2>/dev/null";
    const int ret = std::system(cmd.c_str());
    fs::remove(src);
    if (ret != 0 || !fs::exists(test_file)) GTEST_SKIP() << "gcc not available";

    std::string error_msg;
    EXPECT_TRUE(strip_file(test_file, error_msg)) << "带 .bss 的 .o 必须能 strip: " << error_msg;
    EXPECT_TRUE(fs::exists(test_file));
    EXPECT_GT(fs::file_size(test_file), 0);
}

TEST_F(CraftedElfTest, HugeNonAllocSectionSizeIsRejectedInsteadOfHeapOverflow)
{
    // 保留一个**非 SHF_ALLOC** 的节区（靠名字 `.shstrtab` 骗过 keep 判定）、类型 SHT_NOBITS、
    // `sh_size` 巨大：重排时 `current_offset += sh_size` **回绕** → `e_shoff` 变成 2^64-128 →
    // `output_data.resize()` 只分配到 64 字节，而写节区表按 `data() + e_shoff + i*shentsize`
    // 定位 → 指针回绕到缓冲区**之前**再写（NOBITS 节区的复制在下面被跳过，所以那道
    // elf_range_within 守卫不覆盖它）。修复前用 ASan 实测：`heap-buffer-overflow WRITE`。
    // 布局：[0] null、[1] 真名字表（SHT_STRTAB）、[2] 恶意节区（名字指向 ".shstrtab"）。
    constexpr uint64_t kNamesOff = 0x40;
    const char kNames[] = "\0.shstrtab";  // 索引 1 = ".shstrtab"
    // 大小**含**结尾 NUL：名字要在节区内有终止符，否则 libelf 的 elf_strptr 返回 NULL，
    // 那一节的名字就成了空串 → 被 keep 判定丢掉，构造不出场景（实测踩过）。
    constexpr uint64_t kNamesLen = sizeof(kNames);
    constexpr uint64_t kShdrOff = 0x80;
    const uint64_t kHuge = ~uint64_t{0} - 154 + 1;  // 2^64 - 154

    std::vector<uint8_t> buf(1024, 0);
    Elf64_Ehdr ehdr{};
    std::memcpy(ehdr.e_ident, ELFMAG, SELFMAG);
    ehdr.e_ident[EI_CLASS] = ELFCLASS64;
    ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    ehdr.e_type = ET_EXEC;
    ehdr.e_machine = EM_X86_64;
    ehdr.e_version = EV_CURRENT;
    ehdr.e_ehsize = sizeof(Elf64_Ehdr);
    ehdr.e_shoff = kShdrOff;
    ehdr.e_shnum = 3;
    ehdr.e_shentsize = sizeof(Elf64_Shdr);
    ehdr.e_shstrndx = 1;
    std::memcpy(buf.data(), &ehdr, sizeof(ehdr));
    std::memcpy(buf.data() + kNamesOff, kNames, kNamesLen);

    Elf64_Shdr sh[3]{};
    sh[1].sh_type = SHT_STRTAB;
    sh[1].sh_offset = kNamesOff;
    sh[1].sh_size = kNamesLen;
    sh[1].sh_addralign = 1;
    sh[2].sh_name = 1;  // ".shstrtab" —— keep 判定放行非 ALLOC 节区
    sh[2].sh_type = SHT_NOBITS;
    sh[2].sh_flags = 0;  // 非 SHF_ALLOC —— 走"重排"分支
    sh[2].sh_size = kHuge;
    std::memcpy(buf.data() + kShdrOff, sh, sizeof(sh));

    std::ofstream f(test_file, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    f.close();

    std::string error_msg;
    EXPECT_FALSE(strip_file(test_file, error_msg))
        << "回绕的节区尺寸应被拒绝，而不是在写节区表时越界写堆";
}

TEST_F(CraftedElfTest, LargeNonAllocSectionSizeIsRejectedNotAllocated)
{
    // 与上一条同构，但尺寸**不回绕**（1<<26 = 64 MiB）：`current_offset += sh_size` 得到一个
    // 不回绕但巨大的 `e_shoff`，旧的守卫（只防回绕）放行 → `output_data.resize()` 按输入
    // 可控的值分配 64 MiB 并**返回 true**（写出一个 64 MiB 的产物）。正确行为是**拒绝**：
    // strip 只会删节区，产物不可能大于输入。量级取 1<<26 而非天文数字，是为了让"修复前"那次
    // 运行真的分配一点、但不至于把 CI 拖垮。
    constexpr uint64_t kNamesOff = 0x40;
    const char kNames[] = "\0.shstrtab";
    constexpr uint64_t kNamesLen = sizeof(kNames);
    constexpr uint64_t kShdrOff = 0x80;
    constexpr uint64_t kBig = 1ULL << 26;  // 64 MiB：不回绕

    std::vector<uint8_t> buf(1024, 0);
    Elf64_Ehdr ehdr{};
    std::memcpy(ehdr.e_ident, ELFMAG, SELFMAG);
    ehdr.e_ident[EI_CLASS] = ELFCLASS64;
    ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    ehdr.e_type = ET_EXEC;
    ehdr.e_machine = EM_X86_64;
    ehdr.e_version = EV_CURRENT;
    ehdr.e_ehsize = sizeof(Elf64_Ehdr);
    ehdr.e_shoff = kShdrOff;
    ehdr.e_shnum = 3;
    ehdr.e_shentsize = sizeof(Elf64_Shdr);
    ehdr.e_shstrndx = 1;
    std::memcpy(buf.data(), &ehdr, sizeof(ehdr));
    std::memcpy(buf.data() + kNamesOff, kNames, kNamesLen);

    Elf64_Shdr sh[3]{};
    sh[1].sh_type = SHT_STRTAB;
    sh[1].sh_offset = kNamesOff;
    sh[1].sh_size = kNamesLen;
    sh[1].sh_addralign = 1;
    sh[2].sh_name = 1;  // ".shstrtab" —— keep 判定放行非 ALLOC 节区
    sh[2].sh_type = SHT_NOBITS;
    sh[2].sh_flags = 0;  // 非 SHF_ALLOC —— 走"重排"分支
    sh[2].sh_size = kBig;
    std::memcpy(buf.data() + kShdrOff, sh, sizeof(sh));

    std::ofstream f(test_file, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    f.close();

    std::string error_msg;
    EXPECT_FALSE(strip_file(test_file, error_msg))
        << "不回绕但过大的节区尺寸应被拒绝，而不是按输入可控值分配输出缓冲";
}

TEST_F(CraftedElfTest, ForeignEndianElfIsRefusedNotCorrupted)
{
    // strip 的写出路径按**本机字节序**改写 ELF 头/节区表、而节区数据是原样 memcpy —— 大端 ELF
    // 会被写成混合端序（静默损坏）。修法：拒绝 strip 非本机端序（调用方降级为跳过 strip）。
    // 这条造一个**结构完全合法**的大端 ELF64（所有字段按 BE 编码、libelf 会替我们翻译成
    // 主机序）—— 否则别的校验会先把它拒掉，用例就证明不了"拒绝的原因是端序"。
    constexpr uint64_t kNamesOff = 0x40;
    const char kNames[] = "\0.shstrtab";
    constexpr uint64_t kNamesLen = sizeof(kNames);
    constexpr uint64_t kShdrOff = 0x80;

    std::vector<uint8_t> buf(1024, 0);
    const auto put16 = [&](size_t o, uint16_t v) {
        const uint16_t b = htobe16(v);
        std::memcpy(buf.data() + o, &b, 2);
    };
    const auto put32 = [&](size_t o, uint32_t v) {
        const uint32_t b = htobe32(v);
        std::memcpy(buf.data() + o, &b, 4);
    };
    const auto put64 = [&](size_t o, uint64_t v) {
        const uint64_t b = htobe64(v);
        std::memcpy(buf.data() + o, &b, 8);
    };

    buf[EI_MAG0] = 0x7f;
    buf[EI_MAG1] = 'E';
    buf[EI_MAG2] = 'L';
    buf[EI_MAG3] = 'F';
    buf[EI_CLASS] = ELFCLASS64;
    buf[EI_DATA] = ELFDATA2MSB;  // 大端
    buf[EI_VERSION] = EV_CURRENT;
    put16(16, ET_EXEC);     // e_type
    put16(18, EM_X86_64);   // e_machine
    put32(20, EV_CURRENT);  // e_version
    put64(24, 0);           // e_entry
    put64(32, 0);           // e_phoff
    put64(40, kShdrOff);    // e_shoff
    put32(48, 0);           // e_flags
    put16(52, 64);          // e_ehsize
    put16(54, 56);          // e_phentsize
    put16(56, 0);           // e_phnum
    put16(58, 64);          // e_shentsize
    put16(60, 3);           // e_shnum
    put16(62, 1);           // e_shstrndx
    std::memcpy(buf.data() + kNamesOff, kNames, kNamesLen);

    const auto put_shdr = [&](size_t idx, uint32_t name, uint32_t type, uint64_t flags,
                              uint64_t offset, uint64_t size, uint64_t align) {
        const size_t base = kShdrOff + idx * 64;
        put32(base + 0, name);
        put32(base + 4, type);
        put64(base + 8, flags);
        put64(base + 16, 0);  // sh_addr
        put64(base + 24, offset);
        put64(base + 32, size);
        put32(base + 40, 0);  // sh_link
        put32(base + 44, 0);  // sh_info
        put64(base + 48, align);
        put64(base + 56, 0);  // sh_entsize
    };
    put_shdr(0, 0, SHT_NULL, 0, 0, 0, 0);
    put_shdr(1, 0, SHT_STRTAB, 0, kNamesOff, kNamesLen, 1);
    // 尺寸取与 LE 兄弟（LargeNonAlloc…）相同的 1<<26：小尺寸会让输出的范围校验先撞上
    // （那是另一条与端序无关的路径，见下），那样用例就证明不了"拒绝的原因是端序"。
    put_shdr(2, 1, SHT_NOBITS, 0, 0, 1ULL << 26, 1);

    std::ofstream f(test_file, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    f.close();

    std::string error_msg;
    EXPECT_FALSE(strip_file(test_file, error_msg))
        << "非本机端序的 ELF 必须被拒绝，而不是被写成混合端序";
    EXPECT_FALSE(error_msg.empty()) << "拒绝时应当给出原因";
}

// ============================================================================
// “输入可控的尺寸进分配但没有上界”（2026-10-03 修）—— 两条同族缺陷
//
//   A. ar 成员**自报**尺寸（`archive_entry_size`）无上界：它来自归档头、是不可信输入
//      （ar 的 size 字段是 10 位十进制，上限约 9.3 GiB）。libarchive 对“成员头合法、成员体被
//      截断”的归档**第一次 `archive_read_next_header` 就返回 OK**，于是旧实现
//      `std::vector<uint8_t> data(size)` 会按那个自报值分配并逐字节清零 —— 一个几十字节的
//      文件就能触发上 GiB 分配。上界取**结构性**的那一个：成员数据不可能大于整个归档文件。
//   B. ET_REL 产物/中间量无**聚合**上界：逐节区守卫只判“单个节区落在文件内”，挡不住多个
//      节区指向输入里同一片数据、各自声明 ~input_size（`owned_data` 与 `elf_update` 各拷贝
//      一份 ⇒ 峰值 ~input²/128）。判据与 ET_EXEC/ET_DYN 那条同源：strip 只删节区 ⇒ 产物
//      不可能大于输入。
// ============================================================================

TEST_F(StripTest, ArMemberDeclaredSizeBeyondArchiveIsRefusedBeforeAllocation)
{
    // **回归（2026-10-03）**：ar 头里的 size 是**归档自报**的不可信值。旧实现用
    // `const size_t size = archive_entry_size(entry); std::vector<uint8_t> data(size);`
    // 直接按它分配 —— 而 libarchive 对“成员头合法、成员体被截断”的归档第一次
    // `archive_read_next_header` 就返回 OK，于是这个 **~70 字节**的文件会触发 ~9.3 GiB 分配
    // （实测症状：OOM / `std::bad_alloc`）。修复后必须在**分配之前**用结构性上界拒掉。
    //
    // ⚠️ `9999999999` 是 ar 的 size 字段能放下的最大值（10 位十进制）。修复前的运行会真的去
    // 分配它 —— 那正是本条要钉的缺陷；修复后判据在分配之前命中，不会真的分配。
    std::string ar = "!<arch>\n";
    std::string hdr(60, ' ');
    std::memcpy(hdr.data(), "a.o", 3);
    std::memcpy(hdr.data() + 48, "9999999999", 10);  // size 字段：声称 ~9.3 GiB
    hdr[58] = '`';
    hdr[59] = '\n';
    ar += hdr;
    ar += "short";  // 成员体故意远小于自报尺寸（归档被截断）

    const fs::path archive_file = test_file.string() + "_liar.a";
    write_file(archive_file, ar);
    ASSERT_LT(fs::file_size(archive_file), 100u) << "前置条件：归档必须很小";

    std::string error_msg;
    bool result = true;
    EXPECT_NO_THROW({ result = strip_file(archive_file, error_msg); })
        << "畸形尺寸必须在**分配之前**判掉，而不是先按它分配（修前会 bad_alloc / OOM）";
    EXPECT_FALSE(result) << "自报尺寸大于整个归档 ⇒ 必须判失败";
    EXPECT_FALSE(error_msg.empty()) << "拒绝必须带上原因（空串 = 调用方静默跳过）";

    // 拒绝时不动原文件（与相邻早退一致）
    std::ifstream rf(archive_file, std::ios::binary);
    const std::string now((std::istreambuf_iterator<char>(rf)), std::istreambuf_iterator<char>());
    EXPECT_EQ(now, ar) << "拒绝时不许改写原归档";

    std::error_code ec;
    fs::remove(archive_file, ec);
    fs::remove(archive_file.string() + ".tmp", ec);
}

namespace
{
/**
 * 写一个最小 ET_REL：一个 `.shstrtab` + 一组由 `(sh_offset, sh_size)` 给定的 SHT_PROGBITS
 * 节区（外加 null 节区）。带**真实**的 `.shstrtab` 是为了让 `e_shstrndx` 有有效值 ——
 * 与真实 gcc `.o` 同形，避免走到 libelf 对"名表索引指向非 strtab"的行为上去。
 */
void write_crafted_rel(const fs::path& p, const std::vector<std::pair<uint64_t, uint64_t>>& secs,
                       size_t file_size)
{
    constexpr uint64_t kNamesOff = 64;
    const char kNames[] = "\0.shstrtab";  // [0]=NUL（空名的 offset 0），[1]=".shstrtab"
    constexpr uint64_t kNamesLen = sizeof(kNames);
    constexpr uint64_t kShdrOff = 128;

    std::vector<uint8_t> buf(file_size, 0);
    Elf64_Ehdr ehdr{};
    std::memcpy(ehdr.e_ident, ELFMAG, SELFMAG);
    ehdr.e_ident[EI_CLASS] = ELFCLASS64;
    ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    ehdr.e_type = ET_REL;
    ehdr.e_machine = EM_X86_64;
    ehdr.e_version = EV_CURRENT;
    ehdr.e_ehsize = sizeof(Elf64_Ehdr);
    ehdr.e_shoff = kShdrOff;
    ehdr.e_shnum = static_cast<uint16_t>(secs.size() + 2);  // null + .shstrtab + secs
    ehdr.e_shentsize = sizeof(Elf64_Shdr);
    ehdr.e_shstrndx = 1;  // 指向 .shstrtab
    std::memcpy(buf.data(), &ehdr, sizeof(ehdr));
    std::memcpy(buf.data() + kNamesOff, kNames, kNamesLen);

    std::vector<Elf64_Shdr> sh(secs.size() + 2);  // [0] = null 节区
    sh[1].sh_name = 1;                            // ".shstrtab"
    sh[1].sh_type = SHT_STRTAB;
    sh[1].sh_offset = kNamesOff;
    sh[1].sh_size = kNamesLen;
    sh[1].sh_addralign = 1;
    for (size_t i = 0; i < secs.size(); ++i) {
        sh[i + 2].sh_type = SHT_PROGBITS;
        sh[i + 2].sh_offset = secs[i].first;
        sh[i + 2].sh_size = secs[i].second;
    }
    std::memcpy(buf.data() + kShdrOff, sh.data(), sh.size() * sizeof(Elf64_Shdr));

    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
}
}  // namespace

TEST_F(CraftedElfTest, RelSectionsWithinFileStillStrip)
{
    // **正向对照**：两个各 128 字节、数据都落在文件内的节区，尺寸之和 256 ≤ 512 ⇒ 必须照常
    // strip。它证明下面那条聚合判据只挡“超过输入大小”的形态，不会把正常 ELF 拒掉；同时它也给
    // “本套 crafted-ET_REL 夹具确实能走到 elf_update 并成功”留一条可观察证据。
    write_crafted_rel(test_file, {{0, 128}, {256, 128}}, 512);

    std::string error_msg;
    EXPECT_TRUE(strip_file(test_file, error_msg)) << "正常形态必须照常 strip: " << error_msg;
    EXPECT_TRUE(fs::exists(test_file));
    EXPECT_GT(fs::file_size(test_file), 0);
}

TEST_F(CraftedElfTest, RelOverlappingSectionsAggregateBeyondFileIsRejected)
{
    // **回归（2026-10-03）**：ET_REL 的逐节区守卫只判“每个节区落在文件内”，挡不住**多个节区
    // 指向输入里同一片数据、各自声明 ~input_size** 的形态：`strip_elf_rel_object` 会为每个这样
    // 的节区复制一份自有缓冲（`owned_data`），`elf_update` 再各布局一份 ⇒ 峰值 ~input²/128
    // （实测：223 KB 输入 → 167 MB 输出；节区数越多放大越狠）。
    // 判据 = “被保留、带数据的节区尺寸之和 ≤ 输入大小”（与 ET_EXEC/ET_DYN 那条**同源**：
    // strip 只删节区 ⇒ 产物不可能大于输入）。本用例用 ~512 字节的输入、2 个各 512 字节的节区
    // （都指向偏移 0，逐节区守卫刚好全过）——量级刻意取小：判据若被改坏，测试只会**失败**，
    // 不会把跑测试的机器打爆（修前这里产出约 1 KiB，远不是天文数字）。
    write_crafted_rel(test_file, {{0, 512}, {0, 512}}, 512);

    std::string error_msg;
    EXPECT_FALSE(strip_file(test_file, error_msg))
        << "两个各占满整个文件（且指向同一片）的节区，尺寸之和 1024 > 512 ⇒ 必须拒绝";
    EXPECT_FALSE(error_msg.empty()) << "拒绝必须带上原因（空串 = 调用方静默跳过）";
    EXPECT_EQ(fs::file_size(test_file), 512u) << "拒绝时不许改写原文件";
}
