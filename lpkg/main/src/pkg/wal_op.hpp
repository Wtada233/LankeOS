#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <utility>

namespace fs = std::filesystem;

/**
 * WAL 操作共享层 — 统一回滚与恢复的逆向执行逻辑。
 *
 * 设计说明：
 *   InstallationTask::rollback() 和 recover_packages() 之前各自维护一套
 *   逆向执行逻辑，操作相同但代码分离。wal_op 层将"执行一组 WAL 操作的反向"
 *   提取为单一函数 reverse_execute()，两个调用者以不同来源构建 WALOp 列表：
 *
 *     rollback()       — 从内存向量（backups_ / new_files_ / new_dirs_）构造
 *     recover_packages() — 从 WAL 日志文件解析
 *
 * WALOp::type 值与日志行操作前缀完全对应（BACKUP / COPY / NEW / …），
 * 确保日志格式与逆向逻辑之间无隐式映射。
 */
namespace wal {

/// WAL 操作记录
struct WALOp {
    std::string type;   ///< 操作类型：BACKUP, COPY, NEW, NEW_DIR, REMOVE_OLD,
                        ///< RM_DIR, RM_BAK_CLN, DB, DBNEW, DBRM
    std::string arg1;   ///< 第一参数（src / path / dbpath）
    std::string arg2;   ///< 第二参数（dst / tag），单参数操作为空
};

/// 从 WAL 日志行（已去掉时间戳前缀）解析一条操作。
/// 格式不合法时返回 std::nullopt。
std::optional<WALOp> parse_op(const std::string& line);

/**
 * 逆向执行一组 WAL 操作。
 *
 * 按传入顺序的逆序遍历，对每条操作执行撤销：
 *   BACKUP / REMOVE_OLD  : rename arg2 → arg1（恢复备份）
 *   COPY / NEW / DBNEW   : remove arg1（删除目标文件）
 *   NEW_DIR              : 清空内部 .lpkg_bak 后若空则 rmdir
 *   DB  / DBRM           : rename .lpkg_db_bak_<arg2> → arg1
 *   RM_DIR               : create_directories(arg1)
 *   RM_BAK_CLN           : 跳过（已在删除文件时处理）
 *
 * @param ops   要逆向执行的操作序列（正向顺序即可，函数内部反向遍历）
 * @param root  系统根目录（用于路径查找，当前未使用，保留接口一致性）
 * @return      (restored_count, cleaned_count)
 */
std::pair<int, int> reverse_execute(const std::vector<WALOp>& ops,
                                    const fs::path& root);

} // namespace wal
