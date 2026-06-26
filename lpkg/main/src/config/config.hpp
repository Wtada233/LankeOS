#pragma once

#include <string>
#include <filesystem>
#include <mutex>

/**
 * 非交互模式枚举
 * 控制是否以及如何自动响应用户提示
 */
enum class NonInteractiveMode {
    INTERACTIVE,  // 默认：交互式，等待用户输入
    YES,          // 自动回答"是"
    NO            // 自动回答"否"
};

/**
 * 全局配置单例
 *
 * 以 Meyer's Singleton 模式统一管理所有配置路径和运行模式。
 * 调用 set_root_path() 时自动重新计算所有派生路径，
 * 外部通过 const 访问器获取路径和模式状态。
 */
class Config {
public:
    /** 获取全局单例实例 */
    static Config& instance();

    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    // --- 路径访问器 -------------------------------------------------------

    /** 根目录 */
    const std::filesystem::path& root_dir()        const noexcept { return root_dir_; }
    /** 配置文件目录 */
    const std::filesystem::path& config_dir()      const noexcept { return config_dir_; }
    /** 状态数据目录 */
    const std::filesystem::path& state_dir()       const noexcept { return state_dir_; }
    /** 本地化文件目录 */
    const std::filesystem::path& l10n_dir()        const noexcept { return l10n_dir_; }
    /** 文档目录 */
    const std::filesystem::path& docs_dir()        const noexcept { return docs_dir_; }
    /** 锁文件存放目录 */
    const std::filesystem::path& lock_dir()        const noexcept { return lock_dir_; }
    /** 钩子脚本存放目录 */
    const std::filesystem::path& hooks_dir()       const noexcept { return hooks_dir_; }

    /** 依赖信息目录 */
    const std::filesystem::path& dep_dir()         const noexcept { return dep_dir_; }
    /** 已安装包列表文件 */
    const std::filesystem::path& pkgs_file()       const noexcept { return pkgs_file_; }
    /** 锁定包列表文件 */
    const std::filesystem::path& holdpkgs_file()   const noexcept { return holdpkgs_file_; }
    /** 核心包列表文件 */
    const std::filesystem::path& essential_file()  const noexcept { return essential_file_; }
    /** 镜像配置文件 */
    const std::filesystem::path& mirror_conf()     const noexcept { return mirror_conf_; }
    /** 触发器配置文件 */
    const std::filesystem::path& triggers_conf()   const noexcept { return triggers_conf_; }
    /** 文件归属数据库 */
    const std::filesystem::path& files_db()        const noexcept { return files_db_; }
    /** providers 数据库 */
    const std::filesystem::path& provides_db()     const noexcept { return provides_db_; }
    /** 互斥锁文件路径 */
    const std::filesystem::path& lock_file()       const noexcept { return lock_file_; }

    // --- 模式访问器 -------------------------------------------------------

    /** 获取非交互模式设置 */
    NonInteractiveMode non_interactive_mode() const noexcept { return non_interactive_mode_; }
    /** 设置非交互模式 */
    void set_non_interactive_mode(NonInteractiveMode m) noexcept;

    /** 是否强制覆盖文件 */
    bool force_overwrite_mode() const noexcept { return force_overwrite_mode_; }
    /** 设置强制覆盖模式 */
    void set_force_overwrite_mode(bool v) noexcept;

    /** 是否禁用钩子 */
    bool no_hooks_mode() const noexcept { return no_hooks_mode_; }
    /** 设置禁用钩子模式 */
    void set_no_hooks_mode(bool v) noexcept;

    /** 是否跳过依赖处理 */
    bool no_deps_mode() const noexcept { return no_deps_mode_; }
    /** 设置跳过依赖模式 */
    void set_no_deps_mode(bool v) noexcept;

    /** 是否测试模式（用于构建测试） */
    bool testing_mode() const noexcept { return testing_mode_; }
    /** 设置测试模式 */
    void set_testing_mode(bool v) noexcept;

    // --- 操作方法 ---------------------------------------------------------

    /** 设置根路径，同时重新计算所有派生路径 */
    void set_root_path(const std::string& root_path);
    /** 初始化文件系统结构（创建必要目录和默认文件） */
    void init_filesystem();

    /** 设置架构覆盖值 */
    void set_architecture(const std::string& arch);
    /** 获取目标架构 */
    std::string get_architecture();

    /** 获取镜像源 URL */
    std::string get_mirror_url();

    /** 获取系统临时目录路径 */
    static std::filesystem::path get_tmp_dir();

private:
    Config();

    // --- 路径成员 ---------------------------------------------------------
    std::filesystem::path root_dir_;          // 根路径
    std::filesystem::path config_dir_;        // 配置目录
    std::filesystem::path state_dir_;         // 状态数据目录
    std::filesystem::path l10n_dir_;          // 本地化目录
    std::filesystem::path docs_dir_;          // 文档目录
    std::filesystem::path lock_dir_;          // 锁目录
    std::filesystem::path hooks_dir_;         // 钩子目录

    // 派生路径（由 rebase_paths() 重新计算）
    std::filesystem::path dep_dir_;            // 依赖信息目录
    std::filesystem::path pkgs_file_;          // 已安装包列表文件
    std::filesystem::path holdpkgs_file_;      // 锁定包列表文件
    std::filesystem::path essential_file_;     // 核心包列表文件
    std::filesystem::path mirror_conf_;        // 镜像配置文件
    std::filesystem::path triggers_conf_;      // 触发器配置文件
    std::filesystem::path files_db_;           // 文件归属数据库
    std::filesystem::path provides_db_;        // providers 数据库
    std::filesystem::path lock_file_;          // 互斥锁文件

    // --- 模式成员 ---------------------------------------------------------
    NonInteractiveMode non_interactive_mode_{NonInteractiveMode::INTERACTIVE};  // 非交互模式
    bool force_overwrite_mode_ = false;  // 强制覆盖
    bool no_hooks_mode_ = false;         // 禁用钩子
    bool no_deps_mode_ = false;          // 跳过依赖
    bool testing_mode_ = false;          // 测试模式

    // --- 架构覆盖 ---------------------------------------------------------
    std::string architecture_override_;  // 架构覆盖值（可选）

    /** 重新计算所有派生路径 */
    void rebase_paths();

    mutable std::mutex config_mutex_;
};
