#include "test_breakpoints.hpp"
#include "../config/config.hpp"

BreakpointManager &BreakpointManager::instance() {
  static BreakpointManager inst;
  return inst;
}

void BreakpointManager::set(const std::string &name,
                             std::function<void()> action) {
  if (!enabled())
    return;
  // 覆盖同名断点
  for (auto &[n, a] : breakpoints_) {
    if (n == name) {
      a = std::move(action);
      return;
    }
  }
  breakpoints_.emplace_back(name, std::move(action));
}

bool BreakpointManager::hit(const std::string &name) {
  if (!enabled())
    return false;

  for (auto it = breakpoints_.begin(); it != breakpoints_.end(); ++it) {
    if (it->first == name) {
      if (it->second) {
        it->second(); // 执行 action（通常抛异常）
      }
      breakpoints_.erase(it); // 单次触发
      return true;
    }
  }
  return false;
}

void BreakpointManager::clear(const std::string &name) {
  for (auto it = breakpoints_.begin(); it != breakpoints_.end();) {
    if (it->first == name)
      it = breakpoints_.erase(it);
    else
      ++it;
  }
}

void BreakpointManager::clear_all() { breakpoints_.clear(); }

bool BreakpointManager::enabled() const {
  return Config::instance().testing_mode();
}
