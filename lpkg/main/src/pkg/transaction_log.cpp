#include "transaction_log.hpp"
#include "config/config.hpp"
#include <ctime>
#include <sstream>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

TransactionLog::TransactionLog() {
    log_path_ = Config::instance().lock_dir() / "transaction.log";
    std::error_code ec;
    fs::create_directories(log_path_.parent_path(), ec);
    // O_APPEND 模式下 write() < PIPE_BUF (4096) 保证原子写入
    int fd = ::open(log_path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd >= 0) {
        log_fd_ = fd;
        opened_ok_ = true;
    }
}

TransactionLog::~TransactionLog() {
    if (log_fd_ >= 0)
        ::close(log_fd_);
}

std::string TransactionLog::timestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

void TransactionLog::write(const std::string& line) {
    if (!opened_ok_) return;
    std::string entry = "[" + timestamp() + "] " + line + "\n";
    // 单次 write() 调用，在 O_APPEND 模式下 < 4096 字节时为原子写入
    ::write(log_fd_, entry.data(), entry.size());
    // 立即 fsync 确保日志落盘（在断电场景下不会丢失已写入的行）
    ::fsync(log_fd_);
}

void TransactionLog::begin(const std::string& pkg, const std::string& version) {
    write("BEGIN " + pkg + " " + version);
}

void TransactionLog::backup(const fs::path& src, const fs::path& dst) {
    write("BACKUP " + src.string() + " → " + dst.string());
}

void TransactionLog::copy(const fs::path& src, const fs::path& dst) {
    write("COPY " + src.string() + " → " + dst.string());
}

void TransactionLog::new_file(const fs::path& path) {
    write("NEW " + path.string());
}

void TransactionLog::commit(const std::string& pkg, const std::string& version) {
    write("COMMIT " + pkg + " " + version);
}

void TransactionLog::rollback(const std::string& pkg, const std::string& version) {
    write("ROLLBACK " + pkg + " " + version);
}

void TransactionLog::end(const std::string& pkg, const std::string& version) {
    write("END " + pkg + " " + version);
}

void TransactionLog::log_raw(const std::string& line) {
    fs::path p = Config::instance().lock_dir() / "transaction.log";
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    std::string entry = "[" + timestamp() + "] " + line + "\n";
    ::write(fd, entry.data(), entry.size());
    ::fsync(fd);
    ::close(fd);
}

void TransactionLog::log_raw_no_fsync(const std::string& line) {
    fs::path p = Config::instance().lock_dir() / "transaction.log";
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    std::string entry = "[" + timestamp() + "] " + line + "\n";
    ::write(fd, entry.data(), entry.size());
    ::close(fd);
}

std::string TransactionLog::check_pending() {
    fs::path log_path = Config::instance().lock_dir() / "transaction.log";
    std::ifstream f(log_path);
    if (!f.is_open()) return "";

    std::string line, last_begin, last_commit;
    while (std::getline(f, line)) {
        if (line.find("BEGIN ") != std::string::npos)
            last_begin = line;
        if (line.find("COMMIT ") != std::string::npos)
            last_commit = line;
        if (line.find("END ") != std::string::npos) {
            last_begin.clear();
            last_commit.clear();
        }
    }

    if (!last_begin.empty() && last_begin != last_commit) {
        size_t pos = last_begin.find("BEGIN ");
        if (pos != std::string::npos) {
            auto rest = last_begin.substr(pos + 6);
            pos = rest.find(' ');
            if (pos != std::string::npos)
                return rest.substr(0, pos);
        }
    }
    return "";
}

// ── 事务压缩 ─────────────────────────────────────────────────────────
//
// 用状态机逐行扫描日志，定位第一条未完结事务的起始行偏移。
// 只有"已完结"且不属于任何未完结批量事务的条目才被安全删除。
//
// 状态定义（复用 recover.cpp 的分类逻辑）：
//   NONE    → 无活跃事务，遇到 BEGIN/RM_BEGIN/BEGIN_PKGS 时转入对应状态
//   INSTALL → 单包安装中，遇到 COMMIT+END 或 ROLLBACK+END 回到 NONE
//   REMOVE  → 移除中，   遇到 RM_COMMIT+RM_END 回到 NONE
//   BATCH   → 批量安装中，遇到 COMMIT_PKGS 回到 NONE（内部子事务不退出 BATCH）
//
// 注意：批量事务内单个包的 COMMIT/END 不清除起始偏移——只有最外层的
//        COMMIT_PKGS 可以。这是最关键的边界。
//
// 实现方式：读取所有行入内存 → 解析状态机 → 找到 trim 点 → 写出后缀。
// 日志文件通常很小（< 1MB），因此全量读取不是问题。

void TransactionLog::trim_completed() {
    const fs::path log_path = Config::instance().lock_dir() / "transaction.log";
    if (!fs::exists(log_path)) return;

    // ── 读入所有行 ─────────────────────────────────────────────────────
    std::ifstream f_in(log_path);
    if (!f_in.is_open()) return;
    std::vector<std::string> lines;
    std::string raw;
    while (std::getline(f_in, raw)) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        lines.push_back(std::move(raw));
    }
    f_in.close();

    if (lines.empty()) return;

    // ── 状态机扫描 ─────────────────────────────────────────────────────
    // 事务类型（与 recover.cpp 保持一致）
    enum class TxnType { NONE, INSTALL, REMOVE, BATCH };
    TxnType txn_type = TxnType::NONE;

    // trim_line_idx: 第一条未完结事务的行号索引
    //   NONE 状态下 = -1（无事可 trim）
    //   非 NONE = 当前未完结事务的起始行
    int trim_line_idx = -1;  // -1 = 无未完结事务

    // 辅助：去掉时间戳前缀 "[YYYY-MM-DD HH:MM:SS] "
    auto strip_ts = [](std::string_view sv) -> std::string_view {
        auto ts_end = sv.find(']');
        if (ts_end == std::string_view::npos) return sv;
        if (ts_end + 2 > sv.size()) return "";
        return sv.substr(ts_end + 2);
    };

    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        std::string_view content = strip_ts(lines[i]);
        if (content.empty()) continue;

        if (content.starts_with("BEGIN_PKGS ")) {
            if (txn_type != TxnType::BATCH) {
                if (txn_type == TxnType::NONE) {
                    trim_line_idx = i;  // 记录批量事务起点
                }
                txn_type = TxnType::BATCH;
            }
        } else if (content.starts_with("COMMIT_PKGS")) {
            if (txn_type == TxnType::BATCH) {
                txn_type = TxnType::NONE;
                trim_line_idx = -1;     // 批次完成，清除偏移
            }
        } else if (content.starts_with("RM_BEGIN ")) {
            if (txn_type == TxnType::NONE) {
                trim_line_idx = i;
                txn_type = TxnType::REMOVE;
            }
        } else if (content.starts_with("RM_COMMIT ") || content.starts_with("RM_END ")) {
            if (txn_type == TxnType::REMOVE) {
                txn_type = TxnType::NONE;
                trim_line_idx = -1;
            }
            // BATCH 状态下不清除
        } else if (content.starts_with("BEGIN ")) {
            if (txn_type == TxnType::NONE) {
                trim_line_idx = i;
                txn_type = TxnType::INSTALL;
            }
        } else if (content.starts_with("COMMIT ")) {
            if (txn_type == TxnType::INSTALL) {
                // 不立即清除：INSTALL 还要看后续 END 才完结
            }
            // BATCH 下不清除
        } else if (content.starts_with("ROLLBACK ")) {
            // ROLLBACK（总是来自恢复流程）终结任意类型的事务
            // 恢复流程在 rec 任何事务后写 ROLLBACK+END 标记事务已完结
            if (txn_type != TxnType::NONE) {
                txn_type = TxnType::NONE;
                trim_line_idx = -1;
            }
        } else if (content.starts_with("END ")) {
            if (txn_type == TxnType::INSTALL) {
                txn_type = TxnType::NONE;
                trim_line_idx = -1;
            }
            // 若已经是 NONE（因 ROLLBACK 先行清除），END 不再重复操作
            // BATCH 下不清除
        }
    }

    // ── 执行压缩 ───────────────────────────────────────────────────────
    // trim_line_idx:
    //   -1 → 无未完结事务，全量清空
    //    0 → 第一条就是未完结事务，什么都不要删
    //   >0 → 从第 trim_line_idx 行开始保留

    if (trim_line_idx < 0) {
        // 全部已完结 → 清空
        std::ofstream clear_out(log_path, std::ios::trunc);
        return;
    }
    if (trim_line_idx == 0) {
        // 未完结事务从开头开始 → 不动
        return;
    }

    // 写出 trim_line_idx 及之后的行到 .tmp，rename 覆盖
    const fs::path tmp_path = log_path.string() + ".trim_tmp";
    {
        std::ofstream f_out(tmp_path);
        if (!f_out.is_open()) return;
        for (int i = trim_line_idx; i < static_cast<int>(lines.size()); ++i) {
            f_out << lines[i] << "\n";
        }
        f_out.flush();
        // fsync 保证 .tmp 内容落盘
        int fd = ::open(tmp_path.c_str(), O_WRONLY);
        if (fd >= 0) { ::fsync(fd); ::close(fd); }
    }
    std::error_code ec;
    fs::rename(tmp_path, log_path, ec);
}
