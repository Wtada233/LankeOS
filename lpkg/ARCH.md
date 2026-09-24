# lpkg 原子事务架构（WAL 2.0）

> **目标**：坚不可摧的 WAL 原子事务系统。每个 fsync 位置经过推敲，每个回滚可被回滚，任何单点故障（断电/OOM/段错误）不破坏系统一致性。
> **规范**：每个功能编写完成之后都必须有20+模拟真实情况的回归测试，越复杂越好，遇到什么bug就写什么测试，区分测试和真正的代码错误，如果测试过于复杂导致失败，那么得分情况：1.代码有bug，修复代码 2.测试用例不当，修复测试（测试用例不当的前提是该测试用例不可能在现实中触发/该测试期望的行为不是正确的）编写尽可能模拟真实情况的测试，比如remove -r一个包并在中途Ctrl+C看看能否恢复依赖其的所有包和这个包本身
> **要求**：实现中可以通过编写test_breakpoints.cpp来进行更深入的测试：在源码每一个可能出现bug的角落加入断点，这是最好的防止原子性泄漏的方式
> **最终测试必须超过600个，而且不能编写大量低质量测试，而是模拟用户真实使用情况**

---

## 目录

1. [核心架构约束](#1-核心架构约束)
2. [WAL 2.0 协议](#2-wal-20-协议)
3. [原子操作顺序与 fsync](#3-原子操作顺序与-fsync)
4. [DB 状态管理（确定性回滚顺序）](#4-db-状态管理确定性回滚顺序)
5. [批量事务模型](#5-批量事务模型)
6. [安装流程](#6-安装流程)
7. [移除流程](#7-移除流程)
8. [升级流程](#8-升级流程)
9. [回滚引擎](#9-回滚引擎)
10. [RESTORE 审计日志与二次回滚](#10-restore-审计日志与二次回滚)
11. [rec 恢复（Fallback Only）](#11-rec-恢复fallback-only)
12. [WAL Trim](#12-wal-trim)
13. [实现步骤](#13-实现步骤)

---

## 1. 核心架构约束

### 1.1 批次模型（不变）

- **I-1**: 所有写操作（install / remove / upgrade / reinstall / autoremove）均为批次事务，即使只有一个包也走 `BEGIN_PKGS → ... → COMMIT_PKGS`。不存在"独立包事务"。
- **I-2**: 批次内每个包安装/移除完成后：`.lpkg_bak` 保留（延迟到 COMMIT_PKGS 后清理），`.lpkgtmp` 已清，内存 COMMIT 已写，DB 已落盘（带 WAL 保护）。
- **I-3**: 批次提交前（`COMMIT_PKGS` 未写），整个批次视为未完成，可整体回滚。

### 1.2 回滚约束

- **I-4**: 回滚由 try-catch 驱动。任何正常中断（Ctrl+C、安装依赖失败、文件冲突、磁盘满等抛异常的场景）走 catch → `batch_rollback()` → 恢复文件/DB/目录 → 写 `COMMIT_PKGS` 关闭批次。
- **I-5**: `lpkg rec` **只**用于进程在 catch 前已死亡的场景（断电、段错误、OOM killer）。正常 Ctrl+C 后运行 `rec` 应无操作（批次已被 catch 的 `COMMIT_PKGS` 关闭）。
- **I-6**: **批次回滚永不调用 `recover_packages()`**。批次回滚使用 `batch_rollback()` 只操作当前批次的 WAL 行。
- **I-7**: 回滚在同一个 `BEGIN_PKGS`/`COMMIT_PKGS` 边界内执行。即：`COMMIT_PKGS` 既是成功完结的标记，也是回滚完结的标记。WAL 中不存在"回滚无 COMMIT_PKGS"的情况。
- **I-8**: rollback 的每个逆向操作在文件系统生效后，立即写入 `RESTORE_*` WAL 审计行。若 rollback 中途崩溃，重启后的 `recover_packages()` 能通过 RESTORE 行判断哪些操作已完成，跳过它们继续（幂等续传）。

### 1.3 备份清理纪律

- **I-BAK-1**: `.lpkg_bak` 文件在 `COMMIT_PKGS` 之前绝不删除。即使批次中一个包已成功安装，其 `.lpkg_bak` 也必须保留到批次提交后。
- **I-BAK-2**: 安装流程的 `.lpkg_bak` 清理统一在 `COMMIT_PKGS` 之后执行（调用方收集 `task.get_backups()` 后 `fs::remove`）。批次内绝不提前清理，确保批量回滚时可恢复每个已安装包的文件。
- **I-BAK-3**: 移除流程的 `.lpkg_bak` 清理与安装**完全一致**：统一在 `COMMIT_PKGS` **之后**执行
  （`finish_committed_batch()`：写 `CLEANUP` 行 → 物理删除 stash → `trim_completed` → `cleanup_db_backups`）。
  stash 是移除的**唯一回滚来源**（装的是刚被删掉的文件），因此必须活到"批次已不可能回滚"为止——
  推论：**只要批次未提交，中途中断（Ctrl+C/失败）一律整批恢复**；`CLEANUP` 行位于事务之外
  （trailing 记录），未清理完的部分由 `continue_post_commit_cleanup`（§10）续传。

### 1.4 WAL 语义

- **I-9**: WAL（`transaction.log`）是修复日志，不是备份。批次成功提交后，`trim_completed` 删除该批次的全部日志行。
- **I-10**: 批次成功后所有权/DB 状态转接给新包，旧版本的 `.lpkg_db_bak` 和 `.lpkg_bak` 不再需要。跨批次不保留"旧版本撤回"能力。

### 1.5 DB 状态确定性

- **I-11**: 每个 DB 写入（`DB`/`DBNEW` WAL 行）注明 **"这次写是在哪个包的安装/移除之后"**。格式：`DB <path> <pkg>:<state>`。
- **I-12**: 回滚时，通过 DB 里程碑链式恢复到 `:batch-start`。每个 `DB` 条目指出"此时系统在 X 安装完成后的状态"，逆序恢复时还原到前一个里程碑。

### 1.6 fsync 纪律

- **I-FSYNC-1**: WAL 每行追加后立即 fsync。单行 < 4096 字节在 O_APPEND 模式下 + fsync 保证该行已持久化。
- **I-FSYNC-2**: DB 文件写入顺序：WAL → fsync WAL → 备份原文件 → fsync 备份 → 写 .tmp → fsync .tmp → rename .tmp → fsync 父目录。
- **I-FSYNC-3**: 文件 rename 后立即 fsync 父目录（确保目录元数据落盘）。
- **I-FSYNC-4**: `.lpkg_bak` 文件 rename 后立即 fsync 父目录。
- **I-FSYNC-5**: 所有 write_set_file/write_db_file 使用 .tmp + fsync + rename 模式。

> **I-FSYNC-1 与 I-FSYNC-2 的前两步（WAL 行 + fsync WAL）在任何模式下都成立**：
> WAL 行的**三条**写入路径全部无条件 fsync —— `wal::log_wal_line`、`WalWriter::log`、
> `wal_append_raw`（回滚审计行）。行的持久化是"行 = 一个已开始但可能未完成的操作"
> 这条不变量的前提，丢了行会出现"操作做了、行没了"这种不可恢复组合。`WalWriter` 曾把
> 自己的 fsync 挂在 `durable_fsync_enabled()` 上（默认关闭时静默不持久）—— 那是"实现
> 弱于契约"，已去掉：要非持久的快路径请**显式**用 `WalWriter::log_no_fsync()`。
>
> **WAL 文件自身的目录项 fsync 同样是恒生效的，但只在"本次真的创建了文件"时做**：
> 三条路径都用 `wal::open_wal_append()`（`O_CREAT|O_EXCL` 原子判出是否新建）取得 fd，
> 只有新建时才 `fsync_parent_dir()` —— 目录项只有创建那一刻需要落盘，而 `log_wal_line`
> / `wal_append_raw` **每写一行**都走一次，恒做等于每条 WAL 行白付一次父目录 fsync
> （实测占全部 fsync 的 34%~40%）。`WalWriter` 构造每批只走一次，保持原样（无条件）。
>
> **DB/元数据写（I-FSYNC-2 的 .tmp → fsync → rename → fsync 父目录、I-FSYNC-5）同样
> 始终成立**：`Cache` 的四个 DB 写函数与 `wal::write_string_file_wal` 都带
> `DurableFsyncGuard`（`base/utils.hpp`）。理由是不可恢复窗口：备份
> `.lpkg_db_bak_before:*` 在批次提交后立刻被 `cleanup_db_backups()` 删掉，若新库只
> rename 到页缓存，断电就落在"库空/截断 + 唯一备份已删"上（`files.db`/`pkgs` 是单文件，
> 丢了就是全库所有权归零）。每里程碑只多约 5 次 fsync。（对照：libalpm 从不 fsync、
> local DB 原地 `fopen(w)` 覆盖、无 tmp+rename、无备份 —— 这本就是 lpkg 领先它的地方。）
>
> **受 `durable_fsync_enabled()` 开关（默认关闭，`lpkg --fsync` 打开）影响的只有
> "批量文件数据"**：包内容 / `.lpkgtmp` / `.lpkgnew` 的**内容 fsync**，以及这些文件
> rename 的**父目录 fsync**（原语层就是 `fsync_and_rename` 的 .tmp fsync 与
> `fsync_dir_internal`；`installation_task.cpp` 里 COPY 的 fsync 与文件 rename 的父目录
> fsync 属这一类）。默认模式保证的是：每个操作仍是**一次 rename 或一次顺序 write**，
> 进程被 kill / 崩溃不会看到半写的行/文件，因此 WAL 的不变量与回滚/恢复语义**不变**；
> 断电则可能丢掉**已安装文件的数据本身**（重装即可修复，不涉及状态不可恢复）。
> 代价对比：默认模式每个**文件**省下"内容 fsync + rename 父目录 fsync"（典型 2 次，
> 目录项共享时更少），`--fsync` 才付这笔；WAL 行与 DB 写的 fsync 两种模式都在付
> （200 文件安装：WAL 行本身就有 400+ 次）。

---

## 2. WAL 2.0 协议

### 2.1 日志行完整列表

| 分类 | 行前缀 | 含义 | fsync |
|------|--------|------|-------|
| **批次边界** | `BEGIN_PKGS` | 批次开始 | 写后 fsync |
| | `COMMIT_PKGS` | 批次完结（成功或回滚后） | 写后 fsync |
| **安装操作** | `BEGIN <pkg> <ver>` | 单包安装开始 | 写后 fsync |
| | `COMMIT <pkg> <ver>` | 单包安装提交 | 写后 fsync |
| | `ROLLBACK <pkg> <ver>` | 单包安装回滚 | 写后 fsync |
| | `END <pkg> <ver>` | 单包安装结束 | 写后 fsync |
| **文件操作** | `BACKUP <src> → <dst>` | 文件备份到 `.lpkg_bak` | 写后 fsync |
| | `NEW <path>` | 新文件（日志记录，非实际创建） | 写后 fsync |
| | `NEW_DIR <path>` | 新目录（日志记录） | 写后 fsync |
| | `COPY <src> → <dst>` | `.lpkgtmp` → 目标路径 | 写后 fsync |
| | `REMOVE_OLD <src> → <dst>` | 升级时旧版废弃文件移除 | 写后 fsync |
| | `SAVE_CONF <src> → <dst>` | 移除时配置文件改名保留（`dst = <src>.lpkgsave`，**不进 stash**） | 写后 fsync |
| **移除操作** | `RM_BEGIN <pkg> <ver>` | 移除开始 | 写后 fsync |
| | `RM_COMMIT <pkg> <ver>` | 移除提交 | 写后 fsync |
| | `RM_END <pkg> <ver>` | 移除结束 | 写后 fsync |
| | `CLEANUP <path>` | .bak 清理记录（不可回滚） | 写后 fsync |
| **DB 操作** | `DB <path> <pkg>:<state>` | DB 文件修改后状态 | 写前已备份 + fsync |
| | `DBNEW <path> <pkg>:<state>` | DB 文件新建后状态 | 写前已备份 + fsync |
| | `DBRM <path> <pkg>:<state>` | DB 文件删除后状态 | 写前已备份 + fsync |
| **回滚审计** | `RESTORE_FILE <bak> → <orig>` | rollback：从 .bak 恢复文件 | 写后 fsync |
| | `RESTORE_DB <bak> → <db>` | rollback：从 .db_bak 恢复 DB | 写后 fsync |
| | `RESTORE_DIR <path>` | rollback：重建目录 | 写后 fsync |
| | `REMOVE_FILE <path>` | rollback：删除新文件 | 写后 fsync |
| | `REMOVE_DIR <path>` | rollback：删除新目录 | 写后 fsync |

> 表里的"写后 fsync"是**行内容**的 fsync（`::fsync(fd)`），**恒生效**、不受
> `--fsync`/`durable_fsync_enabled()` 影响（I-FSYNC-1）；三条写入路径
> （`wal::log_wal_line`、`WalWriter::log`、`wal_append_raw`）一视同仁，见 §1.6。
> **WAL 文件自身的目录项**（父目录 dentry）只在"本次调用**创建**了该文件"时才多一次
> `fsync_parent_dir()`（`wal::open_wal_append` 用 `O_CREAT|O_EXCL` 原子判定）—— 那也是
> 恒生效（套 `DurableFsyncGuard`），但**不是每行都做**：文件已存在时目录项早在别处落过
> 盘，每行再 fsync 一次父目录纯属白付（实测占全部 fsync 的 34%~40%）。

### 2.2 DB 状态标签

格式：`<pkg>:<state>`

| 标签 | 含义 |
|------|------|
| `:batch-start` | 批次开始前的状态（最干净的还原点） |
| `<pkg>:installed` | 包 `<pkg>` 安装完成后的状态 |
| `<pkg>:removed` | 包 `<pkg>` 移除完成后的状态 |

### 2.3 DB 备份文件命名

格式：原始路径 + `.lpkg_db_bak_before:` + 写入后里程碑

```
原始：/var/lib/lpkg/pkgs
备份：/var/lib/lpkg/pkgs.lpkg_db_bak_before:A:installed
```
^ 这个文件的内容是"安装 A 之前的 pkgs 内容"。

### 2.4 WAL 示例

**成功安装单个包**：
```
BEGIN_PKGS                        ← fsync
BEGIN curl 8.11.1                    ← fsync
BACKUP /usr/bin/curl → /usr/bin/curl.lpkg_bak_curl  ← fsync
NEW /usr/share/doc/curl/README      ← fsync
COPY /tmp/curl.lpkgtmp → /usr/bin/curl  ← fsync
COMMIT curl 8.11.1                   ← fsync
END curl 8.11.1                      ← fsync
DB /var/lib/lpkg/pkgs curl:installed  ← 备份后 + fsync
DB /var/lib/lpkg/files.db curl:installed  ← 备份后 + fsync
DB /var/lib/lpkg/confhashes.db curl:installed  ← 备份后 + fsync（见 §6.3）
COMMIT_PKGS                          ← fsync
```

**批量 [A, B] 中 B 失败 → 全批次回滚（含 RESTORE 审计）**：
```
BEGIN_PKGS                        ← fsync
BEGIN A 1.0                          ← fsync
BACKUP /usr/bin/a → /usr/bin/a.lpkg_bak_A  ← fsync
COPY /tmp/a → /usr/bin/a             ← fsync
COMMIT A 1.0                         ← fsync
END A 1.0                            ← fsync
DB /var/lib/lpkg/pkgs A:installed     ← 备份后 + fsync
BEGIN B 1.0                          ← fsync
NEW /usr/bin/b                       ← fsync
COPY /tmp/b → /usr/bin/b             ← fsync
ROLLBACK B 1.0                       ← fsync （B 内层 catch）
END B 1.0                            ← fsync
── 外层 catch：batch_rollback ──
RESTORE_DB /var/lib/lpkg/pkgs.lpkg_db_bak_before:A:installed → /var/lib/lpkg/pkgs  ← fsync
RESTORE_FILE /usr/bin/a.lpkg_bak_A → /usr/bin/a  ← fsync
REMOVE_FILE /usr/bin/b               ← fsync
DB /var/lib/lpkg/pkgs :batch-start    ← 备份后 + fsync
ROLLBACK A 1.0                        ← fsync
END A 1.0                             ← fsync
COMMIT_PKGS                           ← fsync
```

**关键点**: 回滚操作（RESTORE_DB、RESTORE_FILE、REMOVE_FILE）也在 WAL 中。如果回滚自身崩溃，重启后 `recover_packages()` 看到 RESTORE_DB 已执行（.bak 已被消费），跳过该步骤继续后续操作。

**移除失败回滚**：
```
BEGIN_PKGS                        ← fsync
RM_BEGIN curl 8.11.1                 ← fsync
BACKUP /usr/bin/curl → /usr/bin/curl.lpkg_bak_curl_a1b2c3   ← fsync
BACKUP /usr/share/man/man1/curl.1 → /usr/share/man/man1/curl.1.lpkg_bak_curl_d4e5f6  ← fsync
── 错误：磁盘满 ──
── catch：batch_rollback ──
RESTORE_FILE /usr/share/man/man1/curl.1.lpkg_bak_curl_d4e5f6 → /usr/share/man/man1/curl.1  ← fsync
RESTORE_FILE /usr/bin/curl.lpkg_bak_curl_a1b2c3 → /usr/bin/curl  ← fsync
DB /var/lib/lpkg/pkgs curl:installed  ← 恢复到安装状态
ROLLBACK curl 8.11.1                   ← fsync
END curl 8.11.1                        ← fsync
COMMIT_PKGS                           ← fsync
```

**升级失败回滚到旧版本**：
```
BEGIN_PKGS
BEGIN libfoo 2.0
BACKUP /usr/lib/libfoo.so.1 → /usr/lib/libfoo.so.1.lpkg_bak_libfoo
NEW /usr/lib/libfoo.so.2
COPY /tmp/libfoo.so.2 → /usr/lib/libfoo.so.2
COMMIT libfoo 2.0
END libfoo 2.0
DB /var/lib/lpkg/pkgs libfoo:installed
── libbar 安装失败 → 批次回滚 ──
RESTORE_DB /var/lib/lpkg/pkgs.lpkg_db_bak_before:libfoo:installed → /var/lib/lpkg/pkgs
RESTORE_FILE /usr/lib/libfoo.so.1.lpkg_bak_libfoo → /usr/lib/libfoo.so.1
REMOVE_FILE /usr/lib/libfoo.so.2
DB /var/lib/lpkg/pkgs :batch-start
ROLLBACK libfoo 2.0
END libfoo 2.0
COMMIT_PKGS
```

### 2.5 RESTORE 审计行在二次回滚中的作用

假设 rollback 中途崩溃：
```
BEGIN_PKGS
... (install A, B fails) ...
RESTORE_DB /var/lib/lpkg/pkgs.lpkg_db_bak_before:A:installed → /var/lib/lpkg/pkgs
── 断电 here ──
```

重启 `recover_packages()`：
1. 读 WAL，看到 `BEGIN_PKGS` 无 `COMMIT_PKGS` → 未完成事务
2. 逆序处理所有行（包括 RESTORE_DB）
3. 遇到 `RESTORE_DB /bakA → /pkgs`：
   - `/bakA` 已经被消费（被 rename 到 /pkgs），不存在
   - → 跳过（幂等：backup 已不存在说明 restore 已完成）
4. 遇到 `BACKUP /usr/bin/a → /usr/bin/a.lpkg_bak_A`：
   - `/usr/bin/a.lpkg_bak_A` 存在 → rename 回 `/usr/bin/a` → 恢复旧文件
5. 写 `COMMIT_PKGS`

结果是幂等的：无论 rollback 在哪个步骤崩溃，下次恢复能得到一致状态。

---

## 3. 原子操作顺序与 fsync

### 3.1 DB 文件写入（Cache::write）

这是最关键的操作顺序，每一步的 fsync 位置决定了断电安全性。

**目标**：修改 DB 文件（如 pkgs、files.db、confhashes.db）

**最终决定（Write-Ahead 优先 + 恢复时 Fallback）**：

```
 1. WAL：写入 DB <path> <milestone>
 2. fsync：WAL
 3. 备份：rename old → .lpkg_db_bak_before:<milestone>
 4. fsync：备份父目录
 5. 写 .tmp：写出新内容到 <path>.tmp
 6. fsync：.tmp
 7. 提交：rename .tmp → <path>
 8. fsync：<path> 父目录
```

**断电分析**：

| 断电点 | 系统状态 | 恢复行为 |
|--------|---------|---------|
| 1-2 之间 | WAL 行未持久化，原文件未改 | `recover_packages()` 无 DB 条目→跳过。**安全** |
| 2-3 之间 | WAL 持久化，未备份，原文件完好 | `reverse_execute` 找到 DB 条目但 `.lpkg_db_bak_before:<milestone>` 不存在→跳过（原文件还在，是离目标最近的可用状态） |
| 3-4 之间 | WAL 已记录，备份已 rename（可能未 fsync） | 备份存在→rename 回→恢复旧内容 |
| 4-5 之间 | WAL + 持久化备份，.tmp 不存在 | restore 从备份恢复 |
| 5-6 之间 | WAL + 备份 + .tmp 可能不完整 | restore 从备份恢复（.tmp 不完整不影响） |
| 6-7 之间 | WAL + 备份 + 完整 .tmp | restore 从备份恢复 |
| 7-8 之间 | WAL + 备份 + 新文件已到位 | restore 从备份恢复 |

**恢复规则**：
- `DB` 条目：`.lpkg_db_bak_before:<milestone>` 存在→rename 回。不存在→跳过（备份未完成，原文件还在）。
- `DBNEW` 条目：备份不存在→文件是新建的，删除。备份存在→rename 回。
- `DBRM` 条目：备份不存在→跳过。备份存在→rename 回。

**附注：cleanup_db_backups()**  
新架构需要一个 `cleanup_db_backups()` 函数来清理孤立的 `.lpkg_db_bak_before:*` 文件（比如在步骤 3 完成但 WAL 步骤 1 未持久化时产生的孤备份）。该函数在 `COMMIT_PKGS` 后调用，与旧版同名函数功能相似但实现需要重写——旧实现已被删除。

### 3.2 文件 BACKUP 操作（install/remove 备份）

```
 1. rename file → file.lpkg_bak_<pkg>
 2. fsync：file.lpkg_bak_<pkg> 父目录
 3. WAL：写入 BACKUP file → file.lpkg_bak_<pkg>
 4. fsync：WAL
```

**断电分析**：
- 1-2 之间：文件已 rename，WAL 未写。恢复时 BACKUP 条目不存在，逆向引擎不处理。但文件已从原位消失。→ `recover_packages()` 不会恢复该文件。**问题**：操作行在 BEGIN_PKGS 和 EOF 之间，被积累。但没有 BACKUP 条目，恢复引擎不知道这个文件存在。
  
  实际上，如果断电在 1-2 之间且文件被 rename 到了 .lpkg_bak，但没有 WAL 条目：`reverse_execute` 不会还原该文件（没有 BACKUP 条目可消费）。文件系统层面，该文件在 bak 中但不会被恢复。

  这个窗口非常小（rename 是原子的）。rename 完成后即使不 fsync，只要内核不丢脏页，文件就存在。但断电可能丢。

  实际上有个更好的顺序：先写 WAL，再做 rename：

```
 1. WAL：写入 BACKUP file → file.lpkg_bak_<pkg>
 2. fsync：WAL
 3. rename file → file.lpkg_bak_<pkg>
 4. fsync：file.lpkg_bak_<pkg> 父目录
```

这样 WAL 先承诺了备份，rename 再执行。崩溃在 1-2：WAL 行未持久化，文件未改。崩溃在 2-3：WAL 有记录，但文件未 rename。`reverse_execute` 遇到 BACKUP 条目，检查 `op.arg2`（.lpkg_bak）是否存在。不存在→跳过。**安全**（原文件还在原位）。
崩溃在 3-4：WAL 有记录，文件已 rename。恢复时遇到 BACKUP 条目，检查 `.lpkg_bak` 存在 → rename 回。**安全**。

**结论**：文件操作也用 write-ahead 顺序——WAL 先于实际操作。

### 3.3 COPY 操作（install 复制文件）

```
 1. copy src → dst.lpkgtmp
 2. fsync：dst.lpkgtmp
 3. WAL：写入 COPY src → dst
 4. fsync：WAL
 5. rename dst.lpkgtmp → dst
 6. fsync：dst 父目录
```

**断电分析**：
- 1-2 之间：.lpkgtmp 可能不完整。WAL 无记录。重新执行即可。
- 2-3 之间：完整 .lpkgtmp，WAL 无记录。恢复时遇到 COPY 条目吗？没有。.lpkgtmp 残留由下次安装清理（或 `cleanup_db_backups()` 见附注）。
- 3-5 之间：WAL 有记录，.lpkgtmp 存在。恢复时遇到 COPY 条目：检查 `op.arg2`（dst）是否存在？dst 不存在（未 rename）。检查 .lpkgtmp 也存在。→ 再次 rename .lpkgtmp → dst。**注意**：恢复引擎不应该再次 rename，它应该只清理。实际上 reverse_execute 是回滚（undo），不是重做（redo）。所以 COPY 的反向是"删除 dst"，而不是"重做 rename"。
  
  那么恢复时对于 COPY 条目应该做什么？`reverse_execute` 反向处理 COPY：删除 `op.arg2`（dst 文件）。但如果 dst 不存在（未 rename），删除什么也不做。**安全**。

等等——`recover_packages()` 执行的是逆向执行（undo），不是前向恢复（redo）。所以对于 COPY，它应该删除目标文件（撤销复制），而不是重新复制。但如果断电在 rename 之前，目标文件不存在，删除无操作。**正确**。

### 3.4 NEW 操作（install 新文件）

NEW 只是一个 WAL 日志记录，文件实际由 COPY 创建。所以 NEW 的逆向非常简单：
```
reverse_execute 遇到 NEW：
  → 删除 op.arg1（文件路径）
  → 如果存在则删除，不存在跳过
```

NEW 的 WAL 写入时机：在备份阶段，当检测到目标路径不存在时写入。早于 COPY。

### 3.5 NEW_DIR 操作

同样只是日志记录。逆向：
```
reverse_execute 遇到 NEW_DIR：
  → 删除目录（仅当为空时）
```

### 3.6 目录 BACKUP（取代 RM_DIR）

> ⚠ **本节下方描述的是历史形态**（逐目录整树 `rename` 成单个 `.lpkg_bak`，函数名
> `detail::backup_dir_tree_whole`）—— 该函数**已不存在**。现行实现是：
> **逐文件 ADVANCE 进 stash（`BACKUP`/`REMOVE_OLD`，可回滚）+ 目录在文件搬空后按
> "最深优先 / 仅最后持有者 / 必须已空"`DIR_RM`（`rmdir` + 元数据记录）**，
> 判据见 `main/src/pkg/package_manager.cpp` 的阶段 A/B 与
> `installation_task.cpp` 的废弃目录阶段。按本节旧描述去改代码会重新引入
> "整树 rename 掉共享祖先目录"的老缺陷。完整不变量见 §3.6.1。

#### 3.6.1 目录与符号链接的判定（对齐 pacman，2026-09 起）

事故背景：发行版布局 `/var/run -> ../run`，某包归档里带实体 `var/run/` 目录条目，
reinstall 后符号链接被 rename 进 stash、再建出实体目录（系统布局被毁）。

采用的判据（逐条对应 libalpm 的实现与回归测试）：

1. **判"盘上是什么"一律 `lstat`（不跟随末段），且必须先剥尾斜杠**
   （`strip_trailing_slash()`，`base/utils.hpp`）。pacman 侧这是**两件事**，别混：
   `llstat()` 在 `src/common/util-common.c`（尾斜杠剥离 + lstat，剥离来自 commit
   `bbeced26`，2014）；而 **FS#51377 修的是 `remove.c` 的 `unlink_file` 在判类型前自己
   剥尾斜杠**（commit `0fd8455c` / cherry-pick `16b91f79`，2017-04-09，随附测试
   `remove-directory-replaced-with-symlink.py`）。原因：尾斜杠会把末尾的符号链接
   **解引用**，`is_symlink`/`is_empty`/`rmdir` 全部落到链接**目标**上 —— 实测会把
   `/var/run` 指向的真实 `/run` 删掉，回滚还会 chmod 穿过去。DB 的目录键保持带斜杠
   （那是键，不是路径），**任何物理操作前先规范化**。
2. **symlink 一律算「非目录」**。"We do not support treating symlinks to directories as
   directories. They are considered a file."（pacman-dev）→ 不写穿链接、不把链接当目录。
3. **类型变更 = 冲突**（`collect_content_conflicts`，逐包检查与整批预检共用）：归档目录 `x/` 撞上盘上非目录
   （普通文件 / 符号链接，含 symlink→目录）→ 拒绝；归档文件 `x` 撞上盘上真目录 → 拒绝
   （pacman 的 case 4 / case 5，"not overwriting dir with file"）。整批中止、什么都不改。
   **覆盖豁免是逐路径的**（pacman `--overwrite <glob>[,<glob>…]`，`Config::overwrite_allows`）：
   只有**命中模式**的路径才豁免，且只豁免前一种（"symlink/文件挡路"，pacman 的闸门
   `!S_ISDIR(lstat)`）；**归档文件撞真目录永不覆盖** —— pacman 文档明说 `--overwrite`
   不允许 dir→file。模式列表语义同 pacman `_alpm_fnmatch_patterns`：可重复、逗号分隔、
   **倒序判定**（后写的先判、先命中者赢）、`!` 前缀 = 命中即**明确不豁免**、空列表 =
   什么都不豁免；`--force-overwrite` 保留，等价于列表最前面的一条 `'*'`（最宽松的兜底，
   仍可被后写的 `--overwrite` 覆盖）。所有权接管（`drop_owners`）**只在该路径被豁免时**
   发生 —— 不是"开了开关就全局放行"；接管同时摘掉旧持有者在这条路径上的哈希记录
   （归属与记录是同一条声明，见 §6.3「记录随包走」）。
   **检查时机**：`install_packages` / `upgrade_packages` 在**进入事务之前**先跑整批预检
   （`check_batch_file_conflicts`），把整批的 content 清单 + 所有权 + 接管顺序一起算，
   判冲突即在**一个文件都没动**时中止（WAL 里没有 BEGIN_PKGS）—— 与上游 libalpm
   （`alpm_trans_commit` 先 `_alpm_sync_check` 整笔 `trans->add`，"要么全不动要么全动"）
   同构；批次循环内逐包的 `check_for_file_conflicts` 保留为**第二道防线**（预检算漏的、
   预检之后中途状态变了的）。
4. **唯一豁免**：该路径由**本包**以另一形态持有（旧版发文件/链接、新版发目录）→
   文件→目录升级（E4）照旧接管；对应 pacman 的 "Check if the directory was a file in dbpkg"。
5. **删除侧**：symlink 一律 `unlink`、绝不 `rmdir`、绝不跟随（pacman commit `b1e495b8`）；
   目录要删必须"文件已搬空 + 目录为空 + 无其他 owner"；DB 记文件键而盘上已是目录 →
   **拒绝整批**（`--force` 才继续，且仍只跳过、绝不搬目录）。
6. **中间段是 symlink**（归档只带 `lib64/foo`、盘上 `lib64 -> usr/lib`）：lpkg **保留链接、
   内容穿过链接写入真实目录**。这是与 libarchive 的有意差异 —— pacman 的落盘层
   （`ARCHIVE_EXTRACT_UNLINK|SECURE_SYMLINKS`）会 `unlinkat` 掉挡路的中间段 symlink，
   FS#51377 的一类投诉正由此而来；lpkg 自己写解包器，选更保守的一侧。
7. **备份目录名被 symlink 占住**（`.lpkg_bak_<pkg>_<pid>`）：直接报错，绝不写穿
   （否则备份落到链接目标里，清理侧永远看不见）。
8. **执行点唯一：`detail::OpSink`**（`main/src/pkg/op_sink.hpp`）。上面第 1 条"任何物理
   操作前先规范化"能成为不变量，靠的是"WAL 行 + 物理操作 + stash 记账"由**同一个方法
   调用**完成，而不是靠每个调用点各自记得剥尾斜杠：曾经写行与做操作分散在不同文件、
   各自再算一遍路径，于是同一类缺陷只修了一半（`INSTALL` 侧剥了、`DIR_RM` 侧没剥）。
   **文件系统**（stash / rename / rmdir）操作只有这几个入口：
   `backup`/`backup_obsolete`（`BACKUP`/`REMOVE_OLD` → rename 进 stash）、
   `new_file`/`new_dir`（纯 WAL 记录，不碰文件系统）、`remove_empty_dir`（`DIR_RM` →
   `rmdir`）、`commit_copy`（`COPY` → rename）。**新增"文件操作 + 它的 WAL 行"一律加在这里**，
   不要回到调用点写成对语句。
   （DB 一族 —— `DB`/`DBNEW`/`DBRM` 的 row + `safe_rename` —— 是**另一个**已文档化的入口点，
   分布在 `db/wal_op.cpp`、`db/cache.cpp` 与 `package_manager.cpp` 的 `cleanup_with_dbr`：
   它们没有 stash 记账、落点是与目标同级的 `.lpkg_db_bak_before:<milestone>` 兄弟名，
   形状与本层不同。要收拢它们得单开一层，别顺手塞进 `OpSink`。）

目录不再使用 `RM_DIR` 记录元数据，而是与文件相同的 `BACKUP` 机制——rename 整个目录为 `.lpkg_bak_<pkg>_<rand>`，通过 WAL 原子化记录目录的移除。

```
WAL: BACKUP /usr/share/doc/foo/ → /usr/share/doc/foo.lpkg_bak_foo_a1b2c3
  含义：目录被 rename 到 .lpkg_bak，回滚时 rename 回原位即可。
```

`reverse_execute` 遇到目录 BACKUP 与文件 BACKUP 走同一代码路径：
```
  → 检查 .lpkg_bak 是否存在
  → 存在则 rename 回原路径（目录及其所有内容一并恢复）
  → 不存在则跳过（幂等）
```

**目录 BACKUP 的安全检查（remove 与 upgrade 共用同一实现
`detail::backup_dir_tree_whole`，见 `pkg/install_common.{hpp,cpp}`）**：
rename 整个目录之前，确认目录此刻已是"**纯本包残留**"——其**每个直接子项**的文件名
都带本包的 `.lpkg_bak_<pkg>_` 标记（本包废弃文件 rename 成的 bak，或更深、已先被
整树 rename 的本包目录 bak）。**任何非本包残留**的直接子项都会让整棵树保留：
  - 其他包的文件、保留的 conffile（移除时改名成 `<路径>.lpkgsave`，见 §7.2）
  - lpkg 自身的状态目录（`usr/share/lpkg/docs` 等，非包所有但绝不可删）
  - 任何**无主**文件/目录（不属于任何包的东西一律不删）
**嵌套层数靠调用方"最深优先"处理获得**：子目录先被整树 rename 成 bak，父目录的
直接子项此时就只剩 bak，检查通过即可整树删——**不要**递归下探去删"无主/空壳"内容。
（曾误用"子树里没有其他包登记内容即可删"的递归规则：会把共享祖先下、包只作普通
目录持有者的 `usr/share/lpkg/docs` 等 lpkg 自身状态目录一起删掉，`make test` 多处
回归。宁可在叶目录里留一个无主空壳，也绝不碰不属于本包的东西。）
若遇到"单层 `is_empty` + 有内容就跳过"的目录删除点，立刻改成**最深优先逐目录整树
`rename` 到单个 `.lpkg_bak`**——那是 remove 的既有算法（`do_remove_package`）。它之
前删不掉**升级**遗留的嵌套 owned 目录，是因为升级旧文件移除那段把目录删除混进了
文件循环 + 单层判空（新目录的文件先 rename 成 bak 占着目录，判"非空"被跳过），
**不是检查本身错**。

**升级（旧版本废弃文件移除）的目录删除 = remove 的同一算法（最深的统一）**：
升级时新版不再包含的旧文件（含符号链接）先逐个 rename 成 `.lpkg_bak`（`REMOVE_OLD`
WAL），随后旧目录走**独立阶段、最深优先**，逐目录调同一
`detail::backup_dir_tree_whole(dir, pkg, "REMOVE_OLD", backups)` 整树 rename 成单个
`.lpkg_bak`（WAL `REMOVE_OLD`，reverse_execute 与 BACKUP 同路径恢复）。嵌套第 2 层
owned 目录（如 `dist-info/licenses/`）因子目录先被 rename 成 bak、父目录直接子项只剩
bak 而被逐层清光，不再残留空壳。

### 3.7 DBRM 操作

```
 1. WAL：写入 DBRM <path> <milestone>
 2. fsync：WAL
 3. rename file → file.lpkg_db_bak_before:<milestone>
 4. fsync：父目录
```

**断电分析**：
- 1-2 之间：WAL 行未持久化。文件未改。
- 2-3 之间：WAL 有记录，文件未 rename。`reverse_execute` 找 `.lpkg_db_bak_before:<milestone>` 不存在→跳过。DBRM 的逆向是 restore 备份回原位。但备份不存在。→ 安全，文件还在原位。
- 3-4 之间：WAL 有记录，文件已 rename。恢复时找备份存在 → rename 回。**正确**。


## 4. DB 状态管理（确定性回滚顺序）

### 4.1 问题

批量多包时，DB 的每次写入必须标注"这次写发生在哪个包的安装/移除之后"，否则回滚无法确定每个 `.lpkg_db_bak_before:*` 备份对应什么系统状态，也就无法确定性地链式恢复到批次开始状态。

### 4.2 DbMilestone 类型

```cpp
struct DbMilestone {
    std::string pkg;   // 包名，":batch-start" 时 pkg="" 
    std::string state; // "installed" | "removed" | "batch-start"

    std::string to_string() const {
        if (pkg.empty()) return ":" + state;
        return pkg + ":" + state;
    }

    static DbMilestone from_string(const std::string &s) {
        auto colon = s.find(':');
        if (colon == std::string::npos) return {"", s};
        if (colon == 0) return {"", s.substr(1)};
        return {s.substr(0, colon), s.substr(colon + 1)};
    }
};
```

### 4.3 写入规则

| 场景 | DB 写入点 | 里程碑 |
|------|----------|--------|
| 批次开始 | `Cache::write(":batch-start")` | 保存"所有包都还没装"的状态 |
| 安装完成包 A | `Cache::write("A:installed")` | "A 已装好"的状态 |
| 移除完成包 A | `Cache::write("A:removed")` | "A 已移除"的状态 |
| 回滚后 | `Cache::write(":batch-start")` | 回到初始状态 |

### 4.4 链式恢复

```
WAL 中的 DB 条目（安装 [A, B, C]，B 失败）：
  DB /pkgs A:installed    ← 备份保存了 "batch-start" 的内容
  DB /pkgs B:installed    ← 不存在（B 安装失败，没到 DB 写入）

回滚时 reverse_execute 逆序：
  1. 遇到 DB /pkgs A:installed
  2. 找 .lpkg_db_bak_before:A:installed
  3. rename 回 /pkgs
  4. DB 恢复为 :batch-start 的内容
  5. 继续逆序（BACKUP、NEW 等文件恢复）
```

更长的链：
```
  DB /pkgs A:installed    .bak = :batch-start 内容
  DB /pkgs B:installed    .bak = A:installed 内容
  DB /pkgs C:installed    .bak = B:installed 内容

回滚到 :batch-start：
  逆序：
    1. DB /pkgs C:installed → .bak_before:C:installed → /pkgs  (→ B:installed 状态)
    2. DB /pkgs B:installed → .bak_before:B:installed → /pkgs  (→ A:installed 状态)
    3. DB /pkgs A:installed → .bak_before:A:installed → /pkgs  (→ :batch-start 状态)
    4. BACKUP、NEW 等恢复
```
### 4.5 关键：DB 恢复后必须重载 Cache

```
rollback 时 reverse_execute 恢复了磁盘 DB 文件（从 .lpkg_db_bak rename 回），
但 Cache 单例（内存）仍然持有 rollback 前的状态。此时：

  内存 Cache 说："文件 X 属于包 B"（因为该路径被 --overwrite 豁免、在内存接管了所有权）
  磁盘 DB 说：  "文件 X 属于包 A"（因为恢复到 :batch-start 的状态）

这就是 OWNER_OVERRIDE 试图解决的 bug——但它用错了方法。
OWNER_OVERRIDE 想在 WAL 中逐条记录"所有权被改了"，回滚时逐条恢复。
但 DB 已经整文件恢复了，所有权自然也跟着 DB 回来了，
只需要让内存重新从磁盘读即可。

正确做法：
  reverse_execute 恢复 DB 文件后 → Cache::instance().load()

这样内存 Cache 从磁盘重新读取 DB 文件内容，与磁盘一致。
不需要 OWNER_OVERRIDE 的逐条追踪，不需要额外 WAL 行。

在 batch_rollback 中：
  1. reverse_execute(ops)      ← 恢复文件 + 恢复 DB 文件
  2. Cache::instance().load()  ← 重载内存 Cache，与磁盘一致
  3. DB /pkgs :batch-start     ← WAL 写入恢复后的状态
  4. ROLLBACK/END/COMMIT_PKGS

--overwrite 豁免的路径直接在内存接管所有权，批次成功则随 DB 写盘持久化，
批次失败则 DB 恢复 + Cache 重载，所有权自动回到旧值，干净彻底。
```

---

## 5. 批量事务模型

### 5.1 统一事务函数

```cpp
/**
 * 统一批量事务执行器。
 *
 * 事务协议（重点：COMMIT_PKGS 唯一标记批次完结，不论成功还是回滚）：
 *
 *   正向路径：
 *     BEGIN_PKGS → execute() → COMMIT_PKGS
 *     ├── Cache::write(":batch-start")    ← 批次开始快照
 *     ├── for each pkg:                   ← 逐包执行
 *     │     Cache::write(pkg + ":installed")
 *     ├── COMMIT_PKGS                     ← 批次完结标记
 *
 *   异常路径（catch）：
 *     execute() 抛异常
 *     ├── batch_rollback(success)         ← 回滚所有已成功包
 *     │     ├── reverse_execute(ops)      ← 逆向执行
 *     │     ├── 写 RESTORE_* 审计行       ← 每步 fsync
 *     │     ├── 写 ROLLBACK/END           ← 包级回滚标记
 *     │     ├── DB 恢复到 :batch-start    ← 链式恢复
 *     │     └── 写 COMMIT_PKGS            ← 批次完结（回滚完成）
 *     └── rethrow
 */
template<typename OpT>
std::vector<std::string> run_batch_transaction(size_t total, OpT&& op);
```

### 5.2 不变量

- 进入 `run_batch_transaction` 前：WAL 已 `trim_completed`，无未完成事务。
- `BEGIN_PKGS` 写入 + fsync 后：异常路径保证 `COMMIT_PKGS` 一定被写入（catch 补写）。
- `COMMIT_PKGS` 是批次完结的唯一标记——不区分"成功完结"和"回滚完结"。外部只看有无 COMMIT_PKGS。
- 回滚后：WAL 包含完整的 RESTORE 审计链，系统状态一致。

### 5.3 回滚触发条件

| 触发条件 | 回滚范围 | 路径 |
|---------|---------|------|
| 安装包中途失败 | 整个批次 | `InstallationTask::run()` 的 catch → 写 ROLLBACK/END 标记 → `batch_rollback()`（单撤销路径，§6.5） |
| 整批中后续包失败 | 前序已成功包 | `run_batch_transaction` 的 catch → `batch_rollback()` |
| Ctrl+C | 整个批次 | 检查点抛异常 → 同异常路径 |
| 致命错误 | 整个批次 | 同异常路径 |
| 断电 | 整个批次 | 下次启动 `recover_packages()` |

---

## 6. 安装流程

### 6.1 顶层流程

```
install_packages(args)
│
├── recover_packages()                  ← 先处理 WAL 残留
├── trim_completed()
├── Cache::load()
├── TmpDirManager + Repo::load_index()
├── 解析参数 → targets
├── 一致性重试循环
├── 用户确认
│
├── check_batch_file_conflicts(plan, order)  ← **整批文件冲突预检**：进入事务**之前**
│                                              把所有成员的 content 清单 + 当前所有权 +
│                                              本批次内的接管顺序一起算；判冲突即抛错中止
│                                              （一个文件都没动，WAL 里连 BEGIN_PKGS 都没有）
│                                              判定与逐包检查共用同一份语义（installation_task.cpp
│                                              的 collect_content_conflicts）
├── run_batch_transaction( [&] {
│   │
│   ├── Cache::write(":batch-start")    ← WAL: DB /pkgs :batch-start (备份批次开始状态)
│   │                                    ← fsync WAL, fsync 备份
│   │
│   ├── for each pkg in order:
│   │     task.run(&ctx)                ← 包内 prepare() 的 check_for_file_conflicts
│   │                                     **第二道防线**（预检算漏的/中途状态变了的）
│   │     Cache::write(pkg + ":installed")  ← 每包完成后 DB 里程碑
│   │     success.push_back(pkg)
│   │
│   ├── COMMIT_PKGS                     ← fsync
│   └── catch:
│       batch_rollback(success)
│         ├── extract WAL 行
│         ├── reverse_execute(ops)
│         │    每步: 操作 → fsync → RESTORE_* 审计 WAL → fsync
│         ├── DB /pkgs :batch-start
│         ├── ROLLBACK pkg + END pkg
│         └── COMMIT_PKGS
│})
│
├── TriggerManager::run_all()
└── 提交完成，清理备份（新架构的 cleanup_db_backups 在此调用）
```

### 6.2 包级安装

```
InstallationTask::run(ctx)
│
├── prepare()
│   ├── download_and_verify
│   ├── extract
│   ├── ensure_dependencies_satisfied
│   └── check_for_file_conflicts
│       └── 该路径被 --overwrite 豁免时：缓存直接改所有权
│           cache.remove_file_owner(path, old_owner)
│           （/etc 条目同时 cache.remove_conf_hash(path, old_owner)：
│            归属与哈希记录是同一条声明，见 §6.3「记录随包走」）
│
├── WAL: BEGIN <pkg> <ver>              ← fsync
│
├── backup_existing_files()
│   ├── for each target: detect new / backup / dir
│   ├── 新目录: WAL: NEW_DIR <path>     ← fsync
│   ├── 新文件: WAL: NEW <path>         ← fsync
│   └── 覆盖:   WAL: BACKUP <src> → <dst>  ← fsync
│                rename phys → bak
│                fsync 父目录
│
├── copy_package_files()
│   ├── for each file:
│   │     （/etc 条目先走三哈希分流，见 §6.3：
│   │       ① 静默换新版（先 BACKUP 进 stash）/ ② 保留用户文件 / ③ 新版落 .lpkgnew）
│   │     copy → dst.lpkgtmp
│   │     fsync dst.lpkgtmp
│   │     WAL: COPY <tmp> → <dst>       ← fsync
│   │     rename dst.lpkgtmp → dst
│   │     fsync 父目录
│   └── ┌─ 异常:
│         WAL: ROLLBACK <pkg> <ver>     ← fsync
│         rollback() (文件级: 恢复 .bak, 删新文件)
│           └─ 每步: RESTORE_* / REMOVE_* ← fsync
│         WAL: END <pkg> <ver>          ← fsync
│         throw
│
├── commit_without_file_ops()
│   ├── register_package()
│   │   ├── 写 deps 文件: WAL: DBNEW <dep_path> <pkg>:installed
│   │   ├── 写 needed_so 文件: WAL: DBNEW <nso_path> <pkg>:installed
│   │   ├── 写 man 文件: WAL: DBNEW <man_path> <pkg>:installed
│   │   │   (WAL → write_string_to_file: .tmp → fsync → rename → fsync 父目录，已实现)
│   │   └── 注册文件所有权 (add_file_owner，内存操作)
│   ├── 处理 REMOVE_OLD (升级时)
│   │   WAL: REMOVE_OLD <src> → <dst>  ← fsync
│   │   rename old_file → .lpkg_bak
│   │   fsync 父目录
│   └── install_hook_files()
│       hooks_dir/<pkg>/ 下的脚本落位：旧脚本 WAL: BACKUP → stash，
│       新脚本 .lpkgtmp → fsync → WAL: COPY → rename
│       （**不执行任何钩子**：执行时机只在批次提交后，见 §6.4）
│
├── WAL: COMMIT <pkg> <ver>             ← fsync
│   (注意：不移除 .bak！所有 .lpkg_bak 延迟到 COMMIT_PKGS 后统一清理，
│   确保批量回滚时可恢复每个已安装包的文件)
├── WAL: END <pkg> <ver>               ← fsync
│
└── return ✓
```

### 6.3 配置文件（/etc 条目）的三哈希分流

安装/升级一个 `/etc/` 条目时，pacman `add.c` 用**三份哈希**决定"用户改没改过这个配置文件"。
判据仍是**路径前缀** `/etc/`（`constants::DIR_ETC`，与移除侧 §7.2.1 同一判据）：

| 符号 | 含义 |
|------|------|
| `hash_local` | 盘上那份文件当前内容的 SHA256（符号链接 / 读不到 → **空**，= 无从判定） |
| `hash_orig`  | **上一次我们往这个包的这个路径里装进去的内容**的 SHA256（记在 `confhashes.db` 里） |
| `hash_pkg`   | 本次包里那个条目的内容 SHA256 |

分流（判定表只有一份：`classify_config_update()`，别在调用点再抄一遍）：

**有旧记录**（正常路径）：

| 条件（按序判） | 结论 | 盘面动作 |
|---|---|---|
| `hash_local == hash_orig` | ① 用户**没改过** | 静默换成新版（**不产生** `.lpkgnew`） |
| `hash_orig == hash_pkg` | ② 包本身没改这个配置 | **保留**用户文件，**连 `.lpkgnew` 都不产生** |
| 三者互异（含 `hash_local` 为空） | ③ | 新版落 `<路径>.lpkgnew` + 告警 |

**无旧记录**（老 DB：本特性之前装的包，`confhashes.db` 里没有这一条）→ **只算两份哈希**：

| 条件 | 结论 | 盘面动作 |
|---|---|---|
| `hash_local == hash_pkg`（盘上那份与新包那份逐字节相同） | ② | **保留**原文件（内容一致 = 没有"新东西"要审阅），不产生 `.lpkgnew` |
| 不一致 / `hash_local` 为空 | ③ | 新版落 `<路径>.lpkgnew` + 告警 |

两个分支**都**由调用点把 **`hash_pkg`（包内那份）**的哈希写进 DB（与正常路径同一个值，
**没有例外**），于是这条退化分支只可能进入一次：记录一旦建起来，下一次升级就回到上面的正常
分流（⚠ 但"记录"本身会被删，那时又会进入 —— 见下）。

> ⚠ 代价（有意为之、写在测试里，⑫ `DegenerateRecordNeverAuthorizesSilentOverwrite` /
> ⑬ `ReshippedConfigAfterDropDoesNotAuthorizeSilentOverwrite`）：盘上那份**不被追认**，
> 所以它与记录不相等时，包**每改一次配置**都会再落一份 `.lpkgnew`（吵，但绝不静默 ——
> 用户始终看得到差异）；用户把 `.lpkgnew` **合并进**盘上那份之后，盘上 == 记录，此后回到
> 正常分流（静默跟上）。
>
> 曾经的做法是"追认盘上那份"（`record = hash_local`）：换来"`.lpkgnew` 不会每次升级都刷"，
> 但等于宣布"盘上那份就是我们装的"，于是**下一次**升级满足 ① 把用户改过的配置**静默覆盖**
> —— 连 `.lpkgnew` 都不产生（用户连新版长什么样都看不到），正好踩中安全底线。而退化路径上
> 那份**往往正是用户自己的文件**（无主文件撞包内文件时 `--overwrite` 是唯一合法入口），
> 且"追认"**不是一次性的**：记录有三处会按设计被删除（升级丢弃 /etc 条目、`remove_conf_hash`、
> 移除包，见下方"记录随包走"），该路径重新归本包时追认被重新武装，下一次升级再静默覆盖一次。
> 判定表（`classify_config_update()`）不受本项影响：无记录时仍是"两份一致 → 保留原文件、
> 不一致 → 落 `.lpkgnew` + 告警"，**用户可见行为不变**。

**安全底线**：用户改过的配置**永远**不被静默覆盖。① 的前提是"盘上那份**逐字节等于**我们上次
装进去的内容"，而这件事只能由记录证明 —— 拿不到 `hash_orig` 时**不拿盘上那份去"猜"**，只按
上面那张"无旧记录"的表判（两份一致 → 保留；不一致 → `.lpkgnew`），**原文件一律不动**；
拿不到 `hash_local`（盘上是符号链接/读不到）时落到 ③。这条底线**没有例外**：退化路径同样
只声明"这个包的这个版本提供过什么"（见上方 ⚠）。

**记录写的永远是"包内内容"的哈希（`hash_pkg`），不是"盘上当时那份"的哈希**。走 ③ 时盘上
仍是**用户**的文件；若在那里把 `hash_local` 记成 `hash_orig`，**下一次**升级就会满足 ①、
"用户改过的配置"被静默换成新版 —— 正好踩中底线。记 `hash_pkg` 则永远只声明"这个包的这个
版本提供过什么"，于是用户文件在每次升级都继续被判为冲突，用户始终能拿到 `.lpkgnew` 去比对。

**存储形态**：`<state_dir>/confhashes.db`（`Config::conf_hashes_db()`），与 `files.db` 同形：

```
/etc/foo.conf	curl:9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08
```

键 = 逻辑路径（与 files.db 的键同形），取值 = `"<pkg>:<sha256>"`（一个路径理论上会被
`--overwrite` 接管给别的包，故用集合、且带包名 —— pacman 的 `hash_orig` 也只取自"**被升级的
那个包**"在本地 DB 里的记录）。

- **不并进 `files.db`**：那里的取值是**属主包名集合**，被 `add_file_owner`（单一属主检查）、
  `get_file_owners`、`*owners.begin()` 当属主集合直接读 —— 混入哈希串会污染所有权语义。
- **走 DB 一族的写接口**（`Cache::write(milestone)` → `write_db_file_wal`）：WAL 行 + 备份 +
  `.tmp` + rename + fsync（§3.1、§4.3 的里程碑与 pkgs/files.db 完全一致），因此批次回滚由
  `reverse_execute` 的 DB/DBNEW 分支自动还原到批次前。新增 DB 文件**不引入**任何新的 WAL 行
  类型、也不改备份策略。
- **与兄弟库同一口径地预建**（`init_filesystem()` 的 `ensure_file_exists`）：一族里要么都预建、
  要么都不建。不预建会同时打歪两件事 —— `write_db_file_wal` 对不存在的文件走 `DBNEW`、**不产**
  `:batch-start` 备份（"每里程碑一份备份"对这一个库不成立）；崩溃恢复的判据也分成"文件缺失"
  与"存在但空"两支（`wal_op.cpp` 的 `batch_start_db_still_in_place`），同一次崩溃的恢复行为
  不该取决于它是第几个加进来的库。
- **记录随包走**：包被移除（`remove_package_files`）、新版本不再提供该文件
  （`commit_without_file_ops` 的废弃 /etc 条目）、或**该路径被 `--overwrite` 接管**（归属在
  安装期就被 `ConflictView::drop_owners` 摘走，见 §6.2 包级安装流程）时删除。
  留着一条已不在册的包的记录 = 重新装回来的包会把"上一次装的哈希"当成旧记录，从而把一份它
  **并不拥有**的同名文件按"用户没改过"静默覆盖。与文件如何处置无关：`--purge-config`（真删）
  与 `.lpkgsave`（改名保留）都不影响它 —— 记录的内容从来不是"盘上有什么"，而是"我们装过什么"。
- **接管时删除记录，而不是"改名转手给新持有者"**：转手会让新持有者在**它自己这次安装**里读到
  一条它从未装过的 `hash_orig`（`copy_package_files` 先 `get_conf_hash` 再 `set_conf_hash`）——
  盘上那份若恰好等于那条外来记录，判定表就落到 ① **静默就地替换**（用户可见的东西被换掉而连
  `.lpkgnew` 都不产生）；而且它随即会被新持有者自己的 `set_conf_hash`（先删同包前缀）抹掉，
  转手是纯亏。两条用例钉住这对选择：`OverwriteTakeoverDropsPreviousOwnerHashRecord`（接管后
  移除旧持有者 → 记录消失）与 `TakeoverDoesNotInheritTheOldOwnersHashRecord`（盘上那份逐字节
  等于旧记录时也**不得** ① —— 必须保留原文件 + 落 `.lpkgnew`）。

**① 静默替换走写入层原语、全程可回滚**：它会**真的改盘**，而"覆盖已有配置"这条路径在本特性
之前**根本不存在**（`backup_existing_files()` 对 `/etc` 前缀是 `continue` = 不备份；
`copy_package_files()` 只在"目标已存在"时写 `.lpkgnew`）。新路径的 WAL 序列：

```
BACKUP /etc/foo → <stash>/foo.lpkg_bak_<pkg>_<rand>   ← OpSink::backup（write-ahead：行先落、再 rename）
COPY  /etc/foo.lpkgtmp → /etc/foo                    ← OpSink::commit_copy（同上）
```

只用 `OpSink` 两个原语，**没有裸 `fs::copy`/`fs::rename` 绕过写入层**（内容先写成 `.lpkgtmp`
→ fsync → 写 COPY 行 → rename；与 §3.2/§3.3 的既有序列一致）。回滚逆序：COPY 的逆操作删掉
目标（新内容）→ BACKUP 的逆操作把备份 rename 回原位 —— **整个 inode 回来**，属主/权限/xattr
一并还原。提交后备份随 stash 清理（用户没改过 → 没有要保留的内容；"保留"是移除侧
`.lpkgsave` 的语义，§7.2.1）。

**顺序与崩溃收敛**：同一事务内**先替换配置（BACKUP+COPY）、后写哈希记录**（记录由
`cache.write(<pkg>:installed)` 落盘 —— 在包的 `COMMIT <pkg>` 之后、批次 `COMMIT_PKGS` 之前，
见 §4.3）。于是**不存在**"替换了但记录没写"的持久态：

- 崩在"COPY 已做、`<pkg>:installed` 未写"→ WAL 里没有 `COMMIT_PKGS` → 批次回滚（或下次
  `recover_packages` 续做回滚）：配置与哈希 DB **一起**退回批次前；
- 崩在 `COMMIT_PKGS` 之后 → 批次已提交，新内容与新记录都已落盘（记录走 DB 一族写接口，
  `write_db_file_wal` 的 fsync 恒生效）。

（若哪天把记录挪到事务外/提交后，就会出现上面那种"半截态"—— 例如"替换了、记录没写"下次
升级会拿盘上的新内容当 bootstrap（自洽但少一次三哈希判定），"记录了、替换没做"会把用户
内容当'已修改'而反复留 `.lpkgnew`。当前排序刻意避开这两者。）

**过渡代价**：本特性之前装的包（老 DB 没有 `confhashes.db`）第一次升级走上面那张"无旧记录"
表 —— 两份一致时静默无事，不一致时多出一份 `.lpkgnew`。此后记录说的是**包内**那份（不是
盘上那份），所以盘上那份在被用户"合并"之前不会再前进：包每改一次配置都再落一份**可见的**
`.lpkgnew`。这是刻意选的一侧代价 —— 另一侧（追认盘上那份）会把用户文件当成我们装的，
下一次升级静默覆盖它（见上方 ⚠）。

### 6.4 批次提交后的收尾（hooks 与 postinst）

`run_batch_transaction` 返回后（`COMMIT_PKGS` 已落盘、批次不可能再回滚）由调用方执行
`finish_committed_batch()`：清理 stash（`CLEANUP` → remove_all）→ 删被移除包的
`hooks_dir/<pkg>/` → 剪枝新版本不再提供的 hook 文件 → **执行 postinst** → `trim_completed`
→ `cleanup_db_backups`。

- **postinst 只在提交之后执行，全仓唯一执行点**（原先在 `commit_without_file_ops()` 末尾 =
  批次内）：批次是"全或无"，回滚能撤销文件与 DB，却撤不回钩子副作用（钩子以 root 跑
  `systemd-sysusers` / `tmpfiles --create` / `useradd`）。上游 libalpm 同理：POST hook 整段在
  "提交 / 中断"判定之后，提交失败一个都不跑。
- **hook 脚本文件在事务内**：`install_hook_files()` 走写入层原语（BACKUP + COPY），因此回滚后
  `hooks_dir/<pkg>/` 里仍是**旧版本**的脚本内容。原先的 `fs::copy(overwrite_existing)` 不进
  WAL、覆盖即永久丢失 —— 回滚后包体是旧版本、钩子却是新版本，后续 remove/upgrade 跑错版本。
- 钩子执行失败只告警不抛（批次已提交、包确实装上了）。

### 6.5 包级回滚（InstallationTask::rollback）

**只写回滚标记，不做任何文件系统撤销**（单撤销路径，见下方说明）：

```
rollback()
│
├── WAL: ROLLBACK <pkg> <ver>          ← fsync（失败包的包级回滚标记，仅信息性）
├── WAL: END <pkg> <ver>               ← fsync
│
└── backups_.clear(); new_files_.clear(); new_dirs_.clear();
```

> **单撤销路径（2026-08-03 重构）**：所有正向操作的逆序执行统一由
> `batch_rollback` → `reverse_execute` 完成（所有 install/upgrade 都经
> `run_batch_transaction`，包级失败必然触发批次回滚）。rollback_files 曾在此处
> 恢复 `.lpkg_bak` 并写 RESTORE_* 审计，与 reverse_execute 形成**双重回滚**——
> rollback_files 恢复的旧文件被 reverse_execute 的 COPY 逆操作（无条件删除 dst）
> 再次删除，升级中途失败的包丢失旧文件。撤销职责收拢后该问题消失，且
> "rollback_files 删新文件失败"的残留也能由 reverse_execute 兜底重试。
> 失败包的 ROLLBACK 标记由 rollback_files 写；成功包的由 batch_rollback 统一写。

---

## 7. 移除流程

### 7.1 顶层流程

```
remove_package(pkg_name, force, wrap_in_txn, purge_config)
│
├── recover_packages() + trim_completed()  (if wrap_in_txn)
├── 版本检查 + 安全检查
│
├── run_batch_transaction(1, [&] {
│   do_remove_package(pkg, force, purge_config)
│ })
```

`force` 与 `purge_config` 是**两个正交维度**：`force`（CLI `--force`）只跳过安全检查
（反向依赖 / 共享文件 / 陈旧文件键），`purge_config`（CLI `--purge-config`）才决定配置文件
的真删。旧实现把"删配置"绑在 `force` 上、且 `remove -r` / autoremove / force-solve-conflict
内部硬编 `force=true` —— 用户不给 `--force` 也会丢配置。

### 7.2 核心移除逻辑

```
do_remove_package(pkg_name, force, purge_config)
│
├── SIGINT 检查
├── prerm hook（文件被删**之前**跑，故不能挪到提交后 —— 那等于静默变成 postrm）
│
├── WAL: RM_BEGIN <pkg> <ver>            ← fsync
│
├── 文件处置阶段：逐文件
│   for each owned_file:
│     SIGINT 检查
│     if 是配置文件（路径前缀 /etc/）且非 purge_config:
│       WAL: SAVE_CONF <phys> → <phys>.lpkgsave  ← fsync（write-ahead）
│       （目标已存在 → 先把旧的移位成 .lpkgsave.1/.2…，那也是同类型的一条行）
│       rename phys → <phys>.lpkgsave      ← 不进 stash：提交后不会被清掉
│     else:
│       WAL: BACKUP <phys> → <bak>          ← fsync（write-ahead）
│       rename phys → bak（带随机后缀防冲突）
│       fsync 父目录
│
├── remove_package_files()               ← 从 DB 移除文件记录
│                                          （/etc 条目同时清 confhashes.db 记录，见 §6.3）
│
├── 目录 BACKUP（取代旧 RM_DIR）
│   for each dir（最深优先，仅最后持有者）:
│     安全检查（§3.6，共享 detail::backup_dir_tree_whole）：目录须是"纯本包残留"
│     （每个直接子项都是本包 .lpkg_bak）；含任何非本包残留 → 整树保留
│     if 可删:
│       WAL: BACKUP <dir> → <dir.lpkg_bak>  ← fsync
│       rename 整目录 → .lpkg_bak
│       fsync 父目录
│
├── 清理 dep/needed_so/man/hooks
│   DBRM ...
│
├── DB 落盘：
│   DB /pkgs pkg:removed                  ← 备份后 + fsync
│   DB /files.db pkg:removed              ← 备份后 + fsync
│   DB /confhashes.db pkg:removed         ← 备份后 + fsync
│
├── WAL: RM_COMMIT <pkg> <ver>           ← fsync
│
└── WAL: RM_END <pkg> <ver>              ← fsync
```

> **CLEANUP 不在包级流程内**：stash（刚被删掉的文件）是回滚的唯一来源，必须活到批次提交之后。
> 清理由批次级的 `finish_committed_batch()` 在 `COMMIT_PKGS` 之后统一执行（见 §7.4）。
>
> **逐包安全检查（共享文件 / 陈旧文件键撞实体目录）不在包级流程内**：它们在
> `remove_packages_in_one_batch()` 里、**进入事务之前**对整批一次性跑完
> （`check_removal_preconditions()`）。原先逐包检查时，批次里前面的包已经跑过 prerm、
> 后面的包才被检查拒绝 —— 整批回滚撤得回文件与 DB，撤不回 prerm 的副作用。前移后拒绝的批次
> 根本不开启事务。prerm 之后仍可能失败的只剩 I/O 错误与 Ctrl+C（上游 libalpm 的 pre_remove
> 同样在事务内、删除之前跑，暴露面相同）。

### 7.2.1 配置文件：改名成 `<路径>.lpkgsave`（只有 `--purge-config` 才真删）

判据是**路径前缀** `/etc/`（`constants::DIR_ETC_PREFIX`），不是包的声明（LankeBUILD 没有
pacman 的 `backup=()` 那一维，本模型不变）。任何移除路径 —— `remove`（无论 `--force`）/
`remove -r` / `autoremove` / `force-solve-conflict` —— 碰到包内配置文件时：

- 默认把它 **rename 成 `<路径>.lpkgsave`**：既不原地留着（用户以为删干净了其实是残留），
  也不静默删除（那是 `--purge-config` 才有的语义）；
- `<路径>.lpkgsave` 已存在时**不覆盖**：先把旧的移位到第一个空闲的 `<路径>.lpkgsave.<N>`
  （N 从 1 起，pacman 的 `shift_pacsave`），移位本身也是一条可回滚的 WAL 行；
- 只有显式 `--purge-config` 才走与普通文件相同的 BACKUP/stash 路径**真删**。

实现要点（`detail::OpSink::save_config()` + `WALOpType::SAVE_CONF`）：

- **绝不走"搬进 stash"这条路**。stash 是批次提交后由 `cleanup_stashes()` 整目录
  `remove_all` 的 —— 把配置搬进 stash 就等于真删，那正是 `--purge-config` 的语义。
  `SAVE_CONF` 的 dst 是**原位旁边的兄弟名**，不进 stash 记账，提交后无人清理它。
- **可变现**：`SAVE_CONF <src> → <dst>` 先落盘再 rename（write-ahead）。逆操作与
  BACKUP/REMOVE_OLD 同形（`reverse_execute` 三型合一个分支：`rename(arg2 → arg1)`，
  dst 不存在即跳过，幂等）。恢复路径收敛为：
  - 崩在"行已落、rename 未做"→ 原文件仍在原位，回滚对 dst 的反向 rename 找不到 dst → no-op；
  - 崩在"rename 已做"→ 回滚把 `.lpkgsave` rename 回原位（内容完整）；
  - 崩在"移位已做、本次改名未做"→ 回滚先把本次改名的 no-op 跳过，再把 `.lpkgsave.<N>`
    rename 回 `.lpkgsave`。
  **移位必须严格先于本次改名**：否则回滚会把上一次的旧存档 rename 到配置原位，盖掉真配置。
- **`.lpkgsave` 不是待清理的 bak**：`trim_completed()` / `continue_post_commit_cleanup()`
  只认 BACKUP/REMOVE_OLD/CLEANUP 的行，不会把它当残留删掉（有集成用例盯着这一点）。

### 7.3 移除回滚

```
run_batch_transaction 的 catch:
  → batch_rollback(success)

batch_rollback 对移除的逆向：
  → 逆序处理 WAL 行（RM_COMMIT 前）：
    DB /pkgs pkg:removed  → 查找 .lpkg_db_bak_before:pkg:removed
       存在   → rename 回 /pkgs (→ pkg:installed)
       WAL: RESTORE_DB <bak> → <db>      ← fsync
    DBRM ...
    BACKUP <dir> → <dir.lpkg_bak>
       → rename .lpkg_bak 回原目录（含目录内所有文件）
       WAL: RESTORE_FILE <bak> → <dir>    ← fsync
    BACKUP <file> → <file.lpkg_bak>
       → rename .lpkg_bak 回原文件
       WAL: RESTORE_FILE <bak> → <file>   ← fsync
    RM_BEGIN  → 跳过
  → DB /pkgs :batch-start                ← 备份后 + fsync
  → ROLLBACK <pkg> <ver>                ← fsync
  → END <pkg> <ver>                      ← fsync
  → COMMIT_PKGS                          ← fsync
```

---

## 8. 升级流程

### 8.1 顶层流程

同 install 流程，使用 `run_batch_transaction`。区别：
- `InstallationTask` 设置了 `old_version_to_replace_`
- `commit_without_file_ops` 中处理 `REMOVE_OLD`

### 8.2 升级回滚

升级回滚从 `.lpkg_bak` 恢复旧版本文件 + DB 链式恢复：

```
WAL 内容:
  DB /pkgs libfoo:installed  (版本 2.0)

回滚:
  1. RESTORE_DB: .bak_before:libfoo:installed → /pkgs (→ 1.0 状态)
  2. REMOVE_FILE: /usr/lib/libfoo.so.2 (新增)
  3. RESTORE_FILE: .lpkg_bak_libfoo → /usr/lib/libfoo.so.1 (旧版本恢复)
  4. DB /pkgs :batch-start
  5. ROLLBACK libfoo 2.0 + END libfoo 2.0
  6. COMMIT_PKGS
```

---

## 9. 回滚引擎

### 9.1 核心 API

```cpp
namespace wal {

struct RollbackStats {
    int files_restored = 0;
    int files_cleaned = 0;
    int dirs_recreated = 0;
    int db_restored = 0;
};

/**
 * 逆向执行一组 WAL 操作。
 *
 * 对每条操作按类型执行逆向，每个操作后写入 RESTORE_* 审计行。
 * 跳过 RESTORE_x/REMOVE_x/元数据行；`:batch-start` DB 条目**只在正式文件仍在（仍持有批次
 * 起点内容）时**跳过，否则从该里程碑的备份还原 —— 判据见 `wal_op.cpp` 的
 * `batch_start_db_still_in_place()`，崩溃窗口的现场复现在
 * `tests/integration/test_db_batch_start_recovery.cpp`。
 *
 * @param ops              待逆向执行的操作（正向顺序）
 * @param write_audit      是否写 RESTORE WAL 审计行（正常=true，rec 时=true）
 * @return RollbackStats
 */
RollbackStats reverse_execute(
    const std::vector<WALOp> &ops,
    bool write_audit = true);

/**
 * 从 WAL 提取当前批次的操作行列表。
 * 从最后一个 BEGIN_PKGS 到文件末尾。
 */
std::vector<WALOp> extract_current_batch_ops();

/**
 * 完整的批次回滚。
 * 1. 提取当前批次 WAL 行
 * 2. 清理内存 cache
 * 3. reverse_execute(ops)
 * 4. DB /pkgs :batch-start
 * 5. ROLLBACK pkg + END pkg 对每个已回滚包
 * 6. COMMIT_PKGS
 */
void batch_rollback(const std::vector<std::string> &success);

} // namespace wal
```

### 9.2 reverse_execute 操作表

| 操作类型 | 逆向操作 | fsync | RESTORE 条目 | 备份不存在时 |
|---------|---------|-------|-----------|-------------|
| `BACKUP`（文件/目录） | rename .bak → 原位 | fsync 父目录 | `RESTORE_FILE <bak> → <orig>` | .bak 不存在→跳过（已被消费→幂等） |
| `REMOVE_OLD` | 同上（同 BACKUP） | 同上 | `RESTORE_FILE <bak> → <orig>` | 同上 |
| `COPY` | remove(dst) | 不需要 | `RESTORE_FILE_RM <dst>` | 不存在→跳过 |
| `NEW` | remove(path) | 不需要 | `RESTORE_FILE_RM <path>` | 不存在→跳过 |
| `NEW_DIR` | rmdir（仅空时） | 不需要 | `RESTORE_DIR_RM <path>` | 不存在→跳过 |
| `DIR_RM` | 需要则 `create_directories` 重建 + 按行内 mode/uid/gid 恢复元数据（路径**先剥尾斜杠**，见 §3.6.1 第 1 条；是 symlink 则只判存在、**不** chmod/chown） | 不需要 | `RESTORE_DIR <path>` | 已存在→跳过（幂等） |
| `DB` | .bak_before 存在→rename 回 | fsync 父目录 + WAL | `RESTORE_DB <bak> → <db>` | .bak 不存在→跳过（WAL 已写但备份未完成→原文件还在） |
| `DBNEW` | .bak 存在→rename 回; 不存在→删除文件 | fsync 父目录 + WAL | `RESTORE_DB` 或 `RESTORE_DB_RM` | - |
| `DBRM` | .bak 存在→rename 回 | fsync 父目录 + WAL | `RESTORE_DB <bak> → <db>` | .bak 不存在→跳过 |
| `CLEANUP` | **跳过（不可回滚）** | 不执行 | 不写入 | - |

---

## 10. RESTORE 审计日志与二次回滚

### 10.1 为什么需要 RESTORE 审计

如果 `batch_rollback()` 自身崩溃，系统处于"半回滚"状态。WAL 中有：
- 正向操作行（BEGIN、BACKUP、COPY 等）
- 部分逆向操作行（RESTORE_FILE、RESTORE_DB 等）
- 没有 COMMIT_PKGS

`recover_packages()` 启动后，收集所有行（包括 RESTORE 行）作为"事务内容"，然后 `reverse_execute` 全部。遇到 RESTORE 行时：
- `RESTORE_FILE <bak> → <orig>`：检查 `<bak>` 是否存在。如果不存在（已被 rename 到 `<orig>`），则此 restore 已完成，跳过。如果存在，则重复执行 restore（幂等）。
- `RESTORE_DB <bak> → <db>`：同上。
- `REMOVE_FILE <path>`：检查 `<path>` 是否存在。存在则删除，不存在则跳过。

### 10.2 二次回滚的场景

场景 A：rollback 在 RESTORE_DB 完成后、RESTORE_FILE 前崩溃
```
WAL 状态：
  BEGIN_PKGS
  ... install A, install B fails ...
  RESTORE_DB /bak → /pkgs     ← 已完成
  RESTORE_FILE /bak → /a      ← 未执行（崩溃在此）
  REMOVE_FILE /new/b
  ...

恢复：
  reverse_execute 逆序：
    REMOVE_FILE /new/b: 文件存在？A 的 copy 还在，所以 /new/b 存在→删除（安全）
    RESTORE_FILE /bak → /a: /bak 存在→rename 回 /a（恢复旧版本 A）
    RESTORE_DB /bak → /pkgs: /bak 不存在（已被第一次消费）→跳过（幂等）
  → 系统状态一致
```

场景 B：rollback 全部完成但 COMMIT_PKGS 未写（在 COMMIT_PKGS 写入前崩溃）
```
WAL 状态：
  BEGIN_PKGS
  ... install A, install B fails ...
  RESTORE_DB /bak → /pkgs
  RESTORE_FILE /bak → /a
  REMOVE_FILE /new/b
  ROLLBACK A 1.0
  END A 1.0
  DB /pkgs :batch-start
  （COMMIT_PKGS 未写，崩溃在此）

恢复：
  reverse_execute 尝试全部逆序：
    DB /pkgs :batch-start: .bak_before::batch-start 存在→rename 回→DB 又恢复成 A 已安装的状态！
    然后 ROLLBACK A 1.0: 只是元数据，跳过
    REMOVE_FILE /new/b: /new/b 不存在→跳过
    RESTORE_FILE /bak → /a: /bak 不存在→跳过
    RESTORE_DB /bak → /pkgs: /bak 不存在→跳过
    ...
  → DB 错误地恢复成了 A 已安装的状态！→ BUG！
```

**解决方案**：`reverse_execute` 在遇到 `DB /pkgs :batch-start` 时，**只在该正式文件仍在、
且仍持有批次起点内容时才**把它当"最终状态标记"、跳过；否则（文件不在 / 已空）从该里程碑的
备份还原。

这个问题的本质是：**`DB /pkgs :batch-start` 意味着"回滚后的最终 DB 状态"，不应该被进一步
逆向** —— 但这只在那个状态**确实还在盘上**时成立。崩溃可能正好落在 `write_db_file_wal` /
`write_set_file_wal` 的 rename 窗口里（正式名已消失、`.tmp` 还没 rename 回来），此时盘上
只剩那份 `:batch-start` 备份，无条件跳过等于它**永远无人消费**，两个后果都不可逆：
`pkgs`/`holdpkgs` 缺失让 `read_set_from_file` 抛异常、整个 `recover_packages()` 失败；
`files.db`/`provides.db`/`confhashes.db` 缺失被 `read_db_uncached` 静默当空表 → 归属归零，
而 `cleanup_db_backups()` 紧接着把唯一备份删掉。

判据**不是**裸 `fs::exists`：启动顺序是 `init_filesystem()`（main.cpp）→ `recover_packages()`，
前者会把窗口里消失的库按**空文件**重建，所以"存在但 0 字节"同样是内容丢了。真正的判据是
`wal_op.cpp` 的 `batch_start_db_still_in_place()`（存在**且**非空；文件空 + 备份空则等价、
跳过），现场复现与断言在 `tests/integration/test_db_batch_start_recovery.cpp`。

在 `reverse_execute` 中（`wal_op.cpp` 的实现形态）：
```cpp
if (is_batch_start_milestone(op) && batch_start_db_still_in_place(op)) continue;
```

同样地，`DBNEW` 和 `DBRM` 的 `:batch-start` 条目也走**同一条判据**（`is_batch_start_milestone`
对三型一视同仁）。

这样场景 B 的恢复顺序（场景 B 里正式文件一直在位 → 仍然跳过，语义一字未变）：
```
reverse_execute 逆序：
  DB /pkgs :batch-start   → 跳过（最终状态标记；判据是 batch_start_db_still_in_place(op)：文件仍在且非空）
  ROLLBACK A 1.0          → 跳过
  END A 1.0               → 跳过
  REMOVE_FILE /new/b      → 不存在，跳过
  RESTORE_FILE /bak → /a  → /bak 不存在，跳过（幂等）
  RESTORE_DB /bak → /pkgs → /bak 不存在，跳过（幂等）
  ...（正向操作逆序）...
  BACKUP /usr/bin/a → /usr/bin/a.lpkg_bak_A
    → /a.lpkg_bak_A 不存在（已被 RESTORE_FILE 消费）→ 跳过
  
  等等——如果 RESTORE_FILE 已经消费了备份，那反向 BACKUP 不应该撤销 RESTORE。
  但 reverse_execute 不区分"这个操作是正向还是RESTORE"。
```

**这就是根本问题：reverse_execute 对 RESTORE 行和正向操作行一视同仁地逆序处理。** 但 RESTORE 行的逆序不是"撤销 restore"（那等于重做正向操作），而是"撤销 undo"（那等于重做 redo）。

**解决方案**：`reverse_execute` 跳过 `RESTORE_*` 和 `REMOVE_*` 行（这些已经是逆向结果，不应该再被逆向）。

```cpp
// 在 reverse_execute 循环中：
if (op.type == "ROLLBACK" || op.type == "END" || 
    op.type == "COMMIT" || op.type == "BEGIN" ||
    op.type == "RM_BEGIN" || op.type == "RM_COMMIT" || 
    op.type == "RM_END" ||
    op.type.starts_with("RESTORE_") || op.type.starts_with("REMOVE_")) {
    // 元数据行/RESTORE 审计行——跳过逆向
    continue;
}
```

这样 `recover_packages()` 中的 `reverse_execute` 对场景 B：
```
逆序处理（跳过 RESTORE_* / REMOVE_* / ROLLBACK / END / BEGIN / COMMIT；DB `:batch-start`
          仅在正式文件仍在且非空时跳过，否则从该里程碑备份还原）：
  只处理正向操作：BACKUP、COPY、NEW、DB（非:batch-start，或 :batch-start 但文件已丢）等
  → 恢复系统到 :batch-start 状态
  → 第二次 COMMIT_PKGS
```

**结论**：`reverse_execute` 始终跳过 `RESTORE_*`、`REMOVE_*`、`ROLLBACK`、`END`、`COMMIT`、`BEGIN`、`RM_*`、`CLEANUP` 元数据/审计行。DB `:batch-start` 条目**例外**：只在正式文件仍在且非空时才跳过（`batch_start_db_still_in_place()`，`wal_op.cpp`），否则它的逆操作要从该里程碑备份把库还原回来 —— 那是"文件缺失/被清空"这个崩溃窗口的唯一还原依据（`test_db_batch_start_recovery.cpp` 逐条钉住缺失与"存在但 0 字节"两种现场）。其余正向操作行（BACKUP、COPY、NEW、NEW_DIR、DB/DBNEW/DBRM 非:batch-start）照常逆向。

### 10.3 关于二次回滚的 RESTORE 行写入策略

`batch_rollback()` 在 reverse_execute **过程中**写入 RESTORE_* 审计行：
- 每完成一个逆向操作 → 立即 WAL: RESTORE_* + fsync
- 这样 WAL 中 RESTORE 行的顺序记录了 rollback 的进度
- rollback 崩溃后 restart 的 `recover_packages()` 中的 `reverse_execute` 跳过 RESTORE 行
- RESTORE 行的存在/不存在帮助判断哪些逆向操作已完成（通过检查 .bak 是否被消费）

---

## 11. rec 恢复（Fallback Only）

### 11.1 定位

`recover_packages()` 是紧急恢复工具，只在进程已死亡（catch 未执行）的场景下使用。

### 11.2 状态机

```cpp
void recover_packages() {
    // 1. 读取 WAL 到 lines[]
    // 2. 状态机扫描：
    //    BEGIN_PKGS → in_txn=true, ops 开始积累
    //    COMMIT_PKGS → in_txn=false, 清空 ops
    //    EOF + in_txn=true → uncommitted_txns.push(ops)
    // 3. 对每个未完成事务：
    //    a) parse_op 解析为 WALOp 列表
    //    b) reverse_execute(ops, true)——未提交批次**一律回滚**：
    //       CLEANUP 只可能出现在事务之外（post-commit 收尾记录），
    //       事务内不存在 CLEANUP，故不需要"CLEANUP ⇒ 不回滚"的分岔
    //           ├── 跳过 RESTORE_*/REMOVE_*/元数据/CLEANUP
    //           ├── :batch-start DB 条目**仅在正式文件仍在且非空时**跳过
    //           │   （batch_start_db_still_in_place()，wal_op.cpp）
    //           ├── 只执行正向操作的逆向
    //           ├── Cache::load()
    //           └── COMMIT_PKGS
    // 4. cleanup_db_backups()
}
```

### 11.3 rec 的关键设计决策

| 设计点 | 决策 | 理由 |
|--------|------|------|
| 是否跳过 RESTORE_* 行 | ✅ 跳过 | RESTORE_* 是 rollback 的产物，再次逆序会重做正向操作 |
| 是否处理 :batch-start DB 标记 | ⚠️ **有条件跳过**：正式文件仍在**且非空**才跳过，否则从该里程碑备份还原（`batch_start_db_still_in_place()`，`wal_op.cpp`） | "最终状态"只在该状态确实还在盘上时成立。`write_db_file_wal`/`write_set_file_wal` 的 rename 窗口里正式名已消失（`init_filesystem()` 之后则是"存在但 0 字节"），此时那份备份是唯一还原依据；无条件跳过 = 它永远无人消费，且 `cleanup_db_backups()` 随后把唯一备份删掉（`pkgs`/`holdpkgs` 缺失让恢复整体失败，`files.db`/`provides.db`/`confhashes.db` 静默归零，均不可逆）。现场复现：`test_db_batch_start_recovery.cpp` |
| 是否写 RESTORE_* 审计 | ✅ 是 | rec 的 reverse_execute 应该与 batch_rollback 行为一致 |
| 旧版二进制写入的"批次内 CLEANUP"WAL | ❌ **不支持** | lpkg 经 lpkg 升级时，**旧二进制**会先跑 `recover_packages()` 处理掉遗留 WAL，新二进制才上线；因此更新后不存在需要兼容的旧形状事务。**手工替换 lpkg 二进制不受支持**（若此时正躺着一个被中断的 remove WAL，回滚会让 DB 回到"已安装"而文件已删 —— 遇到时用 `lpkg rec` 前先人工核对） |
| 是否检测 CLEANUP 分岔 | ❌ 不再需要 | CLEANUP 只出现在事务之外（post-commit），事务内不可能有 → 未提交批次一律 `reverse_execute`。post-commit 的残留清理由 `continue_post_commit_cleanup` 负责（§10） |
| 是否清理孤备份 | ✅ 是 | 清理 .lpkg_db_bak 残留 |

---

## 12. WAL Trim

### 12.1 逻辑

```cpp
void trim_completed() {
    // 只识别 BEGIN_PKGS / COMMIT_PKGS
    // 所有已配对的 BEGIN_PKGS...COMMIT_PKGS 块被删除
    // 最后一个未配对的 BEGIN_PKGS 及其所有行保留
    // 如果没有未配对 BEGIN_PKGS，清空整个日志
}
```

### 12.2 与 RESTORE 审计行的交互

trim 不关心具体行内容，只跟踪 BATCH 边界。RESTORE_* 等行在已完成事务中被 trim 正常清理。未完成事务中的 RESTORE_* 行被保留（它们帮助判断 rollback 进度）。

---

## 13. 实现清单

所有阶段均已完成，具体实现如下。

### 第 1 阶段：基础设施

- **1.1 `DbMilestone`** — 定义于 `wal_op.hpp`。格式 `pkg:state`，`:batch-start` 表示批次开始前的 DB 快照。
- **1.2 `Cache::write` write-ahead** — 实现于 `cache.cpp`。顺序：WAL → fsync → 备份（`.lpkg_db_bak_before:<milestone>`）→ fsync → .tmp → fsync → rename → fsync。`reverse_execute` 遇到备份不存在时跳过（原文件还在，安全）。
- **1.3 `WalWriter`** — 实现于 `transaction_log.hpp/cpp`。每行 O_APPEND + write + fsync，带 move 语义。
- **1.4 `parse_op`** — 实现于 `wal_op.cpp`。解析 25 种操作类型，支持 `→` 分隔符和简单空格分割。
- **1.5 `reverse_execute`** — 实现于 `wal_op.cpp`。跳过元数据行、RESTORE 审计行；`:batch-start` DB 条目**只在正式文件仍在且非空时**跳过，否则从该里程碑备份还原（`batch_start_db_still_in_place()`，见 §11.3 与 `test_db_batch_start_recovery.cpp`）。每步逆向操作后写入 RESTORE_* 审计行（`write_audit=true` 时）。**没有**"里程碑提前停止"机制（正常路径下"逆序跑完整个批次"与"停在 :batch-start"逐字节同效，故不需要）。
- **1.6 `extract_current_batch_ops`** — 实现于 `wal_op.cpp`。从最后一个 `BEGIN_PKGS` 提取到文件末尾。
- **1.7 `batch_rollback`** — 实现于 `wal_op.cpp`。流程：清理内存 cache → `reverse_execute` → `Cache::load()` 重载磁盘 DB → 写 DB `:batch-start` → 写 ROLLBACK/END 标记 → `COMMIT_PKGS`。

### 第 2 阶段：安装事务

- **2.1 `run_batch_transaction`** — 模板定义于 `batch_transaction.hpp`。正向：`BEGIN_PKGS` → `Cache::write(":batch-start")` → 逐包执行 → `COMMIT_PKGS`。异常路径：catch → `batch_rollback` → rethrow。
- **2.2 `install_packages`** — 重构于 `package_manager.cpp`。一致性重试循环在 batch 外部，实际安装封装在 `run_batch_transaction` 中。每包后 `Cache::write(pkg + ":installed")`。安装完成后收集 `.lpkg_bak` 路径并统一清理。
- **2.3 `InstallationTask`** — 重构于 `installation_task.cpp`。`run()` 写入 WAL `BEGIN`/`COMMIT`/`END` 标记。`backup_existing_files()` 写入 `BACKUP`/`NEW`/`NEW_DIR`（write-ahead）。`copy_package_files()` 写入 `COPY`。`rollback_files()` 只写 `ROLLBACK`/`END` 标记（**单撤销路径**，文件撤销统一由 `batch_rollback` → `reverse_execute` 完成，见 §6.5）。`.lpkg_bak` 延迟到批次提交后清理。

### 第 3 阶段：移除事务

- **3.1 `remove_packages_checked`**（`package_manager.cpp`）— **所有多包移除的唯一实现**：`remove_package`（单包）、`remove_packages`（`remove a b c`）、`autoremove`、`remove_package_recursive`（闭包）、`force_solve_conflict` 全部经由它。筛选（未安装/essential/反向依赖）→ `remove_packages_in_one_batch()`（**整组一个批次**，逐包 `do_remove_package`：`RM_BEGIN` → `BACKUP` → `DBRM`（deps/needed_so/man）→ DB → `RM_COMMIT` → `RM_END`）→ `finish_committed_batch()`（post-commit：`CLEANUP` 行 → 删 stash → trim）。逐包各开批次会失去跨包原子性（中途 Ctrl+C 只回滚当前包）。
- **3.2 `remove_package_recursive`** — 重构于 `package_manager.cpp`。算出依赖闭包后走 §3.1 的同一批次机制：闭包内所有包同一个 `run_batch_transaction`，任一失败整批回滚。

### 第 4 阶段：升级事务

- **4.1 `upgrade_packages`** — 重构于 `package_manager.cpp`。整批升级封装在 `run_batch_transaction` 中。每包升级重用 `InstallationTask`（`old_version_to_replace` 设置）。升级完成后收集 `.lpkg_bak` 并清理。

### 第 5 阶段：rec、trim、二次回滚

- **5.1 `recover_packages`** — 实现于 `recover.cpp`。状态机：扫描 WAL → 找到未完成批次（`BEGIN_PKGS` 无对应 `COMMIT_PKGS`）→ `reverse_execute` → `Cache::load()` → `COMMIT_PKGS`。跳过 RESTORE_* 行；`:batch-start` DB 条目**只在正式文件仍在且非空时**跳过，否则从该里程碑备份还原（`batch_start_db_still_in_place()`，§11.3）。通过 CLI `lpkg rec` 或启动时自动调用。
- **5.2 `trim_completed`** — 实现于 `recover.cpp`。从后向前找到最后一个未配对 `BEGIN_PKGS`，删除之前所有已完成的批次日志。
- **5.3 二次回滚测试** — 覆盖于 `test_breakpoints.cpp`。模拟 rollback 中途崩溃（RESTORE_DB 后 / rollback 完成后 COMMIT_PKGS 未写），验证 `recover_packages` 幂等续传。

### 第 6 阶段：测试

- **6.1 断电模拟** — `test_breakpoints.cpp`（17 tests）。覆盖 TODO.md §3 所有断电点：DB 写入（5 个断点）、文件 BACKUP（2 个断点）、COPY（2 个断点）、NEW/NEW_DIR、DBNEW/DBRM。
- **6.2 幂等性** — `test_wal_core.cpp`（63 tests）。覆盖所有操作类型的 `reverse_execute` 幂等性（NULL→跳过、重复→跳过）。
- **6.3 里程碑链式恢复** — `test_wal_core.cpp` + `test_breakpoints.cpp`。验证 DB 备份链 `batch-start ← A:installed ← B:installed` 的正确逆序恢复。
- **6.4 二次回滚幂等** — `test_breakpoints.cpp`。验证 rollback 各阶段中断后 `recover_packages` 能正确继续。
- **6.5 集成测试** — 多个测试文件覆盖：批量安装/移除/升级、依赖链、provides 解析、版本约束、config 保护、SIGINT 保护、并发锁、autoremove、recursive remove。
- **6.6 CLEANUP 阶段测试** — `test_cleanup.cpp`（22 tests）。覆盖 CLEANUP 解析与不可逆性、目录 BACKUP 恢复、随机后缀唯一性、rec CLEANUP 续传、安全检查、现有行为回归。
- **6.7 双重回滚回归（2026-08-03）** — `test_active_rollback.cpp`。升级中途 COPY 失败 / COMMIT 后失败 → 旧文件必须保留（曾双重回滚删旧文件）；CLEANUP write-ahead 崩溃窗口 → 整批可恢复。
- **6.8 全量** — 当前 **500+ tests 全绿**（docker 容器 `make test`），覆盖上述全部章节。

---

## 附录：完整 WAL 示例总结

```
成功安装:
  BEGIN_PKGS → ... → COMMIT_PKGS

成功移除（`remove a b c` / `autoremove` / `remove -r` 闭包 / `force-solve-conflict`
共用 `remove_packages_checked()`，**整组一个批次**）:
  BEGIN_PKGS → [逐包: RM_BEGIN → BACKUP... → DBRM... → DB... → RM_COMMIT → RM_END] → COMMIT_PKGS
  └─ post-commit 收尾（finish_committed_batch）: CLEANUP <stash 根> → 删除 stash → trim_completed
                                               → cleanup_db_backups

安装失败回滚:
  BEGIN_PKGS → ... A OK → ... B FAIL → ROLLBACK B → END B
  → RESTORE_DB → RESTORE_FILE → REMOVE_FILE → DB :batch-start
  → ROLLBACK A → END A → COMMIT_PKGS

移除失败回滚:
  BEGIN_PKGS → RM_BEGIN → BACKUP... → (异常)
  → RESTORE_FILE... → DB :batch-start → COMMIT_PKGS

安装时断电:
  BEGIN_PKGS → BEGIN A → BACKUP ... 断电
  → rec: reverse_execute → COMMIT_PKGS

rollback 自身断电:
  BEGIN_PKGS → ... → RESTORE_DB → 断电
  → rec: 跳过 RESTORE_DB (bak已消费), 继续逆向其他
  → COMMIT_PKGS
```
