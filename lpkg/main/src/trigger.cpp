#include "trigger.hpp"
#include "utils.hpp"
#include "config.hpp"
#include "localization.hpp"
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <iostream>

TriggerManager& TriggerManager::instance() {
    static TriggerManager inst;
    return inst;
}

TriggerManager::TriggerManager() {
    add_default_triggers();
}

void TriggerManager::add_default_triggers() {
    // Built-in defaults for a standard LFS/Systemd environment
    custom_triggers.push_back({std::regex(R"(^/usr/lib/.*\.so.*)"), "ldconfig", R"(^/usr/lib/.*\.so.*)"});
    custom_triggers.push_back({std::regex(R"(^/usr/lib/systemd/system/.*\.service$)"), "systemctl daemon-reload", R"(^/usr/lib/systemd/system/.*\.service$)"});
    custom_triggers.push_back({std::regex(R"(^/usr/share/icons/.*)"), "gtk-update-icon-cache -f -t /usr/share/icons/hicolor", R"(^/usr/share/icons/.*)"});
    custom_triggers.push_back({std::regex(R"(^/usr/share/glib-2.0/schemas/.*\.xml$)"), "glib-compile-schemas /usr/share/glib-2.0/schemas", R"(^/usr/share/glib-2.0/schemas/.*\.xml$)"});
}

void TriggerManager::load_config() {
    std::lock_guard<std::mutex> lock(mtx);
    if (config_loaded) return;

    if (std::filesystem::exists(TRIGGERS_CONF)) {
        std::ifstream file(TRIGGERS_CONF);
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
                        log_warning("Invalid regex in triggers.conf: " + pattern);
                    }
                }
            }
        }
    }
    config_loaded = true;
}

void TriggerManager::check_file(const std::string& path) {
    if (!config_loaded) load_config();
    
    std::lock_guard<std::mutex> lock(mtx);
    for (const auto& trigger : custom_triggers) {
        if (std::regex_match(path, trigger.pattern)) {
            pending_triggers.insert(trigger.command);
        }
    }
}

void TriggerManager::add(const std::string& cmd) {
    std::lock_guard<std::mutex> lock(mtx);
    pending_triggers.insert(cmd);
}

void TriggerManager::run_all() {
    std::lock_guard<std::mutex> lock(mtx);
    if (pending_triggers.empty()) return;
    
    log_info(get_string("info.running_triggers"));
    for (const auto& cmd : pending_triggers) {
        log_info(string_format("info.trigger_exec", cmd.c_str()));
        // Note: Using std::system for now, but in a real shell environment 
        // we might want a safer exec-based approach.
        if (int ret = std::system(cmd.c_str()); ret != 0) {
            log_warning(string_format("warning.trigger_failed", std::to_string(ret).c_str()));
        }
    }
    pending_triggers.clear();
}
