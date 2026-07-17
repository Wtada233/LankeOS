---
title: Build from Source
---

# Build LankeOS from Source

> Building LankeOS requires a Linux host with a high-performance CPU (8+ cores recommended) and at least 64 GB of free disk space.

## Build Environment Requirements

| Component | Requirement |
|-----------|-------------|
| Host OS | Any modern Linux distribution (Arch/Ubuntu/Fedora, etc.) |
| CPU | x86_64, 8+ cores recommended |
| Memory | 8 GiB+ recommended |
| Disk | At least 64 GiB free space |
| Toolchain | GCC, Make, pkg-config |

## Get the Source

```bash
git clone https://github.com/Wtada233/LankeOS.git
cd LankeOS
```

## Build lpkg Package Manager

lpkg is the core tool for the entire build process. Static compilation is recommended:

```bash
cd lpkg

# Static linking (requires docker)
sudo make docker-install

# Or default build
make
sudo make install
```

## Building Packages

Use lpkg's build command to build individual packages:

```bash
# Build bash
sudo lpkg build pkgs/bash

# Build coreutils
sudo lpkg build pkgs/coreutils

# Building the entire system requires building all packages in dependency order
```

## Build System Architecture

LankeOS's build system is based on the following concepts:

### LankeBUILD Format

Each package directory contains two files:

```text
pkgs/<package>/
├── LankeBUILD          # Build script (Shell functions)
└── LankeBUILD.json     # Build metadata (JSON)
```

### LankeBUILD.json

```json
{
  "name": "package-name",
  "version": "1.2.3",
  "sources": ["https://upstream.example.com/pkg-1.2.3.tar.gz"],
  "work_sources": ["https://example.com/patches/fix.patch"],
  "no_strip": false,
  "deps": ["dependency >= 1.0"]
}
```

### LankeBUILD Script

Defines three phases:

```bash
lankebuild_prepare() {
    cd pkg-dir
    # Patches, configuration generation
    patch -p1 < "{WORK_DIR}/fix.patch"
}

lankebuild_build() {
    cd pkg-dir
    # Configure, compile, install to {STAGING_ROOT}
    ./configure --prefix=/usr
    make -j$(nproc)
    make DESTDIR={STAGING_ROOT} install
}

lankebuild_package() {
    # Final adjustments before packaging (optional)
    :
}
```

### Lifecycle

```text
1. Parse LankeBUILD.json → get name, version, source URLs
2. Download and extract sources
3. Replace variables ({PKG_NAME}, {PKG_VER}, {WORK_DIR}, etc.)
4. Execute lankebuild_prepare()
5. Execute lankebuild_build()   → install to STAGING_ROOT
6. Execute lankebuild_package()
7. Post-processing: ELF stripping, SONAME links, remove .la files
8. Package as .lpkg (tar.zst + metadata.json)
```

## Building the Entire Distribution

A complete build involves the following steps:

### 1. Set Up the Workspace: A Machine Running LankeOS (Physical or VM) as the Build Host

- Install all packages, e.g., wlroots (which is not installed by default). Currently, the LankeBUILD system lacks a `build_deps` declaration — this is a major shortcoming and will be improved in the future.

### 2. Start Building

- Run `git clone https://github.com/Wtada233/LankeOS --depth=1`

- Run `hacks.sh` scripts under each `pkgs/*/` directory (these provide special environment setup required by each package, such as symlinks like `python3=python`, special Python libraries, etc., that LankeOS does not provide by default).

- In the `pkgs` directory, run:

  ```bash
  sudo python3 ../lpkg/main/scripts/lankeos-world-rebuild-helper.py
  ```

- Follow the prompts for a semi-automatic rebuild. Normally, you only need to press Ctrl+D at each step. Semi-automatic mode is designed for easier debugging. If you need a fully automatic rebuild, use `yes exit` to quickly exit the shell.

- Generate the provider map and automatically mark dependencies and `needed_so`:

  ```bash
  sudo python3 ../lpkg/main/scripts/gen_deps.py .
  ```
  (Make sure to install `pyelftools` in advance for fast scanning.)

- After all packages are built, create a new LankeOS root directory, set up basic usr merge symlinks, and basic system configuration files (`passwd`, etc.).

- Manually copy `lpkg` to `rootfs/usr/bin`, create `rootfs/etc/resolv.conf` and all necessary certificates under `rootfs/etc/ssl` (you can copy them directly from the host). Then copy all packages to the new rootfs, `chroot` into it, and install all packages using `lpkg` with `--no-deps` to skip dependency resolution.

## Live ISO Packaging

```bash
# 1. Reset system state (clear machine ID, logs, temp files, etc.)
sudo bash live/reinit.sh

# 2. Package the ISO (requires root)
sudo bash live/pack.sh
```

The generated ISO will be at `live/lankeos-live.iso`.

## Dependency Generation

Use `gen_deps.py` to automatically analyze and update package dependencies:

```bash
python3 lpkg/main/scripts/gen_deps.py /path/to/packages
```

This script analyzes ELF NEEDED and SONAME entries to automatically update each package's dependency list in `metadata.json`.
