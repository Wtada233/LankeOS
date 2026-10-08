#pragma once

#include <functional>
#include <string>
#include <vector>

/**
 * BreakpointManager — 测试断点注入框架。
 *
 * 仅在 Config::testing_mode() == true 时生效。
 * 允许测试代码在关键路径注入故障，验证主动回滚（batch_rollback）
 * 而非仅被动恢复（recover_packages）。
 *
 * 使用方式：
 *   BreakpointManager::instance().set("after_commit_" + pkg, []{
 *       throw LpkgException("injected disk full");
 *   });
 *   install_packages({pkg}); // 会在该包 COMMIT 之后触发断点 → 回滚
 *
 * 断点名称约定（**以生产端 `grep -rn 'hit(' main/src` 的实际调用点为准**）：
 *
 *   安装 / 升级（`installation_task{,_letgo,_copy}.cpp` 与 `op_sink.cpp`）：
 *   install_after_begin_<pkg>     BEGIN 行已落、安装动作未做
 *   after_commit_<pkg>            包 COMMIT 行已落、END 行未写
 *   backup_after_wal_<pkg>        BACKUP WAL 行已落、rename 未做
 *   conf_replace_after_wal_<pkg>  `/etc` 配置条目的 BACKUP/save_config WAL 行已落、rename 未做
 *                                （让开趟 stash 处与写入趟共用同一名字）
 *   copy_after_wal_<pkg>          COPY WAL 行已落、rename 未做
 *   symlink_after_wal_<pkg>       NEW(符号链接) 行已落、create_symlink 未做
 *   newdir_after_wal_<pkg>        NEW_DIR 行已落、建目录未做
 *   unstash_after_wal_<pkg>       UNSTASH 行已落、rename 未做
 *   remove_old_after_wal_<pkg>    REMOVE_OLD 行已落、rename 未做
 *   dirmeta_after_wal_<pkg>       DIR_META 行已落、lchown/chmod 未做
 *   xattrset_after_wal_<pkg>      XATTR_SET/XATTR_NEW 行已落、lsetxattr 未做
 *   xattrrm_after_wal_<pkg>       XATTR_SET 行已落、lremovexattr 未做
 *   lpkgnew_after_wal_<pkg>       `.lpkgnew` 的 COPY 行已落、rename 未做
 *   lpkgnew_bak_after_wal_<pkg>   `.lpkgnew` 的 BACKUP 行已落、rename 未做
 *
 *   卸载（`package_manager.cpp`）：
 *   rm_before_file_removal_<pkg>  移除的 BACKUP 阶段完成后、文件删除前
 *   rm_save_conf_after_wal_<pkg>  `/etc` save_config 的 WAL 行已落、rename 未做
 *   rm_backup_after_wal_<pkg>     移除侧 BACKUP 的 WAL 行已落、rename 未做
 *   remove_after_package_<pkg>    本包已删完、下一包未开始
 *   remove_batch_before_commit    全部包已删完、批次尚未提交
 *   cleanup_after_wal             CLEANUP 日志已写、物理删除 stash 前（最后可回滚点）
 *
 *   DB（`package_manager.cpp`）：
 *   batch_db_before_commit        DB 行已落、COMMIT_PKGS 未写
 *
 *   hook（`install_common.cpp`）：
 *   hook_run_<hook 文件名>        包的 hook（postinst.sh / prerm.sh）**执行点**：hooks 已启用、
 *                                脚本确实存在、只剩 exec（沙盒里没有 bash，这是唯一能确定性
 *                                观测"钩子跑了没有"的位置，见
 *                                tests/integration/test_hook_transaction.cpp）
 *
 * 语义：`hit()` 命中后**先 erase 再执行 action**（见 test_breakpoints.cpp），所以每个断点是
 * **一次性**的 —— 同一名字在一次批次里只会触发一次（`set()` 覆盖同名则替换动作）。
 */
class BreakpointManager
{
public:
    static BreakpointManager& instance();

    void set(const std::string& name, std::function<void()> action);

    /// 返回 true = 命中了同名断点并执行了 action。
    bool hit(const std::string& name);

    void clear(const std::string& name);

    void clear_all();

    bool enabled() const;

private:
    BreakpointManager() = default;
    std::vector<std::pair<std::string, std::function<void()>>> breakpoints_;
};
