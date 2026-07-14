#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace depscan {

// ── Status labels for tree nodes ───────────────────────────────────────────
enum class ScanStatus {
  REMOVED,     // Package would be removed
  REBUILD,     // Package needs ABI-triggered rebuild
  INSTALL,     // Package would be newly installed
  ABI_CHANGED, // Target package whose ABI changed (not rebuilt itself)
  KEEP         // Package stays unchanged (shown with --all)
};

// ── A single node in the dependency tree ───────────────────────────────────
struct ScanNode {
  std::string name;
  std::string version;
  ScanStatus status = ScanStatus::KEEP;
  std::string reason; // optional: short explanation
  std::vector<ScanNode> children;

  bool is_affected() const {
    return status == ScanStatus::REMOVED || status == ScanStatus::REBUILD ||
           status == ScanStatus::INSTALL;
  }
};

// ── Public scan API ────────────────────────────────────────────────────────

// Scan what would be REMOVED (broken) if <pkg> is removed.
// Walks the transitive closure of reverse dependencies.
// show_all: also show forward dependencies that are shared and thus stay.
ScanNode scan_remove_tree(const std::string &pkg_name, bool show_all = false);

// Scan what would need an ABI-triggered REBUILD if <pkg> changes ABI.
// Only direct reverse dependencies are affected (indirect ones are
// shielded by the direct layer's abstraction).
// show_all: also show indirect reverse deps (they stay).
ScanNode scan_abibreak_tree(const std::string &pkg_name, bool show_all = false);

// Scan what would be newly INSTALLED if <pkg> (or a local .lpkg file)
// is installed.  Resolves transitive deps via the repository.
// show_all: also show already-installed packages in the tree.
ScanNode scan_install_tree(const std::string &pkg_name, bool show_all = false);

// Convenience: accept local .lpkg path for install scan
ScanNode scan_install_from_file(const std::filesystem::path &lpkg_path,
                                bool show_all = false);

// ── Display ────────────────────────────────────────────────────────────────

// Pretty-print a dependency tree to stdout with Unicode box-drawing.
void print_tree(const ScanNode &node);

// Get a human-readable label for a ScanStatus.
std::string_view status_label(ScanStatus s);

} // namespace depscan
