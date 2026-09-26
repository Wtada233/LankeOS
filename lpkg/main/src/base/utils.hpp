#pragma once

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "config.hpp"  // NonInteractiveMode
#include "constants.hpp"
#include "exception.hpp"

// ============ 日志输出 ============

/** 输出普通信息日志 */
void log_info(std::string_view msg);
/** 输出警告日志 */
void log_warning(std::string_view msg);
/** 输出错误日志 */
void log_error(std::string_view msg);
/**
 * 输出带进度条的日志
 * @param percentage 进度百分比 (0-100)
 * @param bar_width 进度条宽度（字符数）
 */
void log_progress(const std::string& msg, double percentage, int bar_width = 50);

// ============ 进程执行 ============

/** 执行外部命令（参数列表形式） */
int run_command(const std::vector<std::string>& args, const std::filesystem::path& work_dir = "");
/** 执行外部命令（Shell 字符串形式） */
int run_shell(const std::string& cmd, const std::filesystem::path& work_dir = "");

/**
 * 在**目标 root 内**执行 shell 命令。
 *
 * `root_dir == "/"` 时等价于 run_shell；否则 fork + unshare(CLONE_NEWNS) +
 * mount --make-private + chroot + chdir("/") 后再执行——与 `run_hook` 的做法一致。
 * 目标 root 内没有 /bin/bash（或 unshare/chroot 失败）时返回 -1，由调用方告警。
 *
 * 为什么必须有它：外部触发器命令（`systemctl daemon-reload` /
 * `glib-compile-schemas /usr/share/glib-2.0/schemas` / `gtk-update-icon-cache`）里写的是
 * 绝对路径。`lpkg --root /mnt/base install ...` 时若不 chroot，它们会**打在宿主上**，
 * 目标 root 反而没更新（TODO F3）。
 */
int run_shell_in_root(const std::string& cmd);

// ============ 用户交互 ============

/** 向用户请求确认（非交互模式自动返回 true） */
bool user_confirms(const std::string& prompt);

// ============ 系统检查 ============

/** 检查是否以 root 权限运行，非 root 则退出 */
void check_root();

// ============ 并发控制 ============

/**
 * 数据库锁（RAII）
 * 构造时加锁，析构时自动解锁，防止并发操作数据库
 */
class DBLock
{
public:
    DBLock();
    ~DBLock();
    DBLock(const DBLock&) = delete;
    DBLock& operator=(const DBLock&) = delete;

private:
    int lock_fd = -1;  // 锁文件描述符
};

/**
 * 临时目录管理器（RAII）
 * 构造时创建临时目录，析构时自动清理
 */
class TmpDirManager
{
public:
    TmpDirManager();
    ~TmpDirManager();
    TmpDirManager(const TmpDirManager&) = delete;
    TmpDirManager& operator=(const TmpDirManager&) = delete;

private:
    std::filesystem::path tmp_dir_path_;  // 临时目录路径
};

// ============ 文件系统工具 ============

/** 确保目录存在，不存在则创建 */
void ensure_dir_exists(const std::filesystem::path& path);

// ============ 文件系统判定谓词（**不抛**版） ============
//
// `std::filesystem` 的**判定类**调用在"末段符号链接解不开"时不是判 not-found，而是**抛**
// `filesystem_error`。2026-09-25 实测（libstdc++，`ln -s self self` 之后逐条调用）：
//
//     fs::exists(loop)        → 抛 filesystem_error  code=40 (ELOOP)
//     fs::is_directory(loop)  → 抛 filesystem_error  code=40
//     fs::is_empty(loop)      → 抛 filesystem_error  code=40
//     fs::is_symlink(loop)    → 不抛（走 lstat）
//
// ⚠️ **订正 2026-09-26：上面那条 `is_symlink` 的结论只在"末段就是那个环"时成立。**
// **中间段**成环时它照样抛（实测，同一套环境）：
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
 * 与 `fs::is_symlink` 的**唯一**区别就是"不抛"，而这条区别在两种形态上都成立（见上面订正
 * 的那段实测）：末段自环（`fs::is_symlink` 侥幸不抛）与**中间段成环**（它抛）。写
 * `exists_no_follow(p) && !fs::is_symlink(p)` 这种**已经短路掉**的形态可以继续用抛版
 * （lstat 成功就说明没有 ELOOP），但只要右操作数有可能自己吃到环，就必须用本函数。
 */
bool is_symlink_no_follow(const std::filesystem::path& p);

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
 *     "别动 symlink→目录"的守卫会**恒假**（实测 `lstat("link/")` 报的是**目录**）。
 *   · **删除/改名类**（`rmdir` / `rename` / `unlink`）**不会**跟随末段链接，一律 `ENOTDIR`。
 *   · 唯一的例外是 **`fs::remove_all(带尾斜杠的路径)`**：实测它**删光链接目标的全部内容**
 *     并返回 `ENOTDIR`（`link -> real` 时 `real/` 里的条目全没了、链接原样留着）；不带尾斜杠
 *     则只删链接本身。**调用方拿它的返回值去删目录时必须先规范化。**
 *
 * **订正 2026-09-26**：本段原文写"`rmdir` 把 `/var/run -> ../run` 指向的真实 `/run` 删掉"，
 * 并据此把 `rmdir` 与 `chmod` 并列 —— **`rmdir` 那半实测不成立**（末段链接恒 `ENOTDIR`，
 * 连尾斜杠也救不了它）；真正危险的是上面第三条。同一处订正已在 `ARCH.md` 与 `op_sink.hpp`
 * 落过，只有这个文件没跟上。
 *
 * **任何针对物理路径的文件系统操作前先规范化**；DB 键本身保持带斜杠（那是键，不是路径）。
 */
std::filesystem::path strip_trailing_slash(const std::filesystem::path& p);

/**
 * 去除首尾空白（空格与制表符）。依赖串/索引字段/配置值都要按同一套规则归一，
 * 否则 " glibc"、"glibc " 会被当成两个不同的包名去找（TODO.md D1）。
 */
std::string trim_copy(std::string_view s);

/** 确保文件存在，不存在则创建 */
void ensure_file_exists(const std::filesystem::path& path);

// ============ 仓库索引行解析（**唯一**实现） ============

/**
 * 仓库索引（`constants::REPO_INDEX_FILE`）里一个**版本块**的原始字段。
 *
 * 行格式：`包名|版本:哈希:依赖:提供:needed_so;版本2:...|包级提供`
 * （这套格式的常量——`PIPE_CHAR`/`COLON_CHAR`/`COMMA_CHAR`/`SEMICOLON_CHAR`——就住在
 *   `base/constants.hpp`，故解析器也放在这一层，与格式常量同址。）
 *
 * **两个消费者必须共用这一个解析器**：`repo/repository.cpp`（建包表）与
 * `pkg/depend_scanner.cpp`（建 needed_so 反图）。它们曾各写一份，而 depend_scanner 那份
 * 要求版本块 **≥5 字段**、repository.cpp 那份**有意**容忍 4 字段 —— 4 字段的索引行
 * （旧/部分写入器把 provides 写在 `vh[3]`、不写 needed_so）在 `depend remove` /
 * `depend abibreak` 里被**整行丢掉** → 反向依赖图缺一整类边 → 静默报"无受影响包"
 * （给出错误的答案，而不是报错）。
 */
struct RepoIndexVersionBlock {
    std::string name;       ///< 行首的包名
    std::string version;    ///< vh[0]
    std::string hash;       ///< vh[1]，可为空（LankeBUILD 直出的索引不带哈希）
    std::string deps;       ///< vh[2] 原始串：逗号连接，复合约束需调用方再合并（split_dep_field）
    std::string provides;   ///< vh[3]；为空时**回退包级**（行内第 3 段）——旧格式兼容
    std::string needed_so;  ///< vh[4]；4 字段版本块下为空
};

/**
 * 解析一行索引，返回该行的**全部**版本块（空行/`#` 注释行 → 空表）。
 *
 * "取哪个版本"由调用方决定（`repository.cpp` 按 version_compare 排序后取最后；
 * `depend_scanner.cpp` 取版本号最大者）——解析器只负责字段，不替调用方选版本。
 * 版本号为空（`名|:哈希:...`）的畸形块跳过（与 farm 的 `graph.rs` 同判据）。
 */
std::vector<RepoIndexVersionBlock> parse_repo_index_line(std::string_view line);

/**
 * 读集合文件时"文件不存在"的处理策略。**只有调用点能选**，不设隐式默认值以外的东西：
 *   Throw —— 缺文件即抛 error.open_file_failed（**默认**，正常操作路径）
 *   Empty —— 缺文件返回空集（**只给崩溃恢复路径用**，见 Cache::load 的说明）
 *
 * 两个策略下"文件存在却打不开（权限/IO）"都**一律抛** —— 半损/不可读的库绝不能冒充空库。
 */
enum class MissingSetFilePolicy { Throw, Empty };

/** 从文件读取字符串集合（每行一个元素，自动去除 \r） */
std::unordered_set<std::string> read_set_from_file(
    const std::filesystem::path& path, MissingSetFilePolicy policy = MissingSetFilePolicy::Throw);
/** 将字符串集合写入文件（每行一个元素） */
void write_set_to_file(const std::filesystem::path& path,
                       const std::unordered_set<std::string>& data);
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
 * 所有 `.tmp + fsync + rename` 的写入路径都必须走这里，不要各写一套（TODO.md B1）。
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

/** 清理所有临时目录 */
void cleanup_tmp_dirs();

/**
 * 路径 p 是否在 root 之内（含 root 自身）。两边取 lexically_normal、root 尾部分隔符
 * 归一后，按目录边界比较（p == root，或以 root + "/" 开头）。
 *
 * **不要手写 `root + "/"` 前缀拼接**：root == "/" 时得到 "//"，而 lexically_normal
 * 后的路径不可能以 "//" 开头 → 判定恒为 false，把所有路径都判成"不在根内"。
 * root_dir == "/" 恰恰是常规安装（不带 --root）的形态。
 */
bool path_within(const std::filesystem::path& p, const std::filesystem::path& root);

/**
 * 单个路径分量是否安全：非空、不含 '/' 与 NUL、不是 "." / ".."。
 *
 * 包名与版本号来自**不可信来源**（远端索引、.lpkg 内的 metadata.json），而它们会被
 * 直接当成路径分量拼进 tmp_pkg_dir() / dep_dir() / docs_dir() / 下载 URL，
 * 一个 `../` 就能以 root 写到这些目录之外（TODO.md X4）。
 */
bool is_safe_path_component(std::string_view s);

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

/**
 * 回收孤儿备份 stash（TODO.md §5）：删除各文件系统根下、pid 已死的
 * `.lpkg_bak_<pkg>_<pid>` 目录（崩溃/续传未覆盖的残留）。范围：root_dir 顶层 +
 * root_dir 内每个挂载点（= stash 的落点集合，见 mount_points）；/proc 不可用时降级为
 * root 顶层 + 顶层子挂载点。只认"存活进程已消失"（kill(pid,0) 返回 ESRCH）的，
 * 绝不碰正在运行/自 pid 的 stash。
 */
void cleanup_orphan_stashes(const std::set<std::filesystem::path>& keep = {});

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
 * 仅做 rename(2)，失败一律抛异常。曾对 overlayfs 的 EXDEV 做 copy+remove_all
 * 回退，但 copy_recursive 对"指向目录的符号链接"会跟随链接误判为目录，
 * 递归删除整棵被 rename 的目录树（升级 filesystem 包时 /usr/lib 全树被删）。
 * 开 redirect_dir 的 overlay 目录 rename 本就不返回 EXDEV；宁可失败也不破坏数据。
 *
 * @param from  源路径
 * @param to    目标路径
 * @throw       std::filesystem::filesystem_error  rename 失败时
 */
void safe_rename(const std::filesystem::path& from, const std::filesystem::path& to);

// ============ 包路径 / 备份路径工具 ============

/** 生成随机小写字母+数字后缀（用于 .lpkg_bak / stash 文件名防冲突） */
std::string random_suffix(size_t len = constants::RANDOM_SUFFIX_LEN);

// ============ 字符串工具 ============

/** 替换字符串中的所有匹配子串 */
void string_replace_all(std::string& str, const std::string& from, const std::string& to);

/**
 * 用单引号把一段文本包成安全的 shell 参数；文本内的单引号按 POSIX 方式转义为 `'\''`。
 * 构建阶段命令行与 hook 执行路径共用它（此前两处各写一份）。
 */
std::string shell_quote(std::string_view s);

/**
 * 把 from 的 xattr 全部复制到 to（用 l* 变体：不跟随符号链接）。
 *
 * `std::filesystem::copy` **不复制 xattr**，而 `security.capability` 就是一个 xattr ——
 * ping/fping/traceroute/mtr 这类包靠**文件能力**而非 SUID，丢了它非 root 直接不能用。
 *
 * 语义是"**只写 from 有的键**"：逐个 `lsetxattr`，**从不删** `to` 上另有（from 没有的）键。
 * 这条性质被上层依赖：向一个**多包共用**的目录写 xattr 时，别的包设的键不会被抹掉。
 */
void copy_xattrs(const std::filesystem::path& from, const std::filesystem::path& to);

// ============ xattr 的逐键读写 ============
//
// 为什么需要这一组（而不是继续用 `copy_xattrs` 一把梭）：目录的 xattr 要进**事务**
// —— 写之前必须把**旧值**记进 WAL（否则回滚还原不了），所以调用方要能"逐键读旧值 → 记 →
// 逐键写新值"。`copy_xattrs` 是"from → to"的整份复制，表达不了"这一格要记旧值"。

/**
 * 列出 `p` 上的全部 xattr 键（lstat 语义：不跟随符号链接）。`p` 不存在 / 不支持 xattr
 * → 返回空表（**不是错误**：绝大多数目录没有任何 xattr）。
 */
std::vector<std::string> list_xattr_keys(const std::filesystem::path& p);

/**
 * 读 `p` 上某个键的值。**键不存在返回 nullopt**（与"值是空串"区分开：xattr 允许 0 长度
 * 的值，`lgetxattr` 返回 0 而键存在）。读失败（ENOTSUP 等）同样 nullopt。
 */
std::optional<std::vector<char>> read_xattr(const std::filesystem::path& p, const std::string& key);

/** 写一个键（覆盖或新建）。失败返回 false（调用方决定要不要告警/失败）。 */
bool write_xattr(const std::filesystem::path& p, const std::string& key,
                 const std::vector<char>& value);

/** 删一个键。**键本来就不存在** → 返回 false（与"删失败"同一个返回值：调用方只关心
 *  "删掉了没有"，两者都不需要额外处理）。 */
bool remove_xattr(const std::filesystem::path& p, const std::string& key);

// ============ base64（WAL 行里的键与值）============
//
// 用途**只有一个**：把 xattr 的**键与值**编进 WAL 行。它们都是任意字节串，而 WAL 是
// **行式、空格分帧、`" → "` 是箭头分界**的文本协议（见 wal_op.cpp 的 parse_op）——
// 裸放一个含 ` → `（或含换行、含尾部空格）的键，就会把一条行**重新分帧**成另一条合法行，
// 回滚侧照着重构出来的路径去 chmod/chown/lsetxattr。这与归档成员名那套消毒是**同一类**
// 问题（`archive.cpp` 的 reject 逻辑），但那里能拒绝整包，这里拒绝不了（键是模板/上游给的），
// 所以选择**编码**而不是拒绝。
//
// base64 的字母表（`A-Za-z0-9+/=`）不含空格、不含换行、不含 `→` —— 编码之后这些字符在
// 协议层不可能出现。**空值**编码成空串，故调用方要用一个哨兵把"空值"与"没有这一侧"
// 区分开（见 wal_op.cpp 的 XATTR_* 行）。

/** 标准 base64（带 `=` 填充；空输入 → 空串） */
std::string base64_encode(const std::vector<char>& data);

/** 解码；输入含非 base64 字符 / 长度非法 → nullopt（**不抛**：这是回滚路径上的输入） */
std::optional<std::vector<char>> base64_decode(std::string_view s);

/**
 * 按分隔符切分 string_view，返回子串列表（零拷贝，仅分配 vector）
 * @param s  输入的字符串视图
 * @param d  分隔字符
 * @return   切分后的子串列表
 */
inline std::vector<std::string_view> split_string_view(std::string_view s, char d)
{
    std::vector<std::string_view> r;
    size_t start = 0, end;
    while ((end = s.find(d, start)) != std::string_view::npos) {
        r.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    r.push_back(s.substr(start));
    return r;
}

// ============ 二进制文件处理(ELF) ============

/** ELF 文件类型枚举 */
// （2026-09-26 清理：此处原有 `enum class BinaryType` 与 `void strip_binary(...)`。
//   · `BinaryType` **只声明、全仓无人使用**（死代码）→ 删除；
//   · `strip_binary` 的唯一调用者是 `build/builder.cpp`，而它内部调 `elf/strip.cpp` 的
//     `strip_file` —— 它住在 base 层会让**最底层反向依赖 ELF 层**（`base/utils.cpp`
//     为了它 `#include "elf/strip.hpp"`）→ 已挪到 `elf/strip.hpp`，方向正过来了。）
