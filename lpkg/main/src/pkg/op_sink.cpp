#include "op_sink.hpp"

#include <sys/stat.h>

#include <map>
#include <string>
#include <utility>

#include "base/constants.hpp"
#include "base/exception.hpp"
#include "base/utils.hpp"
#include "db/test_breakpoints.hpp"
#include "db/transaction_log.hpp"
#include "i18n/localization.hpp"
#include "install_common.hpp"

namespace fs = std::filesystem;

namespace
{
/// 移位名：`<dst>.<N>`（pacman 的 shift_pacsave 给已有 .pacsave 找空位）
fs::path shifted_save_name(const fs::path& dst, unsigned n)
{
    return fs::path(dst.string() + "." + std::to_string(n));
}

/// 移位搜索上限。pacman 用 INT_MAX；这里给个有限上界，免得病态目录把移除挂死。
/// 真到上限宁可抛错让整批失败（回滚会把配置还原回原位），也绝不覆盖已有存档。
constexpr unsigned MAX_CONFIG_SAVE_SHIFTS = 10000;
}  // namespace

namespace detail
{

OpSink::OpSink(std::string pkg, std::vector<fs::path>* stashes)
    : pkg_(std::move(pkg)), stashes_(stashes)
{
}

fs::path OpSink::backup_impl(const fs::path& phys, std::string_view op,
                             std::string_view after_wal_breakpoint)
{
    // 物理路径先规范化：调用点传进来的可能是 DB 目录键形态（`/x/y/`）或归档条目形态。
    // 尾斜杠会让**判定类**调用（`lstat`/`is_symlink`/`is_empty`/`exists`）落到末尾符号链接的
    // **目标**上，而 WAL 行记的必须是"我们真正动了哪个对象"——两侧理解不一致正是 §3.6.1 第 1 条
    // 那类事故的来源。规范化只在这里做一次，调用点不管。
    // （2026-09-25 订正：原文把 `rename` 也算进"落到目标上"，实测不成立——`rename("link/","x")`
    //  被内核以 ENOTDIR 拒绝，末段符号链接一律不跟随；穿透的只有 chmod/lchown 与判定类调用。）
    const fs::path src = strip_trailing_slash(phys);
    const fs::path bak = stash_bak_target(src, pkg_);

    // write-ahead：先写行（= 承诺这次备份会发生），再做 rename。
    // 崩溃在两者之间 → 行在、bak 不在 → 回滚侧"bak 不存在 → 跳过"（原文件仍在原位，幂等）。
    wal::log_wal_line(std::string(op) + " " + src.string() + " \xe2\x86\x92 " + bak.string());

    if (!after_wal_breakpoint.empty())
        BreakpointManager::instance().hit(std::string(after_wal_breakpoint));

    safe_rename(src, bak);

    // stash 记账：批次提交后 cleanup_stashes 要按这些根把备份整目录删掉。
    // （不在这里去重 —— 清理侧本来就会 sort+unique，这里重复只影响内存里的向量长度。）
    if (stashes_) stashes_->emplace_back(bak.parent_path());
    return bak;
}

fs::path OpSink::backup(const fs::path& phys, std::string_view after_wal_breakpoint)
{
    return backup_impl(phys, "BACKUP", after_wal_breakpoint);
}

fs::path OpSink::backup_obsolete(const fs::path& phys, std::string_view after_wal_breakpoint)
{
    // 升级的废弃文件与"覆盖已有文件"的回滚语义完全一致（reverse_execute 里 BACKUP 与
    // REMOVE_OLD 走同一分支），只有 WAL 关键字不同 —— 关键字不同是为了让审计能区分
    // "被覆盖"与"新版本不再包含"，因此不能合并成一个方法名。
    return backup_impl(phys, "REMOVE_OLD", after_wal_breakpoint);
}

void OpSink::new_file(const fs::path& phys)
{
    // 纯日志记录（文件由 commit_copy 落位）。路径**原样**记录：这是一条会被回滚侧
    // 逐字消费的契约，不能在这里改动字面形态（见头文件契约 2）。
    wal::log_wal_line("NEW " + phys.string());
}

fs::path OpSink::save_config(const fs::path& phys, std::string_view after_wal_breakpoint)
{
    const fs::path src = strip_trailing_slash(phys);
    const fs::path dst(src.string() + std::string(constants::SUFFIX_LPKG_SAVE));

    // 目标名已被占用（同一路径被移除过多次）→ **不覆盖**，先把旧的移位到第一个空闲的
    // `<dst>.<N>`（N 从 1 起，pacman shift_pacsave）。移位本身也是一条 SAVE_CONF
    // （写了就必须能回滚：逆操作 = rename 回来），因此照样 write-ahead。
    //
    // **顺序是安全性的关键：移位必须严格先于本次改名。** 这样"本次改名的 dst"在这条行
    // 写下时**还不存在** —— 万一崩在 write-ahead 窗口（行已落、rename 未做），回滚侧对
    // 本条的反向 rename 找不到 dst，是 no-op。若反过来（先改本次名、再移位），回滚就会把
    // **上一次**留下的旧 .lpkgsave rename 到配置原位，用它盖掉真正的配置（数据丢失）。
    // 判据一律用不抛的 `exists_no_follow`（2026-09-26 修）：`fs::exists(p, ec) ||
    // fs::is_symlink(p)` 这条写法在**中间段**成环时必抛（`fs::exists` 带 ec 对 ELOOP 返回
    // false ⇒ `||` 必然求值抛型的右操作数），而 `exists_no_follow` 要的正是它想表达的语义
    // ——"这个名字被任何东西占着"（含悬空链接与环），且不抛。
    if (exists_no_follow(dst)) {
        unsigned n = 1;
        fs::path shifted = shifted_save_name(dst, n);
        while (n < MAX_CONFIG_SAVE_SHIFTS && exists_no_follow(shifted))
            shifted = shifted_save_name(dst, ++n);
        if (exists_no_follow(shifted))
            throw LpkgException(string_format("error.config_save_shift_exhausted", dst.string()));
        wal::log_wal_line("SAVE_CONF " + dst.string() + " \xe2\x86\x92 " + shifted.string());
        safe_rename(dst, shifted);
    }

    // write-ahead：先写行（= 承诺这次改名会发生），再做 rename。崩溃在两者之间 →
    // 行在、dst 不在 → 回滚侧"dst 不存在 → 跳过"（原文件仍在原位，幂等）。
    wal::log_wal_line("SAVE_CONF " + src.string() + " \xe2\x86\x92 " + dst.string());

    if (!after_wal_breakpoint.empty())
        BreakpointManager::instance().hit(std::string(after_wal_breakpoint));

    safe_rename(src, dst);

    // 注意：**不记账 stashes_** —— dst 是配置文件原位旁边的兄弟名，批次提交后
    // cleanup_stashes 的 remove_all 只作用于 stash 根，碰不到它（这正是"保留"的实现）。
    return dst;
}

bool OpSink::un_stash(const fs::path& bak, const fs::path& orig,
                      std::string_view after_wal_breakpoint)
{
    // 两个路径都规范化：bak 的形态由 stash_bak_target 决定（不含尾斜杠），orig 可能是
    // 目录键形态（`/etc/a/`）—— 尾斜杠会让判定类调用落到末尾符号链接的**目标**上。
    const fs::path src = strip_trailing_slash(bak);
    const fs::path dst = strip_trailing_slash(orig);

    // 幂等（**bak 不存在 → 跳过**）：没有可搬的东西就什么都不做，原位那份原样不动。
    // 这个分支正常路径上不可达（调用点只在"让开趟确实搬过"时才会走到这里），出现即
    // 说明有东西被外部动过（stash 被手工清掉 / 被更早的一次回滚消费）—— 告警可见，
    // 但**绝不**因此失败：失败会把整批拖进回滚，而回滚同样搬不回那份。
    //
    // 措辞刻意**不走 l10n**（同本文件末尾决策表内部错误那两条）：「永远不该出现」的内部
    // 一致性诊断不需要翻译，而翻译目录少一个键会让它变成 [MISSING_STRING: ...]。
    // 也别把这个字符串**字面量**直接喂给日志函数 —— `test_localization_keys.cpp`
    // 的正则把「日志函数名(字面量)」一律当成 l10n 键（拿变量传就不在扫描口径里，
    // 与 decision_hole_msg() 一类同形；它连注释里的这种写法都会扫到）。
    if (!exists_no_follow(src)) {
        const std::string msg =
            "un_stash: 没有可搬回的备份（stash 里那份已不存在），原位保持不变：" + src.string();
        log_warning(msg);
        return false;
    }

    // write-ahead：先写行（= 承诺这次搬回会发生），再做 rename。崩溃在两者之间 →
    // 行在、src 还在 stash、dst 不存在 → 回滚侧对本条是 no-op（dst 没有可搬的），
    // 随后 BACKUP 的逆操作把 src 搬回 dst —— 两次 rename 收敛回原点（幂等）。
    wal::log_wal_line("UNSTASH " + src.string() + " \xe2\x86\x92 " + dst.string());

    if (!after_wal_breakpoint.empty())
        BreakpointManager::instance().hit(std::string(after_wal_breakpoint));

    safe_rename(src, dst);
    return true;
}

void OpSink::new_dir(const fs::path& phys)
{
    // 纯日志记录（目录由调用方的 create_directories 落位）。同上按原样记录：
    // 目录条目本来就带尾斜杠（DB 目录键的形态，与归档条目一致），剥掉会改变 WAL 字面。
    wal::log_wal_line("NEW_DIR " + phys.string());
}

void OpSink::dir_meta(const fs::path& phys, mode_t mode, uid_t uid, gid_t gid, bool record_previous,
                      std::string_view after_wal_breakpoint)
{
    // 目录键带尾斜杠 → 先规范化：lstat/lchown/chmod 对带尾斜杠的路径会**穿透**末段符号
    // 链接落到目标上（见 base/utils.hpp 的谓词说明），而 WAL 行记的必须是"我们真正动了
    // 哪个对象"。
    const fs::path target = strip_trailing_slash(phys);

    // 前置：真实目录（lstat，非符号链接）。**符号链接与"不是目录"都直接返回**，不写行、
    // 不动盘 —— 符号链接那一格由调用方挡住（穿透改的是链接**目标**，可能是别的包持有的
    // 目录），这里不重复制造一个会静默偏移的落点；"不是目录"说明调用方的守卫失职，
    // 静默返回而不是抛异常（写入趟抛 = 整批回滚，而回滚同样改不动它）。
    struct stat st{};
    if (::lstat(target.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return;

    if (record_previous) {
        // write-ahead：先写行（= 承诺这次改元数据会发生），再做 lchown/chmod。
        // 行里记的是**改前**值 —— 回滚侧（`RESTORE_DIRSTATE` 的 `DIR_META` 行）据此写回。
        // 崩溃在两者之间 → 行在、盘面未变 → 回滚把"改前值"再写一遍（幂等，无副作用）。
        wal::log_wal_line("DIR_META " + target.string() + " " + std::to_string(st.st_mode & 07777) +
                          " " + std::to_string(st.st_uid) + " " + std::to_string(st.st_gid));
        if (!after_wal_breakpoint.empty())
            BreakpointManager::instance().hit(std::string(after_wal_breakpoint));
    }

    (void)::lchown(target.c_str(), uid, gid);
    (void)::chmod(target.c_str(), mode);
}

namespace
{
/// xattr 值的 WAL 编码：base64；**空值用哨兵 `-`**（base64 字母表里没有 `-`，故无歧义）。
/// 不能用空字段 —— 尾部空字段与"这一侧不存在"在分帧上同形（`XATTR_NEW` 就没有第三个字段）。
std::string xattr_value_field(const std::vector<char>& v)
{
    return v.empty() ? std::string("-") : base64_encode(v);
}

/// 键的 WAL 编码：base64（键名不允许为空，故空串不可能来自这里）
std::string xattr_key_field(const std::string& key)
{
    return base64_encode(std::vector<char>(key.begin(), key.end()));
}
}  // namespace

bool OpSink::set_xattr(const fs::path& phys, const std::string& key, const std::vector<char>& value,
                       std::string_view after_wal_breakpoint)
{
    const fs::path target = strip_trailing_slash(phys);
    // 前置同 dir_meta：真实目录、非符号链接（见头文件说明）。
    struct stat st{};
    if (::lstat(target.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;

    // 改前状态决定行类型（见头文件）：**读一次**，而不是让调用方告诉我们 —— 调用方可能
    // 刚写过同一个键，它手上的"事实"会过期；这里的读发生在写之前，就是权威的改前状态。
    const auto old = read_xattr(target, key);
    const std::string b64_key = xattr_key_field(key);
    if (old) {
        wal::log_wal_line("XATTR_SET " + target.string() + " " + b64_key + " " +
                          xattr_value_field(*old));
    } else {
        wal::log_wal_line("XATTR_NEW " + target.string() + " " + b64_key);
    }

    if (!after_wal_breakpoint.empty())
        BreakpointManager::instance().hit(std::string(after_wal_breakpoint));

    // 写盘失败**不抛**：xattr 是"附加信息"（能力位、ACL、SELinux 标签），写不上多半是
    // 文件系统不支持（tmpfs 的老内核 / overlay 的某些配置）—— 为此让整批回滚，等于把
    // "这个 fs 不支持 xattr"变成"这个包装不上"。与 `copy_xattrs` 的既有取向一致
    // （它对每个键都 `(void)::lsetxattr`）。
    // 注意：行已经写了、而写盘失败 ⇒ 回滚会"把改前值写回去"（对本来有值的键是幂等的
    // no-op；对本来没值的键会 `lremovexattr` 一个不存在的键，同样无害）。
    return ::write_xattr(target, key, value);
}

bool OpSink::unset_xattr(const fs::path& phys, const std::string& key,
                         std::string_view after_wal_breakpoint)
{
    const fs::path target = strip_trailing_slash(phys);
    // 前置同 dir_meta。**注意这里与 set_xattr 的同一个判据含义不同**：撤销路径上"目标
    // 已经不是真目录了"（被别的包换成符号链接 / 已被删）是**正常结局**，跳过即正确语义。
    struct stat st{};
    if (::lstat(target.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;

    // 键不在盘上 → 无事可做：**不写行**（写了行回滚就会去"还原"一个从来没被我们改过的
    // 键，`XATTR_SET` 还会把某个恰好同名的键写回一个陈旧值）。
    const auto old = read_xattr(target, key);
    if (!old) return false;

    // write-ahead：改前有值 ⇒ `XATTR_SET`（逆操作 = 把旧值写回去）。**行的语义是"记录
    // 改前状态"，不是"记录这是一次删除"** —— 所以删除与覆盖共用同一个行类型（见 UNDO_TABLE）。
    wal::log_wal_line("XATTR_SET " + target.string() + " " + xattr_key_field(key) + " " +
                      xattr_value_field(*old));

    if (!after_wal_breakpoint.empty())
        BreakpointManager::instance().hit(std::string(after_wal_breakpoint));

    return ::remove_xattr(target, key);
}

DirRemoval OpSink::remove_empty_dir(const fs::path& phys)
{
    // 目录键带尾斜杠 → 必须先规范化，否则下面的 lstat 会**解引用**末尾的符号链接：
    // 对 `/root/var/run/`（真实对象是 `var/run -> ../run`）会读到 /run 的 stat、S_ISDIR 通过，
    // 于是"按链接判、按路径动手"——判定落在目标上；而调用方那侧的 is_symlink 守卫同样假阴性。
    // 两条路都错，且错的是同一个对象。
    // （2026-09-25 订正：原文写"最终 rmdir 把真实 /run 删掉"——**不成立**，实测 `rmdir` 对末段
    //  符号链接一律 ENOTDIR、不跟随；不剥尾斜杠的真实后果是"该删的没删"（静默 no-op）。）
    const fs::path target = strip_trailing_slash(phys);

    // 前置校验：真实目录（lstat，不跟随末段）才继续。非目录 / 已消失 → 静默返回：
    // 调用方本该先判，"删除一个不存在的目录"不是错误，也绝不为它写 WAL 行
    // （写了行却什么都没删，回滚侧会照着元数据把目录重建出来 —— 凭空多出一个目录）。
    struct stat st{};
    if (::lstat(target.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return DirRemoval::NotRemoved;

    // **挂载点守卫**：rmdir(2) 对挂载点恒返回 EBUSY（VFS 的 may_delete），而下面那行
    // `fs::remove(target, ec)` 把错误码整个吞掉 —— 结果是 DIR_RM 行已写、所有权已摘，
    // 目录却还在：行声称"已删"，回滚侧据此把它当"我们删掉的"重建，盘面/DB/回滚三方脱节。
    // 直接在 rmdir **之前**判掉：不写 WAL、不 rmdir，报告"跳过"交给调用方告警。
    // （判据必须用 is_mount_point 而不是"rmdir 失败"—— 后者会把 ENOTEMPTY/EACCES 这类
    //   真错误一并吞掉；且 pacman 对目录型 mountpoint 的语义就是保留。）
    if (is_mount_point(target)) return DirRemoval::SkippedMountPoint;

    // **xattr 先记（2026-09-26 补）**：`DIR_RM` 行只带 mode/uid/gid，而回滚侧的
    // `Undo::RecreateDir` 只 `create_directories` + `lchown`/`chmod` ⇒ 被 rmdir 又在回滚里
    // 重建的目录**丢掉整份 xattr**（而目录 xattr 正是 POSIX ACL 与 SELinux 标签的存放处）。
    // 与刚为目录元数据补的 `DIR_META` 是**同一族缺口**，只是长在**移除侧** ——
    // 属性测试的 xattr 维度当场抓到这一格（实测 2/32 种子）。
    //
    // **只记行、不动盘**：目录马上要被 rmdir，没有理由再 lsetxattr 一次（多余且可能失败）。
    // **行序是承重的**：`XATTR_SET…` 必须在 `DIR_RM` **之前** —— 逆序回滚时先撤 `DIR_RM`
    // （把目录重建出来）、**再**撤各 `XATTR_SET`（把旧值写回去），正好落在重建好的目录上；
    // 顺序反了的话，写回会打在一个还不存在的路径上，`Guard::TakenNotSymlink` 会判否跳过
    // ⇒ 静默丢失（行写了、值没回来），比不记还坏。
    //
    // 没有 xattr 的目录**写零行**（遍历空集）⇒ 绝大多数目录移除的 WAL 与改动前逐字节相同。
    // 复用现成的 `XATTR_SET` 行与它的逆操作（`Undo::SetXattr`），不新增行类型。
    for (const std::string& key : list_xattr_keys(target)) {
        const auto val = read_xattr(target, key);
        if (!val) continue;  // 两次读之间被改掉（罕见）→ 不为一个空动作写行
        wal::log_wal_line("XATTR_SET " + target.string() + " " + xattr_key_field(key) + " " +
                          xattr_value_field(*val));
    }

    // write-ahead：先记 DIR_RM（mode/uid/gid 供回滚重建）再 rmdir。
    // 记的是**规范化后**的路径：调用点的目录键本就是已剥尾斜杠的形态（剥的是 key 的尾斜杠，
    // 不是这里的），所以字面与重构前逐字节一致；而回滚侧 `reverse_execute` 的 DIR_RM 分支
    // 同样会先剥尾斜杠 —— 两侧对"这条行描述哪个对象"的理解一致。
    wal::log_wal_line("DIR_RM " + target.string() + " " + std::to_string(st.st_mode & 07777) + " " +
                      std::to_string(st.st_uid) + " " + std::to_string(st.st_gid));
    std::error_code ec;
    fs::remove(target, ec);  // rmdir（仅当为空才成功；非空由调用方的守卫挡住）

    // rmdir 失败**不是静默事件**：DIR_RM 行已经写了（write-ahead 不允许反悔 —— 行的作用是
    // "回滚时把目录重建出来"，先 rmdir 后写行的话，崩在中间就是"目录没了、WAL 无记录"，
    // 盘面与 DB 脱节），而盘面什么都没变：行声称"已删"、DB 归属已摘、目录还在，三方脱节，
    // 回滚会照着行里的元数据把它"重建"出来。挂载点已被上面的守卫拦掉，走到这里是 EROFS /
    // EACCES / is_empty 与 rmdir 之间的 ENOTEMPTY 竞态这类**真错误**，必须让用户看见。
    //
    // 告警在**本方法内部**打，而不是把错误信息回传给调用方：两个调用点都只看
    // SkippedMountPoint，加一个带消息的返回值意味着"每个调用点都要记得判"，而漏判的后果
    // 正是这次要修的静默。本文件的 include 里已有日志设施（base/utils.hpp）。
    if (ec) log_warning(string_format("warning.dir_remove_failed", target.string(), ec.message()));

    return ec ? DirRemoval::NotRemoved : DirRemoval::Removed;
}

void OpSink::commit_copy(const fs::path& tmp, const fs::path& dst,
                         std::string_view after_wal_breakpoint)
{
    // 两个路径都是普通文件路径（不含尾斜杠），规范化在这里是 no-op；仍然过一遍是为了
    // 让"任何碰文件系统的方法都先规范化"成为本类无例外的规则（见头文件契约 2）。
    const fs::path src = strip_trailing_slash(tmp);
    const fs::path target = strip_trailing_slash(dst);

    // write-ahead：先记 COPY（回滚侧据此删除 dst）再 rename。
    // 崩溃在两者之间 → 行在、dst 不存在 → 逆操作"删除 dst"无操作（幂等）。
    wal::log_wal_line("COPY " + src.string() + " \xe2\x86\x92 " + target.string());

    if (!after_wal_breakpoint.empty())
        BreakpointManager::instance().hit(std::string(after_wal_breakpoint));

    safe_rename(src, target);
}

namespace
{
/// 内部一致性错误的措辞（**不**走 l10n：这条永远不该出现，出现即编程错误/表有洞）
std::string decision_hole_msg(const PathFacts& f, std::string_view what, const char* which)
{
    return std::string("内部一致性错误：决策表漏格 —— ") + std::string(what) + " " + f.logical +
           " 的" + which +
           "没有任何一趟认领（`ARCH.md` §5.4 不变量 1）。这正是 4afb443d 之前"
           "「盘上是真目录、新条目是文件/符号链接」两格没人让开的成因。";
}

std::string decision_conflict_msg(const PathFacts& f, std::string_view what, const char* why)
{
    return std::string("内部一致性错误：决策表冲突 —— ") + std::string(what) + " " + f.logical +
           " 被不该管它的趟认领了：" + why + "。";
}

/**
 * 决策表的**后置条件**（头文件里那条"恰好认领一次"的可执行形式）。
 *
 * 两个方向不对称是**有意的**：归档条目（新版本会碰）与 DB 旧键（新版本不再发）是两套
 * 互斥的输入，前者由 ②③ 负责、后者由 ⑤ 负责。任一侧出现"该认领的趟是 Unclaimed"或
 * "别的趟也来认领"，就是这类漏格的**定义**，必须当场响而不是等盘面出怪。
 */
void check_decision_invariants(const PathFacts& f, const PathDecision& d)
{
    const std::string_view what = f.in_archive ? "归档条目" : "DB 旧键";
    if (f.in_archive) {
        // 让开**必须**归第②趟：它跑在写入（第③趟）之前。归给第⑤趟 = 让开赶不上写入。
        if (d.let_go == PathAction::Unclaimed)
            throw LpkgException(decision_hole_msg(f, what, "让开动作"));
        if (d.write == PathAction::Unclaimed)
            throw LpkgException(decision_hole_msg(f, what, "写入动作"));
        if (d.reg != PathAction::Unclaimed)
            throw LpkgException(decision_conflict_msg(f, what, "登记趟不得处理新版本会碰的路径"));
        return;
    }
    if (d.reg == PathAction::Unclaimed) throw LpkgException(decision_hole_msg(f, what, "登记动作"));
    if (d.let_go != PathAction::Unclaimed || d.write != PathAction::Unclaimed)
        throw LpkgException(decision_conflict_msg(f, what, "让开/写入两趟不得处理 DB 旧键"));
}
}  // namespace

namespace
{
/// decide_path 的本体（不含后置条件检查 —— 检查只做一次，见下面的公开入口）
PathDecision decide_path_unchecked(const PathFacts& f)
{
    PathDecision d;

    if (f.in_archive) {
        // ── 归档条目 ────────────────────────────────────────────────────────────
        if (f.entry_is_dir) {
            // 归档是**目录**条目：
            //   · 路径不存在            → 建目录（NEW_DIR + mkdir + 元数据）
            //   · 盘上已是真目录        → 什么都不用做（目录本体在，元数据由写入趟刷）
            //   · 盘上是非目录（普通文件 / 符号链接，含 symlink→目录）→ **让开 + 建目录**。
            //     `/etc` 走 `save_config`（整树改名 `<路径>.lpkgsave`，配置一个不丢），
            //     其余进 stash（单次 rename、回滚即 rename 回来）。
            //     ⚠️ 判据是 `!disk_is_dir`（lstat 语义），**不是** `fs::is_directory`：
            //        后者跟随符号链接，会把 `symlink→目录` 误判成"已是目录"而跳过让开，
            //        随后 rename 撞 EEXIST（lpkg/CLAUDE.md §0 铁律第 1 条）。
            if (!f.disk_exists)
                d.let_go = PathAction::MakeDir;
            else if (f.disk_is_dir)
                d.let_go = PathAction::Noop;
            else
                d.let_go = f.is_config ? PathAction::SaveConfigAndMkDir : PathAction::StashAndMkDir;
            d.write = PathAction::WriteDirMetadata;
            return d;
        }

        if (f.is_config) {
            if (f.disk_is_dir) {
                // `/etc` 的**非目录**条目撞盘上**真目录**（`ARCH.md` §6.3 的「类型变化」格，
                // 即 §3.2.2 第一行）：让开趟必须**先把路清掉** —— 整树改名成
                // `<路径>.lpkgsave/`（内容一个不丢、`SAVE_CONF` 可回滚、**不进 stash** 所以
                // 提交后不会被清掉），让开之后写入趟**就地**落位。
                // 依据：目录不是"用户可能改过的那份**配置文件**"，把整棵树改名保留再就地安装
                // 与 pacman 一致（`conflict.c` 在 `dir_belongsto_pkgs` 放行后也是就地写新条目）。
                //
                // ⚠️ 这一格原先**没人让开**：`backup_existing_files()` 的 `/etc` 早退排在
                //    "真目录挡路"分支之前，那个分支里的 `save_config` 是**死代码** ——
                //    后果是普通文件条目 rename 撞 EISDIR、符号链接条目被守卫拒绝，
                //    这条升级路径**永远不可能成功**（2026-09-25 修）。
                d.let_go = PathAction::SaveConfig;
                d.write = PathAction::WriteInPlace;
                return d;
            }
            // ── `/etc` 的非目录条目 × 盘上的非目录物 ────────────────────────────────
            // **类型变化 = 一条规则**（2026-09-26 统一）：符号链接 / 普通文件 两者互换，
            // 一律"原物改名 `.lpkgsave`（可回滚、内容一个不丢）+ 新物**就地**落位"。
            // 与上面"盘上是真目录"那一格、以及 DB 旧键那侧的废弃清理由此**同一条政策**：
            // 只要一个路径要被彻底放弃，原物就留成 `.lpkgsave`。
            //
            // 改前这一族是分裂的：`file → symlink` 走"链接一律按配置冲突处理"（用户那份
            // 留原样、新链接退 `.lpkgnew`），`symlink → file` 走**三哈希**（拿链接目标的
            // 内容当 `hash_local` 去和包内那份比）—— 后者尤其没道理：类型都换了，还谈
            // "用户改没改过这份配置"。统一之后"类型变化"与"类型未变"成为两个清晰的分界，
            // 而"类型未变"才谈用户改没改（见下面）。
            const bool type_changed = f.disk_exists && (f.entry_is_symlink != f.disk_is_symlink);
            if (type_changed) {
                d.let_go = PathAction::SaveConfig;
                d.write = PathAction::WriteInPlace;
                return d;
            }
            // ── 类型**未变**的三格（盘上没东西 / file→file / symlink→symlink）────────
            //   · **file → file**（盘上是普通文件）→ 让开趟先搬进 stash（BACKUP）—— 第③步
            //     "先移除旧版本的全部触碰面，再安装新版本"在 `/etc` 上的形态。搬走之后
            //     写入趟才可能判定"这份要不要还给用户"：三哈希的 `hash_local` 正是从
            //     **stash 副本**读的，判定为保留时由 `un_stash` 搬回原位。
            //     写入趟**不重新 probe**：它用让开趟记下的事实查同一张表（`PathRecord`），
            //     所以两趟看到的是同一份事实，决策不可能漂移。
            //   · **symlink → symlink** → 让开趟不碰（`entry_is_symlink` 与
            //     `disk_is_symlink` 在让开趟也是真事实）。不接管的理由见下面
            //     `entry_is_symlink` 分支的注释；落点规则（`ARCH.md` §6.3）也不给它
            //     BACKUP/UNSTASH。
            //     **没有** BACKUP/UNSTASH。
            //   · 盘上什么都没有 → 让开趟无需动作（不动盘也是认领，见 `Noop` 的说明）。
            // 注意 `!f.entry_is_symlink`：**符号链接条目**即使盘上被占也让开趟**不碰**
            // （它要的是"不接管"，让开趟搬走反而会逼写入趟再 un_stash 搬回来）。
            d.let_go =
                (f.disk_exists && !f.entry_is_symlink) ? PathAction::Stash : PathAction::Noop;
            if (!f.disk_exists) {
                d.write = PathAction::WriteInPlace;
            } else if (f.entry_is_symlink) {
                // 归档条目与盘上那份**都是符号链接**：不接管盘上那条（`is_directory` 跟随
                // 链接会把整段配置保护绕过去），v2 退到 `.lpkgnew`。
                d.write = PathAction::WriteLpkgnew;
            } else {
                // 盘上被**普通文件**占住 → 三哈希分流决定落点（判定表只有一份：
                // classify_config_update()，这里只用它的结论）
                switch (f.cfg) {
                    case ConfigDisposition::InstallNew:
                        d.write = PathAction::WriteInPlace;  // 写入趟先 Stash 盘上那份再落位
                        break;
                    case ConfigDisposition::KeepLocal:
                        d.write = PathAction::KeepOnDisk;
                        break;
                    case ConfigDisposition::SaveLpkgnew:
                        d.write = PathAction::WriteLpkgnew;
                        break;
                }
            }
            return d;
        }

        // 非 `/etc` 的**非目录**条目：盘上已被占（普通文件 / 符号链接 / **真目录**）→ 搬进
        // stash 让开；否则只登记 `NEW`。
        // ⚠️ 三种占位物走的是**同一个** `Stash`：单次 rename 对三者都成立，不需要分支。
        //    "盘上是真目录"这一格之所以能走到这里，是预检的 `dir_tree_entirely_ours()`
        //    （见 `ARCH.md` §6.3 的整树让开许可）已经放行 ——
        //    决策表不做许可判定，只描述放行之后做什么。
        d.let_go = f.disk_exists ? PathAction::Stash : PathAction::RegisterNew;
        d.write = PathAction::WriteInPlace;
        return d;
    }

    // ── DB 旧键（第⑤趟的输入）──────────────────────────────────────────────────
    if (!f.obsolete) {
        // 新版本仍提供这个路径 → 归 ②③ 处理；本趟只"确认一次"（撤不了任何东西）
        d.reg = PathAction::Noop;
        return d;
    }
    if (f.is_config) {
        // `/etc` 的废弃条目（2026-09-26 改）：
        //   · **文件 / 符号链接** → `SaveConfigObsolete`：原物改名 `<路径>.lpkgsave`（内容
        //     一个不丢、可回滚）+ 撤所有权 + 撤配置哈希记录。与"类型变化"、移除整包
        //     （`rm_save_conf_after_wal_`）三处统一：`/etc` 下的东西永远不会被 lpkg 无声
        //     丢掉，也永远不会占着"新版本该用的那个名字"。
        //     改前是"**保持原位**、只撤所有权"（`DropOwnership`）—— 后果是废弃的配置继续
        //     占着那个名字，而"盘面 == 新版本形态"从此对 `/etc` 前缀永不成立。
        //   · **目录** → 仍是 `DropOwnership`（只撤记录、不碰盘）。这是**有意保留的例外**：
        //     移除侧对 `/etc` 目录本来就刻意"宁可不碰（只告警）"（`<dir>.lpkgsave` 会把
        //     不知名的目录整个搬走），维护者划的范围也是"要彻底删一个**文件**的路径"。
        d.reg = f.entry_is_dir ? PathAction::DropOwnership : PathAction::SaveConfigObsolete;
        return d;
    }
    if (!f.last_owner) {
        // 还有别的持有者 → 文件留在盘上（所有权只摘本包那一份）
        d.reg = PathAction::Noop;
        return d;
    }
    if (f.entry_is_dir) {
        // 废弃目录：候选删除（真正的 rmdir 由执行侧按"最深优先 + 真目录 + 此刻为空"再判）
        d.reg = PathAction::RemoveDir;
        return d;
    }
    if (f.disk_is_dir || f.new_dir_entry) {
        // 新版本把这个路径变成了**目录**（文件→目录 / 符号链接→目录升级）：内容已由写入趟
        // 接管，这里**绝不能**再把它当"废弃旧文件"搬进 stash —— 那会把刚建好的目录搬走，
        // 升级"成功"而目录消失（实测，见 installation_task_letgo.cpp 的 REMOVE_OLD 阶段注释）。
        d.reg = PathAction::Noop;
        return d;
    }
    if (!f.disk_exists) {
        d.reg = PathAction::Noop;  // 盘上本来就没有 → 没什么可搬
        return d;
    }
    d.reg = PathAction::StashObsolete;
    return d;
}
}  // namespace

PathDecision decide_path(const PathFacts& f)
{
    const PathDecision d = decide_path_unchecked(f);
    // **恰好认领一次**的运行时检查（唯一一次，入口处判）——见头文件的后置条件说明
    check_decision_invariants(f, d);
    return d;
}

}  // namespace detail
