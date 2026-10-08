#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace depscan
{

// ── 树节点状态 ─────────────────────────────────────────────────────────────
enum class ScanStatus {
    REMOVED,
    REBUILD,
    INSTALL,
    ABI_CHANGED,  // ABI 变化的目标包自身（不重编）
    KEEP          // 保持不变（--all 时才显示）
};

// ── 依赖树节点 ─────────────────────────────────────────────────────────────
struct ScanNode {
    std::string name;
    std::string version;
    ScanStatus status = ScanStatus::KEEP;
    std::string reason;
    std::vector<ScanNode> children;

    bool is_affected() const
    {
        return status == ScanStatus::REMOVED || status == ScanStatus::REBUILD ||
               status == ScanStatus::INSTALL;
    }
};

// ── 公开扫描 API ───────────────────────────────────────────────────────────

// 走反向依赖的传递闭包；show_all 时也列出共享、因而保留的前向依赖。
ScanNode scan_remove_tree(const std::string& pkg_name, bool show_all = false);

// 只有直接反向依赖受影响（间接的隔着直接层的抽象，不会随 ABI 变化而断）。
// show_all 时也列出保留的间接反向依赖。
ScanNode scan_abibreak_tree(const std::string& pkg_name, bool show_all = false);

// 经仓库解析传递依赖；show_all 时也列出树里已装的包。
ScanNode scan_install_tree(const std::string& pkg_name, bool show_all = false);

ScanNode scan_install_from_file(const std::filesystem::path& lpkg_path, bool show_all = false);

// ── 显示 ───────────────────────────────────────────────────────────────────

// 以 Unicode 制表符把依赖树打印到 stdout。
void print_tree(const ScanNode& node);

// ScanStatus → l10n 键（调用方用 `get_string(...)` 取译文）。以前这里返回硬编码英文标签，
// 而 `depend`/`scan` 的树形输出是**用户可见**的 —— 与 CLI 其余部分不一致。
std::string_view status_label_key(ScanStatus s);

}  // namespace depscan
