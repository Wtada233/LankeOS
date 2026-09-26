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
    // 配置文件"改名保留"：<src> → <src>.lpkgsave（必要时先把旧 .lpkgsave 移位到
    // .lpkgsave.N —— 那也是同类型的一行）。逆操作 = rename(dst → src)，与 BACKUP 同形；
    // 区别在 dst **不在 stash 里**（它是原地旁边的兄弟名，批次提交后**不**被
    // cleanup_stashes 删掉）—— 这正是"保留"与"--purge-config 真删"的分界。
    SAVE_CONF,  // SAVE_CONF <src> → <dst>
    // stash → 原位：把 `OpSink::un_stash()` 搬回去的那份记账（见 `ARCH.md` §9.2 的 `UNSTASH` 行）。
    // arg1 = bak（stash 里那份），arg2 = orig（它被搬回的位置）。**与 BACKUP 恰好互为逆**：
    // 正向 rename(bak → orig)，逆操作 rename(orig → bak)（找不到 orig → 跳过，幂等）。
    // 用途：`/etc` 配置的两种"保留"结果（三哈希的 KeepLocal / SaveLpkgnew）——
    // 第③步统一让开之后，这两种结果从"完全不碰盘"变成"先搬进 stash，判定后再搬回来"，
    // 可观测行为不变（文件回到原位，逐字节、逐 inode 属性不变）。
    UNSTASH,  // UNSTASH <bak> → <orig>
    // 目录的"改前状态"（2026-09-26 新增）：目录是就地改**活对象**的（不像普通文件那样
    // 先写 `.lpkgtmp` 再 rename 覆盖，旧 inode 由 BACKUP 保住），所以 lchown/chmod/xattr
    // 必须**先把旧状态记下来**才能回滚。三行的共同语义是"**记录改前的状态**"，不是
    // "记录正向动作" —— 于是"覆盖一个已有键"与"删掉一个已有键"共用 `XATTR_SET`
    // （两者的改前状态都是"有个值"，逆操作都是把它写回去）。
    //
    // 为什么键与值都要 **base64**：它们是任意字节串（`system.posix_acl_default` 就是二进制），
    // 而 WAL 是行式、空格分帧、`" → "` 是箭头分界的文本协议（见 parse_op）—— 裸放一个含
    // ` → ` / 换行 / 尾部空格的键就会把一条行**重新分帧**成另一条合法行，回滚照着重构出的
    // 路径去 chmod/lsetxattr。归档成员名那套消毒（archive.cpp）挡得住路径，挡不住 xattr 键
    // 名与值，所以这里用编码而不是拒绝。空值编码成空串，故 `XATTR_SET` 的第三个字段用
    // 哨兵把"空值"与"没有这一侧"区分开（见 op_sink.cpp 的 write_xattr）。
    DIR_META,   // DIR_META <path> <mode> <uid> <gid>   (改前的元数据；逆 = 写回)
    XATTR_SET,  // XATTR_SET <path> <b64_key> <b64_old_value>  (改前有值；逆 = 写回旧值)
    XATTR_NEW,  // XATTR_NEW <path> <b64_key>   (改前没有这个键；逆 = 删掉它)

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
    // 目录状态（元数据 / xattr）被写回 —— DIR_META / XATTR_SET / XATTR_NEW 三者的逆操作
    // 共用一条审计行：它们记的是同一个对象（那个目录）的"改前状态"，分三种关键字只会
    // 让审计词汇表变大而收益为零。审计里**不带**键名（键是 base64，塞进来人眼也读不出）。
    RESTORE_DIRSTATE,  // RESTORE_DIRSTATE <path>
    // 旧名称 — 仅用于解析旧 WAL 文件，不再写入
    REMOVE_FILE,  // 已废弃 → RESTORE_FILE_RM
    REMOVE_DIR,   // 已废弃 → RESTORE_DIR_RM
};

struct WALOp {
    WALOpType type = WALOpType::INVALID;  // 默认惰性：未成功解析的行不得被当成真实操作
    std::string raw;                      // 原始行文本（调试用）
    std::string arg1;                     // 参数1
    std::string arg2;                     // 参数2
    std::string arg3;                     // 参数3
    std::string arg4;                     // 参数4（预留）
    std::string arg5;                     // 参数5（预留）
    std::string arg6;                     // 参数6（预留）

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
               type == WALOpType::RESTORE_DIRSTATE ||
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
 * 跳过 RESTORE_x/REMOVE_x/元数据行；:batch-start DB 条目**只在正式文件仍在时**跳过
 * （文件不在 = 进程死在 write_db_file_wal/write_set_file_wal 的 rename 窗口里，此时那份
 * 备份是唯一的还原依据 —— 详见 wal_op.cpp 里的注释）。
 *
 * 没有"里程碑提前停止"机制：正常路径下 :batch-start DB 条目与"逆序跑完整个批次"的结果
 * 逐字节相同（同一个批次起点状态），所以不需要（也无法）提前停。
 *
 * **路径 confinement（纵深防御）**：每条可逆行的**全部目标路径**都必须落在
 * `Config::instance().root_dir()` 之内（或已知的 stash 根之内，见
 * `wal_line_paths_confined()`）；越界的行**告警 + 跳过** —— 与"bak 不存在 → 跳过"同一个
 * 保守方向，**绝不**因此让恢复失败（恢复失败 = 每次启动都重试、所有 lpkg 命令起不来）。
 * `root_dir()` 是 `/`（默认安装）时整套检查自动关掉（任何绝对路径都在其内）。
 *
 * @param ops              待逆向执行的操作（正向顺序）
 * @param write_audit      是否写 RESTORE WAL 审计行
 * @return RollbackStats
 */
RollbackStats reverse_execute(const std::vector<WALOp>& ops, bool write_audit = true);

// ============================================================================
// 批次操作提取
// ============================================================================

/**
 * 从 WAL 提取**当前（正在进行、尚未提交）的那个批次**的操作行：**从文件末尾反向扫**，
 * 撞到第一条 `BEGIN_PKGS` 就是它（若先撞到 `COMMIT_PKGS` 说明尾部批次已提交 → 返回空）。
 * 调用者是 `batch_rollback()` —— 它回滚的正是**本进程刚刚执行失败的那一批**。
 *
 * ⚠️ **不要把它与 `recover.cpp` 的"未提交区域起点"统一**（2026-09-26 明确记录，
 *    免得后人当重复代码合并掉）：那是**另一个问题** —— 崩溃恢复要从**第一个**未配对
 *    `BEGIN_PKGS` 开始，好把"更早的、同样没提交的批次"一起收掉（`scan_batch_pairing()`
 *    一处实现、四个消费者；那里踩过"取最后一个 ⇒ 更早那批永远轮不到"的坑，见 ARCH §11.2）。
 *    而这里问的是"**当前**那批是谁"，反向扫才对。
 *    两者在实践中重合（进程内未配对 `BEGIN_PKGS` 至多一个 —— 批次事务不可重入），
 *    但在**手工构造/破损的 WAL** 上会给出不同答案，而测试正好钉住那些形状
 *    （`test_write_file_wal.cpp` / `test_wal_edge_cases.cpp` / `test_wal_rollback_guards.cpp`）。
 */
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
 *
 * `UNSTASH` 行**不**作为触发器（它的 bak 与同批次的 BACKUP 同根，已被收集；单独拿它当
 * 触发器是把"刚从 stash 搬出来"误读成"这个 stash 可以整目录删"），但会参与**收敛检查**：
 * UNSTASH 引用的 bak 在 reverse 之后仍在 ⇒ 回滚没收敛 ⇒ 保留该根（详见实现里的说明）。
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

/**
 * 以追加模式打开 WAL 文件，并报告**本次调用是否创建了它**（`created`）。
 *
 * 为什么要区分"创建"与"打开"：WAL 文件**自身**的目录项（dentry）只在**首次创建**那一刻
 * 需要落盘（`fsync(文件)` 管不到父目录），所以 `wal::log_wal_line` 与 `wal_append_raw`
 * 这两条"每行都要走一次"的路径只在 `created == true` 时付父目录 fsync —— 恒做等于每条
 * WAL 行白付一次（实测占 fsync 总数 34%~40%）。**行内容自己的 `::fsync(fd)` 恒生效**，
 * 与这里无关（I-FSYNC-1 的前提不变）。
 *
 * 判定用 `O_CREAT|O_EXCL`（原子，无 TOCTOU）：成功 = 本次创建；`EEXIST` = 本来就在 →
 * 退回普通追加打开。返回的 fd 由调用方负责 `::close`；`< 0` 时调用方按原有口径报错。
 */
int open_wal_append(bool& created);

}  // namespace wal
