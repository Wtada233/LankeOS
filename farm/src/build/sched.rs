//! sched.rs — 构建顺序（拓扑 + 环切割 + ABI 受害者重排）。纯排序，无构建副作用。

use std::cmp::Reverse;
use std::collections::{BinaryHeap, HashMap, HashSet, VecDeque};
use std::path::Path;

use crate::build::groups::RebuildGroups;
use crate::graph::Index;
use crate::tr;
use crate::ux;

/// Kahn 拓扑排序，**按 needed_so 链接边 + 声明式重建组边 + farm_flags 边**＋环切割。
///
/// 设计：构建序只看**链接依赖**——需要重建的链接库必须先建，依赖者才能按新 ABI 链接。
/// `deps`/`build_deps` **默认不参与排序**：build 工具由每个容器 `lpkg upgrade` 从 repo 拿最新版，
/// 无需排队。"不链 libpython 但 ABI 敏感"的包（python-cairo/gobject/blueman/meson…）由
/// `data/build/*.yaml` 声明式重建组处理（`build/groups.rs`）——这些组受害者没有 needed_so 链接边，
/// 必须靠 `extra_edges`（victim → on）强制排在触发包 `on` 之后，否则 `--all` 模式下 python-cairo
/// 会在 python 重建前构建（容器升级时 repo 还是旧 python，构建基于旧 ABI 白跑）。
///
/// `pkgs_dir` 用来读配方（LankeBUILD.json）：声明了 `BUILD_AFTER_BUILD_DEPS`（farm_flags）
/// 的包，其 `build_deps` **也作为依赖边**参与排序（见 `build/farm_flags.rs`）——构建期
/// 需要另一个也在本轮重建的包先产出时（如 python-bar 构建要 python-foo），必须等它先建。
/// 与链接边/组边同样**只对 targets 内的包生效**：build_deps 指向本轮不重建的包 → 边丢弃，
/// 该包直接构建不等待。
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
    for n in targets {
        let mut deps: Vec<String> = crate::graph::link_deps(old, n)
            .into_iter()
            .filter(|d| d.as_str() != n.as_str() && names.contains(d.as_str()))
            .collect();
        if let Some(gd) = group_deps.get(n) {
            deps.extend(gd.iter().cloned());
        }
        // farm_flags：BUILD_AFTER_BUILD_DEPS → 该包 build_deps 也进依赖边（仅限 targets 内）。
        // 目标依赖不重建 → 边丢弃，不等待（与链接/组边同规则）。
        let flags = crate::build::farm_flags::flags_of(pkgs_dir, n);
        if flags.contains(&crate::build::farm_flags::FarmFlag::BuildAfterBuildDeps) {
            if let Some(lb) = crate::build::read_lankebuild(pkgs_dir, n) {
                deps.extend(
                    lb.build_deps
                        .iter()
                        .filter(|d| d.as_str() != n.as_str() && names.contains(d.as_str()))
                        .cloned(),
                );
            }
        }
        deps.sort();
        deps.dedup();
        graph.insert(n.clone(), deps.clone());
        in_deg.insert(n.clone(), deps.len());
        for d in deps {
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
            // 剩余节点构成环：三色 DFS 找一条后向边切断，警告后继续（循环会逐条断开）
            if let Some((u, v)) = find_cycle_edge(&graph, &in_deg) {
                eprintln!("  {}", ux::yellow(&tr!("build.cycle", u, v)));
                graph.get_mut(&u).unwrap().retain(|x| x != &v);
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

/// 在剩余子图（未就绪，in_deg > 0）中找一条构成环的后向边 (u→v)。三色 DFS，确定性。
/// 子图无环返回 None。
fn find_cycle_edge(
    graph: &HashMap<String, Vec<String>>,
    in_deg: &HashMap<String, usize>,
) -> Option<(String, String)> {
    let mut nodes: Vec<String> = graph
        .keys()
        .filter(|n| in_deg.get(*n).copied().unwrap_or(0) > 0)
        .cloned()
        .collect();
    nodes.sort();
    if nodes.is_empty() {
        return None;
    }
    let rem: HashSet<String> = nodes.iter().cloned().collect();
    let mut color: HashMap<String, u8> = HashMap::new(); // 0=白 1=灰 2=黑
    for n in &nodes {
        color.insert(n.clone(), 0);
    }
    for root in &nodes {
        if color.get(root).copied().unwrap_or(0) != 0 {
            continue;
        }
        color.insert(root.clone(), 1);
        let mut stack: Vec<(String, usize)> = vec![(root.clone(), 0)]; // (node, 邻接索引)
        while let Some((u, i)) = stack.last().cloned() {
            let neighbors = graph.get(&u).map(|v| v.as_slice()).unwrap_or(&[]);
            if i < neighbors.len() {
                let v = &neighbors[i];
                if let Some(top) = stack.last_mut() {
                    top.1 += 1;
                }
                if !rem.contains(v) {
                    continue;
                }
                match color.get(v).copied().unwrap_or(0) {
                    1 => return Some((u, v.clone())), // u→v 后向边：u、v 同栈=成环
                    0 => {
                        color.insert(v.clone(), 1);
                        stack.push((v.clone(), 0));
                    }
                    _ => {}
                }
            } else {
                color.insert(u.clone(), 2);
                stack.pop();
            }
        }
    }
    None
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
