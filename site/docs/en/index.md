---
layout: home

hero:
  name: LankeOS
  text: A Linux Distribution Built from Scratch
  tagline: Built on Linux From Scratch, featuring a custom C++20 package manager lpkg, Sway on Wayland desktop — from kernel to browser, all hand-built.
  image:
    src: /assets/preview.webp
    alt: LankeOS desktop screenshot — full desktop environment running on Sway tiling window manager
  actions:
    - theme: brand
      text: Download LankeOS
      link: /en/download
    - theme: alt
      text: Quick Start
      link: /en/guide/
    - theme: alt
      text: GitHub
      link: https://github.com/Wtada233/LankeOS

features:
  - title: ⚡ Blazing Fast Boot
    details: In v0.12, the carefully optimized initramfs takes about 4 seconds from power-on to desktop, achieved by disabling non-essential startup services like ldconfig.service.
  - title: 🖥️ Pure Wayland Desktop
    details: Powered by Sway tiling window manager + Mesa graphics stack — no X11 baggage. Full GPU hardware acceleration for a smooth native Wayland experience.
  - title: 📦 Custom Package Manager lpkg
    details: Written in C++20 with atomic transaction rollback, inline metadata validation, aggregate indexes, ELF self-stripping, and multi-segment version comparison. Supports static compilation.
  - title: 🌐 Full Toolchain Coverage
    details: GCC 15.2, LLVM/Clang 21.1, Rust 1.93, Python 3.13, Ruby 4.0, Perl, Lua — a development environment ready out of the box.
  - title: 🔧 Chinese Language Support
    details: Fcitx5 input method works out of the box with pre-installed Noto Sans CJK fonts. lpkg package manager features a full bilingual Chinese/English interface.
  - title: 💾 Live ISO + Persistence
    details: Bootable Live ISO with toram support to load entirely into RAM. OverlayFS persistence via a LABEL=LANKE_DATA partition for keeping changes across reboots.
  - title: 🎵 PipeWire Audio
    details: Complete PipeWire audio/video routing with ALSA and JACK compatibility layers. USB audio devices are plug-and-play.
  - title: 🖼️ GTK3/4 + Firefox
    details: Full GTK3/4 graphics stack with Firefox browser and GStreamer multimedia framework. Run modern GTK applications.
  - title: 🔐 Security Architecture
    details: Linux-PAM authentication, Polkit authorization framework, OpenSSL and GnuTLS crypto libraries. Complete multi-user security model.
---
