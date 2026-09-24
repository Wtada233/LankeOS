#include "op_sink.hpp"

#include <sys/stat.h>

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
    // 尾斜杠会让 rename/lstat 落到末尾符号链接的**目标**上（ARCH.md §3.6.1 第 1 条），
    // 而 WAL 行记的必须是"我们真正动了哪个对象"。规范化只在这里做一次，调用点不管。
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

fs::path OpSink::backup_obsolete(const fs::path& phys)
{
    // 升级的废弃文件与"覆盖已有文件"的回滚语义完全一致（reverse_execute 里 BACKUP 与
    // REMOVE_OLD 走同一分支），只有 WAL 关键字不同 —— 关键字不同是为了让审计能区分
    // "被覆盖"与"新版本不再包含"，因此不能合并成一个方法名。
    return backup_impl(phys, "REMOVE_OLD", {});
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
    std::error_code ec;
    if (fs::exists(dst, ec) || fs::is_symlink(dst)) {
        unsigned n = 1;
        fs::path shifted = shifted_save_name(dst, n);
        while (n < MAX_CONFIG_SAVE_SHIFTS && (fs::exists(shifted, ec) || fs::is_symlink(shifted)))
            shifted = shifted_save_name(dst, ++n);
        if (fs::exists(shifted, ec) || fs::is_symlink(shifted))
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

void OpSink::new_dir(const fs::path& phys)
{
    // 纯日志记录（目录由调用方的 create_directories 落位）。同上按原样记录：
    // 目录条目本来就带尾斜杠（DB 目录键的形态，与归档条目一致），剥掉会改变 WAL 字面。
    wal::log_wal_line("NEW_DIR " + phys.string());
}

DirRemoval OpSink::remove_empty_dir(const fs::path& phys)
{
    // 目录键带尾斜杠 → 必须先规范化，否则下面的 lstat 会**解引用**末尾的符号链接：
    // 对 `/root/var/run/`（真实对象是 `var/run -> ../run`）会读到 /run 的 stat、S_ISDIR 通过，
    // 最终 rmdir 把**真实 /run 删掉**，而调用方的 is_symlink 守卫同样是假阴性。
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

    // write-ahead：先记 DIR_RM（mode/uid/gid 供回滚重建）再 rmdir。
    // 记的是**规范化后**的路径：调用点的目录键本就是已剥尾斜杠的形态（剥的是 key 的尾斜杠，
    // 不是这里的），所以字面与重构前逐字节一致；而回滚侧 `reverse_execute` 的 DIR_RM 分支
    // 同样会先剥尾斜杠 —— 两侧对"这条行描述哪个对象"的理解一致。
    wal::log_wal_line("DIR_RM " + target.string() + " " + std::to_string(st.st_mode & 07777) + " " +
                      std::to_string(st.st_uid) + " " + std::to_string(st.st_gid));
    std::error_code ec;
    fs::remove(target, ec);  // rmdir（仅当为空才成功；非空由调用方的守卫挡住）
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

}  // namespace detail
