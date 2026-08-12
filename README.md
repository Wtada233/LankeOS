[English](README.en.md) | 中文

<h1 align="center">LankeOS</h1>

<p align="center">
  <strong>专注于自动化与简化发行版维护的 Linux 发行版，配备自研包管理器与 ABI 驱动构建农场。</strong>
  <br />
  <em>Linux From Scratch · lpkg (C++20) · lankefarm (Rust) · Wayland · Live ISO</em>
</p>

<p align="center">
  <a href="#快速上手"><img src="https://img.shields.io/badge/Quick_Start-4CAF50?style=for-the-badge" alt="Quick Start" /></a>
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

LankeOS 是一套基于 Linux From Scratch 方法论构建的 Linux 发行版。其核心特色在于把发行版的构建与维护流程自动化：从上游版本追踪、ABI 断裂检测，到增量重建、打包与仓库发布，均由自研工具链完成。本仓库是其公开 monorepo，由以下子项目组成，每个子项目都带有独立的 README：

## 项目组成

| 子项目 | 说明 | 文档 |
|---|---|---|
| [lpkg](lpkg/) | C++20 编写的包管理器：原子 WAL 事务回滚、`needed_so` ABI 校验、聚合索引、静态构建 | [README](lpkg/README.md) |
| [farm](farm/) | Rust 编写的 ABI 驱动增量构建农场，容器隔离构建 | [README](farm/README.md) |
| [pkgs](pkgs/) | 数百个软件包的构建配方（`LankeBUILD` + `LankeBUILD.json`） | — |
| [live](live/) | Live ISO / initramfs 工具链与三种启动模式 | — |
| [site](site/) | VitePress 文档网站（中英双语） | [lankeos.wtada233.top](https://lankeos.wtada233.top) |

## 快速上手

- 编译与使用包管理器 → [lpkg README](lpkg/README.md)
- 驱动 ABI 感知的增量构建 → [farm README](farm/README.md)
- 安装指南、系统要求与下载 → [文档网站](https://lankeos.wtada233.top)

## 贡献

1. Fork 本仓库。
2. 创建特性分支（`git checkout -b feature/my-change`）。
3. 提交更改，写明清晰的提交信息。
4. 推送到你的 fork 并开启 Pull Request。

## 许可证

[GPL-3.0](LICENSE)

<!-- BEAUTIFIED -->
