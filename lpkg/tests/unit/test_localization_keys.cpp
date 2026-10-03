#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>

#include "localization.hpp"

namespace fs = std::filesystem;

namespace
{
/** 一个 l10n 文件里的键集合（每行 `key=value`；空行与 `#` 开头跳过） */
std::set<std::string> keys_of_l10n_file(const fs::path& p)
{
    std::set<std::string> keys;
    std::ifstream f(p);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto pos = line.find('=');
        if (pos == std::string::npos || pos == 0) continue;
        keys.insert(line.substr(0, pos));
    }
    return keys;
}

/**
 * 树里出现过的**全部字符串字面量**（内容集合）。
 *
 * 比 `extract_keys_from_source` 更宽松 —— 它不要求字面量紧跟 `get_string(`/`string_format(`：
 * 键经常是**当参数传**的（`reject("error.unsafe_member_control")`、
 * `log_summary("info.remove_summary", …)`、`print_depend_trees(args, "info.depend_remove_header",
 * …)`）， 那种写法前缀正则一个都匹配不到（2026-10-03 实测：按前缀正则做"死键"检查会误报 10
 * 个在用键）。
 *
 * ⚠️ 代价：**注释里提到某个键也算"用过"**。这对"死键"检查够用 —— 它要抓的是
 * "删了功能却把 l10n 行整条留下、全仓零引用"这种情形。
 */
std::set<std::string> all_string_literals_in(const fs::path& dir)
{
    std::set<std::string> lits;
    const std::regex lit_regex("\"([^\"\\n]*)\"");
    for (const auto& de : fs::recursive_directory_iterator(dir)) {
        if (!de.is_regular_file()) continue;
        const auto ext = de.path().extension();
        if (ext != ".cpp" && ext != ".hpp" && ext != ".py" && ext != ".sh") continue;
        std::ifstream f(de.path());
        const std::string content((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
        for (auto it = std::sregex_iterator(content.begin(), content.end(), lit_regex);
             it != std::sregex_iterator(); ++it) {
            lits.insert((*it)[1].str());
        }
    }
    return lits;
}

/** 测试树的根：优先 cwd，其次上一级（兼容从 build/ 里跑） */
fs::path l10n_project_root()
{
    fs::path root = fs::current_path();
    if (!fs::exists(root / "main/src")) root = root.parent_path();
    return root;
}
}  // namespace

class L10nIntegrityTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        init_localization();
    }

    std::set<std::string> extract_keys_from_source(const fs::path& src_dir)
    {
        std::set<std::string> keys;
        // Search for string literals used in localization functions
        // Using a more conservative regex string
        std::string pattern =
            "(?:get_string|log_info|log_error|log_warning|string_format)\\s*\\(\\s*\"([^\"]+)\"";
        std::regex key_regex(pattern);

        for (auto const& dir_entry : fs::recursive_directory_iterator(src_dir)) {
            if (dir_entry.is_regular_file() && (dir_entry.path().extension() == ".cpp" ||
                                                dir_entry.path().extension() == ".hpp")) {
                std::ifstream f(dir_entry.path());
                std::string content((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
                auto words_begin = std::sregex_iterator(content.begin(), content.end(), key_regex);
                auto words_end = std::sregex_iterator();
                for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
                    keys.insert((*i)[1].str());
                }
            }
        }
        return keys;
    }
};

TEST_F(L10nIntegrityTest, AllSourceKeysExistInTranslations)
{
    fs::path project_root = fs::current_path();
    // Support running from build directory
    if (!fs::exists(project_root / "main/src")) {
        project_root = project_root.parent_path();
    }

    auto source_keys = extract_keys_from_source(project_root / "main/src");

    // Explicitly add keys that might be missed by simple regex (e.g. multi-line or macros)
    std::vector<std::string> manual_keys = {
        "help.output_file", "help.pack_source",    "help.force",     "help.force_overwrite",
        "help.overwrite",   "help.no_hooks",       "help.no_deps",   "help.root_dir",
        "help.target_arch", "help.hash",           "help.pkg_query", "cxxopts.default",
        "cxxopts.usage",    "cxxopts.option_help", "cxxopts.arg",    "cxxopts.positional_help"};
    for (const auto& k : manual_keys) source_keys.insert(k);

    std::vector<std::string> missing_keys;
    for (const auto& key : source_keys) {
        // Skip common false positives if any
        if (key == "command" || key == "packages") continue;

        std::string val = get_string(key);
        if (val.find("[MISSING_STRING:") != std::string::npos) {
            missing_keys.push_back(key);
        }
    }

    std::string error_msg = "The following keys are missing in localization files: ";
    for (const auto& k : missing_keys) error_msg += k + ", ";

    EXPECT_TRUE(missing_keys.empty()) << error_msg;
}
/**
 * **反向检查：不许有"死键"**。
 *
 * 删功能/换文案时很容易只把调用点删掉、把 l10n 行留下：它们不会报错、不会显示，只会**永远**
 * 躺在两个语言文件里，让人误以为"这个键在某个分支上用着"。2026-10-03 一次扫出 22 个这样的
 * 遗留（`info.rollback_debug_*`、`error.upgrade_failed`、`warning.circular_dependency` …——
 * 全是**手动解析依赖时代**的残骸），外加一个只存在于 zh 的行。
 *
 * 判据：**字面量 `"key"` 在这个树里出现过**（见 `all_string_literals_in` —— 不要求紧跟
 * `get_string(`，因为键常常是当参数传的）。扫描范围是整个 `main/`（不只是 `main/src`）——
 * `cxxopts.*` 那几个键由 `main/third_party` 里的 cxxopts 调用。
 */
TEST_F(L10nIntegrityTest, NoOrphanKeysInTranslations)
{
    const fs::path root = l10n_project_root();
    auto used = all_string_literals_in(root / "main");
    const auto used_in_tests = all_string_literals_in(root / "tests");
    used.insert(used_in_tests.begin(), used_in_tests.end());

    std::vector<std::string> orphans;
    for (const auto& key : keys_of_l10n_file(root / "main/l10n/en.txt")) {
        if (!used.contains(key)) orphans.push_back(key);
    }

    std::string msg = "以下 l10n 键没有任何地方引用（删功能时留下的死键）：";
    for (const auto& k : orphans) msg += "\n  " + k;
    EXPECT_TRUE(orphans.empty()) << msg;
}

/**
 * **中英键集合必须一致**（少一个 key 会让那种语言悄悄退化成 `[MISSING_STRING: …]`，
 * 而英文兜底会让它更难被发现）。
 *
 * 2026-10-03 实测存在一条只写在 `zh.txt` 的 `error.installation_failed_rolling_back`
 * （代码里从未引用过），已删。
 */
TEST_F(L10nIntegrityTest, EnglishAndChineseHaveIdenticalKeySets)
{
    const fs::path root = l10n_project_root();
    const auto en = keys_of_l10n_file(root / "main/l10n/en.txt");
    const auto zh = keys_of_l10n_file(root / "main/l10n/zh.txt");

    std::string only_en;
    for (const auto& k : en)
        if (!zh.contains(k)) only_en += "\n  只在 en: " + k;
    std::string only_zh;
    for (const auto& k : zh)
        if (!en.contains(k)) only_zh += "\n  只在 zh: " + k;

    EXPECT_TRUE(only_en.empty() && only_zh.empty()) << only_en << only_zh;
}
