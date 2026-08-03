/**
 * test_write_file_wal.cpp — wal::write_string_file_wal 回归测试
 *
 * 回归背景：register_package 曾对 deps/needed_so/man 元数据文件硬编码 DBNEW
 * WAL 行并用无备份的 write_string_to_file 写入。升级时旧文件已存在但没有
 * .lpkg_db_bak_before 备份，批次回滚时 reverse_execute 的 DBNEW 分支会删除
 * 旧文件——升级回滚后旧版本的依赖/needed_so/man 元数据丢失。
 *
 * write_string_file_wal 修复了这一点：已存在的文件走 DB（备份旧内容）、
 * 新文件走 DBNEW、内容为空走 DBRM（备份后删除），回滚均能恢复。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "../../main/src/base/exception.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/transaction_log.hpp"
#include "../../main/src/db/wal_op.hpp"

namespace fs = std::filesystem;

class WriteFileWalTest : public ::testing::Test
{
protected:
    fs::path suite_dir;
    fs::path test_root;
    fs::path dep_dir;

    void SetUp() override
    {
        suite_dir = fs::absolute("tmp_write_file_wal_test");
        if (fs::exists(suite_dir)) fs::remove_all(suite_dir);
        test_root = suite_dir / "root";
        fs::create_directories(test_root);

        Config::instance().set_root_path(test_root.string());
        Config::instance().set_testing_mode(true);
        Config::instance().init_filesystem();
        dep_dir = Config::instance().dep_dir();
        fs::create_directories(dep_dir);
        Cache::instance().load();
    }

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        fs::remove_all(suite_dir);
    }

    std::string read_wal()
    {
        std::string wpath = wal::wal_log_path();
        if (!fs::exists(wpath)) return "";
        std::ifstream f(wpath);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    std::string read_file(const fs::path& p)
    {
        std::ifstream f(p);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    std::string bak_path(const fs::path& p, const std::string& milestone)
    {
        return p.string() + ".lpkg_db_bak_before:" + milestone;
    }
};

// 新文件：DBNEW、无备份；reverse_execute 删除新文件
TEST_F(WriteFileWalTest, NewFileWritesDBNEWAndReverseRemoves)
{
    wal::begin_batch();
    fs::path dep = dep_dir / "newpkg";
    wal::write_string_file_wal(dep.string(), "dep1\ndep2\n", "newpkg:installed");

    EXPECT_TRUE(fs::exists(dep));
    EXPECT_FALSE(fs::exists(bak_path(dep, "newpkg:installed")));
    EXPECT_NE(read_wal().find("DBNEW " + dep.string() + " newpkg:installed"), std::string::npos);

    auto ops = wal::extract_current_batch_ops(wal::wal_log_path());
    wal::reverse_execute(ops, true);
    EXPECT_FALSE(fs::exists(dep));
}

// 已存在文件：DB、先备份旧内容；reverse_execute 恢复旧内容
TEST_F(WriteFileWalTest, ExistingFileBacksUpOldContentAndReverseRestores)
{
    fs::path dep = dep_dir / "oldpkg";
    {
        std::ofstream f(dep);
        f << "olddep\n";
    }

    wal::begin_batch();
    wal::write_string_file_wal(dep.string(), "newdep\n", "oldpkg:installed");

    // 旧内容已备份，新内容已写入
    EXPECT_TRUE(fs::exists(bak_path(dep, "oldpkg:installed")));
    EXPECT_EQ(read_file(bak_path(dep, "oldpkg:installed")), "olddep\n");
    EXPECT_EQ(read_file(dep), "newdep\n");
    // WAL 行是 DB（文件已存在），不是 DBNEW
    EXPECT_NE(read_wal().find("DB " + dep.string() + " oldpkg:installed"), std::string::npos);

    auto ops = wal::extract_current_batch_ops(wal::wal_log_path());
    wal::reverse_execute(ops, true);
    // 旧内容恢复，备份被消费
    EXPECT_EQ(read_file(dep), "olddep\n");
    EXPECT_FALSE(fs::exists(bak_path(dep, "oldpkg:installed")));
}

// 已存在文件 + 空内容：DBRM 备份后删除；reverse_execute 恢复旧内容
TEST_F(WriteFileWalTest, EmptyContentWritesDBRMAndReverseRestores)
{
    fs::path dep = dep_dir / "emptypkg";
    {
        std::ofstream f(dep);
        f << "oldcontent\n";
    }

    wal::begin_batch();
    wal::write_string_file_wal(dep.string(), "", "emptypkg:installed");

    EXPECT_FALSE(fs::exists(dep));
    EXPECT_TRUE(fs::exists(bak_path(dep, "emptypkg:installed")));
    EXPECT_NE(read_wal().find("DBRM " + dep.string() + " emptypkg:installed"), std::string::npos);

    auto ops = wal::extract_current_batch_ops(wal::wal_log_path());
    wal::reverse_execute(ops, true);
    EXPECT_EQ(read_file(dep), "oldcontent\n");
}

// 文件不存在且内容为空：无操作（默认 create_empty=false）
TEST_F(WriteFileWalTest, MissingFileWithEmptyContentIsNoop)
{
    wal::begin_batch();
    fs::path dep = dep_dir / "ghost";
    wal::write_string_file_wal(dep.string(), "", "ghost:installed");

    EXPECT_FALSE(fs::exists(dep));
    // WAL 里不能出现 ghost 的任何记录
    EXPECT_EQ(read_wal().find("ghost"), std::string::npos);
}

// create_empty=true：空内容也创建空文件（deps 文件语义：空=无依赖是显式状态）
TEST_F(WriteFileWalTest, CreateEmptyCreatesEmptyFileWithDBNEW)
{
    wal::begin_batch();
    fs::path dep = dep_dir / "nodeps";
    wal::write_string_file_wal(dep.string(), "", "nodeps:installed", /*create_empty=*/true);

    EXPECT_TRUE(fs::exists(dep));
    EXPECT_EQ(read_file(dep), "");
    EXPECT_NE(read_wal().find("DBNEW " + dep.string() + " nodeps:installed"), std::string::npos);

    // reverse_execute 删除新创建的空文件
    auto ops = wal::extract_current_batch_ops(wal::wal_log_path());
    wal::reverse_execute(ops, true);
    EXPECT_FALSE(fs::exists(dep));
}
