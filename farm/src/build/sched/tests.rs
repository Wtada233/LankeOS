//! `src/build/sched.rs` 的单元测试（从生产文件搬出：那里测试比生产还长，正文被淹没）。
//! 仍是同一父模块的子模块 ⇒ `use super::*` 与 crate 内私有项照旧可用。

use super::*;
use std::fs;

/// 手搭图：`names` 全部节点 + `edges` 的 (from, to, kind)。
/// `in_deg` = 未满足的依赖数（= 出边数），与 `topo_order` 里的语义一致。
#[allow(clippy::type_complexity)]
fn build(
    names: &[&str],
    edges: &[(&str, &str, EdgeKind)],
) -> (
    HashMap<String, Vec<String>>,
    HashMap<String, usize>,
    HashMap<(String, String), EdgeKind>,
) {
    let mut graph: HashMap<String, Vec<String>> = HashMap::new();
    let mut in_deg: HashMap<String, usize> = HashMap::new();
    let mut kind: HashMap<(String, String), EdgeKind> = HashMap::new();
    for n in names {
        graph.insert(n.to_string(), Vec::new());
        in_deg.insert(n.to_string(), 0);
    }
    for (u, v, k) in edges {
        graph.get_mut(*u).unwrap().push(v.to_string());
        *in_deg.get_mut(*u).unwrap() += 1;
        kind.insert((u.to_string(), v.to_string()), *k);
    }
    for deps in graph.values_mut() {
        deps.sort(); // 与 topo_order 一致：邻接字典序（确定性）
    }
    (graph, in_deg, kind)
}

#[test]
fn cycle_cut_prefers_build_deps_over_link_edge() {
    // 图：a↔b 两条**链接**边；b→c 链接边；c→b **build_deps** 边。
    // 环 [a,b] 全是链接边，但**另一个**环 [b,c] 里那条 c→b 是 build_deps → 必须先切它，
    // 而不是"先遇到的环"里的链接边。
    let (g, d, k) = build(
        &["a", "b", "c"],
        &[
            ("a", "b", EdgeKind::Abi),
            ("b", "a", EdgeKind::Abi),
            ("b", "c", EdgeKind::Abi),
            ("c", "b", EdgeKind::BuildDep),
        ],
    );
    assert_eq!(
        find_cycle_edge(&g, &d, &k),
        Some(("c".to_string(), "b".to_string())),
        "切环必须优先挑 build_deps 边（用户规则：build_deps 优先级在 ABI 下面）"
    );
}

#[test]
fn cycle_cut_picks_build_dep_even_when_it_is_a_dfs_tree_edge() {
    // 真实事故（libadwaita）：三角形 gtk4 →(BuildDep) sysprof →(Abi) libadwaita →(Abi) gtk4。
    // DFS 从名字最小的 gtk4 起，`gtk4→sysprof` 是**树边**、`libadwaita→gtk4` 才是后向边——
    // 旧实现只认后向边 → 挑不到那条 build_deps → 退切链接边 `libadwaita→gtk4`，
    // 于是 libadwaita 先于 gtk4 构建、容器里还是旧 gtk4（4.22.4）→ 要 ≥4.23.1 → configure 失败。
    // 正确：切 gtk4→sysprof（构建工具边，容器里用旧 sysprof 即可），libadwaita 必须等新 gtk4。
    let (g, d, k) = build(
        &["gtk4", "libadwaita", "sysprof"],
        &[
            ("gtk4", "sysprof", EdgeKind::BuildDep),
            ("sysprof", "libadwaita", EdgeKind::Abi),
            ("sysprof", "gtk4", EdgeKind::Abi),
            ("libadwaita", "gtk4", EdgeKind::Abi),
        ],
    );
    assert_eq!(
        find_cycle_edge(&g, &d, &k),
        Some(("gtk4".to_string(), "sysprof".to_string())),
        "环上的 build_deps 边即使是 DFS 树边也必须被挑中"
    );
}

#[test]
fn gtk4_builds_before_libadwaita_after_cycle_cut() {
    // 端到端：切完环后 topo 必须让 gtk4 先于 libadwaita（否则容器里是旧 gtk4，libadwaita 直接失败）
    let dir = std::env::temp_dir().join("farm-cycle-gtk4");
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    write_pkg_pkgs(&dir, "gtk4", &["libgtk-4.so.1"], &[], &["sysprof"]);
    write_pkg_pkgs(
        &dir,
        "libadwaita",
        &["libadwaita-1.so.0"],
        &["libgtk-4.so.1"],
        &["gtk4"],
    );
    write_pkg_pkgs(
        &dir,
        "sysprof",
        &["libsysprof-6.so.6"],
        &["libadwaita-1.so.0", "libgtk-4.so.1"],
        &[],
    );
    let old = Index::from_packages(HashMap::from([
        (
            "gtk4".to_string(),
            crate::graph::PkgInfo {
                name: "gtk4".into(),
                version: "4.22.4".into(),
                sha256: String::new(),
                deps: vec![],
                provides: vec![],
                provides_soname: vec!["libgtk-4.so.1".into()],
                needed_so: vec![],
            },
        ),
        (
            "libadwaita".to_string(),
            crate::graph::PkgInfo {
                name: "libadwaita".into(),
                version: "1.9.3".into(),
                sha256: String::new(),
                deps: vec![],
                provides: vec![],
                provides_soname: vec!["libadwaita-1.so.0".into()],
                needed_so: vec!["libgtk-4.so.1".into()],
            },
        ),
        (
            "sysprof".to_string(),
            crate::graph::PkgInfo {
                name: "sysprof".into(),
                version: "50.0".into(),
                sha256: String::new(),
                deps: vec![],
                provides: vec![],
                provides_soname: vec!["libsysprof-6.so.6".into()],
                needed_so: vec!["libadwaita-1.so.0".into(), "libgtk-4.so.1".into()],
            },
        ),
    ]));
    let targets: Vec<String> = ["gtk4", "libadwaita", "sysprof"]
        .iter()
        .map(|s| s.to_string())
        .collect();
    let order = topo_order(&dir, &targets, &old, &[]);
    let pos = |x: &str| order.iter().position(|n| n == x).unwrap();
    assert!(
        pos("gtk4") < pos("libadwaita"),
        "gtk4 必须在 libadwaita 之前（否则容器里是旧 gtk4）: {order:?}"
    );
    fs::remove_dir_all(&dir).ok();
}

/// 写一个只带 provides/needed_so/build_deps 的配方（topo_order 只读这些）。
fn write_pkg_pkgs(dir: &Path, name: &str, provides: &[&str], needed: &[&str], bd: &[&str]) {
    let p = dir.join(name);
    fs::create_dir_all(&p).unwrap();
    fs::write(
        p.join("LankeBUILD.json"),
        serde_json::to_string(&serde_json::json!({
            "name": name, "version": "1.0",
            "provides": provides, "needed_so": needed, "build_deps": bd
        }))
        .unwrap(),
    )
    .unwrap();
}

#[test]
fn cycle_cut_prefers_group_over_link_edge() {
    // 没有 build_deps 后向边时退而切**组边**，仍然不动链接边
    let (g, d, k) = build(
        &["a", "b", "c"],
        &[
            ("a", "b", EdgeKind::Abi),
            ("b", "a", EdgeKind::Abi),
            ("b", "c", EdgeKind::Abi),
            ("c", "b", EdgeKind::Group),
        ],
    );
    assert_eq!(
        find_cycle_edge(&g, &d, &k),
        Some(("c".to_string(), "b".to_string()))
    );
}

#[test]
fn cycle_cut_falls_back_to_link_edge_when_no_lower_kind() {
    // 环里只有链接边 → 只能切链接边（不能返回 None 卡住）；确定性：候选排序后取第一条 a→b
    let (g, d, k) = build(
        &["a", "b"],
        &[("a", "b", EdgeKind::Abi), ("b", "a", EdgeKind::Abi)],
    );
    assert_eq!(
        find_cycle_edge(&g, &d, &k),
        Some(("a".to_string(), "b".to_string()))
    );
}

#[test]
fn same_pair_keeps_higher_priority_kind() {
    // 同一对 (P→D) 既是 build_deps 又是链接边时，种类取**链接**（更高）——
    // 否则会把它当"可切的 build_deps 边"，等于间接切掉链接约束。
    let mut m: BTreeMap<String, EdgeKind> = BTreeMap::new();
    add_edge(&mut m, "x".into(), EdgeKind::BuildDep);
    add_edge(&mut m, "x".into(), EdgeKind::Abi);
    assert_eq!(m["x"], EdgeKind::Abi, "链接边优先，不因 build_deps 降级");
    add_edge(&mut m, "y".into(), EdgeKind::Abi);
    add_edge(&mut m, "y".into(), EdgeKind::Group);
    assert_eq!(m["y"], EdgeKind::Abi, "组边也不降级链接边");
}
