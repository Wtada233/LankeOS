English | [中文](README.md)

<h1 align="center">lankefarm</h1>

<p align="center">
  <strong>The ABI-driven incremental package build system for LankeOS (written in Rust).</strong>
  <br />
  <em>ABI break detection · incremental rebuilds · container isolation · deterministic ordering</em>
</p>

<p align="center">
  <a href="#usage"><img src="https://img.shields.io/badge/Quick_Start-4CAF50?style=for-the-badge" alt="Quick Start" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-GPLv3-2d8cf0?style=for-the-badge" alt="License" /></a>
</p>

<p align="center">
  <img src="https://img.shields.io/github/actions/workflow/status/Wtada233/LankeOS/farm-build.yml?style=flat&logo=github-actions&logoColor=white" alt="Build status" />
  <img src="https://img.shields.io/badge/Rust-000000?style=flat&logo=rust&logoColor=white" alt="Rust" />
  <img src="https://img.shields.io/github/license/Wtada233/LankeOS?style=flat" alt="License" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Docker-2496ED?style=flat&logo=docker&logoColor=white" alt="Docker" />
  <img src="https://img.shields.io/badge/SQLite-003B57?style=flat&logo=sqlite&logoColor=white" alt="SQLite" />
  <img src="https://img.shields.io/badge/zstd-1E5B8C?style=flat&logo=zstandard&logoColor=white" alt="zstd" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Claude_Code-D97757?style=flat&logo=claude&logoColor=white" alt="Claude Code" />
</p>

`lankefarm` is the **ABI-driven incremental package build system** for LankeOS (written in Rust). It builds a link dependency graph from every package's `needed_so`/`provides`, detects upstream ABI breaks, rebuilds only the smallest affected set, and runs all builds inside isolated Docker containers.

> For the detailed architecture specification, see [ARCH.md](ARCH.md). It is written from the actual code and describes the real behavior of the current implementation — in case of conflict, the code wins.

## Features

- **ABI-driven incremental builds** — Only builds packages whose recipe version differs from the local repository, plus the victims of ABI breaks. Removed SONAMEs are computed from the old index's `needed_so`/`provides` and linked straight to the victims — no tree closure.
- **Container-isolated builds** — Every build runs in a fresh Docker container; `--image` is mandatory. Host builds are forbidden to avoid polluting the host environment.
- **Deterministic build order** — Topological sort with a fixed tie-break (package names in ascending order). No randomness, guaranteed by regression tests.
- **Build plan confirmation** — Lists the topo order for operator confirmation before starting; source pre-download only targets the confirmed set.
- **ABI transition backups** — When a SONAME break is detected, the old `.so` files are backed up to `out/backups/`, restored and `ldconfig`-ed inside each build container so old binaries survive the transition, then cleaned up after the whole build completes.
- **Upstream version tracking** — `track` probes upstream versions and updates `LankeBUILD.json` (read-only proposals by default, `--run` applies them); `gen-trackers` batch-generates tracker YAML files via an LLM.
- **Cold-start seeding** — `seed --remote` downloads a remote repository in parallel with SHA-256 verification, landing `index.txt` and `.lpkg` files intact.
- **Bilingual CLI** — Full zh/en l10n; ANSI colors degrade automatically on non-TTY terminals.

## Usage

### Incrementally rebuild everything pending

```bash
lankefarm build --all --image wtada233/lankeos:latest
```

### Force-rebuild specific packages

```bash
lankefarm build bash curl --image wtada233/lankeos:latest
```

### Validate packages missing their build marker

```bash
lankefarm validate --image wtada233/lankeos:latest
```

### Track upstream versions

```bash
lankefarm track bash --run          # read-only proposal: drop --run
lankefarm track --all --run         # batch apply
```

### Cold-start a remote repository

```bash
lankefarm seed --remote https://lankerepo.wtada233.top
```

### Serve the local repository over HTTP

```bash
lankefarm serve --root out --port 8000
```

## Quick Start

### Prerequisites

- Rust toolchain (stable)
- Docker (required at build time)

### Build

```bash
cd farm
cargo build --release        # binary: target/release/lankefarm
```

### Run

```bash
./target/release/lankefarm --help
```

## CLI Commands

| Command | Description |
|---|---|
| `build <pkg>...\|--all --image <img>` | Incremental, ABI-aware build with plan preview (container builds only, `--image` required) |
| `validate --image <img>` | Rebuild every package missing its `.build_ok` marker |
| `export --output <dir>` | Repack the build repo into distribution-format `<pkg>-<ver>.lpkg` archives |
| `track <pkg>\|--all [--run]` | Probe upstream versions (read-only proposal by default, `--run` applies) |
| `gen-trackers` | Batch-generate tracker YAML files via an LLM |
| `seed --remote <url>` | Cold-start a remote repository (parallel download + SHA-256 verification) |
| `serve [--root out] [--port 8000]` | Static HTTP server for the local repository |

## Build Pipeline

```mermaid
%%{init: {'theme': 'base', 'themeVariables': {'fontSize': '14px'}}}%%
flowchart LR
    A[Incremental selection<br/>version diff + ABI breaks] --> B[Topological sort<br/>deterministic order]
    B --> C[Plan preview + confirm]
    C --> D[Source pre-download<br/>confirmed set]
    D --> E[Per-package container build]
    E --> F{ABI check<br/>removed SONAME?}
    F -->|broken| G[repack / propagate / backup]
    F -->|ok| H[Publish / backup cleanup]
    G --> H

    classDef start fill:#3B82F6,stroke:#2563EB,color:#fff,stroke-width:2px
    classDef process fill:#10B981,stroke:#059669,color:#fff,stroke-width:2px
    classDef decision fill:#F59E0B,stroke:#D97706,color:#fff,stroke-width:2px
    classDef end fill:#8B5CF6,stroke:#7C3AED,color:#fff,stroke-width:2px

    class A start
    class B,C,D,E process
    class F decision
    class G,H end
```

## Source Layout

```
farm/
├── src/
│   ├── main.rs          # entry point
│   ├── cli/             # build / validate / export / track / gen-trackers / seed / serve
│   ├── build/           # scheduler: incremental selection, topo sort, plan preview, pre-download
│   ├── abi.rs           # removed_sonames / detect_abi_breaks / propagate
│   ├── graph.rs         # index.txt parsing + link dependency graph
│   ├── scan.rs          # .lpkg extraction + ELF needed_so/provides scanning
│   ├── repack.rs        # metadata.json drift fix + repacking
│   ├── verify.rs        # three-way verdict of build output vs expected metadata
│   ├── lpkg_binding.rs  # the only seam that touches lpkg (docker orchestration + ABI backup injection)
│   └── ...
├── data/
│   ├── trackers/        # per-package upstream version trackers (github/gitlab/gnome/...)
│   └── build/           # declarative ABI rebuild groups (e.g. the Python ecosystem)
└── tests/               # regression tests
```

## Tech Stack

| Technology | Purpose |
|---|---|
| Rust | Implementation language (serde / clap / rusqlite / ureq / goblin / zstd / sha2) |
| SQLite | Job state store (`out/farm-state.db`) |
| Docker | Container-isolated builds (`--image` specifies the base image) |
| zstd · tar | `.lpkg` extraction / repacking |

## Contributing

PRs and bug reports are welcome. All lpkg interaction is confined to the `LpkgBinding` trait (`lpkg_binding.rs`); implement new lpkg-related features through that seam.

## License

Licensed under GPL-3.0.
