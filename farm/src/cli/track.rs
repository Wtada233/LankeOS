//! track 子命令：串行单包 + 并行 `--all` 引擎。
//!
//! `use super::*;` 拿到父模块的共享 helper（子模块可见祖先的私有项）。

use super::*;
use std::cmp::Ordering as CmpOrdering;
use std::collections::{HashMap, HashSet, VecDeque};
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread;

pub(super) fn cmd_track_run(args: &TrackArgs, apply: bool, probe_fail_continue: bool) -> ExitCode {
    let pkgs_dir = args.pkgs.to_string_lossy().into_owned();
    let data_dir = args.data.to_string_lossy().into_owned();
    let trackers = load_trackers(&data_dir);
    let fetcher = build_fetcher(args);
    let mut errored = false;
    for pkg in &args.pkg {
        // 包来源是 LankeBUILD 体系
        let build = match load_build_json(&PathBuf::from(&pkgs_dir).join(pkg)) {
            Ok(b) => b,
            Err(e) => {
                eprintln!("{e}");
                errored = true;
                continue;
            }
        };
        // file:// 或无远程源：无需 track（本地产物，无上游可追踪）。
        // sources 和 work_sources 都是下载器字段，任一有远程 URL 就可追踪（work_sources-only 包如 noto 字体）。
        let has_remote = super::has_remote_source(&build);
        if !has_remote {
            println!("{}", lankefarm::tr!("track.skip_no_remote", pkg));
            continue;
        }
        // 按 name 字段匹配 tracker（文件名无关）
        let cfg = match trackers.get(&build.name) {
            Some(c) => c,
            None => {
                eprintln!("{}", lankefarm::tr!("track.no_tracker_pkg", build.name));
                errored = true;
                continue;
            }
        };
        let pkg_name = build.name.clone();
        // 本轮解析出的新版本：供**本轮之后处理的其他包**读取（如 moby 的 `same-version-of: docker`）。
        // 注意它在 `propose_with` **返回之后**才写入 —— 所以**同包**条目引用不到自己，那类场景要用
        // 条目级 `same-version-of-source: sources[0]`（见 templates/same_version_of_source.rs）。
        let pending_new = std::cell::RefCell::new(None::<String>);
        // 版本约束解析：优先本轮新版本，其次读其他包的 LankeBUILD.json 版本（same-version / major-of）
        let lookup = |pkg: &str| -> Option<String> {
            if pkg == pkg_name {
                if let Some(v) = pending_new.borrow().as_ref() {
                    return Some(v.clone());
                }
            }
            load_build_json(&PathBuf::from(&pkgs_dir).join(pkg))
                .ok()
                .map(|b| b.version)
        };
        match cfg.propose_with(&fetcher, &lookup, &build.version) {
            Ok(p) => match vercmp::cmp_version(&p.new_version, &p.current_version) {
                CmpOrdering::Greater => {
                    println!(
                        "{}",
                        lankefarm::tr!(
                            "track.proposal",
                            p.pkg_name,
                            p.current_version,
                            p.new_version,
                            p.kind
                        )
                    );
                    for s in &p.sources {
                        println!("  {s}");
                    }
                    for s in &p.work_sources {
                        println!("{}", lankefarm::tr!("track.work_sources", s));
                    }
                    // 记录本轮新版本：供**本轮之后处理的其他包**（如 moby 的 same-version-of: docker）读取；
                    // 同包内要引用自己请用条目级 same-version-of-source（此处写入时本包已探测完毕）
                    *pending_new.borrow_mut() = Some(p.new_version.clone());
                    // 源 URL 可达性探测：任一失败 → 告警；写入需 `--probe-fail-continue` 才豁免
                    let mut probe_failed = false;
                    for s in p.sources.iter().chain(&p.work_sources) {
                        if let Err(e) = lankefarm::net::probe_source(s) {
                            eprintln!(
                                "{}",
                                lankefarm::tr!("track.probe_source_fail", p.pkg_name, e)
                            );
                            probe_failed = true;
                        }
                    }
                    if apply {
                        if probe_failed && !probe_fail_continue {
                            println!("{}", lankefarm::tr!("track.probe_fail_skip", p.pkg_name));
                        } else {
                            apply_proposal(pkg, Path::new(&pkgs_dir), &p);
                        }
                    }
                }
                CmpOrdering::Equal => {
                    println!(
                        "{}",
                        lankefarm::tr!("track.latest", p.pkg_name, p.current_version)
                    );
                }
                CmpOrdering::Less => {
                    error_log!(
                        "{}",
                        lankefarm::tr!(
                            "track.regress",
                            p.pkg_name,
                            p.current_version,
                            p.new_version
                        )
                    );
                }
            },
            Err(e) => {
                error_log!("{}", lankefarm::tr!("track.probe_fail", build.name, e));
                errored = true;
            }
        }
    }
    if errored {
        ExitCode::from(2)
    } else {
        ExitCode::SUCCESS
    }
}

/// track --all 并行调度器状态（worker 共享，单 Mutex 防死锁）。
struct TrackSched {
    /// 就绪队列（前置已全部解析的包）
    queue: VecDeque<String>,
    /// 每个包剩余未解析的前置数（=0 才可出队）
    indeg: HashMap<String, usize>,
    /// 环兜底被"强制就绪"的包：不再递减其 indeg（保持真实计数，避免 0 下溢）
    forced: HashSet<String>,
    /// 尚未完成探测的包数（=0 即全部结束）
    remaining: usize,
    /// 本轮已解析出的新版本（供 same-version / major-of 读取）
    resolved: HashMap<String, String>,
}

/// worker：从就绪队列取包探测，完成后释放其依赖者（入度减到 0 才入队）。
/// 入度门控保证 `after(<pkg>)` / `last` / same-version / major-of 的前置先完成；
/// resolved 在释放依赖者前已写入（同一临界区），依赖者读到的是新版本而非 LankeBUILD.json 旧版本。
#[allow(clippy::too_many_arguments)] // worker 的显式参数比包装结构体更易读
fn track_worker(
    sched: &(Mutex<TrackSched>, Condvar),
    configs: &HashMap<String, TrackerConfig>,
    versions: &HashMap<String, String>,
    dependents: &HashMap<String, Vec<String>>,
    pkg_to_dir: &HashMap<String, String>,
    pkgs_dir: &Path,
    fetcher: &RealFetcher,
    proposals: &AtomicUsize,
    errors: &AtomicUsize,
    apply: bool,
    probe_fail_continue: bool,
) {
    loop {
        // 取一个就绪包；队列空但未完成则等待
        let name = {
            let mut guard = sched.0.lock().unwrap();
            while guard.queue.is_empty() && guard.remaining > 0 {
                guard = sched.1.wait(guard).unwrap();
            }
            if guard.queue.is_empty() {
                return; // 全部完成
            }
            guard.queue.pop_front().unwrap()
        };
        let cfg = &configs[&name];
        let current = &versions[&name];
        // pkg-name → 目录：same-version/major-of/after 引用的都是 pkg-name，目录名可能不同
        let dir_of = |pkg: &str| {
            pkg_to_dir
                .get(pkg)
                .cloned()
                .unwrap_or_else(|| pkg.to_string())
        };
        let lookup = |pkg: &str| {
            if let Some(v) = sched.0.lock().unwrap().resolved.get(pkg) {
                return Some(v.clone());
            }
            load_build_json(&pkgs_dir.join(dir_of(pkg)))
                .ok()
                .map(|b| b.version)
        };
        let result = cfg.propose_with(fetcher, &lookup, current);

        // 源 URL 可达性探测（网络 I/O，锁外）：仅新版提案才探（Equal/Less 无写入，不浪费）
        let mut probe_failed = false;
        let is_newer = matches!(
            &result,
            Ok(p) if vercmp::cmp_version(&p.new_version, &p.current_version) == CmpOrdering::Greater
        );
        if is_newer {
            if let Ok(p) = &result {
                for s in p.sources.iter().chain(&p.work_sources) {
                    if let Err(e) = lankefarm::net::probe_source(s) {
                        eprintln!(
                            "{}",
                            lankefarm::tr!("track.probe_source_fail", p.pkg_name, e)
                        );
                        probe_failed = true;
                    }
                }
            }
        }

        // 完成簿记：先写 resolved，再释放依赖者（同一临界区，顺序保证）
        let mut guard = sched.0.lock().unwrap();
        let mut pending_apply: Option<lankefarm::track::Proposal> = None;
        match result {
            Ok(p) => match vercmp::cmp_version(&p.new_version, &p.current_version) {
                CmpOrdering::Greater => {
                    // 与串行路径（cmd_track_run）走**同一个** i18n 键：否则 LANG=en 下
                    // `track --all` 打中文、`track <pkg>` 打英文
                    println!(
                        "{}",
                        lankefarm::tr!(
                            "track.proposal",
                            p.pkg_name,
                            p.current_version,
                            p.new_version,
                            p.kind
                        )
                    );
                    // 只有更新的版本才参与后续 same-version / major-of 约束
                    guard.resolved.insert(name.clone(), p.new_version.clone());
                    proposals.fetch_add(1, Ordering::Relaxed);
                    pending_apply = Some(p); // 移到 apply 候选（apply 模式才写）
                }
                CmpOrdering::Equal => {
                    println!(
                        "{}",
                        lankefarm::tr!("track.latest", p.pkg_name, p.current_version)
                    );
                }
                CmpOrdering::Less => {
                    error_log!(
                        "{}",
                        lankefarm::tr!(
                            "track.regress",
                            p.pkg_name,
                            p.current_version,
                            p.new_version
                        )
                    );
                    errors.fetch_add(1, Ordering::Relaxed);
                }
            },
            Err(e) => {
                error_log!(
                    "{}",
                    lankefarm::tr!("track.probe_fail_all", cfg.pkg_name, e)
                );
                errors.fetch_add(1, Ordering::Relaxed);
            }
        }
        if let Some(deps) = dependents.get(&name) {
            for d in deps {
                // 环内被强制的包：保持其 indeg 真实计数不变（否则 0-1 下溢），
                // 且它们已在初始就绪集里，无需再次入队。
                if guard.forced.contains(d.as_str()) {
                    continue;
                }
                let e = guard.indeg.get_mut(d).unwrap();
                *e -= 1;
                if *e == 0 {
                    guard.queue.push_back(d.clone());
                }
            }
        }
        guard.remaining -= 1;
        drop(guard);
        sched.1.notify_all();

        // 批量应用（--all --run）：释放锁后再写 LankeBUILD.json，避免持锁做 I/O/网络探测。
        // 源探测失败时：需 --probe-fail-continue 才继续写（否则跳过并已在上方告警）。
        if apply {
            if let Some(p) = pending_apply {
                if probe_failed && !probe_fail_continue {
                    println!("{}", lankefarm::tr!("track.probe_fail_skip", p.pkg_name));
                } else {
                    // 写回目标目录 = pkg-name 对应的目录（目录名可能与 pkg-name 不同）
                    apply_proposal(&dir_of(&p.pkg_name), pkgs_dir, &p);
                }
            }
        }
    }
}

/// 遍历 pkgs/（LankeBUILD 体系是来源）为每个有 tracker 的包探测 → 提案汇总。
/// `-j N` 并行探测：与串行 `order_entries` 同一套 `dep_edges` 做入度门控——
/// `after(<pkg>)` / `last` / same-version / major-of 的前置先解析，并行不破坏顺序。
/// `--all` 全量 / 多包名（-j 并行）共用：遍历目标包 → 依赖图门控 worker 池 → 探测 → 汇总。
/// `restrict` 为 Some(包目录名列表) 时只处理指定包（多包名 track 走这里，-j 生效）；
/// None 时处理 pkgs/ 下全部有 tracker 的包（--all）。
pub(super) fn cmd_track_all(
    args: &TrackArgs,
    apply: bool,
    probe_fail_continue: bool,
    restrict: Option<&[String]>,
) -> ExitCode {
    let pkgs_dir = args.pkgs.to_string_lossy().into_owned();
    let data_dir = args.data.to_string_lossy().into_owned();
    let root = PathBuf::from(&pkgs_dir);
    if !root.is_dir() {
        eprintln!("{}", lankefarm::tr!("pkgs.not_dir", pkgs_dir));
        return ExitCode::from(2);
    }
    let jobs = args.jobs.unwrap_or(1).max(1);
    let trackers = load_trackers(&data_dir);
    let entries: Vec<String> = match &restrict {
        Some(names) => names.to_vec(),
        None => match std::fs::read_dir(&root) {
            Ok(rd) => rd
                .filter_map(|e| e.ok())
                .filter(|e| e.path().is_dir())
                .map(|e| e.file_name().to_string_lossy().into_owned())
                .collect(),
            Err(e) => {
                eprintln!("{}", lankefarm::tr!("pkgs.read_fail", pkgs_dir, e));
                return ExitCode::from(2);
            }
        },
    };

    // 探测集：有 tracker 且有有效 LankeBUILD.json 的包；无 tracker 的走 no_tracker 计数。
    // **统一键为 pkg-name（build.name）**：trackers/dep_edges 都以 tracker 的 pkg-name 为键，
    // 目录名可能与 pkg-name 不一致（LankeBUILD.json 的 name 字段 ≠ 目录名）。
    // 曾用目录名做 configs/versions 的键 → dep_edges 按 pkg-name 查不到 → order/same-version/
    // major-of 边被静默丢弃；apply_proposal 也把 pkg-name 当目录名写，会写到错误路径。
    // pkg_to_dir 维护 pkg-name → 目录 的映射，供文件读写（lookup / apply）。
    let mut configs: HashMap<String, TrackerConfig> = HashMap::new();
    let mut versions: HashMap<String, String> = HashMap::new();
    let mut no_tracker: Vec<String> = Vec::new();
    let mut pkg_to_dir: HashMap<String, String> = HashMap::new();
    let mut sorted = entries.clone();
    sorted.sort();
    for name in &sorted {
        let Ok(build) = load_build_json(&root.join(name)) else {
            continue;
        };
        match trackers.get(&build.name) {
            Some(cfg) => {
                configs.insert(build.name.clone(), cfg.clone());
                versions.insert(build.name.clone(), build.version);
                pkg_to_dir.insert(build.name.clone(), name.clone());
            }
            None => no_tracker.push(name.clone()),
        }
    }

    // 依赖图（与 order_entries 同一套 dep_edges）：入度门控并行
    let probe_names: Vec<String> = configs.keys().cloned().collect();
    let edges = dep_edges(&probe_names, &trackers);
    let mut indeg: HashMap<String, usize> = configs.keys().map(|n| (n.clone(), 0)).collect();
    let mut dependents: HashMap<String, Vec<String>> = HashMap::new();
    for (a, b) in &edges {
        *indeg.get_mut(b).unwrap() += 1;
        dependents.entry(a.clone()).or_default().push(b.clone());
    }
    // 环兜底：Kahn 模拟找出被环阻塞（indeg 无法归零）的包，强制置 0 立即就绪，避免 worker 永久等待
    let mut sim_indeg = indeg.clone();
    let mut sim_q: VecDeque<String> = probe_names
        .iter()
        .filter(|n| sim_indeg[n.as_str()] == 0)
        .cloned()
        .collect();
    let mut sim_done = 0;
    while let Some(n) = sim_q.pop_front() {
        sim_done += 1;
        if let Some(deps) = dependents.get(&n) {
            for d in deps {
                *sim_indeg.get_mut(d).unwrap() -= 1;
                if sim_indeg[d.as_str()] == 0 {
                    sim_q.push_back(d.clone());
                }
            }
        }
    }
    // 环兜底：被环阻塞的包记入 forced 集（强制就绪），但**不修改真实 indeg**——
    // 曾直接置 0，worker 释放依赖者时对已是 0 的 indeg 执行 `*e -= 1` → debug 构建
    // panic / release 构建 usize::MAX 下溢（未定义行为）。
    let mut forced: HashSet<String> = HashSet::new();
    if sim_done < probe_names.len() {
        for n in &probe_names {
            if sim_indeg[n.as_str()] > 0 {
                forced.insert(n.clone());
            }
        }
    }
    let mut ready: Vec<String> = probe_names
        .iter()
        .filter(|n| indeg[n.as_str()] == 0 || forced.contains(n.as_str()))
        .cloned()
        .collect();
    ready.sort();

    let sched = Arc::new((
        Mutex::new(TrackSched {
            queue: ready.into(),
            indeg,
            forced,
            remaining: configs.len(),
            resolved: HashMap::new(),
        }),
        Condvar::new(),
    ));
    let proposals = Arc::new(AtomicUsize::new(0));
    let errors = Arc::new(AtomicUsize::new(0));
    let configs = Arc::new(configs);
    let versions = Arc::new(versions);
    let dependents = Arc::new(dependents);
    let pkg_to_dir = Arc::new(pkg_to_dir);
    let fetcher = Arc::new(build_fetcher(args));

    if jobs > 1 {
        println!("{}", lankefarm::tr!("track.parallel", jobs));
    }
    let mut handles = Vec::new();
    for _ in 0..jobs {
        let sched = Arc::clone(&sched);
        let proposals = Arc::clone(&proposals);
        let errors = Arc::clone(&errors);
        let configs = Arc::clone(&configs);
        let versions = Arc::clone(&versions);
        let dependents = Arc::clone(&dependents);
        let pkg_to_dir = Arc::clone(&pkg_to_dir);
        let fetcher = Arc::clone(&fetcher);
        let root = root.clone();
        handles.push(thread::spawn(move || {
            track_worker(
                &sched,
                &configs,
                &versions,
                &dependents,
                &pkg_to_dir,
                &root,
                &fetcher,
                &proposals,
                &errors,
                apply,
                probe_fail_continue,
            );
        }));
    }
    for h in handles {
        h.join().expect("track worker panicked");
    }

    let proposals = proposals.load(Ordering::Relaxed);
    let errors = errors.load(Ordering::Relaxed);
    // 孤儿 tracker = 无对应包（pkg-name 不在 pkg_to_dir 里）。曾按目录名比较，
    // 目录名 ≠ pkg-name 时会把有效 tracker 误报为孤儿。
    // restrict（多包名）模式下不算 orphans：restrict 集外的 tracker 天然不在 pkg_to_dir，误报。
    let orphans: Vec<String> = if restrict.is_some() {
        Vec::new()
    } else {
        trackers
            .keys()
            .filter(|n| !pkg_to_dir.contains_key(n.as_str()))
            .cloned()
            .collect()
    };
    println!();
    let summary = lankefarm::tr!(
        "track.summary",
        entries.len(),
        if apply {
            lankefarm::tr!("track.summary_applied")
        } else {
            lankefarm::tr!("track.summary_proposals")
        },
        proposals,
        errors,
        no_tracker.len(),
        orphans.len()
    );
    println!("{summary}");
    log(&summary);
    if !no_tracker.is_empty() {
        println!(
            "{}",
            lankefarm::tr!("track.no_tracker", no_tracker.join(", "))
        );
    }
    if !orphans.is_empty() {
        println!("{}", lankefarm::tr!("track.orphans", orphans.join(", ")));
    }
    ExitCode::SUCCESS
}
