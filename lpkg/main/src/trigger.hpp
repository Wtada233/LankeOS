#pragma once

#include <string>
#include <set>
#include <mutex>

class TriggerManager {
public:
    static TriggerManager& instance();
    
    void add(const std::string& cmd);
    void run_all();

private:
    TriggerManager() = default;
    std::set<std::string> triggers;
    std::mutex mtx;
};
