#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "../../main/src/i18n/localization.hpp"

namespace fs = std::filesystem;

class LocalizationTest : public ::testing::Test
{
protected:
    fs::path l10n_dir;

    void SetUp() override
    {
        l10n_dir = fs::current_path() / "tmp_l10n_test";
        fs::remove_all(l10n_dir);
        fs::create_directories(l10n_dir);
        init_localization();
    }

    void TearDown() override
    {
        fs::remove_all(l10n_dir);
    }
};

TEST_F(LocalizationTest, GetStringExistingKey)
{
    std::string key = "info.parsing_lankebuild";
    auto result = get_string(key);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.find("[MISSING_STRING:") == std::string::npos);
}

TEST_F(LocalizationTest, GetStringMissingKey)
{
    std::string missing_key = "nonexistent_key_xyz";
    auto result = get_string(missing_key);
    EXPECT_EQ(result, "[MISSING_STRING: nonexistent_key_xyz]");

    auto result2 = get_string(missing_key);
    EXPECT_EQ(result2, "[MISSING_STRING: nonexistent_key_xyz]");
}

TEST_F(LocalizationTest, StringFormatNormal)
{
    // 正常格式化
    std::string result = string_format("info.building_package", "test-pkg", "1.0.0");
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.find("[MISSING_STRING:") == std::string::npos);
}

TEST_F(LocalizationTest, StringFormatMismatchedArgs)
{
    // 实参**少于**占位符个数（info.building_package 需要两个 {}，这里只给一个）。
    // `string_format` 走 std::vformat（运行时格式串）：实参不足 ⇒ std::format_error ⇒
    // 被 catch ⇒ 返回 "Lpkg Formatting Error [key: <key>]: <what>"。
    // 此前只断言 EXPECT_FALSE(result.empty()) —— 恒真废话。这里钉**真实形态**：
    std::string result = string_format("info.building_package", "only_one_arg_but_needs_two");

    // ① 明确是格式错误这一支（而不是"返回原模板/静默补空"）。用前缀匹配，不假定具名 what() 文案。
    EXPECT_EQ(result.rfind("Lpkg Formatting Error", 0), 0u)
        << "实参不匹配应走 format_error 分支。实际输出：\n"
        << result;
    // ② 错误信息点名了**哪个 key**（否则定位不了是哪个模板写错）
    EXPECT_NE(result.find("info.building_package"), std::string::npos)
        << "格式错误信息应点名出错的 key。实际输出：\n"
        << result;
}

TEST_F(LocalizationTest, NonExistentLocaleFallsBackToEnglish)
{
    // 原名 `NonExistentLocaleFallsBack` 与 `GetStringExistingKey` **逐字重复**、且从不触碰
    // locale（名不副实：它根本没测任何"降级"）。改为真的测「不存在的 locale」那条路：
    // LANG 既不以 "zh" 开头、也不是 "en" → init_localization 选英文语言包
    // （localization.cpp:84 的 `lang = "en"` 默认），即"未知 locale 回退英文"，
    // 而不是吐 `[MISSING_STRING: …]`。
    const char* saved = std::getenv("LANG");
    const std::string saved_lang = saved ? std::string(saved) : std::string();
    ::setenv("LANG", "fr_FR.UTF-8", 1);
    init_localization();

    const std::string msg = get_string("info.building_package");
    // 区分性判据：en 模板 = "Building package: {} version: {}"；zh 模板 = "正在构建软件包: …"。
    // 若回退没发生（吐占位符）、或错误地选成了 zh，本条都会红。
    EXPECT_NE(msg.find("Building package"), std::string::npos)
        << "未知 locale 应回退到英文语言包，实际：\n"
        << msg;
    EXPECT_EQ(msg.find("正在构建"), std::string::npos) << "未知 locale 不该选中中文语言包，实际：\n"
                                                       << msg;

    // 翻译表是进程级全局 —— 用完还原 LANG 并重载，别污染后面的用例。
    if (saved)
        ::setenv("LANG", saved_lang.c_str(), 1);
    else
        ::unsetenv("LANG");
    init_localization();
}

TEST_F(LocalizationTest, MultipleMissingKeys)
{
    auto r1 = get_string("missing_key_1");
    auto r2 = get_string("missing_key_2");
    auto r3 = get_string("missing_key_3");
    EXPECT_NE(r1, r2);
    EXPECT_NE(r2, r3);
}

TEST_F(LocalizationTest, EnglishPackageVersionNotFoundIsNotMisleading)
{
    // 调用点是 (name, vspec, avail)（solver.cpp）。旧英文模板 `Version {} of {} not found…`
    // 渲染成 "Version curl of 8.11.1 …" —— 占位符位置**没反**（name 仍落在第一个），只是这种
    // 措辞读起来像"版本号叫 curl"，容易被误读；改成 "Package curl has no version 8.11.1…"。
    // 这是**可读性**改动，不是修 bug，所以断言必须能区分新旧（见下），否则就是恒真废话。
    //
    // 必须**强制英文**：zh 模板一直是清楚的，不强制就测不到英文那一侧（空转）。
    const char* saved = std::getenv("LANG");
    const std::string saved_lang = saved ? std::string(saved) : std::string();
    ::setenv("LANG", "en_US.UTF-8", 1);
    init_localization();

    const std::string msg =
        string_format("error.package_version_not_found", "curl", "8.11.1", "8.5, 8.10");
    // 区分性判据（旧模板下**会红**，新模板下绿）：英文消息里不得出现 "Version <包名>"
    // 这种"版本号 = 包名"的读法。仅断言"包名在版本之前"是空转 —— 新旧两种措辞都满足。
    EXPECT_EQ(msg.find("Version curl"), std::string::npos)
        << "英文消息把包名挂在 'Version ' 之后，读起来像版本号叫 curl：\n"
        << msg;
    EXPECT_NE(msg.find("curl"), std::string::npos) << msg;
    EXPECT_NE(msg.find("8.11.1"), std::string::npos) << msg;

    // 翻译表是进程级全局 —— 用完还原 LANG 并重载，别污染后面的用例。
    if (saved)
        ::setenv("LANG", saved_lang.c_str(), 1);
    else
        ::unsetenv("LANG");
    init_localization();
}
