English | [中文](README.md)

<h1 align="center">lpkg</h1>

<p align="center">
  <strong>A lightweight, command-line C++20 package manager for LankeOS.</strong>
  <br />
  <em>Atomic WAL transactions with rollback · needed_so ABI validation · aggregate index · static builds</em>
</p>

<p align="center">
  <a href="#usage"><img src="https://img.shields.io/badge/Quick_Start-4CAF50?style=for-the-badge" alt="Quick Start" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-GPLv3-2d8cf0?style=for-the-badge" alt="License" /></a>
</p>

<p align="center">
  <img src="https://img.shields.io/github/actions/workflow/status/Wtada233/LankeOS/lpkg-build.yml?style=flat&logo=github-actions&logoColor=white" alt="Build status" />
  <img src="https://img.shields.io/badge/C++20-00599C?style=flat&logo=cplusplus&logoColor=white" alt="C++20" />
  <img src="https://img.shields.io/github/license/Wtada233/LankeOS?style=flat" alt="License" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Docker-2496ED?style=flat&logo=docker&logoColor=white" alt="Docker" />
  <img src="https://img.shields.io/badge/Bash-4EAA25?style=flat&logo=gnubash&logoColor=white" alt="Bash" />
  <img src="https://img.shields.io/badge/zstd-1E5B8C?style=flat&logo=zstandard&logoColor=white" alt="zstd" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Claude_Code-D97757?style=flat&logo=claude&logoColor=white" alt="Claude Code" />
</p>

`lpkg` is a lightweight, command-line package manager designed for LankeOS. Written in C++20, it provides atomic, traceable package management for LFS (Linux From Scratch) environments.

## Features

-   **Full lifecycle management**: Install, uninstall, upgrade, and reinstall packages.
-   **needed_so verification**: Automatically validates every ELF DT_NEEDED SONAME against the repository before installation. Rejects packages with unresolvable SONAMEs, preventing the "empty provides still installs" class of bugs.
-   **SIGINT graceful shutdown**: Ctrl+C sets a graceful-shutdown flag; the current operation (including any required rollback) runs to completion before exiting. There is no force-terminate — rollback is never interrupted mid-flight.
-   **Smart version parsing**: Supports multi-segment revision numbers (e.g. `1.0.0.1`) with a rigorous comparison algorithm that correctly handles cases like `6.16.1 > 6.6.1`. Supports compound range constraints (e.g. `>= 2.0.0 < 3.0.0`).
-   **Aggregated index**: Uses a compact `index.txt` format where a single line records all versions and their respective hashes, deps, provides, and needed_so.
-   **Embedded metadata**: All metadata (name, version, dependencies, needed_so, virtual provides, man page) is stored in a `metadata.json` inside each package.
-   **Auto dependency generation**: The build farm (`farm/`, Rust) scans ELF DT_NEEDED SONAMEs after packaging to generate `needed_so`, then resolves provider package names into `deps`, eliminating manual version constraint maintenance.
-   **Layout-as-content**: The `content/` directory layout maps directly to the root filesystem.
-   **Automated operations**: Includes `lrepo-mgr.py` for publishing to S3-compatible storage or SCP remote servers. The `--path` flag enables local filesystem repositories for offline testing.
-   **Highly compatible static builds**: Built-in automatic detection of system CA certificate paths ensures statically compiled binaries work across different Linux distributions.
-   **Security**: Mandatory SHA256 hash verification, file conflict detection, and malicious path filtering.
-   **System hooks and triggers**: Supports per-package `postinst`/`prerm` scripts and system-level triggers (e.g. `ldconfig`).

## Usage

Most `lpkg` operations require `root` privileges.

**General syntax:**
```bash
lpkg [options] <command> [arguments]
```

### Common Commands

-   **`install <package>[:version]`**: Install a package. Defaults to the latest version if none is specified.
-   **`upgrade`**: Automatically check the index for updates to all installed packages.
-   **`remove <package> [--force]`**: Remove a package. Use `--force` to remove packages that others depend on.
-   **`query [-p] <package|filename>`**: Query which package owns a file, or list files in a package.
-   **`scan [directory]`**: Scan for orphaned files not owned by any package.
-   **`pack -o <output> -d <directory>`**: Build a `.lpkg` package from a directory. Reads package metadata from `<directory>/metadata.json`.
-   **`build [directory]`**: Automatically build and pack a package from a specific directory.

## Quick Start

### Dependencies

The following libraries are only needed when building `lpkg` directly on the host (dynamic or static linking); the recommended Docker build needs none of them:

- `libcurl`: For file downloads.
- `libarchive`: For extracting package files.
- `libcrypto` (OpenSSL): For hash computation.

On Arch Linux:
```bash
sudo pacman -S curl libarchive openssl
```

### Building and Installing

1.  **Docker build (recommended)**: Building and testing requires nothing but Docker. All dependencies (including the libgit2 and libsolv static libraries) are installed automatically inside an `alpine:3.19` container, producing a pure static binary:
    ```bash
    make docker          # pure static binary: build/lpkg-docker
    sudo make docker-install
    make test            # run the full test suite in the same container
    ```
2.  **Dynamic build (host)**: Requires `libcurl`, `libarchive`, `libelf`, `libgit2`, `libsolv`, and `libopenssl` on the host:
    ```bash
    make && sudo make install
    ```

## Repository Management (Operations Guide)

The project provides a repository management script at `main/scripts/lrepo-mgr.py`.

### 1. Configure Repository Connection
Supports S3-compatible storage or SCP:
```bash
# Configure S3-compatible storage
./main/scripts/lrepo-mgr.py config --set \
    storage.type=s3 \
    storage.endpoint=https://your-s3-endpoint.example.com \
    storage.bucket=your-bucket-id \
    storage.access_key=AK... \
    storage.secret_key=SK...
```

### 2. Publish Packages
This command automatically updates the aggregated index and uploads:
```bash
./main/scripts/lrepo-mgr.py push ./pkgs/*.lpkg
```

### 3. Local Repository (Offline Testing)
```bash
# Initialize a local repo and push packages
./main/scripts/lrepo-mgr.py --path /tmp/repo push ./pkgs/*.lpkg

# View the generated index
cat /tmp/repo/x86_64/index.txt
```

### 4. Clean Up Old Versions
Remove all files from storage that are not listed in `index.txt`:
```bash
./main/scripts/lrepo-mgr.py cleanup
```

## Package Format Specification

Each `.lpkg` file is a tar.zst archive with the following structure:

```text
metadata.json         # Package metadata (name, version, deps, provides, man page, etc.)
content/              # Files (maps directly to root directory)
hooks/                # Hook scripts (optional)
```

### metadata.json Example
```json
{
  "name": "curl",
  "version": "8.11.1",
  "deps": ["glibc", "openssl", "zlib", "zstd", "bash"],
  "provides": ["libcurl.so.4"],
  "needed_so": ["libc.so.6", "libssl.so.3", "libcrypto.so.3", "libz.so.1", "libzstd.so.1"],
  "man": "curl(1) - transfer a URL\n..."
}
```

| Field | Description |
|-------|-------------|
| `name` | Package name |
| `version` | Version string |
| `deps` | Dependency package names (no version constraints; auto-resolved from needed_so by the build farm) |
| `provides` | SONAMEs and virtual capabilities this package provides |
| `needed_so` | DT_NEEDED SONAME list from the package's ELF files (ground truth for runtime deps) |
| `man` | Inline man page content (optional) |

The files under `content/` are extracted directly to the target root (`/`).

## Build Flag Configuration (build.conf)

`lpkg build` uses **Arch Linux compile/link flags with an x86-64-v3 ISA baseline** by default
(`-march=native` is never used, so the build host's CPU features beyond v3 can never leak into
the distribution — v3 means AVX2/FMA and friends, covering the vast majority of x86-64 machines
made after 2013). The defaults live in `main/src/base/build_defaults.hpp` (fallback); the
system-wide config is `/etc/lpkg/build.conf` (makepkg.conf style, installed from
`main/conf/build.conf` by `make install`):

```text
CFLAGS="-march=x86-64-v3 -mtune=generic -O2 -pipe -fno-plt -fexceptions ..."
CXXFLAGS="$CFLAGS -Wp,-D_GLIBCXX_ASSERTIONS"   # fully expanded in the file
LDFLAGS="-Wl,-O1 -Wl,--sort-common -Wl,--as-needed -Wl,-z,relro -Wl,-z,now ..."
LTOFLAGS="-flto=auto"
MAKEFLAGS="-j$(nproc)"                          # $(nproc) expands to the logical core count
```

Before each stage of `build` (`lankebuild_prepare`/`build`/`package`) those flags are
`export`ed into the build environment, so configure/make/cmake inherit them automatically; a
LankeBUILD script may also reference the template variables `{CFLAGS}`, `{CXXFLAGS}`,
`{LDFLAGS}` and `{MAKEFLAGS}`.

**Per-package override** (highest precedence): add optional fields to `LankeBUILD.json`;
leaving them out means "use the configured default":

```json
{
  "name": "foo",
  "version": "1.0",
  "cflags": "-O2 -march=x86-64-v3",
  "ldflags": "-Wl,--as-needed",
  "makeflags": "-j2",
  "lto": true
}
```

`"lto": true` appends `LTOFLAGS` (default `-flto=auto`) to both the compile and link flags.

## Repository Specification

### Directory Structure
```text
/x86_64
  ├── index.txt           # Core index: name|ver:hash:deps:provides:needed_so;...|
  └── bash/
      ├── 5.3.lpkg        # Actual tar.zst compressed package
      └── 5.4.lpkg
```

### Index Line Example
```text
# provides/needed_so are per-version (inside the version block, fields 4 and 5)
curl|8.11.1:hash:glibc,openssl,zlib,zstd,bash:libcurl.so.4:libc.so.6,libssl.so.3,libz.so.1,libzstd.so.1|
```

## Source Architecture

-   **`Repository`**: Parses the aggregated index and implements smart version sorting.
-   **`InstallationTask`**: Atomic transaction model with automatic rollback on failure.
-   **`Downloader`**: Wraps `libcurl` with integrated multi-path certificate detection.
-   **`Cache`**: Local state database stored at `/var/lib/lpkg/`.
-   **`metadata.json`**: In-package embedded metadata (name, version, deps, provides, man page).

## Contributing

PRs and bug reports are welcome. For new feature proposals, please prioritize keeping the binary lightweight.

## License

Licensed under GPL-3.0.
