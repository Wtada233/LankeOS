#include "trigger.hpp"
#include "utils.hpp"
#include "config.hpp"
#include "localization.hpp"
#include "elf/lib_utils.hpp"
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <iostream>

/**
 * 获取 TriggerManager 单例实例
 */
TriggerManager& TriggerManager::instance() {
    static TriggerManager inst;
    return inst;
}

TriggerManager::TriggerManager() {
    // 默认触发器由 /etc/lpkg/triggers.conf 提供，不再硬编码
}

/**
 * 从配置文件加载自定义触发器规则。
 * 若配置文件不存在，自动写入默认规则（使配置完全文件驱动，无硬编码）。
 * 配置文件每行格式：正则表达式 命令
 * 以 # 开头的行和空行将被跳过
 */
void TriggerManager::load_config() {
    std::lock_guard<std::mutex> lock(mtx);
    if (config_loaded) return;

    auto conf_path = Config::instance().triggers_conf();

    // 首次访问时创建默认配置（只触发一次，用户后续可编辑）
    if (!std::filesystem::exists(conf_path)) {
        std::error_code ec;
        std::filesystem::create_directories(conf_path.parent_path(), ec);
        std::ofstream def(conf_path);
        if (def) {
            def << R"(# Default triggers for LFS/Systemd
# Format: <regex_pattern>	<command>
# Lines starting with # are ignored.

# Shared library update -> regenerate ldconfig links
^/usr/lib/.*\.so.*	ldconfig

# Systemd service unit change -> reload daemon
^/usr/lib/systemd/system/.*\.service$	systemctl daemon-reload

# Icon theme change -> update icon cache
^/usr/share/icons/.*	gtk-update-icon-cache -f -t /usr/share/icons/hicolor

# GSettings schema change -> recompile
^/usr/share/glib-2.0/schemas/.*\.xml$	glib-compile-schemas /usr/share/glib-2.0/schemas
)";
        }
    }

    if (!std::filesystem::exists(conf_path)) return;
        std::ifstream file(Config::instance().triggers_conf());
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;

            std::istringstream iss(line);
            std::string pattern, command;
            if (iss >> pattern) {
                std::getline(iss >> std::ws, command);
                if (!command.empty()) {
                    try {
                        custom_triggers.push_back({std::regex(pattern), command, pattern});
                    } catch (const std::regex_error& e) {
                        log_warning(string_format("warning.invalid_trigger_regex", pattern.c_str()));
                    }
                }
            }
        }
    config_loaded = true;
}

/**
 * 检查指定路径是否匹配任意触发器规则
 * 如果匹配，将对应的命令加入待执行集合
 */
void TriggerManager::check_file(const std::string& path) {
    if (!config_loaded) load_config();
    
    std::lock_guard<std::mutex> lock(mtx);
    for (const auto& trigger : custom_triggers) {
        if (std::regex_match(path, trigger.pattern)) {
            pending_triggers.insert(trigger.command);
        }
    }
}

/**
 * 手动添加一个待执行的触发器命令
 */
void TriggerManager::add(const std::string& cmd) {
    std::lock_guard<std::mutex> lock(mtx);
    pending_triggers.insert(cmd);
}

/**
 * 执行所有待处理的触发器命令
 * 特殊处理 ldconfig 命令：直接调用内部 SONAME 链接生成，而非执行外部程序
 * 在测试模式（testing mode）下跳过所有系统级触发器执行
 */
void TriggerManager::run_all() {
    std::lock_guard<std::mutex> lock(mtx);
    if (pending_triggers.empty()) return;

    log_info(get_string("info.running_triggers"));

    for (const auto& cmd : pending_triggers) {
        log_info(string_format("info.trigger_exec", cmd.c_str()));

        // 内部处理 ldconfig，避免调用外部程序
        if (cmd == "ldconfig") {
            log_info(get_string("info.generating_soname_links"));
            apply_soname_links(Config::instance().root_dir() / "usr/lib");
        } else if (Config::instance().testing_mode()) {
            // 测试模式下跳过外部命令（systemctl daemon-reload 等），避免 polkit 弹窗
            log_info(string_format("info.testing_skip_trigger", cmd.c_str()));
        } else {
            // 使用 run_shell 执行，基于 exec 的更安全方案
            if (int ret = run_shell(cmd); ret != 0) {
                log_warning(string_format("warning.trigger_failed", std::to_string(ret).c_str()));
            }
        }
    }
    pending_triggers.clear();
}
