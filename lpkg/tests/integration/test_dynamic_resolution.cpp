#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/constants.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/crypto/hash.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "nlohmann/json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

class DynamicResolutionTest : public ::testing::Test
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

        suite_work_dir = fs::absolute("tmp_dynamic_res_test_" + std::to_string(::getpid()));
        test_root = suite_work_dir / "root";
        pkg_dir = suite_work_dir / "pkgs";
        mirror_dir = suite_work_dir / "mirror" / "x86_64";

        fs::remove_all(suite_work_dir);
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
        Config::instance().set_root_path("/");
        fs::remove_all(suite_work_dir);
    }

    std::string create_pkg(const std::string& name, const std::string& ver,
                           const std::vector<std::string>& deps = {},
                           const std::vector<std::string>& provides = {})
    {
        fs::path work_dir = suite_work_dir / ("pkg_work_" + name);
        fs::create_directories(work_dir / "content" / "usr" / "bin");
        std::ofstream(work_dir / "content" / "usr" / "bin" / name).close();

        std::string pkg_filename = std::format("{}-{}.lpkg", name, ver);
        std::string pkg_path = (pkg_dir / pkg_filename).string();
        pack_package(pkg_path, work_dir.string(), name, ver, deps, provides);

        // Put in mirror
        fs::path mirror_pkg_dir = mirror_dir / name;
        fs::create_directories(mirror_pkg_dir);
        fs::copy_file(pkg_path, mirror_pkg_dir / (ver + ".lpkg"),
                      fs::copy_options::overwrite_existing);

        fs::remove_all(work_dir);
        return pkg_path;
    }

    void update_index(
        const std::vector<std::tuple<std::string, std::string, std::string, std::string>>& entries)
    {
        std::ofstream index(mirror_dir / "index.txt");
        for (const auto& [name, ver, deps, provides] : entries) {
            std::string pkg_filename = std::format("{}-{}.lpkg", name, ver);
            std::string pkg_path = (pkg_dir / pkg_filename).string();
            std::string hash = "unknown";
            if (fs::exists(pkg_path)) {
                hash = calculate_sha256(pkg_path);
            }
            index << name << "|" << ver << ":" << hash << ":" << deps << ":" << provides << ":|\n";
        }
    }
};

// ============================================================================
// 归档 metadata 与仓库索引不一致 → **拒绝安装**（不再"动态重解析"）
//
// ⚠️ 2026-10-02：lpkg 删除了"下载后按真实 metadata 重解计划"的行为。它是 lpkg 手动解析
//   依赖时代的产物；引入 libsolv 之后计划是一次性整体求解出来的，批次中途改计划会导致
//   重复安装 / 顺序倒置 / 所有权脱节。现在索引与归档不一致一律硬报错
//   `error.metadata_mismatch`，整批回滚（详见 package_manager.cpp 的 verify_package_metadata）。
// ============================================================================
TEST_F(DynamicResolutionTest, IndexDependencyMismatchIsRefused)
{
    // libA / libB 都在仓库里；app 的真实 metadata 依赖 libB，而索引（过时）说它依赖 libA。
    create_pkg("libA", "1.0");
    create_pkg("libB", "1.0");
    create_pkg("app", "1.0", {"libB"});
    update_index({{"app", "1.0", "libA", ""}, {"libA", "1.0", "", ""}, {"libB", "1.0", "", ""}});

    try {
        install_packages({"app"});
        FAIL() << "metadata/index mismatch must be refused, not silently re-resolved";
    } catch (const LpkgException& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("app"), std::string::npos) << msg;   // 点名包
        EXPECT_NE(msg.find("deps"), std::string::npos) << msg;  // 点名差异字段
    }

    // 整批回滚：一个包都不该装上（尤其不能"装了 libA 又装了 libB"）
    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed("app"));
    EXPECT_FALSE(Cache::instance().is_installed("libA"));
    EXPECT_FALSE(Cache::instance().is_installed("libB"));
}

TEST_F(DynamicResolutionTest, IndexProviderMismatchIsRefused)
{
    // 索引说 provA 提供 virtual-pkg（过时）；provA 的真实 metadata 只提供 other-pkg。
    create_pkg("provA", "1.0", {}, {"other-pkg"});
    create_pkg("provB", "1.0", {}, {"virtual-pkg"});
    create_pkg("app", "1.0", {"virtual-pkg"});

    update_index({{"app", "1.0", "virtual-pkg", ""},
                  {"provA", "1.0", "", "virtual-pkg"},
                  {"provB", "1.0", "", "virtual-pkg"}});

    try {
        install_packages({"app"});
        FAIL() << "metadata/index mismatch (provides) must be refused";
    } catch (const LpkgException& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("provA"), std::string::npos) << msg;
        EXPECT_NE(msg.find("provides"), std::string::npos) << msg;
    }

    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed("app"));
    EXPECT_FALSE(Cache::instance().is_installed("provA"));
    EXPECT_FALSE(Cache::instance().is_installed("provB"));
}

// ====== 4.1: 依赖名漂移 ======
// 索引说 app 依赖 lib-old，真实 metadata 依赖 lib-new（仓库里没有 lib-new）。
// 现在无论 lib-new 存不存在，都在**元数据一致性校验**处就拒绝。
TEST_F(DynamicResolutionTest, UnresolvableDriftFailure)
{
    // Index says 'app' depends on 'lib-old'.
    // Real metadata.json says 'app' depends on 'lib-new'.
    // 'lib-new' does NOT exist in the repository.
    create_pkg("lib-old", "1.0");

    // 'app' package: real metadata declares 'lib-new' dependency
    create_pkg("app", "1.0", {"lib-new"});

    // Index says it depends on 'lib-old' — only lib-old exists there
    std::string hash = fs::exists(pkg_dir / "app-1.0.lpkg")
                           ? calculate_sha256(pkg_dir / "app-1.0.lpkg")
                           : "unknown";
    std::string lib_hash = fs::exists(pkg_dir / "lib-old-1.0.lpkg")
                               ? calculate_sha256(pkg_dir / "lib-old-1.0.lpkg")
                               : "unknown";
    {
        std::ofstream index(mirror_dir / "index.txt");
        index << "app|1.0:" << hash << ":lib-old|\n";
        index << "lib-old|1.0:" << lib_hash << ":|\n";
    }

    // 拒绝：metadata 与索引不一致（lib-new 是否存在都一样）
    EXPECT_THROW(install_packages({"app"}), LpkgException);
}

// ====== 4.2: 索引漏声明的依赖 ======
// 索引说 app 没有依赖，真实 metadata 却依赖 lib-extra —— 现在**拒绝**（不再"发现并安装"）。
TEST_F(DynamicResolutionTest, UndeclaredDependencyIsRefused)
{
    create_pkg("lib-extra", "1.0");
    create_pkg("app", "1.0", {"lib-extra"});
    update_index({{"app", "1.0", "", ""}, {"lib-extra", "1.0", "", ""}});

    try {
        install_packages({"app"});
        FAIL() << "an index that omits a real dependency must be refused (rebuild the index)";
    } catch (const LpkgException& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("app"), std::string::npos) << msg;
        EXPECT_NE(msg.find("deps"), std::string::npos) << msg;
    }

    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed("app"));
    EXPECT_FALSE(Cache::instance().is_installed("lib-extra"));
}

// ====== 4.3: metadata 不一致 → 整批回滚 ======
// app 的真实 metadata 依赖 broken-dep，索引却说它没有依赖 → 一致性校验拒绝、什么都不装。
TEST_F(DynamicResolutionTest, AtomicRollbackOnFailedDep)
{
    // app 的真实 metadata 依赖 'broken-dep'（不放进镜像）
    create_pkg("app", "1.0", {"broken-dep"});

    // Index says app has no deps
    {
        std::string hash = fs::exists(pkg_dir / "app-1.0.lpkg")
                               ? calculate_sha256(pkg_dir / "app-1.0.lpkg")
                               : "unknown";
        std::ofstream index(mirror_dir / "index.txt");
        index << "app|1.0:" << hash << ":|\n";
        // broken-dep is NOT in the index — it will fail resolution
    }

    // 拒绝（metadata 不一致），且 app 不得登记为已安装
    EXPECT_THROW(install_packages({"app"}), LpkgException);

    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed("app"));
}

// ====== 4.4: 归档自称的 name/version 与索引不符 → 拒绝 ======
// 逐字段比对（不再只比"依赖面"）：索引说 app 是 1.0（于是去取 app/1.0.lpkg），
// 而那个文件里的 metadata 自称 2.0 → 字段 `version` 不符 → 拒绝。
TEST_F(DynamicResolutionTest, ArchiveVersionMismatchIsRefused)
{
    const std::string real = create_pkg("app", "2.0");  // 真身是 2.0
    fs::create_directories(mirror_dir / "app");
    fs::copy_file(real, mirror_dir / "app" / "1.0.lpkg", fs::copy_options::overwrite_existing);
    {
        // 索引里的哈希对着**这个文件**（于是哈希校验会过，只有 metadata 字段对不上）
        const std::string hash = calculate_sha256(real);
        std::ofstream idx(mirror_dir / "index.txt");
        idx << "app|1.0:" << hash << ":::|\n";
    }

    try {
        install_packages({"app"});
        FAIL() << "归档 metadata 自称的 version 与索引不符时必须拒绝";
    } catch (const LpkgException& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("version"), std::string::npos) << "差异字段要点名：" << msg;
        EXPECT_NE(msg.find("2.0"), std::string::npos) << msg;
    }

    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed("app"));
}
