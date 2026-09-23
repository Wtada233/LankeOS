#include "wal_op.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "base/constants.hpp"
#include "base/exception.hpp"
#include "base/utils.hpp"
#include "cache.hpp"
#include "config/config.hpp"
#include "i18n/localization.hpp"

namespace fs = std::filesystem;

namespace wal
{

// ============================================================================
// 类型名映射
// ============================================================================

static constexpr std::pair<std::string_view, WALOpType> TYPE_MAP[] = {
    {"BEGIN_PKGS", WALOpType::BEGIN_PKGS},
    {"COMMIT_PKGS", WALOpType::COMMIT_PKGS},
    {"BEGIN", WALOpType::BEGIN},
    {"COMMIT", WALOpType::COMMIT},
    {"ROLLBACK", WALOpType::ROLLBACK},
    {"END", WALOpType::END},
    {"BACKUP", WALOpType::BACKUP},
    {"NEW", WALOpType::NEW},
    {"NEW_DIR", WALOpType::NEW_DIR},
    {"COPY", WALOpType::COPY},
    {"REMOVE_OLD", WALOpType::REMOVE_OLD},
    {"DIR_RM", WALOpType::DIR_RM},
    {"RM_BEGIN", WALOpType::RM_BEGIN},
    {"RM_COMMIT", WALOpType::RM_COMMIT},
    {"RM_END", WALOpType::RM_END},
    {"CLEANUP", WALOpType::CLEANUP},
    {"DB", WALOpType::DB},
    {"DBNEW", WALOpType::DBNEW},
    {"DBRM", WALOpType::DBRM},
    {"RESTORE_FILE", WALOpType::RESTORE_FILE},
    {"RESTORE_DB", WALOpType::RESTORE_DB},
    {"RESTORE_DIR", WALOpType::RESTORE_DIR},
    {"RESTORE_FILE_RM", WALOpType::RESTORE_FILE_RM},
    {"RESTORE_DIR_RM", WALOpType::RESTORE_DIR_RM},
    {"RESTORE_DB_RM", WALOpType::RESTORE_DB_RM},
    {"REMOVE_FILE", WALOpType::REMOVE_FILE},  // 旧名称，向后兼容解析
    {"REMOVE_DIR", WALOpType::REMOVE_DIR},    // 旧名称，向后兼容解析
};

std::string_view walop_type_name(WALOpType t)
{
    for (const auto& [name, type] : TYPE_MAP)
        if (type == t) return name;
    return "UNKNOWN";
}

WALOpType walop_type_from_name(std::string_view name)
{
    for (const auto& [n, type] : TYPE_MAP)
        if (n == name) return type;
    throw LpkgException(std::string("Unknown WAL op type: ") + std::string(name));
}

// ============================================================================
// WAL 行解析
// ============================================================================

// 解析格式:
//   TYPE arg1 [arg2 [arg3 [arg4 [arg5 [arg6]]]]]
//
// **分帧规则**（路径可以含空格，所以不能无脑按空格切——见 TODO.md A1）：
//   - 箭头形式（BACKUP/COPY/REMOVE_OLD/RESTORE_FILE/RESTORE_DB）以 " → " 为界，
//     两侧整段各自成一个参数：TYPE <src> → <dst>
//   - 其余形式用 tail_args() 给出"arg1 之后还有几个固定字段"，那些字段**从右往左**切，
//     剩下的整段归 arg1：
//       TYPE <路径可含空格> <milestone> (DB/DBNEW/DBRM/BEGIN/COMMIT/ROLLBACK/END/RM_*，tail=1) TYPE
//       <路径可含空格> <mode> <uid> <gid> (DIR_RM，tail=3) TYPE <路径可含空格>
//       (NEW/NEW_DIR/CLEANUP/RESTORE_*_RM，tail=0)
//   尾字段都是版本号/里程碑/数字元数据，不可能含空格，故从右侧锚定是安全的。
//   历史 WAL 行的字段内没有空格，"整段归 arg1" 退化成旧的逐空格切分 → 完全向后兼容。
//   （残留：路径含换行会破行、含字面 " → " 会破箭头分帧——都是文件系统允许但现实中
//     不会出现的名字。）

/// arg1 之后固定字段的个数（从右往左数）
static int tail_args(WALOpType t)
{
    switch (t) {
        case WALOpType::DIR_RM:
            return 3;  // <mode> <uid> <gid>
        case WALOpType::DB:
        case WALOpType::DBNEW:
        case WALOpType::DBRM:
        case WALOpType::BEGIN:
        case WALOpType::COMMIT:
        case WALOpType::ROLLBACK:
        case WALOpType::END:
        case WALOpType::RM_BEGIN:
        case WALOpType::RM_COMMIT:
        case WALOpType::RM_END:
            return 1;  // <milestone> / <ver>
        default:
            return 0;  // 单参数/无参数：arg1 取整段剩余
    }
}

WALOp parse_op(const std::string& line)
{
    WALOp op;  // type 默认 INVALID：未成功解析的行不会冒充真实操作
    op.raw = line;

    // 先切出类型 token，其余整段交给下面的分帧规则
    std::string_view rest = line;
    const auto space = rest.find(' ');
    const std::string_view type_sv =
        (space == std::string_view::npos) ? rest : rest.substr(0, space);
    rest = (space == std::string_view::npos) ? std::string_view{} : rest.substr(space + 1);

    try {
        op.type = walop_type_from_name(type_sv);
    } catch (const LpkgException&) {
        // 未知类型（损坏/半写/未来格式）：保持 INVALID 并记警告。**不能借用任何真实类型
        // 当哨兵**——破损行会被"找最后一个 BEGIN_PKGS"的反向扫描当成批次起点。
        log_warning(string_format("warning.wal_invalid_line", line));
        return op;
    }

    // 箭头形式：两段各以 " → " 为界（两侧都可含空格）
    const auto arrow = rest.find(" \xe2\x86\x92 ");
    if (arrow != std::string_view::npos) {
        op.arg1 = std::string(rest.substr(0, arrow));
        op.arg2 = std::string(rest.substr(arrow + 5));  // skip " → " (3 bytes + 2 spaces)
        return op;
    }

    // 尾部固定字段从右往左切，剩余整段归 arg1
    const int tail = tail_args(op.type);
    std::string tail_vals[3];  // tail_args() 返回 0..3
    std::string_view head = rest;
    for (int i = 0; i < tail; ++i) {
        const auto sp = head.rfind(' ');
        if (sp == std::string_view::npos) {
            // 字段数不够（畸形/历史短行）→ 整段归 arg1，与旧行为一致
            op.arg1 = std::string(rest);
            return op;
        }
        tail_vals[tail - 1 - i] = std::string(head.substr(sp + 1));
        head.remove_suffix(head.size() - sp);
    }
    while (!head.empty() && head.back() == ' ') head.remove_suffix(1);
    op.arg1 = std::string(head);
    if (tail > 0) op.arg2 = tail_vals[0];
    if (tail > 1) op.arg3 = tail_vals[1];
    if (tail > 2) op.arg4 = tail_vals[2];

    return op;
}

// ============================================================================
// checkpoint: DB 条目是否表示 batch-start 最终状态
// ============================================================================

static bool is_batch_start_milestone(const WALOp& op)
{
    if (op.type != WALOpType::DB && op.type != WALOpType::DBNEW && op.type != WALOpType::DBRM)
        return false;
    DbMilestone m = DbMilestone::from_string(op.arg2);
    return m.is_batch_start();
}

// ============================================================================
// reverse_execute — 逆向执行引擎
// ============================================================================

/// 向 WAL 日志追加一行并 fsync（用于回滚审计行和批次标记）
/// 失败时抛 LpkgException——WAL 不可写意味着系统状态无法保证
static void wal_append_raw(const std::string& line)
{
    std::string path = wal_log_path();
    int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) throw LpkgException(string_format("error.wal_open_failed", path));

    std::string l = line + "\n";
    ssize_t written = ::write(fd, l.data(), l.size());
    if (written < 0 || static_cast<size_t>(written) != l.size()) {
        ::close(fd);
        throw LpkgException(string_format("error.wal_write_failed", path));
    }

    if (::fsync(fd) != 0) {
        ::close(fd);
        throw LpkgException(string_format("error.wal_fsync_failed", path));
    }

    ::close(fd);
}

/// 获取 .lpkg_db_bak 备份文件的路径
static std::string db_bak_path(const std::string& db_path, const std::string& milestone)
{
    return db_path + ".lpkg_db_bak_before:" + milestone;
}

/// 安全删除文件
static bool safe_remove(const fs::path& p)
{
    std::error_code ec;
    return fs::remove(p, ec);
}

RollbackStats reverse_execute(const std::vector<WALOp>& ops, bool write_audit)
{
    RollbackStats stats;

    // 逆序遍历
    for (int i = static_cast<int>(ops.size()) - 1; i >= 0; --i) {
        const auto& op = ops[i];

        // 跳过未解析行（skip_in_reverse 已含 INVALID）与元数据/RESTORE 审计行
        if (op.skip_in_reverse()) continue;

        // 跳过 :batch-start DB 条目（最终状态标记）
        if (is_batch_start_milestone(op)) continue;

        switch (op.type) {
            // ── BACKUP / REMOVE_OLD ──────────────────────────────────────────
            case WALOpType::BACKUP:
            case WALOpType::REMOVE_OLD: {
                // arg1 = src (原始路径), arg2 = dst (.lpkg_bak 路径)
                fs::path bak_path = op.arg2;
                fs::path orig_path = op.arg1;

                // fs::exists 对 dangling symlink 返回 false，必须用 is_symlink 补检
                if (fs::exists(bak_path) || fs::is_symlink(bak_path)) {
                    ::safe_rename(bak_path, orig_path);
                    stats.files_restored++;

                    if (write_audit) {
                        wal_append_raw("RESTORE_FILE " + op.arg2 + " \xe2\x86\x92 " + op.arg1);
                    }
                }
                // bak 不存在 → 已被消费，跳过（幂等）
                break;
            }

            // ── DIR_RM（删除空目录；回滚按元数据重建）───────────────────────
            case WALOpType::DIR_RM: {
                // arg1 = 目录路径, arg2 = mode(十进制), arg3 = uid, arg4 = gid
                fs::path p = op.arg1;
                if (p.empty()) break;
                std::error_code ec;
                if (!(fs::exists(p, ec) || fs::is_symlink(p))) {
                    fs::create_directories(p, ec);  // 逆序保证父目录已重建
                }
                if (!ec && fs::is_directory(p) && !fs::is_symlink(p)) {
                    uid_t uid = static_cast<uid_t>(-1);
                    gid_t gid = static_cast<gid_t>(-1);
                    mode_t mode = static_cast<mode_t>(-1);
                    try {
                        if (!op.arg2.empty())
                            mode = static_cast<mode_t>(std::stoul(op.arg2)) & 07777;
                        if (!op.arg3.empty()) uid = static_cast<uid_t>(std::stoul(op.arg3));
                        if (!op.arg4.empty()) gid = static_cast<gid_t>(std::stoul(op.arg4));
                    } catch (const std::exception&) {
                    }
                    if (uid != static_cast<uid_t>(-1) && gid != static_cast<gid_t>(-1))
                        (void)::lchown(p.c_str(), uid, gid);
                    if (mode != static_cast<mode_t>(-1)) (void)::chmod(p.c_str(), mode);
                    stats.dirs_recreated++;
                    if (write_audit) {
                        wal_append_raw("RESTORE_DIR " + p.string());
                    }
                }
                break;
            }

            // ── COPY ─────────────────────────────────────────────────────────
            case WALOpType::COPY: {
                // arg2 = dst（目标文件路径）
                // 逆向：删除目标文件（含 dangling symlink）
                fs::path dst = op.arg2;
                if (fs::exists(dst) || fs::is_symlink(dst)) {
                    safe_remove(dst);
                    stats.files_cleaned++;

                    if (write_audit) {
                        // 逆操作: 删除被 COPY 的目标文件（无备份可恢复）
                        wal_append_raw("RESTORE_FILE_RM " + op.arg2);
                    }
                }
                // 清理中断安装残留的 .lpkgtmp（COPY 只删 dst，arg1 的临时文件仍可能残留）
                const fs::path tmp = op.arg1;
                if (fs::exists(tmp) || fs::is_symlink(tmp)) {
                    safe_remove(tmp);
                    stats.files_cleaned++;
                }
                break;
            }

            // ── NEW ──────────────────────────────────────────────────────────
            case WALOpType::NEW: {
                // arg1 = 文件路径（含 dangling symlink）
                fs::path p = op.arg1;
                if (fs::exists(p) || fs::is_symlink(p)) {
                    safe_remove(p);
                    stats.files_cleaned++;

                    if (write_audit) {
                        // 逆操作: 删除安装时新建的文件（无备份可恢复）
                        wal_append_raw("RESTORE_FILE_RM " + op.arg1);
                    }
                }
                break;
            }

            // ── NEW_DIR ──────────────────────────────────────────────────────
            case WALOpType::NEW_DIR: {
                // arg1 = 目录路径
                fs::path p = op.arg1;
                if (fs::exists(p) && fs::is_directory(p)) {
                    std::error_code ec;
                    if (fs::is_empty(p, ec)) {
                        fs::remove(p, ec);
                        if (!ec && write_audit) {
                            // 逆操作: 删除安装时新建的空目录（无备份可恢复）
                            wal_append_raw("RESTORE_DIR_RM " + op.arg1);
                        }
                    }
                }
                break;
            }

            // ── DB ───────────────────────────────────────────────────────────
            case WALOpType::DB: {
                // arg1 = DB 文件路径, arg2 = 里程碑
                std::string bak = db_bak_path(op.arg1, op.arg2);
                if (fs::exists(bak)) {
                    ::safe_rename(bak, op.arg1);
                    stats.db_restored++;

                    if (write_audit) {
                        wal_append_raw("RESTORE_DB " + bak + " \xe2\x86\x92 " + op.arg1);
                    }
                }
                // bak 不存在 → WAL 已写但备份未完成 → 原文件还在 → 跳过（幂等）
                break;
            }

            // ── DBNEW ────────────────────────────────────────────────────────
            case WALOpType::DBNEW: {
                // arg1 = DB 文件路径, arg2 = 里程碑
                std::string bak = db_bak_path(op.arg1, op.arg2);
                if (fs::exists(bak)) {
                    ::safe_rename(bak, op.arg1);
                    stats.db_restored++;

                    if (write_audit) {
                        // DBNEW 有备份 → 恢复备份（与 DB 逆操作相同）
                        wal_append_raw("RESTORE_DB " + bak + " \xe2\x86\x92 " + op.arg1);
                    }
                } else {
                    // 无备份 → DB 文件是全新创建的 → 删除以恢复安装前状态
                    if (fs::exists(op.arg1)) {
                        safe_remove(op.arg1);
                        stats.files_cleaned++;

                        if (write_audit) {
                            wal_append_raw("RESTORE_DB_RM " + op.arg1);
                        }
                    }
                }
                break;
            }

            // ── DBRM ─────────────────────────────────────────────────────────
            case WALOpType::DBRM: {
                // arg1 = DB 文件路径, arg2 = 里程碑
                std::string bak = db_bak_path(op.arg1, op.arg2);
                if (fs::exists(bak)) {
                    ::safe_rename(bak, op.arg1);
                    stats.db_restored++;

                    if (write_audit) {
                        wal_append_raw("RESTORE_DB " + bak + " \xe2\x86\x92 " + op.arg1);
                    }
                }
                // bak 不存在 → 跳过（幂等）
                break;
            }

            default:
                break;
        }
    }

    return stats;
}

// ============================================================================
// 批次操作提取
// ============================================================================

std::vector<WALOp> extract_current_batch_ops(const std::string& wal_path)
{
    std::vector<WALOp> ops;
    std::ifstream file(wal_path);
    if (!file.is_open()) return ops;

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }

    // 从最后一个 BEGIN_PKGS 开始收集
    int start_idx = -1;
    for (int i = static_cast<int>(lines.size()) - 1; i >= 0; --i) {
        auto op = parse_op(lines[i]);
        // 未解析行（破损/半写尾部）必须跳过：否则它会以 INVALID 之外的类型参与判断，
        // 甚至被当作批次起点，导致整个批次的操作集被截断（见 TODO.md A2）。
        if (!op.is_valid()) continue;
        if (op.type == WALOpType::BEGIN_PKGS) {
            start_idx = i;
            break;
        }
        if (op.type == WALOpType::COMMIT_PKGS) {
            // 最后一个块已完成，无未提交批次
            return {};
        }
    }

    if (start_idx < 0) return {};

    for (size_t i = start_idx; i < lines.size(); ++i) {
        auto op = parse_op(lines[i]);
        if (op.is_valid()) ops.push_back(op);
    }

    return ops;
}

// ============================================================================
// WAL 保护的原始文本文件写入
// ============================================================================

/**
 * 以 write-ahead 顺序写一个原始文本文件（deps / needed_so / man 等 per-package
 * 元数据文件），保证回滚能恢复旧内容。
 *
 *  - content 非空：WAL 行用 DBNEW（新文件）或 DB（已存在，先备份旧内容）；
 *    reverse_execute 的 DBNEW 分支无备份时删除新文件，DB 分支从备份恢复旧内容。
 *  - content 为空且文件已存在：WAL 行用 DBRM（备份后删除），回滚时恢复旧内容。
 *  - content 为空且文件不存在：默认无操作；create_empty=true 时创建空文件（DBNEW）。
 *
 * 备份命名与 cache.cpp 的 .lpkg_db_bak_before:<milestone> 一致。
 */
void write_string_file_wal(const std::string& path, const std::string& content,
                           const std::string& milestone, bool create_empty)
{
    const fs::path p(path);
    const bool is_new = !fs::exists(p);

    if (content.empty() && !is_new) {
        // 内容为空且旧文件存在：DBRM 备份后删除，回滚恢复旧内容
        wal_append_raw("DBRM " + path + " " + milestone);
        std::string bak = path + ".lpkg_db_bak_before:" + milestone;
        safe_rename(p, bak);
        return;
    }
    if (content.empty() && is_new) {
        // 内容为空且文件不存在：默认不创建；deps 文件需显式创建空文件
        if (!create_empty) return;
        wal_append_raw("DBNEW " + path + " " + milestone);
    } else {
        wal_append_raw((is_new ? "DBNEW " : "DB ") + path + " " + milestone);
        if (!is_new) {
            std::string bak = path + ".lpkg_db_bak_before:" + milestone;
            safe_rename(p, bak);
        }
    }

    const fs::path tmp = fs::path(path + ".tmp");
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f.is_open())
            throw LpkgException(string_format("error.create_file_failed", tmp.string()));
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        f.flush();
        if (!f) throw LpkgException(string_format("error.db_write_failed", path));
    }
    // fsync(.tmp) 必须成功（磁盘满/EIO 的唯一信号）→ rename → fsync 父目录
    fsync_and_rename(tmp, p);
}

fs::path stash_root_of_bak(const fs::path& bak)
{
    const fs::path par = bak.parent_path();
    return par.filename().string().rfind(".lpkg_bak_", 0) == 0 ? par : bak;
}

void purge_consumed_stashes(const std::vector<WALOp>& ops)
{
    // stash 目录 = 每个备份目标(dst) 的 stash 根（统一 stash_root_of_bak 判定）
    std::set<fs::path> stashes;
    for (const auto& op : ops) {
        if ((op.type == WALOpType::BACKUP || op.type == WALOpType::REMOVE_OLD) &&
            !op.arg2.empty()) {
            stashes.insert(stash_root_of_bak(op.arg2));
        }
    }
    for (const auto& s : stashes) {
        std::error_code ec;
        fs::remove_all(s, ec);  // reverse 已完成 → stash 已还原干净，只剩空壳/已删
        if (ec) log_warning(string_format("warning.cleanup_failed", s.string()));
    }
}

// ============================================================================
// 批次回滚
// ============================================================================

bool batch_rollback(const std::vector<std::string>& successfully_installed)
{
    std::string wpath = wal_log_path();
    auto ops = extract_current_batch_ops(wpath);
    if (ops.empty()) {
        // 没有可回滚的行（如 WAL 尾部破损导致提取为空）：批次仍未提交、DB 备份
        // 尚未被消费。**绝不写 COMMIT_PKGS、绝不谎报已回滚**——调用方据此保留
        // WAL 与备份，交给下次 recover_packages() 幂等续传。
        log_warning(get_string("warning.wal_no_pending_batch"));
        return false;
    }

    // 1. 无需在此手动清理内存 cache：下方 reverse_execute 恢复磁盘 DB 后，
    //    步骤 3 的 cache.load() 会从磁盘整体重载（曾对 successfully_installed 逐包
    //    remove_installed——install 失败时这些包本就不该在内存、remove 失败时更是
    //    no-op，且随后被 load() 覆盖，纯死代码）。
    auto& cache = Cache::instance();

    // 2. 逆向执行操作
    reverse_execute(ops, true);

    // 2.5 stash 收尸：reverse 已把每个文件从 stash 还原，清掉空 stash（绝不能在
    //     reverse 完成前删——残留的 bak 是"还没还原"的数据）
    purge_consumed_stashes(ops);

    // 3. 重载 Cache（从磁盘恢复的 DB 文件）
    cache.load();

    // 4. DB :batch-start（保存回滚后的状态）
    cache.write(":batch-start");

    // 5. 对每个已成功（已回滚）包写 ROLLBACK + END
    //    版本号从 WAL 的 BEGIN 行提取——load() 后的 Cache 对全新安装的包返回空版本
    std::map<std::string, std::string> pkg_versions;
    for (const auto& op : ops) {
        if (op.type == WALOpType::BEGIN || op.type == WALOpType::RM_BEGIN)
            pkg_versions[op.arg1] = op.arg2;
    }
    for (const auto& pkg : successfully_installed) {
        auto it = pkg_versions.find(pkg);
        std::string ver = (it != pkg_versions.end()) ? it->second : std::string{};
        wal_append_raw("ROLLBACK " + pkg + " " + ver);
        wal_append_raw("END " + pkg + " " + ver);
    }

    // 6. COMMIT_PKGS
    wal_append_raw("COMMIT_PKGS");
    return true;
}

// ============================================================================
// WAL 文件路径
// ============================================================================

std::string wal_log_path()
{
    return (Config::instance().state_dir() / "transaction.log").string();
}

}  // namespace wal
