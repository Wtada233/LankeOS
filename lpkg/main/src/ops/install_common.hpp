#pragma once

#include "package_manager.hpp"
#include "core/repository.hpp"
#include "archive.hpp"
#include "core/constants.hpp"
#include "core/config.hpp"
#include "core/cache.hpp"
#include "core/exception.hpp"
#include "core/localization.hpp"
#include "core/version.hpp"
#include "core/utils.hpp"
#include "nlohmann/json.hpp"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

// Forward declared in package_manager.hpp: InstallPlan, InstallContext, InstallationTask

namespace detail {

// Run a hook script (postinst/prerm) inside chroot if needed
void run_hook(std::string_view pkg_name, std::string_view hook_name);

// Read metadata.json from an .lpkg archive (without extracting the whole package)
// Returns parsed JSON. Throws LpkgException if missing or unreadable.
nlohmann::json read_archive_metadata(const std::filesystem::path& archive_path);

// Read metadata.json from an extracted package directory
void read_package_metadata(const fs::path& tmp_pkg_dir, std::string& name, std::string& version,
                           std::vector<std::string>& deps, std::vector<std::string>& provides,
                           std::string& man);

// Scan content/ directory → list of relative paths
std::vector<std::string> scan_content_files(const fs::path& content_dir);

// Parse raw dep strings (e.g. "libfoo >= 1.0") into DependencyInfo structs
std::vector<DependencyInfo> parse_dep_strings(const std::vector<std::string>& dep_strs);

// Resolve one package and its transitive deps into the shared plan
void resolve_package_dependencies(const std::string& pkg_name, const std::string& version_spec,
                                  bool is_explicit, InstallContext& ctx,
                                  std::set<std::string>& visited_stack);

// Check if upgrading packages in plan would break already-installed packages
std::set<std::string> check_plan_consistency(const std::map<std::string, InstallPlan>& plan);

// BFS over the dependency graph starting from held packages
std::unordered_set<std::string> get_all_required_packages();

} // namespace detail
