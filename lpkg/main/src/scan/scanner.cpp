#include "scanner.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#include "base/utils.hpp"
#include "config/config.hpp"
#include "db/cache.hpp"
#include "i18n/localization.hpp"

namespace fs = std::filesystem;

/**
 * 遍历 /usr、/etc、/opt、/var 等目录，跳过已知的系统路径与包管理器路径。
 */
void scan_orphans(const std::string& scan_root_override)
{
    log_info(get_string("info.scan_loading_db"));
    Cache& cache = Cache::instance();

    fs::path actual_root = Config::instance().root_dir();
    if (!scan_root_override.empty()) {
        actual_root = scan_root_override;
    }

    std::vector<fs::path> scan_roots = {actual_root};

    std::unordered_set<std::string> ignored_prefixes = {
        (actual_root / "etc/lpkg").string(),
        (actual_root / "usr/share/lpkg")
            .string(),  // 这两个是lpkg数据目录，有包DB本身，不是扫描范围
        (actual_root / "var").string(),
        (actual_root / "mnt").string(),  // 挂载点（如 /mnt/base 的镜像树）不属于本机
        (actual_root / "proc").string(),
        (actual_root / "sys").string(),
        (actual_root / "dev").string(),
        (actual_root / "run").string(),
        (actual_root / "tmp").string(),
        (actual_root / "etc/ssl/certs").string(),
        (actual_root / "etc/pki").string(),
        (actual_root / "home").string(),
        (actual_root / "root").string()};

    log_info(get_string("info.scan_start"));

    long long scanned_count = 0;
    long long orphan_count = 0;
    // 遍历是否被 increment 错误提前截断 —— 决定 summary 用哪条文案（见下面的 while 循环）。
    bool walk_truncated = false;

    for (const auto& root : scan_roots) {
        // 两处判定都走**不抛**的谓词：这两个 root 是拼出来的已知路径，
        // 其中**中间段**成环时抛型重载会以 ELOOP 打断整趟扫描 —— 而这不是"某个文件读不了"
        // （那种有下面的 try/catch 兜着），是整趟 `lpkg scan` 直接失败。
        // 判据本身保持原语义：`exists_follow` = 跟随语义的 `fs::exists`；
        // `is_symlink_no_follow` = `fs::is_symlink` 的 lstat 孪生（这个 root 名字**本身**
        // 是符号链接就跳过，例如 /bin -> /usr/bin）。
        if (!exists_follow(root)) continue;

        // 跳过本身就是符号链接的根目录（例如 /bin -> /usr/bin）
        if (is_symlink_no_follow(root)) continue;

        // 显式迭代器循环，且**把 increment 的错误码当成一等公民**。
        //
        // 为什么不能写 `for (...; it != end; it.increment(ec))`：`recursive_directory_iterator
        // ::increment(ec)` 出错时会把迭代器**置为 end**，而 `ec` 是在**循环条件里**被求值的
        // —— 循环体永远没机会看它。于是"走到一半失败"与"正常走完"**完全无法区分**，
        // 扫描结果**静默截断**，summary 还照打一个看着挺大的数。
        // 不剪枝时走 `/` 会在 `/proc/<pid>/task/<pid>/net` 上拿到
        // `EINVAL(22)` → 迭代器变 end → 整趟扫描停在 `/proc` 里，`/usr`（28 万个文件）
        // **一个都没扫到**；而它仍打印"共扫描 389551 个文件"。
        std::error_code walk_ec;
        const auto end = fs::recursive_directory_iterator();
        auto it =
            fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied);
        while (it != end) {
            const auto& entry = *it;
            const std::string path = entry.path().string();  // path() 不抛（取的是缓存值）
            try {
                if (entry.is_symlink() || entry.is_regular_file()) {
                    scanned_count++;

                    bool ignored = false;
                    for (const auto& prefix : ignored_prefixes) {
                        // **分量级**前缀，不是字符串前缀：`path.compare(0, n,
                        // prefix)` 会把 `usr/manual`、`var/logrotate` 这类**同级**目录也算成命中
                        // `usr/man` / `var/log`，害得它们下面的孤儿被静默忽略。要求前缀之后紧跟
                        // `/`（或路径恰好 就是该前缀本身）。
                        if (path.compare(0, prefix.size(), prefix) == 0 &&
                            (path.size() == prefix.size() || path[prefix.size()] == '/')) {
                            ignored = true;
                            break;
                        }
                    }
                    if (ignored) {
                        // 命中忽略前缀 → 不报告。**目录**不在这里剪枝（下面单独判），
                        // 因为目录条目根本不会走到这个分支。
                    } else {
                        std::string key = path;
                        if (actual_root != "/") {
                            // lexically_relative 而非 fs::relative：后者会**解析符号链接**，
                            // 非 / root 下 `lib64 -> lib` 这类路径会被换成真实路径、
                            // 与登记的属主键对不上 → 假孤儿
                            fs::path relative = entry.path().lexically_relative(actual_root);
                            key = "/" + relative.string();
                        }

                        if (cache.get_file_owners(key).empty()) {
                            std::cout << path << std::endl;
                            orphan_count++;
                        }
                    }
                } else {
                    // ── 剪枝：被忽略的目录**根本不走进去** ──────────────
                    // `ignored_prefixes` 此前只用于**过滤报告**，遍历照样递归进 `/proc` /
                    // `/sys` / `/var` —— 那既是本缺陷的**触发器**（`/proc` 下的目录随进程生灭，
                    // 走进去必然撞上 increment 失败 → 整趟截断），也是纯粹的浪费
                    // （`/var` 13 万个条目扫了又不报）。
                    // 判据用**不抛**的 `is_directory(ec)`；`is_symlink()` 已在上面短路，
                    // 所以这里不会是"指向目录的符号链接"（那类按老规矩仍算文件条目上报）。
                    std::error_code dir_ec;
                    if (entry.is_directory(dir_ec) && !dir_ec) {
                        for (const auto& prefix : ignored_prefixes) {
                            if (path == prefix) {
                                it.disable_recursion_pending();
                                break;
                            }
                        }
                    }
                }
            } catch (const std::exception& e) {
                log_warning(string_format("warning.scan_file_error", path, e.what()));
            }

            it.increment(walk_ec);
            if (walk_ec) {
                // 迭代器已被置为 end ⇒ 无法从该位置续走。**绝不静默**：点名断在哪、
                // 什么错，并让 summary 显式报"不完整"，免得用户以为"扫完了、系统很干净"。
                log_warning(string_format("warning.scan_walk_aborted", path, walk_ec.message()));
                walk_truncated = true;
                break;
            }
        }
    }
    log_info(string_format(walk_truncated ? "info.scan_complete_truncated" : "info.scan_complete",
                           scanned_count, orphan_count));
}
