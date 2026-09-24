#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace detail
{

/// `OpSink::remove_empty_dir()` 的结果 —— 调用方据此决定要不要告警
enum class DirRemoval {
    Removed,            ///< rmdir 真的发生了（DIR_RM 行与盘面一致）
    SkippedMountPoint,  ///< 目标是挂载点 → 不写 WAL、不 rmdir（保留），调用方应告警
    NotRemoved,         ///< 其余（目标本就不是真实目录 / rmdir 失败）→ 静默无操作
};

/**
 * 写入层原语：把「一次物理操作 + 它对应的 WAL 行 + stash 记账」融成**一个方法调用**。
 *
 * 为什么要有这一层：WAL 2.0 的不变量是"**每个 WAL 行 = 一个已经开始但可能没做完的操作**，
 * 且行必须在物理操作**之前**写（write-ahead）"。只要"写行"和"做操作"分散在两个文件里、
 * 各自再算一遍路径，两边就会漂移 —— 实战教训（`lpkg/ARCH.md` §3.6.1）是"尾斜杠没剥"这类
 * 缺陷只修了一半：调用点剥了尾斜杠，函数内部又拿原始路径去 lstat/rmdir，尾斜杠把末尾的
 * 符号链接**解引用**，`is_symlink`/`is_empty`/`rmdir` 全部落到链接**目标**上（实测把
 * `/var/run -> ../run` 指向的真实 `/run` 删掉，回滚还会 chmod 穿过去）。
 *
 * 因此本类的契约是：
 *
 *   1. **成对**：调用一个方法 = 写它的 WAL 行 + 立刻做那次物理操作；调用方不再自己
 *      `wal::log_wal_line(...)`，物理操作也一律不再出现在调用点。写成同一行代码就没有
 *      "中途插进别的语句"的余地，write-ahead 顺序不可能被写反。
 *      （两个例外要认清：`new_file`/`new_dir` 是**纯 WAL 记录**、不碰文件系统；
 *      `backup*`/`commit_copy` 里**先** `stash_bak_target()` 建好 stash 根、**再**写行
 *      —— stash 根的 mkdir 不在回滚范围内，与基线逐字一致。）
 *   2. **路径规范化在这一层兜底**：**凡要碰文件系统的方法**（backup / backup_obsolete /
 *      remove_empty_dir / commit_copy）都在内部 `strip_trailing_slash()`。这是**防御性**的
 *      —— 当前 9 个调用点都已各自剥过（判据必须在调用点，见 §3.6.1 第 1 条），所以这条
 *      no-op 路径今天没有覆盖；调用点仍应保持自剥，别把正确性全押在这里。
 *      **纯日志记录的方法**（new_file / new_dir）**按传入原样**记录 —— WAL 行是回滚侧的
 *      输入契约，字面形态不能在这里被"顺手规范化"掉（`NEW_DIR` 记的就是带尾斜杠的目录键）。
 *   3. **stash 记账内联**：备份落进每文件系统 stash 后，把该 stash 根记进调用方的向量
 *      （批次提交后由 `cleanup_stashes` 统一清理）。调用点不必记得"rename 完还要
 *      emplace_back 一次"—— 漏记的后果是备份永远不被清理（残留）或清理不掉（占着空间）。
 *
 * 只做**可回滚**的操作：不可逆动作（如 CLEANUP、.lpkgtmp→目标之外的删除）不在这里。
 */
class OpSink
{
public:
    /**
     * @param pkg      包名（备份名 `.lpkg_bak_<pkg>_<rand>` 与 stash 目录名的组成部分）
     * @param stashes  备份 stash 记账目标（调用方持有；nullptr = 本阶段不产生备份）
     */
    OpSink(std::string pkg, std::vector<std::filesystem::path>* stashes);

    /**
     * 覆盖/接管已有文件：WAL `BACKUP <src> → <stash/bak>` → `rename(src, bak)`。
     * 回滚侧（`reverse_execute` 的 BACKUP/REMOVE_OLD 分支）把 bak rename 回原位。
     *
     * @param after_wal_breakpoint 测试断点名：在"WAL 行已落、rename 未做"这个 write-ahead
     *        窗口命中（`db/test_breakpoints.hpp`）。空串 = 不断点。
     *        必须由这里命中而不能挪回调用点：这两个动作已经被融合成一次调用，窗口只在
     *        本方法内部存在（模拟"崩在 WAL 与 rename 之间"只能在这里注入）。
     * @return stash 中的备份路径（回滚来源），供日志/断言使用
     */
    std::filesystem::path backup(const std::filesystem::path& phys,
                                 std::string_view after_wal_breakpoint = {});

    /** 升级时"新版本不再包含"的废弃文件：WAL `REMOVE_OLD …` → 同上搬进 stash。 */
    std::filesystem::path backup_obsolete(const std::filesystem::path& phys);

    /**
     * 配置文件"改名保留"：WAL `SAVE_CONF <src> → <dst>` → `rename(src, dst)`，
     * `dst = <src>.lpkgsave`（`constants::SUFFIX_LPKG_SAVE`）。
     *
     * **与 `backup()` 的唯一区别是不进 stash**：dst 是原位旁边的兄弟名，批次提交后
     * **不会**被 `cleanup_stashes` 删除 —— 这正是"保留"与"`--purge-config` 真删"的分界。
     * 若把它做成 backup（搬进 stash），提交后的 `remove_all` 会把配置**删掉**。
     * 回滚侧与 BACKUP 同路径（`reverse_execute` 的 BACKUP/REMOVE_OLD/SAVE_CONF 分支
     * 都是"把 dst rename 回 src，找不到 dst 就跳过"），因此崩溃能收敛回"配置仍在"。
     *
     * 目标名已被占用（同一路径被移除过多次）时**不覆盖**：先把旧的移位到第一个空闲的
     * `<dst>.<N>`（N 从 1 起，pacman 的 `shift_pacsave` 语义），移位本身也是一条同类型的
     * WAL 行（可回滚）。
     *
     * @param after_wal_breakpoint 同 backup()：在 WAL 行已落、rename 未做之间命中断点
     * @return 保留路径（`.lpkgsave`），供日志/断言使用
     */
    std::filesystem::path save_config(const std::filesystem::path& phys,
                                      std::string_view after_wal_breakpoint = {});

    /** 新文件：WAL `NEW <path>`（**只记录**，文件由 `commit_copy` 落位）。 */
    void new_file(const std::filesystem::path& phys);

    /** 新目录：WAL `NEW_DIR <path>`（**只记录**，目录由调用方的 create_directories 落位）。 */
    void new_dir(const std::filesystem::path& phys);

    /**
     * 删除一个**空**目录：WAL `DIR_RM <path> <mode> <uid> <gid>` → `rmdir`。
     * 元数据记进 WAL 供回滚重建（`reverse_execute` 的 DIR_RM 分支）。
     *
     * 前置由调用方保证：目标是**真实目录**（非 symlink）且**为空**。非真实目录直接返回
     * （不写 WAL），非空目录 `rmdir` 会失败 —— 两者都不该走到这里，是调用方的守卫失职。
     *
     * **挂载点是唯一的例外，由本方法自己挡住**：`rmdir(2)` 对挂载点恒返回 EBUSY，
     * 旧实现把它连同别的错误一起用 `fs::remove(target, ec)` 静默吞掉 —— 于是 DIR_RM 行
     * 已经写了、目录所有权也已经摘了，盘面却什么都没变，行与盘面不一致。pacman 对
     * **目录型** mountpoint 是"保留 + 不报错"，这里对齐（**文件型** mountpoint 不在
     * 此列：pacman 同样是 unlink→EBUSY 失败，不假装修得比它好）。
     *
     * @return `Removed` / `SkippedMountPoint`（调用方应告警）/ `NotRemoved`（静默）
     */
    DirRemoval remove_empty_dir(const std::filesystem::path& phys);

    /**
     * 落位一个已写好的 `.lpkgtmp`：WAL `COPY <tmp> → <dst>` → `rename(tmp, dst)`。
     * `tmp` 的内容与 fsync 由调用方在此之前完成（本方法只管 write-ahead + rename）。
     *
     * @param after_wal_breakpoint 同 backup()：在 WAL 与 rename 之间命中
     */
    void commit_copy(const std::filesystem::path& tmp, const std::filesystem::path& dst,
                     std::string_view after_wal_breakpoint = {});

private:
    /// backup / backup_obsolete 的唯一实现（只有 WAL 关键字不同）
    std::filesystem::path backup_impl(const std::filesystem::path& phys, std::string_view op,
                                      std::string_view after_wal_breakpoint);

    std::string pkg_;                              // 包名
    std::vector<std::filesystem::path>* stashes_;  // 记账目标（可为 nullptr）
};

}  // namespace detail
