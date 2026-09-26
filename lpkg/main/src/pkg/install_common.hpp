#pragma once

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "archive.hpp"
#include "base/constants.hpp"
#include "base/exception.hpp"
#include "base/utils.hpp"
#include "config/config.hpp"
#include "db/cache.hpp"
#include "i18n/localization.hpp"
#include "nlohmann/json.hpp"
#include "op_sink.hpp"
#include "package_manager.hpp"
#include "repo/repository.hpp"
#include "vercmp/version.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

// InstallPlan、InstallContext、InstallTask 已在 package_manager.hpp 中前置声明

namespace detail
{

/// 在 chroot（若需要）中执行 hook 脚本（postinst / prerm）
void run_hook(std::string_view pkg_name, std::string_view hook_name);

/// 从 .lpkg 存档中读取 metadata.json（无需完整解压整个包）
/// 返回解析后的 JSON，若缺失或无法读取则抛出 LpkgException
nlohmann::json read_archive_metadata(const std::filesystem::path& archive_path);

/// 从已解压的包目录中读取 metadata.json
void read_package_metadata(const fs::path& tmp_pkg_dir, std::string& name, std::string& version,
                           std::vector<std::string>& deps, std::vector<std::string>& provides,
                           std::vector<std::string>& needed_so, std::string& man);

/// 扫描 content/ 目录，返回相对路径列表
std::vector<std::string> scan_content_files(const fs::path& content_dir);

/// 解析原始依赖字符串（如 "libfoo >= 1.0"）为 DependencyInfo 结构体
std::vector<DependencyInfo> parse_dep_strings(const std::vector<std::string>& dep_strs);

// ============================================================================
// 从 `installation_task.cpp` 拆出后**多趟共用**的纯函数
// （让开趟 / 写入趟 / 注册趟各在自己的 TU 里，两个函数都要用 → 下沉到这里）
// ============================================================================

/**
 * 配置文件升级时的**三哈希分流**判定表（pacman `add.c` 的三条分支 + 老 DB 的退化路径）。
 * 语义只有这一份，别在调用点再写一遍 —— 完整判定表见 .cpp 里的定义处。
 *
 * 输入：`hash_local`（盘上那份）/ `hash_orig`（confhashes 记的"上次我们装进去的"）/
 * `hash_pkg`（本次包里那份）。输出：`ConfigDisposition`（落点，见 op_sink.hpp）。
 */
ConfigDisposition classify_config_update(std::string_view hash_local, std::string_view hash_orig,
                                         std::string_view hash_pkg);

/**
 * `.lpkgtmp` 落位前的最后一道闸：tmp 路径是**符号链接**时拒绝写入（抛 `LpkgException`）。
 * 判据只能是 `is_symlink` 不能是 `exists` —— 完整理由见 .cpp 里的定义处。
 */
void refuse_symlink_tmp_path(const std::filesystem::path& tmp_path);

/// 用 libsolv 求解安装/升级/重装计划，填充 InstallContext 的 plan + install_order。
/// 取代旧的手动递归解析 resolve_package_dependencies 及其配套手动校验
/// （check_plan_consistency / check_needed_so_consistency / check_forward_soname_integrity）：
/// solver 建模 installed repo 的 requires（deps+needed_so），求解时原生覆盖
/// 依赖拉入、版本约束、ABI 反向一致性、缺 provider 检测。
void resolve_with_solver(InstallContext& ctx);

/// 从已持有的包开始 BFS 遍历依赖图，获取所有必需的包（供 autoremove）
std::unordered_set<std::string> get_all_required_packages();

// ============================================================================
// 目录整树删除（remove 与 upgrade 共用，见 ARCH.md §3.6）
// ============================================================================

/**
 * `remove_empty_owned_dirs()` 的逐目录回调：`removed == true` = 这个目录真的被 `rmdir`
 * 掉了（`DIR_RM` 已写）；`false` = 因"含无主内容"整树保留。**只用于报告**
 * （升级侧据此打 `info.removing_obsolete_file` 日志与 `LPKG_TRACE_REMOVE` 追踪），
 * 不参与任何判据 —— 传 nullptr 即完全不报告。
 */
using DirReporter = void (*)(const std::filesystem::path& phys, bool removed);

/**
 * 删除一组"本包独占、且此刻为空"的 owned 目录（`DIR_RM` + 元数据记录）。
 *
 * **唯一实现**：移除侧（`do_remove_package()` 阶段 B）与升级侧
 * （`remove_obsolete_files()` 阶段 2）共用 —— 两处原本各写了一遍"剥尾斜杠 → 最深优先 →
 * 最后持有者 → 真目录 → 此刻为空"，任一侧改了守卫另一侧不会跟着变。
 *
 * **候选集与"时序守卫"留在调用点**（那是两侧真正的差异，不塞进这里用参数开关表达）：
 *   · 移除侧 = 本包全部 owned 目录键；
 *   · 升级侧 = 仅"新版本不再提供"的，且先排除"新版本在该目录下还有条目"的
 *     （那一趟跑在写入**之前**，判据要钉回与原来"写入之后"同一口径）。
 *
 * 判据（五条，逐条见 .cpp 的定义处）与"目录型挂载点保留 + 告警"都在本函数内。
 *
 * @param candidate_dir_keys 候选目录键（**DB 键的原始形态**，带尾斜杠）
 * @param report             逐目录报告回调（可空；只影响日志/追踪，不影响判据）
 */
void remove_empty_owned_dirs(Cache& cache, const std::string& pkg,
                             const std::vector<std::string>& candidate_dir_keys, OpSink& sink,
                             DirReporter report = nullptr);

// ============================================================================
// xattr 键的撤销（remove 与 upgrade 共用）
// ============================================================================

/**
 * 本包在某个目录上"声明过、但现在不该再有了"的某个键 → 撤掉它。
 *
 * **唯一实现**：升级侧（`revoke_undeclared_xattrs()`：新版本不再声明这个键）与移除侧
 * （`do_remove_package()`：整包没了）共用 —— 两处的差异只在**怎么算出待撤清单**，而
 * "撤一个键"这件事的判据（别的包还持有吗 / 盘上还有吗 / 行怎么写）必须只有一份。
 *
 * 三件事，顺序不可换：
 *   1. **别的包仍持有这个键 → 不动盘**，只摘本包的登记。xattr 是按目录共用的，一个目录
 *      被多个包持有是常态 —— 按"目录还有没有别的属主"判会撤掉**别人的**键。
 *   2. **撤之前先把旧值写进 WAL**（`OpSink::unset_xattr` 内部做：改前有值 → `XATTR_SET`），
 *      这样整批回滚能把键逐字节还原。
 *   3. 从归属表里摘掉本包；无人持有则整条记录消失（`remove_xattr_key_owner` 负责）。
 *      第 3 步**无论第 1 步走哪一支都要做**：本包确实不再声明它了。
 *
 * @param logical 目录的**逻辑路径**（DB 目录键形态，带尾斜杠，如 `/usr/share/x/`）
 * @return 是否真的从盘上撤掉了（false = 别的包还持有 / 盘上本来就没有那个键）
 */
bool revoke_xattr_key_if_unowned(Cache& cache, const std::string& pkg, const std::string& logical,
                                 const std::string& key, OpSink& sink,
                                 const std::filesystem::path& root);

// ============================================================================
// 每文件系统 sidecar stash（TODO.md 第 2 节）
// 「备份 + 它的 WAL 行」的成对写入在写入层原语 detail::OpSink（pkg/op_sink.hpp）里；
// 这里只放 stash 自身的路径计算与清理。
// ============================================================================

/**
 * phys 所在文件系统的顶层（= 可安全放 stash 的目录）。沿 phys 所在目录向上走到
 * **挂载点**（/proc/self/mountinfo，vfsmount 才是 rename 的 EXDEV 边界）或
 * `Config::root_dir()` 边界为止，**恒 clamp 在 root_dir() 内**（chroot 运行）。
 * stash 放这里保证与 phys 同一挂载 → rename(2) 永不 EXDEV、永不逃出 chroot。
 * 多文件系统时各自一个 stash 根。边界判据不用 st_dev：overlay 上目录与 upper 层
 * 文件的 st_dev 本就不同（见 utils.hpp 的 mount_points 注释）。
 */
std::filesystem::path stash_parent_dir(const std::filesystem::path& phys);

/// 返回并确保 phys 的 stash 目录：`<stash_parent_dir(phys)>/.lpkg_bak_<pkg>_<pid>`，
/// mode 0700、root-only（备份残留隔离，不被普通工具/扫描看到）。
std::filesystem::path ensure_stash_dir(const std::filesystem::path& phys, std::string_view pkg);

/**
 * 在 phys 的 stash 内分配一个唯一 bak 目标（不移动、不写 WAL）。备份文件扁平存放：
 * `<stash>/<原名>.lpkg_bak_<pkg>_<rand>`；同 basename 由随机后缀保证唯一，
 * 还原路径靠调用方写的 BACKUP/REMOVE_OLD WAL 记录映射。
 */
std::filesystem::path stash_bak_target(const std::filesystem::path& phys, std::string_view pkg);

/// 删除一个 stash 目录（整体 remove_all）。
void remove_stash_dir(const std::filesystem::path& stash);

// 注：「删除空目录 + DIR_RM 元数据记录」已并入写入层原语
// `detail::OpSink::remove_empty_dir()`（`pkg/op_sink.hpp`）—— WAL 行与 rmdir 成对发生，
// 路径规范化（strip_trailing_slash）也在那里统一做，不再有第二个入口。

}  // namespace detail
