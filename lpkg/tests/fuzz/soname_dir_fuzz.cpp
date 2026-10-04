// harness：`apply_soname_links()` 面对**敌意 lib 目录**时的行为。
//
// 为什么是它：这个函数按 SONAME 在 `lib_dir` 下建/删符号链接，而两个调用点传的都是
// **包内容** —— `trigger.cpp` 传目标 root 的 `<root>/usr/lib`、`builder.cpp` 传构建 staging，
// 而它跑在**批次提交之后**。所以它的任何越界写都发生在"包已落地、DB 已提交"之后，
// 且判定一抛，命令就报失败（ARCH.md 与 CLAUDE.md 都记着这一类）。
//
// **它刚刚真的出过事**（2026-10-03，`ARCH.md` §20.1）：判定链里用的是**纯词法**的
// `path_within`，而包可以同时发一条 `usr/lib/sub -> /etc` 的符号链接（归档侧对**链接目标**
// 不做任何校验）与一个 `DT_SONAME = "sub/EVIL.so"` 的库 ⇒ `lib_dir/sub/EVIL.so` 词法上完全
// 在 lib_dir 内 ⇒ `fs::create_symlink` 穿过 `sub`，在 `<root>` 之外建出链接。
// **那条是通过读代码发现的**；本 harness 的存在理由就是让同一形状能被**机器**撞出来。
//
// ── 输入是一段"要造出来的目录树"脚本（每行一条记录）─────────────────────────
//   `L <文件名> <SONAME>`  → 在 lib/ 下写一个**手搓的 ELF**（DT_SONAME 完全由输入决定）
//   `F <文件名>`            → 普通文件
//   `D <目录名>`            → 子目录
//   `S <链接名> <目标>`     → 符号链接（**目标可以是绝对路径或 `../`** —— 这正是逃逸向量）
// 字段以空格分隔、不含空格；文件名不含 `/`（要嵌套目录用 `D`）；`SONAME` 与链接**目标**
// 允许含 `/`（逃逸靠的就是它们）。
//
// ── oracle（三条）────────────────────────────────────────────────────────
//  1. **沙箱之外（`/` 与 `/tmp` 的顶层）一个条目都不许变** —— 与 `archive_name_fuzz` 同款；
//  2. **canary 目录必须恒为空**：它是**沙箱内、lib 外**的一个目录，链接目标指向它时任何
//     落位都会被抓到（这一条比第 1 条更敏感：它抓的是"越出 lib_dir"而不是"越出沙箱"）；
//  3. **不许抛**。契约来自调用点：它在**提交之后**跑，抛了就是"包已装好、命令却报失败"。
//     ⚠️ 这条是**有意**断言成硬性的：如果它真的抛了，那就是缺陷本身，不是 harness 写错。
//     若将来发现某类输入能合法地抛（比如磁盘满），应该改成计数而不是直接放开 —— 别默默降级。
//
// 另外统计三类结局（建出链接数 / 被拒/跳过数 / 异常数）并在退出时打印：证明 harness
// 真的走到了**建链接**那条路，而不是每轮都在空转。

#include <elf.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "elf/lib_utils.hpp"
#include "fuzz_common.hpp"

namespace fs = std::filesystem;

namespace
{

constexpr const char* kBase = "/lpkgfuzz";
constexpr const char* kWork = "/lpkgfuzz/soname";
constexpr const char* kLib = "/lpkgfuzz/soname/lib";
constexpr const char* kCanary = "/lpkgfuzz/soname/canary";

/// 每轮最多造多少条记录（够表达"库 + 中间段链接 + 环"的组合，又不让单轮变成 I/O 压力测试）。
constexpr std::size_t kMaxRecords = 24;

/// 逃逸检测的两个观察点（与 archive_name_fuzz 同一手法：抓"顶层条目集合"的变化）。
const char* const kWatch[] = {"/", "/tmp"};
constexpr std::size_t kWatchCount = sizeof(kWatch) / sizeof(kWatch[0]);
std::set<std::string> g_baseline[kWatchCount];

std::size_t g_links_created = 0;
std::size_t g_entries = 0;
std::size_t g_exceptions = 0;
/// **逃逸向量**被造出来过几次（SONAME 含 `/`，或符号链接目标是绝对路径/含 `..`）。
/// 存在的理由同 archive_name_fuzz 的两类计数：证明"危险那一侧"真的被走到了 ——
/// 否则一个只会造无害条目的 harness 看起来也是绿的。
std::size_t g_escape_shaped = 0;

void report_outcomes()
{
    std::fprintf(stderr,
                 "[fuzz] soname_dir: 造条目 %zu 次 / 其中逃逸形态 %zu 次 / 建出 SONAME 链接 %zu 次 "
                 "/ 异常 %zu 次\n",
                 g_entries, g_escape_shaped, g_links_created, g_exceptions);
}

std::set<std::string> list_dir(const char* path)
{
    std::set<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(path, ec)) out.insert(e.path().filename().string());
    return out;
}

std::string diff_entries(const std::set<std::string>& before, const std::set<std::string>& now)
{
    std::string out;
    for (const auto& e : now) {
        if (before.count(e) == 0) {
            out += " +";
            out += e;
        }
    }
    for (const auto& e : before) {
        if (now.count(e) == 0) {
            out += " -";
            out += e;
        }
    }
    return out;
}

[[noreturn]] void oracle_violation(const char* why)
{
    std::fprintf(stderr, "[fuzz] oracle 违反: %s\n", why);
    __builtin_trap();
}

// ======================= 手搓一个带指定 DT_SONAME 的 ELF64 =======================
//
// 布局照抄 `tests/unit/test_elf_soname_null_check.cpp` 的 CraftedElfTest（三个节区：
// null / .dynamic / .dynstr，`e_shstrndx = 0`）。**为什么不用 gcc**：每轮调一次编译器
// 太慢；而手搓还能让 SONAME 取到编译器接受不了的字节形态（含 `/`、`..`、超长、非 UTF-8）。

constexpr std::size_t kBufSize = 512;
constexpr uint64_t kDynOff = 256;  // .dynamic 数据
constexpr uint64_t kStrOff = 288;  // .dynstr 数据

std::vector<uint8_t> crafted_elf(const std::string& soname_in)
{
    std::vector<uint8_t> buf(kBufSize, 0);
    // .dynstr 下标 0 是空串、下标 1 起才是名字（与 CraftedElfTest 的 `"\0libcrafted.so.1"` 同形）
    const std::string soname = soname_in.substr(0, kBufSize - kStrOff - 3);

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
    ehdr.e_shstrndx = 0;  // 无节区名表（名字全空，不影响 SONAME 读取）
    std::memcpy(buf.data(), &ehdr, sizeof(ehdr));

    Elf64_Shdr sh[3]{};
    sh[1].sh_type = SHT_DYNAMIC;
    sh[1].sh_offset = kDynOff;
    sh[1].sh_size = sizeof(Elf64_Dyn);
    sh[1].sh_link = 2;  // 字符串表 = .dynstr
    sh[1].sh_entsize = sizeof(Elf64_Dyn);
    sh[1].sh_addralign = 8;
    sh[2].sh_type = SHT_STRTAB;
    sh[2].sh_offset = kStrOff;
    sh[2].sh_size = soname.size() + 2;
    sh[2].sh_addralign = 1;
    std::memcpy(buf.data() + sizeof(Elf64_Ehdr), sh, sizeof(sh));

    Elf64_Dyn dyn{};
    dyn.d_tag = DT_SONAME;
    dyn.d_un.d_val = 1;  // 指向 .dynstr 里 SONAME 的起点
    std::memcpy(buf.data() + kDynOff, &dyn, sizeof(dyn));
    std::memcpy(buf.data() + kStrOff + 1, soname.data(), soname.size());
    return buf;
}

bool write_bytes(const fs::path& p, const std::vector<uint8_t>& bytes)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

/// 数一数 lib/ 里现在有多少条符号链接（用来证明"建链接"那条路真的走到了）
std::size_t count_symlinks(const fs::path& dir)
{
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_symlink(ec) && !ec) ++n;
    }
    return n;
}

// ============================ 输入 → 目录树 ============================

struct Record {
    char kind;
    std::string a;
    std::string b;
};

std::vector<Record> parse_records(const uint8_t* data, std::size_t size)
{
    std::vector<Record> out;
    std::string line;
    auto flush = [&] {
        if (line.empty()) return;
        const auto sp1 = line.find(' ');
        if (sp1 == std::string::npos) return;
        Record r{line[0], line.substr(sp1 + 1), {}};
        const auto sp2 = r.a.find(' ');
        if (sp2 != std::string::npos) {
            r.b = r.a.substr(sp2 + 1);
            r.a = r.a.substr(0, sp2);
        }
        if (!r.a.empty()) out.push_back(std::move(r));
    };
    for (std::size_t i = 0; i < size && out.size() < kMaxRecords; ++i) {
        if (data[i] == '\n') {
            flush();
            line.clear();
        } else {
            line.push_back(static_cast<char>(data[i]));
        }
    }
    if (out.size() < kMaxRecords) flush();
    return out;
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    std::error_code ec;
    fs::remove_all(kBase, ec);
    fs::create_directories(kWork, ec);
    for (std::size_t i = 0; i < kWatchCount; ++i) g_baseline[i] = list_dir(kWatch[i]);
    silence_stdout();
    // 同 wal_line_fuzz：只静音 C++ 的 `std::cerr`（lpkg 的告警走它，本 harness 每轮都会
    // 触发 `warning.soname_escapes_lib_dir` / `warning.soname_link_failed`，不处理会洪泛），
    // **裸 fd 2 留着** —— sanitizer 报告与本文件的 oracle_violation 走的都是它。
    std::cerr.rdbuf(std::cout.rdbuf());
    std::atexit(report_outcomes);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > 4096) return 0;
    // 含 NUL 的输入丢掉：文件名/链接目标最终都要过 `fs::path` 的 C 串边界，含 NUL 时
    // 实际生效的是**截断后的另一个名字**，读结果时极易误判（同 archive_name_fuzz 的取舍）。
    for (std::size_t i = 0; i < size; ++i) {
        if (data[i] == '\0') return 0;
    }

    // 从干净沙箱开始（清理放轮首：进程被杀也留有界）
    std::error_code ec;
    fs::remove_all(kWork, ec);
    fs::create_directories(kLib, ec);
    fs::create_directories(kCanary, ec);
    if (!fs::exists(kLib) || !fs::exists(kCanary)) return 0;

    const std::vector<Record> records = parse_records(data, size);
    for (const auto& r : records) {
        // 文件名不含 '/'：要嵌套目录就用 `D` 记录，别让 fs::path 把名字当路径分隔符
        if (r.a.find('/') != std::string::npos) continue;
        const fs::path p = fs::path(kLib) / r.a;
        switch (r.kind) {
            case 'L':
                if (r.b.empty()) continue;
                // SONAME 含 `/` 是 2026-10-03 那条缺陷的形态（`lib_dir/sub/EVIL.so` 词法上
                // 仍在 lib_dir 内），单独计数以便确认这一侧被覆盖到
                if (r.b.find('/') != std::string::npos) ++g_escape_shaped;
                if (write_bytes(p, crafted_elf(r.b))) ++g_entries;
                break;
            case 'F':
                if (write_bytes(p, {'x'})) ++g_entries;
                break;
            case 'D':
                if (fs::create_directories(p, ec)) ++g_entries;
                break;
            case 'S':
                if (r.b.empty()) continue;
                if (r.b.find("..") != std::string::npos || r.b.front() == '/') ++g_escape_shaped;
                ec.clear();
                fs::create_symlink(r.b, p, ec);  // 目标**不做任何校验**：这正是要 fuzz 的向量
                if (!ec) ++g_entries;
                break;
            default:
                break;
        }
    }

    const std::size_t links_before = count_symlinks(kLib);

    try {
        apply_soname_links(kLib);
    } catch (const std::exception& e) {
        ++g_exceptions;
        std::fprintf(stderr, "[fuzz] 异常原文: %s\n", e.what());
        oracle_violation(
            "apply_soname_links 抛了 —— 它在**提交之后**跑，抛了就是「包已装好、命令却报失败」");
    } catch (...) {
        ++g_exceptions;
        oracle_violation("apply_soname_links 抛了非 std::exception");
    }

    if (count_symlinks(kLib) > links_before) ++g_links_created;

    // ── oracle 1：沙箱之外的顶层条目集合不许变 ──
    for (std::size_t i = 0; i < kWatchCount; ++i) {
        const std::set<std::string> now = list_dir(kWatch[i]);
        if (now != g_baseline[i]) {
            std::fprintf(stderr, "[fuzz] %s 的顶层条目变了:%s\n", kWatch[i],
                         diff_entries(g_baseline[i], now).c_str());
            oracle_violation("链接落到了沙箱之外");
        }
    }

    // ── oracle 2：canary 恒为空（抓"越出 lib_dir 但仍在沙箱内"）──
    if (!list_dir(kCanary).empty()) {
        std::fprintf(stderr, "[fuzz] canary 里出现了:%s\n",
                     diff_entries({}, list_dir(kCanary)).c_str());
        oracle_violation("有东西落到了 lib_dir 之外（canary 非空）");
    }

    // ── oracle 3：沙箱顶层恒为 {lib, canary} ──
    const std::set<std::string> expected = {"lib", "canary"};
    if (list_dir(kWork) != expected) {
        std::fprintf(stderr, "[fuzz] %s 下条目异常:%s\n", kWork,
                     diff_entries(expected, list_dir(kWork)).c_str());
        oracle_violation("沙箱顶层多出（或少了）条目");
    }

    return 0;
}
