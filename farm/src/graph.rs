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
/// farm 侧只做**基线归一**：它不判定符号版本语义（那是 lpkg 的判据，见 `base/so_spec.hpp`），
/// 但**必须按裸名比较**。否则手写的 `X@{A}` 会被当成"与扫描结果 `X` 不同的 SONAME" ⇒
/// `verify` 判漂移 ⇒ repack 把符号版本**静默涂掉**（`abi` 那一侧更糟：会误报 ABI 断裂并
/// 触发传播重建）。
pub fn so_bare(s: &str) -> &str {
    match s.find('@') {
        Some(i) => &s[..i],
        None => s,
    }
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
/// 花括号不配对时退化成普通逗号切分（lpkg 的读入处会先把这种块整块跳过并告警，
/// 所以 farm 这边只需要"有界、确定"即可）。
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
pub struct RevMap(pub HashMap<String, Vec<String>>);

impl RevMap {
    pub fn build(index: &Index) -> RevMap {
        let mut m: HashMap<String, Vec<String>> = HashMap::new();
        for info in index.packages.values() {
            for soname in &info.needed_so {
                // 裸名（与 `soname_index` 同一套键，见 `so_bare()`）
                m.entry(so_bare(soname).to_string())
                    .or_default()
                    .push(info.name.clone());
            }
        }
        RevMap(m)
    }

    /// 需要某 SONAME 的包（**裸名**查，与 `RevMap::build` 的键一致）
    pub fn needers(&self, soname: &str) -> &[String] {
        self.0
            .get(so_bare(soname))
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
