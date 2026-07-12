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

A complete build typically requires the following steps:

1. **Build lpkg** (done) then use it to build `pkgs/lpkg`, then `lpkg install --force-overwrite` to replace lpkg itself (self-hosting/bootstrap)
2. **Build base toolchain** — glibc, gcc, binutils, coreutils, etc.
3. **Build system infrastructure** — systemd, dbus, util-linux, PAM, etc.
4. **Build network stack** — NetworkManager, wpa_supplicant, openssh, curl
5. **Build graphics stack** — Mesa, Wayland, wlroots, Sway
6. **Build development tools** — LLVM, Rust, Python, Ruby
7. **Build desktop applications** — GTK4, WebKitGTK, Alacritty, Fcitx5
8. **Build kernel and firmware**
9. **Package Live ISO**

> Note: The complete build guide is still being refined. Using the [pre-built ISO](/en/download) is recommended for now.

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
