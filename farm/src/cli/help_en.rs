//! clap 帮助文本的英文覆盖表（纯数据；`LANG=en` 时替换中文 doc comment）。
//!
//! 从 `cli/mod.rs` 拆出（那里曾同时装 6 件事：日志设施 / clap schema /
//! 共享 helper / track 引擎 / gen-trackers 子系统 / 帮助文本英化表）。
//! `use super::*;` 拿到父模块的共享 helper（子模块可见祖先的私有项）。

/// LANG=en 时把 clap 帮助文本（doc comment 是中文）覆盖为英文。builder 风格（消费 self 返回 Self）。
pub(super) fn localize_help(cmd: clap::Command) -> clap::Command {
    if !lankefarm::i18n::is_en() {
        return cmd;
    }
    cmd.mut_arg("log_output", |a| a.help("Write runtime errors/warnings/diagnostics to a log file"))
        .mut_subcommand("build", |c| c
            .about("Build a target package (--all: version-incremental + dependency order); upstream updates come from farm track")
            .mut_arg("all", |a| a.help("Build all packages needing rebuild (version mismatch or ABI victims)"))
            .mut_arg("pkg", |a| a.help("Target package name (omit with --all; forces a rebuild)"))
            .mut_arg("pkgs", |a| a.help("pkgs directory (LankeBUILD tree)"))
            .mut_arg("out", |a| a.help("Artifacts/extract/publish directory"))
            .mut_arg("state", |a| a.help("SQLite state DB (job status/resume)"))
            .mut_arg("arch", |a| a.help("Architecture (publish to out/<arch>/<pkg>/)"))
            .mut_arg("image", |a| a.help("Fresh container base image. Required - container builds only"))
            .mut_arg("repo_port", |a| a.help("Embedded local repo server port (container lpkg upgrade pulls from it)"))
            .mut_arg("download_retries", |a| a.help("Source pre-download network retries")))
        .mut_subcommand("abifix", |c| c
            .about("Auto-fix packages whose LankeBUILD.json references a provider-less SONAME: bump release + rebuild (rescan drops orphaned needed_so; still needed ones BLOCK with a hint to update the provider recipe)")
            .mut_arg("pkgs", |a| a.help("pkgs directory (LankeBUILD tree)"))
            .mut_arg("out", |a| a.help("Artifacts/extract/publish directory"))
            .mut_arg("state", |a| a.help("SQLite state DB (job status/resume)"))
            .mut_arg("arch", |a| a.help("Architecture (publish to out/<arch>/<pkg>/)"))
            .mut_arg("image", |a| a.help("Fresh container base image. Required - container builds only"))
            .mut_arg("repo_port", |a| a.help("Embedded local repo server port (container lpkg upgrade pulls from it)"))
            .mut_arg("download_retries", |a| a.help("Source pre-download network retries")))
        .mut_subcommand("track", |c| c
            .about("Probe upstream versions")
            .mut_arg("pkg", |a| a.help("Target package name (required without --all)"))
            .mut_arg("all", |a| a.help("Probe all packages with trackers (read-only proposals)"))
            .mut_arg("run", |a| a.help("Apply new versions to LankeBUILD.json (default: read-only proposals)"))
            .mut_arg("data", |a| a.help("data/trackers directory"))
            .mut_arg("jobs", |a| a.help("Parallel probes (--all only)"))
            .mut_arg("token", |a| a.help("GitHub token (avoid API rate-limit 403)"))
            .mut_arg("gitlab_token", |a| a.help("GitLab token (GITLAB_TOKEN env fallback)")))
        .mut_subcommand("gen-trackers", |c| c
            .about("Batch-generate tracker YAML via LLM (12 per batch)")
            .mut_arg("pkgs", |a| a.help("pkgs directory (LankeBUILD tree)"))
            .mut_arg("data", |a| a.help("data/trackers directory"))
            .mut_arg("api_endpoint", |a| a.help("LLM API endpoint"))
            .mut_arg("api_key", |a| a.help("LLM API key"))
            .mut_arg("model", |a| a.help("LLM model name"))
            .mut_arg("packages", |a| a.help("Only process these packages (comma-separated)")))
        .mut_subcommand("serve", |c| c
            .about("Serve the local repo over HTTP")
            .mut_arg("root", |a| a.help("Repo root (contains <arch>/index.txt and package .lpkg)"))
            .mut_arg("port", |a| a.help("Port")))
        .mut_subcommand("seed", |c| c
            .about("Cold-start seed from a remote repo")
            .mut_arg("remote", |a| a.help("Remote repo URL (e.g. https://lankerepo.wtada233.top)"))
            .mut_arg("arch", |a| a.help("Architecture"))
            .mut_arg("out", |a| a.help("Local repo root directory"))
            .mut_arg("jobs", |a| a.help("Parallel download/extract threads")))
        .mut_subcommand("validate", |c| c
            .about("Rebuild every package without a valid .build_ok marker (recipe changed or never built)")
            .mut_arg("pkgs", |a| a.help("pkgs directory (LankeBUILD tree)"))
            .mut_arg("out", |a| a.help("Artifacts/extract/publish directory"))
            .mut_arg("state", |a| a.help("SQLite state DB (job status/resume)"))
            .mut_arg("arch", |a| a.help("Architecture (publish to out/<arch>/<pkg>/)")))
        .mut_subcommand("export", |c| c
            .about("Flatten the build repo into a release layout <pkg>-<ver>.lpkg")
            .mut_arg("input", |a| a.help("Build repo root (contains <arch>/ subdir)"))
            .mut_arg("output", |a| a.help("Output directory (flat <pkg>-<ver>.lpkg)"))
            .mut_arg("arch", |a| a.help("Architecture")))
        .mut_subcommand("chk", |c| c
            .about("Maintainer utility checks (qml/pkgconf/pkg-err/introspection/vapi/build-deps/pycache/hook/abi/full). NOT a stable interface - these tools may be removed as techniques change; see `farm chk --help`")
            .mut_subcommand("qml", |c| c.about("Check QML imports: each imported module's owner package must be in this package's deps∪needed_so"))
            .mut_subcommand("pkgconf", |c| c.about("Check pkg-config Requires(/private): each module's owner package must be in deps∪needed_so"))
            .mut_subcommand("pkg-err", |c| c.about("Packaging errors: usr/etc|usr/var misplacement, leftover .la/.a"))
            .mut_subcommand("introspection", |c| c.about("Packages shipping .gir must declare gobject-introspection as a build dependency"))
            .mut_subcommand("vapi", |c| c.about("Packages shipping .vapi must declare vala as a build dependency"))
            .mut_subcommand("build-deps", |c| c.about("Every needed_so provider must be declared directly in this package's build_deps (transitive availability does not count)"))
            .mut_subcommand("pycache", |c| c.about("Packages must not ship __pycache__ bytecode caches (empty dirs included)"))
            .mut_subcommand("hook", |c| c.about("postinst hook check: packages shipping sysusers.d/tmpfiles.d must call systemd-sysusers / systemd-tmpfiles --create"))
            .mut_subcommand("abi", |c| c.about("Full ABI symbol@version audit (formerly manual-abi-fullchk)"))
            .mut_subcommand("full", |c| c.about("Run every check with one set of args: qml + pkgconf + pkg-err + introspection + vapi + build-deps + pycache + hook + abi")))
}
