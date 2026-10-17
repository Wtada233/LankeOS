# farm/fuzz

`cargo-fuzz` harness：`extract_lpkg`（解包路径）、`read_member`（流式取成员）。要 nightly。

LankeOS 的装法是独立的 `CARGO_HOME`/`RUSTUP_HOME`，免得 rustup 的 shim 压过发行版那套（打包用的）rust：

这只是0.21发布后这个README编写时的情况，因为构建时间考虑，LankeOS在前几个release移除了rust-nightly，如果未来重新提供发行版级别的安装，请使用LankeOS的而不是rustup。

```sh
export RUSTUP_HOME=$HOME/.rustup-nightly CARGO_HOME=$HOME/.cargo-nightly PATH=$HOME/.cargo-nightly/bin:$PATH
curl -sSf https://sh.rustup.rs | sh -s -- -y --no-modify-path --profile minimal --default-toolchain nightly
cargo install cargo-fuzz

cd farm
cargo fuzz run extract_lpkg fuzz/seeds/extract_lpkg -- -max_total_time=60
cargo fuzz run read_member  fuzz/seeds/read_member  -- -max_total_time=60
```

`fuzz/artifacts/` 里出现的文件是崩溃复现样本。`corpus/`、`artifacts/`、`target/` 已 gitignore。
