---
layout: home

hero:
  name: LankeOS
  text: 从零构建的 Linux 发行版
  tagline: 基于 Linux From Scratch，配备自研 C++20 包管理器 lpkg，Sway on Wayland 桌面，从内核到浏览器全部亲手构建。
  image:
    src: /screenshots/desktop.png
    alt: LankeOS 桌面截图
  actions:
    - theme: brand
      text: 下载 LankeOS
      link: /download
    - theme: alt
      text: 快速开始
      link: /guide/
    - theme: alt
      text: GitHub
      link: https://github.com/Wtada233/LankeOS

features:
  - title: ⚡ 3 秒启动
    details: 精心优化的 initramfs 从按下电源键到 Sway 桌面就绪仅需约 3 秒。通过屏蔽 ldconfig.service 等非关键启动项实现极速启动。
  - title: 🖥️ 纯 Wayland 桌面
    details: 基于 Sway 平铺式窗口管理器 + Mesa 图形栈，无 X11 包袱。完整 GPU 硬件加速，流畅的 Wayland 原生体验。
  - title: 📦 自研包管理器 lpkg
    details: C++20 编写，支持原子事务回滚、内联元数据验证、聚合索引、ELF 自剥离、多段版本比较。可静态编译至 ~1.5MB。
  - title: 🌐 中文原生支持
    details: Fcitx5 中文输入法开箱即用，Noto Sans CJK 字体预装。lpkg 包管理器提供完整中英文双语界面。
  - title: 🔧 全工具链覆盖
    details: GCC 15.2、LLVM/Clang 21.1、Rust 1.93、Python 3.13、Ruby 4.0、Perl、Lua — 开箱即用的开发环境。
  - title: 💾 Live ISO + 持久化
    details: 可启动 Live ISO，支持 toram 模式加载到内存，也可通过 LABEL=LANKE_DATA 分区实现 OverlayFS 持久化存储。
  - title: 🎵 PipeWire 音频
    details: 完整的 PipeWire 音频/视频路由系统，支持 ALSA 和 JACK 兼容层。USB 音频设备即插即用。
  - title: 🖼️ GTK4 + WebKitGTK
    details: 完整 GTK4 图形栈，支持 WebKitGTK 浏览器引擎、GStreamer 多媒体框架。可运行现代 GTK4 图形应用。
  - title: 🔐 安全体系
    details: Linux-PAM 认证模块、Polkit 授权框架、OpenSSL 加密库、SELinux 支持。完整的多用户安全模型。
---
