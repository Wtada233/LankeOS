#pragma once

#include <string>
#include <set>
#include <vector>
#include <mutex>
#include <regex>

struct CustomTrigger {
    std::regex pattern;
    std::string command;
    std::string pattern_str;
};

class TriggerManager {
public:
    static TriggerManager& instance();
    
    void load_config();
    void check_file(const std::string& path);
    void add(const std::string& cmd);
    void run_all();

private:
    TriggerManager();
    void add_default_triggers();

    std::set<std::string> pending_triggers;
    std::vector<CustomTrigger> custom_triggers;
    std::mutex mtx;
    bool config_loaded = false;
};
