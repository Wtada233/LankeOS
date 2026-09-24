#include "config/cli.hpp"

#include <string>
#include <vector>

#include "base/utils.hpp"
#include "config/config.hpp"
#include "i18n/localization.hpp"

void register_cli_options(cxxopts::Options& options)
{
    // --- 安装/移除选项 ---
    // 选项名/默认值/帮助文本逐字搬自 main.cpp（帮助输出按注册顺序排版，顺序也不能变）。
    // `--overwrite` 是**取值**的向量选项：cxxopts 的向量语义 = 重复给出累积、按 ','
    // 切分（CXXOPTS_VECTOR_DELIMITER，默认 ','）—— 逗号切分与累积都靠它，测试钉住。
    options.add_options(get_string("help.group_install"))(
        "y,yes", get_string("help.yes_mode"), cxxopts::value<bool>()->default_value("false"))(
        "n,no", get_string("help.no_mode"), cxxopts::value<bool>()->default_value("false"))(
        "force", get_string("help.force"), cxxopts::value<bool>()->default_value("false"))(
        "purge-config", get_string("help.purge_config"),
        cxxopts::value<bool>()->default_value("false"))(
        "force-overwrite", get_string("help.force_overwrite"),
        cxxopts::value<bool>()->default_value("false"))("overwrite", get_string("help.overwrite"),
                                                        cxxopts::value<std::vector<std::string>>())(
        "no-hooks", get_string("help.no_hooks"), cxxopts::value<bool>()->default_value("false"))(
        "no-deps", get_string("help.no_deps"), cxxopts::value<bool>()->default_value("false"))(
        "missing-so-no-error", get_string("help.missing_so_no_error"),
        cxxopts::value<bool>()->default_value("false"))(
        "fsync", get_string("help.fsync"), cxxopts::value<bool>()->default_value("false"))(
        "use-system-soname", get_string("help.use_system_soname"),
        cxxopts::value<bool>()->default_value("false"))(
        "r,recursive", get_string("help.recursive"),
        cxxopts::value<bool>()->default_value("false"))("root", get_string("help.root_dir"),
                                                        cxxopts::value<std::string>())(
        "arch", get_string("help.target_arch"), cxxopts::value<std::string>())(
        "hash", get_string("help.hash"), cxxopts::value<std::string>());
}

void apply_cli_config(const cxxopts::ParseResult& result)
{
    // 覆盖豁免：可重复、逗号分隔（cxxopts 的 vector 语义：重复给出累积、按 ',' 切分）。
    // `--force-overwrite` 保留 = 在最前面追加 `'*'`（最宽松的兜底），于是与 `--overwrite`
    // 同给时不报错、后写的模式（含 `!` 取反）覆盖它：更具体的赢（见 compose 的注释）。
    {
        const bool force_all =
            result.count("force-overwrite") && result["force-overwrite"].as<bool>();
        std::vector<std::string> explicit_patterns;
        if (result.count("overwrite"))
            explicit_patterns = result["overwrite"].as<std::vector<std::string>>();
        if (force_all || !explicit_patterns.empty())
            Config::instance().set_overwrite_patterns(
                Config::compose_overwrite_patterns(force_all, explicit_patterns));
    }
    // 这个开关只管**批量文件数据**（包内容、.lpkgtmp/.lpkgnew 及其 rename 的父目录）：
    // 默认不 fsync（仍保证 rename 原子性 + kill/回滚语义，安装快），--fsync 才逐文件
    // fsync（代价每文件 2~6 次，大包安装慢一个数量级）。DB/元数据写与 WAL 行**始终**
    // fsync，不受它影响（理由见 DurableFsyncGuard / durable_fsync_enabled 的注释）。
    // 只在给定时**打开**，不给时不动（程序化设置——测试夹具、将来的配置文件——不该被一次
    // CLI 解析静默清掉）。
    if (result.count("fsync") && result["fsync"].as<bool>()) set_durable_fsync_enabled(true);
}
