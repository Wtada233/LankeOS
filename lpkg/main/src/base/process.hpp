#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// ============ 日志输出 ============

void log_info(std::string_view msg);
void log_warning(std::string_view msg);
void log_error(std::string_view msg);

// ============ 进程执行 ============

/// 参数列表形式。
int run_command(const std::vector<std::string>& args, const std::filesystem::path& work_dir = "");
/// Shell 字符串形式。
int run_shell(const std::string& cmd, const std::filesystem::path& work_dir = "");

/**
 * 在**目标 root 内**执行 shell 命令。
 *
 * `root_dir == "/"` 时等价于 run_shell；否则 fork + unshare(CLONE_NEWNS) +
 * mount --make-private + chroot + chdir("/") 后再执行——与 `run_hook` 的做法一致。
 * 目标 root 内没有 /bin/bash（或 unshare/chroot 失败）时返回 -1，由调用方告警。
 *
 * 为什么必须有它：外部触发器命令（`systemctl daemon-reload` /
 * `glib-compile-schemas /usr/share/glib-2.0/schemas` / `gtk-update-icon-cache`）里写的是
 * 绝对路径。`lpkg --root /mnt/base install ...` 时若不 chroot，它们会**打在宿主上**，
 * 目标 root 反而没更新。
 */
int run_shell_in_root(const std::string& cmd);

// ============ 用户交互 ============

/**
 * 从 stdin 读**一行**（不含行尾），期间轮询 SIGINT 标志 —— **所有交互式输入都走这里**。
 *
 * 直接 `std::cin >> x` / `std::getline(std::cin, …)` 会让该处输入期间**不可中断**：glibc 的
 * handler 带 `SA_RESTART`，被打断的 `read` 自动重启 ⇒ Ctrl+C 只置标志、进程仍旧卡着
 * （用户看到"Ctrl+C 无效，只能 kill -9"）。
 *
 * @return true = 读到一行；false = Ctrl+C 打断 **或** EOF（调用方通常都当作"放弃当前操作"）
 */
bool read_line_interruptible(std::string& out);

/// 非交互模式自动返回 true。
bool user_confirms(const std::string& prompt);

// ============ 系统检查 ============

/// 非 root 则退出。
void check_root();
