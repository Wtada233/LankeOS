#include "cache.hpp"
#include "config.hpp"
#include "exception.hpp"
#include "localization.hpp"
#include "transaction_log.hpp"
#include "utils.hpp"
#include "wal_op.hpp"
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace fs = std::filesystem;

Cache &Cache::instance() {
  static Cache instance;
  return instance;
}

Cache::Cache() { load(); }

bool Cache::is_installed(std::string_view name) {
  std::lock_guard<std::mutex> lock(mtx);
  return installed_pkgs.contains(name);
}

std::string Cache::get_installed_version(std::string_view name) {
  std::lock_guard<std::mutex> lock(mtx);
  auto it = installed_pkgs.find(name);
  if (it != installed_pkgs.end())
    return it->second;
  if (providers.count(name))
    return "virtual";
  return "";
}

bool Cache::is_essential(std::string_view name) {
  std::lock_guard<std::mutex> lock(mtx);
  ensure_essentials();
  return essentials.contains(std::string(name));
}

bool Cache::is_held(std::string_view name) {
  std::lock_guard<std::mutex> lock(mtx);
  return holdpkgs.contains(std::string(name));
}

void Cache::add_installed(std::string_view name, std::string_view ver,
                          bool hold) {
  std::lock_guard<std::mutex> lock(mtx);
  installed_pkgs[std::string(name)] = std::string(ver);
  if (hold)
    holdpkgs.insert(std::string(name));
  dirty = true;
}

void Cache::remove_installed(std::string_view name) {
  std::lock_guard<std::mutex> lock(mtx);
  installed_pkgs.erase(std::string(name));
  holdpkgs.erase(std::string(name));
  dirty = true;
}

void Cache::add_file_owner(std::string_view path, std::string_view pkg) {
  std::lock_guard<std::mutex> lock(mtx);
  file_db[std::string(path)].insert(std::string(pkg));
  dirty = true;
}

void Cache::remove_file_owner(std::string_view path, std::string_view pkg) {
  std::lock_guard<std::mutex> lock(mtx);
  auto it = file_db.find(path);
  if (it != file_db.end()) {
    it->second.erase(std::string(pkg));
    if (it->second.empty())
      file_db.erase(it);
    dirty = true;
  }
}

bool Cache::is_file_owned_by(std::string_view path, std::string_view pkg) {
  std::lock_guard<std::mutex> lock(mtx);
  auto it = file_db.find(path);
  return it != file_db.end() && it->second.contains(std::string(pkg));
}

std::unordered_set<std::string> Cache::get_file_owners(std::string_view path) {
  std::lock_guard<std::mutex> lock(mtx);
  auto it = file_db.find(path);
  return (it != file_db.end()) ? it->second : std::unordered_set<std::string>{};
}

void Cache::add_provider(std::string_view capability, std::string_view pkg) {
  std::lock_guard<std::mutex> lock(mtx);
  providers[std::string(capability)].insert(std::string(pkg));
  dirty = true;
}

void Cache::remove_provider(std::string_view capability, std::string_view pkg) {
  std::lock_guard<std::mutex> lock(mtx);
  auto it = providers.find(capability);
  if (it != providers.end()) {
    it->second.erase(std::string(pkg));
    if (it->second.empty())
      providers.erase(it);
    dirty = true;
  }
}

std::unordered_set<std::string>
Cache::get_providers(std::string_view capability) {
  std::lock_guard<std::mutex> lock(mtx);
  auto it = providers.find(capability);
  return (it != providers.end()) ? it->second
                                 : std::unordered_set<std::string>{};
}

bool Cache::is_provided_by(std::string_view capability, std::string_view pkg) {
  std::lock_guard<std::mutex> lock(mtx);
  auto it = providers.find(capability);
  return it != providers.end() && it->second.contains(std::string(pkg));
}

void Cache::add_reverse_dep(std::string_view dep, std::string_view pkg) {
  std::lock_guard<std::mutex> lock(mtx);
  ensure_reverse_deps();
  reverse_deps[std::string(dep)].insert(std::string(pkg));
}

void Cache::remove_reverse_dep(std::string_view dep, std::string_view pkg) {
  std::lock_guard<std::mutex> lock(mtx);
  ensure_reverse_deps();
  auto it = reverse_deps.find(dep);
  if (it != reverse_deps.end()) {
    it->second.erase(std::string(pkg));
    if (it->second.empty())
      reverse_deps.erase(it);
  }
}

std::unordered_set<std::string> Cache::get_reverse_deps(std::string_view name) {
  std::lock_guard<std::mutex> lock(mtx);
  ensure_reverse_deps();
  auto it = reverse_deps.find(std::string(name));
  return (it != reverse_deps.end()) ? it->second
                                    : std::unordered_set<std::string>{};
}

std::unordered_set<std::string> Cache::get_package_files(std::string_view pkg) {
  std::lock_guard<std::mutex> lock(mtx);
  std::string pkg_str(pkg);
  std::unordered_set<std::string> result;
  for (const auto &[file, owners] : file_db) {
    if (owners.contains(pkg_str)) {
      result.insert(file);
    }
  }
  return result;
}

std::unordered_set<std::string>
Cache::get_package_provides(std::string_view pkg) {
  std::lock_guard<std::mutex> lock(mtx);
  std::string pkg_str(pkg);
  std::unordered_set<std::string> result;
  for (const auto &[cap, owners] : providers) {
    if (owners.contains(pkg_str)) {
      result.insert(cap);
    }
  }
  return result;
}

void Cache::load() {
  std::lock_guard<std::mutex> lock(mtx);
  file_db = read_db_uncached(Config::instance().files_db());
  providers = read_db_uncached(Config::instance().provides_db());

  auto pkg_set = read_set_from_file(Config::instance().pkgs_file());
  installed_pkgs.clear();
  for (const auto &line : pkg_set) {
    if (auto pos = line.find(':'); pos != std::string::npos) {
      installed_pkgs[line.substr(0, pos)] = line.substr(pos + 1);
    }
  }

  holdpkgs = read_set_from_file(Config::instance().holdpkgs_file());
  reverse_deps.clear();
  reverse_deps_loaded = false;
  essentials.clear();
  essentials_loaded = false;
  dirty = false;
}

void Cache::ensure_reverse_deps() {
  if (reverse_deps_loaded)
    return;
  reverse_deps.clear();
  if (fs::exists(Config::instance().dep_dir()) &&
      fs::is_directory(Config::instance().dep_dir())) {
    for (const auto &entry :
         fs::directory_iterator(Config::instance().dep_dir())) {
      if (entry.is_regular_file()) {
        std::string pkg_name = entry.path().filename().string();
        std::ifstream f(entry.path());
        std::string line;
        while (std::getline(f, line)) {
          if (line.empty())
            continue;
          std::string_view sv = line;
          if (sv.back() == '\r')
            sv.remove_suffix(1);
          if (auto pos = sv.find_first_of(" \t");
              pos != std::string_view::npos) {
            sv = sv.substr(0, pos);
          }
          if (!sv.empty()) {
            reverse_deps[std::string(sv)].insert(pkg_name);
          }
        }
      }
    }
  }
  reverse_deps_loaded = true;
}

void Cache::ensure_essentials() {
  if (essentials_loaded)
    return;
  if (fs::exists(Config::instance().essential_file())) {
    essentials = read_set_from_file(Config::instance().essential_file());
  }
  essentials_loaded = true;
}

// ── 直接写入（无 WAL 保护） ────────────────────────────────────────
// 注意：本版本不提供 WAL 保护的原子写入。数据库文件直接覆写。
// WAL 保护的写入将在新的事务架构中重新实现。

void Cache::write() {
  if (dirty) {
    write_file_db();
    write_providers();
    write_pkgs();
    write_holdpkgs();
    dirty = false;
  }
}

void Cache::write(const std::string &milestone) {
  // WAL 2.0: 对所有 DB 文件使用 write-ahead 顺序写入
  //   WAL DB <path> <milestone> → fsync → 备份 → fsync →
  //   .tmp → fsync → rename → fsync parent
  // 反向回滚时从 .lpkg_db_bak_before:<milestone> 恢复

  auto &config = Config::instance();
  auto pkgs_data = build_pkgs_set();
  write_set_file_wal(config.pkgs_file(), pkgs_data, milestone, "DB");
  write_db_file_wal(config.files_db(), file_db, milestone, "DB");
  write_db_file_wal(config.provides_db(), providers, milestone, "DB");
  write_set_file_wal(config.holdpkgs_file(), holdpkgs, milestone, "DB");
  dirty = false;
}

// build_pkgs_set: 将 installed_pkgs map 转换为 set<string> 格式
std::unordered_set<std::string> Cache::build_pkgs_set() const {
  std::unordered_set<std::string> result;
  for (const auto &[name, ver] : installed_pkgs)
    result.insert(name + ":" + ver);
  return result;
}

void Cache::write_pkgs() {
  std::unordered_set<std::string> pkg_set;
  for (const auto &[name, ver] : installed_pkgs) {
    pkg_set.insert(name + ":" + ver);
  }
  write_set_file_direct(Config::instance().pkgs_file(), pkg_set);
}

void Cache::write_holdpkgs() {
  write_set_file_direct(Config::instance().holdpkgs_file(), holdpkgs);
}

void Cache::write_file_db() {
  write_db_file_direct(Config::instance().files_db(), file_db);
}

void Cache::write_providers() {
  write_db_file_direct(Config::instance().provides_db(), providers);
}

// ── 文件系统辅助 ───────────────────────────────────────────────────

namespace {

void atomic_write_with_fsync(const fs::path &dst, const fs::path &tmp) {
  int fd = ::open(tmp.c_str(), O_WRONLY);
  if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
  }
  fs::rename(tmp, dst);
  fsync_parent_dir(dst);
}

} // namespace

void Cache::write_db_file_direct(
    const fs::path &path,
    const std::map<std::string, std::unordered_set<std::string>, std::less<>>
        &db) {
  const fs::path tmp = path.string() + ".tmp";

  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f.is_open())
      throw LpkgException(string_format("error.create_tmp_db_failed"));
    for (const auto &[key, values] : db) {
      std::string joined;
      for (const auto &v : values) {
        if (!joined.empty())
          joined += ',';
        joined += v;
      }
      f << key << "\t" << joined << "\n";
    }
  }

  atomic_write_with_fsync(path, tmp);
}

void Cache::write_set_file_direct(
    const fs::path &path,
    const std::unordered_set<std::string> &data) {
  const fs::path tmp = path.string() + ".tmp";

  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f.is_open())
      throw LpkgException(
          string_format("error.create_file_failed", tmp.string()));
    for (const auto &item : data)
      f << item << "\n";
  }

  atomic_write_with_fsync(path, tmp);
}


// ============================================================================
// WAL 保护的写入方法（write-ahead 顺序）
// ============================================================================

void Cache::write_db_file_wal(
    const fs::path &db_path,
    const std::map<std::string, std::unordered_set<std::string>, std::less<>>
        &db,
    const std::string &milestone, const std::string &wal_op_type) {
  /*
   * 顺序（I-FSYNC-2）：
   *   WAL: <wal_op_type> <path> <milestone>
   *   fsync WAL
   *   备份：rename old → .lpkg_db_bak_before:<milestone>
   *   fsync 备份父目录
   *   写 .tmp
   *   fsync .tmp
   *   rename .tmp → <path>
   *   fsync <path> 父目录
   */

  const bool is_new = !fs::exists(db_path);

  // 1. WAL 行
  wal::log_wal_line(wal_op_type + " " + db_path.string() + " " + milestone);

  // 2. 备份旧文件（仅当文件已存在时）
  std::string bak_path = db_path.string() + ".lpkg_db_bak_before:" + milestone;
  if (!is_new) {
    fs::rename(db_path, bak_path);
    fsync_parent_dir(bak_path);
  }

  // 3. 写 .tmp
  const fs::path tmp = db_path.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f.is_open())
      throw LpkgException(string_format("error.create_tmp_db_failed"));
    for (const auto &[key, values] : db) {
      std::string joined;
      for (const auto &v : values) {
        if (!joined.empty())
          joined += ',';
        joined += v;
      }
      f << key << "\t" << joined << "\n";
    }
  }

  // 4. fsync .tmp
  atomic_write_with_fsync(db_path, tmp);
}

void Cache::write_set_file_wal(
    const fs::path &path, const std::unordered_set<std::string> &data,
    const std::string &milestone, const std::string &wal_op_type) {
  /*
   * 与 write_db_file_wal 相同序列
   */

  const bool is_new = !fs::exists(path);

  // 1. WAL 行
  wal::log_wal_line(wal_op_type + " " + path.string() + " " + milestone);

  // 2. 备份旧文件
  std::string bak_path = path.string() + ".lpkg_db_bak_before:" + milestone;
  if (!is_new) {
    fs::rename(path, bak_path);
    fsync_parent_dir(bak_path);
  }

  // 3. 写 .tmp
  const fs::path tmp = path.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f.is_open())
      throw LpkgException(
          string_format("error.create_file_failed", tmp.string()));
    for (const auto &item : data)
      f << item << "\n";
  }

  // 4. fsync .tmp + rename + fsync parent
  atomic_write_with_fsync(path, tmp);
}

std::map<std::string, std::unordered_set<std::string>, std::less<>>
Cache::read_db_uncached(const fs::path &path) {
  std::map<std::string, std::unordered_set<std::string>, std::less<>> db;
  std::ifstream db_file(path);
  if (!db_file.is_open())
    return db;
  std::string line;
  while (std::getline(db_file, line)) {
    if (line.empty())
      continue;
    size_t tab_pos = line.find('\t');
    if (tab_pos != std::string::npos) {
      std::string key = line.substr(0, tab_pos);
      std::string values = line.substr(tab_pos + 1);
      if (!values.empty() && values.back() == '\r')
        values.pop_back();
      size_t start = 0, end;
      while ((end = values.find(',', start)) != std::string::npos) {
        if (end > start)
          db[key].insert(values.substr(start, end - start));
        start = end + 1;
      }
      if (start < values.size())
        db[key].insert(values.substr(start));
    }
  }
  return db;
}
