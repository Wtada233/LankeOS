//! sched.rs — 构建顺序（拓扑 + 环切割 + ABI 受害者重排）。纯排序，无构建副作用。

use std::cmp::Reverse;
use std::collections::{BTreeMap, BinaryHeap, HashMap, HashSet, VecDeque};

/// 拓扑边的种类——**决定切环偏好**（切环时从低到高挑）：
///
/// - `BuildDep`(0)：**最先切**。build_deps 环多是构建工具互赖（gtk4 ↔ sysprof 那类），
///   容器里用旧版工具即可，切断不影响 ABI 正确性。
/// - `Group`(1)：次之。声明式重建组的排序约束，切断只是少等一轮。
/// - `Abi`(2)：**最后切**。`needed_so` 链接边是构建序的真相——切了会让消费者先于库重建，
///   按旧 ABI 白跑甚至编错（用户规则：build_deps 优先级在 ABI 下面）。
///
/// 同一对 (P→D) 同时是多种边时取**最高**优先级（宁可切别的边）。
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
enum EdgeKind {
    BuildDep = 0,
    Group = 1,
    Abi = 2,
}
use std::path::Path;

use crate::build::groups::RebuildGroups;
use crate::graph::Index;
use crate::tr;
use crate::ux;

/// Kahn 拓扑排序，**按 needed_so 链接边 + build_deps 依赖边 + 声明式重建组边**＋环切割。
///
/// 设计：构建序主要看**链接依赖**——需要重建的链接库必须先建，依赖者才能按新 ABI 链接。
/// `deps` **不参与排序**；`build_deps` **无条件**作为依赖边参与排序（**仅限本轮 targets 内**）：
/// build 工具一般由每个容器 `lpkg upgrade` 从 repo 拿最新版，无需排队——但本轮要重建的依赖必须等它
/// 先产出。**仓库缺失的构建依赖**（`!old.packages.contains_key(d)`：从未进过仓库的首建/引导依赖，
/// 如 gjs 依赖的 sysprof、samba 依赖新加的 talloc/tevent）由 `build_plan` 的
/// `expand_missing_build_deps` **递归并入本轮 targets** 后，本函数照常按其 build_deps 边先建它，
/// 否则依赖方容器 `lpkg upgrade` 装不到它必然 BLOCKED。"不链 libpython 但 ABI 敏感"的包
/// （python-cairo/gobject/blueman/meson…）由 `data/build/*.yaml` 声明式重建组处理
/// （`build/groups.rs`）——这些组受害者没有 needed_so 链接边，必须靠 `extra_edges`（victim → on）
/// 强制排在触发包 `on` 之后，否则 `--all` 模式下 python-cairo 会在 python 重建前构建（容器升级时
/// repo 还是旧 python，构建基于旧 ABI 白跑）。
///
/// 参考 `lpkg/main/scripts/lankeos-world-rebuild-helper.py`（确定性 Kahn + 三色 DFS 切环）；区别：
/// farm 增量构建，已就绪（不在 targets）的包不进图，无需全量重建。循环依赖：打印警告并切断环上
/// **优先级最低**的边（每轮一条，确定性），保证总能给出完整顺序。
///
/// `pkgs_dir` 用来读配方（LankeBUILD.json）：
/// - `build_deps`：**无条件**作为依赖边参与排序（**仅限 targets 内**）——构建期需要另一个包先产出
///   时必须等它先建（如 python-bar 构建要 python-foo 本轮重建的产物）。build_deps 指向本轮不重建的
///   包 → 边丢弃，该包直接构建不等待。
///
/// **切环偏好**（`EdgeKind`）：出现环时按 **build_deps → 组边 → needed_so 链接边** 的顺序挑
/// **位于环上**的边切断（"位于环上" = 该边回归可达，见 `find_cycle_edge`）。**只认 DFS 后向边是不够的**：
/// 环里那条 build_deps 边可能恰好是树边（实测 `gtk4 →(BuildDep) sysprof →(Abi) libadwaita →(Abi) gtk4`，
/// 从名字最小的 gtk4 起 DFS 时 `gtk4→sysprof` 就是树边）→ 挑不到它就会退切链接边 `libadwaita→gtk4`，
/// 于是 libadwaita 先于 gtk4 构建、容器里还是旧 gtk4（4.22.4 而 libadwaita 1.10 要 ≥4.23.1）→ 直接失败。
/// 链接边是构建序的真相（切它会让消费者先于库重建 → 按旧 ABI 白跑），**最后**才切；build_deps 环
/// 多是构建工具互赖（gtk4 ↔ sysprof 那类），容器里用旧版工具即可，**最先**切。
pub(crate) fn topo_order(
    pkgs_dir: &Path,
    targets: &[String],
    old: &Index,
    extra_edges: &[(String, String)],
) -> Vec<String> {
    let names: HashSet<&str> = targets.iter().map(String::as_str).collect();

    // 声明式重建组边：victim → on（victim 依赖 on，on 必须在前）。
    // 与链接边一样只对 targets 内的包生效（on 不在 targets 则无排序约束）。
    let mut group_deps: HashMap<String, Vec<String>> = HashMap::new();
    for (victim, on) in extra_edges {
        if victim == on || !names.contains(victim.as_str()) || !names.contains(on.as_str()) {
            continue;
        }
        group_deps
            .entry(victim.clone())
            .or_default()
            .push(on.clone());
    }

    let mut graph: HashMap<String, Vec<String>> = HashMap::new();
    let mut in_deg: HashMap<String, usize> = HashMap::new();
    let mut rev: HashMap<String, Vec<String>> = HashMap::new();
    // 边种类（供切环偏好）：同一对 (P→D) 同时是多种边时取**最高**优先级（宁可切别的边）
    let mut kind: HashMap<(String, String), EdgeKind> = HashMap::new();
    for n in targets {
        let mut deps: BTreeMap<String, EdgeKind> = BTreeMap::new();
        // 1) needed_so 链接边（ABI 真相，**最高**优先级）
        for d in crate::graph::link_deps(old, n) {
            if d.as_str() != n.as_str() && names.contains(d.as_str()) {
                add_edge(&mut deps, d, EdgeKind::Abi);
            }
        }
        // 2) 声明式重建组边
        if let Some(gd) = group_deps.get(n) {
            for d in gd {
                add_edge(&mut deps, d.clone(), EdgeKind::Group);
            }
        }
        // 3) build_deps（**无条件**，仅限 targets 内；目标不重建 → 边丢弃，与链接/组边同规则）
        if let Some(lb) = crate::build::read_lankebuild(pkgs_dir, n) {
            for d in &lb.build_deps {
                if d.as_str() != n.as_str() && names.contains(d.as_str()) {
                    add_edge(&mut deps, d.clone(), EdgeKind::BuildDep);
                }
            }
            // 顺带告警未知 farm flag（farm_flags 的注册面）。本函数**不再消费任何 flag**——
            // 原 BUILD_AFTER_BUILD_DEPS 的行为已成为 build_deps 边的默认语义（见上方文档）。
            let _ = crate::build::farm_flags::parse_all(&lb.farm_flags);
        }
        let dep_list: Vec<String> = deps.keys().cloned().collect(); // BTreeMap → 已字典序
        for d in &dep_list {
            kind.insert((n.clone(), d.clone()), deps[d]);
        }
        graph.insert(n.clone(), dep_list.clone());
        in_deg.insert(n.clone(), dep_list.len());
        for d in dep_list {
            rev.entry(d).or_default().push(n.clone());
        }
    }

    // 确定性就绪队列：名字升序（BinaryHeap<Reverse> 弹最小值）
    let mut heap: BinaryHeap<Reverse<String>> = targets
        .iter()
        .filter(|n| in_deg[*n] == 0)
        .map(|n| Reverse(n.clone()))
        .collect();
    let mut order: Vec<String> = Vec::new();

    while order.len() < targets.len() {
        if heap.is_empty() {
            // 剩余节点构成环：三色 DFS 找一条后向边切断（**按切环偏好挑**：build_deps → 组边 →
            // 链接边），警告后继续（循环会逐条断开）
            if let Some((u, v)) = find_cycle_edge(&graph, &in_deg, &kind) {
                eprintln!("  {}", ux::yellow(&tr!("build.cycle", u, v)));
                graph.get_mut(&u).unwrap().retain(|x| x != &v);
                kind.remove(&(u.clone(), v.clone()));
                if let Some(ds) = rev.get_mut(&v) {
                    ds.retain(|x| x != &u);
                }
                let e = in_deg.get_mut(&u).unwrap();
                *e -= 1;
                if *e == 0 {
                    heap.push(Reverse(u));
                }
                continue;
            }
            // 理论不可达兜底：剩余按序追加
            let mut rest: Vec<String> = targets
                .iter()
                .filter(|n| !order.contains(n))
                .cloned()
                .collect();
            rest.sort();
            order.extend(rest);
            break;
        }
        let n = heap.pop().unwrap().0;
        order.push(n.clone());
        if let Some(dependers) = rev.get(&n) {
            for d in dependers {
                let e = in_deg.get_mut(d).unwrap();
                *e -= 1;
                if *e == 0 {
                    heap.push(Reverse(d.clone()));
                }
            }
        }
    }
    order
}

/// 记一条依赖边；已有同向边时保留**更高**优先级的种类（宁可切别的边，而不是把链接边降级成
/// 可切的 build_deps 边）。
fn add_edge(deps: &mut BTreeMap<String, EdgeKind>, d: String, k: EdgeKind) {
    deps.entry(d)
        .and_modify(|cur| *cur = (*cur).max(k))
        .or_insert(k);
}

/// 找一条**可切的边**：按切环偏好（build_deps → 组边 → 链接边）挑**确实位于某个环上**的边。
///
/// 为什么不是"找后向边"（旧实现）：环里那条 build_deps 边完全可能是 DFS 的**树边**——
/// 实测三角形 `gtk4 →(BuildDep) sysprof →(Abi) libadwaita →(Abi) gtk4`：DFS 从名字最小的 gtk4 起，
/// `gtk4→sysprof` 是树边、`libadwaita→gtk4` 才是后向边 → 只认后向边就永远选不到那条构建工具边，
/// 只能退而切链接边 → **libadwaita 排到 gtk4 前面**，容器里还是旧 gtk4（4.22.4）而 libadwaita 1.10
/// 要求 ≥4.23.1 → configure 直接失败（真实事故）。判"在环上" = 该边回归可达（v ⇝ u）。
/// 确定性：候选按 (种类, 边) 排序后取第一条。
fn find_cycle_edge(
    graph: &HashMap<String, Vec<String>>,
    in_deg: &HashMap<String, usize>,
    kind: &HashMap<(String, String), EdgeKind>,
) -> Option<(String, String)> {
    let rem: HashSet<String> = graph
        .keys()
        .filter(|n| in_deg.get(*n).copied().unwrap_or(0) > 0)
        .cloned()
        .collect();
    if rem.is_empty() {
        return None;
    }
    let mut edges: Vec<((String, String), EdgeKind)> = kind
        .iter()
        .filter(|((u, v), _)| {
            rem.contains(u)
                && rem.contains(v)
                && graph.get(u).is_some_and(|ds| ds.iter().any(|d| d == v))
        })
        .map(|(e, k)| (e.clone(), *k))
        .collect();
    edges.sort_by(|a, b| a.1.cmp(&b.1).then_with(|| a.0.cmp(&b.0)));
    edges
        .into_iter()
        .find(|((u, v), _)| reaches(graph, v, u, &rem))
        .map(|(e, _)| e)
}

/// `from` 能否**只经剩余子图**（in_deg > 0 的节点）到达 `to`——判一条边是否在环上（切了它才解得开）。
fn reaches(
    graph: &HashMap<String, Vec<String>>,
    from: &str,
    to: &str,
    rem: &HashSet<String>,
) -> bool {
    if from == to {
        return true;
    }
    let mut seen: HashSet<&str> = HashSet::new();
    let mut queue: VecDeque<&str> = VecDeque::new();
    seen.insert(from);
    queue.push_back(from);
    while let Some(n) = queue.pop_front() {
        for d in graph.get(n).map(|v| v.as_slice()).unwrap_or(&[]) {
            if !rem.contains(d) {
                continue;
            }
            if d == to {
                return true;
            }
            if seen.insert(d.as_str()) {
                queue.push_back(d.as_str());
            }
        }
    }
    false
}

/// ABI 受害者入队后按**依赖算法**重排队列（deps-first，环切割）。
/// 被依赖的受害者先建，依赖它们的后建——否则按字母序先建 appstream 时，其构建依赖树里还引用旧
/// SONAME 的受害者（如 librsvg 引用 libxml2.so.2）未重建，装构建依赖硬报错。
///
/// **先去重**：`seen` 只挡"已构建"的包，同一受害者可能被多个 ABI 断裂重复入队（chromium 的
/// 依赖 libA/libB/libC 各断裂一次 → 3 个 chromium 条目）。重复条目传给 topo_order 会污染
/// in_deg/rev（rev[libA] 含 3 个 chromium，弹出 libA 时 in_deg 多减 3 次 → 顺序错乱、chromium
/// 可能在其依赖重建前被构建）。去重后每包唯一；victim 标记取 OR（任一断裂入队 → 按传播重建
/// bump release）。
pub(crate) fn reorder_queue(
    queue: &mut VecDeque<(String, bool)>,
    pkgs_dir: &Path,
    old: &Index,
    groups: &RebuildGroups,
) {
    if queue.len() < 2 {
        return;
    }
    let mut flags: HashMap<String, bool> = HashMap::new();
    for (n, is_victim) in queue.iter() {
        *flags.entry(n.clone()).or_insert(false) |= *is_victim;
    }
    let mut names: Vec<String> = flags.keys().cloned().collect();
    names.sort();
    // 组边一并参与重排：组受害者（python-* 等）排在触发包 python 之后，
    // 与 needed_so 链接边同样处理，避免 `--all` 初始队列里 python-cairo 先于 python。
    let edges = groups.trigger_edges_in(&names);
    let order = topo_order(pkgs_dir, &names, old, &edges);
    *queue = order.into_iter().map(|n| (n.clone(), flags[&n])).collect();
}

#[cfg(test)]
mod tests {
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
                    provides: vec!["libgtk-4.so.1".into()],
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
                    provides: vec!["libadwaita-1.so.0".into()],
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
                    provides: vec!["libsysprof-6.so.6".into()],
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
}
