//! lpkg_binding — 逻辑层与 lpkg 交互的唯一接缝（§3.5 分层架构）。
//!
//! 本模块是**纯接口**：`BuildOutcome` + `trait LpkgBinding` + `StubBinding`，不碰进程/容器。
//! - `StubBinding`：返回 canned 结果，用于集成测试与 `--demo` 模式，绕开真实构建；
//! - docker 实现 `RealBinding`（create/exec 编排）收敛在 `docker` 子模块——本仓库唯一
//!   spawn `docker` 的叶（见 docker.rs），逻辑层模块不依赖它。
//!
//! 绑定优先（ADR #13）：除 lpkg 之外的低层能力（libarchive/下载/哈希/ELF）都应
//! 直接链接进进程内，不 exec 外部程序。

pub mod docker;

use std::collections::HashMap;
use std::path::PathBuf;

/// 一次构建的实际产物扫描结果 + 状态。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct BuildOutcome {
    pub ok: bool,
    pub needed_so: Vec<String>,
    pub provides: Vec<String>,
    pub deps: Vec<String>,
    pub failure_stage: Option<String>,
    /// 构建产物 .lpkg 的路径（RealBinding 填充；StubBinding 为 None）。
    /// 供调度器 publish（进 repo）与 repack（元数据漂移修正）。
    pub lpkg_path: Option<PathBuf>,
}

impl BuildOutcome {
    pub fn success(needed_so: &[&str], provides: &[&str], deps: &[&str]) -> Self {
        BuildOutcome {
            ok: true,
            needed_so: needed_so.iter().map(|s| s.to_string()).collect(),
            provides: provides.iter().map(|s| s.to_string()).collect(),
            deps: deps.iter().map(|s| s.to_string()).collect(),
            failure_stage: None,
            lpkg_path: None,
        }
    }

    pub fn failure(stage: &str) -> Self {
        BuildOutcome {
            ok: false,
            failure_stage: Some(stage.to_string()),
            ..Default::default()
        }
    }
}

/// 逻辑层与 lpkg 交互的唯一接口。
///
/// 只暴露 `build`：依赖拉取（`lpkg upgrade -y`）是每次构建的环境前置，属实现细节，
/// 由 RealBinding 在构建内部完成（容器模式内联在容器脚本里），不进调度器。
pub trait LpkgBinding {
    /// 在 fresh container 中构建 pkg，返回实际扫描结果。
    /// `ok == false` → 确定性构建失败，job 进入 BLOCKED（§8.5 零自动重试）。
    fn build(&mut self, pkg: &str) -> BuildOutcome;

    /// 设置仓库全部提供能力（扫描 not-found 判定用：needed_so 无 provider → 不进 needed_so）。
    /// 默认 no-op；RealBinding 覆盖以填充其 repo_provides 字段。
    fn set_repo_provides(&mut self, _provides: std::collections::HashSet<String>) {}
}

/// Stub：按预设 outcome 返回，不进行任何实际操作。
#[derive(Debug, Default)]
pub struct StubBinding {
    pub outcomes: HashMap<String, BuildOutcome>,
}

impl StubBinding {
    pub fn new(outcomes: HashMap<String, BuildOutcome>) -> Self {
        StubBinding { outcomes }
    }
}

impl LpkgBinding for StubBinding {
    fn build(&mut self, pkg: &str) -> BuildOutcome {
        self.outcomes.get(pkg).cloned().unwrap_or(BuildOutcome {
            ok: true,
            ..Default::default()
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn stub_returns_preset_and_default_success() {
        let mut outcomes = HashMap::new();
        outcomes.insert(
            "llvm".to_string(),
            BuildOutcome::success(&["libxml2.so.3"], &["libLLVM.so", "libLLVM.so.18"], &[]),
        );
        outcomes.insert("bad".to_string(), BuildOutcome::failure("lankebuild_build"));
        let mut b = StubBinding::new(outcomes);

        assert!(b.build("llvm").ok);
        assert_eq!(
            b.build("llvm").provides,
            vec!["libLLVM.so", "libLLVM.so.18"]
        );
        assert!(!b.build("bad").ok);
        assert_eq!(
            b.build("bad").failure_stage.as_deref(),
            Some("lankebuild_build")
        );
        let d = b.build("anything");
        assert!(d.ok);
        assert!(d.needed_so.is_empty());
    }

    #[test]
    fn build_outcome_failure_sets_stage() {
        let f = BuildOutcome::failure("configure");
        assert!(!f.ok);
        assert_eq!(f.failure_stage.as_deref(), Some("configure"));
    }
}
