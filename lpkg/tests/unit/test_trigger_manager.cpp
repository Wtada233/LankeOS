#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../main/src/base/constants.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/trigger/trigger.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

/**
 * TriggerManager 是**进程级单例**：规则表与粘性的 `config_loaded` 由
 * `tests/test_hygiene.hpp` 的 listener 在**每个用例结束时**无条件复位。这里用
 * `IntegrationTestBase` 把 `Config` 的 root 指向沙盒 ⇒ `triggers_conf()` 落在
 * `<sandbox>/etc/lpkg/triggers.conf`，用例可以自己写规则、再断言"匹配真的发生了"。
 *
 * ⚠️ 这些用例**不靠"只断言 `EXPECT_NO_THROW`"**：`check_file` 惰性
 * 加载的配置文件在测试环境里**根本不存在** ⇒ 规则表恒空 ⇒ 待执行队列恒空 ⇒ "不抛异常"是
 * **恒真的废话**（用例名却声称验证了 ldconfig / systemd / 图标 / GSettings 触发器）。
 * 做法：显式写规则 → 断言 `pending_for_test()` 里**确实**出现了对应命令；
 * 去重用例断言队列**大小**。
 */
class TriggerManagerTest : public IntegrationTestBase
{
protected:
    /** 把每条规则（`<正则>\t<命令>`）写进沙盒里的 triggers.conf */
    void write_rules(const std::vector<std::string>& rules)
    {
        const fs::path conf = Config::instance().triggers_conf();
        fs::create_directories(conf.parent_path());
        std::ofstream f(conf);
        for (const auto& r : rules) f << r << "\n";
    }
};

TEST_F(TriggerManagerTest, CheckFileActivatesLdconfigForLibSo)
{
    write_rules({"^/usr/lib/.*\\.so.*\tldconfig"});
    auto& tm = TriggerManager::instance();

    // .so 文件在 /usr/lib 下应触发 ldconfig
    tm.check_file("/usr/lib/libfoo.so.1");
    EXPECT_TRUE(tm.pending_for_test().contains("ldconfig")) << "共享库路径未激活 ldconfig 触发器";
}

TEST_F(TriggerManagerTest, CheckFileActivatesSystemdReload)
{
    write_rules({"^/usr/lib/systemd/system/.*\\.service$\tsystemctl daemon-reload"});
    auto& tm = TriggerManager::instance();

    // .service 文件应触发 systemctl daemon-reload
    tm.check_file("/usr/lib/systemd/system/foo.service");
    EXPECT_TRUE(tm.pending_for_test().contains("systemctl daemon-reload"))
        << "systemd unit 路径未激活 daemon-reload 触发器";
}

TEST_F(TriggerManagerTest, CheckFileIgnoresNonTriggerPaths)
{
    write_rules({"^/usr/lib/.*\\.so.*\tldconfig"});
    auto& tm = TriggerManager::instance();

    // 普通文件不应触发任何触发器，这些路径也不应崩溃
    tm.check_file("/usr/share/doc/foo/readme");
    tm.check_file("/etc/config");
    tm.check_file("");
    tm.check_file("/");
    EXPECT_TRUE(tm.pending_for_test().empty()) << "非触发路径不应向待执行队列加入任何命令";
}

TEST_F(TriggerManagerTest, RunAllHandlesEmptyQueue)
{
    // 空的触发队列应安全执行、且保持为空
    EXPECT_NO_THROW(TriggerManager::instance().run_all());
    EXPECT_TRUE(TriggerManager::instance().pending_for_test().empty());
}

TEST_F(TriggerManagerTest, MultipleAddsDeduplicate)
{
    auto& tm = TriggerManager::instance();

    // 多次添加同一命令应去重
    tm.add("echo test");
    tm.add("echo test");
    tm.add("echo test");
    EXPECT_EQ(tm.pending_for_test().size(), 1u) << "同一命令应被去重为一条";
}

TEST_F(TriggerManagerTest, CheckFileMultipleTimesDeduplicates)
{
    write_rules({"^/usr/lib/.*\\.so.*\tldconfig"});
    auto& tm = TriggerManager::instance();

    tm.check_file("/usr/lib/libtest.so.1");
    tm.check_file("/usr/lib/libtest.so.1");
    tm.check_file("/usr/lib/libtest.so.1");
    EXPECT_EQ(tm.pending_for_test().size(), 1u) << "同一路径多次匹配应只入队一次";
}

TEST_F(TriggerManagerTest, AddDirectly)
{
    auto& tm = TriggerManager::instance();
    tm.add("custom command");
    EXPECT_TRUE(tm.pending_for_test().contains("custom command"));
}

TEST_F(TriggerManagerTest, AddAndRunMultipleCommands)
{
    auto& tm = TriggerManager::instance();
    tm.add("cmd1");
    tm.add("cmd2");
    EXPECT_EQ(tm.pending_for_test().size(), 2u);

    // 测试模式下外部命令被跳过（不真的执行 cmd1/cmd2），只验证队列生命周期
    EXPECT_NO_THROW(tm.run_all());
    EXPECT_TRUE(tm.pending_for_test().empty()) << "run_all 之后待执行队列应被清空";
}

TEST_F(TriggerManagerTest, IconTriggersMatch)
{
    write_rules({"^/usr/share/icons/.*\tgtk-update-icon-cache -f -t /usr/share/icons/hicolor"});
    auto& tm = TriggerManager::instance();

    tm.check_file("/usr/share/icons/hicolor/48x48/apps/foo.png");
    EXPECT_TRUE(
        tm.pending_for_test().contains("gtk-update-icon-cache -f -t /usr/share/icons/hicolor"))
        << "图标主题路径未激活图标缓存触发器";
}

TEST_F(TriggerManagerTest, GsettingsTriggerMatch)
{
    write_rules(
        {"^/usr/share/glib-2.0/schemas/.*\\.xml$\tglib-compile-schemas "
         "/usr/share/glib-2.0/schemas"});
    auto& tm = TriggerManager::instance();

    tm.check_file("/usr/share/glib-2.0/schemas/org.foo.bar.xml");
    EXPECT_TRUE(tm.pending_for_test().contains("glib-compile-schemas /usr/share/glib-2.0/schemas"))
        << "GSettings schema 路径未激活编译触发器";
}
