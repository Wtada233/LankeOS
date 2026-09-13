//! 架构守护测试：把 ARCH.md 里的分层不变量变成可执行断言（"注释/实现不漂移"的护栏）。
//! 这些断言便宜、不碰网络/容器，却能挡住"以后顺手在别处 spawn docker / 去掉超时"的回归。

/// 架构 §5：所有 docker 交互收敛在 `lpkg_binding/docker.rs` 这一个叶；CLI 与逻辑层不得直接 spawn。
#[test]
fn docker_only_spawned_in_binding_leaf() {
    for (name, src) in [
        ("src/cli/build.rs", include_str!("../src/cli/build.rs")),
        ("src/cli/mod.rs", include_str!("../src/cli/mod.rs")),
        ("src/build/mod.rs", include_str!("../src/build/mod.rs")),
        (
            "src/lpkg_binding/mod.rs",
            include_str!("../src/lpkg_binding/mod.rs"),
        ),
        ("src/serve.rs", include_str!("../src/serve.rs")),
    ] {
        assert!(
            !src.contains(r#"Command::new("docker")"#),
            "{name} 不得直接 spawn docker（应收敛到 lpkg_binding/docker.rs）"
        );
    }
}

/// HTTP 客户端必须设**读写空闲**超时：ureq 2 默认 read/write 无超时，卡死连接会永久 hang。
#[test]
fn http_client_sets_read_and_write_timeouts() {
    let src = include_str!("../src/net.rs");
    assert!(src.contains("timeout_read"), "net.rs 应设 timeout_read");
    assert!(src.contains("timeout_write"), "net.rs 应设 timeout_write");
}
