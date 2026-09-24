#pragma once

#include <unistd.h>

#include <string>
#include <string_view>

namespace wal
{

/**
 * WalWriter — WAL 文件原子写入器
 *
 * 所有写操作使用 O_APPEND + write + fsync 确保每行立即持久化。
 * 单行 < 4096 字节在 O_APPEND + fsync 下保证已持久化。
 *
 * WalWriter 是低级工具 —— 每次 `log()` 调用都会 write + fsync，且这个 fsync
 * **恒生效**（I-FSYNC-1；**不受** `--fsync` / `durable_fsync_enabled()` 影响：
 * 行是"某个操作已开始、可能未完成"的唯一证据，丢了行就没有回滚依据，而物理操作
 * 可能已经做完 = 不可恢复组合）。要显式的**非持久**快路径，用 `log_no_fsync()`。
 * 调用者负责以正确的顺序构建 WAL 条目。
 *
 * （生产路径的 WAL 行**全部**走 `wal::log_wal_line()`：`batch_transaction.hpp` 与
 * `InstallationTask` 都是这么写的，本类今天只剩测试在用。曾把 `log()` 的 fsync 挂到
 * `durable_fsync_enabled()` 上——那是"实现弱于契约"：契约（本注释与 ARCH §2.1 的行
 * 清单）写的是"写后 fsync"，而默认模式下本类写出的行会静默变成非持久，将来谁用
 * `begin_batch()` + `log()` 写生产 WAL 行就踩中。既然 `log_no_fsync()` 已经存在，
 * 那条门控就是纯粹的多余且危险的形态，故去掉。）
 */
class WalWriter
{
public:
    /// 打开 WAL 文件进行追加写入
    WalWriter();
    ~WalWriter();

    WalWriter(const WalWriter&) = delete;
    WalWriter& operator=(const WalWriter&) = delete;
    WalWriter(WalWriter&& other) noexcept : fd_(other.fd_), lines_(other.lines_)
    {
        other.fd_ = -1;
        other.lines_ = 0;
    }
    WalWriter& operator=(WalWriter&& other) noexcept
    {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = other.fd_;
            lines_ = other.lines_;
            other.fd_ = -1;
            other.lines_ = 0;
        }
        return *this;
    }

    /// 追加一行到 WAL 并 **fsync（恒生效，不受 --fsync 开关影响）**
    /// 失败时抛 LpkgException
    void log(std::string_view line);

    /// 追加一行到 WAL 但**跳过 fsync** —— 唯一的非持久写入路径，只给"明确不需要
    /// 持久性的非关键路径"用
    /// 失败时静默返回
    void log_no_fsync(std::string_view line);

    /// 对 WAL 文件执行 fsync（`log()` 已逐行 fsync；此接口留给"批量写 + 一次 fsync"
    /// 的调用方，以及需要额外一次 bar 的场合）
    void fsync_wal();

    /// 获取当前写入的行数
    size_t lines_written() const
    {
        return lines_;
    }

private:
    int fd_ = -1;
    size_t lines_ = 0;
};

// ── 便捷函数 ────────────────────────────────────────────────────────────

/// 打开 WAL 文件，写入 BEGIN_PKGS + fsync，返回 WalWriter 实例
/// 调用者持有该实例用于后续 WAL 写入（使用 move 语义）
WalWriter begin_batch();

/// 对当前批次的单条 WAL 行进行 log + fsync
void log_wal_line(std::string_view line);

/// 写入 COMMIT_PKGS + fsync
void commit_batch();

}  // namespace wal
