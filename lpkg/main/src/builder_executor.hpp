#pragma once

#include <string>
#include <vector>
#include <map>
#include <filesystem>

// Download sources and work_sources; auto-extract archives to work_root.
// Returns list of files that were actually downloaded this run.
std::vector<std::filesystem::path>
download_and_prepare_sources(const std::vector<std::string>& sources,
                             const std::vector<std::string>& work_sources,
                             const std::filesystem::path& build_dir,
                             const std::filesystem::path& work_root);

// Detect a single-subdirectory source tree inside work_root.
// Returns work_root if the tree has zero or >1 top-level directories,
// otherwise returns the lone subdirectory.
std::filesystem::path
detect_source_tree(const std::filesystem::path& work_root);

// Substitute {PKG_NAME}, {PKG_VER}, … placeholders in a LankeBUILD script.
// Returns the processed script text.
std::string
process_build_script(const std::filesystem::path& script_path,
                     const std::map<std::string, std::string>& vars);

// Execute a named build phase (e.g. "lankebuild_build") via the shell.
// processed_script_path is the post-substitution script to source first.
void
execute_build_phase(const std::string& phase_name,
                    const std::filesystem::path& work_dir,
                    const std::filesystem::path& processed_script_path);
