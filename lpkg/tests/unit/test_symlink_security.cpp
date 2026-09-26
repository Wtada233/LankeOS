#include <gtest/gtest.h>
#include <sys/stat.h>

#include <filesystem>
#include <fstream>

#include "config.hpp"
#include "constants.hpp"
#include "nlohmann/json.hpp"
#include "package_manager.hpp"
#include "utils.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

class SymlinkLogicTest : public ::testing::Test
{
protected:
    /// 读整个文件（本文件要比对两份内容是否逐字节相同）
    static std::string read_file(const std::filesystem::path& p)
    {
        std::ifstream f(p);
        return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    }

    void Set_Up_Test_Env(const std::string& root_name)
    {
        // Use /tmp to avoid polluting project root
        test_root =
            fs::temp_directory_path() / ("lpkg_test_" + root_name + "_" + std::to_string(getpid()));
        fs::remove_all(test_root);
        fs::create_directories(test_root);
        Config::instance().set_root_path(test_root.string());
        Config::instance().init_filesystem();

        fs::create_directories(test_root / "etc/lpkg");
        {
            std::ofstream f(test_root / "etc/lpkg/mirror.conf");
            f << "http://localhost/lpkg/\n";
        }

        Config::instance().set_testing_mode(true);
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
    }

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        if (!test_root.empty()) {
            fs::remove_all(test_root);
        }
    }

    fs::path test_root;
};

TEST_F(SymlinkLogicTest, PreventsSymlinkChmodFollow)
{
    Set_Up_Test_Env("test_symlink_chmod");

    // 1. Create a "target" file with specific permissions (SUID)
    fs::path target_phys = test_root / "usr/bin/target_binary";
    ensure_dir_exists(target_phys.parent_path());
    {
        std::ofstream f(target_phys);
        f << "binary content";
    }
    chmod(target_phys.c_str(), 04755);

    struct stat st_init;
    lstat(target_phys.c_str(), &st_init);
    ASSERT_TRUE(st_init.st_mode & S_ISUID);

    // 2. Prepare a mock extraction directory
    fs::path pkg_tmp_dir = Config::get_tmp_dir() / "symlink_pkg";
    fs::remove_all(pkg_tmp_dir);
    // With content/ → / mapping, content/usr/bin/my_link installs to /usr/bin/my_link
    fs::create_directories(pkg_tmp_dir / "content" / "usr" / "bin");

    // Create symlink in content/usr/bin/ (relative to link's parent dir /usr/bin/)
    fs::create_symlink("target_binary", pkg_tmp_dir / "content" / "usr" / "bin" / "my_link");

    // Create metadata.json
    json meta;
    meta[std::string(constants::J_NAME)] = "symlink_pkg";
    meta[std::string(constants::J_VERSION)] = "1.0";
    meta[std::string(constants::J_DEPS)] = json::array();
    meta[std::string(constants::J_PROVIDES)] = json::array();
    meta[std::string(constants::J_MAN)] = "";
    {
        std::ofstream f(pkg_tmp_dir / "metadata.json");
        f << meta.dump(2) << std::endl;
    }

    InstallationTask task("symlink_pkg", "1.0", true);
    task.set_tmp_dir(pkg_tmp_dir);

    ASSERT_NO_THROW(task.copy_package_files());

    // 4. Verify the link was created at destination: /usr/bin/my_link
    fs::path link_phys = test_root / "usr/bin/my_link";
    EXPECT_TRUE(fs::is_symlink(link_phys)) << "Symlink was not created at " << link_phys;

    // 5. Verify target remains untouched
    struct stat st_final;
    lstat(target_phys.c_str(), &st_final);
    EXPECT_EQ(st_init.st_mode, st_final.st_mode);
}

TEST_F(SymlinkLogicTest, HandlesConfigSymlinkConflict)
{
    Set_Up_Test_Env("test_symlink_config");

    // 1. Create existing config as regular file
    fs::path conf_phys = test_root / "etc/os-release";
    ensure_dir_exists(conf_phys.parent_path());
    {
        std::ofstream f(conf_phys);
        f << "old release";
    }

    // 2. Mock package with os-release as symlink
    fs::path pkg_tmp_dir = Config::get_tmp_dir() / "filesystem_pkg";
    fs::remove_all(pkg_tmp_dir);
    fs::create_directories(pkg_tmp_dir / "content/etc");
    fs::create_symlink("/usr/lib/os-release", pkg_tmp_dir / "content/etc/os-release");

    json meta;
    meta[std::string(constants::J_NAME)] = "filesystem_pkg";
    meta[std::string(constants::J_VERSION)] = "1.0";
    meta[std::string(constants::J_DEPS)] = json::array();
    meta[std::string(constants::J_PROVIDES)] = json::array();
    meta[std::string(constants::J_MAN)] = "";
    {
        std::ofstream f(pkg_tmp_dir / "metadata.json");
        f << meta.dump(2) << std::endl;
    }

    InstallationTask task("filesystem_pkg", "1.0", true);
    task.set_tmp_dir(pkg_tmp_dir);

    // Trigger config conflict logic: existing file at etc/os-release.lpkgnew
    fs::path lpkgnew_path = test_root / "etc/os-release.lpkgnew";
    ensure_dir_exists(lpkgnew_path.parent_path());
    {
        std::ofstream f(lpkgnew_path);
        f << "stale new file";
    }

    ASSERT_NO_THROW(task.copy_package_files());

    // **类型变化**（盘上是普通文件、包内是符号链接）→ 原物改名 `<路径>.lpkgsave`，
    // 新链接**就地**落位（2026-09-26 统一；改前是"链接一律按配置冲突退 `.lpkgnew`、原文件
    // 留原样"）。这是直连写入趟的**回退路径**（无记录 ⇒ 没有让开趟），所以那次改名由写入趟
    // 自己按表的 `let_go = SaveConfig` 补做。
    EXPECT_TRUE(fs::is_symlink(conf_phys)) << "新链接应当**就地**落 /etc/os-release";
    EXPECT_EQ(fs::read_symlink(conf_phys), "/usr/lib/os-release");
    const fs::path save = test_root / "etc/os-release.lpkgsave";
    ASSERT_TRUE(fs::is_regular_file(save)) << "原物（盘上那份普通文件）应当整份改名到 .lpkgsave";
    EXPECT_EQ(read_file(save), "old release") << ".lpkgsave 里必须逐字节是原来那份";

    // 先前就存在的那份**陈旧 `.lpkgnew`** 不关本格的事（类型变化不再往 `.lpkgnew` 落），
    // 谁也没碰它 —— 既不是链接、内容也一字未改。
    EXPECT_FALSE(fs::is_symlink(lpkgnew_path)) << "类型变化不该往 .lpkgnew 落东西";
    EXPECT_EQ(read_file(lpkgnew_path), "stale new file") << "无关的陈旧残留不该被动";
}
