#pragma once
#include <atomic>
#include <stdexcept>

// sigint_graceful 在 main.cpp 中定义，此处声明供测试断点使用
extern std::atomic<bool> sigint_graceful;

/**
 * 测试断点系统 — 仅在 testing_mode 生效。
 *
 * 在 Test Mode 下，每次安装/移除到达指定战略点时检查对应断点，
 * 若断点被设置，则触发 SIGINT 信号（模拟用户在关键时刻按 Ctrl+C）。
 *
 * 使用方式（测试中）：
 *   testing::break_after_backup.store(true);
 *   // 执行 install / remove → 到达该点时自动触发 SIGINT
 *   testing::reset_all();
 *   recover_packages();  // 验证恢复正确
 *
 * 所有断点默认为 false（不生效），即使 testing_mode 关闭也不影响正常执行。
 * 断点触发后自动复位（auto-reset），避免一次触发影响后续操作。
 */
namespace testing {

// ── Install 断点 ────────────────────────────────────────────────────
inline std::atomic<bool> break_before_install{false};
inline std::atomic<bool> break_after_begin_pkgs{false};
inline std::atomic<bool> break_before_backup{false};
inline std::atomic<bool> break_after_backup{false};
inline std::atomic<bool> break_after_extract{false};
inline std::atomic<bool> break_during_file_copy{false};   // 复制每个文件前检查
inline std::atomic<bool> break_after_file_copy{false};
inline std::atomic<bool> break_before_commit{false};
inline std::atomic<bool> break_after_commit{false};
inline std::atomic<bool> break_before_db_write{false};
inline std::atomic<bool> break_after_db_write{false};
inline std::atomic<bool> break_before_commit_pkgs{false};
inline std::atomic<bool> break_after_commit_pkgs{false};

// ── Remove 断点 ─────────────────────────────────────────────────────
inline std::atomic<bool> break_before_remove{false};
inline std::atomic<bool> break_after_rm_begin{false};
inline std::atomic<bool> break_during_rm_backup{false};  // 每个文件备份前检查
inline std::atomic<bool> break_after_rm_backup{false};
inline std::atomic<bool> break_before_rm_dir{false};
inline std::atomic<bool> break_after_rm_dir{false};
inline std::atomic<bool> break_before_rm_db_write{false};
inline std::atomic<bool> break_before_rm_commit{false};
inline std::atomic<bool> break_after_rm_cleanup{false};

// ── Install 内部细粒度断点（用于依赖场景） ─────────────────────────
inline std::atomic<bool> break_before_each_pkg_install{false};
inline std::atomic<bool> break_after_each_pkg_install{false};

// ── 工具函数 ─────────────────────────────────────────────────────────

/// 检查断点并在触发时模拟崩溃（auto-reset: 触发后复位）
/// 抛出的 LpkgException 会被安装/移除流程捕获为"安装中断"。
/// 同时设 sigint_graceful 向下兼容已有 sigint 检查路径。
inline void check_and_break(std::atomic<bool>& bp) {
    if (bp.load()) {
        bp.store(false);
        sigint_graceful.store(true);
        // 直接抛出异常，确保无论后续有无 sigint 检查都能中断流程
        throw std::runtime_error("Testing breakpoint: simulated crash");
    }
}

/// 重置所有断点
inline void reset_all() {
    break_before_install.store(false);
    break_after_begin_pkgs.store(false);
    break_before_backup.store(false);
    break_after_backup.store(false);
    break_after_extract.store(false);
    break_during_file_copy.store(false);
    break_after_file_copy.store(false);
    break_before_commit.store(false);
    break_after_commit.store(false);
    break_before_db_write.store(false);
    break_after_db_write.store(false);
    break_before_commit_pkgs.store(false);
    break_after_commit_pkgs.store(false);

    break_before_remove.store(false);
    break_after_rm_begin.store(false);
    break_during_rm_backup.store(false);
    break_after_rm_backup.store(false);
    break_before_rm_dir.store(false);
    break_after_rm_dir.store(false);
    break_before_rm_db_write.store(false);
    break_before_rm_commit.store(false);
    break_after_rm_cleanup.store(false);

    break_before_each_pkg_install.store(false);
    break_after_each_pkg_install.store(false);
}

} // namespace testing
