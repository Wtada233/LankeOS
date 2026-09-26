---
layout: home

hero:
  name: LankeOS
  text: A Linux Distribution Built from Scratch
  tagline: Built on Linux From Scratch, featuring a custom C++20 package manager lpkg and an ABI-driven build farm lankefarm, Wayland desktop — build and maintenance pipeline fully automated.
  image:
    src: /assets/preview.webp
    alt: LankeOS desktop screenshot — full desktop environment running on Wayland
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
  - title: ↻ ABI-Driven Incremental Builds
    details: The lankefarm build farm computes removed SONAMEs from the old index's needed_so / provides and locates the consuming packages directly — no transitive closure. A libxml2 break rebuilds llvm only; if llvm's ABI is unchanged, rust stays put. The rebuild set is exactly the set that must be rebuilt.
  - title: ⇄ ABI Transition Backups
    details: When a library's SONAME changes, the old .so is backed up automatically and injected into every build container, so both ABIs coexist throughout the rebuild. Already-installed binaries never break because of a library upgrade.
  - title: ▲ Declarative Upstream Tracking
    details: One readable tracker YAML per package declares its version source and filtering rules — major-version locks, numeric caps, blacklists for anomalous tags, even-minor-only. lankefarm track follows upstream accordingly; the vast majority of packages need no script at all.
  - title: ▣ Custom Package Manager lpkg
    details: Written in C++20 as a static single binary with no runtime dependencies. WAL 2.0 atomic transactions — all-or-nothing batches, automatic rollback on failure, and idempotent resumption even if the rollback itself is interrupted. Runtime dependencies are derived from the artifacts' ELF needed_so rather than written by hand — only implicit ones invisible to scanning (dlopen, QML) need declaring.
  - title: ✓ Repository-Wide Semantic Audits
    details: farm chk checks consistency across the whole repository — which package owns a QML import, a .pc file's Requires, build-dependency declarations for .gir / .vapi, leftover Python bytecode, sysusers / tmpfiles hooks, and a symbol@version ABI audit spanning every package.
  - title: ≡ Reproducible Packaging
    details: Repacking is always normalised — mtime zeroed, uid / gid pinned, traversal sorted by path, extended attributes (including security.capability) preserved. Identical input yields a byte-identical package.
  - title: ⇩ Low Barrier to Install
    details: Download the ISO, boot it, run sudo lanke_install — three steps. No compiling the whole system yourself as source-based distributions require, and no manual partitioning or bootloader setup — the installer handles GPT partitioning, formatting and GRUB configuration.
  - title: ⊞ Wayland Desktop, Two Ecosystems
    details: Both KDE Plasma and the GNOME core stack are available, natively on Wayland (X11 apps run through xwayland). Intel, AMD and NVIDIA all use open-source drivers (iris / radeonsi / nouveau + NVK) — no proprietary blobs required.
  - title: 文 Chinese Friendly
    details: The Fcitx5 input method and Noto Sans CJK fonts work out of the box, and lpkg ships a complete bilingual Chinese/English interface with localised error messages.
---
