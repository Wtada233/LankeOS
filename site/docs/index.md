---
layout: home

hero:
  name: LankeOS
  text: 从零构建的 Linux 发行版
  tagline: 基于 Linux From Scratch，配备自研 C++20 包管理器 lpkg 与 ABI 驱动构建农场 lankefarm，Wayland 桌面，构建与维护流程高度自动化。
  image:
    src: /assets/preview.webp
    alt: LankeOS 桌面截图 — Wayland 桌面上运行的完整桌面环境
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
  - title: ↻ ABI 驱动的增量构建
    details: 构建农场 lankefarm 从旧索引的 needed_so / provides 算出被移除的 SONAME，直连查出消费者包——不做树状闭包。libxml2 断裂只重建 llvm，llvm 的 ABI 未变则 rust 不动。重建集合恰好等于语义上必须重建的那一批。
  - title: ⇄ ABI 过渡备份
    details: 某个库的 SONAME 变化时，旧 .so 被自动备份并注入每个构建容器，新旧 ABI 在整个重建期间并行存活。已安装的旧二进制不会因为一次库升级而断链。
  - title: ▲ 声明式上游版本追踪
    details: 每个包一份可读的 tracker YAML，声明版本来源与筛选规则——主版本锁定、数值封顶、异常 tag 黑名单、只取偶 minor。lankefarm track 据此跟进上游，绝大多数包不需要写一行脚本。
  - title: ▣ 自研包管理器 lpkg
    details: C++20 编写，纯静态单文件、无运行时依赖。WAL 2.0 原子事务：批次全或无、失败自动回滚、回滚自身被中断也能幂等续传。运行时依赖由产物 ELF 的 needed_so 反查 provider 自动推导，不靠手写——只有 dlopen / QML 这类扫不出的隐式依赖才需要声明。
  - title: ✓ 仓库级语义审计
    details: farm chk 在整仓范围内查一致性：QML import 的归属包、.pc 的 Requires、.gir / .vapi 的构建依赖声明、Python 字节码残留、sysusers / tmpfiles 钩子，以及全仓库的符号@版本 ABI 审计。
  - title: ≡ 可复现打包
    details: 重打包一律归一化：mtime 归零、uid / gid 固定、按路径排序遍历、扩展属性（含 security.capability）完整保留。相同的输入产出逐字节相同的包。
  - title: ⇩ 上手门槛低
    details: 下载 ISO、启动、sudo lanke_install 三步装完。不像源码发行版那样需要自己编译整个系统，也不必手工分区与配置引导——安装器自动完成 GPT 分区、格式化与 GRUB 配置。
  - title: ⊞ Wayland 桌面，双生态
    details: KDE Plasma 与 GNOME 核心栈都可用，Wayland 原生（X11 应用走 xwayland）。Intel / AMD / NVIDIA 三大厂商全部走开源驱动（iris / radeonsi / nouveau + NVK），不需要闭源 blob。
  - title: 文 中文友好
    details: Fcitx5 中文输入法与 Noto Sans CJK 字体开箱即用，lpkg 提供完整的中英文双语界面与本地化报错。
---
