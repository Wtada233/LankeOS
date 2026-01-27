#include "trigger.hpp"
#include "utils.hpp"
#include "localization.hpp"
#include <cstdlib>

#include <ranges>

TriggerManager& TriggerManager::instance() {
    static TriggerManager inst;
    return inst;
}

void TriggerManager::add(const std::string& cmd) {
    std::lock_guard<std::mutex> lock(mtx);
    triggers.insert(cmd);
}

void TriggerManager::run_all() {
    std::lock_guard<std::mutex> lock(mtx);
    if (triggers.empty()) return;
    
    log_info(get_string("info.running_triggers"));
    for (const auto& cmd : triggers) {
        log_info(string_format("info.trigger_exec", cmd.c_str()));
        if (int ret = std::system(cmd.c_str()); ret != 0) {
            log_warning(string_format("warning.trigger_failed", std::to_string(ret).c_str()));
        }
    }
    triggers.clear();
}
