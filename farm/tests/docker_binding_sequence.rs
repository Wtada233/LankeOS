//! docker 编排序列锁定：用 PATH 上的假 `docker` 影子脚本记录每次调用的 argv，断言
//! `RealBinding::build` 的**子命令顺序**在 `docker_build` 拆步后保持不变。
//! 这是 B3（拆步重构）"语义不变"的可执行证据，且零容器成本。
//! PATH 是进程级全局：本文件只放这一个测试，避免与其它测试并发互扰。
use std::fs;
use std::path::PathBuf;

use lankefarm::lpkg_binding::docker::{CleanupState, RealBinding};
use lankefarm::lpkg_binding::LpkgBinding;

/// 造一个最小合法 .lpkg（zstd tar：metadata.json + content/libfoo.so），供假 docker "拷回宿主"。
fn make_lpkg(path: &PathBuf) {
    let src = path.with_extension("src");
    let _ = fs::remove_dir_all(&src);
    fs::create_dir_all(src.join("content")).unwrap();
    fs::write(
        src.join("content/libfoo.so"),
        [0x7f, b'E', b'L', b'F', 2, 1, 1],
    )
    .unwrap();
    fs::write(
        src.join("metadata.json"),
        r#"{"name":"demo","version":"1.0","deps":[],"provides":["libdemo.so.1"],"needed_so":[]}"#,
    )
    .unwrap();
    let f = fs::File::create(path).unwrap();
    let enc = zstd::stream::write::Encoder::new(f, 3).unwrap();
    let mut b = tar::Builder::new(enc);
    b.append_dir_all(".", &src).unwrap();
    let enc = b.into_inner().unwrap();
    enc.finish().unwrap();
    let _ = fs::remove_dir_all(&src);
}

#[test]
fn docker_build_subcommand_sequence_is_stable() {
    let base = std::env::temp_dir().join(format!("farm-dockerseq-{}", std::process::id()));
    let _ = fs::remove_dir_all(&base);
    let bin = base.join("bin");
    let out = base.join("out");
    let pkgs = base.join("pkgs");
    fs::create_dir_all(&bin).unwrap();
    fs::create_dir_all(out.join("backups")).unwrap();
    fs::create_dir_all(pkgs.join("demo")).unwrap();
    fs::write(pkgs.join("demo/LankeBUILD.json"), "{}").unwrap();

    // 预置一个合法 .lpkg，假 docker 把它当作"容器产物"拷回 staging（否则 scan 失败）
    let fixture = base.join("fixture.lpkg");
    make_lpkg(&fixture);

    let log = base.join("docker.log");
    let script = format!(
        r#"#!/bin/bash
printf '%s\n' "$*" >> "{log}"
case "$1" in
  create) echo deadbeefcafe ;;
  exec)
    for a in "$@"; do
      case "$a" in *"ls -1 *.lpkg"*) echo "demo-1.0.lpkg"; exit 0;; esac
    done
    exit 0 ;;
  cp)
    last="${{@: -1}}"
    case "$last" in
      *:*) exit 0 ;;
      *)   cp "{fixture}" "$last/demo-1.0.lpkg"; exit 0 ;;
    esac ;;
esac
exit 0
"#,
        log = log.display(),
        fixture = fixture.display()
    );
    fs::write(bin.join("docker"), script).unwrap();
    use std::os::unix::fs::PermissionsExt;
    fs::set_permissions(bin.join("docker"), fs::Permissions::from_mode(0o755)).unwrap();

    let old_path = std::env::var("PATH").unwrap_or_default();
    std::env::set_var("PATH", format!("{}:{}", bin.display(), old_path));

    let cleanup = std::sync::Arc::new(std::sync::Mutex::new(CleanupState {
        out_dir: out.clone(),
        base_image: "fake:latest".into(),
        ..Default::default()
    }));
    let mut b = RealBinding::new("fake:latest", &pkgs, &out, "x86_64", 80, cleanup.clone());
    b.set_repo_provides(Default::default());
    let outcome = b.build("demo");

    std::env::set_var("PATH", old_path);

    assert!(outcome.ok, "假 docker 下 build 应成功: {outcome:?}");

    let calls = fs::read_to_string(&log).unwrap();
    let verbs: Vec<&str> = calls
        .lines()
        .filter_map(|l| l.split_whitespace().next())
        .collect();
    // 顺序不变量（拆步前后必须一致）：前置清理 → create → start → mirror → upgrade → commit
    //   → cp(备份) → restore → cp(配方) → build → ls → cp(产物) → rm -f（ContainerGuard drop）
    assert_eq!(
        verbs,
        vec![
            "ps", "image", "create", "start", "exec", "exec", "commit", "cp", "exec", "cp", "exec",
            "exec", "cp", "rm"
        ],
        "docker 子命令序列变了（拆步必须保持顺序）: \n{calls}"
    );
    // 关键时序：commit 必须早于"恢复备份/拷配方"（否则旧 .so 与源码会滚进 roll 镜像）
    let idx = |v: &str| verbs.iter().position(|x| *x == v).unwrap();
    assert!(idx("commit") < verbs.len() - 1);
    let commit_at = verbs.iter().position(|x| *x == "commit").unwrap();
    let last_cp_backups = calls
        .lines()
        .position(|l| l.contains(":/backups"))
        .expect("应注入备份");
    assert!(
        commit_at < calls.lines().position(|l| l.contains(":/backups")).unwrap(),
        "restore 必须发生在 commit 之后"
    );
    let _ = last_cp_backups;
    // 收尾容器恰好被删一次，且 current_cid 已清空
    assert_eq!(verbs.iter().filter(|v| **v == "rm").count(), 1, "{calls}");
    assert!(cleanup.lock().unwrap().current_cid.is_none());

    let _ = fs::remove_dir_all(&base);
}
