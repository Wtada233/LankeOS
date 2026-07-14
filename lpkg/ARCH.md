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
14. [旧文件清理清单](#14-旧文件清理清单)

---

## 1. 核心架构约束

### 1.1 批次模型（不变）

- **I-1**: 所有写操作（install / remove / upgrade / reinstall / autoremove）均为批次事务，即使只有一个包也走 `BEGIN_PKGS 1 → ... → COMMIT_PKGS`。不存在"独立包事务"。
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
- **I-BAK-2**: 安装流程的 `cleanup_backups()` 在批次模式下为空操作。实际清理由 `COMMIT_PKGS` 后的统一清理阶段执行。
- **I-BAK-3**: 移除流程的目录删除和 `.lpkg_bak` 清理在 `RM_COMMIT` 后的后提交阶段执行。`RM_COMMIT` 前目录保持不变。

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

---

## 2. WAL 2.0 协议

### 2.1 日志行完整列表

| 分类 | 行前缀 | 含义 | fsync |
|------|--------|------|-------|
| **批次边界** | `BEGIN_PKGS <N>` | 批次开始，N=包数 | 写后 fsync |
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
| **移除操作** | `RM_BEGIN <pkg> <ver>` | 移除开始 | 写后 fsync |
| | `RM_COMMIT <pkg> <ver>` | 移除提交 | 写后 fsync |
| | `RM_END <pkg> <ver>` | 移除结束 | 写后 fsync |
| | `RM_DIR <path> <mode> <uid> <gid>` | 目录元数据快照（回滚时重建用） | 写后 fsync |
| **DB 操作** | `DB <path> <pkg>:<state>` | DB 文件修改后状态 | 写前已备份 + fsync |
| | `DBNEW <path> <pkg>:<state>` | DB 文件新建后状态 | 写前已备份 + fsync |
| | `DBRM <path> <pkg>:<state>` | DB 文件删除后状态 | 写前已备份 + fsync |
| **回滚审计** | `RESTORE_FILE <bak> → <orig>` | rollback：从 .bak 恢复文件 | 写后 fsync |
| | `RESTORE_DB <bak> → <db>` | rollback：从 .db_bak 恢复 DB | 写后 fsync |
| | `RESTORE_DIR <path>` | rollback：重建目录 | 写后 fsync |
| | `REMOVE_FILE <path>` | rollback：删除新文件 | 写后 fsync |
| | `REMOVE_DIR <path>` | rollback：删除新目录 | 写后 fsync |

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
BEGIN_PKGS 1                        ← fsync
BEGIN curl 8.11.1                    ← fsync
BACKUP /usr/bin/curl → /usr/bin/curl.lpkg_bak_curl  ← fsync
NEW /usr/share/doc/curl/README      ← fsync
COPY /tmp/curl.lpkgtmp → /usr/bin/curl  ← fsync
COMMIT curl 8.11.1                   ← fsync
END curl 8.11.1                      ← fsync
DB /var/lib/lpkg/pkgs curl:installed  ← 备份后 + fsync
DB /var/lib/lpkg/files.db curl:installed  ← 备份后 + fsync
COMMIT_PKGS                          ← fsync
```

**批量 [A, B] 中 B 失败 → 全批次回滚（含 RESTORE 审计）**：
```
BEGIN_PKGS 2                        ← fsync
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
BEGIN_PKGS 1                        ← fsync
RM_BEGIN curl 8.11.1                 ← fsync
BACKUP /usr/bin/curl → /usr/bin/curl.lpkg_bak_curl   ← fsync
BACKUP /usr/share/man/man1/curl.1 → /usr/share/man/man1/curl.1.lpkg_bak_curl  ← fsync
── 错误：磁盘满 ──
── catch：batch_rollback ──
RESTORE_FILE /usr/share/man/man1/curl.1.lpkg_bak_curl → /usr/share/man/man1/curl.1  ← fsync
RESTORE_FILE /usr/bin/curl.lpkg_bak_curl → /usr/bin/curl  ← fsync
DB /var/lib/lpkg/pkgs curl:installed  ← 恢复到安装状态
ROLLBACK curl 8.11.1                   ← fsync
END curl 8.11.1                        ← fsync
COMMIT_PKGS                           ← fsync
```

**升级失败回滚到旧版本**：
```
BEGIN_PKGS 1
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
BEGIN_PKGS 2
... (install A, B fails) ...
RESTORE_DB /var/lib/lpkg/pkgs.lpkg_db_bak_before:A:installed → /var/lib/lpkg/pkgs
── 断电 here ──
```

重启 `recover_packages()`：
1. 读 WAL，看到 `BEGIN_PKGS 2` 无 `COMMIT_PKGS` → 未完成事务
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

**目标**：修改 DB 文件（如 pkgs、files.db）

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

### 3.6 RM_DIR 操作——记录目录元数据

RM_DIR 不是一个"目录已被删除"的操作日志，而是一个**目录元数据快照**。
它在移除流程中记录目录在删除前的权限和所有权（mode / uid / gid），
供回滚时重建目录使用。RM_DIR 本身不执行删除——目录的实际删除在后提交阶段进行。

```
WAL: RM_DIR <path> <mode> <uid> <gid>
  含义：此目录在移除前具有 mode/uid/gid 权限。
       回滚时需以此权限重建。
```

`reverse_execute` 遇到 RM_DIR：
```
  → create_directories(op.arg1)
  → chmod(op.arg1, mode)
  → chown(op.arg1, uid, gid)
```

注意：重建目录是为了让 BACKUP 的 rename 能成功（rename 到原位置需要父目录存在）。
即使目录本身没有文件（只有子目录），RM_DIR 元数据也保证重建后的目录权限正确。

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

旧版用 `Cache::write(wal_tag)`，`wal_tag=包名` 作为 `.lpkg_db_bak_<tag>` 后缀。当批量多个包时，`.lpkg_db_bak_A` 和 `.lpkg_db_bak_B` 无法确定哪个对应什么系统状态。

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

  内存 Cache 说："文件 X 属于包 B"（因为 force_overwrite 在内存改了所有权）
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

force_overwrite 时直接在内存改所有权，批次成功则随 DB 写盘持久化，
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
void run_batch_transaction(const std::string &wal_tag, size_t total, OpT &op);
```

### 5.2 不变量

- 进入 `run_batch_transaction` 前：WAL 已 `trim_completed`，无未完成事务。
- `BEGIN_PKGS` 写入 + fsync 后：异常路径保证 `COMMIT_PKGS` 一定被写入（catch 补写）。
- `COMMIT_PKGS` 是批次完结的唯一标记——不区分"成功完结"和"回滚完结"。外部只看有无 COMMIT_PKGS。
- 回滚后：WAL 包含完整的 RESTORE 审计链，系统状态一致。

### 5.3 回滚触发条件

| 触发条件 | 回滚范围 | 路径 |
|---------|---------|------|
| 安装包中途失败 | 当前包 | `InstallationTask::run()` 的 catch → 包级文件回滚 |
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
├── run_batch_transaction("pkgs", N, [&] {
│   │
│   ├── Cache::write(":batch-start")    ← WAL: DB /pkgs :batch-start (备份批次开始状态)
│   │                                    ← fsync WAL, fsync 备份
│   │
│   ├── for each pkg in order:
│   │     task.run(&ctx)
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
│       └── force_overwrite 时：缓存直接改所有权
│           cache.remove_file_owner(path, old_owner)
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
│   ├── register_package() (内存操作)
│   ├── 处理 REMOVE_OLD (升级时)
│   │   WAL: REMOVE_OLD <src> → <dst>  ← fsync
│   │   rename old_file → .lpkg_bak
│   │   fsync 父目录
│   └── run_post_install_hook()
│
├── WAL: COMMIT <pkg> <ver>             ← fsync
├── cleanup_backups()
│   (不移除 .bak！所有 .lpkg_bak 延迟到 COMMIT_PKGS 后统一清理，
│   确保批量回滚时可恢复每个已安装包的文件)
├── WAL: END <pkg> <ver>               ← fsync
│
└── return ✓
```

### 6.3 包级回滚（InstallationTask::rollback）

包级回滚在 `copy_package_files()` 的 catch 中触发，只做文件恢复：

```
rollback()
│
├── for each 已备份文件 (backups_):
│   ├── rename .lpkg_bak → 原位置
│   ├── fsync 原位置父目录
│   ├── WAL: RESTORE_FILE <bak> → <orig>  ← fsync
│
├── for each 新文件 (new_files_):
│   ├── fs::remove(新文件)
│   ├── WAL: REMOVE_FILE <path>          ← fsync
│
├── for each 新目录 (new_dirs_):
│   ├── 仅当空时 fs::remove
│   ├── WAL: REMOVE_DIR <path>           ← fsync
│
└── backups_.clear(); new_files_.clear(); new_dirs_.clear();
```

---

## 7. 移除流程

### 7.1 顶层流程

```
remove_package(pkg_name, force, wrap_in_txn)
│
├── recover_packages() + trim_completed()  (if wrap_in_txn)
├── 版本检查 + 安全检查
│
├── run_batch_transaction("remove-" + pkg, 1, [&] {
│   do_remove_package(pkg, force)
│ })
```

### 7.2 核心移除逻辑

```
do_remove_package(pkg_name, force)
│
├── SIGINT 检查
├── prerm hook
│
├── WAL: RM_BEGIN <pkg> <ver>            ← fsync
│
├── 共享文件检查
│
├── 备份阶段：逐文件
│   for each owned_file:
│     SIGINT 检查
│     WAL: BACKUP <phys> → <bak>          ← fsync
│     rename phys → bak
│     fsync 父目录
│
├── remove_package_files()               ← 从磁盘删除文件
│
├── 目录处理：释放所有权（仅内存操作）
│   for each dir（逆序）:
│     释放所有权
│     if 最后持有者:
│       // 记录目录元数据供回滚使用
│       WAL: RM_DIR <path> <mode> <uid> <gid>  ← fsync
│       // 注意：不删除目录！不 RM_BAK_CLN！
│       // 目录和其内部的 .lpkg_bak 在后提交阶段处理
│
├── 清理 dep/needed_so/man/hooks
│   WAL: DBRM /deps/pkg pkg:removed      ← fsync
│   rename /deps/pkg → .lpkg_db_bak_before:pkg:removed
│   fsync 父目录
│   （同样的 DBRM 流程处理 needed_so 和 man）
│
├── DB 落盘：
│   DB /pkgs pkg:removed                  ← 备份后 + fsync
│   DB /files.db pkg:removed              ← 备份后 + fsync
│
├── WAL: RM_COMMIT <pkg> <ver>           ← fsync
│
├── ── 后提交阶段（事务已提交，最佳努力） ──
├── 清理本包 .lpkg_bak
│   for each backup: fs::remove
│   （此刻 .lpkg_bak 已清理，目录变空）
├── 删除空目录（仅最后持有者）
│   for each RM_DIR from this batch:
│     if dir is empty:
│       fs::remove(dir)
├──
├── WAL: RM_END <pkg> <ver>             ← fsync
│
└── return ✓
```

### 7.3 移除回滚

```
run_batch_transaction 的 catch:
  → batch_rollback(success)
  
batch_rollback 对移除的逆向：
  → 逆序处理 WAL 行：
    RM_END      → 跳过（元数据）
    RM_COMMIT   → 跳过（元数据）
    DB /pkgs pkg:removed  → 查找 .lpkg_db_bak_before:pkg:removed
       存在   → rename 回 /pkgs (→ pkg:installed)
       WAL: RESTORE_DB <bak> → <db>      ← fsync
    DBRM /deps/pkg pkg:removed
       → 查找 .lpkg_db_bak_before:pkg:removed（存的是删除前的文件）
       → 存在: rename 回
       WAL: RESTORE_DB <bak> → <db>      ← fsync
    RM_DIR <path> <mode> <uid> <gid>
       → fs::create_directories(path)
       → chmod/chown 根据 mode/uid/gid
       WAL: RESTORE_DIR <path>            ← fsync
    BACKUP /phys → /bak
       → rename bak → phys
       WAL: RESTORE_FILE <bak> → <phys>  ← fsync
    RM_BEGIN  → 已处理完毕
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
 *
 * @param ops              待逆向执行的操作（正向顺序）
 * @param milestone_target 目标里程碑（":batch-start"），
 *                         达到后停止回滚。空=全部逆序
 * @param write_audit      是否写 RESTORE WAL 审计行（正常=true，rec 时=true）
 * @return RollbackStats
 */
RollbackStats reverse_execute(
    const std::vector<WALOp> &ops,
    const std::string &milestone_target = "",
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
 * 4. reverse_execute(ops, ":batch-start")
 * 5. DB /pkgs :batch-start
 * 6. ROLLBACK pkg + END pkg 对每个已回滚包
 * 7. COMMIT_PKGS
 */
void batch_rollback(const std::vector<std::string> &success);

} // namespace wal
```

### 9.2 reverse_execute 操作表

| 操作类型 | 逆向操作 | fsync | RESTORE 条 | 备份不存在时 |
|---------|---------|-------|-----------|-------------|
| `BACKUP` | rename .bak → 原位 | fsync 父目录 | `RESTORE_FILE <bak> → <orig>` | .bak 不存在→跳过（已被消费→幂等） |
| `REMOVE_OLD` | 同上（同 BACKUP） | 同上 | `RESTORE_FILE <bak> → <orig>` | 同上 |
| `COPY` | remove(dst) | 不需要（删除不需要 fsync） | `REMOVE_FILE <dst>` | 不存在→跳过 |
| `NEW` | remove(path) | 不需要 | `REMOVE_FILE <path>` | 不存在→跳过 |
| `NEW_DIR` | rmdir（仅空时） | 不需要 | `REMOVE_DIR <path>` | 不存在→跳过 |
| `RM_DIR` | create_directories + chmod/chown | fsync 父目录 | `RESTORE_DIR <path>` | 已存在→跳过 |
| `DB` | .bak_before 存在→rename 回 | fsync 父目录 + WAL | `RESTORE_DB <bak> → <db>` | .bak 不存在→跳过（WAL 已写但备份未完成→原文件还在） |
| `DBNEW` | .bak 存在→rename 回; 不存在→删除文件 | fsync 父目录 + WAL | `RESTORE_DB` 或 `REMOVE_FILE` | - |
| `DBRM` | .bak 存在→rename 回 | fsync 父目录 + WAL | `RESTORE_DB <bak> → <db>` | .bak 不存在→跳过 |

### 9.3 里程碑停止机制

```cpp
RollbackStats reverse_execute(ops, milestone_target, write_audit) {
    for (int i = ops.size() - 1; i >= 0; --i) {
        const auto &op = ops[i];
        
        if (op.type == "DB" || op.type == "DBNEW" || op.type == "DBRM") {
            // 执行逆向恢复（见上表）
            // ...
            
            // 检查是否达到了目标里程碑
            if (!milestone_target.empty() && op.arg2 == milestone_target) {
                // DB 已达到目标状态，停止进一步回滚
                // 之前的操作不需要再逆向了
                return stats;
            }
        }
        
        // 其他操作类型处理...
    }
    return stats;
}
```

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
  BEGIN_PKGS 2
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
  BEGIN_PKGS 2
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

**解决方案**：`reverse_execute` 在遇到 `DB /pkgs :batch-start` 时，应该将其视为"最终状态标记"，不逆向恢复 DB 到 `:batch-start` 之前的状态。

实际上这个问题的本质是：**`DB /pkgs :batch-start` 意味着"回滚后的最终 DB 状态"，不应该被进一步逆向**。

修复方案：在遇到 `DB /pkgs :batch-start` 时，`reverse_execute` 跳过该 DB 条目的逆序（不恢复备份）。

在 `reverse_execute` 中：
```cpp
if (op.arg2 == ":batch-start") {
    // :batch-start 是"最终状态标记"，不逆向恢复 DB
    // 这表示回滚已彻底完成，DB 已恢复到初始状态
    continue;
}
```

同样地，`DBNEW` 和 `DBRM` 遇到 `:batch-start` 也跳过。

这样场景 B 的恢复顺序：
```
reverse_execute 逆序：
  DB /pkgs :batch-start   → 跳过（最终状态标记）
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
逆序处理（跳过 RESTORE_* / REMOVE_* / ROLLBACK / END / BEGIN / COMMIT / DB :batch-start）：
  只处理正向操作：BACKUP、COPY、NEW、DB（非:batch-start）等
  → 恢复系统到 :batch-start 状态
  → 第二次 COMMIT_PKGS
```

**结论**：`reverse_execute` 始终跳过 `RESTORE_*`、`REMOVE_*`、`ROLLBACK`、`END`、`COMMIT`、`BEGIN`、`RM_*` 元数据行以及 DB `:batch-start` 条目。只处理正向操作行（BACKUP、COPY、NEW、NEW_DIR、RM_DIR、DB/DBNEW/DBRM 非:batch-start）。

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
    //    a) parse_op 解析为 WALOp 列表（跳过不合法行）
    //    b) reverse_execute(ops, "", true)
    //       - 跳过 RESTORE_*/REMOVE_*/ROLLBACK/END/BEGIN/COMMIT/RM_* 行
    //       - 跳过 :batch-start DB 条目
    //       - 只执行正向操作的逆向
    //    c) WAL: COMMIT_PKGS + fsync
    // 4. 清理残留备份（将在新架构中重新实现 cleanup_db_backups）
}
```

### 11.3 rec 的关键设计决策

| 设计点 | 决策 | 理由 |
|--------|------|------|
| 是否跳过 RESTORE_* 行 | ✅ 跳过 | RESTORE_* 是 rollback 的产物，再次逆序会重做正向操作 |
| 是否处理 :batch-start DB 标记 | ✅ 跳过 | :batch-start 是"最终状态"，不逆向 |
| 是否写 RESTORE_* 审计 | ✅ 是 | rec 的 reverse_execute 应该与 batch_rollback 行为一致 |
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
- **1.5 `reverse_execute`** — 实现于 `wal_op.cpp`。跳过元数据行、RESTORE 审计行和 `:batch-start` DB 条目。每步逆向操作后写入 RESTORE_* 审计行（`write_audit=true` 时）。支持里程碑提前停止。
- **1.6 `extract_current_batch_ops`** — 实现于 `wal_op.cpp`。从最后一个 `BEGIN_PKGS` 提取到文件末尾。
- **1.7 `batch_rollback`** — 实现于 `wal_op.cpp`。流程：清理内存 cache → `reverse_execute` → `Cache::load()` 重载磁盘 DB → 写 DB `:batch-start` → 写 ROLLBACK/END 标记 → `COMMIT_PKGS`。

### 第 2 阶段：安装事务

- **2.1 `run_batch_transaction`** — 模板定义于 `batch_transaction.hpp`。正向：`BEGIN_PKGS` → `Cache::write(":batch-start")` → 逐包执行 → `COMMIT_PKGS`。异常路径：catch → `batch_rollback` → rethrow。
- **2.2 `install_packages`** — 重构于 `package_manager.cpp`。一致性重试循环在 batch 外部，实际安装封装在 `run_batch_transaction` 中。每包后 `Cache::write(pkg + ":installed")`。安装完成后收集 `.lpkg_bak` 路径并统一清理。
- **2.3 `InstallationTask`** — 重构于 `installation_task.cpp`。`run()` 写入 WAL `BEGIN`/`COMMIT`/`END` 标记。`backup_existing_files()` 写入 `BACKUP`/`NEW`/`NEW_DIR`（write-ahead）。`copy_package_files()` 写入 `COPY`。`rollback_files()` 执行文件级回滚 + RESTORE_* 审计。`.lpkg_bak` 延迟到批次提交后清理。

### 第 3 阶段：移除事务

- **3.1 `remove_package`** — 重构于 `package_manager.cpp`。单包移除封装在 `run_batch_transaction(1, ...)` 中。流程：`RM_BEGIN` → `BACKUP` → `RM_DIR`（目录元数据）→ `DBRM`（deps/needed_so/man）→ `RM_COMMIT` → 清理 `.lpkg_bak` + 空目录 → `RM_END`。
- **3.2 `remove_package_recursive`** — 重构于 `package_manager.cpp`。所有受影响包在同一个 `run_batch_transaction` 中移除，任一失败则整批回滚。

### 第 4 阶段：升级事务

- **4.1 `upgrade_packages`** — 重构于 `package_manager.cpp`。整批升级封装在 `run_batch_transaction` 中。每包升级重用 `InstallationTask`（`old_version_to_replace` 设置）。升级完成后收集 `.lpkg_bak` 并清理。

### 第 5 阶段：rec、trim、二次回滚

- **5.1 `recover_packages`** — 实现于 `recover.cpp`。状态机：扫描 WAL → 找到未完成批次（`BEGIN_PKGS` 无对应 `COMMIT_PKGS`）→ `reverse_execute` → `Cache::load()` → `COMMIT_PKGS`。跳过 RESTORE_* 行和 `:batch-start` DB 条目。通过 CLI `lpkg rec` 或启动时自动调用。
- **5.2 `trim_completed`** — 实现于 `recover.cpp`。从后向前找到最后一个未配对 `BEGIN_PKGS`，删除之前所有已完成的批次日志。
- **5.3 二次回滚测试** — 覆盖于 `test_breakpoints.cpp`。模拟 rollback 中途崩溃（RESTORE_DB 后 / rollback 完成后 COMMIT_PKGS 未写），验证 `recover_packages` 幂等续传。

### 第 6 阶段：测试

- **6.1 断电模拟** — `test_breakpoints.cpp`（17 tests）。覆盖 TODO.md §3 所有断电点：DB 写入（5 个断点）、文件 BACKUP（2 个断点）、COPY（2 个断点）、NEW/NEW_DIR、RM_DIR、DBNEW/DBRM。
- **6.2 幂等性** — `test_wal_core.cpp`（63 tests）。覆盖所有操作类型的 `reverse_execute` 幂等性（NULL→跳过、重复→跳过）。
- **6.3 里程碑链式恢复** — `test_wal_core.cpp` + `test_breakpoints.cpp`。验证 DB 备份链 `batch-start ← A:installed ← B:installed` 的正确逆序恢复。
- **6.4 二次回滚幂等** — `test_breakpoints.cpp`。验证 rollback 各阶段中断后 `recover_packages` 能正确继续。
- **6.5 集成测试** — 多个测试文件覆盖：批量安装/移除/升级、依赖链、provides 解析、版本约束、config 保护、SIGINT 保护、并发锁、autoremove、recursive remove。总计 412 tests。

---

## 14. 旧文件清理清单

### 14.1 已删除的文件

| 文件 | 原因 |
|------|------|
| `ARCH.md` | 已被 TODO.md 替代 |
| `transaction_log.cpp` / `.hpp` | 旧 WAL 实现，废弃 |
| `wal_op.cpp` / `.hpp` | 旧 WAL 操作实现，废弃（将重写） |
| `recover.cpp` | 旧 rec 实现，废弃（将重写） |
| `test_transaction_log.cpp` | 测试已删除的基础设施 |
| `test_log_trim.cpp` | 同上 |
| `test_log_trim_recovery.cpp` | 同上 |
| `test_db_wal.cpp` | 同上 |
| `test_atomic_rollback.cpp` | 同上 |
| `test_atomic_transaction_fixes.cpp` | 同上 |
| `test_atomicity_boundary.cpp` | 同上 |
| `test_stress_recovery.cpp` | 同上 |

### 14.2 待删除的旧函数

| 函数 | 替代 |
|------|------|
| `rollback_installed_package()` | `batch_rollback()` + `reverse_execute()` |
| `rollback_committed_packages()` | `batch_rollback()` |
| `with_batch_transaction()` | `run_batch_transaction()` |
| `deferred_remove_baks` / `clean_deferred_remove_baks()` | 移至 batch_rollback 管理 |
| `remove_package()` catch 中调 `recover_packages()` | 调 `batch_rollback()` |

### 14.3 待重写的文件

| 文件 | 重写内容 |
|------|---------|
| `wal_op.hpp` | 新 DbMilestone、WALOp、reverse_execute、batch_rollback API |
| `wal_op.cpp` | reverse_execute 实现（含 RESTORE 审计）、batch_rollback、extract_current_batch_ops |
| `transaction_log.hpp` | 新 WalWriter 类 |
| `transaction_log.cpp` | 原子 WAL 写入 + fsync |
| `recover.cpp` | 新 recover_packages + trim_completed |
| `cache.cpp` / `.hpp` | 新 write_db_file write_ahead 顺序 |

---

## 附录：完整 WAL 示例总结

```
成功安装:
  BEGIN_PKGS 1 → ... → COMMIT_PKGS

成功移除:
  BEGIN_PKGS 1 → RM_BEGIN → BACKUP... → RM_DIR... → DBRM... → DB... → RM_COMMIT
  └─ 后提交: 清理 .lpkg_bak → 删除空目录 → RM_END → COMMIT_PKGS

安装失败回滚:
  BEGIN_PKGS 2 → ... A OK → ... B FAIL → ROLLBACK B → END B
  → RESTORE_DB → RESTORE_FILE → REMOVE_FILE → DB :batch-start
  → ROLLBACK A → END A → COMMIT_PKGS

移除失败回滚:
  BEGIN_PKGS 1 → RM_BEGIN → BACKUP... → (异常)
  → RESTORE_FILE... → DB :batch-start → COMMIT_PKGS

安装时断电:
  BEGIN_PKGS 2 → BEGIN A → BACKUP ... 断电
  → rec: reverse_execute → COMMIT_PKGS

rollback 自身断电:
  BEGIN_PKGS 2 → ... → RESTORE_DB → 断电
  → rec: 跳过 RESTORE_DB (bak已消费), 继续逆向其他
  → COMMIT_PKGS
```
