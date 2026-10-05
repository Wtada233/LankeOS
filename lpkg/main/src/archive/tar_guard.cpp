#include "archive/tar_guard.hpp"

#include <archive_entry.h>

#include <cstddef>

#include "archive/archive.hpp"  // escape_member_name（唯一实现，见该头文件）
#include "base/exception.hpp"
#include "i18n/localization.hpp"

namespace fs = std::filesystem;

namespace
{

/// 成员名总长上限（字节）。取 PATH_MAX 量级 —— 超长路径是 libarchive 已知会绕过自己符号
/// 链接检查的那一格（issue #744/#745），所以这里必须自己封。
/// ⚠️ 这是**结构性**判据（名字长到不可能是合法 tar 成员），**不是**资源上界：维护者
/// 2026-10-05 明确拍板不加成员数/解压比/累计尺寸那类限额，别把这条当成先例去补别的。
constexpr std::size_t MAX_MEMBER_NAME_BYTES = 4096;

/// 单个路径分量上限：POSIX 的 NAME_MAX。超过它任何文件系统都建不出来。
constexpr std::size_t MAX_COMPONENT_BYTES = 255;

/**
 * 相对路径归一化后是否**逃出了根**（首分量是 `..`）。
 *
 * 只做词法判定，**不碰文件系统**：这里判的是"这个名字打算去哪儿"，不是"它现在指向哪儿"。
 * `usr/bin/../../etc/x` 归一化后是 `etc/x`（仍在根内，**合法**）；只有归一化结果**以 `..`
 * 开头**才意味着上溯穿出了解压根。
 */
bool escapes_root_lexically(const fs::path& relative_path)
{
    const fs::path normalized = relative_path.lexically_normal();
    for (const auto& part : normalized) {
        if (part == "..") return true;
        if (part == ".") continue;
        return false;  // 第一个实义分量不是 `..` ⇒ 没穿出去
    }
    return false;  // 归一化后为空（`".."` 之外的退化形态）——不判越界
}

}  // namespace

void TarGuard::check(struct archive_entry* entry, std::string_view raw_name,
                     const std::string& relative, const fs::path& archive_path)
{
    const std::string arch = archive_path.string();
    // 危险名字**本身就是载荷**：含 ESC 的名字原样进消息会再污染一份终端、含 `\n` 的会把
    // 异常消息自己切成两行。故一律转义后再进消息（唯一实现在 archive.cpp）。
    const std::string shown = escape_member_name(raw_name);

    // ⚠️ **没有**"无名成员"这条判据 —— 不可达，已删（2026-10-05）。`extract_tar_zst` 在
    // 调用本函数**之前**就有 `if (member.empty()) continue;`（`.`、`./` 这类归一化成空、
    // 本来就不落盘的成员），空名字根本进不来。原判据是死代码。

    // ── 1. 控制字符（含 ESC） ─────────────────────────────────────────────────
    // `\n` / `\r` / `\t` / `\0` 已由 `member_name_rejection_message` 先一步拒掉
    // （那份判据读写两侧共用，消息是 `error.unsafe_member_control`）。这里补的是**其余**
    // 控制字符 —— 最典型的是 ESC：它能把包名/文件名的内容变成终端的 ANSI 指令。
    for (const unsigned char c : raw_name) {
        if (c < 0x20 || c == 0x7f) {
            throw UnsafeArchiveException(
                string_format("error.archive_malformed_name_encoding", arch, shown));
        }
    }

    // ⚠️ **没有**"拒反斜杠"这条判据 —— **会误伤真实包**，已删（2026-10-05 实测）。
    // 通用加固清单里"反斜杠 = 跨平台 zip-slip"排在很前面，但**本仓库的真实产物里有它**：
    // `systemd` 包的成员 `content/usr/lib/systemd/system/system-systemd\x2dmute\x2dconsole.slice`
    // —— systemd 用 `\x2d` 转义 unit 名里的 `-`，**文件名里字面就带反斜杠**（全仓 861 个包
    // 扫下来仅此一例，而它恰好是 base 包：拒了它整个发行版都装不上）。
    // 对 lpkg 而言这种名字完全无害：POSIX 上 `\` 只是普通字节，我们不跨平台解压。
    // 绊线用例 `LiteralBackslashInMemberNameIsLegalSystemdUsesIt` 钉住它 —— 别再把这条加回来。

    // ── 3. 名字/分量长度 ──────────────────────────────────────────────────────
    if (raw_name.size() > MAX_MEMBER_NAME_BYTES) {
        throw UnsafeArchiveException(string_format("error.archive_malformed_path_too_long", arch,
                                                   std::to_string(raw_name.size()),
                                                   std::to_string(MAX_MEMBER_NAME_BYTES)));
    }
    {
        std::size_t run = 0;
        for (const char c : raw_name) {
            if (c == '/') {
                run = 0;
                continue;
            }
            if (++run > MAX_COMPONENT_BYTES) {
                throw UnsafeArchiveException(string_format("error.archive_malformed_path_too_long",
                                                           arch, std::to_string(run),
                                                           std::to_string(MAX_COMPONENT_BYTES)));
            }
        }
    }

    // ── 4. `..` 分量 ──────────────────────────────────────────────────────────
    // **只拒 `..`，`.` 放行**：`./usr/bin/foo` 是 `tar cf pipe .` 那种打包方式的正常产物
    // （成员名带 `./` 前缀），`usr/./bin` 也只会被 `lexically_normal` 无声抹平。把 `.` 一起
    // 拒掉会**误伤大量真实源码 tarball**。
    if (escapes_root_lexically(fs::path(relative))) {
        throw UnsafeArchiveException(
            string_format("error.archive_malformed_dot_component", arch, shown));
    }

    // ⚠️ **没有**"尾斜杠与类型不一致"这条判据 —— 它**不可达**，已删（2026-10-05 实测）。
    // 原本想拒"名字带尾斜杠却不是目录"的成员，实测发现 **libarchive 自己按尾斜杠判定类型**：
    // 一个 typeflag='0'、名字以 `/` 结尾的成员读回来 `archive_entry_filetype()` 就是
    // `AE_IFDIR`，冲突根本到不了本层（用例 `TrailingSlashIsNormalizedByLibarchive` 把这个
    // 行为钉住当绊线）。按仓库纪律，不为走不到的分支留判据。

    // ── 5. 重名成员（本模块存在的首要理由） ───────────────────────────────────
    // 放在最后判，是为了让上面那些"更具体"的原因先报出来 —— 一个既重名又名带 ESC 的成员
    // 报 ESC 更有助于定位。对**合法**归档而言这几条互不影响：重名是唯一一条需要跨成员
    // 状态的。
    if (!seen_.insert(relative).second) {
        throw UnsafeArchiveException(
            string_format("error.archive_malformed_duplicate_member", arch, shown));
    }

    // ── 6. 符号链接目标 ───────────────────────────────────────────────────────
    // 绝对目标**放行**（`--root` 下包发 `<root>/usr/bin/foo -> /etc/foo` 合法且必要）。
    // 相对目标按"符号所在目录 + 目标"词法归一化，看是否还留在根内：
    //   `usr/bin/foo -> ../lib/foo`      ⇒ `usr/lib/foo`   放行（合法且常见）
    //   `usr/bin/x -> ../../../../etc/passwd` ⇒ `../../../etc/passwd` ⇒ 拒
    // 用"目标里含不含 `..`"当判据会误杀第一例 —— 这是本仓库有意的判据选择。
    const auto ftype = archive_entry_filetype(entry);
    if (ftype == AE_IFLNK) {
        const char* target = archive_entry_symlink(entry);
        if (target && *target != '\0') {
            const fs::path t(target);
            if (!t.is_absolute() && escapes_root_lexically(fs::path(relative).parent_path() / t)) {
                throw UnsafeArchiveException(string_format("error.archive_malformed_link_target",
                                                           arch, shown,
                                                           escape_member_name(target)));
            }
        }
    }
}

// ⚠️ **没有** `check_data_complete()`（"声明尺寸与实际数据不符"）—— 它**不可达**，已删
// （2026-10-05 实测）。头声明 4096 字节、实际只给 5 字节时，libarchive 在
// `archive_read_data_block` 上直接返回 **FATAL**（"Truncated tar archive detected while
// reading data"），`extract_tar_zst` 在到达任何比较之前就抛了 `error.extract_failed` ——
// 也就是说这个向量**已经被 libarchive 覆盖**，本模块再写一份只是第二份实现。
// 用例 `TruncatedMemberIsRejectedByLibarchive` 把这条行为钉住（它绿说明"整包拒绝"成立，
// 红的时机是 libarchive 改了行为 —— 那时才需要在这里补判据）。
