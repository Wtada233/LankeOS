#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "../../main/src/archive/archive.hpp"
#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/constants.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/scan/scanner.hpp"
#include "nlohmann/json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

class ToolsTest : public ::testing::Test
{
protected:
    fs::path suite_work_dir;
    fs::path source_dir;
    fs::path root_dir;
    fs::path hooks_dir;
    fs::path output_pkg;
    fs::path test_system_root;

    void SetUp() override
    {
        init_localization();
        suite_work_dir = fs::absolute("tmp_tools_test_" + std::to_string(::getpid()));
        source_dir = suite_work_dir / "lankepkg";
        root_dir = source_dir / "content";
        hooks_dir = source_dir / "hooks";
        output_pkg = suite_work_dir / "test.pkg.lpkg";
        test_system_root = suite_work_dir / "sysroot";

        fs::create_directories(root_dir / "usr/bin");
        fs::create_directories(hooks_dir);
        fs::create_directories(test_system_root / "var/lib/lpkg");  // DB dir

        Config::instance().set_root_path(test_system_root.string());
        Config::instance().init_filesystem();
    }

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        if (fs::exists(suite_work_dir)) fs::remove_all(suite_work_dir);
    }
};

TEST_F(ToolsTest, PackAndVerifyContent)
{
    // 1. Prepare source files
    std::ofstream f(root_dir / "usr/bin/hello");
    f << "executable_content";
    f.close();

    std::ofstream h(hooks_dir / "postinst.sh");
    h << "echo hook";
    h.close();

    // 2. Execute pack with name/version
    EXPECT_NO_THROW(pack_package(output_pkg.string(), source_dir.string(), "test-pkg", "1.0"));
    EXPECT_TRUE(fs::exists(output_pkg));

    // 3. Extract and verify content
    fs::path verify_dir = suite_work_dir / "verify_pack";
    fs::create_directories(verify_dir);

    extract_tar_zst(output_pkg, verify_dir, fs::path(output_pkg).filename().string());

    // Check core file structure (metadata.json replaces files.txt)
    EXPECT_TRUE(fs::exists(verify_dir / "content/usr/bin/hello"));
    EXPECT_TRUE(fs::exists(verify_dir / "hooks/postinst.sh"));
    EXPECT_TRUE(fs::exists(verify_dir / "metadata.json"));

    // Verify metadata.json content
    {
        std::ifstream meta_f(verify_dir / "metadata.json");
        json meta;
        meta_f >> meta;
        EXPECT_EQ(meta[std::string(constants::J_NAME)], "test-pkg");
        EXPECT_EQ(meta[std::string(constants::J_VERSION)], "1.0");
        EXPECT_TRUE(meta.contains(std::string(constants::J_DEPS)));
        EXPECT_TRUE(meta.contains(std::string(constants::J_PROVIDES)));
        EXPECT_TRUE(meta.contains(std::string(constants::J_MAN)));
    }
}

TEST_F(ToolsTest, ScanOrphansLogic)
{
    // 1. Create orphan file
    fs::create_directories(test_system_root / "usr/bin");
    std::ofstream orphan(test_system_root / "usr/bin/orphan");
    orphan << "orphan";
    orphan.close();

    // 2. Create owned file and register to database
    std::ofstream owned(test_system_root / "usr/bin/owned");
    owned << "owned";
    owned.close();

    {
        std::ofstream db(test_system_root / "var/lib/lpkg/files.db");
        db << "/usr/bin/owned\ttest-pkg" << std::endl;
    }
    Cache::instance().load();

    // 3. Run scan and capture output
    testing::internal::CaptureStdout();
    scan_orphans(test_system_root.string());
    std::string output = testing::internal::GetCapturedStdout();

    // 4. Verify
    EXPECT_NE(output.find("orphan"), std::string::npos) << "Should detect orphan file";
    EXPECT_EQ(output.find("owned"), std::string::npos) << "Should NOT report owned file as orphan";
}

TEST_F(ToolsTest, ScanOrphansDoesNotIgnoreSiblingPrefixDirectories)
{
    // 忽略前缀必须按**路径分量**匹配，不是字符串前缀：`usr/share/lpkgx` 不是 `usr/share/lpkg`。
    // 旧写法是字符串前缀比较（`path.compare(0, prefix.size(), prefix) == 0`），于是同级目录
    // 下的孤儿被静默当成忽略目录、永不报出（2026-10-03 修）。
    //
    // 钉子选 **`usr/share/lpkg`**（两份忽略清单里都有它），不用 `usr/man` / `var/log` 这类
    // —— 那些条目只存在于某一份清单里，换清单就会让这条用例红在与被测行为无关的地方。
    fs::create_directories(test_system_root / "usr/share/lpkgx");
    fs::create_directories(test_system_root / "usr/share/lpkg");
    std::ofstream(test_system_root / "usr/share/lpkgx/orphan_sibling") << "x";
    std::ofstream(test_system_root / "usr/share/lpkg/ignored_orphan") << "x";
    Cache::instance().load();

    testing::internal::CaptureStdout();
    scan_orphans(test_system_root.string());
    const std::string out = testing::internal::GetCapturedStdout();

    EXPECT_NE(out.find("orphan_sibling"), std::string::npos)
        << "usr/share/lpkgx 不是 usr/share/lpkg —— 同级前缀目录下的孤儿必须被报出来";
    EXPECT_EQ(out.find("ignored_orphan"), std::string::npos)
        << "usr/share/lpkg 下的文件仍必须忽略（防边界修复把真目录也放开）";
}

TEST_F(ToolsTest, ScanPrunesIgnoredDirectoriesInsteadOfOnlyFilteringThem)
{
    // "忽略前缀"必须让遍历**根本不走进去**，而不是"走进去、只是不报告"（2026-10-03 修）。
    //
    // 为什么这不是洁癖：`recursive_directory_iterator` 走进**易变**目录时会失败 —— 实测本机
    // 扫 `/` 时在 `/proc/<pid>/task/<pid>/net` 上拿到 EINVAL，而 `increment(ec)` 出错会
    // **把迭代器置为 end** ⇒ 整趟扫描静默截断，`/usr` 那 28 万个文件一个都没扫到，
    // 命令还打印"共扫描 389551 个文件"（看着挺大，其实是半棵树）。剪枝根治已知触发器。
    //
    // 判据：**两次扫描的 stdout 必须逐字相同**。往被忽略的前缀里再塞文件，若仍被遍历，
    // `Scanned {} files` 里的数会跟着涨、输出就变了。
    // 故意**不**钉那个数的具体值 —— `init_filesystem()` 会在 lpkg 数据目录下生成若干文件，
    // 那个数由夹具决定，钉死它只是造一条脆断言。
    // 被忽略的子树取 **`usr/share/lpkg`**（两份忽略清单里都有），理由同上面那条用例。
    fs::create_directories(test_system_root / "usr/bin");
    // 名字要够独特：summary 里本来就有 "orphaned files" 这个词，拿 "orphan" 当锚是恒真断言。
    std::ofstream(test_system_root / "usr/bin/scanprune_orphan") << "x";
    Cache::instance().load();

    testing::internal::CaptureStdout();
    scan_orphans(test_system_root.string());
    const std::string baseline = testing::internal::GetCapturedStdout();
    ASSERT_NE(baseline.find("scanprune_orphan"), std::string::npos)
        << "前置：基线扫描本身要能报出孤儿";

    // 往**被忽略的**子树里再加一个文件 —— 它既不该被报告，也不该被**遍历**到。
    fs::create_directories(test_system_root / "usr/share/lpkg/deep/deeper");
    std::ofstream(test_system_root / "usr/share/lpkg/deep/deeper/buried") << "x";

    testing::internal::CaptureStdout();
    scan_orphans(test_system_root.string());
    const std::string with_buried = testing::internal::GetCapturedStdout();

    EXPECT_EQ(with_buried.find("buried"), std::string::npos) << "忽略目录下的文件不该被报告";
    EXPECT_EQ(with_buried, baseline)
        << "被忽略的目录必须被**剪枝**（不进遍历），而不只是过滤报告 —— "
           "若还走进 usr/share/lpkg/，Scanned 计数会变、两次输出就对不上了";
}
