English | [中文](README.md)

# lpkg - A Simple C++ Package Manager

`lpkg` is a lightweight, high-performance, command-line package manager designed for LankeOS. Written in C++20, it provides atomic, traceable package management for LFS (Linux From Scratch) environments.

## Features

-   **Full lifecycle management**: Install, uninstall, upgrade, and reinstall packages.
-   **Smart version parsing**: Supports multi-segment revision numbers (e.g. `1.0.0.1`) with a rigorous comparison algorithm that correctly handles cases like `6.16.1 > 6.6.1`.
-   **Aggregated index**: Uses a compact `index.txt` format where a single line records all versions and their hashes for a package, greatly reducing network requests.
-   **Zero-redundancy storage**: Eliminates `latest.txt` and separate `hash.txt` files. Packages are stored in a flat `<name>/<version>.lpkg` layout.
-   **Automated operations**: Includes `lrepo-mgr.py` for one-click publishing to Tencent Cloud COS (S3) or SCP remote servers.
-   **Highly compatible static builds**: Built-in automatic detection of system CA certificate paths ensures statically compiled binaries work across different Linux distributions.
-   **Security**: Mandatory SHA256 hash verification, file conflict detection, and malicious path filtering.
-   **System hooks and triggers**: Supports per-package `postinst`/`prerm` scripts and system-level triggers (e.g. `ldconfig`).

## User Guide

### Dependencies

Building `lpkg` requires the following libraries:
- `libcurl`: For file downloads.
- `libarchive`: For extracting package files.
- `libcrypto` (OpenSSL): For hash computation.
- `libfmt`: For string formatting (header-only).

On Arch Linux:
```bash
sudo pacman -S curl libarchive openssl fmt
```

### Building and Installing

1.  **Dynamic build**:
    ```bash
    make && sudo make install
    ```
2.  **Static build (recommended for LFS)**:
    The project includes a `Dockerfile.build` that produces a fully statically linked binary:
    ```bash
    sudo docker build -t lpkg-builder -f Dockerfile.build .
    sudo docker run -it --rm -v $(pwd):/app lpkg-builder
    ```

### Usage

Most `lpkg` operations require `root` privileges.

**General syntax:**
```bash
lpkg [options] <command> [arguments]
```

#### Common Commands

-   **`install <package>[:version]`**: Install a package. Defaults to the latest version if none is specified.
-   **`upgrade`**: Automatically check the index for updates to all installed packages.
-   **`remove <package> [--force]`**: Remove a package. Use `--force` to remove packages that others depend on.
-   **`query [-p] <package|filename>`**: Query which package owns a file, or list files in a package.
-   **`scan [directory]`**: Scan for orphaned files not owned by any package.
-   **`pack -o <output> --source <source-dir>`**: Build a `.lpkg` package from a directory.

## Repository Management (Operations Guide)

The project provides a repository management script at `main/scripts/lrepo-mgr.py`.

### 1. Configure Repository Connection
Supports S3 (Tencent Cloud COS, AWS) or SCP:
```bash
# Configure S3/COS
./main/scripts/lrepo-mgr.py config --set \
    storage.type=s3 \
    storage.endpoint=https://cos.ap-hongkong.myqcloud.com \
    storage.bucket=your-bucket-id \
    storage.access_key=AK... \
    storage.secret_key=SK...
```

### 2. Publish Packages
This command automatically extracts dependency information, updates the aggregated index, and uploads:
```bash
./main/scripts/lrepo-mgr.py push ./pkgs/*.lpkg
```

### 3. Clean Up Old Versions
Remove all files from storage that are not listed in `index.txt`:
```bash
./main/scripts/lrepo-mgr.py cleanup
```

## Repository Specification

### Directory Structure
```text
/x86_64
  ├── index.txt           # Core index: name|ver1:hash1,ver2:hash2|deps|provides
  └── bash/
      ├── 5.2.lpkg        # Actual tar.zst compressed package
      └── 5.3.lpkg
```

### Index Line Example
```text
acl|2.3.1:sha...,2.3.2:sha...|attr,coreutils|libacl.so
```

## Source Architecture

-   **`Repository`**: Parses the aggregated index and implements smart version sorting.
-   **`InstallationTask`**: Atomic transaction model with automatic rollback on failure.
-   **`Downloader`**: Wraps `libcurl` with integrated multi-path certificate detection.
-   **`Cache`**: Local state database stored at `/var/lib/lpkg/`.

## Contributing

PRs and bug reports are welcome. For new feature proposals, please prioritize keeping the binary lightweight.

## License

Licensed under GPL-3.0.
