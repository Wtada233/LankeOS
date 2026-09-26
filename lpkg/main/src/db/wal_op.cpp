#include "wal_op.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
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
    {"SAVE_CONF", WALOpType::SAVE_CONF},
    {"UNSTASH", WALOpType::UNSTASH},
    {"DIR_META", WALOpType::DIR_META},
    {"XATTR_SET", WALOpType::XATTR_SET},
    {"XATTR_NEW", WALOpType::XATTR_NEW},
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
    {"RESTORE_DIRSTATE", WALOpType::RESTORE_DIRSTATE},
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
//       <路径可含空格> <mode> <uid> <gid> (DIR_RM/DIR_META，tail=3) TYPE <路径可含空格>
//       <b64_key> (XATTR_NEW，tail=1) TYPE <路径可含空格> <b64_key> <b64_old_value>
//       (XATTR_SET，tail=2) TYPE <路径可含空格>
//       (NEW/NEW_DIR/CLEANUP/RESTORE_*_RM/RESTORE_DIRSTATE，tail=0)
//   尾字段都是版本号/里程碑/数字元数据/Base64（字母表 A-Za-z0-9+/=），不可能含空格，
//   故从右侧锚定是安全的。**XATTR_* 的键与值必须先 base64** —— 它们是任意字节串，
//   裸放一个含空格/换行/`" → "` 的键就会破坏上面两条分帧规则（见 WALOpType 里的说明）。
//   历史 WAL 行的字段内没有空格，"整段归 arg1" 退化成旧的逐空格切分 → 完全向后兼容。
//   （残留：路径含换行会破行、含字面 " → " 会破箭头分帧——都是文件系统允许但现实中
//     不会出现的名字。）

/// arg1 之后固定字段的个数（从右往左数）
static int tail_args(WALOpType t)
{
    switch (t) {
        case WALOpType::DIR_RM:
        case WALOpType::DIR_META:
            return 3;  // <mode> <uid> <gid>
        case WALOpType::XATTR_SET:
            return 2;  // <b64_key> <b64_old_value>
        case WALOpType::XATTR_NEW:
            return 1;  // <b64_key>
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
    bool created = false;
    int fd = open_wal_append(created);
    if (fd < 0) throw LpkgException(string_format("error.wal_open_failed", path));

    // WAL 文件的**目录项** fsync —— 这是 WAL 的**第三条打开/创建路径**（前两条：`WalWriter`
    // 构造、`wal::log_wal_line`）。**恒生效**（所以套守卫，不受默认关闭的
    // durable_fsync_enabled() 影响）：与那两条同理 —— `fsync(文件)` 不覆盖父目录的
    // dentry，"行已 fsync、文件整个不存在"是断电后真实可能的状态，而 WAL 行是回滚的
    // 唯一依据（不可恢复组合）。走到本函数的今天确实都是"批次进行中"，WAL 必然已存在，
    // 所以这是**危害低**的一处补丁；但判据是"这条路径**会不会**创建 WAL 文件"（O_CREAT
    // 在这里写着），不是"今天它是不是恰好不会" —— 依赖后者的话，哪天有人从中途调用它，
    // 保证就静默失效了。
    //
    // **只在真的创建了文件时才做**：目录项只有在那一刻才需要落盘，文件本来就在时再
    // fsync 一次父目录是白付（这条路径每写一行审计行就会走一次；`open_wal_append` 用
    // O_CREAT|O_EXCL 原子地判出"本次是否创建"，不是 TOCTOU 的 fs::exists）。原先恒做，
    // 实测占全部 fsync 的 34%~40%。行内容的 ::fsync(fd) 不受影响，恒生效。
    //
    // 守卫**只包这一小块**：绝不能扩成整个函数/函数外 —— `write_string_file_wal` 在它的
    // 最外层已经有一个守卫（那是 DB/元数据写的保证），而"批量文件数据"（包内容、
    // .lpkgtmp/.lpkgnew）的 fsync 是故意留给开关控制的，顺手打开会让两万文件规模的
    // 安装慢到分钟级。
    if (created) {
        DurableFsyncGuard durable;  // WAL 文件目录项：创建即落盘，与开关无关
        fsync_parent_dir(path);
    }

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

// ============================================================================
// 撤销表 —— `reverse_execute` 的唯一知识来源
//
// 为什么要有这张表：`reverse_execute` 原先按 op 类型逐个 `if/else` 分派，而**每种 op
// 各自**把三件事写了一遍 —— ① 幂等判据（"目标不存在 → 跳过"，但每种 op 的口径不同：
// bak 不存在 / 原位不存在 / 已是真目录 …）；② 路径 confinement（查哪些字段）；③ RESTORE_*
// 审计行（还要记**实际动作**方向）。加一种 op 就得记得在三处都补一遍，漏一处的后果是
// "恢复期静默半成品"（历史上真发生过：`SAVE_CONF` 行曾在 ARCH.md §9.2 的表里漏列、
// `NEW_DIR` 的逆操作曾漏 `strip_trailing_slash`）。
//
// 现在三件事**统一施加**（见 `apply_row()`：判 guard → 做 undo → 计数 + 写审计），逐 op 的
// 差异只剩表里的几格。**新增一种 op = 在表里加一行**（只有"需要全新的谓词/动作"时才顺带
// 加一个 `Guard` / `Undo` 枚举值）。
// ============================================================================

namespace
{

/// WAL 行里的路径字段编号：`ARG1`/`ARG2` 是行内字段；`DB_BAK` 是**派生**路径
/// （DB 家族的备份侧不是 WAL 字段，而是 `<arg1>.lpkg_db_bak_before:<arg2>`，见
/// `db_bak_path()` —— 与 cache.cpp 的备份命名一致）
constexpr int ARG1 = 1;
constexpr int ARG2 = 2;
constexpr int DB_BAK = -1;

/// 表里一格的路径引用：取哪个字段 + 取用时是否剥尾斜杠
struct ArgRef {
    int field = 0;       ///< 0 = 本行没有这一侧
    bool strip = false;  ///< 目录键 / 归档目录条目形态（见 base/utils.hpp）

    constexpr bool present() const
    {
        return field != 0;
    }
};

/**
 * 撤销动作 —— "这一行要做什么"。**rename 方向的定义处**：`RestoreFromBak`
 * （备份侧 → 原位）与 `ReStash`（原位 → 备份侧）**恰好互为逆**，是全表最容易搞反的一格
 * （备份类 op 的逆操作把 stash 里那份搬**回来**，UNSTASH 的逆操作把它**再搬进去**）。
 */
enum class Undo {
    RestoreFromBak,  ///< rename(备份侧 → 原位)：BACKUP / REMOVE_OLD / SAVE_CONF
    ReStash,         ///< rename(原位 → 备份侧)：UNSTASH（与上面方向相反）
    RecreateDir,     ///< create_directories + lchown/chmod：DIR_RM 的逆（按元数据重建）
    RemoveFile,      ///< unlink 主路径：COPY / NEW / DBNEW（无备份那支）
    RemoveEmptyDir,  ///< rmdir（此刻为空）：NEW_DIR 的逆
    RestoreDb,       ///< rename(DB 备份 → 正式文件)：DB / DBNEW / DBRM 有备份那支
    RestoreDirMeta,  ///< lchown+chmod 回行里记的元数据：DIR_META 的逆
    SetXattr,        ///< lsetxattr(行里的键, 行里的旧值)：XATTR_SET 的逆
    RemoveXattr,     ///< lremovexattr(行里的键)：XATTR_NEW 的逆
};

/**
 * 幂等口径 —— "在什么条件下才真的动盘"（判否 ⇒ 跳过：不计数、不写审计行）。
 * 口径确实逐 op 不同（备份侧被占 / 原位被占 / 已是真目录 / DB 备份存在 …），故各成一格；
 * 实现集中在 `guard_ok()` 一处，**永不抛**（回滚路径上的判定绝不能有能力打断事务）。
 */
enum class Guard {
    Always,             ///< 无跳过判据（DIR_RM：**目录缺失正是它要修的状态**，动作自己判）
    BakNameTaken,       ///< 备份侧那个**名字**被占着（lstat 语义：含悬空链接与符号链接环）
    OrigTakenInStash,   ///< UNSTASH：原位仍被占、且 stash 侧那份的父目录还在
    TargetTakenNotDir,  ///< 主路径被占、且**不是**真目录（lstat 语义）
    TargetIsRealDir,    ///< 主路径是**真目录**（lstat 语义，不跟随末段链接）
    TargetTaken,        ///< 主路径被占（lstat 语义）
    DbBakExists,        ///< DB 备份文件存在（**跟随**语义 + **不抛**：`exists_follow`）
    TargetExists,       ///< 主路径存在（**跟随**语义 + **不抛**：`exists_follow`）
    /// 主路径是真实目录（lstat 语义）—— DIR_META 的逆：**只回写真正是目录的那些**。
    /// 目录已经消失（被本批次更晚的逆操作删掉 / 被外部删掉）时跳过：`chmod`/`lchown` 落到
    /// 不存在的路径上毫无意义，落到**符号链接**上更糟（lchown 改链接自身、chmod 穿透到
    /// 链接目标 —— 而那可能是别的包持有的目录，正是写入侧整块跳过 xattr/元数据的那条边界）。
    RealDir,  ///< 主路径是真实目录（lstat 语义）
    /// 主路径被占、且**不是符号链接**（lstat 语义）—— XATTR_* 的逆：xattr 写入要落在
    /// 真实对象上。与 `RealDir` 分开是因为这里的对象除了目录还可能是普通文件（同一条
    /// 行类型将来若被文件路径复用，判据不该跟着改）。
    TakenNotSymlink,  ///< 主路径存在（lstat 语义）且不是符号链接
};

/// 审计行 —— 记的是**实际动作**（不是正向操作名）
struct Audit {
    const char* op = nullptr;  ///< 关键字（RESTORE_FILE / …）；nullptr = 不写审计行
    ArgRef first = {};         ///< 单路径形态 = 该路径；箭头形态 = **源**
    ArgRef second = {};        ///< 箭头形态的**宿**；`field == 0` = 单路径形态
};

/// 计到 `RollbackStats` 的哪个字段（`None` = 本行不计任何统计量 —— NEW_DIR 今天就不计）
enum class Stat { None, FilesRestored, FilesCleaned, DirsRecreated, DbRestored };

/// confinement 口径：查这一行的哪些字段（越界 ⇒ 告警 + 跳过，绝不因此让恢复失败）
enum class Confine { None, Arg1Only, Arg1AndArg2 };

/// 表的一行 = 一种可逆 op 的**一次**撤销。COPY / DBNEW 各有两行 —— 它们的撤销本来就有
/// 两支（COPY：删落点 + 清残留 `.lpkgtmp`，两支互相独立；DBNEW：有备份则还原、无备份则
/// 删掉新建的，两支互斥）。
struct UndoRow {
    WALOpType type;     ///< op 类型
    Undo undo;          ///< 撤销动作（**方向**的定义处）
    ArgRef orig;        ///< **原位**：撤销动作作用的主路径
    ArgRef bak;         ///< **备份/stash 侧**（`field == 0` = 本行没有这一侧）
    Guard guard;        ///< 幂等口径
    Confine confine;    ///< confinement 口径（同一类型的各行必须相同，见 rows_of）
    Audit audit;        ///< 审计行种类
    Stat stat;          ///< 计哪个统计量
    bool also = false;  ///< 本行动作做完后**继续**扫描同类型的后续行（COPY 的两半独立）
};

/**
 * 表本体。**按 op 类型分组**（`rows_of()` 依赖这个不变量），同类型的多行按执行顺序排。
 *
 * 几处一眼看不出、别"顺手统一"的地方：
 *   · `orig`/`bak` 是**角色**不是字段号：BACKUP 的原位在 arg1、UNSTASH 的原位在 arg2 ——
 *     两支的 `undo` 方向相反，所以"哪个字段是原位"必须逐行写清楚，别让读者从字段号去推
 *     （两行的字段号恰好都是 2 → 1，纯属参数顺序对调）。
 *   · `strip` 出现在**目录键形态**的那几格（尾斜杠会让判定类调用解引用末段链接）。
 *   · 审计行的 `strip` 与动作的 `strip` **各自独立**：`DIR_RM` 记**规范化后**的路径
 *     （`RESTORE_DIR <已剥尾斜杠的 p>`），而 `NEW_DIR` 记**原样**的 arg1（含尾斜杠）——
 *     这是今天逐字的行为。
 */
constexpr UndoRow UNDO_TABLE[] = {
    // ── BACKUP / REMOVE_OLD / SAVE_CONF：三者的**逆操作完全相同** ──────────────────
    // rename(arg2 → arg1)（找不到 arg2 → 跳过，幂等）。
    // 判据是"这个**名字**被占着"（含悬空链接、**符号链接环**），所以必须 lstat 语义 ——
    // 原来的 `exists || is_symlink` 正是它。**但不能用会抛的那个版本**：`bak_path` 完全
    // 可以是环本身（原路径是环，rename 进 stash 之后还是环），而 `fs::exists` 对环抛
    // ELOOP → **回滚在这里被打断**，事务收不了尾、盘面停在"旧的那份已搬走、新的还没
    // 落位"的中间态（tests/integration/test_symlink_loop_install.cpp 钉这条）。
    //
    // SAVE_CONF 的正向 dst 是配置文件原位旁边的 `<路径>.lpkgsave`（不在 stash 里，批次
    // 提交后**不**被 CLEANUP/cleanup_stashes 删掉）—— 它只是"改名保留"还是
    // "--purge-config 真删"的分界，回滚侧无需区分。
    {WALOpType::BACKUP,
     Undo::RestoreFromBak,
     {ARG1, false},
     {ARG2, false},
     Guard::BakNameTaken,
     Confine::Arg1AndArg2,
     {"RESTORE_FILE", {ARG2, false}, {ARG1, false}},
     Stat::FilesRestored},
    {WALOpType::REMOVE_OLD,
     Undo::RestoreFromBak,
     {ARG1, false},
     {ARG2, false},
     Guard::BakNameTaken,
     Confine::Arg1AndArg2,
     {"RESTORE_FILE", {ARG2, false}, {ARG1, false}},
     Stat::FilesRestored},
    {WALOpType::SAVE_CONF,
     Undo::RestoreFromBak,
     {ARG1, false},
     {ARG2, false},
     Guard::BakNameTaken,
     Confine::Arg1AndArg2,
     {"RESTORE_FILE", {ARG2, false}, {ARG1, false}},
     Stat::FilesRestored},

    // ── UNSTASH：正向是 rename(arg1=bak → arg2=orig)，逆操作 = **再搬进 stash** ─────
    // rename(orig → bak)，与 BACKUP 恰好互为逆 —— 回滚逆序先撤 UNSTASH（原位 → stash）再
    // 撤 BACKUP（stash → 原位），两次 rename 回到原点。找不到 orig → 跳过（幂等）：即
    // "这条 un_stash 还没做/已经被撤过"，此时 BACKUP 的逆操作会把 stash 里那份搬回原位，
    // 语义仍然正确。
    //
    // **原位侧剥尾斜杠、审计行仍记 WAL 里的原样字面**：`/etc` 的目录条目形态由调用点传入
    // （`un_stash(rec.bak, physical_path)`，physical_path 可能是目录键形态）；判定类调用
    // 落到链接目标上的坑见 base/utils.hpp。
    // 审计记录的是**实际动作**（orig → bak，即"再搬进 stash"），不是"还原"——所以两侧的
    // 方向与 BACKUP 的 RESTORE_FILE 行恰好相反。两行的审计列字面相同（都是 arg2 → arg1）
    // 纯属参数顺序对调：**方向由 `undo` 列定义**，审计列只是把同一次 rename 渲染成源 → 宿。
    //
    // bak **不存在**时不动：那是"stash 已被收尸（purge/清理）"或"这份备份在更早的一次
    // 回滚里已被消费"的形态 —— 往一个不存在的目录里 rename 会 ENOENT 抛错，而回滚路径上
    // 的判定**绝不能有能力打断事务**（对称于 BACKUP 的"bak 不存在 → 跳过"）。判据用不抛的
    // lstat 语义（同一族谓词）。
    {WALOpType::UNSTASH,
     Undo::ReStash,
     {ARG2, true},
     {ARG1, false},
     Guard::OrigTakenInStash,
     Confine::Arg1AndArg2,
     {"RESTORE_FILE", {ARG2, false}, {ARG1, false}},
     Stat::FilesRestored},

    // ── DIR_RM：删除空目录；回滚按元数据重建 ─────────────────────────────────────
    // arg1 = 目录路径, arg2 = mode(十进制), arg3 = uid, arg4 = gid（后三个由元数据列消费）。
    // 目录键带尾斜杠 → 先规范化：否则下面的 is_symlink 守卫恒假（尾斜杠解引用末尾链接），
    // 回滚会 chmod/lchown **穿过**链接改掉链接目标目录的权限/属主，还写一行 RESTORE_DIR
    // 谎报"已重建"。审计行记的就是**规范化后**的那个路径（今天的字面）。
    // （2026-09-26 补注：上面那句"恒假"只描述了**末段是符号链接**那一种形态。**中间段**成环
    // 时抛型 `fs::is_symlink` 连"判否"都做不到 —— 它抛 code=40 直接打断回滚，所以本文件
    // 里凡可能吃到中间段环的判定都换成了 `is_symlink_no_follow()`；这里的 `strip` 仍然必要，
    // 但理由要从"守卫会恒假"升级为"守卫可能既不等价、又可能抛"。）
    // `Guard::Always`：没有"目标不存在就跳过"这一说 —— 目录缺失正是它要修的状态；是否算
    // "真的动了盘"由动作自己判（重建成功、或本来就已是真目录，都算）。
    {WALOpType::DIR_RM,
     Undo::RecreateDir,
     {ARG1, true},
     {},
     Guard::Always,
     Confine::Arg1Only,
     {"RESTORE_DIR", {ARG1, true}, {}},
     Stat::DirsRecreated},

    // ── DIR_META：目录的**改前元数据**；逆操作 = 按行里的 mode/uid/gid 写回 ────────────
    // 为什么需要它：目录是**就地改活对象**的（`write_dir_entry` / `let_go_make_dir` 直接对它
    // lchown/chmod），不像普通文件那样"先写 `.lpkgtmp`、再 rename 覆盖"——**旧 inode 没有
    // 任何备份行保住**（BACKUP 只对文件）。没有这一行，注入失败回滚后目录会保留**新**版本
    // 的 mode/uid，与 ARCH §4 不变量 3（终态 == 事务开始时的盘面）冲突。这不是潜伏问题：
    // 任何"新版本改了某个已存在目录的 mode"的批次失败都会踩到。
    // 三个格的要点：
    //   · `strip = true`：路径来自目录条目（可能带尾斜杠），尾斜杠会让 lchown/chmod
    //     **穿透**到末段符号链接的**目标**上（见 base/utils.hpp 的谓词说明）。
    //   · `Guard::RealDir`：只回写真正还是目录的那些 —— 目录已被更晚的逆操作删掉时跳过，
    //     **不重建**（重建会造出一个本不该存在的路径；那不是本行的职责，`DIR_RM` 才有）。
    //   · `Stat::None`：**不计进 `dirs_recreated`** —— 本行没有重建任何目录，混进那个计数器
    //     会让"重建了几个目录"失去意义（NEW_DIR 的逆操作同样不计，取向一致）。
    {WALOpType::DIR_META,
     Undo::RestoreDirMeta,
     {ARG1, true},
     {},
     Guard::RealDir,
     Confine::Arg1Only,
     {"RESTORE_DIRSTATE", {ARG1, true}, {}},
     Stat::None},

    // ── XATTR_SET / XATTR_NEW：目录上某个 xattr 键的**改前状态** ──────────────────────
    // 语义是"记录改前状态"，**不是**"记录正向动作"。于是"覆盖一个已有的键"与"删掉一个已有
    // 的键"共用 `XATTR_SET`（两者的改前状态都是"有个值"，逆操作都是把它写回去），只有
    // "新建一个键"用 `XATTR_NEW`（改前不存在，逆操作 = 删掉它）。这一格不区分方向是**故意的**：
    // 区分了就要多一个"删除前的旧值"行类型，而它与"覆盖前的旧值"在回滚侧完全同形。
    //
    // 键与值都 base64（见 WALOpType 的说明）：xattr 键名与值是任意字节串，裸放进 WAL 会
    // 破坏行分帧（键里的空格 / 换行 / `" → "`）。**空值**用哨兵 `-`（base64 字母表里没有
    // `-`，故与任何合法编码都不冲突）—— 不能用空字段，那与"这一侧不存在"同形。
    //
    // `Guard::RealDir`（2026-09-26 修，原来是 `TakenNotSymlink`）：**与写入侧逐字对齐** ——
    // 写入侧（`OpSink::set_xattr` / `unset_xattr`）的前置是 `lstat` + `S_ISDIR`，也就是**这些
    // 行只描述真实目录**；而 `TakenNotSymlink` 放行的范围宽得多（普通文件 / FIFO / 设备都算），
    // 于是回滚会 `lsetxattr` 在**写入侧从不写**的路径上造出一个键（"写入侧刻意不造的状态，
    // 回滚侧造出来了"）。`DIR_META` 那一格用的就是 `RealDir` —— 这两格此前没跟上。
    // 与符号链接那一半的关系：`is_real_directory` 同样排除符号链接，所以原来防住的那半没丢。
    // `Confine::Arg1Only`：arg2/arg3 是 base64，**不是路径** —— 若按 Arg1AndArg2 查，
    // 一串 base64 会被当成越界路径从而**整行被跳过**（回滚静默少还原一次）。
    // `Stat::None`：同 DIR_META。
    {WALOpType::XATTR_SET,
     Undo::SetXattr,
     {ARG1, false},
     {},
     Guard::RealDir,
     Confine::Arg1Only,
     {"RESTORE_DIRSTATE", {ARG1, false}, {}},
     Stat::None},
    {WALOpType::XATTR_NEW,
     Undo::RemoveXattr,
     {ARG1, false},
     {},
     Guard::RealDir,
     Confine::Arg1Only,
     {"RESTORE_DIRSTATE", {ARG1, false}, {}},
     Stat::None},

    // ── COPY：两支，**互相独立**（第一支不做也不影响第二支）────────────────────────
    // ① 删落点（arg2）：绝不 rmdir —— COPY 的落点只可能是文件/符号链接（"归档文件撞真
    //    目录"已在 check_for_file_conflicts 前置拒绝）。这里仍显式挡住真目录 ——
    //    `fs::remove` 对**空目录**会 rmdir 成功，实测会把盘上原有的空目录删掉而 DB 仍声称
    //    持有（盘面/DB 脱节，且没有 BACKUP 行可还原）。
    // ② 清残留（arg1）：COPY 只删 dst，`.lpkgtmp` 仍可能残留（清理中断安装的残留）。它
    //    **不是 COPY 落位的对象**，故无审计行，但同样计数。
    {WALOpType::COPY,
     Undo::RemoveFile,
     {ARG2, false},
     {},
     Guard::TargetTakenNotDir,
     Confine::Arg1AndArg2,
     {"RESTORE_FILE_RM", {ARG2, false}, {}},
     Stat::FilesCleaned,
     /*also=*/true},
    {WALOpType::COPY,
     Undo::RemoveFile,
     {ARG1, false},
     {},
     Guard::TargetTaken,
     Confine::Arg1AndArg2,
     {},
     Stat::FilesCleaned},

    // ── NEW：删除安装时新建的文件（含 dangling symlink、含符号链接环）──────────────
    {WALOpType::NEW,
     Undo::RemoveFile,
     {ARG1, false},
     {},
     Guard::TargetTaken,
     Confine::Arg1Only,
     {"RESTORE_FILE_RM", {ARG1, false}, {}},
     Stat::FilesCleaned},

    // ── NEW_DIR：删除安装时新建的**空**目录 ──────────────────────────────────────
    // 写入侧**恒带尾斜杠**（OpSink::new_dir 按原样记录归档条目形态）。尾斜杠让
    // exists/is_directory/is_empty **跟随**末尾符号链接，于是"链接目标恰好是空目录"会被判成
    // "我们建的那个目录空了"——判据落在链接的目标上，路径对象却是链接。加守卫是为了让意图
    // 显式、并与 DIR_RM 分支和 OpSink::remove_empty_dir 对齐（那两处都剥了尾斜杠并判
    // is_symlink，只这里漏）。
    // ⚠️ 别把这里写成"会把链接目标目录删掉"：`rmdir(2)` 对带尾斜杠的符号链接由内核直接
    // 拒绝（ENOTDIR，**不跟随**），所以修复前此形态实际是**静默 no-op**（不删、也不写
    // RESTORE_DIR_RM）——是"该删的没删"，不是"删错"。真正可观测的差异在**无尾斜杠**形态：
    // `fs::remove` 会 unlink 掉链接本身（进而让回滚把它当"我们建的目录"抹掉）。
    // （2026-09-25 据真实 syscall 行为订正。）
    // 审计行记的是**原样**的 arg1（含尾斜杠）—— 与 DIR_RM 的 RESTORE_DIR 不同，别统一。
    // 本行**不计统计量**（今天就是如此）。
    {WALOpType::NEW_DIR,
     Undo::RemoveEmptyDir,
     {ARG1, true},
     {},
     Guard::TargetIsRealDir,
     Confine::Arg1Only,
     {"RESTORE_DIR_RM", {ARG1, false}, {}},
     Stat::None},

    // ── DB / DBRM：从 DB 备份还原（备份侧由 `DB_BAK` 派生，不是 WAL 字段）──────────
    // bak 不存在 → WAL 已写但备份未完成 → 原文件还在 → 跳过（**幂等**）。
    // 判据用**跟随语义 + 不抛**的 `exists_follow`（2026-09-26 订正：原文写"判据用**抛**的
    // `fs::exists`：原样保留"）—— 保留跟随语义是**有意的**（DB 文件可以是符号链接，管理员
    // 把 `state_dir` 搬走时常见），去掉的只是"抛"：备份路径被符号链接环占着时抛 `fs::exists`
    // 会以 ELOOP 打断整条回滚，而回滚路径上的判定绝不能有能力打断事务。
    {WALOpType::DB,
     Undo::RestoreDb,
     {ARG1, false},
     {DB_BAK, false},
     Guard::DbBakExists,
     Confine::Arg1Only,
     {"RESTORE_DB", {DB_BAK, false}, {ARG1, false}},
     Stat::DbRestored},
    {WALOpType::DBRM,
     Undo::RestoreDb,
     {ARG1, false},
     {DB_BAK, false},
     Guard::DbBakExists,
     Confine::Arg1Only,
     {"RESTORE_DB", {DB_BAK, false}, {ARG1, false}},
     Stat::DbRestored},

    // ── DBNEW：两支，**互斥**（第一支做过就不再走第二支）──────────────────────────
    // ① 有备份 → 恢复备份（与 DB 的逆操作相同）
    // ② 无备份 → DB 文件是全新创建的 → 删除以恢复安装前状态
    {WALOpType::DBNEW,
     Undo::RestoreDb,
     {ARG1, false},
     {DB_BAK, false},
     Guard::DbBakExists,
     Confine::Arg1Only,
     {"RESTORE_DB", {DB_BAK, false}, {ARG1, false}},
     Stat::DbRestored},
    {WALOpType::DBNEW,
     Undo::RemoveFile,
     {ARG1, false},
     {},
     Guard::TargetExists,
     Confine::Arg1Only,
     {"RESTORE_DB_RM", {ARG1, false}, {}},
     Stat::FilesCleaned},
};

/// 表里一行引用的路径字面（审计行渲染、动作、判据共用同一个来源）
///   `field` = `ARG1`/`ARG2` → 行内字段；`DB_BAK` → 派生路径；0 → 本行没有这一侧
fs::path arg_path(const WALOp& op, const ArgRef& ref)
{
    if (ref.field == 0) return {};
    if (ref.field == DB_BAK) return db_bak_path(op.arg1, op.arg2);
    const std::string& s = (ref.field == ARG1) ? op.arg1 : op.arg2;
    return ref.strip ? strip_trailing_slash(s) : fs::path(s);
}

/// 表里属于 `t` 的行区间（表**按类型分组** —— 同类型的行在表里连续，见 UNDO_TABLE 说明）
struct RowRange {
    const UndoRow* first;
    const UndoRow* last;
};

RowRange rows_of(WALOpType t)
{
    const UndoRow* b = std::begin(UNDO_TABLE);
    const UndoRow* e = std::end(UNDO_TABLE);
    while (b != e && b->type != t) ++b;
    const UndoRow* first = b;
    while (b != e && b->type == t) ++b;
    return {first, b};
}

/// 表里 `guard` 列的实现（幂等口径）。**永不抛。**
bool guard_ok(Guard g, const fs::path& orig, const fs::path& bak)
{
    switch (g) {
        case Guard::Always:
            return true;
        case Guard::BakNameTaken:
            return exists_no_follow(bak);
        case Guard::OrigTakenInStash:
            return !orig.empty() && exists_no_follow(orig) && !bak.parent_path().empty() &&
                   exists_no_follow(bak.parent_path());
        case Guard::TargetTakenNotDir:
            // 「名字被占着」与「**不是**真目录」都走 lstat 形态（`is_real_directory`）——
            // 两者都不抛：回滚路径上的判定绝不能有能力打断事务。
            return exists_no_follow(orig) && !is_real_directory(orig);
        case Guard::TargetIsRealDir:
            return is_real_directory(orig);
        case Guard::TargetTaken:
            return exists_no_follow(orig);
        case Guard::DbBakExists:
            // **跟随**语义的不抛孪生（`fs::exists` 的判否等价物）—— **不是** `exists_no_follow`：
            // 这两格的历史口径就是 `fs::exists`（跟随末段链接），换成 lstat 语义会**改变答案**
            // （DB 文件可以是符号链接：管理员把 `state_dir` 搬到别处时常见）。原先是**抛**的
            // `fs::exists`，与上面那条"**永不抛**"的纪律直接冲突 —— 备份路径被符号链接环占着时
            // 它会抛 ELOOP，而回滚路径上的判定绝不能有能力打断事务（2026-09-26 修）。
            return exists_follow(bak);
        case Guard::TargetExists:
            return exists_follow(orig);  // 同上：跟随语义 + 不抛
        case Guard::RealDir:
            return is_real_directory(orig);
        case Guard::TakenNotSymlink:
            return exists_no_follow(orig) && !fs::is_symlink(orig);
    }
    return false;
}

/**
 * `XATTR_*` 行里的键：base64 → 字符串。非法（空串 / 非字母表字符 / 长度不对）→ nullopt。
 *
 * **不抛**：这是回滚路径上的输入，而回滚路径上的判定与解码绝不能有能力打断事务
 * （与 `guard_ok` 同一条纪律 —— 解不开就当"这一行不能用"，跳过它继续撤别的）。
 * 空串判非法是**故意的**：xattr 键名不允许为空（`lsetxattr` 对空键返回 EINVAL），
 * 所以空串只可能来自被写坏/截断的行。
 */
std::optional<std::string> decode_b64_text(const std::string& s)
{
    if (s.empty()) return std::nullopt;
    const auto bytes = base64_decode(s);
    if (!bytes) return std::nullopt;
    return std::string(bytes->begin(), bytes->end());
}

/// 表里 `undo` 列的实现。返回**是否真的动了盘**（false ⇒ 不计数、不写审计行）。
/// 幂等判据（guard）与"动作没做成"是两件事：guard 过了但动作没做成（如 rmdir 因
/// ENOTEMPTY 失败）同样不算动过盘 —— 计数与审计行都只描述**实际发生**的动作。
bool perform_undo(Undo u, const WALOp& op, const fs::path& orig, const fs::path& bak)
{
    switch (u) {
        case Undo::RestoreFromBak:
        case Undo::ReStash: {
            // 方向由 undo 列决定（备份类：bak → orig；UNSTASH：orig → bak）
            const bool from_bak = (u == Undo::RestoreFromBak);
            ::safe_rename(from_bak ? bak : orig, from_bak ? orig : bak);
            return true;
        }
        case Undo::RecreateDir: {
            if (orig.empty()) return false;
            std::error_code ec;
            // 逆序保证父目录已重建（`create_directories` 连父一起建）
            if (!exists_no_follow(orig)) fs::create_directories(orig, ec);
            // 解不开的路径在这里判"不存在"，交给 create_directories 去撞真实错误
            // （错误码进 ec）；`is_real_directory` 不抛，两者一起守住元数据块。
            if (ec || !is_real_directory(orig)) return false;
            uid_t uid = static_cast<uid_t>(-1);
            gid_t gid = static_cast<gid_t>(-1);
            mode_t mode = static_cast<mode_t>(-1);
            try {
                if (!op.arg2.empty()) mode = static_cast<mode_t>(std::stoul(op.arg2)) & 07777;
                if (!op.arg3.empty()) uid = static_cast<uid_t>(std::stoul(op.arg3));
                if (!op.arg4.empty()) gid = static_cast<gid_t>(std::stoul(op.arg4));
            } catch (const std::exception&) {
            }
            if (uid != static_cast<uid_t>(-1) && gid != static_cast<gid_t>(-1))
                (void)::lchown(orig.c_str(), uid, gid);
            if (mode != static_cast<mode_t>(-1)) (void)::chmod(orig.c_str(), mode);
            return true;
        }
        case Undo::RemoveFile:
            // 删除失败**不算错误**（与守卫同向：该路径上已经没有可删的东西了）
            safe_remove(orig);
            return true;
        case Undo::RemoveEmptyDir: {
            std::error_code ec;
            if (!fs::is_empty(orig, ec)) return false;
            fs::remove(orig, ec);
            return !ec;  // rmdir 真失败 ⇒ 没动过盘（不写 RESTORE_DIR_RM 谎报）
        }
        case Undo::RestoreDb:
            ::safe_rename(bak, orig);
            return true;
        case Undo::RestoreDirMeta: {
            if (orig.empty()) return false;
            // 与 RecreateDir 的元数据半段同一套解析/施加方式（mode/uid/gid 都是十进制，
            // 由 `dir_meta()` 写行时从 lstat 得来）。区别只有一个：**不 create_directories**
            // —— 这里要还原的是"本来就存在的目录"的元数据，目录缺失说明它已被别的逆操作
            // 处理掉（或无人在意），凭空重建它会造出一个本不该存在的路径。
            uid_t uid = static_cast<uid_t>(-1);
            gid_t gid = static_cast<gid_t>(-1);
            mode_t mode = static_cast<mode_t>(-1);
            try {
                if (!op.arg2.empty()) mode = static_cast<mode_t>(std::stoul(op.arg2)) & 07777;
                if (!op.arg3.empty()) uid = static_cast<uid_t>(std::stoul(op.arg3));
                if (!op.arg4.empty()) gid = static_cast<gid_t>(std::stoul(op.arg4));
            } catch (const std::exception&) {
            }
            if (uid != static_cast<uid_t>(-1) && gid != static_cast<gid_t>(-1))
                (void)::lchown(orig.c_str(), uid, gid);
            if (mode != static_cast<mode_t>(-1)) (void)::chmod(orig.c_str(), mode);
            return true;
        }
        case Undo::SetXattr: {
            const std::optional<std::string> key = decode_b64_text(op.arg2);
            if (!key) return false;  // 行被写坏/非 base64 → 不动盘（回滚路径绝不抛）
            // 值的三种形态：`-` = 空值（哨兵，见 xattr_b64_value 的说明）、base64 文本 = 有值。
            std::vector<char> val;
            if (op.arg3 != "-") {
                const auto decoded = base64_decode(op.arg3);
                if (!decoded) return false;
                val = *decoded;
            }
            return write_xattr(orig, *key, val);
        }
        case Undo::RemoveXattr: {
            const std::optional<std::string> key = decode_b64_text(op.arg2);
            if (!key) return false;
            // 键本来就不在 → write_xattr/remove_xattr 都返回 false，"没动过盘" ⇒ 不写审计行。
            return remove_xattr(orig, *key);
        }
    }
    return false;
}

/**
 * 施加表里的一行 —— **三件事在这里统一做**：
 *   ① 幂等：`guard` 列（"目标不存在/形态不符 → 跳过"）；
 *   ② 动作：`undo` 列（方向由它定义；返回值 = 是否真的动了盘）；
 *   ③ 记录：`stat` 列计数、`audit` 列写 RESTORE_* 审计行（**记实际动作方向**）。
 * 逐 op 的差异只剩表里那几格。
 *
 * @return 本行是否真的动了盘（调用方据此决定要不要继续扫同一类型的后续行）
 */
bool apply_row(const UndoRow& row, const WALOp& op, bool write_audit, RollbackStats& stats)
{
    const fs::path orig = arg_path(op, row.orig);
    const fs::path bak = arg_path(op, row.bak);

    if (!guard_ok(row.guard, orig, bak)) return false;
    if (!perform_undo(row.undo, op, orig, bak)) return false;

    switch (row.stat) {
        case Stat::None:
            break;
        case Stat::FilesRestored:
            stats.files_restored++;
            break;
        case Stat::FilesCleaned:
            stats.files_cleaned++;
            break;
        case Stat::DirsRecreated:
            stats.dirs_recreated++;
            break;
        case Stat::DbRestored:
            stats.db_restored++;
            break;
    }

    if (write_audit && row.audit.op != nullptr) {
        std::string line = row.audit.op;
        line += ' ';
        line += arg_path(op, row.audit.first).string();
        if (row.audit.second.present()) {
            line += " \xe2\x86\x92 ";
            line += arg_path(op, row.audit.second).string();
        }
        wal_append_raw(line);
    }
    return true;
}

}  // namespace

// ============================================================================
// 回滚侧的路径 confinement（纵深防御）
// ============================================================================

namespace
{
/// 分量级前缀比较（`/a/bc` **不**以 `/a/b` 为前缀）。两侧都应已 lexically_normal()。
bool path_has_prefix(const fs::path& prefix, const fs::path& p)
{
    auto pi = prefix.begin();
    auto qi = p.begin();
    for (; pi != prefix.end(); ++pi, ++qi) {
        if (qi == p.end() || *pi != *qi) return false;
    }
    return true;
}

/**
 * `p` 是否落在 `root` 之内（`reverse_execute` 的 confinement 判据）。**永不抛。**
 *
 * 两级判据：
 *  ① **词法级**：`p.lexically_normal()` 必须落在 `root.lexically_normal()` 之内。不碰
 *     文件系统，负责挡掉 `../` 逃逸与"绝对路径就指在 root 之外"（WAL 行是纯文本，
 *     `NEW /etc/sudoers` 这种行在坏 WAL / 被篡改的 WAL 里是一行字而已）。
 *  ② **canonical 复核**（尽力而为，**只解析父目录**）：`weakly_canonical(root)` 与
 *     `weakly_canonical(p.parent_path())` 都成功时再比一次 —— 挡的是词法上合法、实际却
 *     穿透出去的那种：`<root>/evil -> /etc` 配上 `<root>/evil/shadow`。
 *     **末段不解析**：回滚的动作（rename/unlink/rmdir）不跟随末段链接，而 `--root` 安装里
 *     包发绝对目标链接（`<root>/usr/bin/foo -> /etc/foo`）完全合法 —— 解析它会误伤。
 *
 * **解不开就不判越界**（`ec != 0` → 放行）：回滚路径上的判定绝不允许因为"这个路径解不开"
 * 而拒绝一条合法的行。典型是 ELoop 自环 —— 那份备份必须能被 `BACKUP` 的逆操作搬回原位
 * （`tests/integration/test_symlink_loop_install.cpp` 钉着这条），而 canonical 对环恒失败。
 * 同理，中间段尚未重建的 `DIR_RM`（`create_directories` 之前）也解不开。
 */
bool path_within_root(const fs::path& root, const fs::path& p)
{
    if (root.empty() || p.empty()) return true;  // 空路径的含义由各分支自己处理
    const fs::path r = root.lexically_normal();
    const fs::path q = p.lexically_normal();
    if (!path_has_prefix(r, q)) return false;

    // canonical 复核**只解析父目录，末段一律不解析**：
    //   · 回滚侧对这些路径做的是 rename/unlink/rmdir，**内核不跟随末段符号链接** ——
    //     末段指向哪里与被动的那个对象无关（与 op_sink.hpp 里"尾斜杠/末段链接"那条同一课）；
    //   · 解析末段还会**误伤合法 WAL**：`--root` 安装里包可以发绝对目标符号链接
    //     （`<root>/usr/bin/foo -> /etc/foo`），解析它在宿主上落到 root 之外 —— 而它
    //     指向的是**目标系统**里的 `/etc/foo`，完全合法（`NEW`/`COPY` 的逆操作删的就是那个
    //     链接本身）。所以判据落在"**它所在的目录**是不是在 root 里"。
    std::error_code ec_r;
    std::error_code ec_q;
    const fs::path cr = fs::weakly_canonical(r, ec_r);
    const fs::path cq = fs::weakly_canonical(q.parent_path(), ec_q);
    if (ec_r || ec_q) return true;  // 解不开（ELOOP 等）→ 不判越界（见上）
    return path_has_prefix(cr, cq);
}

/**
 * 一条 WAL 行的**全部目标路径**是否都在允许范围内 —— 不在就跳过该行（并告警）。
 *
 * 允许范围有两类：`Config::instance().root_dir()`（WAL 描述的一切都在这棵树下）与
 * **已知的 stash 根**（`referenced_stash_roots()`，即 `.lpkg_bak_*` 目录；备份侧的目标）。
 * 后者在当前实现里本就落在 root 之内（`stash_parent_dir` 恒 clamp 在 root 内），把它
 * 单列是为了**不误拒**：判据写成"root ∪ stash 根"，将来 stash 落点若变也不会把合法行
 * 拒掉。
 *
 * **查哪些字段由 `scope` 给出**（= 撤销表里该 op 类型的 `confine` 列，逐类型判据不再写在
 * 本函数里）。表里的取值只列**可逆**的那些（`CLEANUP`、`RESTORE_` 审计行与元数据行不进
 * reverse，故不在表里）：
 *   · `BACKUP` / `REMOVE_OLD` / `SAVE_CONF` / `UNSTASH` / `COPY` → `Arg1AndArg2`
 *     （原位与备份/stash 侧、或 `.lpkgtmp` 与落点，两侧都要合法）；
 *   · `NEW` / `NEW_DIR` / `DIR_RM` / `DB` / `DBNEW` / `DBRM` → `Arg1Only`
 *     （arg1 就是逆操作要删/重建/还原的那个路径；DB 家族的备份侧由它派生）。
 *
 * 保守方向与"bak 不存在 → 跳过"完全一致：**告警 + 跳过这一行，绝不因此让恢复失败**
 * —— 恢复失败意味着每次启动都重试、所有 lpkg 命令起不来。
 */
bool wal_line_paths_confined(const WALOp& op, Confine scope, const fs::path& root,
                             const std::set<fs::path>& stash_roots)
{
    const auto ok = [&](const std::string& s) {
        if (s.empty()) return true;
        const fs::path p(s);
        if (path_within_root(root, p)) return true;
        for (const auto& sr : stash_roots) {
            if (path_within_root(sr, p)) return true;
        }
        return false;
    };

    switch (scope) {
        case Confine::None:
            return true;  // 该行不碰文件系统（或不进 reverse），无需 confinement
        case Confine::Arg1Only:
            return ok(op.arg1);
        case Confine::Arg1AndArg2:
            return ok(op.arg1) && ok(op.arg2);
    }
    return true;
}
}  // namespace

/// 该 :batch-start DB 行的正式文件是否**仍然持有批次起点的内容**（= 这行可以跳过）///
/// 判据不只是 fs::exists —— main 的启动顺序是 init_filesystem()（main.cpp 约 397 行）→
/// recover_packages()（约 400 行），而 init_filesystem 的 ensure_file_exists 会把崩溃窗口里
/// 消失的库**按空文件重建**（config.cpp）。于是崩在窗口里的库到恢复时是"存在但 0 字节"，
/// 只看 fs::exists 就又把它跳过去了 —— 后果与"文件缺失"完全相同（静默空库 + 唯一备份被
/// cleanup_db_backups 删掉，不可逆），而且这才是**真实二进制**的形态。故：
///
///   - 文件不存在        → 不能跳过（内容没了，备份是唯一依据）
///   - 文件非空          → 跳过（正常路径的语义一字不变）
///   - 文件空 + 备份空   → 跳过（批次起点本来就是空库，正式文件与备份等价，跳过/还原同效）
///   - 文件空 + 备份非空 → **不**跳过（只可能是内容丢了，从备份还原）
///
/// 最后一条不会误伤正常路径：处理到本行时，正式文件已被"更晚各里程碑的逆操作"带回批次
/// 起点状态（= 备份里那份内容）；真正合法的空正式文件 ⟹ 批次起点是空库 ⟹ 备份也是空的。
static bool batch_start_db_still_in_place(const WALOp& op)
{
    std::error_code ec;
    if (!fs::exists(op.arg1, ec) || ec) return false;
    const auto official_size = fs::file_size(op.arg1, ec);
    // stat 失败（异常文件类型等）保守放行：保持既有"在位即跳过"的行为，不在这里发明新语义
    if (ec || official_size > 0) return true;
    const auto bak_size = fs::file_size(db_bak_path(op.arg1, op.arg2), ec);
    return ec ? true : bak_size == 0;  // 备份不存在/无法 stat → 没有可还原的东西 → 跳过
}

RollbackStats reverse_execute(const std::vector<WALOp>& ops, bool write_audit)
{
    RollbackStats stats;

    // ── 路径 confinement（纵深防御）────────────────────────────────────────────
    // 输入端已经堵死（归档成员名/包名/版本号都消毒，见 installation_task.cpp /
    // installation_task_letgo.cpp 的说明）， 要触发得先能写 WAL 文件 —— 那已经是
    // root。所以这是**纵深防御**：万一 WAL 被篡改 （别的工具、编辑器、恶意 root
    // 服务），回滚**不许**照着行里的字面路径去 rename/remove 系统上任意位置的东西。判据见
    // wal_line_paths_confined()。
    //
    // 只在"真的可能出现越界"时才付检索代价：root_dir() 是 `/` 时任何绝对路径都在其内，
    // 收集 stash 根（要读一遍 WAL 文件）没有意义 —— 直接跳过整套检查。
    const fs::path confine_root = Config::instance().root_dir();
    const bool confine_enabled = !confine_root.empty() && confine_root != fs::path("/");
    std::set<fs::path> stash_roots;
    if (confine_enabled) stash_roots = referenced_stash_roots();

    // 逆序遍历
    for (int i = static_cast<int>(ops.size()) - 1; i >= 0; --i) {
        const auto& op = ops[i];

        // 跳过未解析行（skip_in_reverse 已含 INVALID）与元数据/RESTORE 审计行
        if (op.skip_in_reverse()) continue;

        // 本 op 类型在撤销表里的行（同类型多行的按表序执行；无行 = 本类型不可逆）
        const RowRange rows = rows_of(op.type);

        // 越界的行：**告警 + 跳过**（与"bak 不存在 → 跳过"同一个保守方向）——绝不因此让
        // 恢复失败。措辞刻意不走 l10n（内部/安全诊断；且 test_localization_keys.cpp 会把
        // 「日志函数名(字面量)」当成 l10n 键，故用变量传）。
        // 查哪些字段由表里的 confine 列给出 —— 同一类型的各行共用这一格（取首行即可）。
        if (confine_enabled && rows.first != rows.last &&
            !wal_line_paths_confined(op, rows.first->confine, confine_root, stash_roots)) {
            const std::string msg =
                "回滚跳过一条路径越界的 WAL 行（confinement，root=" + confine_root.string() +
                "）：" + op.raw;
            log_warning(msg);
            continue;
        }

        // :batch-start DB 条目（最终状态标记）—— **仅当正式文件仍在（仍持有批次起点内容，
        // 判据见 batch_start_db_still_in_place）时**跳过。
        //
        // 为什么不能无条件跳过：write_db_file_wal / write_set_file_wal（cache.cpp）的序列是
        //   WAL 行 → rename(正式名 → .lpkg_db_bak_before:<milestone>) → 写 .tmp → fsync →
        //   rename(.tmp → 正式名)
        // 批次开头的 5 个 DB 写入（pkgs/files.db/provides.db/confhashes.db/holdpkgs）全是
        // :batch-start 里程碑。进程若死在"正式名已消失、.tmp 还没 rename 回来"这个窗口里
        // （SIGKILL/OOM/段错误/掉电），盘上就只剩那份备份 —— 无条件跳过等于它**永远无人
        // 消费**，两个后果都不可逆：pkgs/holdpkgs 缺失让 read_set_from_file 抛异常、整个
        // recover_packages() 失败；files.db/provides.db/confhashes.db 缺失被 read_db_uncached
        // 静默当空表 → 归属归零，且紧接着 cleanup_db_backups() 会把唯一备份删掉。
        //
        // 放宽不会破坏既有语义：处理到本行时，正式文件已被"更晚各里程碑的逆操作"带回批次
        // 起点状态（WAL 里没有更晚的 DB 行时，正式文件压根没被动过，仍然是批次起点状态）——
        // 正是下面 DB/DBNEW/DBRM 分支要从备份里还原出来的那个状态。所以"文件在位 → 跳过"与
        // "从备份还原"在正常路径上**等价**；而"正式文件缺失 + 该里程碑的备份存在"只可能来自
        // 上面那个崩溃窗口（WAL 行先写、rename 后做，文件缺失必是 rename 之后死的）。
        // 本条不改任何既有策略：备份仍是每里程碑一份、.lpkgsave/三哈希/冲突判据一律不碰。
        //
        // "在位"的判据见 batch_start_db_still_in_place（存在**且仍持有批次起点内容**：
        // init_filesystem 会把窗口里消失的库按空文件重建，"存在但空"同样是内容丢了）。
        if (is_batch_start_milestone(op) && batch_start_db_still_in_place(op)) continue;

        // 逐行施加撤销表 —— 三件事（幂等判据 / 撤销动作 / 计数 + 审计行）都在 apply_row
        // 里统一做，本循环只负责"同类型多行"的调度：
        //   · `also = true` 的行做完后**继续**扫后续行（COPY 的"删落点"与"清残留"互相独立，
        //     第一支没做也不影响第二支）；
        //   · 其余行第一支成立就停（DBNEW 的两支互斥：有备份则还原，否则才删新建的）。
        for (const UndoRow* row = rows.first; row != rows.last; ++row) {
            if (apply_row(*row, op, write_audit, stats) && !row->also) break;
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
    // 元数据（dep/needed_so/man 记录）与 DB 同族：单文件、同样由 cleanup_db_backups
    // 在批次提交后删掉 .lpkg_db_bak_before:*，丢了就是依赖图归零 → 永远持久化，
    // 不受 durable_fsync_enabled()（默认关闭）影响。
    DurableFsyncGuard durable;
    const fs::path p(path);
    // `exists_no_follow`（2026-09-26 修）：判据要的是"**这个名字**上有没有东西"，而悬空链接
    // 同样占着名字。用跟随语义会把悬空链接当成"没有旧内容" ⇒ 写 `DBNEW`（**无备份**）⇒
    // 回滚的 `Undo::RemoveFile` 只 unlink 新文件，**原来的悬空链接永久消失**（而不是"还原"）。
    const bool is_new = !exists_no_follow(p);

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

fs::path stash_root_of_bak(const fs::path& bak_in)
{
    // **先剥尾斜杠**（2026-09-26 加）：今天 WAL 字面量恒不带尾斜杠（`new_dir`/`new_file` 按
    // 原样记录、其余调用点各自剥过），但那条只是**君子协定**；而调用方（`purge_consumed_stashes`
    // 等）会拿本函数的返回值去 `fs::remove_all`，实测 **`fs::remove_all("link/")` 会删光链接
    // 目标的全部内容**（返回 ENOTDIR）⇒ 尾斜杠一旦出现，"判不出 stash 根 → 原样返回自己" 就会
    // 变成灾难。剥掉不改变任何今天的答案（判据本来就要求末段是那个 stash 目录名）。
    const fs::path bak = strip_trailing_slash(bak_in);
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

    // ── 「已消费」判据在 UNSTASH 引入后的复核（见 `ARCH.md` §9.2 的 `UNSTASH` 行）──────
    //
    // 本函数的前提是"reverse 已把每个 bak 还原干净"，它据此**整目录** remove_all。引入
    // UNSTASH 后有两件事要交代清楚：
    //
    // ① **UNSTASH 的 bak 不需要单独贡献 stash 根**（故上面的收集仍是 BACKUP/REMOVE_OLD
    //    两种）。UNSTASH 与同一条路径的 BACKUP 在**同一个批次**里成对出现，而 BACKUP 的
    //    arg2 与 UNSTASH 的 arg1 在同一个 stash 根下 ⇒ 那个根已经被收集了。反过来，若拿
    //    UNSTASH 当触发器，单独的 UNSTASH 行（"这份东西刚从 stash 搬回原位"）会让本函数
    //    去删它所在的 stash —— 那是**激进**方向（可能连带删掉同根下别的 bak），不做。
    //
    // ② "reverse 已完成 ⇒ 可以删"这条推论的**唯一破法**是 UNSTASH 的逆操作不收敛：它把
    //    原位那份 rename 进 bak，紧接着同一路径的 BACKUP 逆操作再把它搬出来 —— 两次
    //    rename 回到原点，幂等成立。任何一步没做完（bak 的父目录被别的流程收尸了、
    //    WAL 里只有 UNSTASH 没有 BACKUP），bak 就会**留在这个根里**，此时整目录 remove_all
    //    会把唯一一份数据删掉。所以这里加一道**收敛检查**：UNSTASH 引用的 bak 在 reverse
    //    之后**仍然存在** ⇒ 不收敛 ⇒ 保留该根（宁可留残留，也不删未收敛的数据）。
    //    正常路径下这个分支不可达（每个 UNSTASH 的 bak 都会被它的 BACKUP 逆操作搬走）。
    std::set<fs::path> unconverged;
    for (const auto& op : ops) {
        if (op.type != WALOpType::UNSTASH || op.arg1.empty()) continue;
        const fs::path bak = op.arg1;
        if (!exists_no_follow(bak)) continue;
        unconverged.insert(stash_root_of_bak(bak));
        // 内部一致性告警，**刻意不走 l10n**：这条永远不该出现（出现即"回滚没收敛"这个
        // 编程错误/环境异常），与 op_sink.cpp 的决策表内部错误同一口径、同一理由。
        // 同样不把字面量直接喂给日志函数：`test_localization_keys.cpp` 的正则会把
        // 「日志函数名(字面量)」当成 l10n 键去翻译目录里查（连注释都不放过）——用变量传。
        const std::string msg =
            "反撤销未收敛：UNSTASH 的备份仍在 stash 中，保留整个 stash 目录不删"
            "（否则会丢掉唯一一份数据）：" +
            bak.string();
        log_warning(msg);
    }

    for (const auto& s : stashes) {
        if (unconverged.contains(stash_root_of_bak(s))) continue;
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

int open_wal_append(bool& created)
{
    const std::string path = wal_log_path();
    created = false;

    // 先按 O_CREAT|O_EXCL 试创建：成功 = **本次调用**创建了 WAL 文件（父目录的 dentry
    // 只有这一刻需要落盘）；EEXIST = 文件本来就在（dentry 早已落过盘）→ 退回普通追加打开。
    //
    // 不用"先 fs::exists 再 open"：那是 TOCTOU 判定（两次系统调用之间文件可能被创建/
    // 删除），而且多一次 stat。这里两次 open 的判定是原子的，且**创建与打开用的是同一
    // 个 O_CREAT 语义** —— 原实现就是"不存在则创建"，这里只是把"创建了"这件事显式化。
    int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC,
                    constants::PERM_WAL_LOG);
    if (fd >= 0) {
        created = true;
        return fd;
    }
    if (errno != EEXIST) return fd;  // 其他错误（ENOENT/权限…）交给调用方按原口径报错

    return ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC, constants::PERM_WAL_LOG);
}

}  // namespace wal
