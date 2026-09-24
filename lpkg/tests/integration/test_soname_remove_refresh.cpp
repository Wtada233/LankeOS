/**
 * test_soname_remove_refresh.cpp — 升级/删除后 SONAME 链接必须跟着变（缺陷 ④ 的集成侧）
 *
 * 两条链路缺一不可：
 *   升级：同 SONAME、不同文件名（libfoo.so.1.2.3 → libfoo.so.1.4.5）时，旧链接指向的
 *         文件已被删 → 链接悬空。修好的 apply_soname_links 会把它重指到新文件
 *         （旧实现在“链接已存在”时直接跳过，于是永久悬空）。
 *   删除：删掉库文件后链接留着悬空 —— 删除路径此前**一次都不跑触发器**
 *         （TriggerManager::run_all 只在 install/upgrade 里 flush），
 *         而 apply_soname_links 现在会把无人提供 SONAME 的悬空链接清掉。
 *
 * 真实 ELF（gcc 不可用则跳过）。
 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/trigger/trigger.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

class SonameRemoveRefreshTest : public IntegrationTestBase
{
protected:
    void SetUp() override
    {
        IntegrationTestBase::SetUp();
        Config::instance().set_no_hooks_mode(true);

        // 触发器规则是文件驱动的（读沙盒 root 的 etc/lpkg/triggers.conf）；本进程内
        // TriggerManager 只加载一次配置，用一个**不匹配任何规则**的路径把它钉在此处的
        // 沙盒配置上（check_file 顺带 load_config，且不会登记任何待执行命令）。
        {
            const fs::path conf = Config::instance().triggers_conf();
            fs::create_directories(conf.parent_path());
            std::ofstream(conf) << "^/usr/lib/.*\\.so.*\tldconfig\n";
        }
        TriggerManager::instance().check_file("/usr/bin/__lpkg_trigger_prime__");
    }

    /**
     * 打一个库包：content/usr/lib/<name>.so.<file_ver>，DT_SONAME = <soname>。
     * 包版本与文件名版本可以不同（用来造“同 SONAME、不同文件名”的升级）。
     */
    bool create_lib_pkg(const std::string& name, const std::string& pkg_ver,
                        const std::string& file_ver, const std::string& soname, int tag)
    {
        const fs::path work = suite_work_dir / ("_lib_" + name + "_" + pkg_ver);
        fs::remove_all(work);
        fs::create_directories(work / "content/usr/lib");
        const fs::path src = work / "lib.c";
        std::ofstream(src) << "int " << name << "_p" << tag << "(void){return " << tag << ";}\n";
        const fs::path out = work / "content/usr/lib" / (name + ".so." + file_ver);
        const std::string cmd = "gcc -shared -fPIC -Wl,-soname," + soname + " -o " + out.string() +
                                " " + src.string() + " 2>/dev/null";
        if (std::system(cmd.c_str()) != 0 || !fs::exists(out)) return false;
        pack_package((pkg_dir / (name + "-" + pkg_ver + ".lpkg")).string(), work.string(), name,
                     pkg_ver, {}, {}, "", {});
        return true;
    }
};

// 升级到“同 SONAME、不同文件名”→ 链接必须重指到新文件（否则指向已删的旧文件 = 悬空）
TEST_F(SonameRemoveRefreshTest, UpgradeRepointsSonameLinkToNewFileName)
{
    if (!create_lib_pkg("librep", "1.2.3", "1.2.3", "librep.so.1", /*tag=*/1))
        GTEST_SKIP() << "gcc 不可用";
    ASSERT_NO_THROW(install_packages({(pkg_dir / "librep-1.2.3.lpkg").string()}));

    const fs::path link = test_root / "usr/lib/librep.so.1";
    ASSERT_TRUE(fs::is_symlink(link)) << "安装后没有生成 SONAME 链接";
    ASSERT_EQ(fs::read_symlink(link).string(), "librep.so.1.2.3");

    ASSERT_TRUE(create_lib_pkg("librep", "1.4.5", "1.4.5", "librep.so.1", /*tag=*/2));
    ASSERT_NO_THROW(install_packages({(pkg_dir / "librep-1.4.5.lpkg").string()}));

    ASSERT_FALSE(fs::exists(test_root / "usr/lib/librep.so.1.2.3")) << "旧文件名应当已被删";
    EXPECT_TRUE(fs::exists(link))
        << "链接悬空（指向升级时被删掉的旧文件名）：依赖 librep.so.1 的二进制将报 "
           "cannot open shared object file";
    EXPECT_EQ(fs::read_symlink(link).string(), "librep.so.1.4.5");
}

// 删除库包 → SONAME 链接不得留在 /usr/lib 里悬空（删除路径此前从不 flush 触发器）
TEST_F(SonameRemoveRefreshTest, RemovePrunesDanglingSonameLink)
{
    if (!create_lib_pkg("libson", "1.0", "1.0", "libson.so.1", /*tag=*/1))
        GTEST_SKIP() << "gcc 不可用";
    ASSERT_NO_THROW(install_packages({(pkg_dir / "libson-1.0.lpkg").string()}));

    const fs::path link = test_root / "usr/lib/libson.so.1";
    ASSERT_TRUE(fs::is_symlink(link));

    ASSERT_NO_THROW(remove_packages({"libson"}, /*force=*/false));

    EXPECT_FALSE(Cache::instance().is_installed("libson"));
    EXPECT_FALSE(fs::exists(test_root / "usr/lib/libson.so.1.0"));
    EXPECT_FALSE(fs::is_symlink(link))
        << "库文件已删除，SONAME 链接仍留在 /usr/lib —— 删除路径没有跑触发器"
           "（或 apply_soname_links 不会清理悬空链接）";
}
