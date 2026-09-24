/**
 * test_db_durability.cpp — DB 备份生命周期 × 断电持久化（缺陷 ① 的集成侧）
 *
 * 单元侧（test_durable_fsync_db.cpp）证明"DB/元数据写在开关关闭时仍会 fsync"。
 * 这里从整条安装链路看两件事：
 *   1. 开关关闭下的真实安装，DB 写确实发出了 fsync；
 *   2. 成功批次收尾后 `.lpkg_db_bak_before:*` 一个不剩 —— 与 1 合起来消除
 *      "新库还在页缓存、唯一备份已被删"这个系统级不可恢复窗口。
 *      （未提交批次失败时**不得**删备份：test_wal_rollback_guards.cpp 覆盖。）
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

class DbDurabilityTest : public IntegrationTestBase
{
protected:
    void SetUp() override
    {
        IntegrationTestBase::SetUp();
        Config::instance().set_no_hooks_mode(true);
        set_durable_fsync_enabled(false);  // 默认模式（开关关闭）才是有问题的那个模式
    }

    void TearDown() override
    {
        set_durable_fsync_enabled(false);
        IntegrationTestBase::TearDown();
    }

    /** 沙盒 root 下所有 .lpkg_db_bak_before:* 备份 */
    std::vector<fs::path> db_backups() const
    {
        std::vector<fs::path> out;
        for (const auto& e : fs::recursive_directory_iterator(test_root))
            if (e.path().filename().string().find(".lpkg_db_bak_before:") != std::string::npos)
                out.push_back(e.path());
        return out;
    }
};

TEST_F(DbDurabilityTest, InstallFsyncsDbWritesEvenWithSwitchOff)
{
    set_durable_fsync_enabled(false);
    const std::string p = create_pkg("dbfsync", "1.0");

    const size_t before = durable_fsync_count_for_tests();
    ASSERT_NO_THROW(install_packages({p}));
    const size_t n = durable_fsync_count_for_tests() - before;

    EXPECT_TRUE(Cache::instance().is_installed("dbfsync"));
    EXPECT_GT(n, 0u) << "开关关闭时整条安装链路里 DB/元数据写一次 fsync 都没做 —— "
                        "DB 只 rename 到页缓存，而收尾立刻删掉 .lpkg_db_bak_before 备份";
}

TEST_F(DbDurabilityTest, SuccessfulBatchLeavesNoDbBackupsAndConsistentDb)
{
    const std::string pa = create_pkg("dbbaka", "1.0");
    const std::string pb = create_pkg("dbbakb", "1.0");
    ASSERT_NO_THROW(install_packages({pa, pb}));

    // 第二批开始前 DB 文件已存在 → 每个 DB 写都会先备份；提交后必须删干净
    EXPECT_TRUE(db_backups().empty())
        << "成功批次收尾没有清掉 DB 备份（残留会让下次启动的清理把它们当成"
           "未提交批次的恢复依据，也可能与后续批次撞名）";
    // DB 内容完整（不是空/截断）
    EXPECT_TRUE(fs::exists(Config::instance().files_db()));
    EXPECT_FALSE(Cache::instance().get_package_files("dbbaka").empty());
    EXPECT_FALSE(Cache::instance().get_package_files("dbbakb").empty());
}
