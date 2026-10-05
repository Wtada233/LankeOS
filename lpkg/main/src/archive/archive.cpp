#include "archive.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cstdint>
#include <memory>

#include "archive/tar_guard.hpp"
#include "base/constants.hpp"
#include "base/exception.hpp"
#include "base/utils.hpp"
#include "i18n/localization.hpp"
#include "ui/term.hpp"

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

/// lpkg 自用的临时落位后缀（`.lpkgtmp`）。2026-10-03 起它是 `constants::SUFFIX_LPKG_TMP`
/// —— 此前这里有一份局部常量、另外两处是裸字面量（同一件事三处表达），现已收敛到常量头。

// 渲染可见形式的成员名（`\xHH`）。判据与理由见 `archive.hpp` 的声明 —— 2026-10-05 起
// 对外可见，`tar_guard.cpp` 共用同一份（别在那边再写一遍）。
std::string escape_member_name(std::string_view name)
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

std::string member_name_rejection_message(std::string_view member,
                                          const std::string& container_path)
{
    // `\0` 到不了这里（archive_entry_pathname 返回 C 串，内部 NUL 会把它截断），留着是
    // 兜底：判据写全，将来若换成按长度取名的 API 也照样成立。
    // `\t` 也必须拒：它不伪造 WAL 行（WAL 是空格分帧），却会破坏**制表符分帧**的归属数据库
    // （files.db / provides.db 写成 `key\tvalues\n`、按**第一个** TAB 读回，见 cache.cpp）——
    // 一个带 TAB 的成员名会让键在重载时被截断、属主串错位（误报共享文件 / 假孤儿）。
    const char* key = nullptr;
    for (const char c : member)
        if (c == '\n' || c == '\r' || c == '\0' || c == '\t') {
            key = "error.unsafe_member_control";
            break;
        }
    if (!key && member.find(" \xe2\x86\x92 ") != std::string_view::npos)
        key = "error.unsafe_member_arrow";

    // lpkg 自用文件名命名空间（`.lpkgtmp` / `.lpkgnew` / `.lpkgsave` / `.lpkg_bak_` …）：
    // 包声明同名成员会与 lpkg 自己的 rename/落位撞名（逐条后果见
    // `constants::RESERVED_MEMBER_NAMES` 的说明）。合法包里不该出现这些名字。
    //
    // **清单只有一张**：`constants::RESERVED_MEMBER_NAMES` —— 本循环遍历它，不在这里另列一遍。
    // 2026-10-03 的缺陷正是"常量头里有 `SUFFIX_LPKG_BAK`、这里只硬编码了另外三个"，而且那个
    // token 是**前缀**形态（`.lpkg_bak_<pkg>_<pid>`），`ends_with` 结构上就抓不到它。
    //
    // 判据：**任一路径分量**命中即拒（不是只看末段）。
    // 只看末段会漏掉 `content/usr/bin/bash.lpkgtmp/x` 这类成员 —— 它的**末段是 `x`**，
    // 而 libarchive 的 disk writer 会为文件成员**自动补建缺失的父目录**（本仓库已实测），
    // 于是包里合法地装出一个目录 `usr/bin/bash.lpkgtmp/`：此后别的包安装 `bash` 时，
    // `<dst>.lpkgtmp` 的落位路径正撞上它 —— 全新安装 `rename(目录 → 不存在路径)` 成功
    // （`usr/bin/bash` 变成**目录**而 lpkg 报成功）；升级 `rename(目录 → 已存在文件)` =
    // ENOTDIR（整批回滚、报错定位不到真因）。冲突预检拦不住（它不比对 `<目标>.lpkgtmp`），
    // 故必须在名字进系统之前按**分量**拒绝；
    // `.lpkg_bak_` 同理且更隐蔽：`content/.lpkg_bak_x_999999/payload` 装出来的顶层目录会被
    // `cleanup_orphan_stashes()`（按该前缀 + pid 已死）在**下一次任意 lpkg 命令**里静默删掉。
    //
    // 目录条目可能带尾斜杠（`x.lpkgnew/`）：按 `/` 切分量、空分量跳过，尾斜杠形态自然覆盖。
    if (!key) {
        std::string_view rest = member;
        while (!rest.empty()) {
            const auto slash = rest.find('/');
            const std::string_view component =
                slash == std::string_view::npos ? rest : rest.substr(0, slash);
            for (const auto& reserved : constants::RESERVED_MEMBER_NAMES) {
                // 空分量（前导/连续/尾随 `/`）两个判据都必为 false，不必显式跳过。
                const bool hit = reserved.starts_with ? component.starts_with(reserved.token)
                                                      : component.ends_with(reserved.token);
                if (hit) {
                    key = "error.unsafe_member_suffix";
                    break;
                }
            }
            if (key) break;
            if (slash == std::string_view::npos) break;
            rest.remove_prefix(slash + 1);
        }
    }

    if (!key) return {};
    // key 取 const char*（而非 string_view）：string_format 收 `const std::string&`，而
    // string_view→string 的构造是 explicit 的，传 string_view 编不过。
    return string_format(key, container_path, escape_member_name(member));
}

/**
 * 归档成员名 → 解压根内的相对路径（去掉前导 "./" 与 "/"），**并拒绝危险成员名**。
 *
 * 成员名是**不可信输入**（未校验的 .lpkg、无校验和的上游源码包）。`fs::path` 语义下
 * `output_dir / "/etc/x"` **等于 "/etc/x"**（绝对右值丢弃左值），所以绝对路径成员会写到
 * 解压根之外：安装期解压根是 /tmp 下的临时目录、构建期是源码树，两者都会污染/覆盖宿主
 * 文件，而且这类写入不进 file_db，`query` 看不到、`remove` 删不掉（历史 TODO.md X2）。
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
    // 判据本体在 `member_name_rejection_message`（打包侧共用同一份；见 archive.hpp）。
    // `UnsafeArchiveException`（不是裸 LpkgException）：构建期自动解压对普通失败容忍，
    // 但对**安全拒绝**必须放行到上层（见 base/exception.hpp）。
    if (const std::string why = member_name_rejection_message(member, archive_path.string());
        !why.empty())
        throw UnsafeArchiveException(why);

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
    // 进度行：左 = `Extracting <label>`，右 = 进度条（TTY）/ `100%`（非 TTY）。
    // 度量取**归档文件已消费字节**（`archive_filter_bytes`）—— tar 是流式格式，成员总数
    // 事先不可知，而"读到文件的第几个字节"是唯一可靠且单调的进度。
    ui::Line line(string_format("ui.extract", label));
    std::error_code size_ec;
    const std::uintmax_t size_raw = fs::file_size(archive_path, size_ec);
    // file_size 失败时返回 UINT64_MAX —— 直接进下面的 step/百分比运算会算出天文数字
    // （step 巨大 + 百分比错乱）。失败即"总量未知"：置 0，与下方既有的 `total_bytes == 0`
    // 分支同一路径（step=1、report_progress 立即返回，不报百分比）。
    const std::uint64_t total_bytes = size_ec ? 0 : static_cast<std::uint64_t>(size_raw);
    std::uint64_t reported = 0;
    // 节流：每 1%（或 1 MiB，取小者）才重绘一次 → 最多 ~100 次/包，与文件数无关。
    const std::uint64_t step =
        total_bytes > 0
            ? std::max<std::uint64_t>(1, std::min<std::uint64_t>(total_bytes / 100, 1u << 20))
            : 1;
    // 每读到一个成员就把"已消费字节"换成进度（节流后）。
    const auto report_progress = [&]() {
        if (total_bytes == 0) return;
        const la_int64_t consumed = archive_filter_bytes(a.get(), -1);
        if (consumed < 0) return;
        const auto done = static_cast<std::uint64_t>(consumed);
        if (done < reported + step && done < total_bytes) return;
        reported = done;
        line.progress(100.0 * static_cast<double>(done) / static_cast<double>(total_bytes),
                      ui::human_size(done) + " / " + ui::human_size(total_bytes));
    };
    // 畸形/敌意 tar 的结构性守卫（重名成员、`..`、链接目标、控制字符、反斜杠、超长路径、
    // 类型与尾斜杠不一致、尺寸说谎）。**跨成员有状态**，故在循环外建一次。
    // 判据本体与"哪些通用加固建议不采纳"的完整清单见 `archive/tar_guard.hpp`。
    TarGuard guard;

    while (true) {
        r = archive_read_next_header(a.get(), &entry);
        if (r == ARCHIVE_EOF) break;
        if (r < ARCHIVE_OK) {
            if (r < ARCHIVE_WARN) {
                const char* err = archive_error_string(a.get());
                throw LpkgException(string_format("error.extract_failed", archive_path.string()) +
                                    ": " + (err ? err : get_string("error.fatal_read")));
            }
            // 可恢复警告：`archive_error_string` 返回的可能为 NULL（无错误串时），
            // 直接喂给 string_view 会构造出空指针视图（UB）——统一兜底。
            const char* warn = archive_error_string(a.get());
            log_warning(warn ? warn : get_string("error.unknown"));
        }

        const char* current_path = archive_entry_pathname(entry);
        if (!current_path) continue;

        // 成员名归一化为相对路径后再拼解压根（`..` 由 SECURE_NODOTDOT 兜底），
        // 保证任何成员都落在 output_dir 之内；危险名字（控制字符 / 字面 " → " / lpkg 自用
        // 后缀）在这里**整包拒绝** —— 见 member_path_relative 的说明。
        const std::string member = member_path_relative(current_path, archive_path);
        if (member.empty()) continue;  // "." 之类不产生文件的成员
        // 畸形 tar 守卫（整包拒绝）。放在 `member.empty()` 之后：`.`
        // 这类归一化成空、本来就不落盘的成员不该进重名判据。
        guard.check(entry, current_path, member, archive_path);
        fs::path dest_path = output_dir / member;
        archive_entry_set_pathname(entry, dest_path.c_str());

        // 修复：Linux 没有 lchmod，libarchive 可能会对符号链接使用 chmod，
        // 这会导致跟随链接并破坏目标文件的权限（例如 sudo 变为 777）
        if (archive_entry_filetype(entry) == AE_IFLNK) {
            archive_entry_set_perm(entry, 0);
        }

        // ── 特殊文件成员守卫 ─────────────────────────────────────────────────────
        //
        // FIFO / 字符设备 / 块设备 / socket 一律**整包拒绝**（抛错），不能照原样解出来：
        //   · **FIFO**：安装期 `calculate_sha256`（crypto/hash.cpp，用 std::ifstream）打开它会在
        //     首次 `read()` 上**永久阻塞**（无 O_NONBLOCK；stdio 也不轮询 SIGINT，Ctrl+C 被吞）
        //     —— 整个安装挂死，只能 kill -9。
        //   · **字符/块设备**：major/minor 可指向 `/dev/zero` 之类，随后的 `fs::copy`
        //     （pkg/installation_task_copy.cpp）会**无限读直到写满磁盘**。
        //   · **socket**：既非普通文件也非目录，同样不被任何下游逻辑预期。
        //     ⚠️ **经 tar 到不了这条**（2026-10-03 实测）：tar/pax 的类型标志位里没有 socket
        //     （'0'..'6' 分别是普通/硬链接/符号链接/字符/块/目录/FIFO），libarchive 写出的
        //     socket 成员读回来也不是 `AE_IFSOCK`（解压**成功**）。所以这个 `AE_IFSOCK` 判断是
        //     **纵深防御**，**没有对应用例 —— 这是有意的，不是漏测**；别去补一个造不出来的
        //     用例（见 lpkg/CLAUDE.md §7.4：不为走不到的分支写断言）。将来若支持别的归档格式，
        //     它自然生效。
        //
        // 用**黑名单**（只拒这四种），**不要**用白名单：本函数同时服务**源码 tarball** 解压
        // （构建期解压上游归档），有些 tar 变体/未来格式的 `filetype` 可能是 0 或本代码不认的
        // 值 —— 白名单会把它们一起拒掉、误伤合法源码包。黑名单只挡已知危险形态。
        const auto ftype = archive_entry_filetype(entry);
        if (ftype == AE_IFIFO || ftype == AE_IFCHR || ftype == AE_IFBLK || ftype == AE_IFSOCK) {
            throw LpkgException(
                string_format("error.archive_unsupported_filetype", archive_path.string(), member));
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
                // 2026-10-05：粒度由「跳过该成员 + 告警」升为**整包拒绝** —— 与成员名守卫、
                // 特殊文件类型守卫、以及 `tar_guard` 的全部判据同一粒度。留给"跳过"的口子
                // 等于让用户拿到一个装了一半、某文件莫名消失的包，而归档的畸形是**结构性**
                // 的、不是单个成员的偶发问题（见 archive/tar_guard.hpp 抬头）。
                // 归一化仍走 `member_path_relative`（那是"成员名 → 根内相对路径"的唯一实现），
                // 所以这条判断留在调用点、不搬进 guard：guard 拿不到归一化函数。
                throw UnsafeArchiveException(
                    string_format("error.archive_malformed_link_target", archive_path.string(),
                                  escape_member_name(member), escape_member_name(hardlink)));
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
            const char* warn = archive_error_string(ext.get());
            log_warning(warn ? warn : get_string("error.unknown"));
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
                    const char* warn = archive_error_string(a.get());
                    log_warning(warn ? warn : get_string("error.unknown"));
                    break;
                }

                if (archive_write_data_block(ext.get(), buff, size, offset) < ARCHIVE_OK) {
                    const char* err = archive_error_string(ext.get());
                    throw LpkgException(
                        string_format("error.extract_failed", archive_path.string()) + ": " +
                        (err ? err : get_string("error.data_block_write")));
                }
            }
            // ⚠️ 这里**没有**"声明尺寸 vs 实际字节数"的复核 —— 它不可达，已删（2026-10-05
            // 实测）：头声明 4096、实际只给 5 时，libarchive 在 `archive_read_data_block`
            // 上直接返 FATAL（"Truncated tar archive detected"），上面那个 `r < ARCHIVE_WARN`
            // 分支先一步抛了。即该向量已由 libarchive 覆盖，再判一次就是第二份实现。
            // finish_entry 的返回值此前被丢弃：它可能因磁盘满/EIO 失败（写盘失败），
            // 而这里若静默吞掉，解压会报成功、盘上却是截断的内容（2026-10-02 修）。
            const int fe = archive_write_finish_entry(ext.get());
            if (fe < ARCHIVE_WARN) {
                const char* err = archive_error_string(ext.get());
                throw LpkgException(string_format("error.extract_failed", archive_path.string()) +
                                    ": " + (err ? err : get_string("error.data_block_write")));
            }
            if (fe < ARCHIVE_OK) {
                const char* warn = archive_error_string(ext.get());
                log_warning(warn ? warn : get_string("error.unknown"));
            }
        }

        report_progress();
    }

    line.finish_progress();
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
        const char* err = archive_error_string(a.get());
        throw LpkgException(string_format("error.open_file_failed", archive_path.string()) + ": " +
                            (err ? err : get_string("error.unknown")));
    }

    struct archive_entry* entry;
    // 逐成员读到 EOF，**不要**用 `== ARCHIVE_OK` 作循环条件：那会在第一个 WARN/FATAL 处
    // 静默停下，于是"某个成员读不了"被当成"没有这个成员"、返回空串（调用方把空串读作
    // "metadata 缺失"，报的却是另一回事）。这里：真读错误（< WARN）抛；可恢复警告记一条
    // 后继续找目标成员（2026-10-02 修）。
    while (true) {
        const int r = archive_read_next_header(a.get(), &entry);
        if (r == ARCHIVE_EOF) break;
        if (r < ARCHIVE_WARN) {
            const char* err = archive_error_string(a.get());
            throw LpkgException(string_format("error.open_file_failed", archive_path.string()) +
                                ": " + (err ? err : get_string("error.fatal_read")));
        }
        if (r < ARCHIVE_OK) {
            const char* warn = archive_error_string(a.get());
            log_warning(warn ? warn : get_string("error.unknown"));
        }

        // `archive_entry_pathname` 可能返回 NULL（畸形/无名字的成员）—— 直接拿来构造
        // std::string 是 UB。与 extract_tar_zst 里的同名判空保持一致，跳过该成员。
        const char* raw_path = archive_entry_pathname(entry);
        if (!raw_path) continue;
        std::string path = raw_path;
        // 去除开头的 ./ 前缀
        if (path.starts_with(constants::CURRENT_DIR_PREFIX))
            path = path.substr(constants::CURRENT_DIR_PREFIX.length());

        if (path == internal_path) {
            // 归档自报大小是**不可信输入**（GNU base-256 头可以声明 1 TiB）：
            // 无上限的 resize 会 bad_alloc/abort 掉整个进程，而调用方只承诺抛
            // LpkgException。要读的只有 metadata.json（几 KB），超过上限即畸形归档。
            // 上限是 `constants::ARCHIVE_MEMBER_MAX_SIZE`——与 `read_package_metadata`
            // 共用同一个常量（别在这里另写一份魔数）。
            const la_int64_t declared = archive_entry_size(entry);
            if (declared < 0 ||
                declared > static_cast<la_int64_t>(constants::ARCHIVE_MEMBER_MAX_SIZE)) {
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
