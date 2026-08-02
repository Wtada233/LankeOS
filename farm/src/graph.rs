//! index.txt 解析 + needed_so 反图（§5）。
//!
//! index.txt 格式：`name|version:hash:deps:provides:needed_so;version2:...`
//! 本模块只依赖 index.txt 文本格式，不触碰 lpkg。
//!
//! 关键语义：
//! - `needed_so` = ELF DT_NEEDED 直接收集，链接级真相 → ABI 反图的唯一依据
//! - `provides` = SONAME + 虚拟提供；ABI 面只取版本化 SONAME（`.so.N`）
//! - `deps` = 运行时脚本/Protocol 依赖，与链接无关，**不参与 ABI 反图**

use std::collections::{HashMap, HashSet, VecDeque};

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

/// 从 provides 列表中筛出版本化 SONAME 集合（ABI 面）。
pub fn soname_provides_of(provides: &[String]) -> HashSet<String> {
    provides
        .iter()
        .filter(|p| is_soname_versioned(p))
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

/// 包 pkg 的**直接**依赖者：链接 pkg 提供的任一版本化 SONAME 的包。
/// 这是 ABI 传播的边（direct-only，§7.2）。
pub fn reverse_dependents(index: &Index, revmap: &RevMap, pkg: &str) -> Vec<String> {
    let mut set = HashSet::new();
    for soname in index.soname_provides(pkg) {
        for needer in revmap.needers(&soname) {
            if needer != pkg {
                set.insert(needer.clone());
            }
        }
    }
    let mut v: Vec<String> = set.into_iter().collect();
    v.sort();
    v
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

/// 反向传递闭包（树状）——用于与 direct-only 对比展示过度重建。
/// 这是 rebuild-helper / remove 场景的语义，ABI 场景**不**用它（§7.2）。
pub fn reverse_closure(index: &Index, revmap: &RevMap, roots: &[String]) -> Vec<String> {
    let mut set = HashSet::new();
    let mut queue: VecDeque<String> = roots.iter().cloned().collect();
    while let Some(p) = queue.pop_front() {
        for dep in reverse_dependents(index, revmap, &p) {
            if set.insert(dep.clone()) {
                queue.push_back(dep);
            }
        }
    }
    let mut v: Vec<String> = set.into_iter().collect();
    v.sort();
    v
}

#[cfg(test)]
mod tests {
    use super::*;

    const CHAIN: &str = "\
libxml2|2.9.0:hash::libxml2.so,libxml2.so.2:ld-linux.so.2,libc.so.6|
llvm|18.1.0:hash::libLLVM.so,libLLVM.so.18:libxml2.so.2,libc.so.6|
rust|1.80.0:hash::rustc:libLLVM.so.18,libc.so.6|
glibc|2.39:hash::libc.so,libc.so.6,ld-linux.so.2:|
";

    #[test]
    fn parse_index_latest_version_block() {
        let idx = Index::parse(CHAIN);
        assert_eq!(idx.packages.len(), 4);
        let llvm = &idx.packages["llvm"];
        assert_eq!(llvm.version, "18.1.0");
        assert_eq!(llvm.needed_so, vec!["libxml2.so.2", "libc.so.6"]);
        assert_eq!(llvm.provides, vec!["libLLVM.so", "libLLVM.so.18"]);
    }

    #[test]
    fn soname_filter_excludes_dev_links_and_virtuals() {
        let idx = Index::parse(CHAIN);
        assert_eq!(
            idx.soname_provides("libxml2"),
            HashSet::from(["libxml2.so.2".to_string()])
        );
        assert!(idx.soname_provides("rust").is_empty());
    }

    #[test]
    fn link_deps_forward() {
        let idx = Index::parse(CHAIN);
        assert_eq!(link_deps(&idx, "llvm"), vec!["glibc", "libxml2"]);
        assert_eq!(link_deps(&idx, "rust"), vec!["glibc", "llvm"]);
        assert_eq!(link_deps(&idx, "libxml2"), vec!["glibc"]);
    }

    #[test]
    fn reverse_dependents_is_soname_precise() {
        let idx = Index::parse(CHAIN);
        let rev = RevMap::build(&idx);
        assert_eq!(reverse_dependents(&idx, &rev, "libxml2"), vec!["llvm"]);
        assert_eq!(reverse_dependents(&idx, &rev, "llvm"), vec!["rust"]);
        assert_eq!(
            reverse_dependents(&idx, &rev, "glibc"),
            vec!["libxml2", "llvm", "rust"]
        );
    }

    #[test]
    fn reverse_closure_vs_direct() {
        let idx = Index::parse(CHAIN);
        let rev = RevMap::build(&idx);
        assert_eq!(
            reverse_closure(&idx, &rev, &["libxml2".to_string()]),
            vec!["llvm", "rust"]
        );
        assert_eq!(reverse_dependents(&idx, &rev, "libxml2"), vec!["llvm"]);
    }

    #[test]
    fn is_soname_versioned_cases() {
        assert!(is_soname_versioned("libfoo.so.1"));
        assert!(is_soname_versioned("libsystemd.so.0"));
        assert!(is_soname_versioned("libc.so.6.1"));
        assert!(is_soname_versioned("ld-linux-x86-64.so.2"));
        assert!(!is_soname_versioned("libfoo.so"));
        assert!(!is_soname_versioned("rustc"));
        assert!(!is_soname_versioned("libfoo.soX"));
    }
}
