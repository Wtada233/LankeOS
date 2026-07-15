#include "transaction_log.hpp"

#include "../base/exception.hpp"
#include "../base/utils.hpp"
#include "../config/config.hpp"
#include "../i18n/localization.hpp"
#include "wal_op.hpp"

#include <fcntl.h>
#include <filesystem>
#include <unistd.h>

namespace fs = std::filesystem;

namespace wal {

// ============================================================================
// WalWriter
// ============================================================================

WalWriter::WalWriter() {
  std::string path = wal_log_path();

  // 确保父目录存在
  fs::path p(path);
  if (auto parent = p.parent_path(); !parent.empty()) {
    std::error_code ec;
    fs::create_directories(parent, ec);
  }

  fd_ = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
  if (fd_ < 0)
    throw LpkgException(string_format("error.wal_open_failed", path));
}

WalWriter::~WalWriter() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

void WalWriter::log(std::string_view line) {
  if (fd_ < 0)
    throw LpkgException(string_format("error.wal_open_failed", wal_log_path()));

  std::string l = std::string(line) + "\n";
  ssize_t written = ::write(fd_, l.data(), l.size());
  if (written < 0 || static_cast<size_t>(written) != l.size())
    throw LpkgException(string_format("error.wal_write_failed", wal_log_path()));

  if (::fsync(fd_) != 0)
    throw LpkgException(string_format("error.wal_fsync_failed", wal_log_path()));

  ++lines_;
}

void WalWriter::log_no_fsync(std::string_view line) {
  if (fd_ < 0)
    return;

  std::string l = std::string(line) + "\n";
  ::write(fd_, l.data(), l.size());
  ++lines_;
}

void WalWriter::fsync_wal() {
  if (fd_ >= 0)
    ::fsync(fd_);
}

// ============================================================================
// 便捷函数
// ============================================================================

WalWriter begin_batch(size_t total_packages) {
  WalWriter w;
  w.log("BEGIN_PKGS " + std::to_string(total_packages));
  return w;
}

void log_wal_line(std::string_view line) {
  std::string path = wal_log_path();
  int fd =
      ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0)
    throw LpkgException(string_format("error.wal_open_failed", path));

  std::string l = std::string(line) + "\n";
  ssize_t written = ::write(fd, l.data(), l.size());
  if (written < 0 || static_cast<size_t>(written) != l.size()) {
    ::close(fd);
    throw LpkgException(string_format("error.wal_write_failed", path));
  }

  if (::fsync(fd) != 0) {
    ::close(fd);
    throw LpkgException(string_format("error.wal_fsync_failed", path));
  }

  ::close(fd);
}

void commit_batch() { log_wal_line("COMMIT_PKGS"); }

} // namespace wal
