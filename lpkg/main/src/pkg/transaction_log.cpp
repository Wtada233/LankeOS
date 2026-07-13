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

    // 统一事务模型：只检查 BEGIN_PKGS / COMMIT_PKGS
    std::string line, last_begin, last_commit;
    while (std::getline(f, line)) {
        // 用 find 而不是 starts_with，因为行含时间戳前缀
        if (line.find("BEGIN_PKGS ") != std::string::npos)
            last_begin = line;
        if (line.find("COMMIT_PKGS") != std::string::npos)
            last_commit = line;
    }

    if (!last_begin.empty() && last_begin != last_commit) {
        // 从时间戳后的内容提取包数量（仅作显示用）
        auto ts_end = last_begin.find(']');
        if (ts_end != std::string::npos) {
            return last_begin.substr(ts_end + 2);
        }
    }
    return "";
}

// ── 事务压缩 ─────────────────────────────────────────────────────────
//
// 统一状态机：只识别 BEGIN_PKGS 和 COMMIT_PKGS，无 INSTALL/REMOVE/BATCH
// 模式之分。BEGIN_PKGS 进入活跃事务，COMMIT_PKGS 退出。
// 其余所有行（BEGIN、END、COMMIT、ROLLBACK、RM_BEGIN、RM_COMMIT、
// RM_END 等）在活跃事务中被静默忽略（不改变状态）。
//
// 匹配 trim_completed，与 recover_packages 的状态机逻辑保持一致。

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

    // ── 统一状态机扫描 ─────────────────────────────────────────────────
    // 只关心 BEGIN_PKGS 和 COMMIT_PKGS，其余行不影响状态。
    // trim_line_idx: 第一条未完结事务的行号索引
    //   -1 = 无未完结事务（可全量压缩）
    //    0 = 从第一行开始就是未完结事务（不可压缩）
    //   >0 = 从该行开始保留
    int depth = 0;
    int trim_line_idx = -1;

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
            if (depth == 0) {
                trim_line_idx = i;  // 记录未完结事务起点
            }
            depth++;
        } else if (content.starts_with("COMMIT_PKGS")) {
            depth--;
            if (depth == 0) {
                trim_line_idx = -1;  // 事务完结
            }
        }
        // 其余所有行（BEGIN、END、COMMIT、ROLLBACK、RM_BEGIN 等）不影响状态
    }

    // ── 执行压缩 ───────────────────────────────────────────────────────
    if (trim_line_idx < 0) {
        std::ofstream clear_out(log_path, std::ios::trunc);
        return;
    }
    if (trim_line_idx == 0) {
        return;
    }

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
