#pragma once

#include <filesystem>
#include <string>
#include <string_view>

/**
 * 原子写入原始字符串内容：.tmp → fsync → rename（safe_rename 内含父目录 fsync）。
 * 断电在 rename 前 → 原文件不变；断电在 rename 后 → 新文件完整。顺序保持内容原样。
 */
void write_string_to_file(const std::filesystem::path& path, std::string_view content);

/**
 * 收尾一次"已写好 .tmp"的原子写：fsync(.tmp) → rename → fsync 父目录。
 *
 * **fsync 的返回值必须检查**：ofstream 成功只说明内容进了页缓存，磁盘满/EIO 要等
 * fsync 才暴露；丢掉它就等于把截断内容 rename 进正式位置（DB 静默损坏、且下一轮
 * 还会把这个截断内容当成"备份"）。失败抛 LpkgException。
 *
 * 所有 `.tmp + fsync + rename` 的写入路径都必须走这里，不要各写一套。
 */
void fsync_and_rename(const std::filesystem::path& tmp, const std::filesystem::path& dst);

/**
 * 断电一致性（fsync）开关。
 *
 * **默认关闭**：仍然保证 rename(2) 的原子性与 kill/SIGKILL 后的回滚/恢复语义 —— 因为
 * 单个操作始终是"一次 rename 或一次顺序写"，页缓存里的顺序不会被进程死亡打断；只是
 * **不保证断电持久性**（断电可能丢掉最近写入的**包文件数据**）。
 *
 * **它管的只有"批量文件数据"**：包内容与 `.lpkgtmp`/`.lpkgnew` 的内容 fsync，以及这些
 * 文件 rename 的父目录 fsync（两万文件规模下每文件 2~6 次 fsync，是安装耗时的大头）。
 *
 * **DB/元数据写不受它控制**（`Cache` 的四个 DB 写函数、`wal::write_string_file_wal`
 * 都在 `DurableFsyncGuard` 里）：库文件只 rename 到页缓存、而唯一备份
 * `.lpkg_db_bak_before:*` 在批次提交后立刻被 `cleanup_db_backups()` 删掉，断电落在
 * 这个窗口里就是"库空/截断 + 备份已删"的**系统级不可恢复**状态（files.db / pkgs 是
 * 单文件，丢了就是全库所有权归零）。每里程碑只多约 5 次 fsync。
 *
 * **WAL 行自己的 fsync 也不受它控制**（`wal::log_wal_line` 无条件 fsync）：行的持久化
 * 是"行 = 一个已开始但可能未完成的操作"这条不变量的前提，关了会出现"操作做了、行没了"
 * 这种不可恢复组合。**WAL 文件自身的目录项 fsync 同样不受它控制**（`WalWriter` 构造、
 * `wal::log_wal_line` 与 `wal_append_raw` 三处打开路径都套了 `DurableFsyncGuard`）：
 * `fsync(文件)` 不覆盖父目录的 dentry，"行已 fsync、文件整个不存在"是断电后真实可能的状态
 * —— 行是唯一的回滚依据。
 *
 * 开关只在这两个**写入原语**里分支（`fsync_dir_internal`、`fsync_and_rename`），因此
 * 事务与 WAL 逻辑里看不到它 —— "有没有 fsync"是写入层的属性，不是事务层的分支。
 */
bool durable_fsync_enabled();
void set_durable_fsync_enabled(bool on);

/**
 * 作用域内强制持久化（RAII）：构造时把内部标志置真，析构时还原（嵌套安全）。
 *
 * 用途 —— 让两类"丢了就不可恢复"的写恒持久化，不受默认关闭的
 * `durable_fsync_enabled()` 影响（理由见上）：
 *   - **DB/元数据写**：`fsync(.tmp)` → rename → fsync 父目录，调用点一行即可：
 *
 * ```cpp
 * void Cache::write_set_file_wal(...)
 * {
 *     DurableFsyncGuard durable;  // DB 一族永远持久化
 *     ...
 * }
 * ```
 *
 *   - **WAL 文件的打开/首次创建**（`wal::WalWriter` 构造、`wal::log_wal_line`、
 *     `wal_op.cpp` 的 `wal_append_raw` —— 三条路径都有 `O_CREAT`，都必须落盘 dentry）：
 *     只需覆盖 `fsync_parent_dir` 那一行，**用一个块把守卫关住**，别让它活过 WalWriter 的
 *     生存期。
 *
 * **只用在 DB/元数据写函数的最外层与上述 WAL 打开路径**；绝不要包到安装/删除的批量
 * 文件操作上 —— 那类数据（包内容、`.lpkgtmp`/`.lpkgnew`）是故意留给开关控制的。
 */
class DurableFsyncGuard
{
public:
    DurableFsyncGuard();
    ~DurableFsyncGuard();
    DurableFsyncGuard(const DurableFsyncGuard&) = delete;
    DurableFsyncGuard& operator=(const DurableFsyncGuard&) = delete;

private:
    bool prev_;  // 进入前的值：嵌套时内层析构必须还原成内层看到的值，而不是无条件 false
};

/**
 * 测试用：本进程内**受开关影响的两个原语**（`fsync_and_rename` 的 .tmp 内容 fsync、
 * `fsync_dir_internal` 的目录项 fsync）实际发出过多少次 `::fsync`。
 *
 * 生产逻辑**不读**它——它存在的唯一理由是让测试能直接观察"开关对某条写路径生效没有"，
 * 而不必去数 syscall（strace）或猜调用点。WAL 行自己的 fsync（`wal::log_wal_line`）
 * 不经过这两个原语，因此不计入——它本来就不受开关控制。
 */
size_t durable_fsync_count_for_tests();

/**
 * fsync 目标文件所在父目录，确保 rename 后的 dentry 落盘。
 *
 * rename(2) 在同文件系统内是原子的，但如果父目录的 dentry 未落盘，
 * 断电后目录可能指向旧路径，rename 的"原子性"在磁盘上不会体现。
 * safe_rename() 内部已调用此函数，一般不需直接使用。
 */
void fsync_parent_dir(const std::filesystem::path& child_path);

/**
 * 安全重命名。
 *
 * 仅做 rename(2)，失败一律抛异常 —— **不做 copy+remove_all 回退**：copy_recursive 对
 * "指向目录的符号链接"会跟随链接误判为目录，递归删除整棵被 rename 的目录树
 * （升级 filesystem 包时 /usr/lib 全树被删）。开 redirect_dir 的 overlay 目录 rename
 * 本就不返回 EXDEV；宁可失败也不破坏数据。
 *
 * @throw std::filesystem::filesystem_error rename 失败时
 */
void safe_rename(const std::filesystem::path& from, const std::filesystem::path& to);
