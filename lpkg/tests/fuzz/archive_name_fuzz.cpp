// harness #2：归档成员名 → 整归档解压 + 沙箱逃逸检测。
//
// 为什么是它：`archive.cpp:118` 原话 —— "成员名是**不可信输入**（未校验的 .lpkg、无校验和的
// 上游源码包）"。名字会变成 WAL 行的字面内容，也能决定解压落点。判据在
// `member_name_rejection_message`（读侧 `archive.cpp:171`、写侧 `packer.cpp:40` **共用同一份**）。
//
// **输入是"成员名"，不是归档字节**：直接变异压缩后的 .tar.zst 字节，绝大多数变异会死在 zstd
// framing 上、压根到不了判据，覆盖率是假的。所以每轮由 harness 现场造一个**裸 tar**
// （`extract_tar_zst` 走 `archive_read_support_filter_all`，见 `archive.cpp:184`，裸 tar
// 照样能读），成员名取 fuzz 输入 —— 变异预算全花在真正要测的那个量上。
//
// **tar 是手写的，不是 libarchive 写的**（2026-10-03 改）。原因：libarchive 3.8.3 的写侧对
// 某类路径名有 heap-buffer-overflow（用一个**只链 libarchive、零 lpkg 代码**的 20 行程序
// 复现过：`archive_write_header` 内部 `strncpy` 读到自己缓冲区前 1 字节）。那是库的缺陷，
// 不是 lpkg 的，但走写侧的 harness 会把它当"崩溃"报出来、并让整轮 fuzz 就此结束。
// 手写 ustar 之后，fuzz 字节只进入 **libarchive 的读侧**与 **lpkg 的判据** —— 正是生产路径
// 的形状（`packer.cpp` 也从不把任意名字交给写侧）。
//
// **关于 `..` 的预期**（别把它当缺陷报）：`member_path_relative`(`archive.cpp:133`) 只剥前导
// `/` 与 `./`，**不**拦 `..`；生产靠 libarchive 的 `ARCHIVE_EXTRACT_SECURE_NODOTDOT` /
// `SECURE_SYMLINKS`(`archive.cpp:191-195`) 兜底 ——
// `tests/unit/test_archive_confinement.cpp:113-124` 钉的就是"整包被拒"。所以含 `..`
// 的名字**表现为整包解压失败**才是对的。
//
// **逃逸检测的边界（别读成比它更强的保证）**：容器里没有 CAP_SYS_ADMIN（挂载不可用），所以
// 只能做**有界深度**检查 —— 每轮后核对 `/`、`/tmp` 的**顶层条目集合**没变，且 `/lpkgfuzz`
// 下恰好只有 `{in.tar, run}`。它抓得住"`..` 逃逸到某顶层目录之下"，**抓不住**"逃逸进某个
// 已存在目录的深处"（例如 `/usr/lib/x`）—— 后者要 `chroot` 才完备，而 `chroot` 会打断
// libFuzzer 自己的语料/产物路径，代价与收益不匹配（本批不做，留作升级路径）。

#include <dirent.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "archive/archive.hpp"
#include "fuzz_common.hpp"

namespace fs = std::filesystem;

namespace
{

constexpr const char* kBase = "/lpkgfuzz";
constexpr const char* kRun = "/lpkgfuzz/run";
constexpr const char* kArc = "/lpkgfuzz/in.tar";

/// 成员名上限：语料里最长的注入样例也就几十字节；1 KiB 足够表达所有形态，又能挡住
/// "名字无限长"这种把单次迭代拖成 I/O 压力的输入。
constexpr std::size_t kMaxName = 1024;

/// 每次从 `/lpkgfuzz/run` 的祖先链出发、经过 ≤2 个 `..` 能落到的位置，就是这两个的顶层。
/// 用 C 数组而不是 `std::vector`：静态存储期的初始化不该有抛的可能。
const char* const kWatch[] = {"/", "/tmp"};
constexpr std::size_t kWatchCount = sizeof(kWatch) / sizeof(kWatch[0]);
std::set<std::string> g_baseline[kWatchCount];

/// 两类结局各计一次。存在的理由有两个：① 让 catch 不是空的（空 catch 会掩盖问题）；
/// ② 退出时打一行，用来确认"拒绝"那条路径**真的被走到**了 —— 否则一个只跑接受路径的
/// harness 看起来也是绿的。
std::size_t g_extracted = 0;
std::size_t g_refused = 0;

void report_outcomes()
{
    std::fprintf(stderr, "[fuzz] archive_name: 解压成功 %zu 次 / 被拒 %zu 次\n", g_extracted,
                 g_refused);
}

std::set<std::string> list_dir(const char* path)
{
    std::set<std::string> out;
    DIR* d = ::opendir(path);
    if (d == nullptr) return out;
    while (struct dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name != "." && name != "..") out.insert(name);
    }
    ::closedir(d);
    return out;
}

void snapshot_watch()
{
    for (std::size_t i = 0; i < kWatchCount; ++i) g_baseline[i] = list_dir(kWatch[i]);
}

// ============================ 手写 ustar ============================
//
// 只做这一件事所需的最小集：一个成员（普通文件或目录），长名字走 GNU longlink。
// 不用 PAX：PAX 要为每个属性算 `"%d %s=%s\n"` 的长度，多一处算术就多一处能写错的地方。

constexpr std::size_t kBlock = 512;

/// tar 的数值字段：宽度 -1 位八进制 + 结尾 NUL（`snprintf` 自己补 NUL）。
void put_octal(char* field, std::size_t width, uint64_t value)
{
    std::snprintf(field, width, "%0*llo", static_cast<int>(width - 1),
                  static_cast<unsigned long long>(value));
}

void put_bytes(char* field, std::size_t width, std::string_view s)
{
    std::memcpy(field, s.data(), std::min(s.size(), width));
}

/// 校验和按规范算：**校验字段本身先当 8 个空格**参与求和。
uint64_t header_checksum(const char* h)
{
    uint64_t sum = 0;
    for (std::size_t i = 0; i < kBlock; ++i) {
        sum += (i >= 148 && i < 156) ? static_cast<unsigned char>(' ')
                                     : static_cast<unsigned char>(h[i]);
    }
    return sum;
}

void append_header(std::vector<char>& out, std::string_view name, char typeflag, uint64_t size)
{
    std::array<char, kBlock> h{};
    put_bytes(h.data() + 0, 100, name);  // 超长部分由 GNU longlink 携带
    put_octal(h.data() + 100, 8, 0644);
    put_octal(h.data() + 108, 8, 0);  // uid
    put_octal(h.data() + 116, 8, 0);  // gid
    put_octal(h.data() + 124, 12, size);
    put_octal(h.data() + 136, 12, 0);     // mtime
    std::memset(h.data() + 148, ' ', 8);  // 校验和字段先填空格（规范如此）
    h[156] = typeflag;
    std::memcpy(h.data() + 257, "ustar", 5);
    h[262] = '\0';
    h[263] = '0';
    h[264] = '0';  // version "00"
    const uint64_t sum = header_checksum(h.data());
    std::snprintf(h.data() + 148, 7, "%06llo", static_cast<unsigned long long>(sum));
    h[154] = '\0';
    h[155] = ' ';  // 经典写法：6 位八进制 + NUL + 空格
    out.insert(out.end(), h.begin(), h.end());
}

/// 追加一段数据并按 512 字节补齐。
void append_padded(std::vector<char>& out, std::string_view data)
{
    out.insert(out.end(), data.begin(), data.end());
    const std::size_t rem = data.size() % kBlock;
    if (rem != 0) out.insert(out.end(), kBlock - rem, '\0');
}

/// 造一个只含一个成员的裸 tar（GNU 风格，长名字用 `././@LongLink`）。
std::vector<char> build_archive(const std::string& name)
{
    std::vector<char> out;
    const bool is_dir = !name.empty() && name.back() == '/';
    // ustar 的 name 字段只有 100 字节（可无 NUL 结尾）。`> 99` 就走 longlink —— 这样
    // 100 字节的名字也不会踩"字段恰好填满"的边界。
    if (name.size() > 99) {
        append_header(out, "././@LongLink", 'L', name.size() + 1);
        append_padded(out, std::string(name) + '\0');
    }
    append_header(out, name, is_dir ? '5' : '0', is_dir ? 0 : 1);
    if (!is_dir) append_padded(out, "x");
    out.insert(out.end(), 2 * kBlock, '\0');  // 归档结束标记：两个零块
    return out;
}

bool write_file(const fs::path& path, const std::vector<char>& bytes)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return written == bytes.size();
}

// ============================ oracle ============================

/// "第二个集合里相对第一个多了什么、少了什么" —— 逃逸诊断里最有信息量的就是它：
/// 逃逸成功时，凭空多出来的那个名字直接告诉你东西落到哪了。
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

/// oracle 违反 —— **先把原因打到 stderr 再崩**（同 `elf_strip_fuzz`：SIGILL 的栈会被 ASan 丢掉，
/// 不打这行的话两处断点长得一模一样，没法定位）。stderr 没有被静音（见 `fuzz_common.hpp`）。
[[noreturn]] void oracle_violation(const char* why)
{
    std::fprintf(stderr, "[fuzz] oracle 违反: %s\n", why);
    __builtin_trap();
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    std::error_code ec;
    fs::remove_all(kBase, ec);
    fs::create_directories(kBase, ec);
    snapshot_watch();
    silence_stdout();  // 进度行走 stdout（ui/term.cpp）；stderr 留着给 sanitizer 与警告
    std::atexit(report_outcomes);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > kMaxName) return 0;
    // 含 NUL 的名字丢掉：tar 的 name 字段与 GNU longlink 都是以 NUL 结尾的 C 串，内部 NUL
    // 会让实际生效的名字被截断 —— 那种输入 fuzz 的是"截断后的另一个名字"，读结果时极易误判。
    for (std::size_t i = 0; i < size; ++i) {
        if (data[i] == '\0') return 0;
    }
    const std::string name(reinterpret_cast<const char*>(data), size);

    std::error_code ec;
    fs::remove_all(kRun, ec);  // 从干净沙箱开始（清理放在轮**首**：进程被杀也留有界）
    fs::create_directories(kRun, ec);
    if (!write_file(kArc, build_archive(name))) return 0;

    try {
        extract_tar_zst(kArc, kRun, "fuzz");
        ++g_extracted;
    } catch (const std::exception&) {
        // 拒绝是正常结局：判据抛 UnsafeArchiveException（`archive.cpp:173`），libarchive 抛
        // 它自己的（SECURE_NODOTDOT / SECURE_SYMLINKS / 越界 hardlink 目标）。
        ++g_refused;
    }

    // ---- oracle：没有东西落到沙箱之外 ----
    for (std::size_t i = 0; i < kWatchCount; ++i) {
        const std::set<std::string> now = list_dir(kWatch[i]);
        if (now != g_baseline[i]) {
            const std::string diff = diff_entries(g_baseline[i], now);
            std::fprintf(stderr, "[fuzz] %s 的顶层条目变了:%s\n", kWatch[i], diff.c_str());
            oracle_violation("沙箱之外出现了（或消失了）条目");
        }
    }
    // /lpkgfuzz 下恒为这两个（多出或少了任何东西，都是逃逸或清理漏了）
    const std::set<std::string> expected = {"in.tar", "run"};
    const std::set<std::string> base = list_dir(kBase);
    if (base != expected) {
        const std::string diff = diff_entries(expected, base);
        std::fprintf(stderr, "[fuzz] %s 下条目异常:%s\n", kBase, diff.c_str());
        oracle_violation("/lpkgfuzz 下的条目不是 {in.tar, run}");
    }

    return 0;
}
