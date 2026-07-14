#pragma once

#include <string>
#include <string_view>
#include <unistd.h>

namespace wal {

/**
 * WalWriter — WAL 文件原子写入器
 *
 * 所有写操作使用 O_APPEND + write + fsync 确保每行立即持久化。
 * 单行 < 4096 字节在 O_APPEND + fsync 下保证已持久化。
 *
 * WalWriter 是低级工具 —— 每次 log() 调用都会 write + fsync。
 * 调用者负责以正确的顺序构建 WAL 条目。
 */
class WalWriter {
public:
  /// 打开 WAL 文件进行追加写入
  WalWriter();
  ~WalWriter();

  WalWriter(const WalWriter &) = delete;
  WalWriter &operator=(const WalWriter &) = delete;
  WalWriter(WalWriter &&other) noexcept
      : fd_(other.fd_), lines_(other.lines_) {
    other.fd_ = -1;
    other.lines_ = 0;
  }
  WalWriter &operator=(WalWriter &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0)
        ::close(fd_);
      fd_ = other.fd_;
      lines_ = other.lines_;
      other.fd_ = -1;
      other.lines_ = 0;
    }
    return *this;
  }

  /// 追加一行到 WAL 并 fsync
  /// 失败时抛 LpkgException
  void log(std::string_view line);

  /// 追加一行到 WAL 但跳过 fsync（仅用于非关键路径）
  /// 失败时静默返回
  void log_no_fsync(std::string_view line);

  /// 对 WAL 文件执行 fsync
  void fsync_wal();

  /// 获取当前写入的行数
  size_t lines_written() const { return lines_; }

private:
  int fd_ = -1;
  size_t lines_ = 0;
};

// ── 便捷函数 ────────────────────────────────────────────────────────────

/// 打开 WAL 文件，写入 BEGIN_PKGS <N> + fsync，返回 WalWriter 实例
/// 调用者持有该实例用于后续 WAL 写入（使用 move 语义）
WalWriter begin_batch(size_t total_packages);

/// 对当前批次的单条 WAL 行进行 log + fsync
void log_wal_line(std::string_view line);

/// 写入 COMMIT_PKGS + fsync
void commit_batch();

} // namespace wal
