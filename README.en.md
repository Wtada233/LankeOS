English | [中文](README.md)

<h1 align="center">LankeOS</h1>

<p align="center">
  <strong>A Linux distribution focused on automating and simplifying distribution maintenance, with a custom package manager and ABI-driven build farm.</strong>
  <br />
  <em>Linux From Scratch · lpkg (C++20) · lankefarm (Rust) · Wayland · Live ISO</em>
</p>

<p align="center">
  <a href="#quick-start"><img src="https://img.shields.io/badge/Quick_Start-4CAF50?style=for-the-badge" alt="Quick Start" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-GPLv3-2d8cf0?style=for-the-badge" alt="License" /></a>
  <a href="https://lankeos.wtada233.top"><img src="https://img.shields.io/badge/Documentation-lankeos.wtada233.top-1557a0?style=for-the-badge" alt="Documentation" /></a>
</p>

<p align="center">
  <img src="https://img.shields.io/github/actions/workflow/status/Wtada233/LankeOS/lpkg-build.yml?style=flat&logo=github-actions&logoColor=white" alt="Build status" />
  <img src="https://img.shields.io/github/v/release/Wtada233/LankeOS?style=flat" alt="Release" />
  <img src="https://img.shields.io/badge/C++20-00599C?style=flat&logo=cplusplus&logoColor=white" alt="C++20" />
  <img src="https://img.shields.io/github/license/Wtada233/LankeOS?style=flat" alt="License" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Rust-000000?style=flat&logo=rust&logoColor=white" alt="Rust" />
  <img src="https://img.shields.io/badge/Bash-4EAA25?style=flat&logo=gnubash&logoColor=white" alt="Bash" />
  <img src="https://img.shields.io/badge/Docker-2496ED?style=flat&logo=docker&logoColor=white" alt="Docker" />
  <img src="https://img.shields.io/badge/Wayland-C642FF?style=flat" alt="Wayland" />
  <img src="https://img.shields.io/badge/SQLite-003B57?style=flat&logo=sqlite&logoColor=white" alt="SQLite" />
  <img src="https://img.shields.io/badge/VitePress-5C73E7?style=flat&logo=vitepress&logoColor=white" alt="VitePress" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Claude_Code-D97757?style=flat&logo=claude&logoColor=white" alt="Claude Code" />
</p>

LankeOS is a Linux distribution built using the Linux From Scratch methodology. Its core characteristic is automating and simplifying the build and maintenance pipeline — from upstream version tracking and ABI break detection to incremental rebuilds, packaging, and repository publishing. This repository is the public monorepo of the distribution, made up of the following sub-projects, each with its own README:

> [!WARNING]
> The vast majority of the source code in this project is generated using AI models.
> This does not imply that the code quality is poor—there is human review and extensive testing as a safety net—but it may leave behind some comments/documentation artifacts.
> For example, meaningless comments like "User requested..." or "0.18 fix" may appear. These are documentation issues, not code quality problems. If you encounter any, please file an issue or submit a PR to fix them.
> As for the "AI Slop" bias, it does not directly characterize LankeOS. The project is more about extensively using AI for writing and maintenance, rather than being a pure pile of unmanaged, neglected spaghetti code.

## Components

| Sub-project | Description | Docs |
|---|---|---|
| [lpkg](lpkg/) | C++20 package manager: atomic WAL transactions with rollback, `needed_so` ABI validation, aggregate index, static builds | [README](lpkg/README.en.md) |
| [farm](farm/) | Rust ABI-driven incremental build farm with container-isolated builds | [README](farm/README.en.md) |
| [pkgs](pkgs/) | Hundreds of package build recipes (`LankeBUILD` + `LankeBUILD.json`) | — |
| [live](live/) | Live ISO / initramfs tooling with three boot modes | — |
| [site](site/) | VitePress documentation site (zh + en) | [lankeos.wtada233.top](https://lankeos.wtada233.top) |

## Quick Start

- Build and use the package manager → [lpkg README](lpkg/README.en.md)
- Drive ABI-aware incremental builds → [farm README](farm/README.en.md)
- Install guide, system requirements, and downloads → [documentation site](https://lankeos.wtada233.top)

## Contributing

1. Fork the repository.
2. Create a feature branch (`git checkout -b feature/my-change`).
3. Commit your changes with a clear message.
4. Push to your fork and open a Pull Request.

## License

[GPL-3.0](LICENSE)

<!-- BEAUTIFIED -->
