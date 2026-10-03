#pragma once

#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <vector>

/**
 * 自定义触发器：文件路径正则匹配 -> 执行命令
 */
struct CustomTrigger {
    std::regex pattern;   // 文件路径匹配正则
    std::string command;  // 匹配后执行的命令
};

/**
 * 触发器管理器（单例）
 *
 * 监听文件路径变化并根据配置的触发器规则执行相应命令。
 * 支持默认触发器与用户自定义触发器。
 */
class TriggerManager
{
public:
    /** 获取全局单例实例 */
    static TriggerManager& instance();

    /** 加载触发器配置文件 */
    void load_config();
    /** 检查文件路径是否匹配任何触发器规则 */
    void check_file(const std::string& path);
    /** 添加一个待执行的触发器命令 */
    void add(const std::string& cmd);
    /** 执行所有待处理的触发器命令 */
    void run_all();

    /**
     * 清空单例状态（**仅供测试**）：规则表、待执行集合全部丢弃，`config_loaded` 归零。
     *
     * 存在理由：这是**进程级单例**，而 `config_loaded` 是粘性的 —— 任何一个用例加载过规则
     * 之后，后面的用例都会看到它（路径被映射到触发器、`run_all()` 真的跑命令），于是同一份
     * 代码出现"**单跑绿、全量红**"：单独跑那个用例时没人替它加载规则，全量跑时前一个用例
     * 已经加载过了。测试基类在 TearDown 里调用本函数，让每个用例从同一状态起步。
     */
    void reset_for_test();

    /**
     * **仅供测试**：返回当前待执行命令集合的副本。
     *
     * 存在理由：此前触发器用例**只断言 `EXPECT_NO_THROW`**，而配置文件在测试环境里不存在时
     * 规则表恒空 ⇒ 队列恒空 ⇒ "不抛"是**恒真的废话**（用例名却声称验证了触发器匹配）。
     * 有了它，用例才能断言"匹配/去重**真的发生了**"。
     */
    std::set<std::string> pending_for_test();

private:
    TriggerManager();

    std::set<std::string> pending_triggers;      // 待执行的命令集合（自动去重）
    std::vector<CustomTrigger> custom_triggers;  // 用户自定义触发器列表
    std::mutex mtx;                              // 线程安全锁
    bool config_loaded = false;                  // 配置是否已加载
};
