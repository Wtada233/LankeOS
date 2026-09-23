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

> **Runtime requirement**: operational commands (`build`/`validate`/`abifix`/`export`/`seed`)
> **must run as root** — `.lpkg` unpack/repack needs root-owned files and SUID/SGID (farm no longer drops
> privileges via `sudo`). Run as the root user or via `sudo farm …`. `track`/`serve` do not need root.

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

## Tracker Configuration (data/trackers/\<pkg\>.yaml)

`farm track` resolves a package's upstream version from `data/trackers/<pkg>.yaml`. A tracker is the **complete manifest of sources / work_sources**: each entry declaratively probes one slot. When a probe succeeds and the version moved forward, the package's `sources`/`work_sources` in LankeBUILD.json are **atomically replaced** (empty lists are written as keys; any entry failure skips the package).

```yaml
# Single-source package: GitHub tags probing
pkg-name: systemd
version-source: sources[0]        # explicitly declares which entry provides the version
sources:
  - tracker-template: github
    repo: systemd/systemd
    mode: tags
    tag-prefix: v
    template: https://github.com/{repo}/archive/refs/tags/{tag}.tar.gz
```

Templates: `github` `gitlab` `html-index` `gnome` `gcs` `sourceforge` `pypi` `same-version` (directly locks another package's version) `script` (inline bash). Each template accepts only its own fields — setting an unsupported field (e.g. `max-version` on github) or misspelling a field name (e.g. `tag-prefx`) errors out instead of being silently ignored.

`script` is an **entry-level** escape hatch (a peer of the other templates, not a package-level type): one script produces one source/work_source slot, with stdout being **exactly one line** `<version>|URL`. Use it only when no template can express the source — scripts are not reusable and cannot be uniformly validated:

```yaml
pkg-name: ant
version-source: sources[0]
sources:
  - tracker-template: script
    script: |
      page=$(curl -fsSL "https://www.apache.org/dist/ant/source/")
      ver=$(printf '%s\n' "$page" | grep -oE 'apache-ant-[0-9.]+-src\.tar\.bz2' | sed 's/apache-ant-//; s/-src.tar.bz2//' | sort -V | tail -n1)
      test -n "$ver" || exit 1
      echo "$ver|https://www.apache.org/dist/ant/source/apache-ant-$ver-src.tar.bz2"
```

When upstream publishes a **dynamically enumerated** set (the number of files is not fixed), add `expand: true`: each stdout line is one `<version>|URL` and maps to one consecutive slot (e.g. libreoffice-i18n's 123 langpacks). Without it, **multiple lines are an error** — deliberately, so an un-migrated script fails loudly instead of being silently truncated.

When one entry's content is **derived from** another entry, use `version-var` to inject the upstream resolved version as a script variable. Probing independently would produce a **version-inconsistent manifest** if upstream releases between the two probes (main source at version A, vendor at version B — the build breaks):

```yaml
work_sources:
  - tracker-template: script
    expand: true
    version-var: {main: sources[0]}   # $main = the version sources[0] resolved this run
    script: |
      echo "$main|https://x/vendor-$main.tar.gz"
```

Only slots **earlier** than the entry can be referenced (`sources` are probed before `work_sources`, left to right within a list); forward/self references fail at probe time with the usable range in the message.

work_sources-only packages (fonts, jars — non-archive files must live in work_sources) carry only a work_sources list plus `version-source: work_sources[0]`, with no `sources` entry.

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
| `export --output <dir>` | Flatten-copy the build repo into distribution-format `<pkg>-<ver>.lpkg` files (no repack) |
| `chk <kind> --source <repo> [--pkgs …]` | Maintainer utility checks (**not a stable interface** - may be removed as techniques change): `qml` (QML import deps) / `pkgconf` (.pc Requires deps) / `pkg-err` (usr/etc|usr/var, .la, .a) / `hook` (sysusers/tmpfiles → postinst auto-run) / `abi` (full ABI symbol@version audit) / `full` (run all at once). Shared `--source/--arch/--pkgs-dir/--cache/--pkg/--full-rescan`; non-root, read-only |
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
| zstd · tar (Rust crates) | `.lpkg` extraction / repacking (pure Rust — no system `zstd`/`tar`/`sudo` needed) |

## Contributing

PRs and bug reports are welcome. All lpkg interaction is confined to the `LpkgBinding` trait (`lpkg_binding.rs`); implement new lpkg-related features through that seam.

## License

Licensed under GPL-3.0.
