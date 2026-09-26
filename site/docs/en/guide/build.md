---
title: Build from Source
---

# Build LankeOS from Source

> Building LankeOS requires a Linux host (8+ cores recommended), at least 64 GiB of free disk space, Docker, and root privileges.

## Build Environment Requirements

| Component | Requirement |
|-----------|-------------|
| Host OS | Any modern Linux distribution (Arch/Ubuntu/Fedora, etc.) |
| CPU | x86_64-v3, 8+ cores recommended |
| Memory | 8 GiB+ recommended |
| Disk | At least 64 GiB free space |
| Container | Docker (all builds run in fresh containers; `--image` is required) |
| Privileges | root (required by `lankefarm` and `lpkg` operations) |

## Get the Source

```bash
git clone https://github.com/Wtada233/LankeOS.git
cd LankeOS
```

## Build the Toolchain

Two tools underpin the whole pipeline: `lpkg` (the package manager that builds and installs individual packages) and `lankefarm` (the build farm that handles incremental selection, ordering, ABI propagation and repository publishing).

### lpkg

```bash
cd lpkg
sudo make docker-install     # recommended: static binary, built inside an Alpine container
```

A host build is also possible (requires `libcurl`, `libarchive`, `libelf`, `libgit2`, `libsolv` and `openssl` development packages):

```bash
make
sudo make install
```

### lankefarm

```bash
cd farm
cargo build --release        # output: farm/target/release/lankefarm
```

## Build System Architecture

LankeOS's build system is based on the following concepts:

### LankeBUILD Format

Each package directory contains two files:

```text
pkgs/<package>/
├── LankeBUILD          # Build script (three shell functions)
└── LankeBUILD.json     # Build metadata (JSON)
```

### LankeBUILD.json

```json
{
  "name": "package-name",
  "version": "1.2.3",
  "release": 1,
  "sources": ["https://upstream.example.com/pkg-1.2.3.tar.gz"],
  "work_sources": [],
  "build_deps": ["base-devel"],
  "needed_so": ["libc.so.6"],
  "provides": ["libfoo.so.1"],
  "deps": []
}
```

| Field | Description |
|-------|-------------|
| `sources` | Primary sources (release tarballs / GitHub archives / `git+<url>@<tag>`). Kept up to date by `lankefarm track` |
| `work_sources` | Auxiliary sources (patches, data files, fonts — non-archive files) |
| `build_deps` | **Build-time** dependencies, must be complete (the build container only installs `build_deps`). Includes `base-devel` as the anchor |
| `deps` | **Runtime** dependencies. Normally empty — farm derives them from `needed_so`; only implicit dependencies invisible to ELF scanning (dlopen / QML / D-Bus) are written by hand |
| `needed_so` / `provides` | Written automatically by farm after scanning the built ELF files — **do not edit manually** |
| `release` | Packaging revision, bumped for in-distribution rebuilds (final version looks like `1.2.3+1`) |
| `no_strip` | Skip ELF stripping (interpreters, compilers, `linux`, etc.) |

### LankeBUILD Script

Three phases, called in order by `lpkg` (with `cwd` set to the extracted source tree):

```bash
lankebuild_prepare() {
    cd pkg-dir
    # patches, configure regeneration, autoreconf
    patch -p1 < "{WORK_DIR}/../fix.patch"
}

lankebuild_build() {
    cd pkg-dir
    # configure, compile, install to {STAGING_ROOT}
    ./configure --prefix={PREFIX}
    make -j$(nproc)
    make DESTDIR={STAGING_ROOT} install
}

lankebuild_package() {
    # final adjustments before packaging (drop static libs, write postinst hooks, ...)
    :
}
```

Placeholders such as `{STAGING_ROOT}` are substituted at build time; see the [lpkg documentation](/en/lpkg/) for the full list.

### Lifecycle

```text
1. Parse LankeBUILD.json → name, version, source URLs
2. Download and extract sources
3. Substitute placeholders ({PREFIX}, {STAGING_ROOT}, {WORK_DIR}, ...)
4. (Optional) Run pkgs/<pkg>/hacks.sh — per-package environment fixes
5. Run lankebuild_prepare() → lankebuild_build() → lankebuild_package() in order
6. Post-processing: strip ELF files, clean up libtool files, generate SONAME links
7. Pack as .lpkg (tar.zst + metadata.json); **after a successful build** write .build_ok into the recipe directory (contents = the current recipe hash; skipped/blocked packages get no marker)
```

## Building the Entire Distribution

### 1. Cold Start: Seed from a Remote Repository

farm **refuses to build without a baseline** — `out/<arch>/index.txt` serves both as the index visible inside containers and as the baseline for ABI propagation, so it must exist. The first step is therefore to seed:

```bash
lankefarm seed --remote https://lankerepo.wtada233.top --out out
```

`seed` downloads the remote `index.txt` and every `.lpkg` in parallel (verifying SHA-256 for each) and writes the index **verbatim**: hashes are not rewritten and `.lpkg` files are not repacked. That way the index that `lpkg` sees inside containers and the one farm uses for ABI propagation are the same source of truth.

For offline environments, the [build snapshot on archive.org](https://archive.org/details/lankeos-0.20) can be used instead (it contains a full `pkgs.tar.xz` repository archive and a build workspace snapshot).

### 2. Incremental Builds: validate and build

First pull the base image for build containers (a LankeOS with a complete toolchain preinstalled):

```bash
docker pull wtada233/lankeos:latest
```

**Rebuild on recipe changes** — `validate` rebuilds every package whose `.build_ok` marker is missing or stale:

```bash
sudo lankefarm validate --image wtada233/lankeos:latest
```

The marker *is* the recipe hash (`sha256(LankeBUILD contents + LankeBUILD.json contents)`), and it lives in the **recipe directory** (`pkgs/<pkg>/.build_ok`), independent of the repository directory `out/`. So "the recipe changed" is equivalent to "the marker went stale": **editing a recipe by hand (`build_deps`, patches, build flags) or simply deleting its `.build_ok` means that package gets rebuilt**, with no need to track affected packages by hand.

`seed` does not affect the markers — it only populates `out/` and never touches `pkgs/`. So running `validate` right after seeding rebuilds **nothing at all**, as long as the recipes and the artifacts in the repository come from the same build.

Conversely, a genuine **full rebuild from source of all ~860 packages** (deleting every `.build_ok`, or a toolchain-level change that ripples through many recipes) takes **at least 3 days** at the current repository scale — chromium, rust, llvm, libreoffice and linux dominate the cost. Day-to-day incremental rebuilds touch only the affected packages, a far smaller scope.

**Rebuild on upstream version changes** — `build --all` skips packages whose version already matches the local repository:

```bash
sudo lankefarm build --all --image wtada233/lankeos:latest
```

**Force a rebuild of specific packages**:

```bash
sudo lankefarm build bash curl --image wtada233/lankeos:latest
```

#### ABI-driven increments (what sets farm apart)

After a package is rebuilt, if it no longer provides an old SONAME, farm computes the set of removed SONAMEs from the **old index's `needed_so`/`provides`** and rebuilds the packages that consume them directly — no transitive closure, so a `libxml2` break rebuilds `llvm` only, and if `llvm`'s ABI is unchanged afterwards `rust` stays put. Declarative rebuild groups in `data/build/*.yaml` cover packages that are ABI-sensitive without linking the library (the Python ecosystem; Qt private-API consumers are detected dynamically by a script that greps ELF version strings).

During the rebuild, removed `.so` files are backed up to `out/backups/` and restored plus `ldconfig`-ed inside every build container, so binaries that have not been rebuilt yet keep working through the transition; the backups are only cleaned up once the whole build finishes and nothing references them.

Other subcommands: `track` (probe upstream versions), `abifix` (fix packages referencing a SONAME with no provider), `export` (flatten the repository into release layout), `serve` (local repository HTTP server), `chk` (maintenance audit toolset).

### 3. Assembling a rootfs

Once every package is in `out/<arch>/`, assemble a new root directory. The first step is installing `filesystem` — the only package carrying `keep_fs_layout`, which declares the filesystem layout:

```bash
sudo lpkg install filesystem --root /mnt/lfs --arch x86_64
```

It provides two things:

- **The usr-merge layout**: `bin → usr/bin`, `sbin → usr/bin`, `lib → usr/lib`, `lib64 → usr/lib`, `usr/sbin → bin`, `usr/lib64 → lib`
- **The `/etc` skeleton and distribution identity**: `/etc/os-release` (a symlink to `/usr/lib/os-release`), `lanke-release`, `profile`, `shells`, `hostname`, `hosts`, `inputrc`, `vimrc`, `issue`, `locale.conf`, `vconsole.conf`, `/etc/skel/*`, `/etc/sysconfig/clock`, `/etc/default/grub`, the fastfetch config, `/usr/share/lankeos/*`

**A small remainder outside `filesystem` has to be created by hand** — these belong to no package, and the package manager cannot maintain them (the runtime or the installer takes them over):

- **Mount-point directories**: `/proc`, `/sys`, `/dev`, `/run`, `/tmp`, plus whichever of `/var`, `/boot`, `/home`, `/root`, `/mnt`, `/srv`, `/opt` you need
- **Account files**: `/etc/passwd`, `/etc/group`, `/etc/shadow`
- **`/etc/resolv.conf`**: can be copied straight from the host
- **CA certificates under `/etc/ssl/certs`**: copy from the host, or generate them after installing `make-ca`

Then install the remaining packages you need:

```bash
sudo lpkg install base --root /mnt/lfs --arch x86_64
```

`base` (base system) and `base-devel` (build toolchain) are metapackages; add `plasma-meta` or `gnome` for a desktop.

> lpkg reads its repository URL from `mirror.conf`. To point it at the `out/` you just built, either point `/etc/lpkg/mirror.conf` at a local path (`file:///path/to/out` or `/path/to/out`), or run `lankefarm serve --root out --port 8080` and use `http://127.0.0.1:8080`.

### 4. Optional per-package environment fixes

If `pkgs/<pkg>/hacks.sh` exists, `lpkg build` prints its contents before the build phases and runs it after confirmation (automatically with `-y`), for packages that need special environment preparation. No package in the current tree uses this mechanism.

## Live ISO Packaging

```bash
# 1. Reset system state (clear machine-id, logs, temp files, rebuild the mtab symlink, ...)
#    The target root is fixed at /mnt/lfs
sudo bash live/reinit.sh

# 2. Generate the squashfs image (pack.sh does not do this — run it yourself)
sudo mksquashfs /mnt/lfs live/ISO/live/rootfs.sfs -comp xz

# 3. Package the ISO (requires root; re-executes itself via sudo)
sudo bash live/pack.sh
```

The result is `live/lankeos-live.iso` (UEFI boot only).

Two things to note:

- `pack.sh` takes the kernel artifacts from **outside the repository** (relative to `live/ISO/boot/`, that is `../../../../../tools/krnl/`: `config-lanke`, `System.map-lanke`, `vmlinuz-lanke`), so you must place them yourself.
- `pack.sh` **deletes** `ISO/live/rootfs.sfs` and the kernel copies when it finishes, so the squashfs must be regenerated before every ISO build.

This is also why ISO building is not part of CI: it depends on a kernel from outside the repository and a hand-assembled rootfs.

## Further Reading

- [lpkg documentation](/en/lpkg/) — package manager, package format and repository layout
- `lpkg/ARCH.md` — the WAL 2.0 atomic transaction specification
- `farm/ARCH.md` — build farm architecture, incremental selection, ABI propagation and version tracking
