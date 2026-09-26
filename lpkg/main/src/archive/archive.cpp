#include "archive.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <memory>

#include "base/constants.hpp"
#include "base/exception.hpp"
#include "base/utils.hpp"
#include "i18n/localization.hpp"

namespace fs = std::filesystem;

/** libarchive 读取句柄的自定义删除器 */
struct ArchiveReadDeleter {
    void operator()(struct archive* a) const
    {
        if (a) {
            archive_read_close(a);
            archive_read_free(a);
        }
    }
};

/** libarchive 写入句柄的自定义删除器 */
struct ArchiveWriteDeleter {
    void operator()(struct archive* a) const
    {
        if (a) {
            archive_write_close(a);
            archive_write_free(a);
        }
    }
};

using ArchiveReadHandle = std::unique_ptr<struct archive, ArchiveReadDeleter>;
using ArchiveWriteHandle = std::unique_ptr<struct archive, ArchiveWriteDeleter>;

/// lpkg 自用的临时落位后缀（`.lpkgtmp`）。常量头只导出了 `SUFFIX_LPKG_NEW`，安装期
/// "先写临时文件再 rename"用的 `".lpkgtmp"` 在 installation_task_copy.cpp /
/// installation_task_register.cpp 里是字面量 —— 这里为守卫补一份，两边改动要一起改（守卫见
/// member_path_relative）。
static constexpr std::string_view SUFFIX_LPKG_TMP = ".lpkgtmp";

/**
 * 把成员名里的不可见字节渲染成可见形式（供异常消息用）。
 *
 * 危险名字**本身就是攻击载荷**，不能原样进消息：含 ANSI 转义的名字会再污染一份终端，
 * 含 `\n` 的名字会把异常消息**自己**切成两行 —— 错误消息被行式消费的地方（日志、CLI
 * 输出）就重演了这里正在修的同一个 bug。故一律转义后再进消息。
 */
static std::string escape_member_name(std::string_view name)
{
    static constexpr char HEX[] = "0123456789abcdef";
    std::string out;
    out.reserve(name.size());
    for (const unsigned char c : name) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c >= 0x20 && c != 0x7f) {
            out.push_back(static_cast<char>(c));
        } else {
            out += "\\x";
            out.push_back(HEX[c >> 4]);
            out.push_back(HEX[c & 0x0f]);
        }
    }
    return out;
}

/**
 * 归档成员名 → 解压根内的相对路径（去掉前导 "./" 与 "/"），**并拒绝危险成员名**。
 *
 * 成员名是**不可信输入**（未校验的 .lpkg、无校验和的上游源码包）。`fs::path` 语义下
 * `output_dir / "/etc/x"` **等于 "/etc/x"**（绝对右值丢弃左值），所以绝对路径成员会写到
 * 解压根之外：安装期解压根是 /tmp 下的临时目录、构建期是源码树，两者都会污染/覆盖宿主
 * 文件，而且这类写入不进 file_db，`query` 看不到、`remove` 删不掉（TODO.md X2）。
 *
 * 只归一化**成员名**；符号链接的**目标内容**保持原样（包内绝对链接是合法的）。
 *
 * 守卫放在本函数（而非两个调用点各自判）的理由：成员名流入文件系统的**唯一**通道就是
 * 这里 —— 成员自己的名字与硬链接的**目标名**都经本函数变成路径。`extract_file_from_archive`
 * 虽然也读名字，但它只做字符串比较、不落盘，不构成通道。
 *
 * 命中即**拒绝整个归档**（抛错，失败要响），绝不"跳过该成员继续"：归档已经表达了恶意
 * 意图，静默跳过只会让用户拿到"装了一半、某文件莫名消失"的包。
 *
 * @param archive_path 仅用于错误消息（用户要知道是哪个归档被拒）
 */
static std::string member_path_relative(const char* raw, const fs::path& archive_path)
{
    std::string_view sv(raw);
    while (true) {
        if (sv.starts_with("./")) {
            sv.remove_prefix(2);
            continue;
        }
        if (sv.starts_with('/')) {
            sv.remove_prefix(1);
            continue;
        }
        break;
    }
    const std::string member(sv);

    // ── 危险成员名守卫 ────────────────────────────────────────────────────────
    //
    // 为什么必须在这里挡：成员名会经 `scan_content_files` → file_db → 安装期 OpSink 变成
    // **WAL 行的字面内容**（`op + " " + src.string() + " → " + bak.string()`，op_sink.cpp），
    // 而 WAL 是**行式**协议 —— 写侧 `line + "\n"`（wal_append_raw），读侧 std::getline 逐行
    // 再 parse_op 分帧，两侧都不转义。于是：
    //   · 名字里的 `\n` 把一行切成两行，第二行成为**独立可解析**的 WAL 行：成员名
    //     `usr/share/x\nDIR_RM /etc 511 0 0` 造出的第二行是一条合法 DIR_RM（mode 记的是
    //     十进制，511 = 八进制 0777），而回滚侧会照行里的**绝对路径** create_directories +
    //     chmod/lchown —— 崩溃恢复/回滚就会以 root 去 chmod/chown 任意绝对路径、
    //     `--root` 隔离失效。
    //     （订正 2026-09-26：本条原先写"`reverse_execute()` 的 DIR_RM 分支**没有任何**路径
    //      confinement"—— 那是当时的实况，现在**已经有**了（两级判据，见 ARCH §9.2）。
    //     但**名字消毒仍然是必需的**：① confinement 只保证"不越出 root"，挡不住"落在 root 内
    //     的**另一个**路径"（下面那条字面 `" → "` 破坏分帧就属这类，confinement 完全挡不住）；
    //     ② 纵深防御 —— 输入端堵比让回滚侧拒绝更早、更省事。）
    //   · 名字里的字面 `" → "` 破坏箭头分帧：parse_op 把它当分界，非箭头类型的 arg1 被截断成
    //     前缀，回滚就作用到"前缀同名"的**别的路径**上。
    // 伤害都发生在**解压之后**（WAL 层无从分辨），唯一能挡的地方就是名字进入系统之前。
    // key 取 const char*（而非 string_view）：string_format 收 `const std::string&`，而
    // string_view→string 的构造是 explicit 的，传 string_view 编不过。
    const auto reject = [&](const char* key) {
        throw LpkgException(string_format(key, archive_path.string(), escape_member_name(member)));
    };
    // `\0` 到不了这里（archive_entry_pathname 返回 C 串，内部 NUL 会把它截断），留着是
    // 兜底：判据写全，将来若换成按长度取名的 API 也照样成立。
    for (const char c : member)
        if (c == '\n' || c == '\r' || c == '\0') reject("error.unsafe_member_control");
    if (member.find(" \xe2\x86\x92 ") != std::string::npos) reject("error.unsafe_member_arrow");

    // `.lpkgtmp` / `.lpkgnew` 是 lpkg 自用的命名空间：安装期"先写 `<dst>.lpkgtmp` 再 rename
    // 到位"与"配置冲突时落 `<dst>.lpkgnew` 给用户审阅"都用它们。包声明同名成员会与那些落位
    // 撞名：`<dst>.lpkgtmp → <dst>` 的 rename 会盖掉包里的成员（反之亦然），而 `.lpkgnew`
    // 更是把"待用户审阅的配置"直接变成包内容。合法包里不该出现这两个名字。
    // 目录条目可能带尾斜杠（`x.lpkgnew/`）→ 先剥掉再取末段。
    std::string_view last = member;
    while (last.ends_with('/')) last.remove_suffix(1);
    if (const auto slash = last.rfind('/'); slash != std::string_view::npos)
        last = last.substr(slash + 1);
    if (last.ends_with(constants::SUFFIX_LPKG_NEW) || last.ends_with(SUFFIX_LPKG_TMP))
        reject("error.unsafe_member_suffix");

    return member;
}

/**
 * 解压 tar.zst 归档文件到目标目录
 * 包含安全检查：路径穿越防护、符号链接权限修复、硬链接/软链接目标重映射
 * 每解压 100 个文件输出一次进度；进度与完成日志都点名 label（多包批次里分得清在解压谁）
 */
void extract_tar_zst(const fs::path& archive_path, const fs::path& output_dir,
                     const std::string& label)
{
    ArchiveReadHandle a(archive_read_new());
    archive_read_support_filter_all(a.get());
    archive_read_support_format_all(a.get());

    ArchiveWriteHandle ext(archive_write_disk_new());
    archive_write_disk_set_options(
        ext.get(), ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_OWNER |
                       ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_XATTR | ARCHIVE_EXTRACT_FFLAGS |
                       ARCHIVE_EXTRACT_SECURE_SYMLINKS | ARCHIVE_EXTRACT_SECURE_NODOTDOT |
                       ARCHIVE_EXTRACT_UNLINK);
    // 注：这里**不能**加 ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS —— 本实现是先把
    // "解压根 + 成员名"的绝对路径写回 entry 再交给 disk writer，该选项会因此拒绝
    // 每一个成员（实测 "Path is absolute"）。绝对路径的防护由下面的 member_path_relative
    // 归一化承担：成员名先变成相对路径再拼根，绝无逃出 output_dir 的可能。
    // 已移除：archive_write_disk_set_standard_lookup(ext.get());
    // 该函数在静态链接的 chroot 环境下因 NSS 问题可能导致段错误

    if (archive_read_open_filename(a.get(), archive_path.c_str(), constants::ARCHIVE_BUFFER_SIZE) !=
        ARCHIVE_OK) {
        const char* err = archive_error_string(a.get());
        throw LpkgException(string_format("error.extract_failed", archive_path.string()) + ": " +
                            (err ? err : get_string("error.unknown")));
    }

    struct archive_entry* entry;
    int r = ARCHIVE_OK;
    long long count = 0;
    while (true) {
        r = archive_read_next_header(a.get(), &entry);
        if (r == ARCHIVE_EOF) break;
        if (r < ARCHIVE_OK) {
            if (r < ARCHIVE_WARN) {
                const char* err = archive_error_string(a.get());
                throw LpkgException(string_format("error.extract_failed", archive_path.string()) +
                                    ": " + (err ? err : get_string("error.fatal_read")));
            }
            log_warning(archive_error_string(a.get()));
        }

        const char* current_path = archive_entry_pathname(entry);
        if (!current_path) continue;

        // 成员名归一化为相对路径后再拼解压根（`..` 由 SECURE_NODOTDOT 兜底），
        // 保证任何成员都落在 output_dir 之内；危险名字（控制字符 / 字面 " → " / lpkg 自用
        // 后缀）在这里**整包拒绝** —— 见 member_path_relative 的说明。
        const std::string member = member_path_relative(current_path, archive_path);
        if (member.empty()) continue;  // "." 之类不产生文件的成员
        fs::path dest_path = output_dir / member;
        archive_entry_set_pathname(entry, dest_path.c_str());

        // 修复：Linux 没有 lchmod，libarchive 可能会对符号链接使用 chmod，
        // 这会导致跟随链接并破坏目标文件的权限（例如 sudo 变为 777）
        if (archive_entry_filetype(entry) == AE_IFLNK) {
            archive_entry_set_perm(entry, 0);
        }

        // 硬链接：目标必须落在解压根内 —— 否则一个成员就能给任意已有文件
        // （如 /etc/shadow）起别名，随后被当作包内容复制进系统。
        // 判据用**原始目标**：绝对路径（指向根外）或 `..` 上溯出根 → 跳过该成员；
        // 根内的相对目标按"归一化后的绝对路径"重写（libarchive 需要绝对路径）。
        const char* hardlink = archive_entry_hardlink(entry);
        if (hardlink) {
            const fs::path root_n = output_dir.lexically_normal();
            const fs::path hl_norm =
                (root_n / member_path_relative(hardlink, archive_path)).lexically_normal();
            if (fs::path(hardlink).is_absolute() || !path_within(hl_norm, root_n)) {
                log_warning(string_format("warning.archive_unsafe_member", hardlink));
                continue;
            }
            archive_entry_set_hardlink(entry, hl_norm.c_str());
        }

        r = archive_write_header(ext.get(), entry);
        if (r < ARCHIVE_OK) {
            if (r < ARCHIVE_WARN) {
                const char* err = archive_error_string(ext.get());
                throw LpkgException(string_format("error.extract_failed", archive_path.string()) +
                                    ": " + (err ? err : get_string("error.fatal_write")));
            }
            log_warning(archive_error_string(ext.get()));
        } else {
            const void* buff;
            size_t size;
            la_int64_t offset;
            while (true) {
                r = archive_read_data_block(a.get(), &buff, &size, &offset);
                if (r == ARCHIVE_EOF) break;
                if (r < ARCHIVE_OK) {
                    if (r < ARCHIVE_WARN) {
                        const char* err = archive_error_string(a.get());
                        throw LpkgException(
                            string_format("error.extract_failed", archive_path.string()) + ": " +
                            (err ? err : get_string("error.data_block_read")));
                    }
                    log_warning(archive_error_string(a.get()));
                    break;
                }

                if (archive_write_data_block(ext.get(), buff, size, offset) < ARCHIVE_OK) {
                    const char* err = archive_error_string(ext.get());
                    throw LpkgException(
                        string_format("error.extract_failed", archive_path.string()) + ": " +
                        (err ? err : get_string("error.data_block_write")));
                }
            }
            archive_write_finish_entry(ext.get());
        }

        if (++count % constants::PROGRESS_INTERVAL_FILES == 0) {
            log_info(string_format("info.extracting", label, count));
        }
    }

    log_info(string_format("info.extract_complete", label, count));
}

/**
 * 从归档文件中提取指定路径的内部文件内容并返回字符串
 * 自动去除路径前缀 "./"，当文件不存在时返回空字符串
 */
std::string extract_file_from_archive(const fs::path& archive_path,
                                      const std::string& internal_path)
{
    ArchiveReadHandle a(archive_read_new());
    archive_read_support_filter_all(a.get());
    archive_read_support_format_all(a.get());

    if (archive_read_open_filename(a.get(), archive_path.c_str(), constants::ARCHIVE_BUFFER_SIZE) !=
        ARCHIVE_OK) {
        throw LpkgException(string_format("error.open_file_failed", archive_path.string()) + ": " +
                            archive_error_string(a.get()));
    }

    struct archive_entry* entry;
    while (archive_read_next_header(a.get(), &entry) == ARCHIVE_OK) {
        std::string path = archive_entry_pathname(entry);
        // 去除开头的 ./ 前缀
        if (path.starts_with(constants::CURRENT_DIR_PREFIX))
            path = path.substr(constants::CURRENT_DIR_PREFIX.length());

        if (path == internal_path) {
            // 归档自报大小是**不可信输入**（GNU base-256 头可以声明 1 TiB）：
            // 无上限的 resize 会 bad_alloc/abort 掉整个进程，而调用方只承诺抛
            // LpkgException。要读的只有 metadata.json（几 KB），超过上限即畸形归档。
            constexpr la_int64_t kMaxMemberSize = 16 * 1024 * 1024;
            const la_int64_t declared = archive_entry_size(entry);
            if (declared < 0 || declared > kMaxMemberSize) {
                throw LpkgException(string_format("error.archive_member_too_large",
                                                  std::to_string(declared), archive_path.string()));
            }
            size_t size = static_cast<size_t>(declared);
            std::string content;
            content.resize(size);
            ssize_t bytes_read = archive_read_data(a.get(), content.data(), size);
            // archive_read_data 可能返回少于 size 的字节（截断/损坏归档）
            if (bytes_read >= 0 && static_cast<size_t>(bytes_read) < size)
                content.resize(static_cast<size_t>(bytes_read));
            else if (bytes_read < 0)
                content.clear();
            return content;
        }
        archive_read_data_skip(a.get());
    }

    return "";
}
