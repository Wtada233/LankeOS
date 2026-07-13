#pragma once

#include <string>
#include <string_view>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

/**
 * WAL 风格的事务日志。记录每个文件操作的意图和结果。
 * 日志写入 Config::instance().lock_dir() / "transaction.log"
 *
 * 每行使用单次 write() 写入，在 O_APPEND 模式下 < 4096 字节即为原子写入。
 * 断电不会损坏已有行——最多丢失最后一行（部分写入行不合法，在恢复时被忽略）。
 *
 * 格式：每行一个操作
 *   BEGIN <pkg> <version>         事务开始
 *   BACKUP <src> → <dst>          备份文件 → .lpkgbak
 *   COPY <src> → <dst>            复制文件（.lpkgtmp → rename）
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
    int log_fd_ = -1;
    bool opened_ok_ = false;
    fs::path log_path_;

    void write(const std::string& line);
    static std::string timestamp();
};
