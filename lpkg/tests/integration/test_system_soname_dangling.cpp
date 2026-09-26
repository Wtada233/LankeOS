/**
 * test_system_soname_dangling.cpp — 悬空 SONAME 链接不是"系统已提供"
 *
 * `--use-system-soname` 有**两处**判据，它们必须一致：
 *   · solver 侧 `collect_system_sonames()`（install_common.cpp）→ 决定"要不要求解出真实
 *     提供者包"；
 *   · 安装期前向校验 `Config::has_system_soname()`（config.cpp）→ 决定"装完之后这个 SONAME
 *     算不算满足"。
 * 前者曾经只要"名字像 `lib*.so*` 且 `is_symlink()` 为真"就收，**不看链接目标在不在**；
 * 后者用 `fs::exists(cand)`（**跟随**链接）→ 悬空链接判为"不满足"。于是同一个 SONAME 得到
 * 相反结论：solver 不拉真实提供者（它以为系统已经有了），前向校验却认为没满足。只开
 * `--use-system-soname` 时整批报错（吵，但至少不装错），一旦同时开了 `--missing-so-no-error`
 * （farm bootstrap 的固定组合，见 farm/ARCH.md）就只剩一条 warning —— **装出一个缺库的系统**。
 *
 * 悬空链接是现实中真会出现的形态（升级/清理删掉真实 .so、只留下 SONAME 链接），所以本文件
 * 的每个"悬空"用例都配一个"链接指向真实文件"的正对照：那才是 SONAME 链接的常见形态，
 * 修悬空不能把它一起毙掉（否则 `--use-system-soname` 整个特性失效，而本文件的"悬空"断言
 * 会因为 collect_system_sonames 返回空而**假装变绿**）。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/constants.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/crypto/hash.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"

namespace fs = std::filesystem;

class SystemSonameDanglingTest : public ::testing::Test
{
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;
    fs::path mirror_dir;

    void SetUp() override
    {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        init_localization();

        suite_work_dir = fs::absolute("tmp_system_soname_dangling_test");
        fs::remove_all(suite_work_dir);
        test_root = suite_work_dir / "root";
        pkg_dir = suite_work_dir / "pkgs";
        mirror_dir = suite_work_dir / "mirror" / "x86_64";
        fs::create_directories(test_root);
        fs::create_directories(pkg_dir);
        fs::create_directories(mirror_dir);

        Config::instance().set_root_path(test_root.string());
        Config::instance().set_architecture("x86_64");
        Config::instance().init_filesystem();

        std::ofstream(test_root / "etc/lpkg/mirror.conf")
            << "file://" << suite_work_dir.string() << "/mirror/" << std::endl;
    }

    void TearDown() override
    {
        // 全局开关必须复位：泄漏出去会让别的套件的求解/校验行为悄悄变样
        Config::instance().set_use_system_soname_mode(false);
        Config::instance().set_missing_so_no_error_mode(false);
        Config::instance().set_root_path("/");
        Config::instance().set_testing_mode(false);
        fs::remove_all(suite_work_dir);
    }

    std::string create_pkg(const std::string& name, const std::string& ver,
                           const std::vector<std::string>& deps = {},
                           const std::vector<std::string>& provides = {},
                           const std::vector<std::string>& needed_so = {})
    {
        fs::path work_dir = suite_work_dir / ("pkg_work_" + name);
        fs::create_directories(work_dir / "content" / "usr" / "bin");
        std::ofstream(work_dir / "content" / "usr" / "bin" / name).close();

        const std::string pkg_path = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(pkg_path, work_dir.string(), name, ver, deps, provides, "", needed_so);

        fs::path mirror_pkg_dir = mirror_dir / name;
        fs::create_directories(mirror_pkg_dir);
        fs::copy_file(pkg_path, mirror_pkg_dir / (ver + ".lpkg"),
                      fs::copy_options::overwrite_existing);
        fs::remove_all(work_dir);
        return pkg_path;
    }

    void update_index(const std::vector<std::tuple<std::string, std::string, std::string,
                                                   std::string, std::string>>& entries)
    {
        std::ofstream index(mirror_dir / "index.txt");
        for (const auto& [name, ver, deps, provides, needed_so] : entries) {
            std::string hash = "unknown";
            const fs::path pkg_path = pkg_dir / (name + "-" + ver + ".lpkg");
            if (fs::exists(pkg_path)) hash = calculate_sha256(pkg_path);
            index << name << "|" << ver << ":" << hash << ":" << deps << ":" << provides << ":"
                  << needed_so << "\n";
        }
    }

    /** 在沙盒 /usr/lib 下放一个**悬空**的 SONAME 链接（目标不存在） */
    void make_dangling_soname_link(const std::string& soname)
    {
        const fs::path lib_dir = test_root / fs::path(constants::USR_LIB);
        fs::create_directories(lib_dir);
        fs::create_symlink(soname + ".9.9.9", lib_dir / soname);  // 目标刻意不存在
    }

    /** 在沙盒 /usr/lib 下放一个"SONAME 链接 → 真实文件"的常规形态 */
    void make_real_soname_link(const std::string& soname)
    {
        const fs::path lib_dir = test_root / fs::path(constants::USR_LIB);
        fs::create_directories(lib_dir);
        std::ofstream(lib_dir / (soname + ".9.9.9")) << "not-a-real-elf-but-exists\n";
        fs::create_symlink(soname + ".9.9.9", lib_dir / soname);
    }

    /** 在沙盒 /usr/lib 下放一个普通的 .so 文件（非链接） */
    void make_real_soname_file(const std::string& soname)
    {
        const fs::path lib_dir = test_root / fs::path(constants::USR_LIB);
        fs::create_directories(lib_dir);
        std::ofstream(lib_dir / soname) << "not-a-real-elf-but-exists\n";
    }

    /** 建仓库：app 需要 soname，pkg-prov 提供它 */
    void setup_repo_with_provider(const std::string& soname)
    {
        create_pkg("pkg-prov", "1.0", {}, {soname});
        create_pkg("app", "1.0", {}, {}, {soname});
        update_index({
            {"app", "1.0", "", "", soname},
            {"pkg-prov", "1.0", "", soname, ""},
        });
    }
};

// ============================================================================
// 1. 悬空链接：不算"系统已提供" → 真实提供者必须被拉进安装计划
// ============================================================================

TEST_F(SystemSonameDanglingTest, DanglingSonameLinkStillPullsTheProvider)
{
    make_dangling_soname_link("libdangling.so.1");
    ASSERT_FALSE(fs::exists(test_root / fs::path(constants::USR_LIB) / "libdangling.so.1"))
        << "前提被破坏：这个链接不该指向存在的目标（fs::exists 跟随链接）";

    setup_repo_with_provider("libdangling.so.1");

    Config::instance().set_use_system_soname_mode(true);
    EXPECT_NO_THROW(install_packages({"app"}));

    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("app"));
    EXPECT_TRUE(Cache::instance().is_installed("pkg-prov"))
        << "悬空链接被当成了'系统已提供' → solver 不拉真实提供者，装出来的系统缺这个 SONAME";
}

// ============================================================================
// 2. farm bootstrap 的组合（--use-system-soname + --missing-so-no-error）：
//    悬空链接同样不得冒充"已提供" —— 这是**静默**装出缺库系统的那条路
// ============================================================================

TEST_F(SystemSonameDanglingTest, DanglingSonameLinkIsNotProvidedEvenWithMissingSoTolerance)
{
    make_dangling_soname_link("libdangling2.so.1");
    setup_repo_with_provider("libdangling2.so.1");

    Config::instance().set_use_system_soname_mode(true);
    Config::instance().set_missing_so_no_error_mode(true);
    // 容忍模式会把"确实没提供者"降级成告警，所以这里**不能**用"抛不抛错"当判据 ——
    // 判据只能是"提供者有没有进计划"。
    EXPECT_NO_THROW(install_packages({"app"}));

    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("app"));
    EXPECT_TRUE(Cache::instance().is_installed("pkg-prov"))
        << "开启 missing-so 容忍后，悬空链接被当'已提供'就会**静默**装出缺库的系统";
}

// ============================================================================
// 3. 正对照：SONAME 链接指向**存在的**文件 → 仍算系统已提供（不拉提供者）
//
// 这条是必需的：真实系统上的 SONAME 链接**就是**符号链接（`libfoo.so.1 → libfoo.so.1.2.3`），
// 若"是链接就一律不算已提供"，特性直接失效 —— 而上面两条"悬空"断言会因为系统 SONAME 集合
// 恒空而假装变绿。
// ============================================================================

TEST_F(SystemSonameDanglingTest, SonameLinkToExistingFileIsStillSystemProvided)
{
    make_real_soname_link("libgood.so.1");
    setup_repo_with_provider("libgood.so.1");

    Config::instance().set_use_system_soname_mode(true);
    EXPECT_NO_THROW(install_packages({"app"}));

    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("app"));
    EXPECT_FALSE(Cache::instance().is_installed("pkg-prov"))
        << "链接目标存在 → 系统已提供该 SONAME，不该再装提供者包";
}

// ============================================================================
// 4. 正对照：普通 .so 文件（非链接）→ 同上（防止"加判据"时把类型判断写反）
// ============================================================================

TEST_F(SystemSonameDanglingTest, RegularSoFileIsStillSystemProvided)
{
    make_real_soname_file("libplain.so.1");
    setup_repo_with_provider("libplain.so.1");

    Config::instance().set_use_system_soname_mode(true);
    EXPECT_NO_THROW(install_packages({"app"}));

    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("app"));
    EXPECT_FALSE(Cache::instance().is_installed("pkg-prov"))
        << "普通 .so 文件存在 → 系统已提供该 SONAME";
}

// ============================================================================
// 5. 不开关时不受影响：默认路径照旧拉提供者（与 /usr/lib 里有什么无关）
// ============================================================================

TEST_F(SystemSonameDanglingTest, DefaultPathUnaffectedByDanglingLink)
{
    make_dangling_soname_link("libdangling3.so.1");
    setup_repo_with_provider("libdangling3.so.1");

    Config::instance().set_use_system_soname_mode(false);
    EXPECT_NO_THROW(install_packages({"app"}));

    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("pkg-prov"))
        << "未开 --use-system-soname 时，系统里有什么 .so 都不该改变求解结果";
}
