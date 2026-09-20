#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace wal
{

// ============================================================================
// DbMilestone — 确定性的 DB 状态标签
// ============================================================================

struct DbMilestone {
    std::string pkg;    // 包名，":batch-start" 时 pkg=""
    std::string state;  // "installed" | "removed" | "batch-start"

    std::string to_string() const
    {
        if (pkg.empty()) return ":" + state;
        return pkg + ":" + state;
    }

    static DbMilestone from_string(const std::string& s)
    {
        auto colon = s.find(':');
        if (colon == std::string::npos) return {"", s};
        if (colon == 0) return {"", s.substr(1)};
        return {s.substr(0, colon), s.substr(colon + 1)};
    }

    bool is_batch_start() const
    {
        return state == "batch-start";
    }
};

// ============================================================================
// WALOp — 单个 WAL 操作行
// ============================================================================

enum class WALOpType {
    // 解析失败/未知类型。**必须留在首位**：WALOp 默认构造即 INVALID，任何未被成功解析的
    // 行都是"惰性"的，扫描方只需 is_valid() 一处判断。曾经用 `type=BEGIN_PKGS` 当哨兵 +
    // `arg1="__INVALID__"` 字符串标记，导致破损行在"找最后一个 BEGIN_PKGS"的反向扫描里
    // 冒充真实批次起点，整批回滚静默失效（见 TODO.md A2）。
    INVALID,

    // 批次边界
    BEGIN_PKGS,   // BEGIN_PKGS <N>
    COMMIT_PKGS,  // COMMIT_PKGS

    // 安装操作
    BEGIN,     // BEGIN <pkg> <ver>
    COMMIT,    // COMMIT <pkg> <ver>
    ROLLBACK,  // ROLLBACK <pkg> <ver>
    END,       // END <pkg> <ver>

    // 文件操作
    BACKUP,      // BACKUP <src> → <dst>
    NEW,         // NEW <path>
    NEW_DIR,     // NEW_DIR <path>
    COPY,        // COPY <src> → <dst>
    REMOVE_OLD,  // REMOVE_OLD <src> → <dst>
    DIR_RM,      // DIR_RM <path> <mode> <uid> <gid>  (删除空目录；回滚按元数据重建)

    // 移除操作
    RM_BEGIN,   // RM_BEGIN <pkg> <ver>
    RM_COMMIT,  // RM_COMMIT <pkg> <ver>
    RM_END,     // RM_END <pkg> <ver>
    CLEANUP,    // CLEANUP <path>                      (不可回滚的 .bak 清理记录)

    // DB 操作
    DB,     // DB <path> <milestone>
    DBNEW,  // DBNEW <path> <milestone>
    DBRM,   // DBRM <path> <milestone>

    // 回滚审计 — 描述实际文件动作，而非正向操作名
    // 恢复备份: rename .bak → 原位
    RESTORE_FILE,  // RESTORE_FILE <bak> → <orig>
    RESTORE_DB,    // RESTORE_DB <bak> → <db>
    RESTORE_DIR,   // RESTORE_DIR <path>
    // 删除操作: 无备份可恢复，直接删除以回退到安装前状态
    RESTORE_FILE_RM,  // RESTORE_FILE_RM <path>   (COPY/NEW 逆操作)
    RESTORE_DIR_RM,   // RESTORE_DIR_RM <path>    (NEW_DIR 逆操作)
    RESTORE_DB_RM,    // RESTORE_DB_RM <path>     (DBNEW 无备份 逆操作)
    // 旧名称 — 仅用于解析旧 WAL 文件，不再写入
    REMOVE_FILE,  // 已废弃 → RESTORE_FILE_RM
    REMOVE_DIR,   // 已废弃 → RESTORE_DIR_RM
};

struct WALOp {
    WALOpType type = WALOpType::INVALID;  // 默认惰性：未成功解析的行不得被当成真实操作
    std::string raw;   // 原始行文本（调试用）
    std::string arg1;  // 参数1
    std::string arg2;  // 参数2
    std::string arg3;  // 参数3
    std::string arg4;  // 参数4（预留）
    std::string arg5;  // 参数5（预留）
    std::string arg6;  // 参数6（预留）

    /// 行是否被成功解析（未知/损坏行 = INVALID）。所有扫描/回放都必须先判它。
    bool is_valid() const
    {
        return type != WALOpType::INVALID;
    }

    bool is_metadata() const
    {
        return type == WALOpType::ROLLBACK || type == WALOpType::END || type == WALOpType::COMMIT ||
               type == WALOpType::BEGIN || type == WALOpType::RM_BEGIN ||
               type == WALOpType::RM_COMMIT || type == WALOpType::RM_END ||
               type == WALOpType::BEGIN_PKGS || type == WALOpType::COMMIT_PKGS;
    }

    bool is_restore_audit() const
    {
        return type == WALOpType::RESTORE_FILE || type == WALOpType::RESTORE_DB ||
               type == WALOpType::RESTORE_DIR || type == WALOpType::RESTORE_FILE_RM ||
               type == WALOpType::RESTORE_DIR_RM || type == WALOpType::RESTORE_DB_RM ||
               type == WALOpType::REMOVE_FILE ||  // 旧名称兼容
               type == WALOpType::REMOVE_DIR;     // 旧名称兼容
    }

    /// reverse_execute 需要跳过的行（未解析/元数据/审计/CLEANUP 均不可逆）
    bool skip_in_reverse() const
    {
        return !is_valid() || is_metadata() || is_restore_audit() || type == WALOpType::CLEANUP;
    }
};

// ============================================================================
// 类型名转换
// ============================================================================

std::string_view walop_type_name(WALOpType t);
WALOpType walop_type_from_name(std::string_view name);

// ============================================================================
// WAL 行解析
// ============================================================================

/// 解析单行 WAL 日志为 WALOp
WALOp parse_op(const std::string& line);

// ============================================================================
// 回滚统计
// ============================================================================

struct RollbackStats {
    int files_restored = 0;
    int files_cleaned = 0;
    int dirs_recreated = 0;
    int db_restored = 0;
};

// ============================================================================
// 逆向执行引擎
// ============================================================================

/**
 * 逆向执行一组 WAL 操作。
 *
 * 对每条操作按类型执行逆向，每个操作后写入 RESTORE_* 审计行。
 * 跳过 RESTORE_x/REMOVE_x/元数据行和 :batch-start DB 条目。
 *
 * 没有"里程碑提前停止"机制：:batch-start DB 条目本就被跳过（最终状态标记），
 * 逆序跑完整个批次恰好恢复到批次开始状态，不需要（也无法）提前停。
 *
 * @param ops              待逆向执行的操作（正向顺序）
 * @param write_audit      是否写 RESTORE WAL 审计行
 * @return RollbackStats
 */
RollbackStats reverse_execute(const std::vector<WALOp>& ops, bool write_audit = true);

// ============================================================================
// 批次操作提取
// ============================================================================

/// 从 WAL 日志文件提取当前（最后一个未完成的）批次的操作行列表
std::vector<WALOp> extract_current_batch_ops(const std::string& wal_path);

// ============================================================================
// 批次回滚
// ============================================================================

/**
 * 完整的批次回滚。
 * 1. 提取当前批次 WAL 行
 * 2. 清理内存 cache
 * 3. reverse_execute(ops)
 * 4. DB /pkgs :batch-start
 * 5. ROLLBACK pkg + END pkg 对每个已回滚包
 * 6. COMMIT_PKGS
 *
 * @return true = 确实回滚了（批次已由 COMMIT_PKGS 收尾，DB 备份已被消费，可以安全清理）；
 *         false = 无可回滚的行（WAL 里没有未完成批次，如尾部破损行导致 ops 为空）——
 *         此时**批次仍开着、DB 备份还没被消费**，调用方必须保留它们交给下次 rec 续传，
 *         绝不能 cleanup_db_backups()（否则文件能还原而 DB 永远还原不回来，见 TODO.md A2/A3）。
 */
bool batch_rollback(const std::vector<std::string>& successfully_installed);

// ============================================================================
// 崩溃续传清理（recover.cpp 实现）
// ============================================================================


/**
 * 备份目标 → 其所在 stash 根：父目录名以 `.lpkg_bak_` 开头（= stash 目录）则取父目录，
 * 否则原样返回（兼容非 stash 的兜底路径）。recover 续传与 purge 统一走这里，避免各自
 * 再写一遍"父目录即 stash"的判定。
 */
std::filesystem::path stash_root_of_bak(const std::filesystem::path& bak);

/**
 * WAL 当前仍引用到的 stash 根集合（BACKUP/REMOVE_OLD 的 dst、CLEANUP 的 arg1）。
 * `cleanup_orphan_stashes()` **必须**跳过这些：它们是回滚/续传的数据来源，而 stash 落在
 * 文件系统顶层（= root_dir 的直接子目录）正是 reaper 的扫描范围，被延迟处理的未提交批次
 * 其 pid 又必然已死 —— 不排除就会在同一次启动里被回收（TODO.md Z5）。
 */
std::set<std::filesystem::path> referenced_stash_roots();

// ============================================================================
// stash 收尸（TODO：备份移到每文件系统隔离 stash 后）
// ============================================================================

/**
 * 清理"已全部还原消费"的 stash 目录。reverse_execute 把每个 BACKUP/REMOVE_OLD 的
 * 文件从 stash 还原后，stash 应已空；本函数把 ops 中所有指向 stash（父目录名为
 * `.lpkg_bak_*`）的备份目标父目录 remove_all 掉，避免空 stash 残留。
 * 只应在完整 reverse（未抛异常）后调用——若还有未还原的 bak 在里面绝不能删。
 */
void purge_consumed_stashes(const std::vector<WALOp>& ops);

// ============================================================================
// WAL 保护的原始文本文件写入
// ============================================================================

/**
 * 以 write-ahead 顺序写一个原始文本文件（deps/needed_so/man 等 per-package
 * 元数据文件），已存在的旧文件先备份为 .lpkg_db_bak_before:<milestone>，
 * 回滚时 reverse_execute 的 DB/DBNEW/DBRM 分支可恢复旧内容。
 *
 * @param create_empty  content 为空且文件不存在时是否仍创建空文件
 *                     （deps 文件需要：空内容表示"无依赖"这一显式状态）
 */
void write_string_file_wal(const std::string& path, const std::string& content,
                           const std::string& milestone, bool create_empty = false);

// ============================================================================
// WAL 文件路径
// ============================================================================

/// 获取 WAL 日志文件的路径
std::string wal_log_path();

}  // namespace wal
