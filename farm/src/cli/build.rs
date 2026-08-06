use std::io::IsTerminal;
use std::path::PathBuf;
use std::process::ExitCode;
use std::sync::{Arc, Mutex};
use lankefarm::build::{self, BuildOptions};
use lankefarm::lpkg_binding::{CleanupState, RealBinding};
use super::Args;

pub(crate) fn cmd_build(args: &Args) -> ExitCode {
    let pkgs_dir = args.pkgs.clone().unwrap_or_else(|| "pkgs".to_string());
    let out_dir = args.out.clone().unwrap_or_else(|| PathBuf::from("out"));

    // --all → 空 targets（run_build 内部做全量 + 版本增量跳过：effective_version 与本地 repo 一致则跳过）
    let targets: Vec<String> = if args.all {
        Vec::new()
    } else if !args.pkg.is_empty() {
        args.pkg.clone()
    } else {
        eprintln!("farm build <pkg>... | --all");
        return ExitCode::from(2);
    };
    run_build_flow(args, pkgs_dir, out_dir, targets, /*validate=*/false)
}

/// validate：自动重建所有没有 `.build_ok` 标记的包（成功构建才写标记；跳过/blocked 不写）。
/// 排序与增量构建一致（run_build 内部同一 topo_order + ABI 传播）。
pub(crate) fn cmd_validate(args: &Args) -> ExitCode {
    let pkgs_dir = args.pkgs.clone().unwrap_or_else(|| "pkgs".to_string());
    let out_dir = args.out.clone().unwrap_or_else(|| PathBuf::from("out"));
    run_build_flow(args, pkgs_dir, out_dir, Vec::new(), /*validate=*/true)
}

/// build / validate 共用完整流程：state → image 校验 → 内嵌 serve → RealBinding → run_build → 汇总。
fn run_build_flow(
    args: &Args,
    pkgs_dir: String,
    out_dir: PathBuf,
    targets: Vec<String>,
    validate: bool,
) -> ExitCode {
    // SQLite 状态（§11）：job 状态 + 构建历史（增量由 run_build 的版本对比驱动）
    let state_path = args
        .state
        .clone()
        .unwrap_or_else(|| out_dir.join("farm-state.db"));
    let state = match lankefarm::state::State::open(&state_path) {
        Ok(s) => {
            println!("{}", lankefarm::tr!("state.open", state_path.display()));
            Some(s)
        }
        Err(e) => {
            eprintln!("{}", lankefarm::tr!("state.open_fail", e));
            None
        }
    };

    // 仅容器构建：--image 必填（禁止主机 lpkg build 污染宿主环境）
    let base_image = match args.image.as_deref() {
        Some(i) if !i.is_empty() => i.to_string(),
        _ => {
            eprintln!("{}", lankefarm::tr!("build.need_image"));
            return ExitCode::from(2);
        }
    };
    let arch = args.arch.clone().unwrap_or_else(|| "x86_64".to_string());
    let repo_port = args.repo_port.unwrap_or(80);

    // Ctrl+C 中断清理：rm 当前容器 → 删 DB 当前条目 → finalize（最新 commit 覆盖 base + 删 roll 镜像）。
    let cleanup = Arc::new(Mutex::new(CleanupState {
        current_cid: None,
        current_pkg: None,
        out_dir: out_dir.clone(),
        base_image: base_image.clone(),
        state_path: state_path.clone(),
    }));
    {
        let c = cleanup.clone();
        if let Err(e) = ctrlc::set_handler(move || {
            let s = c.lock().unwrap();
            if let Some(cid) = &s.current_cid {
                let _ = std::process::Command::new("docker")
                    .args(["rm", "-f", cid])
                    .status();
            }
            if let Some(pkg) = &s.current_pkg {
                if let Ok(st) = lankefarm::state::State::open(&s.state_path) {
                    let _ = st.delete_job(pkg);
                }
            }
            let _ = lankefarm::lpkg_binding::finalize_roll(&s.out_dir, &s.base_image);
            eprintln!("\n[ctrl-c] 已清理：容器 / DB 条目 / 滚动镜像，baseline 已用最新 commit 覆盖");
            std::process::exit(130);
        }) {
            eprintln!("  [warn] 安装 Ctrl+C 处理器失败（中断将不自动清理）: {e}");
        }
    }
    // 内嵌本地 repo 服务器（容器 lpkg upgrade 从这拉依赖，§8）。
    // serve_ready 绑定成功后经 channel 确认就绪；绑定失败（如非 root 绑默认端口 80）
    // 立即暴露并退出，不再静默吞掉 + 盲等 300ms。
    println!("[serve] 内嵌本地 repo 服务器 http://127.0.0.1:{repo_port}（docker 模式）");
    let (serve_tx, serve_rx) = std::sync::mpsc::channel::<Result<(), String>>();
    let serve_handle = {
        let repo_root = out_dir.clone();
        let port = repo_port;
        std::thread::spawn(move || {
            let res = lankefarm::serve::serve_ready("127.0.0.1", &repo_root, port, |_actual| {
                serve_tx
                    .send(Ok(()))
                    .map_err(|e| format!("serve 就绪信号发送失败: {e}"))
            });
            if let Err(e) = &res {
                let _ = serve_tx.send(Err(e.clone()));
            }
            res
        })
    };
    match serve_rx.recv_timeout(std::time::Duration::from_secs(5)) {
        Ok(Ok(())) => {}
        Ok(Err(e)) => {
            eprintln!("{}", lankefarm::tr!("build.serve_fail", repo_port, e));
            drop(serve_handle);
            return ExitCode::from(2);
        }
        Err(_) => {
            eprintln!(
                "{}",
                lankefarm::tr!("build.serve_fail", repo_port, "5s 内未就绪")
            );
            drop(serve_handle);
            return ExitCode::from(2);
        }
    }

    // RealBinding：--image 走 fresh container 编排（§8），否则宿主 lpkg build
    let mut binding = RealBinding::new(
        base_image.clone(),
        pkgs_dir.clone(),
        out_dir.clone(),
        arch.clone(),
        repo_port,
        cleanup.clone(),
    );
    let opts = BuildOptions {
        pkgs_dir: PathBuf::from(&pkgs_dir),
        out_dir: out_dir.clone(),
        targets,
        arch,
        image: base_image.clone(),
        download_retries: args.download_retries.unwrap_or(3),
        interactive: std::io::stdin().is_terminal(),
        build_data_dir: PathBuf::from("data/build"),
        validate,
    };
    let report = match build::run_build(&opts, &mut binding, state.as_ref()) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("{e}");
            return ExitCode::from(2);
        }
    };

    // 整个 build 流程完毕后收尾：最新 commit 扁平化覆盖 base、删全部 roll 镜像、计数归零。
    // （Ctrl+C 时由信号处理器做同样的事）
    if let Err(e) = lankefarm::lpkg_binding::finalize_roll(&out_dir, &base_image) {
        eprintln!("  [warn] build 收尾 finalize 失败（滚动镜像残留，下次构建会重试）: {e}");
    }

    // 进程退出即关停内嵌 serve（无状态静态服务器，无需优雅关闭）
    drop(serve_handle);

    println!();
    println!(
        "{}",
        lankefarm::tr!(
            "summary.title",
            lankefarm::ux::green(&lankefarm::tr!("summary.built", report.built.len())),
            lankefarm::ux::dim(&lankefarm::tr!("summary.repacked", report.repacked.len())),
            lankefarm::ux::dim(&lankefarm::tr!("summary.abi_broken", report.abi_broken.len())),
            lankefarm::ux::dim(&lankefarm::tr!("summary.skipped_cnt", report.skipped.len())),
            lankefarm::ux::dim(&lankefarm::tr!("summary.source_missing", report.source_missing.len())),
            lankefarm::ux::red(&lankefarm::tr!("summary.blocked_cnt", report.blocked.len()))
        )
    );
    if !report.source_missing.is_empty() {
        println!("  {}", lankefarm::ux::yellow(&lankefarm::tr!("summary.source_missing", report.source_missing.join(", "))));
    }
    if !report.blocked.is_empty() {
        println!("  {}", lankefarm::ux::red(&lankefarm::tr!("summary.blocked", report.blocked.join(", "))));
    }
    if !report.skipped.is_empty() {
        println!("  {}", lankefarm::tr!("summary.skipped", report.skipped.join(", ")));
    }
    ExitCode::SUCCESS
}
