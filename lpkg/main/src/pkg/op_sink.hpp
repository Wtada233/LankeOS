#pragma once

#include <sys/types.h>

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace detail
{

/// `OpSink::remove_empty_dir()` 的结果 —— 调用方据此决定要不要告警
enum class DirRemoval {
    Removed,            ///< rmdir 真的发生了（DIR_RM 行与盘面一致）
    SkippedMountPoint,  ///< 目标是挂载点 → 不写 WAL、不 rmdir（保留），调用方应告警
    NotRemoved,         ///< 其余。两种成因：① 目标本就不是真实目录 → 静默（无事可做）；
                        ///< ② rmdir 真的失败（EROFS/EACCES/竞态）→ **本方法内部已告警**
                        ///<    `warning.dir_remove_failed`（DIR_RM 行已写、盘面没删，
                        ///<    属"行与盘面不一致"，必须可见 —— 2026-09-25 起不再静默）。
                        ///< 注意：此时 WAL 行**刻意保留**，它是回滚重建目录的依据；先 rmdir
                        ///< 后写行会把"行/盘面不一致"恶化成"目录没了、WAL 无记录"。
};

/**
 * 写入层原语：把「一次物理操作 + 它对应的 WAL 行 + stash 记账」融成**一个方法调用**。
 *
 * 为什么要有这一层：WAL 2.0 的不变量是"**每个 WAL 行 = 一个已经开始但可能没做完的操作**，
 * 且行必须在物理操作**之前**写（write-ahead）"。只要"写行"和"做操作"分散在两个文件里、
 * 各自再算一遍路径，两边就会漂移 —— 实战教训（`lpkg/ARCH.md` §3.6.1）是"尾斜杠没剥"这类
 * 缺陷只修了一半：调用点剥了尾斜杠，函数内部又拿原始路径去 lstat/rmdir，尾斜杠让
 * `is_symlink`/`is_empty`/`exists` 这类**判定**落到链接**目标**上，于是"我们按链接判、却按
 * 路径动手"，两侧理解的对象不是同一个。
 *
 * （2026-09-25 订正：原文写"实测把 `/var/run -> ../run` 指向的真实 `/run` 删掉"——**不成立**。
 *  实测 `rmdir("link/")` 与 `rmdir("link")` 一律 ENOTDIR（内核不跟随末段链接），真实发生的是
 *  ① 判定类调用落到目标上（决策错）；② `chmod`/`lchown` **穿过去**改坏目标目录的权限/属主。
 *  守卫仍然必要，只是后果要说准。）
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
 *      save_config / un_stash / remove_empty_dir / commit_copy）都在内部
 *      `strip_trailing_slash()`（逐个核过 **6/6**；`backup` / `backup_obsolete` 共用
 *      `backup_impl`，剥在那一处）。
 *      ⚠️ **这一层是唯一保证，不是"防御性兜底"**。原文写"当前 **9** 个调用点都已各自剥过、
 *      判据必须在调用点（见 §3.6.1 第 1 条）"——实测**两个数都不对**：调用点实际是 **22** 个，
 *      其中**至少 5 个没有自剥**（`installation_task_letgo.cpp` 的 `backup(e.physical_path)`
 *      与 `un_stash(rec.bak, e.physical_path)`、`installation_task_copy.cpp` 的
 *      `save_config(e.physical_path)` 那几处）。它们今天安全，靠的**正是**这里 —— 所以
 *      调用点**可以**自剥（读起来更清楚），但**不许把正确性押在调用点自剥上**
 *      （2026-09-26 按实测订正；这正是本仓库 §0 铁律点名的那类"注释承诺的性质不成立"）。
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

    /**
     * 升级时"新版本不再包含"的废弃文件：WAL `REMOVE_OLD …` → 同上搬进 stash。
     *
     * @param after_wal_breakpoint 与 `backup()` 逐字同义（WAL 行已落、rename 未做之间命中）。
     *        2026-09-26 补：此前**只有它**没有这个参数（`backup` / `save_config` / `un_stash` /
     *        `commit_copy` / `dir_meta` / `set_xattr` / `unset_xattr` 七个都有）⇒ "废弃搬运的
     *        WAL 已写、rename 未做"这个窗口**注入不进去**，是断点覆盖之外的一个洞。
     *        命名照让开趟的既有风格按 **WAL 关键字**取（`backup_after_wal_` /
     *        `conf_replace_after_wal_` 同理）：`remove_old_after_wal_<pkg>`。
     */
    std::filesystem::path backup_obsolete(const std::filesystem::path& phys,
                                          std::string_view after_wal_breakpoint = {});

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

    /**
     * 把 stash 里那份**搬回原位**（`un_stash` 原语，见 `ARCH.md` §9.2 的 `UNSTASH` 行）：
     * WAL `UNSTASH <bak> → <orig>` → `rename(bak, orig)`。
     *
     * 用途（唯一）：`/etc` 配置的两种"保留"结果。第 ③ 步（改执行顺序）之后，三哈希的
     * `KeepLocal` / `SaveLpkgnew` 不再"完全不碰盘"——让开趟已经把盘上那份搬进了 stash
     * （为了判定，也为了统一"先搬空再写入"），判定为"保留"时由写入趟用本方法搬回来。
     * **可观测行为不变**：文件回到原位，内容与 inode 属性（属主/权限/xattr）逐字不变
     * （rename 不动 inode，硬链接关系也不断）。
     *
     * 幂等（**bak 不存在 → 跳过**，不抛）：这一份可能已被消费（更早的一次回滚、
     * stash 收尸），也可走"直接调用写入趟"的回退路径而压根没搬过。跳过时原位那份**原样
     * 不动** —— 与"保留"的语义一致，绝不因为找不到备份去碰原位那份。
     * 回滚侧（`reverse_execute` 的 UNSTASH 分支）与这里恰好互为逆：rename(orig → bak)。
     *
     * @param after_wal_breakpoint 同 backup()：在 WAL 行已落、rename 未做之间命中断点
     * @return true = 真的搬回来了；false = 没有可搬的（bak 不存在，已跳过）
     */
    bool un_stash(const std::filesystem::path& bak, const std::filesystem::path& orig,
                  std::string_view after_wal_breakpoint = {});

    /** 新文件：WAL `NEW <path>`（**只记录**，文件由 `commit_copy` 落位）。 */
    void new_file(const std::filesystem::path& phys);
    /** 新目录：WAL `NEW_DIR <path>`（**只记录**，目录由调用方的 create_directories 落位）。 */
    void new_dir(const std::filesystem::path& phys);

    /**
     * 目录的元数据（`lchown` + `chmod`）：**先把改前值写进 WAL（`DIR_META`）再动盘**。
     *
     * 为什么必须有这一层：目录是**就地改活对象**的，不像普通文件那样"先写 `.lpkgtmp`、
     * 再 rename 覆盖"（旧 inode 由 `BACKUP` 保住）。没有这一行，注入失败回滚后目录会保留
     * **新**版本的 mode/uid —— 与 ARCH §4 不变量 3（终态 == 事务开始时的盘面）冲突。
     * 这不是潜伏问题：任何"新版本改了某个已存在目录的 mode"的批次失败都会踩到。
     *
     * 前置：目标是**真实目录**（lstat，非符号链接）。不是真目录 → 直接返回（不写行、不动盘）：
     * 符号链接那一格由调用方挡住（`symlink→目录` 时 lchown/chmod 会穿透去改**链接目标**，
     * 那可能是别的包持有的目录），本方法不重复制造一个会静默偏移的落点。
     *
     * @param record_previous 是否记"改前值"。默认 true（**正确性默认**：记下的就是本次改动
     *        覆盖掉的那个状态）。传 false 的**唯一**正当场合是"这个目录是本批次刚建出来的"
     *        —— 它由 `NEW_DIR` 的逆操作（删除）负责收尾，记一行改前值纯属冗余。传错方向
     *        （该记而不记）会让那个目录的元数据**回滚不回去**，所以调用点必须真的知道。
     * @param after_wal_breakpoint 测试断点名：在"WAL 行已落、lchown/chmod 未做"这个窗口命中。
     *        `record_previous = false` 时**没有行也就没有窗口**，此参数被忽略。
     */
    void dir_meta(const std::filesystem::path& phys, mode_t mode, uid_t uid, gid_t gid,
                  bool record_previous = true, std::string_view after_wal_breakpoint = {});

    /**
     * 往一个目录写一个 xattr 键：**先把改前状态写进 WAL 再写盘**。行类型由改前状态决定 ——
     * 键本来有值 → `XATTR_SET`（逆 = 写回旧值）；键本来不存在 → `XATTR_NEW`（逆 = 删掉）。
     *
     * 为什么键/值要 base64：见 `wal_op.hpp` 的 `XATTR_SET` 说明（WAL 是行式、空格分帧、
     * `" → "` 是箭头分界的文本协议，而 xattr 键名与值是任意字节串）。
     *
     * 前置同 `dir_meta`（真实目录、非符号链接）—— 不是则返回 false 且**不写行**（保持
     * "行 = 已发生的动作"这条不变量；符号链接上的写入会落到链接自身/目标，与调用方的
     * 边界不一致）。
     *
     * @return 是否真的写了（false = 前置不满足，跳过）
     */
    bool set_xattr(const std::filesystem::path& phys, const std::string& key,
                   const std::vector<char>& value, std::string_view after_wal_breakpoint = {});

    /**
     * 删掉一个目录上的 xattr 键（升级/移除时的**撤销**：本包声明过、新版本不再声明）。
     * 改前状态照旧先进 WAL（删掉一个"本来有值"的键 ⇒ 写成 `XATTR_SET`，逆操作把旧值写回），
     * 再 `lremovexattr`。
     *
     * 键本来就不在盘上 → 返回 false 且**不写行**（无事的行会让回滚白写一次审计）。
     *
     * @return 是否真的删掉了
     */
    bool unset_xattr(const std::filesystem::path& phys, const std::string& key,
                     std::string_view after_wal_breakpoint = {});

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
     * @return `Removed` / `SkippedMountPoint`（调用方应告警）/ `NotRemoved`（见枚举注释：
     *         目标不是真目录 → 静默；rmdir 真失败 → 本方法内部告警）
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

// ============================================================================
// 升级/安装的**路径决策表**（决策表本体与 `/etc` 落点规则见 `ARCH.md` §6.3）
//
// 为什么要有这一层：原先一次升级分三趟 —— ② `backup_existing_files()`（让开）、
// ③ `copy_package_files()`（写入）、⑤ `commit_without_file_ops()`（注册 + 废弃清除）；
// 第③步重构之后是 ②a 让开归档条目 / ②b 让开 DB 旧键 / ③ 写入 / ④ 注册（废弃清除已前移到 ②b）
// ③ `copy_package_files()`（写入）、⑤ `commit_without_file_ops()`（注册 + 废弃清除）
// —— 而"同一个路径该由哪一趟处理"原先只写在三处的注释里。已经长出过漏格：
// `installation_task.cpp` 第②步曾写"仅真正的目录由目录逻辑处理、跳过"，但"目录逻辑"
// 是第⑤步、跑在**拷贝之后**，于是"盘上是真目录、新条目是文件/符号链接"这一格没人让开，
// `rename` 撞 EISDIR，整条升级路径被预检拒掉（见 lpkg/CLAUDE.md §1.2）。
// 判据：同一批路径的处理分散在 N 处、且"谁负责"靠注释维持 ⇒ 漏格是必然而非偶然。
//
// 因此把"路径 → 动作"收成一个函数（`decide_path()`），三趟**都只经它**决定
// "这个路径归谁、做什么"。放在 `op_sink.hpp` 而不是 `installation_task.cpp`：
// 它是**动作词汇表**（动作 = 选哪个 `OpSink` 原语）的另一半，且要对测试可见。
//
// ⚠️ 本层只做**决策**，不做许可判定：`dir_tree_entirely_ours()`（见 `ARCH.md` §6.3 的整树让开许可）
//    是**冲突预检**的判据，不在这里 —— 决策表描述"放行之后各趟做什么"。
// ============================================================================

/**
 * 配置文件升级时的**三哈希分流**结论（pacman `add.c` 的语义）。
 *
 * 三份哈希：`hash_local` = 盘上那份的内容哈希；`hash_orig` = **上一次我们往这个包的
 * 这个路径里装进去的内容**的哈希（记在 `confhashes.db` 里，见 `Config::conf_hashes_db()`）；
 * `hash_pkg` = 本次包里那个条目的内容哈希。
 *
 * 定义搬到这里（原先在 `installation_task.cpp` 的匿名命名空间）：它是决策表的**输入事实**
 * 之一（「`/etc` 且盘上被占且非目录 → 按三哈希结论决定落点」），而决策表要对测试可见。
 * 判定逻辑本身仍只有一份：`classify_config_update()`（`install_common.cpp` —— 它被让开趟与
 * 写入趟两个 TU 共用，故与其它跨趟纯函数一起下沉）。
 */
enum class ConfigDisposition {
    /// ① 盘上 == 上次装进去的 → 用户**没改过** → 静默换成新版（不产生 `.lpkgnew`）
    InstallNew,
    /// ② 上次装进去的 == 本次的 → 包本身没改这个配置 → 保留用户文件、**连 `.lpkgnew` 都不产生**
    KeepLocal,
    /// ③ 三者互异 / 无从判定 → 新版落 `.lpkgnew` + 告警
    SaveLpkgnew,
};

/** 三趟（让开 / 写入 / 注册）—— 每个动作归属**恰好一趟** */
enum class PathPass {
    LetGo,     ///< ② `backup_existing_files()`：让开（搬进 stash / 改名 `.lpkgsave` / 建目录）
    Write,     ///< ③ `copy_package_files()`：写入（落位 / 落 `.lpkgnew` / 保留不碰盘）
    Register,  ///< ⑤ `commit_without_file_ops()`：注册 + 废弃清除
};

/**
 * 决策表认领的动作。每个动作对应**一个**（或一组固定的）`OpSink` 原语 ——
 * "路径 → 动作"的映射只有 `decide_path()` 一份，三趟各自只执行 `action_of(本趟)`。
 *
 * `Noop` 与 `Unclaimed` 的区别是本层的要害：`Noop` = "本趟**考虑过**这个路径，决定不动"
 * （也是一次认领），`Unclaimed` = "本趟不管这个路径"。历史上的漏格正是把
 * "本该由我让开"错当成"别人会管"。
 */
enum class PathAction {
    Unclaimed,           ///< 本趟对这个路径没有动作（没认领它）
    Noop,                ///< 本趟认领了，但**故意不动盘**
    Stash,               ///< `sink.backup()`：搬进 stash（WAL `BACKUP`）
    StashObsolete,       ///< `sink.backup_obsolete()`：废弃搬运（WAL `REMOVE_OLD`）
    SaveConfig,          ///< `sink.save_config()`：改名 `<路径>.lpkgsave`（WAL `SAVE_CONF`）
    MakeDir,             ///< `sink.new_dir()` + `mkdir` + 套元数据（路径本来没有被占）
    StashAndMkDir,       ///< 先 `Stash` 再 `MakeDir`（非 `/etc`：挡路物让开 + 建目录）
    SaveConfigAndMkDir,  ///< 先 `SaveConfig` 再 `MakeDir`（`/etc`：整树改名 + 建目录）
    RegisterNew,         ///< `sink.new_file()`：新路径占位（WAL `NEW`），无需让开
    WriteInPlace,        ///< 落位到逻辑路径本身（让开趟已把它清空；`/etc` 的 InstallNew
                         ///< 由让开趟 `Stash` 清空 —— 写入趟不得再 backup 一次）
    WriteLpkgnew,        ///< 落位到 `<逻辑路径>.lpkgnew`（配置冲突，原文件留原样；让开趟搬过
                         ///< 的话先 `un_stash` 把原位那份搬回来）
    KeepOnDisk,          ///< 保留盘上那份：不落位、不搬、不删（`/etc` 的 `KeepLocal`；
                         ///< 让开趟搬过的话先 `un_stash` 把原位那份搬回来）
    WriteDirMetadata,  ///< 目录条目的元数据（`lchown`/`chmod`）；目录本体由 ② 或 ensure_dir_exists
                       ///< 建
    DropOwnership,     ///< 只撤所有权（+ 配置哈希记录），**不碰盘**（`/etc` 的废弃**目录**）
    SaveConfigObsolete,  ///< 废弃的 `/etc` **文件/符号链接**：先 `sink.save_config()` 把原物改名
                         ///< `<路径>.lpkgsave`（2026-09-26 起；此前是"原地不动"的
                         ///< `DropOwnership`），再撤所有权 + 配置哈希记录。与"类型变化"和
                         ///< 移除整包三处**统一到同一条规则**：`/etc` 下的东西永远不会被 lpkg
                         ///< 无声丢掉，也永远不会占着"新版本该用的那个名字"。
    RemoveDir,           ///< `sink.remove_empty_dir()`：空目录 `rmdir` + 元数据（WAL `DIR_RM`）
};

/**
 * 决策表的输入：一个逻辑路径的**全部相关事实**（纯数据、无 I/O —— 由调用方 probe 后填入）。
 *
 * `logical` 用**归档条目 / DB 键的原始形态**（目录带尾斜杠，见 lpkg/CLAUDE.md §6）。
 * 事实是**时间点相关**的。第②步只是把决策收拢到一处，没有消除这层时间点依赖；第③步
 * （改执行顺序）用两种手段消除它：
 *   · 让开趟把**旧版本的全部触碰面**先搬空 —— 写入趟看到的是"路径已空"，不再受"让开趟
 *     刚才动过它"的影响；
 *   · 文档意义上的**每个归档条目**的事实由让开趟记进 `PathRecord`，写入趟**逐字复用**
 *     （`ProbeLedger`），两趟查的是同一份事实（见下面 `PathRecord` 的说明）。
 * 写入趟因此不再自己 probe 盘面：`decide_path()` 被查询时看到的事实，永远来自**一次**
 * probe（让开趟那次）。
 */
struct PathFacts {
    std::string logical;            ///< 逻辑路径（`/usr/share/x` 或 `/etc/a/`）
    bool in_archive = true;         ///< 来自归档条目（false = DB 旧键，只出现在第⑤趟）
    bool is_config = false;         ///< `/etc/` 前缀
    bool entry_is_dir = false;      ///< 归档条目是目录（`logical` 带尾斜杠）
    bool entry_is_symlink = false;  ///< 归档条目是符号链接（归档形态 ⇒ **两趟都据实填**：
                                    ///< 让开趟靠它决定"不接管、不搬"，写入趟靠它决定退
                                    ///< `.lpkgnew` 还是就地落位）
    bool disk_exists = false;       ///< lstat 语义：存在（符号链接算存在）
    bool disk_is_dir = false;       ///< lstat 语义：**真目录**（符号链接**不算**）
    bool disk_is_symlink = false;   ///< lstat 语义：盘上那份是**符号链接**（2026-09-26 新增）。
                                    ///< 只在 `disk_exists && !disk_is_dir` 时有意义。加它是因为
                                    ///< `/etc` 的落点规则要按**类型是否变化**分流，而原来
                                    ///< `disk_exists` + `disk_is_dir` 两个事实**区分不出**"盘上是
                                    ///< 普通文件"与"盘上是符号链接"—— 这两种在旧表里落进了同一个
                                    ///< 分支（三哈希 / `.lpkgnew`），正是类型矩阵没统一的原因。
    bool obsolete = false;          ///< 第⑤趟：新版本不再提供这个路径
    bool last_owner = false;        ///< 第⑤趟：摘掉本包归属后，该路径已无其他持有者
    bool new_dir_entry = false;     ///< 第⑤趟：新版本在 `<bare>/` 登记了**目录**条目
    ConfigDisposition cfg = ConfigDisposition::InstallNew;  ///< 仅 `/etc` 且盘上被占且非目录
};

/**
 * 决策表的输出：**每个趟至多一个动作**（"同趟不重复认领"由字段结构保证）。
 * `Unclaimed` = 该趟不认领这个路径。
 */
struct PathDecision {
    PathAction let_go = PathAction::Unclaimed;
    PathAction write = PathAction::Unclaimed;
    PathAction reg = PathAction::Unclaimed;

    PathAction action_of(PathPass pass) const
    {
        switch (pass) {
            case PathPass::LetGo:
                return let_go;
            case PathPass::Write:
                return write;
            case PathPass::Register:
                return reg;
        }
        return PathAction::Unclaimed;
    }
};

// ============================================================================
// 事务内的「路径事实」记录表（让开趟 probe 一次、写入趟逐字复用）
//
// 第②步把"路径 → 动作"收成了一个函数，但**事实本身**仍是时间点相关的：让开趟与写入趟
// 各自 probe 一次盘面，而两次 probe 之间盘面已被让开趟改过（"让开"的全部意义就是改它）。
// 于是同一个路径在两趟里可能看到**不同的事实** —— 那正是漏格的结构性来源
// （`/etc` 被占的那一格尤其致命：让开趟把配置搬进 stash 之后，写入趟再看盘面就是"空的"，
//   三哈希分流整段失效）。
//
// 第③步的解法：**一个路径在一个事务里只 probe 一次**。让开趟 probe 完把事实记在这里
// （连同它在此基础上补齐的 `/etc` 三哈希结论），写入趟**逐字复用**这条记录去查同一张表
// —— 两趟看到的是同一份事实，决策不可能漂移。
//
// 覆盖范围：**每一个归档条目**都记一条（事实部分），写入趟不再自己 probe 盘面。其中
// `/etc` 的**非目录、非符号链接**条目**额外**记下三哈希结论与 stash 落点（唯一"事实之外
// 还要带结论"的一族，也是唯一"事实真的会改变写入动作"的一族）。唯一不记的是"包内那份
// 读不到"的条目：写入趟在循环头就 `continue`（同一个判据），记了也没人读。
//
// 生命周期 = **一个 InstallationTask**：它是 `InstallationTask` 的成员，不是函数级静态表。
// 于是同一进程里先后处理**同名包**（批量安装、依赖递归里重名、先卸后装）在结构上不可能
// 互相清/踩 —— 记录根本没有第二个持有者。让开趟入口 `begin()` 清空自己那一份，写入趟
// 跑完 `clear()` 释放；异常打断写入趟时残留的记录也只属于本 task，随 task 一起消失
// （再也不会像静态表那样"下一位同名包读到上一位的脏记录"）。
// ============================================================================

/// 让开趟为某个归档条目记下的事实与结论（写入趟只读）
struct PathRecord {
    PathFacts facts;            ///< 让开趟 probe 到的事实（`cfg` 由让开趟补齐）
    std::string hash_pkg;       ///< `/etc` 条目：包内那份的哈希（写入趟写 confhashes 记录用）
    bool stashed = false;       ///< 这份是否被让开趟搬进了 stash
    std::filesystem::path bak;  ///< `stashed == true` 时：stash 里那份的路径
};

/**
 * 一次安装任务内的记录表：**归档条目名 → 让开趟记下的事实**。
 *
 * 只有两个时刻碰它：让开趟（`begin()` + 逐个 `record()`）与写入趟（逐个 `find()`，末尾
 * `clear()`）。`find()` 返回的指针在下一次 `record()`/`begin()`/`clear()` 之前有效 ——
 * 两趟不交叉读写，调用方按此使用。
 */
class ProbeLedger
{
public:
    /// 进入让开趟（记录的**生产者**）：清空上一轮留下的（同一 task 重跑、上一批的残留）
    void begin()
    {
        by_entry_.clear();
    }

    /// 记一条（仅让开趟调用）
    void record(const std::string& entry, const PathRecord& rec)
    {
        by_entry_[entry] = rec;
    }

    /// 查一条；没有记录 → `nullptr`（写入趟据此退回"自己 probe"的老路径：测试直接调用
    /// `copy_package_files()` 时没有让开趟，那条回退路径必须保留）
    const PathRecord* find(const std::string& entry) const
    {
        const auto it = by_entry_.find(entry);
        return it == by_entry_.end() ? nullptr : &it->second;
    }

    /// 写入趟跑完 → 释放（避免长批次里逐包累积）。与 `begin()` 同一个动作，名字只表达
    /// 调用点意图（生产者入场 / 消费者离场）。
    void clear()
    {
        by_entry_.clear();
    }

    std::size_t size() const
    {
        return by_entry_.size();
    }
    bool empty() const
    {
        return by_entry_.empty();
    }

private:
    std::map<std::string, PathRecord> by_entry_;
};

/**
 * 决策表本体：**路径 → 动作**（逐格见 `ARCH.md` §6.3 的表）。
 * 三趟（②③⑤）都只经它决定"这个路径该谁处理、做什么"。
 *
 * **后置条件（"恰好认领一次"的可执行形式）**：
 *   · 归档条目（= 新版本会碰的路径）：**让开趟与写入趟各认领恰一次** —— 可以是 `Noop`
 *     （"考虑过并决定不动"也是认领），但**绝不许 `Unclaimed`**；登记趟不得染指。
 *     这一条直接编码了那次漏格的教训：让开动作只能归**跑在写入之前**的第②趟，
 *     把它推给第⑤趟（跑在拷贝之后）就是漏格本身。
 *   · DB 旧键（第⑤趟的输入）：**登记趟认领恰一次**，让开/写入两趟不得染指。
 * 违反即抛 —— 它意味着"某个路径没人负责"或"两趟抢同一个路径"。
 */
PathDecision decide_path(const PathFacts& f);

}  // namespace detail
