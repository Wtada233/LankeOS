/**
 * test_elf_soname_null_check.cpp — 畸形 DT_SONAME 不得让 get_elf_soname() 段错误
 *
 * 缺陷：`soname = elf_strptr(elf, shdr.sh_link, dyn.d_un.d_val);`
 *
 * libelf 在**三种**情况下对 elf_strptr 返回 NULL：offset 越出 .dynstr、sh_link 指向的不是
 * SHT_STRTAB、目标节区不存在（efllutils 的 libelf_strptr.c：先查 sh_type != SHT_STRTAB →
 * ELF_E_NOT_A_STRING，再查 offset >= data->d_size → ELF_E_INVALID_OFFSET，两条都 return NULL）。
 * 把 NULL 直接赋给 std::string 是 UB：libstdc++ 下等价 strlen(nullptr) → SIGSEGV（实测 gcc -O2
 * 编译的旧代码在这两个用例上 exit=139）。
 *
 * 危害面：get_elf_soname 对 `usr/lib` 下**每个** ELF 调用（安装期的 ldconfig 触发器
 * apply_soname_links 以 root 跑、构建期 make_depend 也调），输入来自**不可信包** —— 一个畸形
 * .so 就能让打包/安装进程在事务中途段错误。修复后与 strip.cpp 对同一 API 的判空（
 * `name_ptr ? name_ptr : ""`）对齐。
 *
 * 本文件钉住的不变量：
 *   ① 手搓 ELF 的节区遍历/gelf_getdyn/elf_strptr 这条**取值路径确实走通**（正向对照）——
 *      否则"返回空串"的三条断言会因为"整只 ELF 压根没被解析"而恒真（假绿）；
 *   ② d_val 越出 .dynstr → 返回空串，不崩；
 *   ③ sh_link 指向非 SHT_STRTAB（这里用 SHT_NULL 的 0 号节区）→ 返回空串，不崩；
 *   ④ gcc 造的**真实** .so 只改一个 d_val 字段（最贴近事故形态）→ 同上。
 *
 * 手搓格式的手法（3 个节区 + e_shstrndx = 0）与 tests/unit/test_elf_stripping.cpp 的
 * CraftedElfTest 一致；本文件 ①②③ 不依赖 gcc，④ 在 gcc 不可用时 GTEST_SKIP。
 */

#include <fcntl.h>
#include <gelf.h>
#include <gtest/gtest.h>
#include <libelf.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "lib_utils.hpp"

namespace fs = std::filesystem;

class ElfSonameNullCheckTest : public ::testing::Test
{
protected:
    fs::path test_dir;
    fs::path test_file;

    // 手搓 ELF 的固定布局（全部落在文件内，libelf 才肯给 elf_getdata 数据）
    static constexpr uint64_t kDynOff = 256;  // .dynamic 数据
    static constexpr uint64_t kStrOff = 288;  // .dynstr 数据
    // "\0libcrafted.so.1"：下标 0 是空串，下标 1 起才是名字（正向对照取 d_val = 1）
    static constexpr char kDynstr[] = "\0libcrafted.so.1";

    void SetUp() override
    {
        if (elf_version(EV_CURRENT) == EV_NONE) FAIL() << "libelf version mismatch";
        test_dir = fs::absolute("tmp_soname_null_check");
        if (fs::exists(test_dir)) fs::remove_all(test_dir);
        fs::create_directories(test_dir);
        test_file = test_dir / "crafted.so";
    }

    void TearDown() override
    {
        // SetUp 里 FAIL()（libelf 版本不符）会让 test_dir 保持空 → 不给 remove_all 空路径
        if (!test_dir.empty()) fs::remove_all(test_dir);
    }

    /**
     * 手搓最小 ELF64：null / .dynamic / .dynstr 三个节区，DT_SONAME 的取值完全由参数决定。
     *
     *   d_val    = DT_SONAME 的 d_un.d_val（写进唯一的 Elf64_Dyn）
     *   sh_link  = SHT_DYNAMIC 的 sh_link（指向哪个节区当字符串表）
     *   str_size = .dynstr 的 sh_size（改成 1 就能让"看起来合法"的 d_val 越界）
     */
    void write_crafted_elf(uint64_t d_val, uint32_t sh_link, uint64_t str_size)
    {
        std::vector<uint8_t> buf(512, 0);

        Elf64_Ehdr ehdr{};
        std::memcpy(ehdr.e_ident, ELFMAG, SELFMAG);
        ehdr.e_ident[EI_CLASS] = ELFCLASS64;
        ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
        ehdr.e_ident[EI_VERSION] = EV_CURRENT;
        ehdr.e_type = ET_DYN;
        ehdr.e_machine = EM_X86_64;
        ehdr.e_version = EV_CURRENT;
        ehdr.e_ehsize = sizeof(Elf64_Ehdr);
        ehdr.e_shoff = sizeof(Elf64_Ehdr);
        ehdr.e_shnum = 3;
        ehdr.e_shentsize = sizeof(Elf64_Shdr);
        ehdr.e_shstrndx = 0;  // 无节区名表（名字全空，不影响本组断言）
        std::memcpy(buf.data(), &ehdr, sizeof(ehdr));

        Elf64_Shdr sh[3]{};
        sh[1].sh_type = SHT_DYNAMIC;
        sh[1].sh_offset = kDynOff;
        sh[1].sh_size = sizeof(Elf64_Dyn);
        sh[1].sh_link = sh_link;
        sh[1].sh_entsize = sizeof(Elf64_Dyn);
        sh[1].sh_addralign = 8;
        sh[2].sh_type = SHT_STRTAB;
        sh[2].sh_offset = kStrOff;
        sh[2].sh_size = str_size;
        sh[2].sh_addralign = 1;
        std::memcpy(buf.data() + sizeof(Elf64_Ehdr), sh, sizeof(sh));

        Elf64_Dyn dyn{};
        dyn.d_tag = DT_SONAME;
        dyn.d_un.d_val = d_val;
        std::memcpy(buf.data() + kDynOff, &dyn, sizeof(dyn));
        std::memcpy(buf.data() + kStrOff, kDynstr, sizeof(kDynstr));

        std::ofstream f(test_file, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(buf.data()),
                static_cast<std::streamsize>(buf.size()));
    }

    /** gcc 造一个带真实 DT_SONAME 的共享库；gcc 不可用返回 false */
    bool build_real_library()
    {
        const fs::path src = test_dir / "probe.c";
        {
            std::ofstream f(src);
            f << "int soname_probe(void) { return 7; }\n";
        }
        const std::string cmd = "gcc -shared -fPIC -Wl,-soname,libcrafted.so.1 -o " +
                                test_file.string() + " " + src.string() + " 2>/dev/null";
        const int ret = std::system(cmd.c_str());
        fs::remove(src);
        return ret == 0 && fs::exists(test_file) && fs::file_size(test_file) > 0;
    }

    /**
     * 用 libelf 只**定位**（不改）真实 .so 里两个字段的文件偏移：
     *   d_val_off    = DT_SONAME 条目的 d_un.d_val 字段
     *   sh_link_off  = .dynamic 节区头的 sh_link 字段
     * 写入交给下面的 poke_*（裸 pwrite）：绕开 libelf 的写路径，除了目标字段一个字节都不动。
     */
    bool locate_soname_fields(off_t& d_val_off, off_t& sh_link_off)
    {
        int fd = ::open(test_file.c_str(), O_RDONLY);
        if (fd < 0) return false;

        Elf* elf = elf_begin(fd, ELF_C_READ, nullptr);
        if (!elf) {
            ::close(fd);
            return false;
        }

        bool found = false;
        GElf_Ehdr ehdr;
        if (gelf_getehdr(elf, &ehdr) != nullptr) {
            Elf_Scn* scn = nullptr;
            while ((scn = elf_nextscn(elf, scn)) != nullptr) {
                GElf_Shdr shdr;
                if (gelf_getshdr(scn, &shdr) == nullptr) continue;
                if (shdr.sh_type != SHT_DYNAMIC) continue;

                sh_link_off = static_cast<off_t>(ehdr.e_shoff) +
                              static_cast<off_t>(elf_ndxscn(scn)) * ehdr.e_shentsize +
                              static_cast<off_t>(offsetof(Elf64_Shdr, sh_link));

                Elf_Data* data = elf_getdata(scn, nullptr);
                if (!data) break;
                const size_t entsize = shdr.sh_entsize ? shdr.sh_entsize : sizeof(Elf64_Dyn);
                const size_t count = static_cast<size_t>(shdr.sh_size / entsize);
                for (size_t i = 0; i < count; ++i) {
                    GElf_Dyn dyn;
                    if (gelf_getdyn(data, i, &dyn) == nullptr) continue;
                    if (dyn.d_tag != DT_SONAME) continue;
                    d_val_off = static_cast<off_t>(shdr.sh_offset) +
                                static_cast<off_t>(i) * static_cast<off_t>(entsize) +
                                static_cast<off_t>(offsetof(Elf64_Dyn, d_un));
                    found = true;
                    break;
                }
                break;
            }
        }

        elf_end(elf);
        ::close(fd);
        return found;
    }

    /** 裸写字段（宿主/目标都是小端 x86-64，字面值即盘上字节） */
    bool poke(const off_t off, const void* bytes, const size_t n)
    {
        int fd = ::open(test_file.c_str(), O_WRONLY);
        if (fd < 0) return false;
        const bool ok = ::pwrite(fd, bytes, n, off) == static_cast<ssize_t>(n);
        ::close(fd);
        return ok;
    }

    /** 裸读字段（还原用） */
    bool peek(const off_t off, void* bytes, const size_t n)
    {
        int fd = ::open(test_file.c_str(), O_RDONLY);
        if (fd < 0) return false;
        const bool ok = ::pread(fd, bytes, n, off) == static_cast<ssize_t>(n);
        ::close(fd);
        return ok;
    }
};

// ── 正向对照：手搓 ELF 必须真的被解析出 SONAME ───────────────────────────
//
// 没有这一条，下面三条"返回空串"就可能是"整只 ELF 没被解析"造成的假绿。
TEST_F(ElfSonameNullCheckTest, CraftedElfPositiveControlReadsRealSoname)
{
    write_crafted_elf(/*d_val=*/1, /*sh_link=*/2, /*str_size=*/sizeof(kDynstr));
    EXPECT_EQ(get_elf_soname(test_file), "libcrafted.so.1")
        << "手搓 ELF 的解析路径没走通 —— 下面几条断言会变成假绿";
}

// ── ① d_val 越出 .dynstr ────────────────────────────────────────────────
TEST_F(ElfSonameNullCheckTest, SonameOffsetBeyondDynstrReturnsEmptyInsteadOfDereferencingNull)
{
    // .dynstr 只有 18 字节，d_val = 100000 远超它 → elf_strptr 返回 NULL
    write_crafted_elf(/*d_val=*/100000, /*sh_link=*/2, /*str_size=*/sizeof(kDynstr));

    EXPECT_EQ(get_elf_soname(test_file), "")
        << "修复前：std::string(nullptr) → SIGSEGV（实测旧代码 exit=139）";
}

// ── ② sh_link 指向非 SHT_STRTAB 的节区 ──────────────────────────────────
TEST_F(ElfSonameNullCheckTest, SonameWithNonStrtabShLinkReturnsEmptyInsteadOfDereferencingNull)
{
    // 0 号节区是 SHT_NULL：d_val 本身合法，但"字符串表"根本不是字符串表 → elf_strptr 返回 NULL
    write_crafted_elf(/*d_val=*/1, /*sh_link=*/0, /*str_size=*/sizeof(kDynstr));

    EXPECT_EQ(get_elf_soname(test_file), "")
        << "修复前：std::string(nullptr) → SIGSEGV（实测旧代码 exit=139）";
}

// ── ③ 真实 .so 上改字段（最贴近"不可信包里的畸形 .so"） ──────────────────
TEST_F(ElfSonameNullCheckTest, RealLibraryWithBrokenSonameFieldsReturnsEmpty)
{
    if (!build_real_library()) GTEST_SKIP() << "gcc 不可用";
    const auto size_before = fs::file_size(test_file);

    // 补丁前：真实产物本来能正常读出 SONAME（同时证明定位逻辑没找错字段）
    ASSERT_EQ(get_elf_soname(test_file), "libcrafted.so.1");

    off_t d_val_off = -1;
    off_t sh_link_off = -1;
    ASSERT_TRUE(locate_soname_fields(d_val_off, sh_link_off)) << "没定位到 DT_SONAME 条目";

    uint64_t original_val = 0;
    ASSERT_TRUE(peek(d_val_off, &original_val, sizeof(original_val)));

    // (a) d_val 越出 .dynstr
    const uint64_t beyond = 100000;  // 远超 .dynstr 的几百字节
    ASSERT_TRUE(poke(d_val_off, &beyond, sizeof(beyond)));
    EXPECT_EQ(get_elf_soname(test_file), "") << "真实 .so 的一个越界 d_val 不得让取值路径段错误";

    // 还原那 8 个字节 → SONAME 又能读出来，证明上面那条 "" 是补丁造成的，而不是文件被写坏
    ASSERT_TRUE(poke(d_val_off, &original_val, sizeof(original_val)));
    ASSERT_EQ(get_elf_soname(test_file), "libcrafted.so.1") << "还原 d_val 后读不出来了";

    // (b) .dynamic 的 sh_link 指向 SHT_NULL（0 号）节区：d_val 合法但"字符串表"不是字符串表
    const uint32_t zero = 0;
    ASSERT_TRUE(poke(sh_link_off, &zero, sizeof(zero)));
    EXPECT_EQ(get_elf_soname(test_file), "") << "sh_link 非 SHT_STRTAB 时不得让取值路径段错误";

    // 全程只改了 8 + 4 个字节：文件大小不变（poke 没有截断）
    EXPECT_EQ(fs::file_size(test_file), size_before);
}
