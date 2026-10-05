//! 元数据一致性校验与 repack/传播决策（§6）。
//!
//! 实际扫描结果 vs metadata.json（配方/上一版）比较**两**个 farm 所有权的字段，
//! 两侧都按**规格**（裸 SONAME + 符号版本集合）比，不是只比裸名：
//! - `provides_soname` **少**了（覆盖不到旧声明）→ ABI 断裂 → 最高信号：repack 修正 + 传播重建依赖者
//! - `provides_soname` **多**了 / `needed_so` 规格有变 → 元数据陈旧（二进制没变）→ repack（不 rebuild）
//!
//! ⚠️ **这两个字段是 farm 生成的**：`repack` 与 `update_lankebuild_metadata` 都写**扫描值**，
//! 所以漂移时**手写在配方里的、扫描复现不出来的规格会被覆盖**（维护者 2026-10-05 拍板：
//! 保留版本级判据，字段视为 farm 生成 —— 原先"farm 只做基线归一"的约定随之作废）。
//!
//! **`provides`（虚拟 provider）不参与比较**：它由人手写在 LankeBUILD.json，farm 不扫不比、
//! 原样保留（`repack.rs` 同契约"`provides` 不读不改"）。
//!
//! **`deps` 不参与比较**：deps 由 gen_deps/deprules 规则生成，farm 不扫不比（repack.rs 同
//! 契约"deps 不读不改"）。`ScanResult.deps` 保留以表达扫描输出形状，但 decide 不读它。

/// 构建后扫描结果 / 期望元数据 —— **全库唯一来源**。
///
/// 历史：曾有两份同名异构 `ScanResult`（`scan.rs` 版含 name/version，本模块版只有
/// needed_so/provides/deps），调用方 `build/repo.rs` 手工把两侧 `deps` 填空 → 字段增减时极易只改
/// 一边。现由 verify 拥有该类型：`decide()` 是唯一消费者、放它旁边最不易漂移；verify 不依赖
/// scan/lpkg，分层不变（是上层 scan 依赖本模块）。
/// `name`/`version` 是扫描侧的溯源信息，**`decide()` 不读**（只读 needed_so/provides_soname；deps 亦不读）。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ScanResult {
    pub name: String,
    pub version: String,
    pub needed_so: Vec<String>,
    /// 本包导出的 SONAME（`provides_soname`）——ABI 面比较字段。
    pub provides_soname: Vec<String>,
    pub deps: Vec<String>,
}

impl ScanResult {
    /// 三名参构造（name/version 留空）：比较/测试侧用。
    pub fn new(needed_so: &[&str], provides_soname: &[&str], deps: &[&str]) -> Self {
        ScanResult {
            needed_so: needed_so.iter().map(|s| s.to_string()).collect(),
            provides_soname: provides_soname.iter().map(|s| s.to_string()).collect(),
            deps: deps.iter().map(|s| s.to_string()).collect(),
            ..Default::default()
        }
    }

    /// 由已知的三字段（已拥有所有权）构造（比较侧：`BuildOutcome` → actual）。
    pub fn from_parts(
        needed_so: Vec<String>,
        provides_soname: Vec<String>,
        deps: Vec<String>,
    ) -> Self {
        ScanResult {
            needed_so,
            provides_soname,
            deps,
            ..Default::default()
        }
    }

    /// 由 `.lpkg` 内 metadata.json 的 `Value` 构造（期望值；缺字段 → 空）。name/version 仅作溯源。
    pub fn from_metadata_json(meta: &serde_json::Value) -> Self {
        let arr = |k: &str| -> Vec<String> {
            meta[k]
                .as_array()
                .map(|a| {
                    a.iter()
                        .filter_map(|v| v.as_str().map(String::from))
                        .collect()
                })
                .unwrap_or_default()
        };
        ScanResult {
            name: meta["name"].as_str().unwrap_or("").to_string(),
            version: meta["version"].as_str().unwrap_or("").to_string(),
            needed_so: arr("needed_so"),
            provides_soname: arr("provides_soname"),
            deps: arr("deps"),
        }
    }

    /// 补溯源信息（扫描侧：`scan_lpkg` 用）。
    pub fn with_name(mut self, name: impl Into<String>, version: impl Into<String>) -> Self {
        self.name = name.into();
        self.version = version.into();
        self
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum VerifyAction {
    /// 两字段全部一致 → 直接进 local repo
    Unchanged,
    /// needed_so 漂移（二进制未变，只元数据错）→ repack（不 rebuild）
    Repack { needed_drift: bool },
    /// provides_soname 漂移 → ABI 面变化 → repack 修正 + 传播重建依赖者
    AbiBreak,
}

/// 决策：实际扫描 vs 期望 metadata。
///
/// 两侧都按**规格**（`(裸 SONAME, 版本集合)`）比较，不再只比裸名 —— 见下。
/// `provides_soname` 的**断裂**优先于一切漂移（ABI 面变化是最高信号）。`deps`/`provides`
/// 不比较（见模块头注释）。
pub fn decide(actual: &ScanResult, meta: &ScanResult) -> VerifyAction {
    // `provides_soname`（本包**导出**的 ABI 面）——两个方向语义不同：
    //   · `provides_break`：新声明**覆盖不到**旧声明（少一个 SONAME，或少一个版本节点
    //     `X@{A,B}` → `X@{A}`）。**真断裂** —— 需要那个版本的消费者会被 lpkg 在安装期拒掉，
    //     必须重建它们。此前只比裸名 ⇒ 这一半被静默放过。
    //   · `provides_grew`：反方向（新声明多出来）。**不是断裂**（向后兼容），但**是漂移** ——
    //     配方/索引里的声明陈旧，要写回。漏了这一半 ⇒ 纯新增的 SONAME / 版本**永远同步不出去**
    //     （新包首建、升级新增一个库时，`LankeBUILD.json` 的 provides_soname 一直空着）。
    //     ⚠️ 这一条是 `update_lankebuild_metadata` 的唯一触发路径（`build/mod.rs`），
    //     写的是**扫描值** ⇒ 手写在配方里的、扫描复现不出来的规格会被覆盖。
    let provides_break = !covers_specs(&actual.provides_soname, &meta.provides_soname);
    let provides_grew = !covers_specs(&meta.provides_soname, &actual.provides_soname);
    // `needed_so`（本包**消费**的 ABI 面）：任一方向的规格变化都是漂移（对接的版本变了 ⇒
    // 配方要与产物一致）。此前只比裸名 ⇒ 版本级变化（对接新 glibc）永不写回配方。
    let needed_drift = canon_specs(&actual.needed_so) != canon_specs(&meta.needed_so);
    if provides_break {
        VerifyAction::AbiBreak
    } else if provides_grew || needed_drift {
        VerifyAction::Repack { needed_drift }
    } else {
        VerifyAction::Unchanged
    }
}

/// 规格集合的**规范形**：`parse_so_spec` 归一到 `(裸名, 去重排序的版本集)`。
/// 逐字比较规格串会把 `X@{B,A}` 与 `X@{A,B}` 判成不同；集合语义才是判据。
fn canon_specs(v: &[String]) -> std::collections::BTreeSet<(String, Vec<String>)> {
    v.iter().map(|s| crate::graph::parse_so_spec(s)).collect()
}

/// 新声明集合是否**覆盖**旧声明集合（逐 SONAME：旧声明/导出的每个版本都必须仍有人导出）。
///
/// 判据是**保守包含**（`so_covers`）：新增版本不算断裂（向后兼容），少一个版本才算。
fn covers_specs(new_specs: &[String], old_specs: &[String]) -> bool {
    let (old, new) = (canon_specs(old_specs), canon_specs(new_specs));
    // 旧的每个 (SONAME, 版本集合) 都必须被新的**某个**规格覆盖
    old.iter().all(|(n, syms)| {
        let need = if syms.is_empty() {
            n.clone()
        } else {
            format!("{n}@{{{}}}", syms.join(","))
        };
        new.iter().any(|(nn, ns)| {
            let provided = if ns.is_empty() {
                nn.clone()
            } else {
                format!("{nn}@{{{}}}", ns.join(","))
            };
            crate::graph::so_covers(&provided, &need)
        })
    })
}

#[cfg(test)]
mod tests;

#[cfg(test)]
mod symbol_version_tests {
    use super::*;

    /// **版本级断裂**：metadata（= 上一次扫描的产物）声明了 `libx.so.1@{LIBX_1.0,LIBX_1.1}`，
    /// 而本次扫描只剩 `libx.so.1`（**少了两个版本**）⇒ 这是**真 ABI 断裂**，必须重建需要那些
    /// 版本的消费者。
    ///
    /// ⚠️ **订正 2026-10-05（其一）**：本用例原为 `hand_written_specs_are_not_seen_as_drift`，断言这种
    /// 情况是 `Unchanged` —— 当时的前提是"farm 的扫描**不产出**符号版本，metadata 里的规格是手写
    /// 的，逐字比会把它误判成漂移、repack 会涂掉"。**那个前提已经不存在**：现在扫描**会产出**
    /// 符号版本（`scan.rs` 读 verdef）⇒ 扫描结果与上次产物不一致就是**真的变了**，而"版本变少"
    /// 正是这条新判据要抓的东西。
    ///
    /// ⚠️ **订正 2026-10-05（其二）**：反方向（版本变多）原本断言 `Unchanged` —— 那是在 **pin 缺陷**：
    /// "不是断裂"被错误地实现成了"不是漂移"，于是**纯新增**的 SONAME / 版本永远同步不进
    /// `LankeBUILD.json`（新包首建、升级新增一个库时配方一直是空的）。现在分成两个判据：
    /// 少 → `AbiBreak`，多 → `Repack`（都要写回，只有前者触发传播重建）。
    #[test]
    fn losing_symbol_versions_is_an_abi_break() {
        let actual = ScanResult {
            needed_so: vec!["libc.so.6".into(), "libm.so.6".into()],
            provides_soname: vec!["libx.so.1".into()],
            ..Default::default()
        };
        let meta = ScanResult {
            needed_so: vec!["libc.so.6@GLIBC_2.40".into(), "libm.so.6".into()],
            provides_soname: vec!["libx.so.1@{LIBX_1.0,LIBX_1.1}".into()],
            ..Default::default()
        };
        assert_eq!(
            decide(&actual, &meta),
            VerifyAction::AbiBreak,
            "少导出一个版本也是 ABI 断裂（需要它的消费者会被安装期拒掉）"
        );

        // 反方向：**新增**版本**不是断裂**（向后兼容，不触发重建），但**是漂移**（配方要写回）。
        // 用 meta 的 needed_so 构造，隔离出 provides 侧这唯一的变量。
        let actual_plus = ScanResult {
            provides_soname: vec!["libx.so.1@{LIBX_1.0,LIBX_1.1,LIBX_1.2}".into()],
            ..meta.clone()
        };
        assert_ne!(
            decide(&actual_plus, &meta),
            VerifyAction::AbiBreak,
            "新增版本不是断裂（不该触发消费者重建）"
        );
        assert_eq!(
            decide(&actual_plus, &meta),
            VerifyAction::Repack {
                needed_drift: false
            },
            "但它是漂移 —— 不写回的话，新增的 SONAME/版本永远同步不进配方"
        );

        // 对照：**纯新增一个 SONAME**（旧集合是空集）同样必须判漂移。
        // 这是 `covers_specs(_, old=[])` 恒真的那一格 —— 空集 `all()` 会让它退化成 Unchanged。
        let fresh = ScanResult {
            provides_soname: vec!["libnew.so.1".into()],
            ..meta.clone()
        };
        let empty_meta = ScanResult {
            provides_soname: Vec::new(),
            ..meta.clone()
        };
        assert_eq!(
            decide(&fresh, &empty_meta),
            VerifyAction::Repack {
                needed_drift: false
            },
            "新包首建：provides_soname 从空到有 ⇒ 必须同步进配方（空集不是『覆盖』）"
        );

        // 对照：**真的**换了 SONAME 仍必须被发现
        let mut broken = meta.clone();
        broken.provides_soname = vec!["libx.so.2".into()];
        assert_eq!(decide(&actual, &broken), VerifyAction::AbiBreak);
        // needed_so 漂移（**消费者侧**变化 ⇒ repack，不是断裂）：provides 两侧保持一致，
        // 免得被上面那条 provides 判据抢先命中（优先级：provides 断裂 > needed 漂移）
        let mut drift = actual.clone();
        drift.needed_so = vec!["libz.so.1".into()];
        assert_eq!(
            decide(&actual, &drift),
            VerifyAction::Repack { needed_drift: true }
        );
    }
}
