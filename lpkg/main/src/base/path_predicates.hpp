#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// ============ 文件系统工具 ============

void ensure_dir_exists(const std::filesystem::path& path);

// ============ 文件系统判定谓词（**不抛**版） ============
//
// `std::filesystem` 的**判定类**调用在"末段符号链接解不开"时不是判 not-found，而是**抛**
// `filesystem_error`。（libstdc++，`ln -s self self` 之后逐条调用）
//
//     fs::exists(loop)        → 抛 filesystem_error  code=40 (ELOOP)
//     fs::is_directory(loop)  → 抛 filesystem_error  code=40
//     fs::is_empty(loop)      → 抛 filesystem_error  code=40
//     fs::is_symlink(loop)    → 不抛（走 lstat）
//
// `fs::is_symlink` 的"不抛"**只在"末段就是那个环"时成立**；**中间段**成环时它照样抛
// （同一套 libstdc++ 环境）：
//
//     ln -s self self
//     fs::is_symlink("self")      → true，不抛        （末段 = 环）
//     fs::is_symlink("self/x")    → **抛** code=40    （中间段 = 环）
//     fs::is_symlink("self/x/")   → **抛** code=40    （中间段 = 环 + 尾斜杠）
//     fs::exists("self/x", ec)    → false，ec=ELOOP（不抛）
//
// 后者才是真正危险的那一种：**判定有没有环的那个调用自己先炸了**。而
// `fs::exists(phys, ec) || fs::is_symlink(phys)` 这类"ec 版 + 抛版"的**短路写法**在
// 中间段成环时**必然**求值右操作数（左边返回 false）⇒ 必抛。判据是**短路方向**，不是
// "用了 ec 重载没有"：`A(ec) || throwing` 与 `!A(ec) && throwing` 都会把抛型那半边求值。
// 反过来，`exists_no_follow(p) && !fs::is_symlink(p)`（`Guard::TakenNotSymlink` 就是它）
// **安全** —— `exists_no_follow` 已确认 lstat 成功，右操作数不会遇到 ELOOP。
//
// 于是本族多了一个 `is_symlink_no_follow()`，并把"凡是可能吃到中间段环的路径"一律换成它。
//
// 于是**符号链接环**（自环、两跳环、上游包自带的环）会让"这个路径归谁 / 该不该删"这类
// 判定直接抛异常 —— 卸载/升级/崩溃恢复在预检或删除阶段炸掉、整批回滚，那个包从此**再也
// 卸不掉**，而盘上不过是一个无害的环。判定类调用不该有能力打断事务：它读不出答案时，
// 答案就是"否"（与 not-found 同义）。
//
// **逐个调用点判断，别全局替换**：`fs::read_symlink` / `fs::canonical` 这类**取值**调用在
// 不可达时就该失败（调用方自己判 nullopt/空）；"写之前先看目标形态"的调用点则要用
// **follow** 语义 —— 见 ensure_dir_exists 的写法。要区分"不存在"与"不可达"的调用点，
// 自己用带 `std::error_code` 的重载自取 ec。
//
// 注：尾斜杠会让内核把前一个分量解析成目录（解引用末段链接），调用前按需
// `strip_trailing_slash()`。

/**
 * lstat 语义：这个**名字**被任何对象占着（含悬空符号链接、自环符号链接）→ true。
 * 判据是"lstat 成功"，不是"目标可达"——`fs::exists(p) || fs::is_symlink(p)` 想要的就是
 * 它，但那条写法在 ELOOP 上会抛。解不开/不可达（ELOOP、EACCES、父目录不是目录）→ false。
 */
bool exists_no_follow(const std::filesystem::path& p);

/**
 * stat 语义（**跟随**末段符号链接）：目标可达 → true；悬空链接、ELOOP、EACCES → false。
 * 绝不抛。
 */
bool exists_follow(const std::filesystem::path& p);

/**
 * 末段是目录（**跟随**符号链接）→ true；不存在/不是目录/解不开 → false，绝不抛。
 *
 * 与 `fs::is_directory`（抛版）**同义**，只多一条"不抛"。判据里有意**保留跟随语义**的
 * 调用点用它 —— 那些位置的目标可以是符号链接（usr-merge 的 `/lib -> usr/lib`、
 * 管理员把 `/var/lib/lpkg` 搬到别处），换成 lstat 语义会让这些布局直接失效。
 */
bool is_directory_follow(const std::filesystem::path& p);

/**
 * 末段是**真目录**（不跟随符号链接，符号链接一律 false）→ true。
 * 等价于 `fs::is_directory(p) && !fs::is_symlink(p)`，但只发一次 lstat、且解不开时判 false。
 */
bool is_real_directory(const std::filesystem::path& p);

/**
 * 末段是**符号链接**（lstat 语义，不跟随）→ true；解不开（**含中间段 ELOOP**）→ false。
 * **绝不抛。**
 *
 * 与 `fs::is_symlink` 的**唯一**区别就是"不抛"，而这条区别在两种形态上都成立（见上面那张
 * 调用表）：末段自环（`fs::is_symlink` 侥幸不抛）与**中间段成环**（它抛）。写
 * `exists_no_follow(p) && !fs::is_symlink(p)` 这种**已经短路掉**的形态可以继续用抛版
 * （lstat 成功就说明没有 ELOOP），但只要右操作数有可能自己吃到环，就必须用本函数。
 */
bool is_symlink_no_follow(const std::filesystem::path& p);

/**
 * 两个路径**各自**解出的符号链接目标是否逐字节相同；任一侧不是符号链接 / 读不出 → false。
 * **绝不抛**（`read_symlink` 的 ec 重载 —— 与谓词族同一纪律：环、权限不足都不得打断整批）。
 *
 * "不确定就判不同"是**有意**的保守方向：调用方（`/etc` 的 symlink→symlink 那一格）用它决定
 * "这条路径变了没有"。判成"不同"顶多多留一份给用户审阅的 `.lpkgnew`；判成"相同"却会把
 * 真正改过的链接当成没动过、直接放过。
 */
bool symlink_targets_equal(const std::filesystem::path& a, const std::filesystem::path& b);

/**
 * 末段是**普通文件**（lstat 语义，符号链接一律 false）→ true；解不开 / 是目录 / 是 FIFO /
 * 设备 / **中间段 ELOOP** → false，绝不抛。
 *
 * 与 `fs::is_regular_file` 的差别不只是"不抛"：后者**跟随**末段链接（走 `status`），
 * 所以对**末段是环**的路径抛 ELOOP —— 而"把符号链接环 rename 进 stash 之后它还是个环"
 * 是真实形态（包可以自带环），判定这类对象时只能用 lstat 版。
 */
bool is_regular_file_no_follow(const std::filesystem::path& p);

/**
 * 目录键 / 归档目录条目路径 → 可安全操作的路径：去掉末尾斜杠（根 "/" 原样返回）。
 *
 * file_db 的目录键一律带尾斜杠（`/var/run/`），归档里的目录条目同理。但按
 * path_resolution(7)，"路径以 '/' 结尾"会强制把**前一个分量**解析成目录 —— 末尾的符号
 * 链接被**解引用**。**哪一类调用会穿过去，彼此完全不同**：
 *   · **判定类**（`lstat` / `fs::is_symlink` / `fs::is_empty` / `fs::exists`）与
 *     **元数据类**（`chmod` / `chown` / `l*xattr`）**会**落到**目标**上 ⇒ 目录清理路径上
 *     "别动 symlink→目录"的守卫会**恒假**（`lstat("link/")` 报的是**目录**）。
 *   · **删除/改名类**（`rmdir` / `rename` / `unlink`）**不会**跟随末段链接，一律 `ENOTDIR`。
 *   · 唯一的例外是 **`fs::remove_all(带尾斜杠的路径)`**：它**删光链接目标的全部内容**
 *     并返回 `ENOTDIR`（`link -> real` 时 `real/` 里的条目全没了、链接原样留着）；不带尾斜杠
 *     则只删链接本身。**调用方拿它的返回值去删目录时必须先规范化。**
 *
 * **任何针对物理路径的文件系统操作前先规范化**；DB 键本身保持带斜杠（那是键，不是路径）。
 */
std::filesystem::path strip_trailing_slash(const std::filesystem::path& p);

void ensure_file_exists(const std::filesystem::path& path);

/**
 * 本进程可见的挂载点集合（读 /proc/self/mountinfo，还原 \040 等八进制转义、去尾分隔符）。
 *
 * **判"是否跨文件系统"必须用它，不能用 st_dev**：overlay 上"目录报 overlay 的 dev、
 * upper 层文件报底层 fs 的 dev"，btrfs 子卷/多层镜像里同一条路径链上的 st_dev 也不一致
 * ——拿 st_dev 当边界，会把同一次 rename 的源和目标误判成跨设备（或反之）。真正的
 * EXDEV 边界是 vfsmount：跨挂载点一律 EXDEV，即使同一 superblock 的 bind mount。
 *
 * /proc 未挂载（最小 chroot）时返回空表，调用方自行降级。结果按进程缓存：事务期间
 * lpkg 自身不改挂载表（hook 的 mount namespace 在子进程里）。
 */
const std::vector<std::filesystem::path>& mount_points();

/** p（去尾分隔符后）是否为挂载点（= 其所在文件系统的顶层）。 */
bool is_mount_point(const std::filesystem::path& p);
