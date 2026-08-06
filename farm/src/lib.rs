//! LankeOS build farm — 逻辑层（§3.5 分层架构）。
//!
//! 本 crate 是纯逻辑层：graph / abi / verify / track 不触碰 lpkg。
//! 所有对 lpkg 的实际操作收敛在 `lpkg_binding`（trait LpkgBinding）；
//! stub 绑定用于集成测试与 `--demo` 模式，绕开真实构建。

pub mod abi;
pub mod build;
pub mod export;
pub mod graph;
pub mod i18n;
pub mod llm;
pub mod lpkg_binding;
pub mod net;
pub mod repack;
pub mod scan;
pub mod seed;
pub mod serve;
pub mod state;
pub mod track;
pub mod ux;
pub mod verify;
