#include "trigger.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

#include "config.hpp"
#include "elf/lib_utils.hpp"
#include "localization.hpp"
#include "utils.hpp"

/**
 * 获取 TriggerManager 单例实例
 */
TriggerManager& TriggerManager::instance()
{
    static TriggerManager inst;
    return inst;
}

TriggerManager::TriggerManager()
{
    // 默认触发器由 /etc/lpkg/triggers.conf 提供，不再硬编码
}

/**
 * 从配置文件加载自定义触发器规则。
 * 若配置文件不存在，自动写入默认规则（使配置完全文件驱动，无硬编码）。
 * 配置文件每行格式：正则表达式 命令
 * 以 # 开头的行和空行将被跳过
 */
void TriggerManager::load_config()
{
    std::lock_guard<std::mutex> lock(mtx);
    if (config_loaded) return;

    auto conf_path = Config::instance().triggers_conf();

    // 默认配置由 Makefile 安装到 /etc/lpkg/triggers.conf。**文件缺失 = 所有触发器
    // 静默失效**（连内部 ldconfig 分支也不会执行，因为它同样由配置里的命令名驱动），
    // 所以必须告警——但只告警一次（每次 check_file 都打印会淹没输出），
    // 且**不能置 config_loaded**：调用方可能在之后才创建该文件（首次安装/测试即是），
    // 置位会让它永远不被加载。
    if (!std::filesystem::exists(conf_path)) {
        static bool warned_missing_conf = false;
        if (!warned_missing_conf) {
            log_warning(string_format("warning.trigger_conf_missing", conf_path.string()));
            warned_missing_conf = true;
        }
        return;
    }
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
void TriggerManager::check_file(const std::string& path)
{
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
void TriggerManager::add(const std::string& cmd)
{
    std::lock_guard<std::mutex> lock(mtx);
    pending_triggers.insert(cmd);
}

/**
 * 执行所有待处理的触发器命令
 * 特殊处理 ldconfig 命令：直接调用内部 SONAME 链接生成，而非执行外部程序
 * 在测试模式（testing mode）下跳过所有系统级触发器执行
 */
void TriggerManager::run_all()
{
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
            // **在目标 root 内**执行：命令里写的是绝对路径（/usr/share/... 等），
            // 不 chroot 就会打在宿主上、目标 root 反而没更新（TODO F3）
            if (int ret = run_shell_in_root(cmd); ret != 0) {
                log_warning(string_format("warning.trigger_failed", std::to_string(ret).c_str()));
            }
        }
    }
    pending_triggers.clear();
}
