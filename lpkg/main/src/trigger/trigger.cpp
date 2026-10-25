#include "trigger.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

#include "config.hpp"
#include "localization.hpp"
#include "ui/term.hpp"
#include "utils.hpp"

namespace
{
/**
 * `load_config` 的"配置不可用只告警一次"门控。
 *
 * 放在**文件作用域**（而不再是 `load_config` 里的函数级 static）是为了让
 * `reset_for_test()` 能复位它 —— 否则这是进程级状态跨用例泄漏：某个用例触发过一次
 * "配置缺失"告警后，后面每个用例都不再打印，一旦有人为 `warning.trigger_conf_missing`
 * 写用例就会"单跑绿、全量红"。这正是 `tests/test_hygiene.hpp` 那条契约（新增全局状态
 * 必须在其中一并复位）要防的事。
 */
bool g_warned_conf_unavailable = false;
}  // namespace

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

    // 默认配置由 Makefile 安装到 /etc/lpkg/triggers.conf。**配置不可用 = 所有触发器
    // 静默失效**（连内部 ldconfig 分支也不会执行，因为它同样由配置里的命令名驱动），
    // 所以必须告警——但只告警一次（每次 check_file 都打印会淹没输出），
    // 且**不能置 config_loaded**：调用方可能在之后才创建该文件（首次安装/测试即是），
    // 置位会让它永远不被加载。
    // 判定用**不抛谓词族**（见 base/path_predicates.hpp）：`fs::exists`
    // 在中间段成环（ELOOP）时会抛， 判定类调用不该有能力打断命令。用 follow
    // 语义：配置文件是符号链接时按**目标**判定 ——
    // 悬空链接视同缺失（否则会静默跳过加载、所有触发器失效）。
    //
    // "配置不可用"有**两种**：文件缺失，与文件存在却打不开。两种的用户可见后果完全一样
    // （所有触发器失效），所以共用同一次告警（复用 `warning.trigger_conf_missing`：文案说的是
    // "未找到配置"，但失效后果与路径名都一致；l10n 键在本改动边界之外，不新增键）。
    auto warn_disabled_once = [&] {
        if (!g_warned_conf_unavailable) {
            log_warning(string_format("warning.trigger_conf_missing", conf_path.string()));
            g_warned_conf_unavailable = true;
        }
    };

    if (!exists_follow(conf_path)) {
        warn_disabled_once();
        return;
    }
    // 用 conf_path（与上面**同一条**路径表达式）打开，并**检查结果**：此前这里另取
    // `Config::instance().triggers_conf()` 且不查 open —— 打开失败会读成空配置却照样走到
    // 末尾置 `config_loaded = true`，于是所有触发器静默失效、且再也不会重试。
    std::ifstream file(conf_path);
    if (!file.is_open()) {
        warn_disabled_once();
        return;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream iss(line);
        std::string pattern, command;
        if (iss >> pattern) {
            std::getline(iss >> std::ws, command);
            if (!command.empty()) {
                try {
                    custom_triggers.push_back({std::regex(pattern), command});
                } catch (const std::regex_error& e) {
                    // `e.what()` 才是能定位的那半句（"unmatched (" 之类）—— 丢掉它就只剩
                    // "正则无效"，用户还得自己去猜写坏了哪个括号。
                    log_warning(
                        string_format("warning.invalid_trigger_regex", pattern.c_str(), e.what()));
                }
            }
        }
    }
    config_loaded = true;
}

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

void TriggerManager::add(const std::string& cmd)
{
    std::lock_guard<std::mutex> lock(mtx);
    pending_triggers.insert(cmd);
}

void TriggerManager::reset_for_test()
{
    std::lock_guard<std::mutex> lock(mtx);
    pending_triggers.clear();
    custom_triggers.clear();
    config_loaded = false;  // 关键：粘性的"已加载"标志必须一起清，否则下一个用例仍看到旧规则
    // "配置不可用只告警一次"的门控同样是进程级状态，一起复位（见文件顶部 g_warned_conf_unavailable
    // 的说明与 tests/test_hygiene.hpp 的契约）。
    g_warned_conf_unavailable = false;
}

std::set<std::string> TriggerManager::pending_for_test()
{
    std::lock_guard<std::mutex> lock(mtx);
    return pending_triggers;
}

/**
 * 执行待办系统触发器。
 * 在测试模式（testing mode）下跳过所有系统级触发器执行
 */
void TriggerManager::run_all()
{
    std::lock_guard<std::mutex> lock(mtx);
    if (pending_triggers.empty()) return;

    ui::section(get_string("ui.section_triggers"));

    for (const auto& cmd : pending_triggers) {
        // 单行状态：`==> Running system trigger: ldconfig ... [OK]`（[OK] 靠终端最右）。
        ui::Line line(string_format("ui.running_trigger", cmd));

        if (Config::instance().testing_mode()) {
            // 测试模式下跳过外部命令（ldconfig / systemctl daemon-reload 等），避免 polkit 弹窗
            line.finish(ui::skipped());
        } else {
            // **在目标 root 内**执行：命令里写的是绝对路径（/usr/share/... 等），
            // 不 chroot 就会打在宿主上、目标 root 反而没更新
            const int ret = run_shell_in_root(cmd);
            line.finish(ui::ok(ret == 0));
            if (ret != 0) {
                log_warning(string_format("warning.trigger_failed", std::to_string(ret).c_str()));
            }
        }
    }
    pending_triggers.clear();
}
