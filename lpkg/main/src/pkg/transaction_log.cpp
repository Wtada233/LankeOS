#include "transaction_log.hpp"
#include "config/config.hpp"
#include <ctime>
#include <sstream>

TransactionLog::TransactionLog() {
    log_path_ = Config::instance().lock_dir() / "transaction.log";
    std::error_code ec;
    fs::create_directories(log_path_.parent_path(), ec);
    log_.open(log_path_, std::ios::app);
}

TransactionLog::~TransactionLog() {
    if (log_.is_open())
        log_.close();
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
    std::lock_guard<std::mutex> lock(mtx_);
    if (log_.is_open())
        log_ << "[" << timestamp() << "] " << line << std::endl;
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
    std::ofstream f(p, std::ios::app);
    if (f.is_open())
        f << "[" << timestamp() << "] " << line << std::endl;
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
        // 有 BEGIN 没有对应的 COMMIT/END → 未完成事务
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
