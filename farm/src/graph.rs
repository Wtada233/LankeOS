//! index.txt 解析 + needed_so 反图（§5）。
//!
//! index.txt 格式：`name|version:hash:deps:provides:needed_so;version2:...`
//! 本模块只依赖 index.txt 文本格式，不触碰 lpkg。
//!
//! 关键语义：
//! - `needed_so` = ELF DT_NEEDED 直接收集，链接级真相 → ABI 反图的唯一依据
//! - `provides` = SONAME + 虚拟提供；ABI 面只取版本化 SONAME（`.so.N`）
//! - `deps` = 运行时脚本/Protocol 依赖，与链接无关，**不参与 ABI 反图**

use std::collections::{HashMap, HashSet};

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct PkgInfo {
    pub name: String,
    pub version: String,
    /// index.txt 里的 SHA256（seed 校验下载产物；由 LankeBUILD 构建的索引为空）。
    pub sha256: String,
    pub deps: Vec<String>,
    pub provides: Vec<String>,
    pub needed_so: Vec<String>,
}

/// 一个仓库索引（单架构）。`packages` 只保留每个包的最新版本块。
#[derive(Debug, Default)]
pub struct Index {
    pub packages: HashMap<String, PkgInfo>,
    /// capability → providers（由全部 provides 构建，含虚拟提供）
    provides_index: HashMap<String, Vec<String>>,
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

/// 从 provides 列表中筛出 ABI 面 SONAME 集合。
///
/// 包含：
/// - 版本化 `.so.*`（libfoo.so.1）——ABI 主信号
/// - 无版本化但**同包无版本化兄弟项**的裸 `.so`——即**无 SONAME 的实体库**
///   （tcl 的 libtcl8.6.so、expect 的 libexpect5.45.4.so），运行时 DT_NEEDED 目标，
///   其提供面变化同样是 ABI 断裂
///
/// 排除：
/// - dev symlink（libfoo.so 指向 libfoo.so.1，同包必有版本化兄弟项）
/// - 虚拟提供 / 非库名（rustc、golang）
pub fn soname_provides_of(provides: &[String]) -> HashSet<String> {
    let versioned: Vec<&String> = provides.iter().filter(|p| is_soname_versioned(p)).collect();
    provides
        .iter()
        .filter(|p| {
            let p = p.as_str();
            if is_soname_versioned(p) {
                return true;
            }
            let bare = p.strip_suffix(".so").unwrap_or(p);
            p.ends_with(".so")
                && !versioned
                    .iter()
                    .any(|v| v.strip_prefix(bare).is_some_and(|r| r.starts_with(".so.")))
        })
        .cloned()
        .collect()
}

fn split_field(field: Option<&str>) -> Vec<String> {
    field
        .map(|s| {
            s.split(',')
                .filter(|x| !x.is_empty())
                .map(|x| x.to_string())
                .collect()
        })
        .unwrap_or_default()
}

impl Index {
    /// 解析 index.txt 文本（每个包取最后一个版本块 = 最新版本）。
    pub fn parse(content: &str) -> Index {
        let mut packages = HashMap::new();
        for raw in content.lines() {
            let line = raw.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let mut parts = line.splitn(3, '|');
            let name = parts.next().unwrap_or("").trim().to_string();
            let rest = parts.next().unwrap_or("");
            let pkg_level_provides = parts.next().unwrap_or("").trim();
            if name.is_empty() || rest.is_empty() {
                continue;
            }
            let last_block = rest.split(';').next_back().unwrap_or("");
            let vparts: Vec<&str> = last_block.splitn(6, ':').collect();
            if vparts.is_empty() || vparts[0].is_empty() {
                continue;
            }
            let mut provides = split_field(vparts.get(3).copied());
            if !pkg_level_provides.is_empty() {
                provides.extend(split_field(Some(pkg_level_provides)));
            }
            packages.insert(
                name.clone(),
                PkgInfo {
                    name,
                    version: vparts[0].to_string(),
                    sha256: vparts.get(1).copied().unwrap_or("").to_string(),
                    deps: split_field(vparts.get(2).copied()),
                    provides,
                    needed_so: split_field(vparts.get(4).copied()),
                },
            );
        }
        Index::from_packages(packages)
    }

    pub fn from_packages(packages: HashMap<String, PkgInfo>) -> Index {
        let mut provides_index: HashMap<String, Vec<String>> = HashMap::new();
        for info in packages.values() {
            for cap in &info.provides {
                provides_index
                    .entry(cap.clone())
                    .or_default()
                    .push(info.name.clone());
            }
        }
        Index {
            packages,
            provides_index,
        }
    }

    /// 包 pkg 的版本化 SONAME provides（ABI 面）。
    pub fn soname_provides(&self, pkg: &str) -> HashSet<String> {
        self.packages
            .get(pkg)
            .map(|i| soname_provides_of(&i.provides))
            .unwrap_or_default()
    }

    /// needed_so 条目 → provider 包（供前向链接与校验）。
    pub fn providers_of(&self, soname: &str) -> &[String] {
        self.provides_index
            .get(soname)
            .map(Vec::as_slice)
            .unwrap_or(&[])
    }

    /// 仓库提供的全部能力（provides_index 的 key，含 SONAME 与虚拟提供）。
    /// 扫描 not-found 判定用：needed_so 条目不在其中 → 无 provider → 不进 needed_so。
    pub fn all_provided_capabilities(&self) -> std::collections::HashSet<String> {
        self.provides_index.keys().cloned().collect()
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
                m.entry(soname.clone()).or_default().push(info.name.clone());
            }
        }
        RevMap(m)
    }

    pub fn needers(&self, soname: &str) -> &[String] {
        self.0.get(soname).map(Vec::as_slice).unwrap_or(&[])
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
