/**
 * test_tmp_path_symlink_guard.cpp — `.lpkgtmp` 落位不得写穿符号链接
 *
 * ── 缺陷 ────────────────────────────────────────────────────────────────────
 * 写入层原语是"先写 `<目标>.lpkgtmp`，再 `rename(tmp → 目标)`"。而**写 tmp 的每一步都
 * 跟随末段符号链接**：`fs::copy(..., overwrite_existing)`、`chmod`/`lchown`、
 * `fs::permissions` 全都会落到链接目标上。于是盘上只要存在一个
 * `usr/bin/foo.lpkgtmp → /etc/sudoers`，处理 `usr/bin/foo` 时内容与权限就写到
 * `/etc/sudoers` 上去了。而**冲突预检只看归档清单里的路径、不看 `<目标>.lpkgtmp`**
 * （它不在清单里），全程无告警 —— 重装/升级时确定性命中。
 *
 * 三处同构代码都要挡（分处 `installation_task_copy.cpp`（2 处）与
 * `installation_task_register.cpp`（1 处））：普通文件的 in-place 分支、 配置的 `.lpkgnew`
 * 分支、hook 脚本分支。判据**只能是** `is_symlink`，不能是 `exists`： 上一轮崩溃留下的 `.lpkgtmp`
 * **普通文件**是正常残留，必须照旧覆盖（`StaleRegular…`
 * 那条测试专门钉这个边界，防止"修太狠"把正常的崩溃续写也拒了）。
 *
 * ── 取证方式 ───────────────────────────────────────────────────────────────
 * 链接目标指向**沙盒 root 之外**的普通文件（`root_dir()` 是 test_root，victim 在
 * suite_work_dir 下）—— 这正是危害所在：包内容写到安装根之外。断言"安装失败 + 目标文件
 * 内容与权限**逐字节未变**"，而不是只看抛没抛异常（写穿了也可能照样抛在别处）。
 *
 * 三个分支的到达方式：
 *   · in-place 与 hook 分支：走真实 `install_packages`（顶层入口，最贴近用户看到的"安装失败"）；
 *   · `.lpkgnew` 分支：需要"盘上配置被用户改过 + 包内配置又变了"（三哈希三条互异），
 *     构造三段式安装只为触发一个分支不划算，改按既有风格直连 `copy_package_files()`
 *     （见 tests/unit/test_symlink_security.cpp 的同款用法）—— 它正是走到该分支的那次调用。
 */

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/constants.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "nlohmann/json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

class TmpPathSymlinkGuardTest : public ::testing::Test
{
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;
    /// 沙盒 root **之外**的受害文件：链接指向它，"写穿"就是写到安装根外面
    fs::path victim;
    /// 受害文件落盘时的权限（比对"没被 chmod 跟随改掉"；绝对值受 umask 影响，故取现场值）
    mode_t victim_mode = 0;

    void SetUp() override
    {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        init_localization();

        suite_work_dir = fs::absolute("tmp_tmp_path_symlink_test");
        if (fs::exists(suite_work_dir)) fs::remove_all(suite_work_dir);
        test_root = suite_work_dir / "root";
        pkg_dir = suite_work_dir / "pkgs";
        fs::create_directories(test_root);
        fs::create_directories(pkg_dir);

        Config::instance().set_root_path(test_root.string());
        Config::instance().init_filesystem();
        Cache::instance().load();

        victim = suite_work_dir / "victim_outside_root.txt";
        std::ofstream(victim) << "ORIGINAL VICTIM CONTENT\n";
        {
            struct stat st;
            ASSERT_EQ(lstat(victim.c_str(), &st), 0);
            victim_mode = st.st_mode & 07777;
        }
    }

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        fs::remove_all(suite_work_dir);
    }

    static void write_file(const fs::path& p, const std::string& content)
    {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << content;
    }

    static std::string read_file(const fs::path& p)
    {
        std::ifstream f(p);
        return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    }

    /**
     * 打一个包。fill 拿到 **(包根, content/)** 两个路径：content/ 是安装内容，包根下另有
     * `hooks/`（`pack_package` 从包根取 hooks/，写进 content/ 里就变成安装到 /hooks 了）。
     */
    template <typename F>
    std::string pack(const std::string& name, const std::string& ver, F fill) const
    {
        const fs::path work = suite_work_dir / ("_pkg_" + name + "_" + ver);
        fs::create_directories(work / "content");
        fill(work, work / "content");
        const std::string path = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(path, work.string(), name, ver, {}, {}, "man " + name, {});
        return path;
    }

    /** 受害文件必须**逐字节**保持原样（内容），且权限没被 `chmod` 跟随改掉 */
    void expect_victim_untouched(const std::string& what) const
    {
        EXPECT_EQ(read_file(victim), "ORIGINAL VICTIM CONTENT\n")
            << what << "：链接目标文件的内容被写穿了";
        struct stat st;
        ASSERT_EQ(lstat(victim.c_str(), &st), 0);
        EXPECT_EQ(st.st_mode & 07777, victim_mode) << what << "：链接目标文件的权限被改过";
    }
};

/**
 * ① in-place 分支（真实安装入口）：盘上已有 `usr/bin/foo.lpkgtmp → <root 外>`。
 *
 * 攻击者不必自己动手：同一批里另一个包（或同一包的另一个成员）把这条链接当内容装进去即可
 * —— 冲突预检不扫 `<目标>.lpkgtmp`，链接照样能落到盘上。这里直接预置该状态，锚定"处理
 * `foo` 时绝不跟着链接写"。
 */
TEST_F(TmpPathSymlinkGuardTest, InPlaceTmpSymlinkIsRefused)
{
    const fs::path tmp_link = test_root / "usr" / "bin" / "foo.lpkgtmp";
    fs::create_directories(tmp_link.parent_path());
    fs::create_symlink(victim.string(), tmp_link);
    ASSERT_TRUE(fs::is_symlink(tmp_link));  // 前提取证：链接确实在盘上

    const std::string pkg = pack("foo_pkg", "1.0", [](const fs::path&, const fs::path& c) {
        write_file(c / "usr/bin/foo", "NEW\n");
    });

    std::string msg;
    try {
        install_packages({pkg});
    } catch (const LpkgException& e) {
        msg = e.what();
    }
    EXPECT_NE(msg.find("foo.lpkgtmp"), std::string::npos)
        << "本该因 tmp 路径是符号链接而拒绝，却成功了或错在别处";

    expect_victim_untouched("in-place 分支");
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/foo")) << "被拒 = 整批回滚，包文件不该留下";
    // **拒绝 ≠ 静默删掉别人的链接**：那条链接不是本包的东西，原样留在盘上（要修也得用户
    // 自己看见了再动手 —— 这也是把"拒绝"与"unlink 掉再继续"区分开的地方）
    EXPECT_TRUE(fs::is_symlink(tmp_link)) << "拒绝写入时把盘上原有的链接删掉了";
}

/** ② hook 脚本分支（真实安装入口）：hooks_dir/<pkg>/postinst.sh.lpkgtmp 是符号链接 */
TEST_F(TmpPathSymlinkGuardTest, HookTmpSymlinkIsRefused)
{
    const fs::path hook_tmp = Config::instance().hooks_dir() / "hook_pkg" / "postinst.sh.lpkgtmp";
    fs::create_directories(hook_tmp.parent_path());
    fs::create_symlink(victim.string(), hook_tmp);
    ASSERT_TRUE(fs::is_symlink(hook_tmp));

    const std::string pkg = pack("hook_pkg", "1.0", [](const fs::path& root, const fs::path& c) {
        write_file(c / "usr/bin/hook_pkg", "bin\n");
        write_file(root / "hooks/postinst.sh", "#!/bin/sh\necho hi\n");
    });

    std::string msg;
    try {
        install_packages({pkg});
    } catch (const LpkgException& e) {
        msg = e.what();
    }
    EXPECT_NE(msg.find("postinst.sh.lpkgtmp"), std::string::npos)
        << "本该因 hook 的 tmp 路径是符号链接而拒绝，却成功了或错在别处";

    expect_victim_untouched("hook 分支");
    // hook 脚本落位走同一个原语 —— 失败必须整批回滚，不在 hooks_dir 里留半成品
    EXPECT_FALSE(fs::is_regular_file(Config::instance().hooks_dir() / "hook_pkg" / "postinst.sh"));
}

/**
 * ③ 配置 `.lpkgnew` 分支（直连 `copy_package_files`）：三哈希三条互异 → 新内容落
 * `<配置>.lpkgnew`，而 `<配置>.lpkgnew.lpkgtmp` 是符号链接。
 *
 * 盘上配置被用户改过（"user edited"）+ 包内配置也变了（"pkg new"）+ 无旧记录 → SaveLpkgnew。
 */
TEST_F(TmpPathSymlinkGuardTest, LpkgnewTmpSymlinkIsRefused)
{
    write_file(test_root / "etc" / "foo", "user edited\n");
    const fs::path new_link = test_root / "etc" / "foo.lpkgnew.lpkgtmp";
    fs::create_symlink(victim.string(), new_link);
    ASSERT_TRUE(fs::is_symlink(new_link));

    const fs::path work = suite_work_dir / "_pkg_cfg_pkg";
    write_file(work / "content" / "etc" / "foo", "pkg new\n");
    json meta;
    meta[std::string(constants::J_NAME)] = "cfg_pkg";
    meta[std::string(constants::J_VERSION)] = "1.0";
    meta[std::string(constants::J_DEPS)] = json::array();
    meta[std::string(constants::J_PROVIDES)] = json::array();
    meta[std::string(constants::J_MAN)] = "";
    std::ofstream(work / "metadata.json") << meta.dump(2) << std::endl;

    InstallationTask task("cfg_pkg", "1.0", true);
    task.set_tmp_dir(work);

    std::string msg;
    try {
        task.copy_package_files();
    } catch (const LpkgException& e) {
        msg = e.what();
    }
    EXPECT_NE(msg.find("foo.lpkgnew.lpkgtmp"), std::string::npos)
        << "本该因 .lpkgnew 的 tmp 路径是符号链接而拒绝，却成功了或错在别处";

    expect_victim_untouched(".lpkgnew 分支");
    EXPECT_EQ(read_file(test_root / "etc" / "foo"), "user edited\n") << "用户改过的配置被动了";
}

/**
 * 边界（防止"修太狠"）：上一轮崩溃留下的 `.lpkgtmp` **普通文件**必须照旧被覆盖。
 *
 * 这是"先写 tmp 再 rename"这套原语的**正常残留形态**（崩在 copy 与 rename 之间），
 * 拒绝它会让一次崩溃后的所有重装永久失败 —— 故判据只能 `is_symlink`，不能 `exists`。
 */
TEST_F(TmpPathSymlinkGuardTest, StaleRegularLpkgtmpIsStillOverwritten)
{
    const fs::path stale = test_root / "usr" / "bin" / "foo.lpkgtmp";
    write_file(stale, "stale residue from a crashed run\n");
    ASSERT_FALSE(fs::is_symlink(stale));

    const std::string pkg = pack("foo_pkg2", "1.0", [](const fs::path&, const fs::path& c) {
        write_file(c / "usr/bin/foo", "NEW\n");
    });

    EXPECT_NO_THROW(install_packages({pkg})) << "普通文件的 .lpkgtmp 残留不该被拒绝";
    EXPECT_EQ(read_file(test_root / "usr/bin/foo"), "NEW\n");
    EXPECT_FALSE(fs::exists(stale)) << "残留的 .lpkgtmp 已被 rename 落位，不该还在";
}

/**
 * 包内 `hooks/` 的**符号链接**成员指向包外 ⇒ **整包拒绝**（并点名条目与它解析到的目标）。
 *
 * 2026-09-26 修：`directory_entry::is_regular_file()` 与随后的 `fs::copy` **都跟随末段
 * 链接** ⇒ `hooks/postinst.sh -> /etc/shadow` 会让 root 把**宿主那份文件的内容**拷成
 * `hooks_dir/<pkg>/postinst.sh`（mode 随源 + 执行位）并当 postinst **执行** ——
 * 包内容读出了包外、还以 root 执行。边界取"解析后仍落在**本包解压目录**之内"：
 * 包内互指是合法用法（照旧跟随复制），指到包外整包拒绝。
 */
TEST_F(TmpPathSymlinkGuardTest, HookSymlinkEscapingThePackageIsRefused)
{
    const std::string pkg = "hook_escape";
    const fs::path work = suite_work_dir / ("_pkg_" + pkg);
    write_file(work / "content" / "usr" / "bin" / pkg, "#!/bin/sh\n");
    // hooks/ 下放一个**指向包外**的符号链接
    fs::create_directories(work / "hooks");
    fs::create_symlink("/etc/shadow", work / "hooks" / "postinst.sh");
    ASSERT_TRUE(fs::is_symlink(work / "hooks" / "postinst.sh"));

    const std::string pkg_path = (pkg_dir / (pkg + "-1.0.lpkg")).string();
    pack_package(pkg_path, work.string(), pkg, "1.0", {}, {}, "man " + pkg, {});

    std::string msg;
    try {
        install_packages({pkg_path});
    } catch (const LpkgException& e) {
        msg = e.what();
    }
    ASSERT_FALSE(msg.empty()) << "包内 hook 链接指到包外 ⇒ 必须整包拒绝（否则以 root 把宿主"
                                 "那份文件的内容拷进 hooks 目录并执行）";
    std::cerr << "[hook-escape] 拒绝原文：" << msg << "\n";
    EXPECT_NE(msg.find("postinst.sh"), std::string::npos) << "报错没点名那个条目：" << msg;
    EXPECT_NE(msg.find("/etc/shadow"), std::string::npos)
        << "报错必须点名它**解析到的目标**（否则用户无从判断是包坏了还是自己配错）：" << msg;
    // 整批回滚：hooks 目录下不留半成品、包也没装上
    EXPECT_FALSE(fs::exists(Config::instance().hooks_dir() / pkg / "postinst.sh"));
    Cache::instance().load();
    EXPECT_NE(Cache::instance().get_installed_version(pkg), "1.0");
}

/** 对照（防"一律拒绝"）：包内**互指**的 hook 链接照旧工作 —— 今天的行为不变。 */
TEST_F(TmpPathSymlinkGuardTest, HookSymlinkInsideThePackageStillWorks)
{
    const std::string pkg = "hook_inside";
    const fs::path work = suite_work_dir / ("_pkg_" + pkg);
    write_file(work / "content" / "usr" / "bin" / pkg, "#!/bin/sh\n");
    fs::create_directories(work / "hooks");
    write_file(work / "hooks" / "real.sh", "#!/bin/sh\nexit 0\n");
    fs::create_symlink("real.sh", work / "hooks" / "postinst.sh");  // 包内互指

    const std::string pkg_path = (pkg_dir / (pkg + "-1.0.lpkg")).string();
    pack_package(pkg_path, work.string(), pkg, "1.0", {}, {}, "man " + pkg, {});

    std::string msg;
    try {
        install_packages({pkg_path});
    } catch (const LpkgException& e) {
        msg = e.what();
    }
    EXPECT_TRUE(msg.empty()) << "包内互指的 hook 链接是合法用法，不该被拒：" << msg;
    const fs::path installed = Config::instance().hooks_dir() / pkg / "postinst.sh";
    ASSERT_TRUE(fs::exists(installed)) << "包内链接的 hook 应当照旧落位";
    std::ifstream f(installed);
    const std::string got{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    EXPECT_NE(got.find("exit 0"), std::string::npos)
        << "跟随复制的语义不变（落位的是链接目标那份脚本的内容）：" << got;
}
