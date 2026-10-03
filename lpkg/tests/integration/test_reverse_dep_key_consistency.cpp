/**
 * test_reverse_dep_key_consistency.cpp — `deps/` 行 → 依赖**包名**：四种实现、三种规则
 *
 * `deps/` 元数据文件里存的是**原样的**依赖串（`register_package` 直接把 `deps_` 写进去，
 * `installation_task_register.cpp`）。而"从这一行里取出包名"这件事在四处各写了一遍、
 * 用了三种不同规则：
 *
 *   · `install_common.cpp`  `get_all_required_packages`   → `find_first_of(" \t<>=")`
 *   · `installation_task_register.cpp` **登记**反向依赖      → `find_first_of(" \t<>=")`
 *   · `installation_task_register.cpp` **摘除**反向依赖      → `ss >> dn`（**纯空白**）
 *   · `db/cache.cpp`       `ensure_reverse_deps`（建索引）  → `find_first_of(" \t")`
 *
 * ── 订正：我原本断言的"登记/摘除键不一致 ⇒ 摘不掉"是**错的** ──────────────────────
 * `Cache::load()` 会把 `reverse_deps_loaded` 复位（`cache.cpp:450`），于是**每次摘除之前
 * 必然先有一次 `ensure_reverse_deps()` 重建**，而重建用的规则（纯空白）恰好与摘除用的
 * 规则（`ss >> dn`）**一致** ⇒ 两者互相抵消，那条分叉被惰性重建**掩盖**了。
 * 我按"摘不掉"写的第一版用例，在**改任何代码之前就是绿的** —— 那是恒真废话，不是证据
 * （实测踩到：三条全绿）。这一段留在这里，免得后人再按同一个错推演写一遍。
 *
 * ── 真正**可达**的缺陷在 `ensure_reverse_deps` 的规则上 ────────────────────────────
 * 它用**纯空白**切名字 ⇒ 遇到 `provb>=2.0` 这种**约束紧贴包名**（无空格）的写法，整串被
 * 当成包名，键成了 `provb>=2.0`。于是**按包名查永远查不到**：
 *   · `get_reverse_deps("provb")` 空 ⇒ **autoremove 看不到还有谁依赖它**
 *     （`collect_recursive_remove_set` 走的就是这条），可能把它连带删掉。
 *
 * 用例里刻意用**两种**写法各考一遍：无空格的 `provb>=2.0`（在此分叉）与带空格的
 * `provc >= 2.0`（在此恰好一致）——后者是**对照**，它今天就该绿，用来证明"红"不是
 * 因为用例本身写坏了。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

class ReverseDepKeyConsistencyTest : public IntegrationTestBase
{
protected:
    void SetUp() override
    {
        IntegrationTestBase::SetUp();
        setup_local_mirror();  // 空镜像：让 install 的 repo.load_index 快速且不联网
    }

    void TearDown() override
    {
        IntegrationTestBase::TearDown();
    }

    /** 打一个只含 `usr/bin/<name>` 的包，`deps` 原样进 metadata */
    std::string pack(const std::string& name, const std::string& ver,
                     const std::vector<std::string>& deps = {})
    {
        const fs::path work = suite_work_dir / ("_pkg_" + name + "_" + ver);
        fs::remove_all(work);
        fs::create_directories(work / "content/usr/bin");
        std::ofstream(work / "content/usr/bin" / name) << name << " " << ver << "\n";
        const std::string path = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(path, work.string(), name, ver, deps, {}, "man " + name, {});
        return path;
    }

    /**
     * 先把"被依赖的那个包"**真装进去** —— 否则求解器会在 `libB`/`libC` 上直接拒绝
     * （`Dependency 'x' has no provider in repository`），用例红在**前提没搭起来**上，
     * 而不是红在它要考的那件事上（实测踩到：三条全红在 no provider）。
     */
    void install_provider(const std::string& name, const std::string& ver)
    {
        ASSERT_NO_THROW(install_packages({pack(name, ver)}));
        ASSERT_EQ(Cache::instance().get_installed_version(name), ver);
    }
};

TEST_F(ReverseDepKeyConsistencyTest, NoSpaceConstraintIsStillFoundByPackageName)
{
    const std::string pkg = "rdnospace";
    install_provider("provb", "2.0");

    // 依赖串**约束紧贴包名**（`deps/provnospace` 文件里存的就是 "provb>=2.0" 这一串原文）
    ASSERT_NO_THROW(install_packages({pack(pkg, "1.0", {"provb>=2.0"})}));
    ASSERT_EQ(Cache::instance().get_installed_version(pkg), "1.0");

    // 模拟**下一条命令**：`Cache::load()` 复位 `reverse_deps_loaded`，下一次查询必然触发
    // `ensure_reverse_deps()` 从盘上重建索引 —— 这才是 autoremove 真实看到的那份索引。
    Cache::instance().load();

    const auto dependents = Cache::instance().get_reverse_deps("provb");
    EXPECT_TRUE(dependents.find(pkg) != dependents.end())
        << "按**包名** 'provb' 查不到依赖者 " << pkg
        << " —— 重建索引时把整串 'provb>=2.0' 当成了包名。autoremove 正是这么查的，"
           "于是它会以为 provb 没人依赖而把它连带删掉。";
}

/** 对照组：**带空格**的常规写法在纯空白规则下恰好正确 —— 它今天就该绿。 */
TEST_F(ReverseDepKeyConsistencyTest, SpacedConstraintIsTheControlGroup)
{
    const std::string pkg = "rdspaced";
    install_provider("provc", "2.0");

    ASSERT_NO_THROW(install_packages({pack(pkg, "1.0", {"provc >= 2.0"})}));
    Cache::instance().load();

    const auto dependents = Cache::instance().get_reverse_deps("provc");
    EXPECT_TRUE(dependents.find(pkg) != dependents.end())
        << "对照组也不成立 —— 那说明问题不在'约束紧贴包名'上，用例的前提写错了";
}

/** 复合约束（`>= a < b`）且**第一段也紧贴**：整串仍不该被当成包名。 */
TEST_F(ReverseDepKeyConsistencyTest, CompoundConstraintNameIsFoundByPackageName)
{
    const std::string pkg = "rdcompound";
    install_provider("provd", "2.0");

    ASSERT_NO_THROW(install_packages({pack(pkg, "1.0", {"provd>=2.0 <3.0"})}));
    Cache::instance().load();

    const auto dependents = Cache::instance().get_reverse_deps("provd");
    EXPECT_TRUE(dependents.find(pkg) != dependents.end())
        << "按包名 'provd' 查不到 —— 纯空白规则把 'provd>=2.0' 当成了包名";
}
