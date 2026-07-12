#pragma once

#include <string>
#include <string_view>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace fs = std::filesystem;

/**
 * WAL 风格的事务日志。记录每个文件操作的意图和结果。
 * 日志写入 Config::instance().log_dir() / "transaction.log"
 *
 * 格式：每行一个操作
 *   BEGIN <pkg> <version>         事务开始
 *   BACKUP <src> <dst>            备份文件 → .lpkgbak
 *   COPY <src> <dst>              复制文件（.lpkgtmp → rename）
 *   NEW <path>                    新文件（回滚时需删除）
 *   COMMIT <pkg> <version>        事务提交（包注册完成）
 *   ROLLBACK <pkg> <version>      事务回滚
 *   END <pkg> <version>           事务结束
 */
class TransactionLog {
public:
    TransactionLog();
    ~TransactionLog();

    void begin(const std::string& pkg, const std::string& version);
    void backup(const fs::path& src, const fs::path& dst);
    void copy(const fs::path& src, const fs::path& dst);
    void new_file(const fs::path& path);
    void commit(const std::string& pkg, const std::string& version);
    void rollback(const std::string& pkg, const std::string& version);
    void end(const std::string& pkg, const std::string& version);

    /** 检查是否有未完成事务，返回未提交的包名（空串表示无） */
    static std::string check_pending();

    /** 直接写入一行日志（不加时间戳，用于在无 TransactionLog 实例处写日志） */
    static void log_raw(const std::string& line);

private:
    std::ofstream log_;
    std::mutex mtx_;
    fs::path log_path_;

    void write(const std::string& line);
    static std::string timestamp();
};
