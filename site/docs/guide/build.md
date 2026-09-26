---
title: 从源码构建
---

# 从源码构建 LankeOS

> 构建 LankeOS 需要一台 Linux 主机（CPU 建议 8 核以上）、至少 64 GiB 可用磁盘空间、Docker，以及 root 权限。

## 构建环境要求

| 组件 | 要求 |
|------|------|
| 操作系统 | 任意现代 Linux 发行版（Arch/Ubuntu/Fedora 等） |
| CPU | x86_64-v3，建议 8 核以上 |
| 内存 | 建议 8 GiB+ |
| 磁盘 | 至少 64 GiB 可用空间 |
| 容器 | Docker（所有构建都在 fresh 容器内进行，`--image` 为必填参数） |
| 权限 | root（`lankefarm` 与 `lpkg` 的操作命令需要） |

## 获取源码

```bash
git clone https://github.com/Wtada233/LankeOS.git
cd LankeOS
```

## 构建工具链

两个工具是整条流程的基础：`lpkg`（包管理器，负责构建与安装单个包）与 `lankefarm`（构建农场，负责增量选择、排序、ABI 传播与仓库发布）。

### lpkg

```bash
cd lpkg
sudo make docker-install     # 推荐：在 Alpine 容器内编译为纯静态二进制
```

也可以主机直编（需自备 `libcurl`、`libarchive`、`libelf`、`libgit2`、`libsolv`、`openssl` 开发包）：

```bash
make
sudo make install
```

### lankefarm

```bash
cd farm
cargo build --release        # 产物：farm/target/release/lankefarm
```

## 构建系统架构

LankeOS 的构建系统基于以下概念：

### LankeBUILD 格式

每个软件包目录包含两个文件：

```text
pkgs/<package>/
├── LankeBUILD          # 构建脚本（三个 Shell 函数）
└── LankeBUILD.json     # 构建元数据（JSON）
```

### LankeBUILD.json

```json
{
  "name": "package-name",
  "version": "1.2.3",
  "release": 1,
  "sources": ["https://upstream.example.com/pkg-1.2.3.tar.gz"],
  "work_sources": [],
  "build_deps": ["base-devel"],
  "needed_so": ["libc.so.6"],
  "provides": ["libfoo.so.1"],
  "deps": []
}
```

| 字段 | 说明 |
|------|------|
| `sources` | 主源（release tarball / github 归档 / `git+<url>@<tag>`）。`lankefarm track` 会自动更新 |
| `work_sources` | 辅助源（补丁、数据文件、字体等非归档文件） |
| `build_deps` | **构建时**依赖，必须写全（构建容器只装 `build_deps`）。含 `base-devel` 作为锚点 |
| `deps` | **运行时**依赖。通常留空——由 farm 从 `needed_so` 反查生成；只有 dlopen / QML / D-Bus 这类扫描不出的隐式依赖才手写 |
| `needed_so` / `provides` | 由 farm 扫描构建产物的 ELF 自动写入，**不要手编** |
| `release` | 打包修订号，发行版内部重建时递增（成品版本形如 `1.2.3+1`） |
| `no_strip` | 跳过 ELF strip（解释器、编译器、`linux` 等） |

### LankeBUILD 脚本

定义三个阶段，由 `lpkg` 依次调用（`cwd` 是解压后的源码目录）：

```bash
lankebuild_prepare() {
    cd pkg-dir
    # 打补丁、配置生成、autoreconf
    patch -p1 < "{WORK_DIR}/../fix.patch"
}

lankebuild_build() {
    cd pkg-dir
    # 配置、编译、安装到 {STAGING_ROOT}
    ./configure --prefix={PREFIX}
    make -j$(nproc)
    make DESTDIR={STAGING_ROOT} install
}

lankebuild_package() {
    # 打包前的最终调整（删静态库、写 postinst 钩子等）
    :
}
```

`{STAGING_ROOT}` 等占位符在构建时被替换，完整列表见 [lpkg 文档](/lpkg/)。

### 生命周期

```text
1. 解析 LankeBUILD.json → 获取名称、版本、源码 URL
2. 下载并解压源码
3. 替换占位符（{PREFIX}、{STAGING_ROOT}、{WORK_DIR} 等）
4. （可选）执行 pkgs/<包名>/hacks.sh —— 包级环境修补
5. 依次执行 lankebuild_prepare() → lankebuild_build() → lankebuild_package()
6. 后处理：strip ELF、清理 libtool 文件、生成 SONAME 链接
7. 打包为 .lpkg（tar.zst + metadata.json）；**成功构建后**在配方目录写入 .build_ok（内容 = 当前配方哈希；跳过/blocked 的包不写）
```

## 构建整个发行版

### 1. 冷启动：从远程仓库播种

farm **禁止无基线构建**——`out/<arch>/index.txt` 同时是容器可见的索引和 ABI 传播的基线，必须存在。所以第一步是播种：

```bash
lankefarm seed --remote https://lankerepo.wtada233.top --out out
```

`seed` 会并行下载远程的 `index.txt` 与全部 `.lpkg`（逐个校验 SHA-256），并把索引**原样**落地：不重写哈希、不重打 `.lpkg`。这样容器里 `lpkg` 看到的索引与 farm 计算 ABI 传播用的是同一份真源。

离线环境可以用 [archive.org 上的构建快照](https://archive.org/details/lankeos-0.20) 代替（内含 `pkgs.tar.xz` 全量仓库归档与构建工作区快照）。

### 2. 增量构建：validate 与 build

先拉取构建容器的基础镜像（一套预装完整工具链的 LankeOS）：

```bash
docker pull wtada233/lankeos:latest
```

**按配方变化重建**——`validate` 重建所有没有有效 `.build_ok` 标记的包：

```bash
sudo lankefarm validate --image wtada233/lankeos:latest
```

标记的内容就是配方哈希（`sha256(LankeBUILD 内容 + LankeBUILD.json 内容)`），并且它存放在**配方目录**里（`pkgs/<包名>/.build_ok`），与仓库目录 `out/` 无关。所以"配方改了"等价于"标记失效"：**手动改动配方（`build_deps`、补丁、构建参数）或直接删掉 `.build_ok`，该包就会被重建**，不需要自己记哪些包受影响。

`seed` 不影响标记——它只填 `out/`，不碰 `pkgs/`。因此刚 `seed` 完再跑 `validate`，只要配方与仓库里的产物出自同一次构建，它会**一个包都不重建**。

反过来，真要**从源码重建全部约 860 个包**（删掉全部 `.build_ok`，或动了工具链这类会波及大量配方的改动），按当前仓库规模**至少需要 3 天**——chromium、rust、llvm、libreoffice、linux 是主要成本。日常增量只重建受影响的包，规模远小于全量。

**按上游版本重建**——`build --all` 跳过版本与本地仓库一致的包：

```bash
sudo lankefarm build --all --image wtada233/lankeos:latest
```

**强制重建指定包**：

```bash
sudo lankefarm build bash curl --image wtada233/lankeos:latest
```

#### ABI 驱动的增量（farm 与普通构建脚本的核心区别）

某个包重建后，若它不再提供旧的 SONAME，farm 会用**旧索引的 `needed_so`/`provides`** 算出被移除的 SONAME 集合，直接查出消费者包并一并重建——不做树状闭包，因此 `libxml2` 断裂只重建 `llvm`，而 `llvm` 重建后 ABI 未变则 `rust` 不动。`data/build/*.yaml` 里的声明式重建组负责补上"不链该库但 ABI 敏感"的包（Python 生态；Qt 私有 API 的消费者则由脚本扫 ELF 里的版本字符串动态判定）。

重建期间，被移除的旧 `.so` 会备份到 `out/backups/` 并在每个构建容器里恢复 + `ldconfig`，让尚未重建的旧二进制在过渡期继续工作；整个构建结束后确认无人再引用才清理。

其它子命令：`track`（探测上游版本）、`abifix`（修复引用了无 provider SONAME 的包）、`export`（把仓库扁平化为发行布局）、`serve`（本地仓库 HTTP 服务）、`chk`（维护期检則工具集）。

### 3. 组装 rootfs

包全部产出到 `out/<arch>/` 后，组装一个新的根目录。第一步装 `filesystem`——它是唯一带 `keep_fs_layout` 的包，负责声明文件系统布局：

```bash
sudo lpkg install filesystem --root /mnt/lfs --arch x86_64
```

它提供两样东西：

- **usr-merge 布局**：`bin → usr/bin`、`sbin → usr/bin`、`lib → usr/lib`、`lib64 → usr/lib`、`usr/sbin → bin`、`usr/lib64 → lib`
- **`/etc` 骨架与发行版标识**：`/etc/os-release`（指向 `/usr/lib/os-release` 的软链）、`lanke-release`、`profile`、`shells`、`hostname`、`hosts`、`inputrc`、`vimrc`、`issue`、`locale.conf`、`vconsole.conf`、`/etc/skel/*`、`/etc/sysconfig/clock`、`/etc/default/grub`、fastfetch 配置、`/usr/share/lankeos/*`

**`filesystem` 之外还有一小部分需要手工创建**——它们不属于任何包，包管理器无法维护（运行时或安装器会接管）：

- **挂载点目录**：`/proc`、`/sys`、`/dev`、`/run`、`/tmp`，以及 `/var`、`/boot`、`/home`、`/root`、`/mnt`、`/srv`、`/opt` 中你需要的
- **账户文件**：`/etc/passwd`、`/etc/group`、`/etc/shadow`
- **`/etc/resolv.conf`**：可直接从宿主复制
- **`/etc/ssl/certs` 的 CA 证书**：可直接从宿主复制，或装完 `make-ca` 后生成

然后安装其余所需的包：

```bash
sudo lpkg install base --root /mnt/lfs --arch x86_64
```

`base`（基础系统）与 `base-devel`（构建工具链）是元包，桌面可另加 `plasma-meta` 或 `gnome`。

> lpkg 从 `mirror.conf` 读取仓库地址。要让它看到刚构建好的 `out/`，把 `/etc/lpkg/mirror.conf` 指向本地路径（`file:///path/to/out` 或 `/path/to/out`），或另开一个 `lankefarm serve --root out --port 8080` 并填写 `http://127.0.0.1:8080`。

### 4. 可选的包级环境修补

若 `pkgs/<包名>/hacks.sh` 存在，`lpkg build` 会在构建阶段之前打印其内容，并在确认后执行（`-y` 时自动执行），用于个别包所需的特殊环境准备。当前仓库中没有包使用该机制。

## Live ISO 打包

```bash
# 1. 重置系统状态（清除 machine-id、日志、临时文件，重建 mtab 软链等）
#    目标根目录固定为 /mnt/lfs
sudo bash live/reinit.sh

# 2. 生成 squashfs 镜像（pack.sh 不含此步，需自行执行）
sudo mksquashfs /mnt/lfs live/ISO/live/rootfs.sfs -comp xz

# 3. 打包 ISO（需 root；会自动 sudo 提权）
sudo bash live/pack.sh
```

生成的文件为 `live/lankeos-live.iso`（仅 UEFI 启动）。

有两点需要注意：

- `pack.sh` 会从**仓库之外**取内核产物（相对 `live/ISO/boot/` 的 `../../../../../tools/krnl/`，即 `config-lanke` / `System.map-lanke` / `vmlinuz-lanke`），需要你自行放置。
- `pack.sh` 结束时**会删除** `ISO/live/rootfs.sfs` 与内核副本，因此每次打包前都要重新生成 squashfs。

也正因如此（ISO 依赖仓库外的内核与手工组装的 rootfs），ISO 构建不纳入 CI。

## 延伸阅读

- [lpkg 文档](/lpkg/) —— 包管理器、包格式与仓库规范
- `lpkg/ARCH.md` —— WAL 2.0 原子事务架构规范
- `farm/ARCH.md` —— 构建农场架构、增量选择、ABI 传播与版本追踪
