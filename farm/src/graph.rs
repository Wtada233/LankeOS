//! index.txt 解析 + needed_so 反图（§5）。
//!
//! index.txt 格式：`name|version:hash:deps:provides:provides_soname:needed_so;version2:...|`
//! - 每行**恰好 3 个 `|` 段**：包名 / 版本块串 /（第 3 段恒为空，仅作结尾）；
//! - 每版本块**恰好 6 个冒号字段**，块之间用 `;` 分隔。
//!
//! 本模块只依赖 index.txt 文本格式，不触碰 lpkg。
//!
//! 关键语义：
//! - `needed_so` = ELF DT_NEEDED 直接收集，链接级真相 → ABI 反图的唯一依据
//! - `provides` = **纯虚拟 provider**（手写在 LankeBUILD.json，与 `.so` 无关，如 `rustc`）
//! - `provides_soname` = 本包**导出的 SONAME**（ELF 扫描产物）；ABI 面只取版本化 SONAME（`.so.N`）
//! - `deps` = 运行时脚本/Protocol 依赖，与链接无关，**不参与 ABI 反图**

use std::collections::{HashMap, HashSet};

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct PkgInfo {
    pub name: String,
    pub version: String,
    /// index.txt 里的 SHA256（seed 校验下载产物；由 LankeBUILD 构建的索引为空）。
    pub sha256: String,
    pub deps: Vec<String>,
    /// 纯虚拟 provider（手写在 LankeBUILD.json，与 `.so` 无关）。
    pub provides: Vec<String>,
    /// 本包导出的 SONAME（ELF 扫描产物，与 `needed_so` 同源）。
    pub provides_soname: Vec<String>,
    pub needed_so: Vec<String>,
}

/// 一个仓库索引（单架构）。`packages` 只保留每个包的最新版本块。
#[derive(Debug, Default)]
pub struct Index {
    pub packages: HashMap<String, PkgInfo>,
    /// 虚拟 provider → providers（`provides` 字段）
    /// ⚠️ 这里**没有** virtual-produces 的反查表：8.0.0 起 `needed_so` 只能被
    /// `provides_soname` 满足，farm 侧唯一需要反查的就是 SONAME（`soname_index`）。
    /// 虚拟 `provides` 仍然**逐包保留**（`PkgInfo.provides`，farm 不许覆写手写的那份），
    /// 但没有消费者要"按虚拟能力反查包"，所以不留一张永远没人读的表（死代码）。
    /// SONAME → providers（`provides_soname` 字段；needed_so 链接边解析用）
    soname_index: HashMap<String, Vec<String>>,
}

/// 规格串 → **裸 SONAME**（`X@{A,B}` / `X@A` → `X`）。
///
/// farm 在**建索引 / 查表 / 图比较**这一层只按裸名：`soname_index` 的键、`link_deps` 的比对、
/// `RevMap` 的查询都用它 —— 同一个库的多个规格必须归到同一条边。
///
/// ⚠️ **订正 2026-10-05**：原文写"farm 侧只做**基线归一**：它**不判定符号版本语义**（那是
/// lpkg 的判据，见 `base/so_spec.hpp`），但必须按裸名比较；否则手写的 `X@{A}` 会被当成不同的
/// SONAME ⇒ `verify` 判漂移 ⇒ repack 把符号版本静默涂掉"。**那个前提已被推翻**：
/// `needed_so` / `provides_soname` 现由 **farm 生成**（`scan.rs` 读 ELF 的 verdef/verneed），
/// 漂移判定在 `verify::decide()` 里是**版本级**的 —— 规格按 `(裸名, 去重排序的版本集)`
/// （`canon_specs`）比较：`provides_soname` 少一个版本节点 → `AbiBreak`，多出来 → `Repack`，
/// `needed_so` 任一方向的规格变化 → `Repack`，`Repack` 会**写回 `LankeBUILD.json`（写扫描值）**。
/// `so_bare()` 仍只在**上面那层**（裸名索引 / 图）沿用 —— 别再把它读成"farm 忽略符号版本"。
pub fn so_bare(s: &str) -> &str {
    match s.find('@') {
        Some(i) => &s[..i],
        None => s,
    }
}

/// 规格串 → `(裸 SONAME, 规范化版本集合)`。
///
/// **与 lpkg `base/so_spec.cpp` 的 `parse_so_spec()` 同规则**（宽容解析：畸形 → 整串当裸名；
/// 版本**去重 + 字典序**）。跨语言契约 —— 真值表见 `lpkg/tests/unit/test_so_spec.cpp` 与
/// `lpkg/main/scripts/check_index_conformance.py` 的 `SO_SPEC_VECTORS`，本文件 tests 里有同一份镜像。
pub fn parse_so_spec(s: &str) -> (String, Vec<String>) {
    let Some(at) = s.find('@') else {
        return (s.to_string(), Vec::new());
    };
    let (name, ver) = (&s[..at], &s[at + 1..]);
    // 版本段字符集（与 lpkg 的 `symbol_ok` 同）
    let ok = |t: &str| {
        !t.is_empty()
            && t.bytes()
                .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'_' | b'.' | b'+' | b'-'))
    };
    // **名字段也要校验**（与 lpkg 的 `soname_ok` 同）：非空、不含空白/控制字符、不含结构字符
    // `@ { } , | ; :`。⚠️ 这条是"镜像向量"抓出来的 —— 我第一版只校验了版本段，于是 `"X @A"`
    // 被解析成裸名 `"X "` + 版本 `A`（lpkg 那边判畸形）⇒ 两侧对同一串给出相反结论。
    let name_ok = |t: &str| {
        !t.is_empty()
            && t.bytes().all(|b| {
                b > 0x20
                    && b != 0x7f
                    && !matches!(b, b'@' | b'{' | b'}' | b',' | b'|' | b';' | b':')
            })
    };
    if !name_ok(name) {
        return (s.to_string(), Vec::new()); // 畸形 → 整串当裸名
    }
    let mut syms: Vec<String> = Vec::new();
    if name.is_empty() || ver.is_empty() {
        return (s.to_string(), Vec::new()); // 畸形 → 整串当裸名
    } else if let Some(inner) = ver.strip_prefix('{') {
        let Some(inner) = inner.strip_suffix('}') else {
            return (s.to_string(), Vec::new());
        };
        if inner.contains('{') || inner.contains('}') || inner.is_empty() {
            return (s.to_string(), Vec::new());
        }
        for t in inner.split(',') {
            if !ok(t) {
                return (s.to_string(), Vec::new());
            }
            syms.push(t.to_string());
        }
    } else {
        if ver.contains('{') || ver.contains('}') || !ok(ver) {
            return (s.to_string(), Vec::new());
        }
        syms.push(ver.to_string());
    }
    syms.sort();
    syms.dedup();
    (name.to_string(), syms)
}

/// provider 的声明是否**覆盖**某条 need（**保守**，与 lpkg 的 `so_spec_satisfies()` 同规则）：
/// SONAME 必须相同；need 裸 ⇒ 库在就行；need 带版本 ⇒ provider 必须**也声明**且覆盖**全部**版本。
pub fn so_covers(provided: &str, needed: &str) -> bool {
    let (pn, pv) = parse_so_spec(provided);
    let (nn, nv) = parse_so_spec(needed);
    if pn != nn {
        return false;
    }
    if nv.is_empty() {
        return true;
    }
    if pv.is_empty() {
        return false;
    }
    nv.iter().all(|v| pv.contains(v))
}

/// 从 old/new 两份 `provides_soname` 算"**不再提供的版本**"（`X@V` 串，排序）：
/// 只报"**库还在、某个版本没了**"；整个 SONAME 消失由 `removed_sonames()` 表达（那条走裸名）。
pub fn removed_provided_versions(old_specs: &[String], new_specs: &[String]) -> Vec<String> {
    let offer = |specs: &[String]| -> std::collections::BTreeSet<String> {
        let mut set = std::collections::BTreeSet::new();
        for s in specs {
            let (name, syms) = parse_so_spec(s);
            for v in syms {
                set.insert(format!("{name}@{v}"));
            }
        }
        set
    };
    let (old, new) = (offer(old_specs), offer(new_specs));
    // 版本级的"消失"只在该 SONAME **整体还在**时才算（整体消失由裸名那条边负责，不重复报）
    let new_names: std::collections::HashSet<String> =
        new_specs.iter().map(|s| parse_so_spec(s).0).collect();
    old.difference(&new)
        .filter(|k| new_names.contains(&parse_so_spec(k).0))
        .cloned()
        .collect()
}

/// 是否为版本化 SONAME（ABI 面）：`libfoo.so.1`。
/// 排除裸 dev 链接 `libfoo.so` 与虚拟提供（`rustc`、`golang` 等）。
pub fn is_soname_versioned(s: &str) -> bool {
    let Some(dot) = s.find(".so") else {
        return false;
    };
    let after = &s[dot + 3..];
    after.starts_with('.')
        && after[1..]
            .chars()
            .next()
            .is_some_and(|c| c.is_ascii_digit())
}

/// 从 SONAME 列表（`provides_soname`）中筛出 ABI 面 SONAME 集合。
///
/// 包含：
/// - 版本化 `.so.*`（libfoo.so.1）——ABI 主信号
/// - 无版本化但**同包无版本化兄弟项**的裸 `.so`——即**无 SONAME 的实体库**
///   （tcl 的 libtcl8.6.so、expect 的 libexpect5.45.4.so），运行时 DT_NEEDED 目标，
///   其提供面变化同样是 ABI 断裂
///
/// 排除：
/// - dev symlink（libfoo.so 指向 libfoo.so.1，同包必有版本化兄弟项）
pub fn soname_provides_of(soname_list: &[String]) -> HashSet<String> {
    // 形状判据只认**裸名**（`X@{A,B}` 的 `{A,B}` 里没有 `.so`，不剥掉会把它判成"非版本化"）
    let versioned: Vec<&str> = soname_list
        .iter()
        .map(|p| so_bare(p))
        .filter(|p| is_soname_versioned(p))
        .collect();
    soname_list
        .iter()
        .map(|p| so_bare(p))
        .filter(|p| {
            if is_soname_versioned(p) {
                return true;
            }
            let bare = p.strip_suffix(".so").unwrap_or(p);
            p.ends_with(".so")
                && !versioned
                    .iter()
                    .any(|v| v.strip_prefix(bare).is_some_and(|r| r.starts_with(".so.")))
        })
        .map(String::from)
        .collect()
}

/// 字段内的列表切分：**花括号感知**的逗号切分。
///
/// `provides_soname` / `needed_so` 的条目可以是 `libc.so.6@{GLIBC_2.40,GLIBC_2.39}`，
/// 而花括号里的逗号**不是**字段分隔符。lpkg 侧的 `split_so_list()`（`base/so_spec.cpp`）
/// 是同一条规则的 C++ 实现 —— 两边由 `main/scripts/check_index_conformance.py` 与 lpkg 的
/// C++ 孪生测试同一份 fixture 钉住。改写这里之前先看那份 fixture。
///
/// 花括号**未闭合**时**不会**退化成普通逗号切分：`depth` 是计数器，未配对的 `{` 会让其后所有
/// 逗号**都不切**（整段留成一条）；只有**多余的 `}`** 因为 `depth` 被 clamp 在 0 才退化回普通
/// 逗号切分。lpkg 的读入处会把这种畸形块整块跳过并告警，所以 farm 这边只需要"有界、确定"即可。
///
/// ⚠️ **订正 2026-10-05**：原文笼统写"花括号不配对时退化成普通逗号切分"—— **对未闭合的 `{`
/// 不成立**（实测：`a{b,c` 切成一条 `a{b,c`；`a}b,c` 才切成 `a}b` 与 `c`）。
fn split_field(field: Option<&str>) -> Vec<String> {
    field.map(split_brace_aware).unwrap_or_default()
}

fn split_brace_aware(s: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut start = 0usize;
    let mut depth: i32 = 0;
    for (i, b) in s.bytes().enumerate() {
        match b {
            b'{' => depth += 1,
            b'}' => depth = (depth - 1).max(0),
            b',' if depth == 0 => {
                let piece = s[start..i].trim();
                if !piece.is_empty() {
                    out.push(piece.to_string());
                }
                start = i + 1;
            }
            _ => {}
        }
    }
    let tail = s[start..].trim();
    if !tail.is_empty() {
        out.push(tail.to_string());
    }
    out
}

impl Index {
    /// 解析 index.txt 文本（每个包取最后一个版本块 = 最新版本）。
    ///
    /// 严格匹配新格式：每行**恰好 3 个 `|` 段**、每版本块**恰好 6 个冒号字段**。
    /// 不达标的行**整行跳过**（不再有"字段数不够就用包级 provides 兜底"的旧分叉）。
    pub fn parse(content: &str) -> Index {
        let mut packages = HashMap::new();
        for raw in content.lines() {
            let line = raw.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let parts: Vec<&str> = line.split('|').collect();
            if parts.len() != 3 {
                continue;
            }
            let name = parts[0].trim().to_string();
            let rest = parts[1];
            if name.is_empty() || rest.is_empty() {
                continue;
            }
            let last_block = rest.split(';').next_back().unwrap_or("");
            let vparts: Vec<&str> = last_block.split(':').collect();
            if vparts.len() != 6 || vparts[0].is_empty() {
                continue;
            }
            packages.insert(
                name.clone(),
                PkgInfo {
                    name,
                    version: vparts[0].to_string(),
                    sha256: vparts[1].to_string(),
                    deps: split_field(Some(vparts[2])),
                    provides: split_field(Some(vparts[3])),
                    provides_soname: split_field(Some(vparts[4])),
                    needed_so: split_field(Some(vparts[5])),
                },
            );
        }
        Index::from_packages(packages)
    }

    pub fn from_packages(packages: HashMap<String, PkgInfo>) -> Index {
        // 键是**裸 SONAME**（`so_bare`）：`X@{A,B}` 与 `X@A` 是同一个库的两个规格，
        // 按原样串建表会让"消费者需要 X@A、提供者声明 X@{A,B}"查不到 provider。
        // 原始规格串仍逐字保留在 `PkgInfo.provides_soname` 里（farm 只转录、不改写）。
        let mut soname_index: HashMap<String, Vec<String>> = HashMap::new();
        for info in packages.values() {
            for soname in &info.provides_soname {
                soname_index
                    .entry(so_bare(soname).to_string())
                    .or_default()
                    .push(info.name.clone());
            }
        }
        Index {
            packages,
            soname_index,
        }
    }

    /// 包 pkg 的版本化 SONAME（ABI 面）：`provides_soname` 过滤出的 ABI 面集合。
    pub fn soname_provides(&self, pkg: &str) -> HashSet<String> {
        self.packages
            .get(pkg)
            .map(|i| soname_provides_of(&i.provides_soname))
            .unwrap_or_default()
    }

    /// 裸 SONAME → 索引里**声明过**的全部规格串（`provides_soname` 原样；供 `abifix` 做版本级覆盖判定）。
    pub fn soname_specs(&self, soname: &str) -> Vec<&str> {
        let bare = so_bare(soname);
        let mut out: Vec<&str> = self
            .soname_index
            .get(bare)
            .map(|owners| {
                owners
                    .iter()
                    .filter_map(|p| self.packages.get(p))
                    .flat_map(|i| i.provides_soname.iter())
                    .map(String::as_str)
                    .filter(|spec| so_bare(spec) == bare)
                    .collect::<Vec<_>>()
            })
            .unwrap_or_default();
        out.sort_unstable();
        out.dedup();
        out
    }

    /// needed_so 条目 → provider 包（供前向链接与校验）。按**裸 SONAME** 查
    /// （`provides_soname` 的基线归一形 —— 见 `so_bare()`）。
    pub fn providers_of(&self, soname: &str) -> &[String] {
        self.soname_index
            .get(so_bare(soname))
            .map(Vec::as_slice)
            .unwrap_or(&[])
    }

    /// 仓库提供的全部 **SONAME**（只含 `provides_soname` 那一半）。
    ///
    /// ⚠️ **别把虚拟 `provides` 并进来**：8.0.0 起 `needed_so` 只能被 `provides_soname`
    /// 满足（lpkg 侧把两边放进两个命名空间）。并进来会让"仓库里没有 provider 的 needed_so"
    /// 因为一个**同名的虚拟能力**而留下 —— 而那个条目在 lpkg 那边根本解析不出 provider。
    ///
    /// 用途：scan 的 not-found 过滤（needed_so 条目不在其中 → 无 provider → 不记进 needed_so）
    /// 与 `abifix` 的缺失清单判定（两者同源）。
    pub fn all_provided_sonames(&self) -> std::collections::HashSet<String> {
        self.soname_index.keys().cloned().collect()
    }

    /// 排序的包名列表（确定性输出用）。
    pub fn sorted_names(&self) -> Vec<String> {
        let mut v: Vec<String> = self.packages.keys().cloned().collect();
        v.sort();
        v
    }

    pub fn len(&self) -> usize {
        self.packages.len()
    }

    pub fn is_empty(&self) -> bool {
        self.packages.is_empty()
    }
}

/// SONAME → 需要它的包集合（ABI 反图的基础）。
/// 只从 `needed_so` 构建；**不用 `deps`**（deps 是运行时脚本/Protocol 边）。
#[derive(Debug, Default)]
pub struct RevMap {
    /// 裸 SONAME → 需要它的包（既有语义：`X` 与 `X@V` 都登记到这里）
    bare: HashMap<String, Vec<String>>,
    /// `裸 SONAME@版本` → 需要**那个版本**的包（need 声明里的每个版本各登记一次）
    versions: HashMap<String, Vec<String>>,
}

impl RevMap {
    pub fn build(index: &Index) -> RevMap {
        let mut bare: HashMap<String, Vec<String>> = HashMap::new();
        let mut versions: HashMap<String, Vec<String>> = HashMap::new();
        for info in index.packages.values() {
            for soname in &info.needed_so {
                bare.entry(so_bare(soname).to_string())
                    .or_default()
                    .push(info.name.clone());
                // 版本级：need 带版本时，**每个**版本各登记一次（`X@{A,B}` ⇒ X@A 与 X@B）
                let (name, syms) = parse_so_spec(soname);
                for v in syms {
                    versions
                        .entry(format!("{name}@{v}"))
                        .or_default()
                        .push(info.name.clone());
                }
            }
        }
        RevMap { bare, versions }
    }

    /// 需要某 SONAME 的包（**裸名**查）
    pub fn needers(&self, soname: &str) -> &[String] {
        self.bare
            .get(so_bare(soname))
            .map(Vec::as_slice)
            .unwrap_or(&[])
    }

    /// 需要某**具体版本**（`X@V`）的包。版本级的 ABI 断裂用它定位受害者。
    pub fn version_needers(&self, soname_at_version: &str) -> &[String] {
        self.versions
            .get(soname_at_version)
            .map(Vec::as_slice)
            .unwrap_or(&[])
    }
}

/// 包 pkg 的链接依赖（前向）：needed_so → provider 包名。
pub fn link_deps(index: &Index, pkg: &str) -> Vec<String> {
    let mut set = HashSet::new();
    if let Some(info) = index.packages.get(pkg) {
        for soname in &info.needed_so {
            for prov in index.providers_of(soname) {
                if prov != pkg {
                    set.insert(prov.clone());
                }
            }
        }
    }
    let mut v: Vec<String> = set.into_iter().collect();
    v.sort();
    v
}

#[cfg(test)]
mod tests;
