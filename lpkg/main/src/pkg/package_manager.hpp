#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "repo/repository.hpp"

/// 安装计划：已解析完毕、待安装的包
struct InstallPlan {
  std::string name, actual_version, sha256;
  bool is_explicit = false; ///< 用户显式指定安装
  std::filesystem::path local_path;
  std::vector<DependencyInfo> dependencies;
  std::vector<std::string> provides;
  std::vector<std::string> needed_so;
  bool force_reinstall = false;   ///< 强制重新安装
  bool metadata_verified = false; ///< 是否已验证元数据
};

/// 递归安装事务的共享上下文
struct InstallContext {
  Repository &repo;
  std::map<std::string, InstallPlan> &plan;
  std::vector<std::string> &install_order;
  std::map<std::string, std::filesystem::path> &local_candidates;
  std::vector<std::pair<std::string, std::string>> &targets;
  bool force_reinstall;
  bool top_level; ///< 是否为顶级调用（非递归子调用）
  std::vector<std::string> successfully_installed; ///< 当前事务中已成功安装的包
  std::unordered_set<std::string>
      installed_set{}; ///< 与 successfully_installed 同步，用于 O(1) 成员检查
};

class InstallationTask {
public:
  InstallationTask(std::string pkg_name, std::string version,
                   bool explicit_install,
                   std::string old_version_to_replace = "",
                   std::filesystem::path local_package_path = "",
                   std::string expected_hash = "",
                   bool force_reinstall = false);

  /// 主入口：执行安装任务，ctx 用于递归依赖发现
  void run(InstallContext *ctx = nullptr);

  /// 外部调用者仍使用旧接口
  void run_simple() { run(nullptr); }

  // --- 元数据验证模式（公开） ---
  void download_and_verify_package();
  void extract_and_validate_package();

  // --- 测试用（公开） ---
  void copy_package_files();
  void rollback_files(); ///< 包级文件回滚（含 RESTORE_* WAL 审计）
  /// 获取备份列表（供批次成功后清理 .lpkg_bak）
  const std::vector<std::pair<std::filesystem::path, std::filesystem::path>> &
  get_backups() const { return backups_; }

  // 元数据验证调用者的访问器
  const std::vector<std::string> &deps() const { return deps_; }
  const std::vector<std::string> &provides() const { return provides_; }
  const std::filesystem::path &archive_path() const { return archive_path_; }
  const std::filesystem::path &tmp_pkg_dir() const { return tmp_pkg_dir_; }
  void set_tmp_dir(const std::filesystem::path &p) { tmp_pkg_dir_ = p; }

private:
  std::string pkg_name_;
  std::string version_;
  bool explicit_install_ = false;
  std::filesystem::path tmp_pkg_dir_;
  std::string actual_version_;
  std::filesystem::path archive_path_;
  std::string old_version_to_replace_;
  std::filesystem::path local_package_path_;
  std::string expected_hash_;
  bool has_config_conflicts_ = false;
  bool force_reinstall_ = false;
  std::vector<std::string> deps_;
  std::vector<std::string> provides_;
  std::vector<std::string> needed_so_;
  std::string man_content_;

  void prepare(InstallContext *ctx = nullptr);
  void ensure_dependencies_satisfied(InstallContext &ctx);
  void check_for_file_conflicts(InstallContext *ctx = nullptr);
  void backup_existing_files();
  void cleanup_backups();
  void commit_without_file_ops();
  void register_package();
  void run_post_install_hook();

public:
  // 测试钩子（非生产用途）：在 copy_package_files 每个文件复制前调用
  std::function<void()> on_before_file_copy;

private:
  std::vector<DependencyInfo> parse_deps() const;

  // 事务状态（仅供文件级备份/清理使用，不含事务保护语义）
  std::vector<std::pair<std::filesystem::path, std::filesystem::path>> backups_;
  std::vector<std::filesystem::path> new_files_;
  std::vector<std::filesystem::path> new_dirs_;
};

/// 公共 API：安装包
void install_packages(const std::vector<std::string> &pkg_args,
                      const std::string &hash_file = "",
                      bool force_reinstall = false);

void remove_package(const std::string &pkg_name, bool force = false,
                    bool wrap_in_txn = true);
void autoremove();
void upgrade_packages();
void reinstall_package(const std::string &pkg_name);
void query_package(const std::string &pkg_name);
void query_file(const std::string &filename);
void show_man_page(const std::string &pkg_name);
void write_cache();
void remove_package_files(const std::string &pkg_name, bool force = false);
void remove_package_recursive(const std::string &pkg_name, bool force = false);
