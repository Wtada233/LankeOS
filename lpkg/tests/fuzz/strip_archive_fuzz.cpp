// harness #4：二进制/归档的 strip 入口 —— `strip_file()`（含 `process_archive` 那条 ar 路径）。
//
// 为什么是它：`builder.cpp` 对 staging 下每个 magic 命中的文件（`\x7fELF` 或 `!<`）调
// `strip_binary` → `strip_file`，输入是**不可信的上游构建产物**。而 `.a` 那条路
// （`process_archive`）**自己重写整份归档**，还要把符号索引里那串"符号 → 成员头偏移"
// 重算到新位置。这里真出过 bug：重写后**照抄**旧索引（成员变短 ⇒ 其后成员全前移 ⇒ 偏移
// 全错），`ld` 报 `error adding symbols: no more archived files`（硬失败，不是"少个优化"），
// 实测污染过 bison / nspr / gcc 三个包的产物。`elf_strip_fuzz` 覆盖不到这条路 —— 它只调
// `strip_elf_data`（纯内存），`.a` 在它那里一律返回 false。
//
// oracle（都不需要外部工具）：
//   ① **绝不产垃圾**：返回 true 时，盘上那份必须仍是 libelf 能解析的 ELF；
//   ② **绝不产空**：返回 true 时文件不得为空（既有用例 `MalformedArchiveIsPreservedNotEmptied`
//      钉的就是这个方向）；
//   ③ **内容成员名单不变**：重写前后成员**名字**逐一相同（改名/重排会让下游"按名取成员"错位）；
//   ④ **索引保持性**（当年那个真 bug 本身）：输入带着一份**有效**的符号索引（每条偏移都
//      落在成员头上）⇒ 输出也得有索引、且每条偏移仍落在成员头上。**判据必须先看输入** ——
//      输入自带坏索引时 lpkg 会原样照抄（畸形归档我们不修，只是别把好的弄坏）。索引要么被
//      正确重算，要么整份归档根本不被碰（认不出索引时 `process_archive` 返回 false）——
//      **不允许出现"索引还在、偏移指不到成员"**。
//
// **已知覆盖边界**：不校验索引里**符号名**与实际成员定义的对应关系（那要解 `.symtab`），
// 只判"偏移落在成员头上"。符号名那一层由 gtest 的
// `ArchiveWithTwoMembersKeepsIndexConsistent`（含真 ld 链接）覆盖。

#include <archive.h>
#include <archive_entry.h>
#include <elf.h>
#include <gelf.h>
#include <libelf.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "elf/strip.hpp"

namespace fs = std::filesystem;

namespace
{

constexpr const char* kDir = "/tmp/lpkgfz_strip";
constexpr const char* kFile = "/tmp/lpkgfz_strip/f.bin";

/// 独立地把一串字节当 ELF 解析一遍（同 `elf_strip_fuzz` 的判据）。
bool parses_as_elf(const std::vector<uint8_t>& bytes)
{
    if (bytes.size() < EI_NIDENT) return false;
    Elf* elf =
        elf_memory(const_cast<char*>(reinterpret_cast<const char*>(bytes.data())), bytes.size());
    if (elf == nullptr) return false;
    GElf_Ehdr hdr{};
    const bool ok = gelf_getehdr(elf, &hdr) != nullptr;
    elf_end(elf);
    return ok;
}

/// 极简 ar 扫描（GNU ar 成员头固定 60 字节 ASCII：`name[16] mtime[12] uid[6] gid[6]
/// mode[8] size[10] end[2]("`\n")`，成员数据按 2 字节对齐）。只取 oracle 要用的两样：
/// 成员头偏移、索引成员里的偏移（libarchive 不给文件偏移，所以这两样只能自己量）。
struct ArInfo {
    bool is_ar = false;
    bool has_index = false;
    std::vector<uint64_t> header_offsets;
    std::vector<uint64_t> index_offsets;
};

ArInfo scan_ar(const std::vector<uint8_t>& b)
{
    ArInfo r;
    if (b.size() < 8 || std::memcmp(b.data(), "!<arch>\n", 8) != 0) return r;
    r.is_ar = true;
    std::size_t off = 8;
    while (off + 60 <= b.size()) {
        const char* h = reinterpret_cast<const char*>(b.data() + off);
        if (h[58] != '`' || h[59] != '\n') break;  // 头尾哨兵不对 ⇒ 到此为止（不猜）
        std::string name(h, 16);
        while (!name.empty() && name.back() == ' ') name.pop_back();
        const long n = std::strtol(std::string(h + 48, 10).c_str(), nullptr, 10);
        if (n < 0) break;
        const std::size_t body = off + 60;
        const std::size_t size = static_cast<std::size_t>(n);
        if (body + size > b.size()) break;

        r.header_offsets.push_back(off);
        if (name == "/" || name == "/SYM64/") {
            r.has_index = true;
            // 32 位 GNU 索引体：`<n:4BE>` + n × `<成员头偏移:4BE>` + 名字表。
            if (name == "/" && size >= 4) {
                const auto be32 = [&](std::size_t o) {
                    return (static_cast<uint32_t>(b[body + o]) << 24) |
                           (static_cast<uint32_t>(b[body + o + 1]) << 16) |
                           (static_cast<uint32_t>(b[body + o + 2]) << 8) |
                           static_cast<uint32_t>(b[body + o + 3]);
                };
                const uint32_t cnt = be32(0);
                if (4ull + 4ull * cnt <= size) {
                    for (uint32_t i = 0; i < cnt; ++i) r.index_offsets.push_back(be32(4 + 4 * i));
                }
            }
        }
        off = body + size + (size & 1);  // 成员数据按偶数偏移对齐
    }
    return r;
}

/// 内容成员名 —— 走 **libarchive**（生产路径同一个读取器），即任何消费者（ld / ar / nm）
/// 看到的名字。
///
/// ⚠️ **别拿成员头里的裸名字段来比**：libarchive 的 ar 写入器会把**不合规**的名字段规范化
/// （实测 `/     -` → `     -/`），裸字段比对会把这段既有行为误报成"改名"——本 harness 第一版
/// 就是这么假红的（那个输入 `index_invalid=false`，**根本没进** strip.cpp 的索引代码）。
/// 名字段形如 `/     -` 的归档不是任何真实工具产出的形态，但 fuzzer 会造出来，判据要免疫。
/// 成员头里的名字段是不是 GNU ar 能**原样往返**的形态（可打印 ASCII，`/` 只做结尾终止符）。
///
/// **为什么 oracle ③ 要先过这道筛**：libarchive 的 ar **写入器**会把不合规的名字段规范化
/// ——实测 `/             p` → `             p/`、`a.o/è` → `è/`（高位字节）——于是"名字不变"
/// 会假红三次（本 harness 第一、二、三版各一次）。那既**不是 lpkg 的逻辑**（`index_invalid`
/// 为 false 时压根没进新代码，而且改动前也走同一条 libarchive 拷贝路径），也不是真实上游会
/// 产出的形态：GNU ar 的名字里不允许 `/`（它是终止符），名字段以 `/` 开头则被保留给特殊成员
/// （`/`、`//`、`/SYM64/`）。**生产侧对这类归档的态度也很明确**：`scan_ar_raw_layout` 一见
/// 以 `/` 开头又非上述三种的布局就判"不可信"（`Refuse`，整份归档不剥）。
///
/// 所以本 harness 和被测代码的适用范围对齐：**只对合规名字段的归档钉"名字/顺序不变"**。
/// 真实语料（本机 112 个 `.a` + gcc 造的 1/8/30 成员归档）全部落在这个集合里。
bool ar_name_fields_are_conforming(const std::vector<uint8_t>& b)
{
    if (b.size() < 8 || std::memcmp(b.data(), "!<arch>\n", 8) != 0) return false;
    std::size_t off = 8;
    while (off + 60 <= b.size()) {
        const char* h = reinterpret_cast<const char*>(b.data() + off);
        if (h[58] != '`' || h[59] != '\n') return false;
        std::string name(h, 16);
        while (!name.empty() && name.back() == ' ') name.pop_back();
        const bool special = (name == "/" || name == "//" || name == "/SYM64/");
        if (!special) {
            if (name.empty()) return false;
            for (std::size_t i = 0; i < name.size(); ++i) {
                const unsigned char c = static_cast<unsigned char>(name[i]);
                if (c < 0x21 || c > 0x7e) return false;              // 非可打印 ASCII
                if (c == '/' && i + 1 != name.size()) return false;  // `/` 只能是结尾终止符
            }
        }
        const long n = std::strtol(std::string(h + 48, 10).c_str(), nullptr, 10);
        if (n < 0) return false;
        off += 60 + static_cast<std::size_t>(n) + (static_cast<std::size_t>(n) & 1);
    }
    return true;
}

std::vector<std::string> content_names_via_libarchive(const std::vector<uint8_t>& bytes)
{
    std::vector<std::string> out;
    struct archive* a = archive_read_new();
    archive_read_support_format_all(a);
    if (archive_read_open_memory(a, bytes.data(), bytes.size()) != ARCHIVE_OK) {
        archive_read_free(a);
        return out;
    }
    struct archive_entry* e = nullptr;
    while (archive_read_next_header(a, &e) == ARCHIVE_OK) {
        const char* n = archive_entry_pathname(e);
        const std::string_view nm = n != nullptr ? std::string_view(n) : std::string_view();
        if (nm != "/" && nm != "//" && nm != "/SYM64/" && nm != "__.SYMDEF" &&
            nm != "__.SYMDEF SORTED") {
            out.emplace_back(nm);
        }
        archive_read_data_skip(a);
    }
    archive_read_free(a);
    return out;
}

std::vector<uint8_t> read_file(const fs::path& p)
{
    std::vector<uint8_t> out;
    FILE* f = std::fopen(p.c_str(), "rb");
    if (f == nullptr) return out;
    uint8_t buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return out;
}

[[noreturn]] void oracle_violation(const char* why)
{
    std::fprintf(stderr, "[fuzz] oracle 违反: %s\n", why);
    __builtin_trap();
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    elf_version(EV_CURRENT);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > (1u << 20)) return 0;
    std::vector<uint8_t> in(data, data + size);

    // 只喂这两类：别的类型在 `identify_file_type` 就被判掉了，喂进去纯属浪费迭代。
    const bool is_elf = size >= 4 && in[0] == 0x7f && in[1] == 'E' && in[2] == 'L' && in[3] == 'F';
    const bool is_ar = size >= 8 && std::memcmp(in.data(), "!<arch>\n", 8) == 0;
    if (!is_elf && !is_ar) return 0;
    // 名字段不是 GNU ar 能原样往返的形态（libarchive 写入器会规范化它）⇒ 跳过，见该函数注释
    if (is_ar && !ar_name_fields_are_conforming(in)) return 0;

    const ArInfo before = is_ar ? scan_ar(in) : ArInfo{};
    const std::vector<std::string> names_before =
        is_ar ? content_names_via_libarchive(in) : std::vector<std::string>{};

    std::error_code ec;
    fs::create_directories(kDir, ec);
    fs::remove(std::string(kFile) + ".tmp", ec);  // 上一轮可能留下的
    {
        FILE* f = std::fopen(kFile, "wb");
        if (f == nullptr) return 0;
        std::fwrite(in.data(), 1, in.size(), f);
        std::fclose(f);
    }

    std::string err;
    if (!strip_file(kFile, err)) {
        // 拒绝是正常结局（畸形输入本该被拒；认不出索引时也会走到这里 —— 那种情况下
        // `process_archive` **一个字节都不碰原文件**，用户得到告警，库照旧能链）。
        // `error_msg` 是否为空这里不断言 —— `strip.cpp` 里有**有意**的静默跳过。
        return 0;
    }

    const std::vector<uint8_t> out = read_file(kFile);
    if (out.empty()) oracle_violation("返回 true 却把文件写空了");
    if (is_elf && !parses_as_elf(out)) oracle_violation("ELF 产物不能被 libelf 重新解析");
    if (is_ar) {
        if (content_names_via_libarchive(out) != names_before) {
            oracle_violation("ar 内容成员的名字/顺序在重写前后不一致（下游按名取成员会错位）");
        }
        const ArInfo after = scan_ar(out);
        // ④ **保持性**：输入里的索引**本来就好** ⇒ 输出里的也必须好。判据必须先看输入 ——
        // 输入自带一份坏索引（fuzzer 造得出来，实测偏移 1750335488）时，lpkg 只要没改动成员
        // 就会**原样照抄**它（既有行为：畸形归档我们**不修**，只是别把好的弄坏），单方面拿输出
        // 断言"偏移必须落在成员头"会假红。
        const auto index_consistent = [](const ArInfo& i) {
            for (const uint64_t o : i.index_offsets) {
                if (std::find(i.header_offsets.begin(), i.header_offsets.end(), o) ==
                    i.header_offsets.end()) {
                    return false;
                }
            }
            return true;
        };
        if (before.has_index && index_consistent(before)) {
            if (!after.has_index) {
                oracle_violation(
                    "输入带（有效的）符号索引、输出没有了 —— 索引被静默丢掉，`.a` 被降级成"
                    "非正常形态（binutils 在 --strip-debug 档位是保留并重建的）");
            }
            if (!index_consistent(after)) {
                oracle_violation(
                    "写出的符号索引里有偏移指不到成员头（索引失效 ⇒ "
                    "ld: error adding symbols: no more archived files）");
            }
        }
    }
    return 0;
}
