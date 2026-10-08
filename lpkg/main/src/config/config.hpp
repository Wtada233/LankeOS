#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "base/constants.hpp"  // NonInteractiveMode（下移到 base，修 base↔config 成环）

/**
 * 全局配置单例：以 Meyer's Singleton 统一管理所有配置路径和运行模式。
 * `set_root_path()` 时自动重新计算所有派生路径；外部通过 const 访问器读取。
 */
class Config
{
public:
    static Config& instance();

    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    // --- 路径访问器 -------------------------------------------------------

    const std::filesystem::path& root_dir() const noexcept
    {
        return root_dir_;
    }
    const std::filesystem::path& config_dir() const noexcept
    {
        return config_dir_;
    }
    const std::filesystem::path& state_dir() const noexcept
    {
        return state_dir_;
    }
    const std::filesystem::path& l10n_dir() const noexcept
    {
        return l10n_dir_;
    }
    const std::filesystem::path& docs_dir() const noexcept
    {
        return docs_dir_;
    }
    const std::filesystem::path& lock_dir() const noexcept
    {
        return lock_dir_;
    }
    const std::filesystem::path& hooks_dir() const noexcept
    {
        return hooks_dir_;
    }

    const std::filesystem::path& dep_dir() const noexcept
    {
        return dep_dir_;
    }
    /// needed_so（DT_NEEDED SONAME 列表）目录。
    const std::filesystem::path& needed_so_dir() const noexcept
    {
        return needed_so_dir_;
    }
    const std::filesystem::path& pkgs_file() const noexcept
    {
        return pkgs_file_;
    }
    const std::filesystem::path& holdpkgs_file() const noexcept
    {
        return holdpkgs_file_;
    }
    const std::filesystem::path& essential_file() const noexcept
    {
        return essential_file_;
    }
    const std::filesystem::path& mirror_conf() const noexcept
    {
        return mirror_conf_;
    }
    const std::filesystem::path& triggers_conf() const noexcept
    {
        return triggers_conf_;
    }
    /// build.conf；缺失时回退 build_defaults.hpp 的内置默认。
    const std::filesystem::path& build_conf() const noexcept
    {
        return build_conf_;
    }
    const std::filesystem::path& files_db() const noexcept
    {
        return files_db_;
    }
    const std::filesystem::path& provides_db() const noexcept
    {
        return provides_db_;
    }
    /// **SONAME → 属主包** 的归属库（与 `provides.db` 并列的另一张表，见 8.0.0 的字段拆分）
    const std::filesystem::path& provides_soname_db() const noexcept
    {
        return provides_soname_db_;
    }
    /**
     * 配置文件哈希数据库（逻辑路径 → `"<pkg>:<sha256>"` 集合）。
     *
     * 记录"**这个包的这个版本往这个 /etc 路径里装过什么内容**"，供升级时的三哈希分流判定
     * `hash_orig`（用户改没改过）。**不能并进 files.db**：那里的取值是**属主包名集合**，
     * 被 `add_file_owner`（单一属主检查）/ `get_file_owners` 当属主集合直接读，混入哈希会
     * 污染所有权语义。
     */
    const std::filesystem::path& conf_hashes_db() const noexcept
    {
        return conf_hashes_db_;
    }
    /**
     * xattr 键归属数据库（`<逻辑路径>\x1f<base64 键>` → **属主包名集合**）。
     *
     * 记录"**哪个包在这个目录上声明过这个 xattr 键**"，供升级/移除时判断"这个键还该不该在"。
     * 为什么需要它（而不是照 `files.db` 那样只记路径）：xattr 是**按目录共用**的 —— 一个目录
     * 被多个包持有、每个包各自声明自己那几个键，所以撤销时的判据必须是"**这个键**还有没有
     * 别的属主"，而不是"这个目录还有没有别的属主"（后者会让 A 包撤掉 B 包的键）。
     *
     * **只记目录**（普通文件的 xattr 随"`.lpkgtmp` + rename"进新对象，不存在陈旧键问题）。
     *
     * **不能并进 `files.db`**：那里的键是**裸路径**，取值是属主集合，而这里每个目录会有
     * 多行（每个键一行）、键里还带 base64 —— 混在一起会让 `get_file_owners()` 把
     * `"<路径>\x1f<b64>"` 当成另一个路径。
     *
     * 键里的分隔符用 `\x1f`（US，控制字符）：归档成员名消毒已经挡掉了 `\n`/`\r`/`\0` 与
     * 字面 `" → "`，但**没挡控制字符**，所以写入侧还要额外拒绝"路径里含 `\x1f` 或 `\t`"
     * 的条目（`\t` 会破 DB 格式本身 —— 与 `files.db` 同一个约束）——见 cache 的
     * `add_xattr_key_owner()`。
     */
    const std::filesystem::path& xattr_keys_db() const noexcept
    {
        return xattr_keys_db_;
    }
    const std::filesystem::path& lock_file() const noexcept
    {
        return lock_file_;
    }

    // --- 模式访问器 -------------------------------------------------------

    NonInteractiveMode non_interactive_mode() const noexcept
    {
        return non_interactive_mode_;
    }
    void set_non_interactive_mode(NonInteractiveMode m) noexcept;

    // --- 覆盖豁免（pacman `--overwrite` 语义） ---------------------------------

    /// 空列表 = 什么都不豁免。
    bool overwrite_allows(const std::string& path) const;

    /// 来自 CLI 的 `--overwrite` 收集结果。
    void set_overwrite_patterns(std::vector<std::string> patterns);

    /**
     * 组装覆盖豁免模式列表：`--force-overwrite` 等价于在**最前面**追加 `'*'`——
     * 它排在最后被判定（pacman 倒序遍历），于是后给的 `--overwrite` 模式（含 `!` 取反）
     * 都能覆盖它：两者同给时更具体的赢，不报错。
     */
    static std::vector<std::string> compose_overwrite_patterns(
        bool force_overwrite, const std::vector<std::string>& explicit_patterns);

    /**
     * 向后兼容入口：`--force-overwrite` / 既有测试用的进程级开关。
     * `true` ≡ `set_overwrite_patterns({"*"})`，`false` ≡ 清空（什么都不豁免）。
     */
    void set_force_overwrite_mode(bool v) noexcept;

    bool no_hooks_mode() const noexcept
    {
        return no_hooks_mode_;
    }
    void set_no_hooks_mode(bool v) noexcept;

    bool no_deps_mode() const noexcept
    {
        return no_deps_mode_;
    }
    void set_no_deps_mode(bool v) noexcept;

    /// bootstrap/过渡期容忍。
    bool missing_so_no_error_mode() const noexcept
    {
        return missing_so_no_error_mode_;
    }
    void set_missing_so_no_error_mode(bool v) noexcept;

    /// backup 的旧 SONAME 装在 /usr/lib 时，用它满足 needed_so。
    bool use_system_soname_mode() const noexcept
    {
        return use_system_soname_mode_;
    }
    void set_use_system_soname_mode(bool v) noexcept;

    /// 系统 /usr/lib（或 lib64）是否已有该 SONAME 文件（如 ABI 过渡备份的旧 .so）。
    bool has_system_soname(const std::string& soname) const noexcept;

    bool testing_mode() const noexcept
    {
        return testing_mode_;
    }
    void set_testing_mode(bool v) noexcept;

    /**
     * **仅供测试**：把所有粘性开关与路径复位到"测试用的中性值"（见 .cpp 的定义处说明）。
     *
     * 存在的理由：这些都是**进程级单例**上的状态，一个用例改了、后面每个用例都看得见 ——
     * 症状是"单跑绿、全量红"。复位由测试的**全局 listener**（`tests/test_hygiene.hpp`）
     * 在每个用例结束时无条件调用，与 fixture 无关。
     */
    void reset_for_test();

    // --- 操作方法 ---------------------------------------------------------

    /// 同时重新计算所有派生路径。
    void set_root_path(const std::string& root_path);
    /// 创建必要目录和默认文件。
    void init_filesystem();

    void set_architecture(const std::string& arch);
    std::string get_architecture();

    std::string get_mirror_url();

    static std::filesystem::path get_tmp_dir();

private:
    Config();

    // --- 路径成员 ---------------------------------------------------------
    std::filesystem::path root_dir_;
    std::filesystem::path config_dir_;
    std::filesystem::path state_dir_;
    std::filesystem::path l10n_dir_;
    std::filesystem::path docs_dir_;
    std::filesystem::path lock_dir_;
    std::filesystem::path hooks_dir_;

    // 派生路径（由 rebase_paths() 重新计算）
    std::filesystem::path dep_dir_;
    std::filesystem::path needed_so_dir_;
    std::filesystem::path pkgs_file_;
    std::filesystem::path holdpkgs_file_;
    std::filesystem::path essential_file_;
    std::filesystem::path mirror_conf_;
    std::filesystem::path triggers_conf_;
    std::filesystem::path build_conf_;
    std::filesystem::path files_db_;
    std::filesystem::path provides_db_;
    std::filesystem::path provides_soname_db_;
    std::filesystem::path conf_hashes_db_;  // 升级三哈希分流的 hash_orig
    std::filesystem::path xattr_keys_db_;   // 撤销"本包不再声明"的键
    std::filesystem::path lock_file_;

    // --- 模式成员 ---------------------------------------------------------
    NonInteractiveMode non_interactive_mode_{NonInteractiveMode::INTERACTIVE};
    std::vector<std::string> overwrite_patterns_;  // 倒序判定（pacman 语义）
    bool no_hooks_mode_ = false;
    bool no_deps_mode_ = false;
    bool missing_so_no_error_mode_ = false;
    bool use_system_soname_mode_ = false;
    bool testing_mode_ = false;

    // --- 架构覆盖 ---------------------------------------------------------
    std::string architecture_override_;

    void rebase_paths();

    mutable std::mutex config_mutex_;
};
