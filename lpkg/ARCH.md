# lpkg 原子事务架构

## 图例

```
──→  正常流程     ~~→  异常/回滚
[方框] = 阶段     <菱形> = 判断
(圆角) = WAL 日志行写入
```

---

## 1. 核心概念

### 1.1 "撤销"有三种，每种语义不同

| 操作 | WAL 记录 | 备份文件 | 运行钩子 | 审计语义 |
|------|----------|----------|----------|----------|
| `InstallationTask` 内部 ROLLBACK | `ROLLBACK pkg` → `END pkg` | 不生成（文件未修改） | 否 | "安装过程中失败" |
| `rollback_installed_package()` | `ROLLBACK pkg` → `END pkg` | 直接删除（文件已安装） | 否 | "批次失败，撤销已装包" |
| `remove_package()` | `RM_BEGIN` → `BACKUP` → `RM_COMMIT` → `RM_END` | 先 `.lpkg_bak` 再删 | 运行 `prerm` | "用户主动删除了包" |

**决策规则：**
- 安装中途失败 → `InstallationTask::run()` 的 catch 写 `ROLLBACK + END`（文件级 undo 已在 `rollback()` 中完成）
- 批量前序包已成功安装、后序包失败（或外层异常触发）→ `rollback_installed_package()` 写 `ROLLBACK + END`（内存 DB 撤销 + 落盘 + 删文件）
- 用户 `lpkg remove` / `autoremove` → `remove_package()` 写 `RM_BEGIN/…/RM_COMMIT`（含备份和钩子）

### 1.2 两层 WAL 事务

```
外层（批次级，由 install_packages / remove_package_recursive 管理）：
  BEGIN_PKGS <N>    —— 事务开始，N = 本次涉及的包数量
  ...               —— 内层操作行（BACKUP / COPY / NEW / NEW_DIR / DB / DBRM / DBNEW 等）
  COMMIT_PKGS        —— 事务完结

内层（包级，由 InstallationTask / remove_package 管理）：
  安装成功：BEGIN pkg ver → ... → COMMIT pkg ver → END pkg ver
  安装回滚：BEGIN pkg ver → ... → ROLLBACK pkg ver → END pkg ver
  移除成功：RM_BEGIN pkg ver → ... → RM_COMMIT pkg ver → RM_END pkg ver
```

**`BEGIN_PKGS` / `COMMIT_PKGS` 是 `trim_completed` 和 `recover_packages` 唯一识别的标记。**
内层的 `BEGIN/COMMIT/ROLLBACK/END/RM_BEGIN/RM_COMMIT` 等**不**影响外层批次状态。

### 1.3 rec 的正确定位

```
rec 只应对以下紧急场景：
  - 断电（进程无痕消失，catch 未执行）
  - 段错误（SIGSEGV，进程立刻终止）
  - OOM killer（SIGKILL，不可捕获）

正常中断（Ctrl+C、安装依赖失败）走的都是 try-catch 路径：
  1. 内层 InstallationTask::run() 的 catch 写 ROLLBACK + END（文件级回滚）
  2. 外层 install_packages_internal / install_packages 的 catch
     写 rollback_installed_package（前序包撤销）+ COMMIT_PKGS（批次完结）

rec 只在进程（包括 catch）根本没能跑到的场景下使用。
```

| 场景 | 最终状态 | 需要 rec? |
|------|---------|----------|
| Ctrl+C → 内层 rollback → 外层补 COMMIT_PKGS | 干净回滚 | **否** |
| 正常安装/移除完成 | 完全提交 | **否** |
| 断电：BEGIN_PKGS 已写但操作未完成 | 文件半残 | **是** |
| 断电：RM_BEGIN 已写但 RM_COMMIT 未写 | `.lpkg_bak` 存在 | **是** |
| OOM：批量部分完成 | 部分包已提交 | **是** |
| 段错误：任何 WAL 不完整 | 系统不一致 | **是** |

---

## 2. 安装流程（install_packages）

```
install_packages(pkg_args, hash_file, force)
│
├── trim_completed()                     ← 压缩已完结日志
├── Cache::load()
├── TmpDirManager + Repository.load_index()
│
├── 解析参数（3 种格式）
│   · 本地 .lpkg / .zst 文件
│   · 包名:版本
│   · 纯包名（取最新版）
│   └─→ targets = [(name, ver), ...]
│
├── 一致性重试循环 ───────────────────────────────────────────────┐
│   │                                                              │
│   ├── resolve_package_dependencies()                             │
│   │    递归 DFS（depth ≤ 64），生成 plan + order                  │
│   │                                                              │
│   ├── check_plan_consistency(plan)                               │
│   │    ┌─ 有冲突? ─→ 用户确认移除? ──→ 移除 → 重新解析 ──────────┘
│   │    └─ 无冲突 → 继续                                          │
│   │                                                              │
│   ├── check_needed_so_consistency(plan)                          │
│   │    ┌─ 有断裂? ─→ 用户确认移除? ──→ 移除 → 重新解析 ──────────┘
│   │    └─ 无断裂 → 继续                                          │
│   │                                                              │
│   └── 全部通过 → break                                           │
│                                                                  │
├── needed_so 完整性校验（3 层兜底：plan → 缓存 → repo）            │
│   ┌─ 有 SONAME 无提供者? ─→ LpkgException ✕                      │
│   └─ 全部可解析 → 继续                                            │
│                                                                  │
├── 用户确认（显示安装列表） → (y/N)                                │
│   ┌─ 拒绝? ─→ return                                              │
│   └─ 接受 → 继续                                                  │
│                                                                  │
├── (BEGIN_PKGS <N>)                          ← 外层批次 WAL       │
│                                                                  │
├── try:                                                            │
│   │                                                                │
│   ├── install_packages_internal(ctx) ──────────────────────────  │
│   │   │                                                          │
│   │   │  i = 0                                                    │
│   │   │  loop (i < order.size()):                                │
│   │   │    │                                                      │
│   │   │    ├── sigint_graceful? ─→ throw LpkgException ✕        │
│   │   │    │                                                      │
│   │   │    ├── 元数据验证（若未验证）                             │
│   │   │    │   download_and_verify → read metadata.json           │
│   │   │    │   ┌─ metadata 与索引不匹配? ─→ 更新 repo            │
│   │   │    │   │   clear plan → re-resolve → i=0 restart         │
│   │   │    │   └─ 匹配 → 缓存 local_path, verified=true           │
│   │   │    │                                                      │
│   │   │    └── InstallationTask::run() ────────────────────────  │
│   │   │        │                                                 │
│   │   │        ├── prepare():                                    │
│   │   │        │   download_and_verify → extract                 │
│   │   │        │   → ensure_dependencies_satisfied               │
│   │   │        │   → check_for_file_conflicts                   │
│   │   │        │                                                 │
│   │   │        ├── (BEGIN <pkg> <ver>)        ← 内层 WAL        │
│   │   │        │                                                 │
│   │   │        ├── backup_existing_files()                       │
│   │   │        │   逐文件: (BACKUP) / (NEW) / (NEW_DIR)         │
│   │   │        │   sigint_graceful? ← 逐文件检查                │
│   │   │        │                                                 │
│   │   │        ├── copy_package_files()                          │
│   │   │        │   逐文件: (COPY) → .lpkgtmp → rename            │
│   │   │        │   sigint_graceful? ← 逐文件检查                │
│   │   │        │   ┌─ 异常 ─→ rollback()                       │
│   │   │        │   │   (ROLLBACK) + (END) ~~→ throw ✕          │
│   │   │        │   └─ 成功 → 继续                               │
│   │   │        │                                                 │
│   │   │        ├── commit_without_file_ops()                     │
│   │   │        │   register_package()   ← 内存 Cache 更新       │
│   │   │        │   remove_obsolete_files（升级时）               │
│   │   │        │   run_post_install_hook()                       │
│   │   │        │                                                 │
│   │   │        ├── (COMMIT <pkg> <ver>)                         │
│   │   │        ├── cleanup_backups()   ← 删 .lpkg_bak           │
│   │   │        ├── (END <pkg> <ver>)                             │
│   │   │        └── → 返回 ✓                                      │
│   │   │                                                          │
│   │   │    ctx.successfully_installed.push_back(name)            │
│   │   │    ctx.installed_set.insert(name)                        │
│   │   │    i++ → loop                                            │
│   │   │                                                          │
│   │   │    ┌─ catch(...) ────────────────────────────────────    │
│   │   │    │   rollback_committed_packages() ← 撤销前序包        │
│   │   │    │   throw                                              │
│   │   │    └──────────────────────────────────────────────────    │
│   │   │                                                          │
│   │   └── 循环结束 → 返回 ✓                                      │
│   │                                                              │
│   ├── Cache::instance().write("pkgs")   (DB/DBNEW WAL)          │
│   ├── (COMMIT_PKGS)                                             │
│   └── TriggerManager::run_all()                                 │
│                                                                  │
├── catch(...):                                                    │
│   ├── rollback_committed_packages()       ← 撤销已安装包         │
│   ├── (COMMIT_PKGS)                       ← 关闭批次            │
│   └── throw                                                      │
│                                                                  │
└── cleanup_db_backups()                                           │
    → "安装完成"                                                   │
```

### 2.1 异常路径决策树

```
情况 A：异常在 install_packages_internal 的 for 循环内发生
        （一个包安装失败，内层 catch 已写 ROLLBACK + END）

  → install_packages_internal 的 catch：
      rollback_committed_packages() 撤销 successfully_installed 中所有前序包
      （写 ROLLBACK + END + cache.write）
  → 异常传播到 install_packages 的 catch：
      rollback_committed_packages() → 列表已空 → 无操作
      COMMIT_PKGS 关闭批次
  → throw

情况 B：异常在 install_packages_internal 正常返回后发生
        （如 break_before_db_write、break_before_commit_pkgs 测试断点）

  → install_packages 的 catch：
      rollback_committed_packages() 撤销 successfully_installed 中所有已安装包
      COMMIT_PKGS 关闭批次
  → throw
```

---

## 3. 重装流程（reinstall_package）

```
reinstall_package(arg)
│
├── 解析 arg：
│   ┌─ 含 '/' 或 '.lpkg' 结尾? → 从 metadata.json 读包名
│   └─ 否则 → 参数本身即包名
│
├── 包未安装? → install_packages({arg})     ← 退化到安装流程
│
├── 保存旧的 force_overwrite_mode
├── set_force_overwrite_mode(true)          ← 强制覆盖
│
└── install_packages({arg}, "", true)       ← force_reinstall=true
    │                                        走的完全就是安装流
    └── 恢复旧的 force_overwrite_mode
```

---

## 4. 升级流程（upgrade_packages）

```
upgrade_packages()
│
├── trim_completed()
├── TmpDirManager + Repository.load_index()
│
├── 快照已安装包列表：
│   installed = [(name, version), ...]
│
├── 构建升级计划：
│   for each (n, curr) in installed:
│   │   ├── ⑂ sigint_graceful? → return           ← 同 install 的 SIGINT 检查
│   │   ├── repo.find_package(n)
│   │   │   ┌─ 不存在? → continue
│   │   │   └─ 存在 → version_compare(curr, lat)?
│   │   │       ┌─ 无需升级? → continue
│   │   │       └─ 需要升级 → plan.push(n, lat, held, curr, hash)
│   │   └── 下一个包
│   │
│   ├── plan 为空? → "全部最新"，return
│   │
│   ├── 用户确认（与 install 同款 prompt）
│   │   ┌─ 拒绝? → return
│   │   └─ 接受 → 继续
│   │
│   ├── with_batch_transaction(success, "upgrade", N)  ← 共享事务层
│   │   ├── (BEGIN_PKGS <N>)
│   │   │
│   │   ├── try:
│   │   │   for each e in plan:
│   │   │   │   ├── ⑂ sigint_graceful? → throw      ← 逐包检查
│   │   │   │   ├── InstallationTask::run()
│   │   │   │   │   ← 复用安装流（含内层 BEGIN/COMMIT/END）
│   │   │   │   └── success.push_back(e.name)
│   │   │   │
│   │   ├── Cache::instance().write("upgrade")
│   │   ├── (COMMIT_PKGS)
│   │   │
│   │   └── catch(...):
│   │       ├── rollback_committed_packages(success)  ← 已升包全部降级
│   │       └── (COMMIT_PKGS) → throw
│   │
│   └── cleanup_db_backups()
│
└── → "N 个包已升级"

══════════════════════════════════════════════════════
  v5.1 变更：upgrade 现在使用与 install 相同的统一
  事务协议（BEGIN_PKGS / COMMIT_PKGS）。任一包升
  级失败（含 sigint）→ 已升级包全部降级，系统保持
  一致。不再有"各包独立升级"的行为。
══════════════════════════════════════════════════════
```

---

## 5. 移除流程（remove_package）

```
remove_package(pkg_name, force, wrap_in_txn)
│
├── trim_completed()              (if wrap_in_txn)
├── 版本检查 → 不存在则 return
│
├── 安全检查（!force 时）：
│   ├── is_essential? → return
│   ├── get_reverse_deps 非空? → return
│   └── 包提供虚拟包的反向依赖非空? → return
│
├── (BEGIN_PKGS 1)                (if wrap_in_txn)
├── (RM_BEGIN <pkg> <ver>)
│
├── prerm hook
│
├── 共享文件检查
│   ┌─ 有共享文件? → LpkgException ✕
│   └─ 无 → 继续
│
├── 备份阶段：逐文件
│   for each owned_file（逆字典序）:
│     (BACKUP <phys> → <phys>.lpkg_bak_<pkg>)
│     fs::rename(phys, phys.lpkg_bak_<pkg>)
│
├── remove_package_files(pkg_name, force)   ← 阶段 2
│   ├── 共享文件检查（再次）
│   ├── 逆字典序遍历：
│   │   ├── 目录? → 跳过
│   │   └── 文件? → fs::remove + remove_file_owner + remove_providers
│   └── [In file owner map from cache]
│
├── 目录处理：释放所有权 → 清扫 .lpkg_bak → rmdir（阶段 3）
│   for each dir（逆字典序）:
│     remove_file_owner
│     ┌─ 还有其他所有者? → 跳过
│     └─ 无:
│       ├─ 清扫目录内 .lpkg_bak_<pkg> → (RM_BAK_CLN)
│       └─ fs::remove(dir) → (RM_DIR)
│
├── 清理 dep / needed_so / man / hooks 文件（阶段 4）
│   (DBRM) for dep file
│   (DBRM) for needed_so file
│   (DBRM) for man page
│   fs::remove_all(hooks_dir/pkg)
│   cache.remove_installed(pkg)
│
├── Cache::instance().write(pkg_name)    ← DB 落盘（WAL 保护）
│
├── (RM_COMMIT <pkg> <ver>)              ← 阶段 6
├── 清除全部 .lpkg_bak
├── (RM_END <pkg> <ver>)
│
├── cleanup_db_backups()
├── (COMMIT_PKGS)                        (if wrap_in_txn)
│
└── → "移除成功"
```

### 5.1 rollback_installed_package（事务回滚）vs remove_package（用户删除）

| 维度 | `remove_package` | `rollback_installed_package` |
|------|------------------|------------------------------|
| WAL 前缀 | `RM_BEGIN` | `ROLLBACK` |
| 备份 | `.lpkg_bak` 备份原文件再删 | 直接删除 |
| prerm 钩子 | 运行 | 不运行 |
| 共享文件检查 | 阻止删除 | 不检查 |
| 逆向依赖检查 | 阻止删除 | 不检查 |
| 审计含义 | 用户主动删除 | 事务回滚 |

---

## 6. 递归移除流程（remove_package_recursive）

```
remove_package_recursive(pkg_name)
│
├── collect_recursive_remove_set(pkg_name)
│   BFS：本包传递反向依赖 + 本包提供虚拟包的反向依赖
│
├── 排除 essential 包
├── 显示受影响列表
├── 按反向依赖数量升序排列（叶子先删）
│
├── 3 轮验证码确认（交互模式）
│   ┌─ 输入错误? ─→ return
│   └─ 全部正确 → 继续
│
├── trim_completed()
├── (BEGIN_PKGS <N>)
│
├── try:
│   ├── for each p in to_remove:
│   │     remove_package(p, force=true, wrap_in_txn=false)
│   │     └─→ 走标准移除流（不含外层的 BEGIN/COMMIT_PKGS）
│   ├── Cache::instance().write("recursive-remove")
│   └── (COMMIT_PKGS)
│
├── catch(...):
│   ├── COMMIT_PKGS
│   └── throw       ← rec 会通过 reverse_execute 回滚已移除的包
│
├── cleanup_db_backups()
└── → "递归移除完成"
```

---

## 7. rec 恢复流程（recover_packages）

```
recover_packages()          "lpkg rec"
│
├── 读取 transaction.log
│
├── ─── 统一状态机 ──────────────────────────────
│   只识别两个标记：
│     BEGIN_PKGS → in_txn = true,  开始累积操作行
│     COMMIT_PKGS → in_txn = false, 清空累积
│
│   其余所有行（BEGIN/END/COMMIT/ROLLBACK/RM_BEGIN/
│        RM_COMMIT/BACKUP/COPY/NEW/NEW_DIR/DB/DBRM/
│        DBNEW 等）一概不改变 in_txn，全部积累。
│
│   遇到第二个 BEGIN_PKGS（嵌套）：
│     → 保存当前累积到 uncommitted_txns
│     → 重新开始累积
│
│   文件结束 → in_txn=true → 存入 uncommitted_txns
│
├── ┌─ uncommitted_txns 为空?
│   │  → "无未完成事务"，return
│   └─ 非空 → for each 未完成事务:
│       │
│       ├── parse_op() → WALOp 列表
│       │   跳过错行和不合法行（断电部分写入安全）
│       │
│       └── reverse_execute(ops, root_dir) ─────────────┐
│           │  逆序遍历 ops：                            │
│           │                                            │
│           ├── BACKUP / REMOVE_OLD                      │
│           │   rename(arg2, arg1)  ← .bak → 原文件     │
│           │   文件不存在? → skip                       │
│           │                                            │
│           ├── COPY                                    │
│           │   ┌─ dst 存在? ─→ 安全检查                │
│           │   │  ┌─ 对应 BACKUP 已消费?               │
│           │   │  │  → skip（幂等性保护）              │
│           │   │  └─ 未消费? → fs::remove(dst)         │
│           │   └─ .lpkgtmp 存在? → remove              │
│           │                                            │
│           ├── NEW → fs::remove(path)                  │
│           ├── NEW_DIR → 清理 .lpkg_bak → rmdir        │
│           ├── RM_DIR → fs::create_directories(path)   │
│           ├── RM_BAK_CLN → skip                       │
│           ├── DB / DBRM → rename(.lpkg_db_bak_, orig) │
│           └── DBNEW → fs::remove(path)               │
│           │                                            │
│           return (restored, cleaned)                   │
│                                                       │
├── (COMMIT_PKGS)   ← 每个恢复的事务标记完结            │
├── cleanup_db_backups()                                │
└── → "恢复完成：N 文件已恢复，M 已清理"                │
```

---

## 8. WAL 协议

### 8.1 完整日志行列表

| 行前缀 | 所属组件 | 含义 |
|--------|---------|------|
| `BEGIN_PKGS <N>` | install / upgrade / rec-remove | 批次事务开始，N=包数 |
| `COMMIT_PKGS` | install / rec-remove | 批次事务完结 |
| `BEGIN <pkg> <ver>` | InstallationTask | 单包安装开始 |
| `COMMIT <pkg> <ver>` | InstallationTask | 单包安装提交（注册完成） |
| `ROLLBACK <pkg> <ver>` | InstallationTask / rollback | 单包安装回滚 |
| `END <pkg> <ver>` | InstallationTask / rollback | 单包安装结束 |
| `BACKUP <src> → <dst>` | install / remove | 文件备份到 `.lpkg_bak` |
| `COPY <src> → <dst>` | install | `.lpkgtmp` → 目标路径 |
| `NEW <path>` | install | 新文件 |
| `NEW_DIR <path>` | install | 新目录 |
| `RM_BEGIN <pkg> <ver>` | remove | 移除开始 |
| `RM_COMMIT <pkg> <ver>` | remove | 移除提交 |
| `RM_END <pkg> <ver>` | remove | 移除结束 |
| `RM_DIR <path>` | remove | 目录已删除 |
| `RM_BAK_CLN <path>` | remove | 目录内 `.lpkg_bak` 已清扫 |
| `REMOVE_OLD <src> → <dst>` | upgrade | 旧版废弃文件移除 |
| `DB <path> <tag>` | Cache | DB 文件修改 |
| `DBNEW <path> <tag>` | Cache | DB 文件新建 |
| `DBRM <path> <tag>` | Cache | DB 文件删除 |
| `RESTORE <bak> → <orig>` | rec | 恢复操作（rec 追加） |

### 8.2 WAL 示例

**成功安装：**
```
BEGIN_PKGS 1
BEGIN curl 8.11.1
BACKUP /usr/bin/curl → /usr/bin/curl.lpkg_bak_curl
NEW /usr/share/doc/curl/README
COPY /tmp/curl.lpkgtmp → /usr/bin/curl
COMMIT curl 8.11.1
END curl 8.11.1
DB /var/lib/lpkg/pkgs curl
COMMIT_PKGS
```

**Ctrl+C 中断安装（新行为）：**
```
BEGIN_PKGS 1
BEGIN openjdk25 25.0.4
COPY .../jmod → .../jmod
ROLLBACK openjdk25 25.0.4      ← 内层 catch
END openjdk25 25.0.4
COMMIT_PKGS                      ← 外层 catch 补写，批次关闭
```

**批量 [A,B] 中 B 失败 → A 事务回滚：**
```
BEGIN_PKGS 2
BEGIN pkgA 1.0
BACKUP /usr/bin/a → /usr/bin/a.lpkg_bak_pkgA
COPY /tmp/a → /usr/bin/a
COMMIT pkgA 1.0
END pkgA 1.0
BEGIN pkgB 1.0
COPY /tmp/b → /usr/bin/b
ROLLBACK pkgB 1.0               ← B 安装失败，内层 catch
END pkgB 1.0
DB /var/lib/lpkg/pkgs pkgA      ← rollback_installed_package 落盘
ROLLBACK pkgA 1.0               ← A 被事务回滚（非用户删除）
END pkgA 1.0
COMMIT_PKGS
```

**断电（需要 rec）：**
```
BEGIN_PKGS 3
BEGIN pkgA 1.0
BACKUP /a → /a.lpkg_bak_pkgA
COPY /tmp/a → /a                 ← 写到此断电
（进程死亡，无 ROLLBACK，无 COMMIT_PKGS）
→ rec: 反向执行 BACKUP → 恢复文件, COPY → 清理临时文件
```

**用户主动移除：**
```
BEGIN_PKGS 1
RM_BEGIN curl 8.11.1
BACKUP /usr/bin/curl → /usr/bin/curl.lpkg_bak_curl
RM_DIR /usr/share/doc/curl/
DBRM /var/lib/lpkg/deps/curl curl
DBRM /var/lib/lpkg/needed_so/curl curl
DB /var/lib/lpkg/pkgs curl
RM_COMMIT curl 8.11.1
RM_END curl 8.11.1
COMMIT_PKGS
```

---

## 9. 不变量（Invariants）

以下不变量在任何情况下（安装/移除/升级/Ctrl+C/断电恢复）都必须保持成立。

### 9.1 WAL 结构不变量

- **I-WAL-1**: 每个 `BEGIN_PKGS` 必须有且仅有一个匹配的 `COMMIT_PKGS`（单一事务）或不在日志中（活跃事务）。
- **I-WAL-2**: `trim_completed` 只识别 `BEGIN_PKGS`/`COMMIT_PKGS` 作为状态标记，内层标记不影响批次状态。
- **I-WAL-3**: `recover_packages` 使用与 `trim_completed` 完全相同的状态机。

### 9.2 操作语义不变量

- **I-OP-1**: `rollback_installed_package` 写 `ROLLBACK` + `END`，不写 `RM_BEGIN`。
- **I-OP-2**: `remove_package` 写 `RM_BEGIN` + `RM_COMMIT` + `RM_END`，不写 `ROLLBACK`。
- **I-OP-3**: `rollback_installed_package` 不运行 `prerm` 钩子。
- **I-OP-4**: `remove_package` 运行 `prerm` 钩子。
- **I-OP-5**: 成功安装后立即 `cleanup_backups()` → `.lpkg_bak` 被删除。

### 9.3 原子性不变量

- **I-ATOM-1**: 单包安装失败 → 系统无该包的文件残留、DB 无该包记录。
- **I-ATOM-2**: 批量 `[A, B]` 中 B 失败 → A 被 `rollback_installed_package` 撤销。
- **I-ATOM-3**: 批量 `[A, B]` 中 B 失败 → WAL 中 A 是 `ROLLBACK` 行，不是 `RM_BEGIN` 行。
- **I-ATOM-4**: 任何阶段中断后，系统和 DB 最终一致：包要么存在（DB+文件），要么不存在。

### 9.4 恢复不变量

- **I-REC-1**: `recover_packages()` 在有 `COMMIT_PKGS` 的日志上运行 → 无操作。
- **I-REC-2**: `recover_packages()` 在无 `COMMIT_PKGS` 的日志上运行 → 恢复到事务前。
- **I-REC-3**: `recover_packages()` 多次运行 → 幂等。
- **I-REC-4**: 正常 Ctrl+C 后运行 `rec` → 无操作（批次已被 COMMIT_PKGS 关闭）。
- **I-REC-5**: rec 是紧急工具，不用于正常中断清理。
- **I-REC-6**: rec 只处理"进程在 catch 前死亡"的场景。

### 9.5 幂等性不变量

- **I-IDEM-1**: `reverse_execute` 对 `BACKUP` 反向 → `.bak` 不存在则 skip。
- **I-IDEM-2**: `reverse_execute` 对 `COPY` 反向 → 对应 `BACKUP` 已消费则 skip。
- **I-IDEM-3**: `rollback_installed_package` 同一包调用两次 → 第二次 `get_installed_version` 空 → return。
- **I-IDEM-4**: `recover_packages()` 连续执行多次 → 结果相同。

### 9.6 审计不变量

- **I-AUDIT-1**: WAL 中 `ROLLBACK` 行含义 = "事务回滚"，不解释为用户删除。
- **I-AUDIT-2**: WAL 中 `RM_BEGIN`/`RM_COMMIT` 行含义 = "用户主动删除了包"。
- **I-AUDIT-3**: 一个包的 WAL 生命周期是唯一的：要么 `BEGIN → COMMIT/ROLLBACK → END`，要么 `RM_BEGIN → RM_COMMIT → RM_END`。不会混合出现两种模式。
- **I-AUDIT-4**: `REMOVE_OLD`（升级时旧文件清理）与 `BACKUP`（覆盖前备份）语义分离，可区分。

---

## 10. 自动恢复（Auto-Recovery）

### 10.1 动机

WAL 中残留未完成事务时启动新操作会导致日志状态机混乱：

```
旧事务：BEGIN_PKGS 1 → BACKUP /usr/bin/foo → 进程断电死亡（无 COMMIT_PKGS）
新事务：lpkg install bar → trim_completed() 保留旧事务 → 追加 BEGIN_PKGS 1 ...
```

`recover_packages()` 读到两个 `BEGIN_PKGS` 会把第一段未提交事务加入 `uncommitted_txns`
然后 `reverse_execute`——但新操作对文件系统的修改可能已被反向执行错误撤销。

### 10.2 自动恢复机制

**所有写操作在开始新事务前自动调用 `recover_packages()`**，确保 WAL 始终是干净的再开始新事务。

触发点（`package_manager.cpp`）：

| 操作 | 入口函数 | 恢复时机 |
|------|---------|---------|
| `install` | `install_packages()` | `trim_completed()` 前 |
| `remove` | `remove_package(wrap_in_txn=true)` | `trim_completed()` 前 |
| `upgrade` | `upgrade_packages()` | `trim_completed()` 前 |
| `remove -r` | `remove_package_recursive()` | `trim_completed()` 前 |
| `reinstall` | `reinstall_package()` → 调 `install_packages()` | 同上 |
| `autoremove` | 调 `remove_package()` | 同上 |

### 10.3 流程

```
lpkg install pkgA       lpkg remove pkgB       lpkg upgrade
     │                       │                      │
     ├── recover_packages()  ← 先处理 WAL 残留      │
     │   ├─ reverse_execute  (恢复文件 + DB)         │
     │   └─ COMMIT_PKGS      (标记事务完结)           │
     │                                                │
     ├── trim_completed()    ← 所有事务已完结 → 清空  │
     ├── ... (正常流程)                                │
     └── BEGIN_PKGS → ... → COMMIT_PKGS               │
         在干净的 WAL 上开始新事务                      │
```

### 10.4 不变量

- **I-AUTO-1**: 任何写操作前 WAL 中无未完成事务（由 `recover_packages` 保证）。
- **I-AUTO-2**: `recover_packages` 在同一操作中被多次调用 → 幂等（第二次看到 `COMMIT_PKGS` → 无操作）。
- **I-AUTO-3**: 自动恢复不替代 `lpkg rec`——手动调用 `rec` 始终可用，但日常不需要。
- **I-AUTO-4**: 自动恢复产生的 `COMMIT_PKGS` 可被 `trim_completed` 正常压缩。 
