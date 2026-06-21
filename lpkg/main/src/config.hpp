#pragma once

#include <string>
#include <filesystem>

// =====================================================================
// NonInteractiveMode — moved here from utils.hpp
// =====================================================================
enum class NonInteractiveMode {
    INTERACTIVE,  // default: prompt the user
    YES,          // auto-answer yes to all prompts
    NO            // auto-answer no to all prompts
};

// =====================================================================
// Config — global configuration singleton
//
// Replaces 14 mutable path globals + 5 mode globals with a clean
// Meyer's singleton.  All paths are rebased when set_root_path() is
// called; every consumer accesses them through const accessors.
// =====================================================================
class Config {
public:
    static Config& instance();

    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    // --- Path accessors ---------------------------------------------------
    const std::filesystem::path& root_dir()        const noexcept { return root_dir_; }
    const std::filesystem::path& config_dir()      const noexcept { return config_dir_; }
    const std::filesystem::path& state_dir()       const noexcept { return state_dir_; }
    const std::filesystem::path& l10n_dir()        const noexcept { return l10n_dir_; }
    const std::filesystem::path& docs_dir()        const noexcept { return docs_dir_; }
    const std::filesystem::path& lock_dir()        const noexcept { return lock_dir_; }
    const std::filesystem::path& hooks_dir()       const noexcept { return hooks_dir_; }

    const std::filesystem::path& dep_dir()         const noexcept { return dep_dir_; }
    const std::filesystem::path& pkgs_file()       const noexcept { return pkgs_file_; }
    const std::filesystem::path& holdpkgs_file()   const noexcept { return holdpkgs_file_; }
    const std::filesystem::path& essential_file()  const noexcept { return essential_file_; }
    const std::filesystem::path& mirror_conf()     const noexcept { return mirror_conf_; }
    const std::filesystem::path& triggers_conf()   const noexcept { return triggers_conf_; }
    const std::filesystem::path& files_db()        const noexcept { return files_db_; }
    const std::filesystem::path& provides_db()     const noexcept { return provides_db_; }
    const std::filesystem::path& lock_file()       const noexcept { return lock_file_; }

    // --- Mode accessors ---------------------------------------------------
    NonInteractiveMode non_interactive_mode() const noexcept { return non_interactive_mode_; }
    void set_non_interactive_mode(NonInteractiveMode m) noexcept { non_interactive_mode_ = m; }

    bool force_overwrite_mode() const noexcept { return force_overwrite_mode_; }
    void set_force_overwrite_mode(bool v) noexcept { force_overwrite_mode_ = v; }

    bool no_hooks_mode() const noexcept { return no_hooks_mode_; }
    void set_no_hooks_mode(bool v) noexcept { no_hooks_mode_ = v; }

    bool no_deps_mode() const noexcept { return no_deps_mode_; }
    void set_no_deps_mode(bool v) noexcept { no_deps_mode_ = v; }

    bool testing_mode() const noexcept { return testing_mode_; }
    void set_testing_mode(bool v) noexcept { testing_mode_ = v; }

    // --- Operations -------------------------------------------------------
    void set_root_path(const std::string& root_path);
    void init_filesystem();

    void set_architecture(const std::string& arch);
    std::string get_architecture();

    std::string get_mirror_url();

    static std::filesystem::path get_tmp_dir();

private:
    Config();

    // --- Path members -----------------------------------------------------
    std::filesystem::path root_dir_;
    std::filesystem::path config_dir_;
    std::filesystem::path state_dir_;
    std::filesystem::path l10n_dir_;
    std::filesystem::path docs_dir_;
    std::filesystem::path lock_dir_;
    std::filesystem::path hooks_dir_;

    // Derived paths  (recomputed by rebase_paths)
    std::filesystem::path dep_dir_;
    std::filesystem::path pkgs_file_;
    std::filesystem::path holdpkgs_file_;
    std::filesystem::path essential_file_;
    std::filesystem::path mirror_conf_;
    std::filesystem::path triggers_conf_;
    std::filesystem::path files_db_;
    std::filesystem::path provides_db_;
    std::filesystem::path lock_file_;

    // --- Mode members -----------------------------------------------------
    NonInteractiveMode non_interactive_mode_{NonInteractiveMode::INTERACTIVE};
    bool force_overwrite_mode_ = false;
    bool no_hooks_mode_ = false;
    bool no_deps_mode_ = false;
    bool testing_mode_ = false;

    // --- Architecture override --------------------------------------------
    std::string architecture_override_;

    void rebase_paths();
};
