#include "test_breakpoints.hpp"

#include "../config/config.hpp"

BreakpointManager& BreakpointManager::instance()
{
    static BreakpointManager inst;
    return inst;
}

void BreakpointManager::set(const std::string& name, std::function<void()> action)
{
    if (!enabled()) return;
    // 覆盖同名断点
    for (auto& [n, a] : breakpoints_) {
        if (n == name) {
            a = std::move(action);
            return;
        }
    }
    breakpoints_.emplace_back(name, std::move(action));
}

bool BreakpointManager::hit(const std::string& name)
{
    if (!enabled()) return false;

    for (auto it = breakpoints_.begin(); it != breakpoints_.end(); ++it) {
        if (it->first == name) {
            // **先 erase 再执行 action**：action 的约定用法就是抛异常（注入失败），
            // 若先执行则 erase 不可达 → 断点会重复触发（曾使"期望第二次成功"的用例失效）
            auto action = std::move(it->second);
            breakpoints_.erase(it);
            if (action) action();
            return true;
        }
    }
    return false;
}

void BreakpointManager::clear(const std::string& name)
{
    for (auto it = breakpoints_.begin(); it != breakpoints_.end();) {
        if (it->first == name)
            it = breakpoints_.erase(it);
        else
            ++it;
    }
}

void BreakpointManager::clear_all()
{
    breakpoints_.clear();
}

bool BreakpointManager::enabled() const
{
    return Config::instance().testing_mode();
}
