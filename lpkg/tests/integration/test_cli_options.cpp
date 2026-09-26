/**
 * test_cli_options.cpp — CLI 解析层 → Config 的接线覆盖（本轮新增的 4 个开关）
 *
 * 背景：`--purge-config` / `--force-overwrite` / `--overwrite` / `--fsync` 此前只有
 * "绕过 CLI" 的用例（直接调 `Config::set_overwrite_patterns` /
 * `Config::compose_overwrite_patterns`），**解析层本身零覆盖**。于是选项名、或
 * `result.count("…")` 里的字符串 typo（如 `"overwrit"`）不会让任何用例变红：选项注册成
 * 一个没人引用的名字后功能静默失效，而全套测试仍绿。count() 对未知键只是返回 0 —— 连
 * 异常都没有，纯粹静默。
 *
 * 本文件用**真实 argv 数组**驱动 `register_cli_options` + `apply_cli_config`（两者在
 * main/src/config/cli.cpp 里，见那里的"为什么单独一个翻译单元"注释），钉住：
 *   ① 注册名与类型：`--overwrite` 是取值的向量选项、其余三个是布尔开关；
 *   ② 向量语义：同一次给逗号分隔的多个模式、重复给出累积（cxxopts 的
 *      CXXOPTS_VECTOR_DELIMITER 切分 + 逐次 append）；
 *   ③ 优先级：`--force-overwrite` ≡ 最前面追加 `'*'`，被后写的 `--overwrite`（含 `!` 取反）
 *      覆盖 —— 且**与给出顺序无关**；
 *   ④ 布尔默认值：不给 `--purge-config` / `--fsync` 时为 false。
 *
 * 覆盖边界（明确不测、别当已覆盖）：`--purge-config` → `remove_packages(..., purge_config)`
 * 的**派发**在 main/src/main_cli.cpp 的 handle_command 命令分支里。
 * （订正 2026-09-26：此处原写"测试二进制够不到，因为 main.cpp 有自己的 main()"—— 那个
 * 理由已不成立：分派路径随 main_cli.cpp 进了 LPKG_OBJS ⇒ 进了测试二进制，端到端的
 * 分派层用例在 tests/unit/test_cli_dispatch.cpp。本套件仍然只钉"能被解析、默认值可
 * 无条件读"——而这正是那行派发代码（`result["purge-config"].as<bool>()`，不带 count
 * 判断）的前提，两者互补不重复。）
 *
 * 夹具：覆盖豁免模式与 durable fsync 都是**进程级**全局状态（别的套件依赖其默认值：
 * test_overwrite_globs.cpp 从"什么都不豁免"起、test_durable_fsync_db.cpp 依赖默认关），
 * 所以本套件自取自还，不给 shuffle 留污染。
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/cli.hpp"
#include "../../main/src/config/config.hpp"
#include "../test_base.hpp"

namespace
{

/**
 * 用真实 argv 数组走一遍解析（等价 main() 里的 `options.parse(argc, argv)`）。
 *
 * 只注册 install/remove 组选项（本轮 4 个开关都在这一组）。main_cli.cpp 随后注册的位置参数
 * （command/packages）不在这里：本套件测的是"选项名能被识别 + 解析结果落到 Config"，
 * 位置参数与这两件事无关（且不注册位置参数时 cxxopts 只把它们收进 unmatched，不报错）。
 */
cxxopts::ParseResult parse_cli(const std::vector<std::string>& args)
{
    // cxxopts 只读 argv（`const char* const*`），但签名要可写指针 → 从可写副本取地址。
    std::vector<std::string> storage = args;
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& a : storage) argv.push_back(a.data());

    cxxopts::Options options("lpkg");
    register_cli_options(options);
    return options.parse(static_cast<int>(argv.size()), argv.data());
}

/** 一步到位：argv → 解析 → 落到 Config（等价 main() 里 parse 之后的那段接线） */
void apply_cli(const std::vector<std::string>& args)
{
    apply_cli_config(parse_cli(args));
}

}  // namespace

class CliOptionsTest : public IntegrationTestBase
{
protected:
    void SetUp() override
    {
        IntegrationTestBase::SetUp();  // 内含 init_localization()：真帮助文本而不是占位符

        saved_fsync_ = durable_fsync_enabled();
        set_durable_fsync_enabled(false);
        Config::instance().set_overwrite_patterns({});
    }

    void TearDown() override
    {
        Config::instance().set_overwrite_patterns({});
        set_durable_fsync_enabled(saved_fsync_);
        IntegrationTestBase::TearDown();
    }

    /** 批量判定一批路径（用于"两种写法/两种顺序必须等价"的对照） */
    static std::vector<int> allows(const std::vector<std::string>& paths)
    {
        std::vector<int> out;
        out.reserve(paths.size());
        for (const auto& p : paths) out.push_back(Config::instance().overwrite_allows(p) ? 1 : 0);
        return out;
    }

    bool saved_fsync_ = false;
};

// ── `--overwrite`：逗号切分 + 可重复累积 + `!` 取反 ─────────────────────────────

TEST_F(CliOptionsTest, OverwriteSplitsOnCommaAndAccumulatesRepeats)
{
    auto result = parse_cli({"lpkg", "--overwrite", "a,b", "--overwrite", "!c"});
    // 两次给出 = 两个槽位（cxxopts 的向量语义：逐次 append，不是"后者覆盖前者"）
    EXPECT_EQ(result.count("overwrite"), 2u);
    EXPECT_EQ(result["overwrite"].as<std::vector<std::string>>(),
              (std::vector<std::string>{"a", "b", "!c"}));

    apply_cli_config(result);
    const auto& cfg = Config::instance();
    EXPECT_TRUE(cfg.overwrite_allows("a"));   // 逗号切分出的第 1 个
    EXPECT_TRUE(cfg.overwrite_allows("b"));   // 逗号切分出的第 2 个
    EXPECT_FALSE(cfg.overwrite_allows("c"));  // '!' 取反 = 明确不豁免
    // "开了就全局放行"是错的语义：未命中模式的路径照旧不放行
    EXPECT_FALSE(cfg.overwrite_allows("d"));
}

TEST_F(CliOptionsTest, OverwriteSplitsCommaAndNegationInsideOneSlot)
{
    // 同一个槽位里既有逗号又有取反：切分出的 `!keep` 仍是取反模式（前缀没被切丢）
    apply_cli({"lpkg", "--overwrite", "/usr/share/x/*,!/usr/share/x/keep"});
    const auto& cfg = Config::instance();
    EXPECT_TRUE(cfg.overwrite_allows("/usr/share/x/a"));
    EXPECT_FALSE(cfg.overwrite_allows("/usr/share/x/keep"));
}

// ── `--force-overwrite`：`'*'` 优先级最低、后写覆盖它、与顺序无关 ────────────────

TEST_F(CliOptionsTest, ForceOverwriteIsLowestPriorityFallback)
{
    apply_cli({"lpkg", "--force-overwrite", "--overwrite", "!x"});
    const auto& cfg = Config::instance();
    // 后写的 `!x` 压过兜底的 `'*'`（'*' 排在列表最前 → 倒序判定时最后被看）
    EXPECT_FALSE(cfg.overwrite_allows("x"));
    EXPECT_TRUE(cfg.overwrite_allows("y"));
    EXPECT_TRUE(cfg.overwrite_allows("/usr/share/z/file"));
}

TEST_F(CliOptionsTest, ForceOverwriteAndOverwriteAreOrderIndependent)
{
    const std::vector<std::string> paths = {"x", "y", "usr/share/z", "/usr/bin/foo"};

    apply_cli({"lpkg", "--force-overwrite", "--overwrite", "!x"});
    const auto force_first = allows(paths);

    apply_cli({"lpkg", "--overwrite", "!x", "--force-overwrite"});
    const auto overwrite_first = allows(paths);

    // `--force-overwrite` 的 `'*'` 由 compose 固定在**最前面**，与两者在 argv 里的先后无关
    EXPECT_EQ(force_first, overwrite_first);
    EXPECT_EQ(force_first, (std::vector<int>{0, 1, 1, 1}));  // 只有 x 被取反拒掉
}

TEST_F(CliOptionsTest, ForceOverwriteAloneEqualsStarPattern)
{
    const std::vector<std::string> paths = {"a", "usr/share/x", "/usr/bin/foo", "/etc/keep"};

    apply_cli({"lpkg", "--force-overwrite"});
    const auto force_mode = allows(paths);

    apply_cli({"lpkg", "--overwrite", "*"});
    const auto star_mode = allows(paths);

    // 帮助文本承诺的 "--force-overwrite（等价于 --overwrite '*'）"：行为必须逐路径一致
    EXPECT_EQ(force_mode, star_mode);
    EXPECT_EQ(force_mode, (std::vector<int>{1, 1, 1, 1}));  // 全部放行
}

// ── `--fsync` / `--purge-config`：布尔开关的注册与默认值 ────────────────────────

TEST_F(CliOptionsTest, FsyncFlagTurnsOnDurableFsync)
{
    set_durable_fsync_enabled(false);
    apply_cli({"lpkg"});  // 不给开关
    EXPECT_FALSE(durable_fsync_enabled());

    apply_cli({"lpkg", "--fsync"});
    EXPECT_TRUE(durable_fsync_enabled());
}

TEST_F(CliOptionsTest, FsyncFlagOnlyTurnsOnAndNeverResets)
{
    // 全局只有一个 durable fsync 开关：CLI **只在给定时打开**，不给时不动它
    // （程序化设置——测试夹具、将来的配置文件——不该被一次 CLI 解析静默清掉）
    set_durable_fsync_enabled(true);
    apply_cli({"lpkg"});
    EXPECT_TRUE(durable_fsync_enabled());
}

TEST_F(CliOptionsTest, PurgeConfigIsRegisteredAndDefaultsToFalse)
{
    auto given = parse_cli({"lpkg", "--purge-config"});
    EXPECT_EQ(given.count("purge-config"), 1u);
    EXPECT_TRUE(given["purge-config"].as<bool>());

    // handle_command 对它**无条件** as<bool>()（不看 count）→ 不给时也必须能读且为 false。
    // 默认值注册丢了/名字打错，这里会抛 requested_option_not_present 或 no_such_option。
    auto defaulted = parse_cli({"lpkg"});
    EXPECT_EQ(defaulted.count("purge-config"), 0u);
    EXPECT_FALSE(defaulted["purge-config"].as<bool>());
}

TEST_F(CliOptionsTest, MisspelledOptionIsRejectedAtParseTime)
{
    // 本套件存在的理由：注册名 typo 必须在**解析层**炸出来，而不是"注册成一个没人引用的
    // 名字、功能静默失效而全套测试仍绿"。这里用故意写错的 `--overwrit` 钉住该性质：
    // 只要 register_cli_options 里的名字对得上，未注册的名字就必然被拒。
    EXPECT_THROW(parse_cli({"lpkg", "--overwrit", "a"}), cxxopts::exceptions::no_such_option);
}
