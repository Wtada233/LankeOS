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
- **I-8**: rollback 的每个逆向操作在文件系统生效后，立即写入 `RESTORE_*` WAL 审计行。若 rollback 中途崩溃，重启后的 `recover_packages()` 靠**每条逆操作自查"备份/目标还在不在"**幂等续传（已消费的备份 → 跳过）。
  （订正 2026-09-25：原文后半句写"能通过 RESTORE 行判断哪些操作已完成" —— 现行实现把 RESTORE 行整类跳过（`skip_in_reverse()`），判断依据是文件系统状态而不是这些行，见 §10.3。）

### 1.3 备份清理纪律

- **I-BAK-1**: `.lpkg_bak` 文件在 `COMMIT_PKGS` 之前绝不删除。即使批次中一个包已成功安装，其 `.lpkg_bak` 也必须保留到批次提交后。
- **I-BAK-2**: 安装流程的 `.lpkg_bak` 清理统一在 `COMMIT_PKGS` 之后执行（调用方把
  `task.get_stashes()` 收进批次级向量，交给 `finish_committed_batch()` 的
  `cleanup_stashes()` 按 stash 根 `remove_all`）。批次内绝不提前清理，确保批量回滚时可恢复每个已安装包的文件。
> **stash 根的派生必须先 `strip_trailing_slash`**（2026-09-26 第三轴审计补）：`stash_root_of_bak()`
> 的返回值会被喂给 `fs::remove_all`，而实测 **`fs::remove_all("link/")` 会删光链接目标的全部内容**
> （并返回 `ENOTDIR`）。今天它只靠"WAL 字面量恒不带尾斜杠"这条君子协定（bak 名由
> `dir / (base + 后缀 + …)` 拼出），尾斜杠一旦出现就是灾难性的 —— 故在派生处显式剥一次。
  （订正 2026-09-25：原文写的是"调用方收集 `task.get_backups()` 后 `fs::remove`" —— 没有
  `get_backups()` 这个接口，备份早已改成每文件系统 stash、清理是按目录 `remove_all`
  而不是逐文件 `fs::remove`。）
- **I-BAK-3**: 移除流程的 `.lpkg_bak` 清理与安装**完全一致**：统一在 `COMMIT_PKGS` **之后**执行
  （`finish_committed_batch()`：写 `CLEANUP` 行 → 物理删除 stash → 剪枝 hooks → 执行 postinst → `trim_completed` → `cleanup_db_backups`）。
  stash 是移除的**唯一回滚来源**（装的是刚被删掉的文件），因此必须活到"批次已不可能回滚"为止——
  推论：**只要批次未提交，中途中断（Ctrl+C/失败）一律整批恢复**；`CLEANUP` 行位于事务之外
  （trailing 记录），未清理完的部分由 `continue_post_commit_cleanup`（**§11.2 步骤 1.5**；原文写的
  §10 是"RESTORE 审计"，不含这个话题）续传。

### 1.4 WAL 语义

- **I-9**: WAL（`transaction.log`）是修复日志，不是备份。批次成功提交后，`trim_completed` 删除该批次的全部日志行（**例外**：该批次的 post-commit 清理还没做完、盘上仍有它引用的 stash/备份时，整个文件保留，见 §12.1）。
- **I-10**: 批次成功后所有权/DB 状态转接给新包，旧版本的 `.lpkg_db_bak` 和 `.lpkg_bak` 不再需要。跨批次不保留"旧版本撤回"能力。

### 1.5 DB 状态确定性

- **I-11**: 每个 DB 写入（`DB`/`DBNEW` WAL 行）注明 **"这次写是在哪个包的安装/移除之后"**。格式：`DB <path> <pkg>:<state>`。
- **I-12**: 回滚时，通过 DB 里程碑链式恢复到 `:batch-start`。每个 `DB` 条目指出"此时系统在 X 安装完成后的状态"，逆序恢复时还原到前一个里程碑。

### 1.6 fsync 纪律

- **I-FSYNC-1**: WAL 每行追加后立即 fsync。单行 < 4096 字节在 O_APPEND 模式下 + fsync 保证该行已持久化。
- **I-FSYNC-2**: DB 文件写入顺序：WAL → fsync WAL → 备份原文件 → fsync 备份 → 写 .tmp → fsync .tmp → rename .tmp → fsync 父目录。
- **I-FSYNC-3**: 文件 rename 后调 `fsync_parent_dir()`（确保目录元数据落盘）——但**默认模式下它什么都不做**：`fsync_parent_dir()` 的唯一实现 `fsync_dir_internal()` 就是开关的两个分支点之一，只有落在 `DurableFsyncGuard` 块内时才是真的落盘（见下方"受开关影响"段）。
- **I-FSYNC-4**: `.lpkg_bak` 文件 rename 后调 `fsync_parent_dir()`——与 I-FSYNC-3 是**同一条原语**（`safe_rename()` 的尾巴），因此同样受默认关闭的开关影响：备份搬进 stash 的 rename 不在任何守卫块里，`--fsync` 下才真的落盘。
- **I-FSYNC-5**: 所有 write_set_file/write_db_file 使用 .tmp + fsync + rename 模式。

> **I-FSYNC-1 与 I-FSYNC-2 的前两步（WAL 行 + fsync WAL）在任何模式下都成立**：
> WAL 行的**三条**写入路径全部无条件 fsync —— `wal::log_wal_line`、`WalWriter::log`、
> `wal_append_raw`（回滚审计行）。行的持久化是"行 = 一个已开始但可能未完成的操作"
> 这条不变量的前提，丢了行会出现"操作做了、行没了"这种不可恢复组合。`WalWriter` 曾把
> 自己的 fsync 挂在 `durable_fsync_enabled()` 上（默认关闭时静默不持久）—— 那是"实现
> 弱于契约"，已去掉：要非持久的快路径请**显式**用 `WalWriter::log_no_fsync()`。
>
> **WAL 文件自身的目录项 fsync 同样是恒生效的；"只在本次真的创建了文件时才做"这半句
> 只适用于两条路径**：`wal::log_wal_line` 与 `wal_append_raw` 走
> `wal::open_wal_append()`（`O_CREAT|O_EXCL` 原子判出是否新建），只有新建时才
> `fsync_parent_dir()` —— 目录项只有创建那一刻需要落盘，而它们**每写一行**都走一次，
> 恒做等于每条 WAL 行白付一次父目录 fsync（实测占全部 fsync 的 34%~40%）。
> **`WalWriter` 构造是例外**：它走裸 `::open`（**无** `O_EXCL`、没有 created 判定），并且
> **无条件** `fsync_parent_dir()`（每批一次，代价可忽略）。
> ⚠️ 订正 2026-09-26：原文写"三条路径都用 `open_wal_append`、只有新建时才做"，
> 与紧随其后的"`WalWriter` 保持原样（无条件）"自相矛盾 —— 按代码写准。
>
> **DB/元数据写（I-FSYNC-2 的 .tmp → fsync → rename → fsync 父目录、I-FSYNC-5）同样
> **"这个名字空不空"一律用 lstat 语义**（2026-09-26 第三轴审计补）：`write_string_file_wal` 的
> `is_new = !fs::exists(p)` 曾用**跟随**语义 ⇒ 一个**悬空符号链接**被当成"没有旧内容" ⇒ 写
> `DBNEW`（**无备份**）⇒ 回滚的 `Undo::RemoveFile` 把新文件 unlink，**原来的悬空链接永久消失**
> （而不是"还原"）。改成 `exists_no_follow` 后，悬空链接算"名字被占" ⇒ 走有备份的那一支。
>
> 始终成立**：`Cache` 的四个 DB 写函数与 `wal::write_string_file_wal` 都带
> `DurableFsyncGuard`（`base/utils.hpp`）。理由是不可恢复窗口：备份
> `.lpkg_db_bak_before:*` 在批次提交后立刻被 `cleanup_db_backups()` 删掉，若新库只
> rename 到页缓存，断电就落在"库空/截断 + 唯一备份已删"上（`files.db`/`pkgs` 是单文件，
> 丢了就是全库所有权归零）。每里程碑只多约 5 次 fsync。（对照：libalpm 从不 fsync、
> local DB 原地 `fopen(w)` 覆盖、无 tmp+rename、无备份 —— 这本就是 lpkg 领先它的地方。）
>
> **受 `durable_fsync_enabled()` 开关（默认关闭，`lpkg --fsync` 打开）影响的是
> "批量文件数据"及其**目录项****，分支点只有写入层的两个原语：
>
> - **内容 fsync**：包内容 / `.lpkgtmp` / `.lpkgnew` 的 `::fsync`（**两处**显式的
>   `if (durable_fsync_enabled())` 块：`installation_task_copy.cpp` 的 `stage_regular_file()`
>   —— 普通文件与 `.lpkgnew` 两个落点共用这一份；`installation_task_register.cpp` 的
>   `install_hook_files()` —— hook 脚本的 `.lpkgtmp`），以及 `fsync_and_rename()` 里那一次
>   .tmp fsync（安装/升级路径上它的调用点都在守卫块内，今天只有 builder 的
>   `write_string_to_file()` 会落进未受保护的分支）。
>   （订正 2026-09-26：原文把这两处都指成 `installation_task.cpp` —— 该文件已按趟拆成 4 个
>   TU，这两个块分别落在 `_copy.cpp` 与 `_register.cpp`。）
> - **目录项 fsync**：`fsync_dir_internal()` —— 它是 `fsync_parent_dir()` 的**唯一**实现，
>   而 `fsync_parent_dir()` 是 `safe_rename()` 的尾巴，**每一次 rename 都走它**。所以
>   "rename 后 fsync 父目录"在默认模式下**一律不发生**（`.lpkg_bak` 搬进 stash、COPY 落位、
>   符号链接落位、DB 备份 rename…… 全部如此，这就是 I-FSYNC-3 / I-FSYNC-4 的真实强度），
>   **只有**落在 `DurableFsyncGuard` 块里的那几处例外：WAL 的三条打开/创建路径、以及
>   DB/元数据写内部（见上一段）。**别把它读成"只有大文件的 fsync 被关掉"**——目录项这一半
>   是全局的。
>
> 默认模式保证的是：每个操作仍是**一次 rename 或一次顺序 write**，
> 进程被 kill / 崩溃不会看到半写的行/文件，因此 WAL 的不变量与回滚/恢复语义**不变**；
> 断电则可能丢掉**已安装文件的数据本身**（重装即可修复，不涉及状态不可恢复）。
> 代价对比：默认模式每个**文件**省下"内容 fsync + rename 父目录 fsync"（典型 2 次，
> 目录项共享时更少），`--fsync` 才付这笔；WAL 行与 DB 写的 fsync 两种模式都在付
> （200 文件安装：WAL 行本身就有 400+ 次）。
>
> （2026-09-25 订正：原文说开关只影响"包内容/.lpkgtmp/.lpkgnew 的内容 fsync 与**这些文件**
> rename 的父目录 fsync"，从而让 I-FSYNC-3 读起来像恒生效 —— 实际 `fsync_dir_internal` 是
> **所有** `fsync_parent_dir` 的实现，默认模式下一次目录项 fsync 都不发。只改描述，不改行为。）

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
| **文件操作** | `BACKUP <src> → <dst>` | 覆盖/接管已有文件：rename 进 stash（write-ahead） | 写后 fsync |
| | `NEW <path>` | 新文件（日志记录，非实际创建） | 写后 fsync |
| | `NEW_DIR <path>` | 新目录（日志记录） | 写后 fsync |
| | `COPY <src> → <dst>` | `.lpkgtmp` → 目标路径 | 写后 fsync |
| | `REMOVE_OLD <src> → <dst>` | 升级时旧版废弃文件移除（同样 rename 进 stash） | 写后 fsync |
| | `UNSTASH <bak> → <orig>` | 把 stash 里那份**搬回原位**（第③步引入）；逆操作 = 再搬进 stash | 写后 fsync |
| | `SAVE_CONF <src> → <dst>` | 配置文件改名保留（`dst = <src>.lpkgsave`，**不进 stash**）。三个入口：移除整包时（可被 `--purge-config` 改成真删）、升级时 `/etc` 条目**类型变化**、升级时 `/etc` **废弃条目**（文件/符号链接） | 写后 fsync |
| **目录状态** | `DIR_META <path> <mode> <uid> <gid>` | 目录元数据的**改前**值（write-ahead：先写行再 `lchown`/`chmod`） | 写后 fsync |
| （2026-09-26 新增） | `XATTR_SET <path> <b64_key> <b64_old>` | 该键**本来有值**、即将被覆盖或删除 —— 行里是**改前**值 | 写后 fsync |
| | `XATTR_NEW <path> <b64_key>` | 该键**本来不存在**、即将被写入 | 写后 fsync |
| | `DIR_RM <path> <mode> <uid> <gid>` | 删除一个**空目录**（`rmdir`；元数据供回滚重建）。目录若带 xattr 则**先逐键写 `XATTR_SET`**（见 §3.6） | 写后 fsync |
| **移除操作** | `RM_BEGIN <pkg> <ver>` | 移除开始 | 写后 fsync |
| | `RM_COMMIT <pkg> <ver>` | 移除提交 | 写后 fsync |
| | `RM_END <pkg> <ver>` | 移除结束 | 写后 fsync |
| | `CLEANUP <path>` | stash 根清理记录（不可回滚） | 写后 fsync |
| **DB 操作** | `DB <path> <pkg>:<state>` | DB 文件修改后状态 | 写前已备份 + fsync |
| | `DBNEW <path> <pkg>:<state>` | DB 文件新建后状态 | 写前已备份 + fsync |
| | `DBRM <path> <pkg>:<state>` | DB 文件删除后状态 | 写前已备份 + fsync |
| **回滚审计** | `RESTORE_FILE <bak> → <orig>` | rollback：从 .bak 恢复文件 | 写后 fsync |
| | `RESTORE_DB <bak> → <db>` | rollback：从 .db_bak 恢复 DB | 写后 fsync |
| | `RESTORE_DIR <path>` | rollback：按 `DIR_RM` 行里的元数据重建目录 | 写后 fsync |
| | `RESTORE_FILE_RM <path>` | rollback：删除新文件（`COPY`/`NEW` 的逆操作） | 写后 fsync |
| | `RESTORE_DIR_RM <path>` | rollback：删除新建的目录（`NEW_DIR` 的逆操作） | 写后 fsync |
| | `RESTORE_DB_RM <path>` | rollback：删除新建的 DB（`DBNEW` 无备份时的逆操作） | 写后 fsync |
| | `RESTORE_DIRSTATE <path>` | rollback：`DIR_META` / `XATTR_SET` / `XATTR_NEW` 三种逆操作**共用**的审计行 —— 它们记的是同一个对象（目录状态）的三类改前值，分三个关键字只让词汇表变大 | 写后 fsync |
| **旧名称（只解析）** | `REMOVE_FILE <path>` / `REMOVE_DIR <path>` | `RESTORE_FILE_RM` / `RESTORE_DIR_RM` 的历史名：**仍能解析**（`walop_type_from_name`），但当前代码**不再写入**（`wal_op.hpp` 的 `WALOpType` 注释与 `wal_op.cpp` 的 `TYPE_MAP`） | 写后 fsync |

上表是**当前可解析的全部行前缀 = 33 个**（`wal_op.cpp` 的 `TYPE_MAP`，不含只作哨兵的
`INVALID`）。**数的是前缀个数，不是表格行数** —— `REMOVE_FILE` / `REMOVE_DIR` 那一行装了
**两个**前缀（订正 2026-09-26：本句原先写"28 个"，那是数了**表格行**）。
2026-09-25 订正：原文只列 24 行、且把 `REMOVE_FILE`/`REMOVE_DIR` 当成现行
的写出行（实际是只解析的旧名），并漏掉了 `DIR_RM` 与三条 `RESTORE_*_RM`。
2026-09-26 新增 4 个：`DIR_META` / `XATTR_SET` / `XATTR_NEW`（目录状态的**改前**值，
见 §3.8）与审计行 `RESTORE_DIRSTATE`。
（示例章节里 `BACKUP <src> → <dst>` 的 `dst` **落点**从简写成原位兄弟名；实际备份落在
每文件系统 stash `<fsroot>/.lpkg_bak_<pkg>_<pid>/`，见 §3.6、§3.6.1 第 8 条。）

> 表里的"写后 fsync"是**行内容**的 fsync（`::fsync(fd)`），**恒生效**、不受
> `--fsync`/`durable_fsync_enabled()` 影响（I-FSYNC-1）；三条写入路径
> （`wal::log_wal_line`、`WalWriter::log`、`wal_append_raw`）一视同仁，见 §1.6。
> **WAL 文件自身的目录项**（父目录 dentry）只在"本次调用**创建**了该文件"时才多一次
> `fsync_parent_dir()`（`wal::open_wal_append` 用 `O_CREAT|O_EXCL` 原子判定）—— 那也是
> 恒生效（套 `DurableFsyncGuard`），但**不是每行都做**：文件已存在时目录项早在别处落过
> 盘，每行再 fsync 一次父目录纯属白付（实测占全部 fsync 的 34%~40%）。
>
> ⚠️ **订正 2026-09-26：上面这条"只有新建时才做"只适用于两条路径，不适用于 `WalWriter`。**
> 三条写入路径里 ——
> · `wal::log_wal_line`（`transaction_log.cpp`）与 `wal_append_raw`（`wal_op.cpp`）走
>   `open_wal_append()`（`O_CREAT|O_EXCL` ⇒ 原子判出是否新建）⇒ **只有新建时**才多一次父目录 fsync；
> · **`WalWriter` 构造走的是裸 `::open(O_WRONLY|O_APPEND|O_CREAT|O_CLOEXEC)`**（无 `O_EXCL`、
>   没有 created 判定），并且**无条件** `fsync_parent_dir()`。原文那半句写在同段里紧跟着
>   "`WalWriter` 构造每批只走一次，保持原样（无条件）"，**自相矛盾** —— 现按代码写准。
> 两者共同的、真正恒成立的那一半是：**套 `DurableFsyncGuard`、不受 `durable_fsync_enabled()`
> 影响**。

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
RESTORE_FILE_RM /usr/bin/b           ← fsync
DB /var/lib/lpkg/pkgs :batch-start    ← 备份后 + fsync
ROLLBACK A 1.0                        ← fsync
END A 1.0                             ← fsync
COMMIT_PKGS                           ← fsync
```

**关键点**: 回滚操作（`RESTORE_DB`、`RESTORE_FILE`、`RESTORE_FILE_RM`…）也写进 WAL —— 但它们是
**审计痕迹**，不是恢复的输入：重启后的 `recover_packages()` 一律**跳过** `RESTORE_*` 行（`skip_in_reverse()`），
改用"备份/目标还在不在"的幂等判据继续逆向（例如 `RESTORE_DB` 已消费了那份备份，对应 `DB` 正向行的
逆操作就自然是 no-op）。详见 §10.2 / §10.3。

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
DB /var/lib/lpkg/pkgs :batch-start      ← 回滚把 6 个库**各写一条**，milestone **恒为**
DB /var/lib/lpkg/holdpkgs :batch-start     `:batch-start`（batch_rollback 第 4 步 =
…（provides.db / files.db / confhashes.db 同）  `cache.write(":batch-start")`）
ROLLBACK curl 8.11.1                   ← fsync
END curl 8.11.1                        ← fsync
COMMIT_PKGS                           ← fsync
```
> 订正 2026-09-26：原文这一行写的是 `DB /var/lib/lpkg/pkgs curl:installed ← 恢复到安装状态`
> —— **代码永远不会写那一行**（回滚期的 milestone 只有 `:batch-start` 一个值；`curl:installed`
> 是**正向** DB 行的形态）。另外：本批若有 `DB`/`DBNEW`/`DBRM` 行，它们的逆操作会先产出
> `RESTORE_DB <path>.lpkg_db_bak_before:<milestone> → <path>` 审计行（本示例的批次里没有 DB 行，
> 故不出现）。顺序按 `batch_rollback()` 的实现：`reverse_execute` → `purge_consumed_stashes`
> → `cache.write(":batch-start")` → 逐包 `ROLLBACK`/`END` → `COMMIT_PKGS`。

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
RESTORE_FILE_RM /usr/lib/libfoo.so.2
DB /var/lib/lpkg/pkgs :batch-start
ROLLBACK libfoo 2.0
END libfoo 2.0
COMMIT_PKGS
```

### 2.5 RESTORE 审计行在二次回滚中的作用

> ⚠ 本节标题里的"作用"要按现行实现读：RESTORE 行**不参与恢复决策**（一律被跳过，§10.3），
> 它们的价值是**审计与取证**；下面的例子演示的是"**靠正向行的备份是否还在**收敛"这个过程。
> 2026-09-25 订正前的原文把 3/4 两步写成"遇到 RESTORE_DB 就跳过 / 遇到 BACKUP 就还原"，
> 读起来像按行类型分流 —— 实际分流只由 `skip_in_reverse()` 做一次。

假设 rollback 中途崩溃：
```
BEGIN_PKGS
... (install A, B fails) ...
RESTORE_DB /var/lib/lpkg/pkgs.lpkg_db_bak_before:A:installed → /var/lib/lpkg/pkgs
── 断电 here ──
```

重启 `recover_packages()`：
1. 读 WAL，看到 `BEGIN_PKGS` 无 `COMMIT_PKGS` → 存在未提交区域（§11.2）
2. 逆序处理所有行；**`RESTORE_*` 行整类跳过**（它们是上次回滚的审计痕迹，不是恢复的输入）
3. 遇到正向行 `DB /pkgs A:installed`（其逆操作要消费 `.lpkg_db_bak_before:A:installed`）：
   - 那份备份已被上次回滚的 `RESTORE_DB` 消费（rename 回了 `/pkgs`）→ 不存在
   - → 跳过（幂等）
4. 遇到正向行 `BACKUP /usr/bin/a → /usr/bin/a.lpkg_bak_A`：
   - 备份存在（上次还没还原到它）→ rename 回 `/usr/bin/a` → 恢复旧文件
5. 写 `COMMIT_PKGS`（`wal::commit_batch()`）

结果是幂等的：无论 rollback 在哪个步骤崩溃，下次恢复能得到一致状态 —— 靠的是**每条逆操作的
"备份/目标还在不在"自查**，而不是解析 RESTORE 行。

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
清理孤立的 `.lpkg_db_bak_before:*`（比如在步骤 3 完成但 WAL 步骤 1 未持久化时产生的孤备份、
或批次已提交但 post-commit 清理还没跑就崩了的残留）。**当前实现在 `db/recover.cpp`**：

- **先过门控**：`wal_has_unpaired_batch()` 为真（WAL 里还有未配对的 `BEGIN_PKGS`）→
  **直接 return，一个都不删**。这些备份是"重试还原 DB"的唯一依据，少了这道闸，
  `main_cli.cpp` 的 `lpkg rec` 分支（`recover_packages()` 之后**无条件**再调一次本函数）会把
  恢复失败的还原点删光，CLI 还照打"恢复完成"。
- 然后**递归**扫 `state_dir()` 与 `docs_dir()` 两个根（man 备份写在 `docs/`，它在
  state_dir 之外，漏扫会让每次安装/升级都残留 `*.man.lpkg_db_bak_before:*`），删除文件名里
  含 `.lpkg_db_bak_before:` 的项。
- **调用点四处**：`finish_committed_batch()`（提交后收尾）、`run_batch_transaction` 的
  "回滚成功"分支、`recover_packages()`（没有未完成批次时，以及全部批次都恢复成功时）、
  `main_cli.cpp` 的 `lpkg rec` 分支。

（2026-09-25 订正：本节原先是设计笔记口吻——"需要一个 `cleanup_db_backups()`……实现需要重写
——旧实现已被删除"，且没提门控。函数早已落地，此处改为描述现行行为。）

### 3.2 文件 BACKUP 操作（install/remove 备份）

**当前顺序（write-ahead）**——实现在 `detail::OpSink::backup_impl()`
（`main/src/pkg/op_sink.cpp`，`backup()`/`backup_obsolete()` 共用同一实现、只有 WAL 关键字不同）：

```
 1. WAL：写入 BACKUP file → <stash>/file.lpkg_bak_<pkg>_<rand>
 2. fsync：WAL 行（恒生效，§1.6）
 3. rename file → <stash>/file.lpkg_bak_<pkg>_<rand>
 4. fsync：备份父目录 —— `safe_rename()` 的尾巴；**默认模式下这一步不生效**（§1.6）
```

**断电分析**：
- 1-2 之间：WAL 行未持久化，文件未改。**安全**（什么都没发生）。
- 2-3 之间：WAL 有记录、文件仍在原位。`reverse_execute` 遇到 BACKUP 条目，检查 `op.arg2`
  （备份路径）是否存在 —— 不存在 → 跳过，原文件还在原位。**安全**。
- 3-4 之间：WAL 有记录、文件已 rename 进 stash。恢复时检查备份存在 → rename 回原位。**安全**。

**结论**：文件操作是 write-ahead 顺序 —— WAL 先于实际 rename。行的含义是"这次备份**已承诺
会发生**"，所以"行在、备份不在"时回滚侧一律当作"还没做"（跳过即幂等），不存在"文件被搬走
了、WAL 里却没人知道"的窗口。

> **历史（已被本节自身推翻，勿再照做）**：本节原先描述的是"先 `rename`、后写 WAL"的顺序，
> 并自己推导出该顺序的漏洞 —— 崩在两者之间时文件已进了 `.lpkg_bak`、WAL 里却没有 BACKUP
> 条目，恢复引擎无从知道这个文件存在（原文件永久留在 bak 里）。当时的结论就是"文件操作也
> 用 write-ahead"，现已落地为上面那份顺序；旧推演段保留在此仅作记录。
> （另：旧文里 `.lpkg_bak_<pkg>` 是**原位兄弟名**，现行落点是每文件系统 stash
> `<fsroot>/.lpkg_bak_<pkg>_<pid>/`，见 §2.1 表下的注与 §3.6.1 第 8 条。）

### 3.3 COPY 操作（install 复制文件）

```
 1. copy src → dst.lpkgtmp  （+ xattr/属主/权限）
 2. fsync：dst.lpkgtmp      —— `installation_task_copy.cpp` 的 `stage_regular_file()` 里显式的
                              if 块，**受 --fsync 开关控制**（订正 2026-09-26：原文指成
                              `installation_task.cpp`，该文件已按趟拆成 4 个 TU）
                                （默认不 fsync；§1.6）
 3. WAL：写入 COPY src → dst  （write-ahead：`OpSink::commit_copy` 内）
 4. fsync：WAL 行（恒生效）
 5. rename dst.lpkgtmp → dst
 6. fsync：dst 父目录       —— `safe_rename()` 的尾巴，**默认模式下不生效**（§1.6）
```

**断电分析**：
- 1-2 之间：.lpkgtmp 可能不完整。WAL 无记录。重新执行即可（下次安装用
  `overwrite_existing` 覆盖同名 `.lpkgtmp`；崩溃后的普通文件残留是这套原语的正常形态，
  `refuse_symlink_tmp_path()` 只拒绝**符号链接**形态的 tmp 路径）。
- 2-3 之间：完整 .lpkgtmp，WAL 无记录。此时回滚不认识它 —— 但落点只是 tmp，没有正式文件
  被改，整批回滚照常进行。
- 3-5 之间：WAL 有记录、dst 还没落位。回滚侧的 COPY 逆操作是**删除 dst**（undo，不是
  redo），dst 不存在时是 no-op；同时它还会顺手清掉 `op.arg1` 指向的 `.lpkgtmp`
  （`wal_op.cpp` 的 COPY 分支，专门收掉"COPY 行已写、rename 未做"的中间态）。
- 5-6 之间：已 rename，逆操作删掉 dst 即回到安装前。**安全**。

**结论**：COPY 是 write-ahead，"WAL 行 + 目标 rename" 由 `OpSink::commit_copy` **一次调用**
完成，调用点不可能把顺序写反（见 §3.6.1 第 8 条）。

### 3.4 NEW 操作（install 新文件）

NEW 只是一个 WAL 日志记录，文件实际由 COPY 创建。所以 NEW 的逆向非常简单：
```
reverse_execute 遇到 NEW：
  → 删除 op.arg1（文件路径）
  → 如果存在则删除，不存在跳过
```

NEW 的 WAL 写入时机：在**备份阶段**（`backup_existing_files()`）当检测到目标路径不存在时写入，
早于 COPY。另外两处也会写 `NEW`：`copy_package_files()` 的**符号链接条目**分支（落位新链接前
写 `NEW <dest>`；若 dest 已被占用，则先 `BACKUP` 让开 —— 行序 BACKUP → NEW 不可反），以及
`install_hook_files()` 的 `hooks_dir/<pkg>/` 目录本身（`NEW_DIR`）。

### 3.5 NEW_DIR 操作

同样只是日志记录（目录由调用方的 `create_directories` 落位）。逆向：
```
reverse_execute 遇到 NEW_DIR：
  → 路径先剥尾斜杠（否则 is_symlink 恒假）
  → 非 symlink && 存在 && 是真目录 && 为空 → rmdir + RESTORE_DIR_RM
  → 其余情况一律跳过（不删）
```
（订正 2026-09-25：原文只写"删除目录（仅当为空时）"，漏了"剥尾斜杠 + 非 symlink + 真目录"
三道守卫；`wal_op.cpp` 的 NEW_DIR 分支有它们，回归测试见
`tests/unit/test_wal_newdir_symlink_guard.cpp`。）

### 3.6 目录的处置（取代旧的 RM_DIR / "目录 BACKUP"）

**现行算法（remove 与 upgrade 共用同一套阶段，没有"整目录备份"这回事）**：

1. **文件先走**：目录里属于本包的**文件/符号链接**先逐个 rename 进每文件系统 stash
   （`OpSink::backup*`：`REMOVE_OLD` 或 `BACKUP`），目录因此**变空** —— 备份不再原位占位。
2. **目录后删**：目录走**独立阶段、最深优先**（子目录先删，父目录才可能空），判据三条 ——
   **仅最后持有者**（`cache.get_file_owners(key)` 摘掉本包后为空）、**盘上是真目录**
   （`lstat` 语义，`!fs::is_symlink`）、**此刻为空**（`fs::is_empty`）。三条都过才调
   `OpSink::remove_empty_dir()`：WAL `DIR_RM <path> <mode> <uid> <gid>` → `rmdir`。
   **目录若带 xattr，每一键先写一行 `XATTR_SET`（2026-09-26 补）**：`DIR_RM` 只带
   mode/uid/gid，而回滚的 `RESTORE_DIR` 只重建目录 + 恢复元数据 ⇒ 重建出来的目录会**丢掉整份
   xattr**（而目录 xattr 正是 POSIX ACL 与 SELinux 标签的存放处）—— 与当初为目录元数据补
   `DIR_META` 是**同一族缺口**，只是长在**移除侧**（属性测试的 xattr 维度实测命中 2/32 种子）。
   **行序是承重的**：`XATTR_SET…` 必须在 `DIR_RM` **之前** —— 逆序回滚才会"先重建目录、
   **再**写回各键"，正好落在重建好的目录上；反了就会打在还不存在的路径上、被
   `Guard::RealDir` 判否跳过 ⇒ **行写了、值没回来**（比不记还坏）。
   **只记行、不动盘**（目录马上要 rmdir），且**没有 xattr 的目录写零行** ⇒ 绝大多数目录移除的
   WAL 与改动前逐字节相同。
   **最深优先的判据是"路径字符串长度降序"，不是字典序倒序** —— 子孙恒长于祖先，长度序才能
   保证子目录先删；字面倒序不保证（`/a/b` > `/a/zz`，但前者是祖先）。

   - remove 侧：`package_manager.cpp` 的 `do_remove_package` **阶段 A**（文件）/ **阶段 B**（目录）。
   - upgrade 侧：`installation_task_letgo.cpp` 的 `remove_obsolete_files()`：阶段 1
     （`obsolete_files_pass`，废弃文件）/ 阶段 2（`obsolete_dirs_pass`，废弃目录，内部按路径
     长度降序排序 = 最深优先）。**它跑在写入之前**（让开趟的②b），见 §6.2。
     （订正 2026-09-26：原文写"`installation_task.cpp` 的 `commit_without_file_ops()` 阶段 1/2"
     —— 文件与函数名都已移动：`commit_without_file_ops()` 现在只有注册 + hooks 两件事。）
   - **upgrade 侧专有的第四道守卫（订正 2026-09-26 补）："新版本在该目录下仍有条目 → 跳过"。**
     因为这一趟现在跑在**写入之前**，而"此刻为空"的原判据（跑在写入**之后**时）会因新版本
     刚写进去的内容而非空 ⇒ 原口径下这种目录本就被跳过；提前看它还是空的，会被 `rmdir` 掉、
     紧接着又被写入趟重建（丢原 mode/uid、白写两条 WAL）。所以显式排除
     `new_set` 里以 `<废弃目录键>` 开头的条目（`obsolete_dirs_pass`）。**remove 侧没有这一条**
     （那里没有"新版本"）。
3. **回滚**：`DIR_RM` 的逆操作是 `RESTORE_DIR` —— `create_directories` 重建 + **按行内
   mode/uid/gid 恢复元数据**（路径先剥尾斜杠；不是真目录就静默跳过、**不** chmod/chown）。
   ⚠️ **幂等口径是 `Guard::Always`，不是"已存在就跳过"**：目录缺失正是它要修的状态，所以
   即使目录**已存在**，只要还**是真目录**就照样 `lchown`/`chmod` 写回行内值并计一次
   `RESTORE_DIR`（回滚路径上"目录已存在"反而是常态 —— 写入趟的 `ensure_dir_exists` 会把它
   建回来）。只有"不是真目录"（symlink／解不开）才判否、跳过。
   与"整树 rename 回来"的旧模型相比，回滚不再需要逐层 rename，也不存在"备份目录里混进
   别人文件"的可能。

**"非空即保留"就是安全边界**：任何一个直接子项不属于本包（其他包的文件、被 `.lpkgsave`
保留的 conffile、lpkg 自身状态目录如 `usr/share/lpkg/docs`、任何**无主**内容）都会让
`is_empty` 判假 → 整个目录保留、一个字节都不动。宁可在叶目录里留一个无主空壳，也绝不碰
不属于本包的东西。挂载点是唯一由 `remove_empty_dir()` 自己挡下的例外：`rmdir(2)` 对挂载点
恒返回 EBUSY，因此在写 WAL **之前**就判掉（`is_mount_point`）并报告
`SkippedMountPoint`，由调用方告警保留（对齐 pacman 对目录型 mountpoint 的语义）。

> ⚠ **历史形态（已不存在，别再照做）**：本节原先描述的是"逐目录整树 `rename` 成单个
> `.lpkg_bak`"，函数 `detail::backup_dir_tree_whole(dir, pkg, op, backups)`，配一套"目录此刻
> 必须是**纯本包残留**"的判据（每个直接子项的文件名都带本包 `.lpkg_bak_<pkg>_` 标记）。
> 该函数**已从全仓删除**（`pkg/install_common.{hpp,cpp}` 里没有它，`grep` 只剩本文档与
> `tests/integration/test_upgrade_obsolete_dir_cleanup.cpp` 的一处头注释，那处注释同样待订正）。
> 按本节旧描述去改代码会重新引入"整树 rename 掉共享祖先目录"的老缺陷。
> **旧模型为什么被换掉**：整树 rename 依赖"直接子项全是本包 bak"这个判据，而它又依赖
> "本包文件已全部先 rename 成 bak"；**升级**那段当时把目录删除混进了文件循环 + 单层
> `is_empty` 判空 —— 刚 rename 的 bak 正占着目录，判"非空"必被跳过 → 嵌套第 2 层 owned
> 目录（如 `dist-info/licenses/`）永远删不掉，下游构建一探测 site-packages 的 dist-info 就崩。
> 换成"文件搬进 stash（不在原目录占位）→ 目录天然空 → 最深优先 rmdir"之后，这个嵌套空壳
> 问题**从机制上**消失（`tests/integration/test_upgrade_obsolete_dir_cleanup.cpp` 钉住）。

#### 3.6.1 目录与符号链接的判定（对齐 pacman，2026-09 起）

事故背景：发行版布局 `/var/run -> ../run`，某包归档里带实体 `var/run/` 目录条目，
reinstall 后符号链接被 rename 进 stash、再建出实体目录（系统布局被毁）。

采用的判据（逐条对应 libalpm 的实现与回归测试）：

1. **判"盘上是什么"一律 `lstat`（不跟随末段），且必须先剥尾斜杠**
   （`strip_trailing_slash()`，`base/utils.hpp`）。pacman 侧这是**两件事**，别混：
   `llstat()` 在 `src/common/util-common.c`（尾斜杠剥离 + lstat，剥离来自 commit
   `bbeced26`，2014）；而 **FS#51377 修的是 `remove.c` 的 `unlink_file` 在判类型前自己
   剥尾斜杠**（commit `0fd8455c` / cherry-pick `16b91f79`，2017-04-09，随附测试
   `remove-directory-replaced-with-symlink.py`）。原因：尾斜杠要求"前一分量是目录"，
   于是**判定类调用全部落到链接的目标上** —— `is_symlink` 对 `/x/link/` 恒假（尾斜杠强制
   解引用）、`is_empty`/`exists`/`fs::is_directory` 读的都是链接**目标**的状态，判据错在
   **对象**上，决策随之错（"链接目标恰好是空目录"会被当成"我们建的那个目录空了"）。
   **订正（2026-09-25，以真实 syscall 实测为准）**：原文此处写"`rmdir` 也落到目标上、
   实测会把 `/var/run` 指向的真实 `/run` 删掉"—— **不成立**。Linux 上 `rmdir("link/")`
   与 `rmdir("link")` 都返回 **ENOTDIR**（内核拒绝，**不跟随**末段符号链接），
   `remove("link/")`（unlink 路径）同理；没有任何路径能让 `rmdir` 删掉符号链接的**目标**
   目录。真正会"穿过去"的是 **`chmod`/`lchown`**：`chmod("link/", 0700)` 成功且改的是
   目标目录的权限、属主。守卫**依然必要**，理由要按真实行为说：不剥尾斜杠时
   `NEW_DIR` 分支的 `rmdir` 是**静默 no-op**（该删的没删，而且连 `RESTORE_DIR_RM` 审计行
   都不写），而 `chmod`/`lchown` 会真真切切改坏**共享**目录（如 `/usr/lib`、`/run`）的
   属主/权限 —— 这也正是回滚侧（`reverse_execute` 的 `DIR_RM`/`NEW_DIR` 分支）与
   `OpSink::remove_empty_dir` 都先 `strip_trailing_slash()` 再判 `is_symlink` 的理由。
   DB 的目录键保持带斜杠（那是键，不是路径），**任何物理操作前先规范化**。
2. **symlink 一律算「非目录」**。"We do not support treating symlinks to directories as
   directories. They are considered a file."（pacman-dev）→ 不写穿链接、不把链接当目录。
3. **类型变更 = 冲突**（`collect_content_conflicts`，逐包检查与整批预检共用）：归档目录 `x/` 撞上盘上非目录
   （普通文件 / 符号链接，含 symlink→目录）→ 拒绝；归档文件 `x` 撞上盘上真目录 → 拒绝
   （pacman 的 case 4 / case 5，"not overwriting dir with file"）。
   **后者不是无条件的** —— 见第 4 条的豁免 (b)。整批中止、什么都不改。
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
4. **豁免有两条（订正 2026-09-26：原文写"**唯一**豁免"，漏了反方向那一条）**：
   **(a) 该路径由本包以另一形态持有**（旧版发文件/链接、新版发目录）→ 文件→目录升级
   （E4）照旧接管；对应 pacman 的 "Check if the directory was a file in dbpkg"。
   **(b) 盘上是真目录、归档是文件/符号链接（即第 3 条被拒的那一格）—— 当该目录整棵子树
   （递归）里的每个条目都属于"本包"或"本批次正在升级的包"时放行接管**：
   `dir_tree_entirely_ours()`（`installation_task.cpp`，对齐 pacman `conflict.c:
   dir_belongsto_pkgs`），判据是 `dir_takeover`，放行后**必须跳出**该路径后续的检查
   （**⚠️ 它算条目的 DB 键必须用 `lexically_relative`，不能用 `fs::relative`**（2026-09-26 第三轴
   审计修复）：后者**解析符号链接（含末段）** ⇒ 查的是**链接目标**的归属。后果一：树里有"别的包
   持有、解析目标归本包"的链接 ⇒ 判"整树都是我们的" ⇒ 整树搬走 ⇒ 提交后 stash 被 `remove_all`
   ⇒ **别人那份文件永久消失而它的 DB 归属还在原位**（正是这个函数存在的唯一意义所在）。后果二：
   中间段是链接（usr-merge `/bin`→`usr/bin`）⇒ 键查不到 ⇒ 判"无主" ⇒ 拒绝本可放行的升级。
   同仓库 `scan/scanner.cpp` 早已为同一问题改用 `lexically_relative`（"`fs::relative` 会解析符号
   链接…与登记的属主键对不上 → 假孤儿"）；现已复现端到端现场并钉住）
   放行后**必须跳出**该路径后续的检查
   （`if (dir_takeover) continue;` —— 尾部那条"路径级"检查按**无尾斜杠**的 `bare` 查归属，
   而目录在 DB 里的键**带尾斜杠**，不跳出会把刚放行的接管又判成"无主手工文件"重新拒掉）。
   判否时必须点名**第一个不属我们的条目**的持有者（无主则 `error.unknown_manual_file`）——
   否则报错会退化成"owned by package <本包自己>"这种无用的定位。
   **边界**：树里有**别的包**的条目 → **结构性必须拒绝**（整树搬走会让那个包 DB 里的归属
   当场脱节）；树里有**无主**文件（用户自己塞的）→ 现行选**拒绝**（pacman 也如此，升级
   卡住但零丢失）。
   ⚠️ **这条判据不许删**：删掉它等于允许整树搬走别人的/用户的文件 —— 静默数据丢失 +
   跨包所有权脱节（"反正都要先搬空"不是理由：先搬空正是靠这条判据才知道**有权**搬）。
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
   `save_config`（`SAVE_CONF` → rename 到 `<路径>.lpkgsave`，**不进 stash**；目标名被占用时
   还先做一次同类型的移位 rename）、`new_file`/`new_dir`（纯 WAL 记录，不碰文件系统）、
   `remove_empty_dir`（`DIR_RM` → `rmdir`）、`commit_copy`（`COPY` → rename）、
   `un_stash`（`UNSTASH` → rename 回原位；第③步引入，逆向见 §9.2）。
   **新增"文件操作 + 它的 WAL 行"一律加在这里**，不要回到调用点写成对语句。
   （订正 2026-09-25：原列表漏了 `save_config` —— 它同样是"WAL 行 + 物理 rename"成对的入口。）
   （补充 2026-09-26：`un_stash` 同属此列；并且**凡碰文件系统的方法都在内部 `strip_trailing_slash`**
   —— 这条契约现在 `save_config` 与 `un_stash` 也满足了。）
   （DB 一族 —— `DB`/`DBNEW`/`DBRM` 的 row + `safe_rename` —— 是**另一个**已文档化的入口点，
   分布在 `db/wal_op.cpp`、`db/cache.cpp` 与 `package_manager.cpp` 的 `cleanup_with_dbr`：
   它们没有 stash 记账、落点是与目标同级的 `.lpkg_db_bak_before:<milestone>` 兄弟名，
   形状与本层不同。要收拢它们得单开一层，别顺手塞进 `OpSink`。）

> ⚠ **历史形态的另一半（同样已不存在）**：旧模型里还曾有一条**递归判据** ——"子树里没有
> 其他包登记的内容即可删"。它会递归下探，把共享祖先下、包只作**普通目录持有者**的
> `usr/share/lpkg/docs` 等 lpkg 自身状态目录当"无主空壳"删掉（`make test` 多处回归：
> 保留 conffile / reinstall / SIGINT 三套用例一起炸）。**规则订正**：无主内容宁留不删 ——
> 现行判据只看**该目录自己是否为空 + 本包是不是它的最后持有者**，绝不递归下探（见本节开头的
> 三条判据）。升级侧原先另写一套"文件循环 + 单层 `is_empty` 判空"（就是嵌套 owned 目录删不掉
> 的来源），现在与 remove 侧共用同一套阶段 ——"最深的统一"从承诺变成事实。

### 3.7 DBRM 操作

```
 1. WAL：写入 DBRM <path> <milestone>
 2. fsync：WAL（恒生效，§1.6）
 3. rename file → file.lpkg_db_bak_before:<milestone>
 4. fsync：父目录 —— `safe_rename()` 的尾巴，**默认模式下不生效**（§1.6；只有落在
    `DurableFsyncGuard` 块内的 DB 写才真的落盘）
```

调用点：`package_manager.cpp` 的 `cleanup_with_dbr`（移除时清 dep/needed_so/man）。

**断电分析**：
- 1-2 之间：WAL 行未持久化。文件未改。
- 2-3 之间：WAL 有记录，文件未 rename。`reverse_execute` 找 `.lpkg_db_bak_before:<milestone>` 不存在→跳过。DBRM 的逆向是 restore 备份回原位。但备份不存在。→ 安全，文件还在原位。
- 3-4 之间：WAL 有记录，文件已 rename。恢复时找备份存在 → rename 回。**正确**。


### 3.8 目录状态的改前值（2026-09-26 新增：`DIR_META` / `XATTR_SET` / `XATTR_NEW`）

**为什么需要这三行**：目录是**就地改活对象** —— 与普通文件不同，没有"先写 `.lpkgtmp` 再
rename"那层保护（旧文件的 inode 被 `BACKUP` 搬进 stash 保住了，新文件是**另一个** inode）。
`write_dir_entry()` 与 `let_go_make_dir()` 直接对**活着的**目录调 `lchown`/`chmod`/
`copy_xattrs`，在 2026-09-26 之前**一个 WAL 行都不写** ⇒ 注入失败回滚后该目录保持**新**的
mode/uid/xattr，违反 §5.4 不变量 3（终态 == 事务开始时的盘面）。不是潜伏问题：任何"新版本改了
某个已存在目录的 mode"的批次失败都会踩到。

**做法**：三个原语在 `OpSink`（本仓库约定：新增"文件操作 + 它的 WAL 行"一律加在写入层），
语义统一为**"记录改前状态"**而非"记录正向动作"，且**先写行再动盘**（write-ahead）：

| 原语 | WAL 行 | 何时写 | 逆操作 |
|---|---|---|---|
| `OpSink::dir_meta(phys, mode, uid, gid, record_previous, …)` | `DIR_META <path> <mode> <uid> <gid>` | `record_previous` 为真时 | `lchown`+`chmod` 写回 |
| `OpSink::set_xattr(phys, key, val, …)` | 键**本来有值** → `XATTR_SET <path> <b64_key> <b64_old>`；**本来不存在** → `XATTR_NEW <path> <b64_key>` | 恒 | `lsetxattr` 旧值 / `lremovexattr` |
| `OpSink::unset_xattr(phys, key, …)` | `XATTR_SET`（行里是**待删那个键的旧值**） | 恒 | `lsetxattr` 旧值 |

四个刻意的决定：

- **`XATTR_SET` 同时承载"覆盖旧键"与"删掉旧键"**：两者的改前状态都是"有个值"、逆操作都是
  写回去。区分它们要多一个"删除前的旧值"行类型，而那个在回滚侧完全同形。
- **键与值都 base64**：xattr 键名与值是任意字节串（`system.posix_acl_default` 就是二进制），
  而 WAL 是行式、空格分帧、`" → "` 是箭头分界的**文本协议** —— 裸放一个含空格/换行/`→` 的键
  会把一行**重新分帧**成另一条合法行，回滚照着重构出的路径去 `lsetxattr`。**空值**用哨兵
  `-`（base64 字母表里没有它），不能用空字段 —— 那与"这一侧不存在"同形。
- **`Confine::Arg1Only`**（不是 `Arg1AndArg2`）：`arg2`/`arg3` 是 base64、**不是路径**；按两
  字段查会把一串 base64 判成越界路径，从而**整行被跳过** ⇒ 回滚静默少还原一次。
- **`record_previous` 的判据是"让开趟记下的原始事实"**（`facts->disk_exists && facts->disk_is_dir`
  = "让开之前它是个真目录"），**不是此刻的 `existed`** —— 此刻它可能正是让开趟刚建出来的。
  本批次刚建出的目录由 `NEW_DIR` 的逆操作（删除）收尾，记一行改前值纯属冗余。
  回退路径（只有测试直连才有，`facts == nullptr`）取 `true`：**保守地记**，宁可多一行冗余。

**边界（与相邻的元数据块同源）**：`write_dir_entry()` 与 `let_go_make_dir()` 在
`is_symlink_no_follow(probe) && is_real_directory(probe)` 不成立时**整块跳过** ——
`symlink→目录`（`/lib64 -> usr/lib`、`/var/run -> ../run`）时内容要**穿过**链接写进真实目录，
但目录条目的 uid/mode/xattr **不能**跟着穿过去（lchown/chmod/lsetxattr 在带尾斜杠时会落到
**链接目标**上，把**别的包持有的**目录改成包内值）。判据一律用**剥过尾斜杠**的 `probe`：
`fs::is_symlink("link/")` 会**跟随**尾斜杠（实测返回 false），用带斜杠的形态守卫会失效。

**窗口断点**：三个原语都收 `after_wal_breakpoint`，调用点传
`dirmeta_after_wal_<pkg>` / `xattrset_after_wal_<pkg>` / `xattrrm_after_wal_<pkg>`（断点清单见
§13 第 6 阶段 6.1）。三条窗口用例各自钉三件事：断点**真命中** / **WAL 里已有那一行** /
**盘面还没被改** —— 只钉前两件的话，"先改盘再补写行"的实现照样绿。


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

每次 `Cache::write(<milestone>)` 写的是 **6 个库**（订正 2026-09-26：原文没有这份清单，而
"5 个"的说法散在全文各处 —— `xattrkeys.db` 于 2026-09-26 加入）：`pkgs`、`files.db`、
`provides.db`、`confhashes.db`、**`xattrkeys.db`**（§6.3.1）、`holdpkgs`。
每个库各写一条**同里程碑**的 `DB` 行（`write_db_file_wal` / `write_set_file_wal`）。

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

**批次开头还有 6 条 `:batch-start` 条目**（订正 2026-09-26：原文写 5 条 —— `Cache::write()`
现在每个里程碑写 **6** 个库，`xattrkeys.db`（§6.3.1）加进来了）（`Cache::write(":batch-start")` 对
`pkgs`/`files.db`/`provides.db`/`confhashes.db`/`holdpkgs` 各写一次，见 `cache.cpp`）——
它们在逆序里排在最后（写得最早），跑到的判据见 §10.2：正式文件**仍在位且非空**（= 已被
更晚的各里程碑逆操作带回批次起点内容）就跳过；不在位则从该里程碑备份还原；
**"在位但为空"时要看备份**（订正 2026-09-26：原文只写了"在位且非空 → 跳过 / 不在位或为空 →
还原"，把四分支压成了两分支）—— **文件空 + 备份也空 → 同样跳过**（两者等价，还省下一条
`RESTORE_DB` 审计行）；只有"文件空 + 备份非空"才真的要从备份还原。

更长的链（下面都以 `pkgs` 一个库举例；实际每个里程碑是 6 个库各一条同里程碑的 `DB` 行）：
```
  DB /pkgs A:installed    .bak = :batch-start 内容
  DB /pkgs B:installed    .bak = A:installed 内容
  DB /pkgs C:installed    .bak = B:installed 内容

回滚到 :batch-start：
  逆序：
    1. DB /pkgs C:installed → .bak_before:C:installed → /pkgs  (→ B:installed 状态)
    2. DB /pkgs B:installed → .bak_before:B:installed → /pkgs  (→ A:installed 状态)
    3. DB /pkgs A:installed → .bak_before:A:installed → /pkgs  (→ :batch-start 状态)
    4. DB /pkgs :batch-start → 跳过（上面的判据：正式文件已在位）
    5. BACKUP、NEW 等恢复
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

在 batch_rollback 中（`wal_op.cpp`，顺序即实现顺序）：
  1. extract_current_batch_ops()   ← 提取行；**提取为空则直接 return false**（见 §9.1）
  2. reverse_execute(ops, true)    ← 恢复文件 + 恢复 DB 文件（每步写 RESTORE_* 审计行）
  3. purge_consumed_stashes(ops)   ← 文件已还原，清掉空 stash 根
  4. Cache::instance().load()      ← 重载内存 Cache，与磁盘一致
  5. cache.write(":batch-start")   ← 写恢复后的状态（6 个库各一条 DB 行）
  6. ROLLBACK/END（每个已回滚包）+ COMMIT_PKGS

（订正 2026-09-25：原文的步骤 2/3 之间漏了 purge_consumed_stashes，且没写"提取为空 →
return false、不写 COMMIT_PKGS"这条 —— 后者决定调用方能不能清理 DB 备份，见 §9.1。）

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
 *     ├── batch_rollback(success)         ← 回滚所有已成功包（返回 false = 没有可回滚的行）
 *     │     ├── extract_current_batch_ops()  ← 提取为空 → **return false，不写 COMMIT_PKGS**
 *     │     ├── reverse_execute(ops)      ← 逆向执行；每步写 RESTORE_* 审计行（fsync 恒生效）
 *     │     ├── purge_consumed_stashes()  ← 文件已还原 → 清掉空 stash 根
 *     │     ├── Cache::load()             ← 从磁盘重载恢复后的 DB
 *     │     ├── DB /pkgs :batch-start     ← 链式恢复后的最终状态（6 个库各一行）
 *     │     ├── ROLLBACK/END 标记         ← 每个已回滚包
 *     │     └── COMMIT_PKGS               ← 批次完结（回滚完成）
 *     ├── cleanup_db_backups() + trim_completed()   ← **仅在 batch_rollback 返回 true 时**
 *     └── rethrow
 */
template<typename OpT>
std::vector<std::string> run_batch_transaction(OpT&& op);
```

### 5.2 不变量（WAL / 批次级）

- 进入 `run_batch_transaction` 时它自己先 `trim_completed()`（函数第一行；WAL 无未完成事务是它的前提，main 启动时已 `recover_packages()`）。
- `BEGIN_PKGS` 写入 + fsync 后：异常路径保证 `COMMIT_PKGS` 一定被写入（catch 补写）——**例外有两条**（订正 2026-09-26：原文写"**唯一**例外是 `batch_rollback()` 返回 false"，漏了第二条）：
  1. `batch_rollback()` 返回 **false**（WAL 里提不出未提交批次的行，比如尾部被写坏）；
  2. **回滚自身抛异常** —— `reverse_execute` 里的 `safe_rename` 失败、`wal_append_raw` 写不进去等都会抛穿 `batch_rollback`，而 `batch_transaction.hpp` 为它写了**专门的 `catch (...)`**：绝不清理 DB 备份、不 `trim_completed`，同样把未提交批次与全部 `.lpkg_db_bak_before:*` / `.lpkg_bak` 原样留给下次 `recover_packages()` 幂等续传。这条分支的存在本身就说明设计上承认"回滚可能失败"（§11.2 的未提交区域定义也以"前一批回滚失败后又开了新批次"为前提）。
  两条都不写 `COMMIT_PKGS`、都不清 DB 备份 —— 这正是"宁可留现场，不可删重试依据"。
- `COMMIT_PKGS` 是批次完结的唯一标记——不区分"成功完结"和"回滚完结"。外部只看有无 COMMIT_PKGS。
- 回滚后：WAL 包含完整的 RESTORE 审计链，系统状态一致。

### 5.3 回滚触发条件

| 触发条件 | 回滚范围 | 路径 |
|---------|---------|------|
| 安装包中途失败 | 整个批次 | `InstallationTask::run()` 的 catch → 写 ROLLBACK/END 标记 → `batch_rollback()`（单撤销路径，§6.5） |
| 整批中后续包失败 | **整个批次**（与前一行相同） | `run_batch_transaction` 的 catch → `batch_rollback()` |
| Ctrl+C | 整个批次 | 检查点抛异常 → 同异常路径 |
| 致命错误 | 整个批次 | 同异常路径 |
| 断电 | 整个批次 | 下次启动 `recover_packages()` |

> 订正 2026-09-26：第 2 行原文写"**前序已成功包**"，那是把回滚范围写小了一档。`batch_rollback()`
> 拿的是 `extract_current_batch_ops()` —— **整批**的行（从批次 `BEGIN_PKGS` 到 EOF），
> `reverse_execute` 逆序处理**全部**行；`successfully_installed` 只用来决定给哪些包写
> `ROLLBACK`/`END` **标记**。失败包**自己**那半截文件/DB 操作（它的 `BACKUP`/`COPY`/`NEW`/`DB` 行）
> 同样必须被撤销 —— 而它的 `rollback_files()` 已经不碰文件系统（只写标记 + 清内存追踪，§6.5），
> 所以撤销完全依赖 `batch_rollback`。写成"前序已成功包"会让人以为失败包的半成品不被撤。
> （本条订正原先误插在表格中间、把表切成了两截，2026-09-26 挪到表后。）

### 5.4 路径级不变量（安装/升级；编号 1–5，与上面 §5.2 的批次级不变量是两套）

> **出处**：本节与 §5.5 原在 `REFACTOR-upgrade.md` §4 / §5，2026-09-26 随该文**拆解并入本文**
> （该文已删除）。下面 **1–5 的编号在代码与测试注释里被按号引用**（形如"不变量 4"）——
> **改号要连带改全仓引用**。

1. 每个被触碰的路径**恰好被决策表认领一次**（无遗漏、无重复）。`check_decision_invariants()`
   把它写成可执行断言（`op_sink.cpp`），模型层穷举 **576** 个事实组合
   （`tests/integration/test_upgrade_decision_table.cpp`）。
2. 无失败时，终态 == 新版本内容（逐路径、逐类型、逐字节）。
   **`/etc` 前缀例外**（且是有意的）：用户改过的配置被保留（`.lpkgnew` / 原样），**废弃的配置
   改名 `<路径>.lpkgsave`**（文件/符号链接；`/etc` **目录**仍原位不动）。所以属性测试断言
   "盘面 == 新版本形态"时**必须排除 `/etc` 前缀** —— 随机化属性测试已实证：`/etc` 的各种组合
   只能"记录行为"，不能要求它们满足一般形态。
3. 注入失败时，终态 == 事务开始时的盘面（逐路径、逐类型、逐字节）+ DB 回到旧版本号。
4. 用户改过的配置**永不静默丢失**（要么留在原位、要么 `.lpkgsave`、要么 `.lpkgnew`）。
5. 不属于本包的路径**永不被移动或删除**（含无主文件、其他包的文件、共享目录）。
   推论（判据见 §6.3 的整树让开许可）：要把一棵**目录树**整体让开/搬走，前提是那棵树里每个
   条目都属于本包或本批次升级的包；否则拒绝并点名**真实**持有者。

### 5.5 已明确的取舍

- **升级期间被置换的 `/etc` 配置会短暂不在盘上。** 实测规模：本机 **363** 个被包持有的
  `/etc` 文件；真正会被"进程启动时读"的是 `/etc/ld.so.cache`、`/etc/nsswitch.conf`（glibc）、
  `/etc/pam.d/*`（shadow/util-linux/linux-pam/cups）。失效方向均为 **fail-closed**
  （PAM 缺文件 → 认证失败而非放行），且 lpkg 全程不重启任何服务。这个窗口在让开趟前移
  **之前**就已存在于"用户没改过 → 静默换新版"那条路（同样先搬进 stash），前移只是把它扩展到
  "用户改过"的两种结果。**已拍板接受。**

> 订正 2026-09-26：原 `REFACTOR-upgrade.md` §5 还有第二条「**不引入**整树 rename
> （`backup_dir_tree_whole` 那种）」—— **未并入**：那个函数全仓**已不存在**（有测试注释专门
> 记过这一点），而移除侧的目录清除现在**确实**逐目录整树 rename 成单个 `.lpkg_bak`
> （见 §3.6）。把它搬进现行规范等于带进一条过期断言，故丢弃。


---

## 6. 安装流程

### 6.1 顶层流程

```
install_packages(args)
│
├── Cache::load()
├── TmpDirManager + Repo::load_index()
├── 解析参数 → targets
├── resolve_with_solver(ctx)                ← 依赖解析（libsolv）
├── 目标都落实了吗？（first_unreached_target）／用户确认
│
├── check_batch_file_conflicts(plan, order)  ← **整批文件冲突预检**：进入事务**之前**
│                                              把所有成员的 content 清单 + 当前所有权 +
│                                              本批次内的接管顺序一起算；判冲突即抛错中止
│                                              （一个文件都没动，WAL 里连 BEGIN_PKGS 都没有）
│                                              判定与逐包检查共用同一份语义（installation_task.cpp
│                                              的 collect_content_conflicts）
├── run_batch_transaction( [&] {
│   │
│   ├── Cache::write(":batch-start")    ← WAL: DB <6 个库> :batch-start (备份批次开始状态)
│   │                                    ← fsync WAL, fsync 备份
│   │
│   ├── for each pkg in order:
│   │     元数据核对（真 metadata ≠ 索引 → 重解并 i=0 重启，见下）
│   │     task.run(&ctx)                ← 包内 prepare() 的 check_for_file_conflicts
│   │                                     **第二道防线**（预检算漏的/中途状态变了的）
│   │     Cache::write(pkg + ":installed")  ← 每包完成后 DB 里程碑
│   │     success.push_back(pkg)
│   │
│   ├── COMMIT_PKGS                     ← fsync
│   └── catch:
│       batch_rollback(success)
│         ├── extract WAL 行（为空 → 不写 COMMIT_PKGS、保留现场）
│         ├── reverse_execute(ops)
│         │    每步: 操作 → fsync → RESTORE_* 审计 WAL → fsync
│         ├── purge_consumed_stashes
│         ├── Cache::load()
│         ├── DB <6 个库> :batch-start
│         ├── ROLLBACK pkg + END pkg
│         └── COMMIT_PKGS
│       （返回 true 才 cleanup_db_backups + trim_completed）
│})
│
├── TriggerManager::run_all()
└── 提交后收尾 finish_committed_batch()：CLEANUP → 删 stash → 剪枝 hooks → 执行 postinst
    → trim_completed → cleanup_db_backups（详见 §6.4）
```

> **订正 2026-09-25**：原图在开头写了 `recover_packages()` / `trim_completed()` 两步、并在
> 解析参数之后写了一个"一致性重试循环"。两处都与 `install_packages()` 的实际形态不符：
> WAL 恢复**不在** `install_packages()` 里，而是进程启动时在 `main_cli.cpp` 做一次
> （`init_filesystem()` → `recover_packages()` → `trim_completed()` →
> `cleanup_orphan_stashes()`）；`trim_completed()` 在本函数里由
> `run_batch_transaction()` 的第一行再跑一次。而"一致性重试"不是外层循环 ——
> **元数据核对就在批次循环内**（下载后比对真实 metadata 与索引，不一致就
> `resolve_with_solver` 重解、`i = 0` 从游标头重来，见 `package_manager.cpp`），
> 代码注释里明确写着"此处不再需要外层死循环"。另外这里写的是 6 个库
> （`pkgs`/`files.db`/`provides.db`/`confhashes.db`/`holdpkgs`），不是只有 `pkgs`。

### 6.2 包级安装

> **第③步之后的两趟结构（2026-09-26）**：一次升级 = **先移除旧版本的全部触碰面**（让开趟：
> `backup_existing_files()` 处理归档条目 + `remove_obsolete_files()` 处理 DB 旧键）
> **再安装新版本**（写入趟），同一个 WAL 批次内完成。让开趟与写入趟的"这个路径归谁处理、
> 做什么"由**唯一一份决策表**决定：`detail::decide_path(const PathFacts&) → PathDecision`
> （`pkg/op_sink.hpp`；`PathDecision` 每趟一个字段，天然保证"同一趟不会认领两个动作"；
> 入口的后置条件 `check_decision_invariants()` 保证"归档条目必须让开趟+写入趟各认领一次、
> DB 旧键必须登记趟认领一次"，违反即抛）。决策表的**输入事实**由各趟自己 probe ——
> **事实只由让开趟 probe 一次**：让开趟为**每个归档条目**记一条事实进 `ProbeLedger`
> （`detail::ProbeLedger`，`InstallationTask` 的**成员** —— 不是进程级静态表，所以同一进程里
> 先后处理同名包不会互相踩），写入趟三个分支（符号链接/目录/普通文件）的 `decide_path` 输入
> **一律**来自它。**没有一格需要二次 probe** —— 逐格核对过"让开之后再看盘面会不会改结论"：
> 目录条目由 `entry_is_dir` 提前返回（事实无关）；非 `/etc` 的格子二次 probe 只会让 `let_go`
> 从 `Stash` 变 `RegisterNew`，而写入趟**从不读 `let_go`**；`/etc` 撞真目录那一格两条判据路径
> 都落到 `WriteInPlace`。唯一"事实真的改结论"的是 `/etc` 普通文件撞被占那一格 —— 二次 probe
> 会看到"让开趟已把配置搬空"从而**静默覆盖用户改过的那份**（踩 §5.4 不变量 4），这也正是记录型
> 最初被引入的原因。

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
│   ├── 新目录: WAL: NEW_DIR <path>     ← fsync（纯记录，目录由这里的 create_directories 落位）
│   ├── 新文件: WAL: NEW <path>         ← fsync（纯记录，文件由下面的 COPY 落位）
│   └── 覆盖:   WAL: BACKUP <src> → <dst>  ← fsync（write-ahead：行先落）
│                rename phys → bak（stash）
│                fsync 父目录（默认不生效，§1.6）
│
├── remove_obsolete_files()            ← 第③步：从 commit_without_file_ops() **整段前移**到写入之前
│   （旧版本有、新版本不再提供的触碰面，跑在写入之前 —— 让开趟的②b。
│     pacman 也是"先删旧包文件（含目录）、再解压新包"，写入阶段因此看到干净的路径）
│   ├── 废弃文件/符号链接，非 /etc: WAL: REMOVE_OLD <src> → <dst>  ← fsync → rename 进 stash
│   ├── 废弃目录，非 /etc（最深优先、仅最后持有者、必须已空 **+ 新版本在该目录下没有条目**）:
│   │     WAL: DIR_RM <dir> <mode> <uid> <gid>  ← fsync → rmdir
│   ├── /etc 的废弃**文件/符号链接**: WAL: SAVE_CONF <src> → <dst> ← fsync → rename 成
│   │     `<路径>.lpkgsave`，**再**撤所有权 + 撤配置哈希记录（订正 2026-09-26：原文写
│   │     "/etc 废弃条目只撤所有权、不搬不删"，那是**旧政策**；维护者已改成 `.lpkgsave`）
│   ├── /etc 的废弃**目录**: **只撤所有权 + 撤记录，不搬、不删**（有意保留 —— 移除侧对
│   │     `/etc` 目录本来也是"宁可不碰、只告警"；目录**里面**的文件按上一行改名）
│   └── 撤销本包持有、且新版本不再声明的**目录 xattr 键**（WAL 先记旧值，§6.3 / §3.8）
│
├── copy_package_files()
│   ├── for each file:
│   │     （/etc 条目的分流见 §6.3，现行规则是**按类型是否变化**分派 —— 订正 2026-09-26：
│   │       原文这里只写了三哈希的三条路，那是"类型未变"那一半）
│   │     ⚠️ 第③步之后：让开趟**已经**把要碰的旧路径清掉了（"先移除旧版本的全部触碰面"），
│   │        所以 ②③ 两条"保留"路今天要先 `WAL: UNSTASH <bak> → <orig>` 把那份**搬回原位**
│   │        （见 §9.2 操作表里的 `UNSTASH` 行）。类型变化与 `InstallNew` 则**就地**落新内容。
│   │     copy → dst.lpkgtmp
│   │     fsync dst.lpkgtmp（受 --fsync 开关控制，默认不 fsync，§1.6）
│   │     WAL: COPY <tmp> → <dst>       ← fsync（write-ahead：行先落）
│   │     rename dst.lpkgtmp → dst
│   │     fsync 父目录（默认不生效，§1.6）
│   │     （目录条目：元数据与 xattr 经 §3.8 的三个原语 —— **先写改前值再动盘**）
│   └── ┌─ 异常（**②③④ + COMMIT + END** 整段的 try 块，订正 2026-09-26：原文写"整个阶段二/三"，
│         │   实际 `installation_task.cpp` 的 try 从 `backup_existing_files()` 起、含注册趟与
│         │   COMMIT/END —— 注册趟或 COMMIT 失败同样走这条）:
│         rollback_files(): WAL: ROLLBACK <pkg> <ver> + END <pkg> <ver>  ← fsync
│           （**只写这两个标记**，不做任何文件系统撤销 —— 撤销统一由外层
│              `batch_rollback` → `reverse_execute` 完成，见 §6.5）
│         throw
│
├── commit_without_file_ops()
│   ├── register_package()
│   │   ├── 写 deps 文件: WAL: DB/DBNEW <dep_path> <pkg>:installed
│   │   ├── 写 needed_so 文件: WAL: DB/DBNEW <nso_path> <pkg>:installed
│   │   ├── 写 man 文件: WAL: DB/DBNEW <man_path> <pkg>:installed
│   │   │   （三者都走 wal::write_string_file_wal：旧文件存在则先备份成
│   │   │     .lpkg_db_bak_before:<milestone>；内容为空且旧文件在 → DBRM 备份后删除；
│   │   │     新建 → DBNEW；然后 .tmp → fsync → rename → fsync 父目录，fsync 恒生效）
│   │   └── 注册文件所有权 (add_file_owner 独占 / add_dir_owner 累加，内存操作)
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

⚠️ **先分清两个常量**（订正 2026-09-26：原文写"`constants::DIR_ETC`，与移除侧 §7.2.1 **同一判据**"，不成立）：
- `constants::DIR_ETC` = `"etc/"` —— **归档条目**的相对形态（`scan_content_files` 的返回值），
  本节判"这条归档条目是不是配置"用它；
- `constants::DIR_ETC_PREFIX` = `"/etc/"` —— **逻辑路径**形态（DB 键、`/` 开头的绝对路径），
  移除侧（§7.2.1）与 `LogEntry` 判的是它。
两者**不是同一个字符串**，混用会让某侧永远判假。本节用前者。

| 符号 | 含义 |
|------|------|
| `hash_local` | 用户那份配置的内容 SHA256（符号链接 / 读不到 → **空**，= 无从判定）。
**第③步之后它读的是 stash 副本**（`hash_stashed_copy(rec.bak)`），不是"判定时刻盘上那份" ——
让开趟已经把盘上那份搬进了 stash，判定不再依赖"它在盘上还在不在"。
这也是"升级期间 `/etc` 配置短暂不在盘上"那个窗口的前提（§5 取舍）。 |
| `hash_orig`  | **上一次我们往这个包的这个路径里装进去的内容**的 SHA256（记在 `confhashes.db` 里） |
| `hash_pkg`   | 本次包里那个条目的内容 SHA256 |

分流（判定表只有一份：`classify_config_update()`，别在调用点再抄一遍）：

> ⚠ **三哈希只覆盖"类型未变"里的 file→file 那一格**（订正 2026-09-26：原文把整节写成
> "三哈希 + 符号链接条目走另一条分支"，那是**旧设计**；维护者已把 `/etc` 的落点规则改统一）。

**现行规则：按"类型是否变化"分派**（`decide_path()`，判据是新事实
`PathFacts::disk_is_symlink` —— 只有 `disk_exists` + `disk_is_dir` 区分不出"盘上是普通文件"
还是"盘上是符号链接"，而这两条路从此不同）：

| 盘上 | 归档条目 | 判定 | 让开趟 | 写入趟 |
|---|---|---|---|---|
| 真目录 | 目录 | 类型未变 | `Noop` | `WriteDirMetadata`（刷 uid/mode/xattr）|
| 普通文件 | 普通文件 | 类型未变 | `Stash`（为三哈希备 stash 副本）| **三哈希分流**（下面两张表）|
| 符号链接 | 符号链接 | 类型未变 | `Noop` | `WriteLpkgnew`（不接管）|
| 普通文件 | **符号链接** | **类型变化** | `SaveConfig` | `WriteInPlace` |
| **符号链接** | **普通文件** | **类型变化** | `SaveConfig` | `WriteInPlace` |
| 真目录 | 非目录 | **类型变化** | `SaveConfig` | `WriteInPlace` |
| 非目录 | 目录 | **类型变化** | `SaveConfigAndMkDir` | `WriteDirMetadata` |
| 无 | 任意 | — | `Noop`/`MakeDir` | 就地落位 |

即：**类型一变，原物就改名 `<路径>.lpkgsave`（内容一个不丢、`SAVE_CONF` 可回滚、不进 stash
所以提交后不会被清掉），新物就地落位**；**类型没变**才走"用户改没改过"那套（三哈希 /
`.lpkgnew` / 目录元数据）。这样"落点"只由一个更硬的判据决定，不用去猜用户有没有改过。

> 历史（**已被推翻，别照做**）：此前是"`dir → 非目录` 走 `.lpkgsave`、`file → 符号链接`
> 让原文件留原样 + 新链接退 `.lpkgnew`"两套不同规则，理由是"只有普通文件才是用户可能改过的
> 那份配置"。维护者 2026-09-26 决定按类型变化统一（见 `CLAUDE.md` §8 第 4 条的订正）。两行旧规则当且仅当在**类型未变**时与现行规则等价
> —— 类型一变就分道扬镳，这正是被推翻的那部分。

> ⚠ **符号链接条目的两条腿**（类型未变时）：**不做三哈希、不调 `classify_config_update`、
> 也不写 `set_conf_hash`**；目标路径已存在（**含符号链接** —— `fs::is_directory` 会跟随链接，
> 故必须显式排掉）就把新链接落成 `<路径>.lpkgnew` + 告警。后果：**`/etc` 下的符号链接条目
> 每次 reinstall 都会再落一份 `.lpkgnew`**（第一次装时目标不存在，落在本体；此后每次目标
> 都在），与"用户改没改过"无关。
> ⚠ **conf 记录随「键」废弃而撤，不随形态变化而撤**（2026-09-26 由两个独立 agent 各自撞见并
> 在 HEAD 上复现，属**既存语义**、非本轮回归）：`remove_obsolete_files` 阶段 1 的废弃判据是
> `facts.obsolete = !new_set.contains(old_file)`，而 `new_set` 装的是**全部**归档条目 ——
> 归档里的**符号链接按文件键登记**（`scan_content_files` 只有真目录才带尾斜杠）。于是
> v1 的 `/etc/x`（普通文件）在 v2 变成**符号链接**时，键 `/etc/x` **仍在 `new_set`** ⇒ 不算废弃
> ⇒ 登记趟给 `Noop`：那条记录**既不更新也不撤**（留着当以后的 `hash_orig`）。
> 只有 v2 **不再提供**该路径、或把它变成**目录**（键变成 `/etc/x/`）时旧文件键才废弃 →
> 撤销记录（`remove_conf_hash`）。**注意处置动作现在分两支**（见上方落点规则表）：
> 废弃的 `/etc` **文件/符号链接**先改名 `<路径>.lpkgsave`（`SaveConfigObsolete`）**再**撤记录；
> 废弃的 `/etc` **目录**是 `DropOwnership`（只撤记录、不碰盘）。（订正 2026-09-26：
> 原文只写 `DropOwnership → remove_conf_hash`，那是文件那一支改政策之前的形态。）
> 已知后果（**未证实为缺陷**，属性测试只跑两段版本序列故未覆盖）：三段序列
> `file → symlink → file`（且内容回到旧值）时 `hash_orig == hash_pkg` 会判 `KeepLocal`，
> 盘上仍是那个**符号链接**（"升级没生效"，**非数据丢失**；内容不同时走 `SaveLpkgnew`，安全）。
>
> 另：盘上是 `symlink→目录`、归档发目录条目（`x/`）这一类由 §3.6.1 第 3/4 条的类型变更冲突
> 判据接管，同样够不到三哈希。

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

> ⚠️ **三哈希看不见 mode —— "只改权限"会被静默纠正**（2026-09-26 实测，此前**完全静默**）：
> 三份都是**内容**哈希，所以"用户只 `chmod` 过这份配置"在它眼里与"用户没动过"**不可区分**
> ⇒ 走 ① 静默换新版；而落位时 `stage_regular_file` 的 `lchown`/`chmod` 取自**包内条目**
> ⇒ 用户改的权限被一并改回包内值（实测：盘上 0600 → 升级后 0644，**且没有任何输出**）。
> 目录那边至少有 `warning.dir_perm_mismatch`（目录元数据的改前值是 write-ahead 的、顺手能比），
> 文件这边此前连告警都没有。**现行处置：加 `warning.file_perm_mismatch`，先告警、再纠正 ——
> 只把"静默"变"可见"，语义不变**（判据取两侧 `lstat`、只比 mode；在**让开趟**报，因为那是
> 唯一还看得见盘上那份的时刻 —— 写入趟落位时原位已被 `Stash` 清空）。
> **「内容未变时该不该保留用户改的 mode」已拍板（2026-09-26）：不保留 —— 包内值胜出**，要求只是
> 「**不静默**」（即上面的告警）。因此**不另立 mode 记录**：`confhashes.db` 只记内容哈希，而既然
> 结论是包内值胜出，「用户改过 mode」本就无需判定。

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
- **记录随包走**：包被移除（`remove_package_files`）、或**该路径被 `--overwrite` 接管**（归属在
  安装期就被 `ConflictView::drop_owners` 摘走，见 §6.2 包级安装流程）、或新版本**不再提供该
  文件**时删除 —— 最后这一支由让开趟的 `remove_obsolete_files()` 做（**订正 2026-09-26**：
  原文写"`commit_without_file_ops` 的废弃 /etc 条目"，而废弃清除已在第③步整段前移到写入
  **之前**，见 §6.2；`/etc` 条目现在还会先改名 `.lpkgsave`，见 §6.3 的落点规则表）。
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

### 6.3.1 目录 xattr 的**键归属表**（`xattrkeys.db`，2026-09-26 新增）

目录的 xattr 有两件事与文件不同，所以需要一张**独立**的表：

1. **目录是就地改活对象** —— 改前值必须进 WAL（§3.8 的 `XATTR_SET`/`XATTR_NEW`），这样**回滚**
   能把某个键还原成"旧值"或"本来不存在"。这一半**不需要** DB 表。
2. **升级时"新版本不再声明的键"要能撤掉** —— 这是**跨事务**的：上一次装了什么键，WAL 早就被
   trim 掉了。所以必须有一份持久记录。而且撤销的粒度是**键**、不是目录 —— 一个目录被多个包
   持有是常态（`/usr/share` 之类），按目录记就分不清"这个键是谁设的"。

**存储形态**：`<state_dir>/xattrkeys.db`（`Config::xattr_keys_db()`），与 `files.db` 同形：

```
<逻辑目录键>\x1f<base64 的键>	<pkg1> <pkg2>        ← 取值 = 属主包名集合（排好序）
```

键用 `\x1f`（unit separator）连接**逻辑目录键**与 **base64 键**：base64 字母表里没有 `\x1f`，
所以两半不会串味；base64 则保证键名里的任意字节（含 `\t`/`\n`/`\x1f` 本身）不会破坏 DB 行格式。

- **接进 DB 里程碑机制的每一环**（漏任意一环，这张表就会在崩溃/回滚后与盘面脱节）：路径与
  访问器（`config`）；**预建**（`init_filesystem()` 的 `ensure_file_exists`，与兄弟库同口径）；
  读侧（`Cache::load()` → `read_db_uncached`，缺文件 = 空表）；带里程碑写
  （`Cache::write(milestone)` → `write_db_file_wal(..., "DB")`）；孤儿备份清理
  （`cleanup_db_backups()` 扫 `state_dir`，本库在其中）；回滚/恢复（走 WAL 的 DB/DBNEW/DBRM 行，
  与文件清单无关）。
- **写入时登记**：`write_dir_entry()` 每设一个键就 `add_xattr_key_owner(<目录键>, key, pkg)`。
- **撤销**：唯一实现 `revoke_xattr_key_if_unowned()`（`install_common`），两个调用点 ——
  升级侧 `revoke_undeclared_xattrs()`（从 `remove_obsolete_files()` 调；先按目录读一次包内
  `llistxattr` 当"新版本声明集"）与移除整包侧。**只撤本包持有、且还有没有别的属主**的键；
  删除前把旧值写进 WAL（否则"撤"本身又是一个不可回滚点）。
- **`/etc` 配置的"保留成 `.lpkgsave`"那套政策与这里无关** —— xattr 没有"改名保留"一说，
  它是"这个键不再属于任何包了就删掉"。
- **拒绝登记的输入**（宁可不记，也不能记错）：路径含 `\x1f`/`\t`/`\n`/`\r`（DB 行格式本身的
  分隔符，与 `files.db` 同一约束）→ 拒绝登记（不写、返回 false）。归档成员名消毒**不挡**控制
  字符，所以这是唯一能挡住它们的地方；记成会被解析歪的记录会**删错键**，比留一份陈旧 xattr 糟。

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
- **hook 成员是符号链接时"限定在包内"**（2026-09-26 补，第三轴审计）：`directory_entry::
  is_regular_file()` **跟随**末段链接，而 `fs::copy` **也跟随** —— 于是 `hooks/postinst.sh ->
  /etc/shadow` 会让 root 把**宿主**该文件的内容拷成 `hooks_dir/<pkg>/postinst.sh`（带 exec）并
  **当 postinst 执行**：包内容读出了包外。判据：解析后（`weakly_canonical`）必须仍落在**本包的解压
  目录之内** —— 包内互指（`hooks/postinst.sh -> ./real.sh`）**照旧跟随复制**（那是合法用法，
  今天的行为不变）；落到包外则**整包拒绝**（新键 `error.hook_symlink_escapes_package`，
  点名条目与它解析到的目标；与归档成员名消毒同款处置）。
  实测确认打包侧**保留**链接成员（`archive_read_disk_set_symlink_physical`）⇒ 这条路径可达。
- 钩子执行失败只告警不抛（批次已提交、包确实装上了）。
- **清理 stash 失败也不算批次失败**：`cleanup_stashes()` 抛错只告警（`warning.cleanup_deferred`），
  `CLEANUP` 记录留着，由下次 `recover_packages()` 的 `continue_post_commit_cleanup()` 续传。
- 末两步的顺序固定：`trim_completed()` 先、`cleanup_db_backups()` 后 —— 而
  `cleanup_db_backups()` 自己还带一道门控（WAL 里仍有未配对批次就一个都不删，见 §3.1 附注），
  因此"提交后再调用"不等于"一定删"，那是**故意**的（别把上个未提交批次的重试依据一起扫掉）。

### 6.5 包级回滚（InstallationTask::rollback_files）

**只写回滚标记，不做任何文件系统撤销**（单撤销路径，见下方说明）：

```
rollback_files()
│
├── WAL: ROLLBACK <pkg> <ver>          ← fsync（失败包的包级回滚标记，仅信息性）
├── WAL: END <pkg> <ver>               ← fsync
│
└── stashes_.clear(); new_files_.clear(); new_dirs_.clear();
```
（成员名就是这三个向量；`hook_files_` **不**清 —— 它由批次级的 `hook_sets` 账本用
`did_process()` 判读，清了会把"本包没被处理"误表示成"新版本没有 hooks"。）

> **单撤销路径（2026-08-03 重构）**：所有正向操作的逆序执行统一由
> `batch_rollback` → `reverse_execute` 完成（所有 install/upgrade 都经
> `run_batch_transaction`，包级失败必然触发批次回滚）。rollback_files 曾在此处
> 恢复 `.lpkg_bak` 并写 RESTORE_* 审计，与 reverse_execute 形成**双重回滚**——
> rollback_files 恢复的旧文件被 reverse_execute 的 COPY 逆操作（无条件删除 dst）
> 再次删除，升级中途失败的包丢失旧文件。撤销职责收拢后该问题消失，且
> "rollback_files 删新文件失败"的残留也能由 reverse_execute 兜底重试。
> 失败包的 ROLLBACK 标记由 rollback_files 写；成功包的由 batch_rollback 统一写。
> （2026-09-25 订正：原图写的是 `backups_.clear()`，该成员在 stash 化改造后叫
> `stashes_`（`package_manager.hpp`）；§13 2.3 的描述一直是对的。）

---

## 7. 移除流程

### 7.1 顶层流程

```
remove_package(pkg_name, force, wrap_in_txn, purge_config)      ← 库层单包入口
│   （`wrap_in_txn` 是**遗留参数、实现已忽略**：批次边界现在由
│     remove_packages_checked 统一决定，见下）
└── remove_packages_checked({pkg_name}, force, purge_config)     ← 与所有移除路径同一实现
```

**所有移除路径最终汇到同一个批次实现**（`package_manager.cpp` 的
`remove_packages_in_one_batch`）：`remove_package`（单包）/ `remove_packages`（`remove a b c`）/
`autoremove` / `force_solve_conflict` 经 `remove_packages_checked`（筛选 → 批次 → 收尾）；
`remove_packages_recursive`（闭包）自带一套筛选（反向依赖闭包 + 剔除 essential + 三次验证码
确认）后**直接**调 `remove_packages_in_one_batch` —— 它不经过 `remove_packages_checked`，
但两者共用同一个批次实现与同一套 `check_removal_preconditions`（见下）。

```
remove_packages_checked(pkgs, force, purge_config)
│
├── 不存在的包直接跳过（info.package_not_installed）
├── 安全检查（removal_allowed）：essential / 反向依赖 / 提供能力的反向依赖
│     ← **全或无**：任一包被拒 → 一个都不删、连事务都不开（WAL 里不留 RM_BEGIN）
├── remove_packages_in_one_batch(to_remove, force, purge_config, stashes)
│   ├── check_removal_preconditions(pkgs, force)   ← **进入事务之前**、整批一次跑完
│   │     （共享文件 + "DB 文件键所指路径在盘上已是实体目录"；见下方注）
│   └── run_batch_transaction([&] { for each pkg: do_remove_package(...) })
├── finish_committed_batch(stashes, to_remove)     ← 提交后收尾（§6.4）
├── TriggerManager::run_all()                      ← 删库后的 SONAME 链接刷新
└── 每个包打一行 info.package_removed_successfully
```

`force` 与 `purge_config` 是**两个正交维度**：`force`（CLI `--force`）只跳过安全检查
（反向依赖 / 共享文件 / 陈旧文件键），`purge_config`（CLI `--purge-config`）才决定配置文件
的真删。旧实现把"删配置"绑在 `force` 上、且 `remove -r` / autoremove / force-solve-conflict
内部硬编 `force=true` —— 用户不给 `--force` 也会丢配置。

（2026-09-25 订正：原图写的是 `remove_package` 自己"`recover_packages() + trim_completed()`
(if wrap_in_txn) → 版本检查 → `run_batch_transaction(1, …)` → `do_remove_package(pkg, force,
purge_config)`"。现行实现里 `remove_package` 只是转调，`wrap_in_txn` 被忽略
（`bool /*wrap_in_txn*/`）；WAL 恢复在进程启动时做一次（§6.1）；`do_remove_package` 的签名
已变成 `(pkg_name, purge_config, ver, stashes)`，**不再收 force** —— 安全检查整体前移到了批次
入口，批次内没有任何"跳过检查"的分支。）

### 7.2 核心移除逻辑

```
do_remove_package(pkg_name, purge_config, ver, stashes)
│
├── SIGINT 检查
├── prerm hook（文件被删**之前**跑，故不能挪到提交后 —— 那等于静默变成 postrm）
│
├── WAL: RM_BEGIN <pkg> <ver>            ← fsync
│
├── 阶段 A：文件处置（逐 owned **非目录**条目）
│   for each owned_file:
│     SIGINT 检查
│     盘上是实体目录（DB 却记成文件键）→ **只跳过 + 告警**，绝不搬进 stash
│       （非 force 时批次预检通常已拒绝整批 ⇒ 走不到这里；**但 `/etc` 键是例外** ——
│         批次预检把 `/etc` 前缀的条目整条跳过（`package_manager.cpp`），所以非 force
│         的移除照样走到这里、只告警 + 跳过。订正 2026-09-26：原文那句括号没写这个例外。）
│     if 是配置文件（路径前缀 /etc/）且非 purge_config:
│       WAL: SAVE_CONF <phys> → <phys>.lpkgsave  ← fsync（write-ahead）
│       （目标已存在 → 先把旧的移位成 .lpkgsave.1/.2…，那也是同类型的一条行）
│       rename phys → <phys>.lpkgsave      ← 不进 stash：提交后不会被清掉
│     else:
│       WAL: BACKUP <phys> → <bak>          ← fsync（write-ahead）
│       rename phys → bak（stash 目录 + 随机后缀防冲突）
│       fsync 父目录（默认不生效，§1.6）
│     登记触发器 check_file（删除也是"路径变了"，ldconfig 规则照常入队）
│
├── remove_package_files()               ← 从 DB 移除文件记录 + provides
│                                          （/etc 条目同时清 confhashes.db 记录，见 §6.3）
│
├── 阶段 B：目录（最深优先、仅最后持有者、必须已空）
│   for each owned **目录**条目（按**路径字符串长度**降序排序 = 最深优先）:
│     cache.remove_file_owner(键)；仍有其他持有者 → 跳过
│     规范化掉尾斜杠 → 非真目录 / 是 symlink → 跳过（绝不 rmdir 链接）
│     非空 → 跳过（含无主内容 → 整树保留）
│     WAL: DIR_RM <dir> <mode> <uid> <gid> ← fsync（write-ahead）→ rmdir
│     （挂载点由 remove_empty_dir 自己挡下 → 跳过 + 告警）
│
├── 清理 dep/needed_so/man（DBRM，逐文件）+ 内存里的反向依赖边
│   DBRM <dep_file> / <needed_so_file> / <man_file>    ← 行后 rename 成 .lpkg_db_bak_before:…
│   （**hooks_dir/<pkg>/ 不在这里**：它无 WAL 记录，放在可回滚的批次内会让批次回滚后
│     钩子永久丢失 —— 改由提交后的 finish_committed_batch() 删，见 §6.4）
│
├── cache.remove_installed(pkg)（内存）
│
├── DB 落盘（先于 RM_COMMIT：提交标记前 DB 已持久化，崩溃可恢复）：
│   DB <6 个库> pkg:removed               ← 备份后 + fsync（§1.6 恒生效）
│
├── WAL: RM_COMMIT <pkg> <ver>           ← fsync
│
└── WAL: RM_END <pkg> <ver>              ← fsync
```

> **CLEANUP 不在包级流程内**：stash（刚被删掉的文件）是回滚的唯一来源，必须活到批次提交之后。
> 清理由批次级的 `finish_committed_batch()` 在 `COMMIT_PKGS` 之后统一执行（见 **§6.4**）。
>
> **逐包安全检查（共享文件 / 陈旧文件键撞实体目录）不在包级流程内**：它们在
> `remove_packages_in_one_batch()` 里、**进入事务之前**对整批一次性跑完
> （`check_removal_preconditions()`）。原先逐包检查时，批次里前面的包已经跑过 prerm、
> 后面的包才被检查拒绝 —— 整批回滚撤得回文件与 DB，撤不回 prerm 的副作用。前移后拒绝的批次
> 根本不开启事务。prerm 之后仍可能失败的只剩 I/O 错误与 Ctrl+C（上游 libalpm 的 pre_remove
> 同样在事务内、删除之前跑，暴露面相同）。
>
> （2026-09-25 订正：本图原先的目录段写的是"目录 BACKUP（取代旧 RM_DIR）→ 安全检查共用
> `detail::backup_dir_tree_whole` → 整目录 rename 成 `.lpkg_bak`"—— 该函数已不存在；现行是
> 上面的**阶段 B：`DIR_RM`**。同时把"hooks"从 DBRM 那一步移出（它从来不是 DBRM），
> 并把"`CLEANUP` 见 §7.4"改为 **§6.4**——本文档没有 §7.4。）

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
    DB <6 个库> pkg:removed  → 查找 .lpkg_db_bak_before:pkg:removed
       存在   → rename 回原位 (→ 该库回到 pkg:installed 的内容)
       WAL: RESTORE_DB <bak> → <db>      ← fsync
    DBRM <dep/needed_so/man> → .lpkg_db_bak_before:pkg:removed 存在 → rename 回
       WAL: RESTORE_DB <bak> → <file>    ← fsync
    DIR_RM <dir> <mode> <uid> <gid>      ← 目录是**重建**出来的（不是从备份搬回）
       → create_directories 重建 + 按行内 mode/uid/gid 恢复元数据
         （先剥尾斜杠；**已存在且是真目录也照样** chmod/chown 写回行内值 —— 幂等口径是
          `Guard::Always`，"目录已存在就跳过"**不存在**；只有"不是真目录"才跳过）
       WAL: RESTORE_DIR <dir>            ← fsync
    BACKUP/REMOVE_OLD/SAVE_CONF <src> → <dst>
       → rename dst 回 src（dst 不存在 → 跳过，幂等）
       WAL: RESTORE_FILE <dst> → <src>   ← fsync
    RM_BEGIN  → 跳过（元数据行）
  → purge_consumed_stashes（还原完的空 stash 根）
  → Cache::load()
  → DB <6 个库> :batch-start             ← 备份后 + fsync
  → ROLLBACK <pkg> <ver>                ← fsync
  → END <pkg> <ver>                      ← fsync
  → COMMIT_PKGS                          ← fsync
```

（2026-09-25 订正：原图把目录的逆向写成 `BACKUP <dir> → <dir.lpkg_bak>` 的整树 rename 回来
—— 现行算法里目录根本没有 `.lpkg_bak`，它是 `DIR_RM` 的元数据重建，见 §3.6。另补上
`purge_consumed_stashes` / `Cache::load()` 两步与 `SAVE_CONF` 同分支。）

---

## 8. 升级流程

### 8.1 顶层流程

同 install 流程，使用 `run_batch_transaction`。区别：
- `InstallationTask` 设置了 `old_version_to_replace_`
- **废弃清除走让开趟的 `remove_obsolete_files()`（跑在写入之前）**，不再在注册趟里（订正
  2026-09-26：原文写"`commit_without_file_ops` 中处理旧版本废弃条目" —— 那是第③步重构**前**
  的位置）：阶段 1 `REMOVE_OLD`（非 `/etc` 文件 → stash）/ `SAVE_CONF`（`/etc` 文件与符号
  链接 → `.lpkgsave`）/ `DropOwnership`（`/etc` 目录 → 只撤所有权）、阶段 2 `DIR_RM`
  （非 `/etc` 目录，最深优先 / 仅最后持有者 / 必须已空 / **新版本在该目录下没有条目**，
  见 §3.6）；另撤销本包持有且新版本不再声明的目录 xattr 键（§6.3）。

### 8.2 升级回滚

升级回滚从 stash 里的 `.lpkg_bak` 恢复旧版本文件 + DB 链式恢复：

```
WAL 内容（以 pkgs 为例；实际每个里程碑 6 个库各有行）:
  DB /var/lib/lpkg/pkgs libfoo:installed  (版本 2.0)

回滚:
  1. RESTORE_DB: .lpkg_db_bak_before:libfoo:installed → /var/lib/lpkg/pkgs (→ 1.0 状态)
  2. RESTORE_FILE_RM: /usr/lib/libfoo.so.2（新增文件，无备份 → 删除）
  3. RESTORE_FILE: stash 里的 libfoo.so.1.lpkg_bak_libfoo_<随机> → /usr/lib/libfoo.so.1（旧版本恢复）
  4. DB <6 个库> :batch-start
  5. ROLLBACK libfoo 2.0 + END libfoo 2.0
  6. COMMIT_PKGS
```

（2026-09-25 订正：第 2 步的审计行名现行是 `RESTORE_FILE_RM`（`REMOVE_FILE` 只是仍可解析的
旧名，见 §2.1）；第 3 步的备份落点在每文件系统 stash，不在原位。）

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
 * 从 WAL 日志文件提取当前（最后一个未完成的）批次的操作行列表。
 * **从文件末尾反向扫**：遇到的第一个有效行若是 BEGIN_PKGS → 从它取到文件末尾
 * （只收 `is_valid()` 的行，破损/半写的尾部行一律丢弃）；若是 COMMIT_PKGS →
 * 说明最后一个块已配对，**返回空**（无未提交批次）。
 */
std::vector<WALOp> extract_current_batch_ops(const std::string &wal_path);

/**
 * 完整的批次回滚。**返回值决定调用方能不能清理 DB 备份**：
 * true  = 确实回滚了（批次已由 COMMIT_PKGS 收尾，DB 备份已被 reverse_execute 消费，可安全清理）
 * false = 无可回滚的行（WAL 里没有未完成批次）→ **不写 COMMIT_PKGS、保留现场**，
 *         交给下次 recover_packages() 续传（绝不能 cleanup_db_backups()）
 *
 * 1. extract_current_batch_ops()   —— 为空 → log_warning + return false
 * 2. reverse_execute(ops, true)    —— 逆向执行 + 每步写 RESTORE_* 审计行
 * 3. purge_consumed_stashes(ops)   —— 文件已还原，清掉空 stash 根
 *     ⚠️ 但**先扫一遍 `UNSTASH` 行**：被它引用的 `bak` 在 reverse 之后**仍然存在** ⇒ 判定
 *     "反撤销未收敛" ⇒ 该行引用的**整个 stash 根**在删除阶段被 `continue` 跳过（不是跳过一行行
 *     的 remove_all，是连根不删）。这条守卫是引入 `UNSTASH` 时新增的安全判据：`purge_consumed_stashes`
 *     是按"reverse 已完成 ⇒ stash 已空"来**整目录 delete** 的，而 `UNSTASH` 的逆操作正是"把那份
 *     再搬进 stash" —— 任何一步没做完，唯一一份数据就留在那个根里，整目录删掉即不可逆。
 * 4. Cache::instance().load()      —— 重载磁盘 DB（**不再手动清内存 cache**：
 *     曾逐包 remove_installed，随后必被 load() 覆盖，是死代码）
 * 5. cache.write(":batch-start")   —— 写回滚后的状态（6 个库各一行 DB）
 * 6. ROLLBACK pkg + END pkg（每个已回滚包，版本号从 WAL 的 BEGIN/RM_BEGIN 行取）
 * 7. COMMIT_PKGS
 */
bool batch_rollback(const std::vector<std::string> &success);

} // namespace wal
```

（2026-09-25 订正：本块原写 `extract_current_batch_ops()` 无参、`batch_rollback` 返回 void、
且步骤 2 是"清理内存 cache" —— 三者都与 `wal_op.hpp` / `wal_op.cpp` 不符。）

### 9.2 reverse_execute 操作表

| 操作类型 | 逆向操作 | fsync | RESTORE 条目 | 备份不存在时 |
|---------|---------|-------|-----------|-------------|
| `BACKUP`（只可能是**文件**/符号链接；目录不走这条路，见 §3.6） | rename .bak → 原位 | fsync 父目录 | `RESTORE_FILE <bak> → <orig>` | .bak 不存在→跳过（已被消费→幂等） |
| `REMOVE_OLD` | 同上（同 BACKUP 分支） | 同上 | `RESTORE_FILE <bak> → <orig>` | 同上 |
| `SAVE_CONF` | 同上（**三型合一个分支**：`rename(arg2 → arg1)`，dst 不在 stash 里） | 同上 | `RESTORE_FILE <bak> → <orig>` | dst 不存在→跳过（幂等） |
| `UNSTASH`（第③步引入） | **再搬进 stash**：`rename(arg2 → arg1)`（与 BACKUP 正好互为逆） | fsync 父目录 | `RESTORE_FILE <orig> → <bak>`（记**实际动作方向**） | orig 不存在 / bak 父目录不存在→跳过（幂等） |
| ↑ 引入 `UNSTASH` 时**必须一并复核** `purge_consumed_stashes` 的"已消费"判据：被 `UNSTASH` 搬回原位的 bak **已被消费**，不该再被它 `remove_all` 掉；同一路径的 `BACKUP` 行在前、`UNSTASH` 行在后，回滚逆序才收敛回原点（两步 rename，幂等）。 | | | | |
| `COPY` | remove(dst)；**并顺带清掉 `arg1` 的 `.lpkgtmp`**。⚠️ **删除那一支的判据不是"存在"而是 `Guard::TargetTakenNotDir`**（`exists_no_follow(orig) && !is_real_directory(orig)`）—— 落点是**真目录**时**绝不删**：`fs::remove` 对**空目录**会 `rmdir` 成功，那会把盘上原有的空目录删掉而 DB 仍声称持有（盘面/DB 脱节，且没有 BACKUP 行可还原；实测事故） | 不需要 | `RESTORE_FILE_RM <dst>` | 不存在→跳过 |
| `DIR_META`（2026-09-26 新增，§3.8） | `lchown`+`chmod` 写回行内 mode/uid/gid | fsync 父目录 | `RESTORE_DIRSTATE <path>` | `Guard::RealDir`：**只回写真还是目录的**；目录已被更晚的逆操作删掉时跳过、**不重建**（重建是 `DIR_RM` 的职责） |
| `XATTR_SET`（2026-09-26 新增） | `lsetxattr` 写回 `arg3` 的**旧值**（base64 解码后） | fsync 父目录 | `RESTORE_DIRSTATE <path>` | `Guard::RealDir`：**只回写真还是目录的** —— 与写入侧（`lstat`+`S_ISDIR`，**只描述真实目录**）逐字对齐。（订正 2026-09-26：原先用 `Guard::TakenNotSymlink`，它**普通文件/FIFO/设备也放行** ⇒ 回滚会在写入侧从不写的路径上 `lsetxattr` **造出一个键**；`DIR_META` 那一格本来就是 `RealDir`，只有这两格没对齐） |
| `XATTR_NEW`（2026-09-26 新增） | `lremovexattr` 删掉 `arg2` 的键（base64 解码） | fsync 父目录 | `RESTORE_DIRSTATE <path>` | `Guard::RealDir` 同上 |
| `NEW` | remove(path)（含 dangling symlink） | 不需要 | `RESTORE_FILE_RM <path>` | 不存在→跳过 |
| `NEW_DIR` | rmdir（仅当"非 symlink + 是真目录 + 为空"） | 不需要 | `RESTORE_DIR_RM <path>` | 不存在→跳过 |
| `DIR_RM` | 需要则 `create_directories` 重建 + 按行内 mode/uid/gid 恢复元数据（路径**先剥尾斜杠**，见 §3.6.1 第 1 条；不是真目录则跳过、**不** chmod/chown） | 不需要 | `RESTORE_DIR <path>` | 幂等口径是 `Guard::Always`：**目录已存在且是真目录也照样** `lchown`/`chmod` 写回并计一次 —— "已存在→跳过"**不存在**（目录缺失正是它要修的状态；"目录已存在"在回滚路径上反而是常态）。只有"不是真目录"才判否 |
| `DB` | .bak_before 存在→rename 回 | fsync 父目录 + WAL | `RESTORE_DB <bak> → <db>` | .bak 不存在→跳过（WAL 已写但备份未完成→原文件还在） |
| `DBNEW` | .bak 存在→rename 回; 不存在→删除文件 | fsync 父目录 + WAL | `RESTORE_DB` 或 `RESTORE_DB_RM` | - |
| `DBRM` | .bak 存在→rename 回 | fsync 父目录 + WAL | `RESTORE_DB <bak> → <db>` | .bak 不存在→跳过 |
| `CLEANUP` | **跳过（不可回滚）** | 不执行 | 不写入 | - |
| 元数据行（`BEGIN`/`COMMIT`/`ROLLBACK`/`END`/`RM_*`/`BEGIN_PKGS`/`COMMIT_PKGS`） | 跳过（`is_metadata()`） | - | 不写入 | - |
| RESTORE 审计行（含旧名 `REMOVE_FILE`/`REMOVE_DIR`） | 跳过（`is_restore_audit()`） | - | 不写入 | - |

> 表外还有两列，只有新加的三行会用到（见 §3.8 与 `UNDO_TABLE` 的注释）：
> **`Stat`** 全部取 `Stat::None` —— `DIR_META`/`XATTR_SET`/`XATTR_NEW` 既没还原文件、也没重建
> 目录，混进 `files_restored`/`dirs_recreated` 会让那两个计数器失去意义；
> **`Confine`** 全部取 `Arg1Only`（理由见下方 confinement 一节的 ⚠️）。

**路径 confinement（2026-09-26 增补）**：`reverse_execute` 回放前会校验 WAL 行里涉及的路径
是否落在允许范围内（`wal_line_paths_confined()`，`db/wal_op.cpp`）——**两级判据**：
① **词法**：规范化后必须是 `root_dir()` 的分量前缀（挡 `../`、挡"绝对路径就在 root 外"）；
② **canonical 复核，且只解析父目录**（挡 `<root>/evil -> /etc` 这类穿透；解不开 → 判不越界）。
⚠️ **机制订正 2026-09-26：允许集对每一行都是同一个** —— `wal_line_paths_confined()` 里只有一个
`ok()` 判定，内容是 `path_within_root(root, p) || ∃sr ∈ stash_roots: path_within_root(sr, p)`，
**对所有行一视同仁**。逐行不同的是 `Confine::` 这个枚举，它**只决定查哪些字段**：

| `Confine` | 查什么 | 用在 |
|---|---|---|
| `None` | 不查（该行不碰文件系统，或不进 reverse） | `CLEANUP`、`BEGIN/COMMIT/ROLLBACK/END`、`RESTORE_*` 审计行等 |
| `Arg1Only` | 只查 `arg1` | `NEW`/`NEW_DIR`/`DIR_RM`/`DB*`/**`DIR_META`**/**`XATTR_SET`**/**`XATTR_NEW`** |
| `Arg1AndArg2` | 两个都查 | `BACKUP`/`REMOVE_OLD`/`SAVE_CONF`/`UNSTASH`/**`COPY`** |

（订正：原文写"`BACKUP`/…/`UNSTASH` 查 root ∪ stash 根；`COPY`/`NEW`/… 查 root"，把"查哪些字段"
说成了"查哪个允许根"，且把 `COPY` 归错了组 —— `COPY` 的 `confine` 是 `Arg1AndArg2`。
原文还说 stash 落点"**可能不在 root 内**"，而代码注释说"后者在当前实现里**本就落在 root 之内**
（`stash_parent_dir` 恒 clamp 在 root 内），把它单列是为了**不误拒**" —— 两边对现状的断言相反，
以代码为准。）
`stash_roots` 由 `referenced_stash_roots()` 从 WAL 行里推出（含 `UNSTASH` 的 stash 侧）。

**`DIR_META`/`XATTR_*` 为什么必须是 `Arg1Only`**：它们的 `arg2`/`arg3` 是 **base64 编码的
xattr 键/值**，**不是路径** —— 若按 `Arg1AndArg2` 查，一串 base64 会被判成越界路径，从而**整行
被跳过**（回滚静默少还原一次）。

**越界 → 告警 + 跳过该行，绝不抛** —— 恢复路径抛异常 = 每次启动都重试、所有 lpkg 命令起不来；
这与"bak 不存在 → 跳过"是同一个保守方向。`root_dir() == "/"`（默认安装）时整套检查关闭
（任何绝对路径都在其内）；**末段不解析**，所以 `--root` 下包发的绝对目标链接
（`<root>/usr/bin/foo -> /etc/foo`）不会被误判。

（2026-09-25 订正：`SAVE_CONF` 行原表漏列 —— 它与 BACKUP/REMOVE_OLD 是**同一个分支**；
`REMOVE_FILE`/`REMOVE_DIR` 是只解析的旧名、不是现写的审计行，故归入"审计行"一类。
未解析的行（`INVALID`，尾部半写等）也一律跳过 —— `skip_in_reverse()` 是"该不该逆向"的
唯一判据。）

---

## 10. RESTORE 审计日志与二次回滚

### 10.1 为什么需要 RESTORE 审计

如果 `batch_rollback()` 自身崩溃，系统处于"半回滚"状态。WAL 中有：
- 正向操作行（BEGIN、BACKUP、COPY 等）
- 部分逆向操作行（RESTORE_FILE、RESTORE_DB 等）
- 没有 COMMIT_PKGS

`recover_packages()` 启动后，收集所有行（包括 RESTORE 行）作为"事务内容"，然后 `reverse_execute`
全部。**RESTORE_\* 行本身一律被跳过**（`WALOp::skip_in_reverse()` → `is_restore_audit()`）——
它们是**上一次回滚留下的记录**，再逆向一次等于"撤销撤销" = 重做正向操作（推演见 §10.2 末尾）。
真正的幂等判据在**正向操作行**上：每个逆操作自己检查"备份/目标还在不在" ——

- `BACKUP`/`REMOVE_OLD`/`SAVE_CONF` 的反向 rename：`<bak>` 不存在 → 已被消费 → 跳过；
- `COPY`/`NEW` 的反向删除：目标不存在 → 跳过；
- `DIR_RM` 的反向重建：**目录已存在也照样** `lchown`/`chmod` 写回（`Guard::Always`）——
  "已存在就跳过"**不成立**，见 §9.2 该行。

所以第二次恢复与第一次**收敛到同一个状态**，与 RESTORE 行写没写、写到第几步无关。
（旧名 `REMOVE_FILE`/`REMOVE_DIR` 只用于**解析**老 WAL：解析成功后同样按审计行跳过，
**不会**被当成"删除这个路径"执行 —— 见 §2.1 表末行。）

（2026-09-25 订正：本节的原文写"遇到 RESTORE 行时：`RESTORE_FILE` 检查 bak 是否存在，
存在就**重复执行 restore**（幂等）；`REMOVE_FILE <path>` 检查存在则删除" —— 两处都与现行实现
相反：RESTORE 行既不重复执行，`REMOVE_FILE` 也不是现写/现执行的删除行。）

### 10.2 二次回滚的场景

场景 A：rollback 在 RESTORE_DB 完成后、RESTORE_FILE 前崩溃
```
WAL 状态：
  BEGIN_PKGS
  ... install A, install B fails ...
  RESTORE_DB /bak → /pkgs          ← 已完成
  RESTORE_FILE /bak → /a           ← 未执行（崩溃在此）
  RESTORE_FILE_RM /new/b
  ...

恢复：
  reverse_execute 逆序（RESTORE_* 行全部跳过，只看正向操作行）：
    NEW /new/b（正向行）: 文件存在？A 的 copy 还在，所以 /new/b 存在→删除（安全）
    BACKUP /a → /bak（正向行）: /bak 存在→rename 回 /a（恢复旧版本 A）
      ⚠️ 订正 2026-09-26：原文把参数顺序写成 `BACKUP /bak → /a`，**反了** —— 正向 `BACKUP` 行
      记的是 `<原位> → <stash>`（`OpSink::backup` 写的就是这个方向），`/a` 才是原位。
      （`RESTORE_FILE /bak → /a` 那些审计行方向是对的：它记的是**实际动作**方向。）
    DB /pkgs A:installed（正向行）: .lpkg_db_bak_before:A:installed 不存在（已被第一次消费）→跳过（幂等）
  → 系统状态一致
```

场景 B：rollback 全部完成但 COMMIT_PKGS 未写（在 COMMIT_PKGS 写入前崩溃）
```
WAL 状态：
  BEGIN_PKGS
  ... install A, install B fails ...
  RESTORE_DB /bak → /pkgs
  RESTORE_FILE /bak → /a
  RESTORE_FILE_RM /new/b
  DB <6 个库> :batch-start
  ROLLBACK A 1.0
  END A 1.0
  （COMMIT_PKGS 未写，崩溃在此）

> ⚠️ 订正 2026-09-26：上面这三行的**顺序原文写反了**（原文是 `ROLLBACK`/`END` 在前、
> `DB … :batch-start` 在后）。`batch_rollback()` 的实现顺序是：`reverse_execute` →
> `purge_consumed_stashes` → **`cache.write(":batch-start")`（6 个库）** →
> **逐包 `ROLLBACK`/`END`** → `COMMIT_PKGS`。§2.4 的示例与 §9.1 的步骤表本来就是对的，
> 只有本节这份"现场清单"对调了两段 —— 而它是取证用的，顺序反了会误导复盘。

恢复：
  reverse_execute 逆序（RESTORE_* 行跳过；正向行各自按"备份/目标在不在"幂等处理）：
    DB <6 个库> :batch-start（回滚收尾写的那条）: 它的逆操作与"停在它不跑"**等价** ——
      `.lpkg_db_bak_before::batch-start` 里那份就是批次起点内容，与盘上那份逐字节相同
      （回滚收尾时刚把同一份内容写下去）。**但"等价"只在正式文件确实还在、且仍是批次
      起点内容时成立** → 判据见下方"解决方案"
    ROLLBACK A 1.0 / END A 1.0: 元数据，跳过
    NEW /new/b: 不存在→跳过；BACKUP /bak → /a: /bak 不存在→跳过
    DB A:installed: .bak 不存在→跳过
    其余正向行：备份/目标都已被第一次回滚消费 → 全部跳过
  → 收敛到批次起点状态。**结论：不需要"里程碑提前停止"机制**（`wal_op.hpp` 的
     `reverse_execute` 注释：正常路径下"逆序跑完整个批次"与"停在 :batch-start"逐字节同效）
```

> **订正 2026-09-25**：本节原先把场景 B 结论写成"DB 错误地恢复成了 A 已安装的状态！→ BUG！"
> —— 那条推演不成立（`:batch-start` 备份里是批次起点内容，不是"A:installed 内容"；两份
> 里程碑的备份名也不同，不会互相覆盖）。现行代码甚至明确写着**不存在**"里程碑提前停止"
> 机制，因为"逆序跑完"与"停在 :batch-start"在正常路径下等价。真正需要那条
> `batch_start_db_still_in_place()` 判据的理由是**另一个**窗口（下面的 rename 窗口），
> 与"恢复成 A 已安装"无关。

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

判据**不是**裸 `fs::exists`：启动顺序是 `init_filesystem()`（main_cli.cpp）→ `recover_packages()`，
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
  DB <6 个库> :batch-start → 跳过（判据 `batch_start_db_still_in_place(op)`：文件**仍在位且非空**；
                              在位但为空时再看备份 —— **文件空 + 备份空同样跳过**，只有"空 + 备份非空"才还原）
  ROLLBACK A 1.0          → 跳过（元数据）
  END A 1.0               → 跳过（元数据）
  NEW /new/b              → 不存在，跳过
  BACKUP /a → /bak        → 正向行；逆操作 bak → /a，而 bak 不存在（已被第一次回滚的 RESTORE_FILE
                            消费）→ 跳过（幂等）
  DB <6 个库> A:installed → bak 不存在，跳过（幂等）
  ...（正向操作逆序）...
```
> 这里"反向处理 BACKUP 时 bak 已被 RESTORE_FILE 消费"正是幂等的来源 —— 不是靠"认出哪一行
> 是 RESTORE、哪一行是正向"，而是靠**每条逆操作自己检查备份/目标还在不在**。

**这就是根本问题：reverse_execute 若对 RESTORE 行和正向操作行一视同仁地逆序处理**，RESTORE 行的
逆序就不再是"撤销 restore"（那等于重做正向操作）。**解决方案**：`reverse_execute` 跳过
`RESTORE_*`（含旧名 `REMOVE_*`）行 —— 这些已经是逆向结果，不应该再被逆向。

```cpp
// 在 reverse_execute 循环中（当前实现的形态，见 wal_op.hpp）
if (op.skip_in_reverse()) continue;   // !is_valid() || is_metadata() || is_restore_audit() || CLEANUP

// is_metadata():   ROLLBACK / END / COMMIT / BEGIN / RM_BEGIN / RM_COMMIT / RM_END /
//                  BEGIN_PKGS / COMMIT_PKGS
// is_restore_audit(): RESTORE_FILE / RESTORE_DB / RESTORE_DIR / RESTORE_FILE_RM /
//                  RESTORE_DIR_RM / RESTORE_DB_RM / REMOVE_FILE(旧名) / REMOVE_DIR(旧名)
```

这样 `recover_packages()` 中的 `reverse_execute` 对场景 B：
```
逆序处理（元数据行、RESTORE_* 审计行、CLEANUP、未解析行一律跳过；DB `:batch-start`
          仅在正式文件仍在（且仍持有批次起点内容）时跳过，否则从该里程碑备份还原）：
  只处理正向操作：BACKUP、COPY、NEW、NEW_DIR、DIR_RM、DB（非:batch-start，或 :batch-start 但文件已丢）等
  → 恢复系统到 :batch-start 状态
  → 写 COMMIT_PKGS（`wal::commit_batch()`）
```

**结论**：`reverse_execute` 始终跳过 `RESTORE_*`（含旧名 `REMOVE_*`）、`ROLLBACK`、`END`、
`COMMIT`、`BEGIN`、`RM_*`、`BEGIN_PKGS`/`COMMIT_PKGS`、`CLEANUP` 以及**未解析（INVALID）行**
（判据只有一处：`WALOp::skip_in_reverse()`）。DB `:batch-start` 条目**例外**：只在正式文件仍在
且非空时才跳过（`batch_start_db_still_in_place()`，`wal_op.cpp`），否则它的逆操作要从该里程碑备份
把库还原回来 —— 那是"文件缺失/被清空"这个崩溃窗口的唯一还原依据
（`test_db_batch_start_recovery.cpp` 逐条钉住缺失与"存在但 0 字节"两种现场）。其余正向操作行
（BACKUP、REMOVE_OLD、SAVE_CONF、COPY、NEW、NEW_DIR、DIR_RM、DB/DBNEW/DBRM 非:batch-start）
照常逆向。

### 10.3 关于二次回滚的 RESTORE 行写入策略

`batch_rollback()` 在 reverse_execute **过程中**写入 RESTORE_* 审计行（`wal_append_raw`，
每行 fsync 恒生效）：
- 每完成一个逆向操作 → 立即 WAL: RESTORE_* + fsync
- 这样 WAL 中 RESTORE 行的顺序记录了 rollback 的进度
- rollback 崩溃后 restart 的 `recover_packages()` 中的 `reverse_execute` **跳过**这些行
- **但"哪些逆向操作已完成"不靠这些行判断** —— 判据是**备份/目标是否还在盘上**（每条逆操作
  自查，见 §10.2）。RESTORE 行是**审计痕迹**（取证、复盘、测试断言用），不是恢复的输入。
  （2026-09-25 订正：原来这里写"RESTORE 行的存在/不存在帮助判断哪些逆向操作已完成"，与
  实现相反 —— `skip_in_reverse()` 直接跳过它们，没有任何读取它们的判据。）

---

## 11. rec 恢复（Fallback Only）

### 11.1 定位

`recover_packages()` 是紧急恢复工具，只在进程已死亡（catch 未执行）的场景下使用。

### 11.2 状态机

```cpp
void recover_packages() {          // db/recover.cpp
    // 0. WAL 不存在 / 读不出内容 → 直接返回（没有任何事要做）
    // 1. 读取 WAL 到 lines[]（空行与 \r 先去掉）
    // 1.5 continue_post_commit_cleanup(lines)
    //     —— **必须先做，且早于任何 trim**：删掉"已提交批次"残留的 stash/备份。
    //        它按"第一个未配对 BEGIN_PKGS"划出未提交区域，只清区域**之外**的
    //        BACKUP/REMOVE_OLD 目标与尾部 CLEANUP 行引用的 stash 根；区域内的一个都不碰
    //        （那是下面 reverse_execute 要用的）。一旦先 trim，残留 bak 的来源记录就没了。
    // 2. 状态机扫描（**前向 depth 记账**）：
    //    BEGIN_PKGS  → depth 0→1 的那一个是"未提交区域起点"，++depth
    //    COMMIT_PKGS → --depth（回到 0 即该批次已配对）
    //    结尾 depth > 0 → 存在未提交区域：[第一个未配对 BEGIN_PKGS, EOF)
    //    ⚠ 取"第一个"而不是"最后一个"：一次崩溃可能留下**两个**未提交批次（前一批
    //      回滚自身失败后同进程又开了新批次），按最后一个裁剪会把更早那批的 BEGIN/BACKUP
    //      行当已完成内容丢掉（恢复依据就此消失）。
    // 3. 没有未提交区域 → trim_completed() + cleanup_db_backups() → 返回
    // 4. 对未提交区域（**一次逆序回滚整段**，可能横跨两个未提交批次）：
    //    a) parse_op 解析为 WALOp 列表（只收 is_valid() 的行）
    //    b) reverse_execute(ops, true)——未提交批次**一律回滚**：
    //       CLEANUP 只可能出现在事务之外（post-commit 收尾记录），
    //       事务内不存在 CLEANUP，故不需要"CLEANUP ⇒ 不回滚"的分岔
    //           ├── 跳过未解析行/元数据行/RESTORE 审计行/CLEANUP（skip_in_reverse）
    //           ├── :batch-start DB 条目**仅当正式文件仍在位且非空时**跳过
    //           │   （batch_start_db_still_in_place()，wal_op.cpp；在位但为空时再看备份：
    //           │    文件空 + 备份空也跳过，只有"空 + 备份非空"才还原）
    //           ├── 只执行正向操作的逆向
    //           ├── purge_consumed_stashes(ops)
    //           │     （内含"UNSTASH 反撤销未收敛 ⇒ 整个 stash 根不删"的守卫，见 §9.1）
    //           ├── Cache::load(/*tolerate_missing_set_files=*/true)
    //           │     （容忍"pkgs/holdpkgs 不存在"：一条缺失记录不该把整段恢复作废，
    //           │       但 load 会逐个告警；正常操作路径仍是硬错误）
    //           └── wal::commit_batch()  → 写 COMMIT_PKGS
    //    整段回滚抛异常 → 告警、**这一整段保持未提交**（WAL 与备份原样保留，下次 rec 可重试），
    //    并把 any_batch_failed 置位
    //    ⚠️ 订正 2026-09-26：原文写"单个批次……**继续处理其余批次**"，而**现行没有"批次循环"** ——
    //    rollback_uncommitted_region() 只被调用一次、覆盖整段未提交区域，内部也没有对"批次"
    //    的循环；抛异常即告警 + 返回 true（失败）。"其余批次"没有承载它的控制流。
    //    （本节第 4 步的标题自己写的就是"**一次逆序回滚整段**（可能横跨两个未提交批次）"，
    //    与"逐个批次继续处理"本来就不相容。）
    // 5. cleanup_db_backups() —— **仅当没有批次失败**；而且它内部还有一道独立门控
    //    （WAL 里仍有未配对批次 → 一个都不删），见 §3.1 附注
}
```

（2026-09-25 订正：原状态机漏了 1.5 的续传清理，把"未完成事务"写成了可多个独立批次（现行是
**一整段未提交区域**），漏了 `purge_consumed_stashes` / `tolerate_missing_set_files` / 失败批次
保留现场，且把第 4 步写成无条件 `cleanup_db_backups()`。）

### 11.3 rec 的关键设计决策

| 设计点 | 决策 | 理由 |
|--------|------|------|
| 是否跳过 RESTORE_* 行 | ✅ 跳过 | RESTORE_* 是 rollback 的产物，再次逆序会重做正向操作 |
| 是否处理 :batch-start DB 标记 | ⚠️ **有条件跳过**：正式文件仍在**且非空**才跳过，否则从该里程碑备份还原（`batch_start_db_still_in_place()`，`wal_op.cpp`） | "最终状态"只在该状态确实还在盘上时成立。`write_db_file_wal`/`write_set_file_wal` 的 rename 窗口里正式名已消失（`init_filesystem()` 之后则是"存在但 0 字节"），此时那份备份是唯一还原依据；无条件跳过 = 它永远无人消费，且 `cleanup_db_backups()` 随后把唯一备份删掉（`pkgs`/`holdpkgs` 缺失让恢复整体失败，`files.db`/`provides.db`/`confhashes.db` 静默归零，均不可逆）。现场复现：`test_db_batch_start_recovery.cpp` |
| 是否写 RESTORE_* 审计 | ✅ 是 | rec 的 reverse_execute 应该与 batch_rollback 行为一致 |
| 旧版二进制写入的"批次内 CLEANUP"WAL | ❌ **不支持** | lpkg 经 lpkg 升级时，**旧二进制**会先跑 `recover_packages()` 处理掉遗留 WAL，新二进制才上线；因此更新后不存在需要兼容的旧形状事务。**手工替换 lpkg 二进制不受支持**（若此时正躺着一个被中断的 remove WAL，回滚会让 DB 回到"已安装"而文件已删 —— 遇到时用 `lpkg rec` 前先人工核对） |
| 是否检测 CLEANUP 分岔 | ❌ 不再需要 | CLEANUP 只出现在事务之外（post-commit），事务内不可能有 → 未提交批次一律 `reverse_execute`。post-commit 的残留清理由 `continue_post_commit_cleanup` 负责（**§11.2 步骤 1.5**） |
| 是否清理孤备份 | ⚠️ **有条件**：有批次恢复失败 → 不清理；WAL 里仍有未配对批次 → `cleanup_db_backups()` 自己也不清理（`wal_has_unpaired_batch()` 门控） | `.lpkg_db_bak_before:*` 是"重试还原 DB"的唯一依据。`main_cli.cpp` 的 `lpkg rec` 分支在 `recover_packages()` 之后**无条件**再调一次 `cleanup_db_backups()` —— 门控必须放在函数内部，否则刚被特意保留的还原点会被立刻删光，而 CLI 照打"恢复完成" |
| 未提交区域怎么划 | **第一个**未配对 `BEGIN_PKGS` → EOF，**一次逆序回滚整段** | 一次崩溃可能留下两个未提交批次（前一批回滚失败后同进程又开新批次）。按"最后一个 BEGIN_PKGS"逐轮收敛是错的：已提交批次的行仍在 WAL 里，下一轮起点仍落在它上面，更早那批永远轮不到（实测两轮下来一个文件都没还原） |
| 某个批次回滚失败怎么办 | 告警 + **保持未提交**（WAL/备份原样保留，下次可重试）+ 跳过 DB 备份清理 | 一条失败不该把整段恢复作废，也不该把重试依据删掉。⚠️ 订正 2026-09-26：原文在这行还写了"**继续处理其余批次**"，但现行实现里**没有批次循环**（整段一次回滚，见 §11.2 第 4 步的订正） |
| `lpkg rec` 的退出码 | **恒为 0**（订正 2026-09-26 补）：`recover_packages()` 无返回值，某段回滚失败时只打 `warning.rollback_remove_failed`；`run_rec_command()` 不读任何失败信号，`handle_command()` 对 `CMD_REC` 恒 `return 0`。于是"回滚失败 + 备份被保留 + 打印恢复完成 + 退出码 0"是可能组合（DB 备份的内部门控保证不会误删，所以是**报告不准**而非数据丢失）。对照：`remove_packages()` 为"脚本凭退出码区分"专门抛 `error.removal_refused` —— 这一处的取向恰好相反，**别拿退出码判断恢复是否成功** |

---

## 12. WAL Trim

### 12.1 逻辑

```cpp
void trim_completed() {              // db/recover.cpp
    // 1. WAL **不存在 / 读不出内容** → 直接返回，**不删任何东西**
    //    （订正 2026-09-26：原文把这一支写成"删掉文件"，那是把"文件存在但为空"混了进来 ——
    //     read_wal_lines 返回 nullopt（路径不存在/打不开）→ return；只有文件**存在且为空**
    //     才 fs::remove(wpath)。与 §11.2 步骤 0 的写法对齐。）
    // 2. **前向 depth 记账**找"**第一个**未配对 BEGIN_PKGS"的位置：
    //    BEGIN_PKGS  → depth 0→1 的那个位置记下来；++depth
    //    COMMIT_PKGS → --depth；回到 0 即该批次已配对 → 位置清空
    //    ⚠ 必须取"第一个"而不是"最后一个"：一次崩溃可能留下两个未提交批次（前一批
    //      回滚失败后同进程又开新批次），按最后一个裁剪会把更早那批的 BEGIN/BACKUP
    //      行当已完成内容删掉 —— 恢复依据就此消失（与 continue_post_commit_cleanup、
    //      recover_packages 的区域判定同一套记账）。
    // 3. 有未配对区域：
    //    · 位置 == 0 → 没有可清理的已完成批次，**整个文件原样保留**
    //    · 否则把 [位置, EOF) 的行写进 .trim_tmp → fsync → rename 回 WAL（I-FSYNC-5）
    // 4. **没有**未配对区域（所有事务都已提交）时，**不无条件清空**：
    //    先扫一遍所有行 —— 只要有 BACKUP/REMOVE_OLD 的目标、或尾部 CLEANUP 行引用的
    //    stash 根**仍在盘上**（`exists_no_follow` —— lstat 语义，**等价于**
    //    `fs::exists || fs::is_symlink` 但**不抛**；订正 2026-09-26：原文引的是后者的写法，
    //    而它是**抛**的，行里的 bak 路径可能正是**符号链接环**（原路径是环、被 rename 进
    //    stash 后依然是环）→ 那条写法在它上面抛 filesystem_error，于是**每次启动的 trim
    //    都失败**、WAL 永远裁剪不掉），就说明 post-commit 清理还没
    //    做完 → **保留整个文件**（BACKUP 上下文还在，`recover_packages` 会续传），
    //    什么都不裁；只有全都删干净了才 `trunc` 清空整个日志。
}
```

（2026-09-25 订正：原文写"最后一个未配对的 BEGIN_PKGS 及其所有行保留 / 如果没有未配对
BEGIN_PKGS 清空整个日志"，两处都与现行实现不同 —— 取的是**第一个**未配对，且"无未配对"
还要先过第 4 步的 pending 检查。）

### 12.2 与 RESTORE 审计行的交互

trim 只按 `BEGIN_PKGS`/`COMMIT_PKGS` 配对跟踪批次边界，不关心块内行内容：RESTORE_*、
CLEANUP 等行在已完成事务中随整块被清掉，未提交区域里的行（含 RESTORE_*）一律保留。

**但它确实会看两类行的"落点"**：第 4 步要检查 `BACKUP`/`REMOVE_OLD` 的目标与 `CLEANUP`
引用的 stash 根是否还在盘上 —— 那是"post-commit 清理完成没有"的唯一判据
（`continue_post_commit_cleanup()` 用同一份信息续删）。
（订正 2026-09-25：原文写"trim 不关心具体行内容，只跟踪 BATCH 边界"，与第 4 步不符。）

---

## 13. 实现清单

所有阶段均已完成，具体实现如下。

### 第 1 阶段：基础设施

- **1.1 `DbMilestone`** — 定义于 `wal_op.hpp`。格式 `pkg:state`，`:batch-start` 表示批次开始前的 DB 快照。
- **1.2 `Cache::write` write-ahead** — 实现于 `cache.cpp`。顺序：WAL → fsync → 备份（`.lpkg_db_bak_before:<milestone>`）→ fsync → .tmp → fsync → rename → fsync。`DB` / `DBRM` 的逆操作在备份不存在时跳过（原文件还在，安全）；⚠️ **`DBNEW` 不同**（订正 2026-09-26：原文把这条概括成"`reverse_execute` 遇到备份不存在时跳过"，对 `DBNEW` 不成立）—— `DBNEW` 是"该路径上原本**没有任何东西**"，所以它有**两支**：有备份则还原（`Guard::DbBakExists`），**没有备份则删除该文件**（逆操作 + `Guard::TargetExists`，审计行 `RESTORE_DB_RM`）。因为 `DBNEW` 里没有"原文件还在"这回事。
- **1.3 `WalWriter`** — 实现于 `transaction_log.hpp/cpp`。每行 O_APPEND + write + fsync，带 move 语义。
- **1.4 `parse_op`** — 实现于 `wal_op.cpp`。解析 **33 个行前缀**（`TYPE_MAP`，见 §2.1；订正 2026-09-26：原文写 28，那是数了表格**行**数 —— `REMOVE_FILE`/`REMOVE_DIR` 那一行装两个前缀，另有本轮新增的 4 个），支持 `→` 分隔符与从右往左的尾部字段分帧（路径可含空格）。未知类型 → `INVALID`（`is_valid()` 为假），**绝不借用真实类型当哨兵**。
- **1.5 `reverse_execute`** — 实现于 `wal_op.cpp`。跳过未解析行/元数据行/RESTORE 审计行/CLEANUP（判据只有 `WALOp::skip_in_reverse()` 一处）；`:batch-start` DB 条目**只在正式文件仍在位且非空时**跳过，否则从该里程碑备份还原（`batch_start_db_still_in_place()`：四分支 —— 缺 / 非空 / 空+备份空 / 空+备份非空，见 §11.3 与 `test_db_batch_start_recovery.cpp`）。**已表驱动**（订正 2026-09-26：原文按"逐类型的 if/else"描述）：一张 `UNDO_TABLE`（现行 **16** 行）+ `apply_row()` —— 幂等判据 / 撤销动作 / 计数 + 审计行三件事各只剩一处施加点；表内容见 §9.2。每步逆向操作后写入 RESTORE_* 审计行（`write_audit=true` 时）。**没有**"里程碑提前停止"机制（正常路径下"逆序跑完整个批次"与"停在 :batch-start"逐字节同效，故不需要）。
- **1.6 `extract_current_batch_ops`** — 实现于 `wal_op.cpp`（签名收 `wal_path`）。**从文件末尾反向**找最后一个 `BEGIN_PKGS`，取它到文件末尾的有效行；若先遇到 `COMMIT_PKGS`（块已配对）→ 返回空。
- **1.7 `batch_rollback`** — 实现于 `wal_op.cpp`，**返回 bool**。流程：`extract_current_batch_ops`（为空 → 告警 + `return false`，**不写 COMMIT_PKGS、保留现场**）→ `reverse_execute(ops, true)` → `purge_consumed_stashes(ops)` → `Cache::load()` 重载磁盘 DB → 写 DB `:batch-start`（6 个库）→ 写 ROLLBACK/END 标记（版本号从 WAL 的 BEGIN/RM_BEGIN 行取）→ `COMMIT_PKGS`。（原文的"清理内存 cache"这一步已不存在：曾逐包 `remove_installed`，随后必被 `load()` 覆盖，是死代码。）

### 第 2 阶段：安装事务

- **2.1 `run_batch_transaction`** — 模板定义于 `batch_transaction.hpp`。函数第一行先 `trim_completed()`。正向：`BEGIN_PKGS` → `Cache::write(":batch-start")` → 逐包执行 → `wal::commit_batch()`（写 `COMMIT_PKGS`）。异常路径：catch → `batch_rollback` →（返回 true 才）`cleanup_db_backups()` + `trim_completed()` → rethrow。
- **2.2 `install_packages`** — 重构于 `package_manager.cpp`。**元数据一致性重解析在批次循环内部**（下载后比对真实 metadata 与索引，不一致就重解 + `i = 0` 重启游标），没有外层重试循环。实际安装封装在 `run_batch_transaction` 中，每包后 `Cache::write(pkg + ":installed")`。安装完成后把 `task.get_stashes()` 收进批次向量，交给 `finish_committed_batch()` 统一清理（`CLEANUP` → `remove_all`）。
- **2.3 `InstallationTask`** — **已按"趟"拆成 4 个 TU**（订正 2026-09-26：原文写"重构于 `installation_task.cpp`"，而该文件现在只剩骨架 + 冲突引擎；`backup_existing_files` / `copy_package_files` / `commit_without_file_ops` 三个**都已不在**它里面）。现行分布：
  - `installation_task.cpp` — 骨架：`run()`（写 WAL `BEGIN`/`COMMIT`/`ROLLBACK`/`END` 标记）、`prepare()`、`rollback_files()`、`download_and_verify_package()`、`extract_and_validate_package()`、`ensure_dependencies_satisfied()`、`check_for_file_conflicts()`，以及冲突引擎 `dir_tree_entirely_ours()` / `collect_content_conflicts()`。
  - `installation_task_letgo.cpp`（让开趟）— `backup_existing_files()`（`BACKUP`/`SAVE_CONF`/`NEW`/`NEW_DIR`，write-ahead）、`remove_obsolete_files()`（`REMOVE_OLD`/`DIR_RM`（升级废弃条目）/`SAVE_CONF`（废弃 `/etc` 条目）/`DropOwnership`，**跑在写入之前**，§6.2）、`revoke_undeclared_xattrs()`（§6.3）。
  - `installation_task_copy.cpp`（写入趟）— `copy_package_files()`（`COPY`；`/etc` 条目先走三哈希分流，§6.3；目录状态经 §3.8 的三个原语）。
  - `installation_task_register.cpp`（注册趟）— `commit_without_file_ops()`（**只有** `register_package()` + `install_hook_files()`）、`register_package()`、`install_hook_files()`（`BACKUP`/`COPY`/`NEW_DIR`）。
  `rollback_files()` 只写 `ROLLBACK`/`END` 标记（**单撤销路径**，文件撤销统一由 `batch_rollback` → `reverse_execute` 完成，见 §6.5）。`.lpkg_bak` 延迟到批次提交后清理（stash 化：`get_stashes()` → `finish_committed_batch`）。

### 第 3 阶段：移除事务

- **3.1 移除的批次实现** — `package_manager.cpp` 的 `remove_packages_in_one_batch()` 是**所有**移除路径的汇合点（`do_remove_package` 的唯一调用点）：`remove_package`（单包）、`remove_packages`（`remove a b c`）、`autoremove`、`force_solve_conflict` 经 `remove_packages_checked`（筛选未安装/essential/反向依赖采用**全或无**：任一被拒则一个都不删、连事务都不开）进来，`remove_packages_recursive`（闭包）自带筛选后直接进来。先 `check_removal_preconditions()` 整批检查，再**整组一个批次**逐包 `do_remove_package`（`RM_BEGIN` → 阶段 A `SAVE_CONF`/`BACKUP` → `remove_package_files` → 阶段 B `DIR_RM` → `DBRM`（deps/needed_so/man）→ DB（6 个库）→ `RM_COMMIT` → `RM_END`），调用方随后 `finish_committed_batch()`（post-commit：`CLEANUP` 行 → 删 stash → 删被移除包的 hooks → 剪枝 hooks → postinst → trim → `cleanup_db_backups`）。逐包各开批次会失去跨包原子性（中途 Ctrl+C 只回滚当前包）。
  （订正 2026-09-25：原文把这段记在 `remove_packages_checked` 名下、并称 `remove_package_recursive` 也经它 —— 后者实际直接调 `remove_packages_in_one_batch`；顺序里也漏了阶段 B 的 `DIR_RM`。）
- **3.2 `remove_package_recursive`** — 重构于 `package_manager.cpp`（实现是 `remove_packages_recursive`）。算出依赖闭包后走 §3.1 的同一批次机制：闭包内所有包同一个 `run_batch_transaction`，任一失败整批回滚；闭包里的 essential 包剔除并告警。

### 第 4 阶段：升级事务

- **4.1 `upgrade_packages`** — 重构于 `package_manager.cpp`。整批升级封装在 `run_batch_transaction` 中。每包升级重用 `InstallationTask`（`old_version_to_replace` 设置）。升级完成后把 `task.get_stashes()` 交给 `finish_committed_batch()` 清理。

### 第 5 阶段：rec、trim、二次回滚

- **5.1 `recover_packages`** — 实现于 `recover.cpp`。状态机（现行形态见 §11.2）：续传 post-commit 清理 → 前向 depth 记账划出**未提交区域**（第一个未配对 `BEGIN_PKGS` → EOF）→ `reverse_execute(ops, true)` → `purge_consumed_stashes` → `Cache::load(tolerate_missing_set_files=true)` → `wal::commit_batch()`（写 `COMMIT_PKGS`）；某段恢复失败则保留现场、跳过 DB 备份清理。`:batch-start` DB 条目**只在正式文件仍在且非空时**跳过，否则从该里程碑备份还原（`batch_start_db_still_in_place()`，§11.3）。通过 CLI `lpkg rec` 或启动时自动调用。
- **5.2 `trim_completed`** — 实现于 `recover.cpp`。**前向** depth 记账找**第一个**未配对 `BEGIN_PKGS`，保留它及其后的行、删掉之前的已完成批次；全部已配对时还要先确认 post-commit 清理没有未完成的 bak（`post_commit_cleanup_pending()`），否则**整个文件保留**（§12.1）。

> **recover 侧的四个函数名（订正 2026-09-26 补：全文此前从没出现过它们 —— 照 §11.2/§12.1
> 去代码里找落点会找不到）**：
> · **`scan_batch_pairing()`**（`recover.cpp`）—— "**第一个**未配对 `BEGIN_PKGS`"这条规则的
>   **唯一实现**，四个消费者共用：`continue_post_commit_cleanup` / `recover_packages` /
>   `trim_completed` / `wal_has_unpaired_batch`。§11.2 与 §12.1 各用一大段文字**复述**它，
>   描述与它一致，但改代码时请认这个名字；
> · **`rollback_uncommitted_region()`** —— 对未提交区域**一次逆序回滚整段**（§11.2 第 4 步）；
> · **`post_commit_cleanup_pending()`** —— trim 前判断"post-commit 清理是否做完"（§12.1 第 4 步），
>   判据用 `exists_no_follow`（lstat 语义，不抛）；
> · **`rewrite_wal_tail()`** —— trim 的落盘动作（写 `.trim_tmp` → fsync → rename 回 WAL）。
- **5.3 二次回滚测试** — 覆盖于 `tests/integration/test_breakpoints.cpp`（模拟 rollback 中途崩溃：RESTORE_DB 后 / rollback 完成后 COMMIT_PKGS 未写，验证 `recover_packages` 幂等续传）与 `tests/integration/test_db_batch_start_recovery.cpp`（`:batch-start` 库"缺失/存在但 0 字节"两个窗口的现场复现）。

### 第 6 阶段：测试

- **6.1 断电模拟** — `tests/integration/test_breakpoints.cpp`（20 tests，2026-09-26 计数）。
  断点总数 **19**（第③步补了"符号链接落位 / 目录落位 / `UNSTASH`"三个空档：`symlink_after_wal_<pkg>`、
  `newdir_after_wal_<pkg>`、`unstash_after_wal_<pkg>`；2026-09-26 又补**目录状态**那三个窗口：
  `dirmeta_after_wal_<pkg>` / `xattrset_after_wal_<pkg>` / `xattrrm_after_wal_<pkg>`，见 §3.8；
  2026-09-26 再补**废弃搬运**那一个：`remove_old_after_wal_<pkg>`（`REMOVE_OLD` 行已落、
  rename 未做 —— 此前只有 `backup_obsolete` 没有 `after_wal_breakpoint`，是断点覆盖之外的洞）；
  完整清单见 `CLAUDE.md` §2）——
  第③步那三个由 `tests/integration/test_unstash_primitive.cpp` 的 `UnstashBreakpointTest`（6 tests）
  覆盖，WAL 行路径 confinement 由 `tests/integration/test_wal_confinement.cpp` 的
  `WalConfinementTest`（9 tests）覆盖，目录状态那三个由 `tests/integration/test_dir_state_wal.cpp`
  （16 tests）覆盖。
  覆盖断电窗口（按当前用例名）：文件 BACKUP 的 rename 前后两个窗口（`FileBackupPowerLoss_BeforeRename` / `_AfterRename`）、COPY 的 rename 前后两个窗口（`CopyPowerLoss_Completed` / `_BeforeRename`）、DB 写入的"备份在位 / 备份不在"（`DbWritePowerLoss_BackupExists` / `_NoBackup`）、`DbNewRmRecovery`（DBNEW/DBRM 恢复）、`MixedNewCopyBackupReverse`（NEW/COPY/BACKUP 混合逆序）、`CleanupIsIrreversible`，以及 rollback 中途崩溃的二次/三次续传（`SecondaryRollbackAfter*` / `TripleRecoverIdempotent`）。
  （订正 2026-09-25：原文写"DB 写入（5 个断点）、文件 BACKUP（2 个断点）、COPY（2 个断点）"，与用例实际构成不符 —— 覆盖这些窗口的用例是**手工构造 WAL 状态**（该文件里有"手工构造"一节），不是 `BreakpointManager` 的断点；断点清单里也没有 DB 写入类的命名断点。
  订正 2026-09-26：原文那句"覆盖 `TODO.md §3` 所有断电点"里的 `TODO.md §3` **指不到现行 `TODO.md` 的任何一节** —— 现行 `TODO.md`（7.1.1 恢复的那版）的"§3"是"暂缓（低危/需单独决策）"，里面没有断电点表；"§3 + 断电点表"只存在于**已删除**的那版 `TODO.md`（`git show 21dcd5d8:lpkg/TODO.md`）。此处改成按**用例名**列举，不再引用那个编号。）
- **6.2 幂等性** — `tests/unit/test_wal_core.cpp`（68 tests）。覆盖所有操作类型的 `reverse_execute` 幂等性（NULL→跳过、重复→跳过）。
- **6.3 里程碑链式恢复** — `test_wal_core.cpp` + `test_breakpoints.cpp` + `tests/integration/test_db_backup_chain.cpp` / `test_db_batch_start_recovery.cpp`。验证 DB 备份链 `batch-start ← A:installed ← B:installed` 的正确逆序恢复。
- **6.4 二次回滚幂等** — `test_breakpoints.cpp`。验证 rollback 各阶段中断后 `recover_packages` 能正确继续。
- **6.5 集成测试** — 多个测试文件覆盖：批量安装/移除/升级、依赖链、provides 解析、版本约束、config 保护、SIGINT 保护、并发锁、autoremove、recursive remove。
- **6.6 CLEANUP 阶段测试** — `tests/unit/test_cleanup.cpp`（26 tests）。覆盖 CLEANUP 解析与不可逆性、stash / `DIR_RM` 恢复、随机后缀唯一性、rec CLEANUP 续传、安全检查、现有行为回归。
- **6.7 双重回滚回归（2026-08-03）** — `tests/integration/test_active_rollback.cpp`。升级中途 COPY 失败 / COMMIT 后失败 → 旧文件必须保留（曾双重回滚删旧文件）；CLEANUP write-ahead 崩溃窗口 → 整批可恢复。
- **6.8 全量** — 当前 **984 个测试宏全绿**（docker 容器 `make test`；`grep -rhE 'TEST(_F|_P)?\(' tests/ | wc -l`，2026-09-26 计数：**984 tests / 117 suites / 983 PASSED / 0 FAILED**，1 SKIPPED = `UpgradePropertyTest.SingleSeedReplay`，它是需要显式指定种子的复现入口），覆盖上述全部章节。**该数字随加测试而变，别当契约** —— 要引用它请现数一次；`tests/` 才是唯一事实来源。

---

## 附录：完整 WAL 示例总结

```
成功安装:
  BEGIN_PKGS → ... → COMMIT_PKGS

成功移除（`remove a b c` / `autoremove` / `force-solve-conflict` 走 `remove_packages_checked()`；
**`remove -r` 闭包不走它** —— `remove_packages_recursive()` 自己筛完直接调
`remove_packages_in_one_batch()`（订正 2026-09-26：原文把闭包也算进 `remove_packages_checked()`）。
四条路径的共同点是**整组一个批次**）:
  BEGIN_PKGS → [逐包: RM_BEGIN → (阶段A: SAVE_CONF/BACKUP...) → (阶段B: DIR_RM...) → DBRM...
                → DB(6 个库)... → RM_COMMIT → RM_END] → COMMIT_PKGS
  └─ post-commit 收尾（finish_committed_batch）: CLEANUP <stash 根> → 删除 stash
                                               → 剪枝/删除 hooks → postinst → trim_completed
                                               → cleanup_db_backups

安装失败回滚:
  BEGIN_PKGS → ... A OK → ... B FAIL → ROLLBACK B → END B
  → RESTORE_DB → RESTORE_FILE → RESTORE_FILE_RM → DB :batch-start
  → ROLLBACK A → END A → COMMIT_PKGS

移除失败回滚:
  BEGIN_PKGS → RM_BEGIN → BACKUP... → (异常)
  → RESTORE_FILE... / RESTORE_DIR(重建被 rmdir 的空目录) → DB :batch-start
  → ROLLBACK <pkg> <ver> → END <pkg> <ver> → COMMIT_PKGS
  （⚠️ 订正 2026-09-26：原文漏了 `ROLLBACK`/`END` 两行 —— 移除失败时
   `successfully_installed` = 本批已删完的包，`batch_rollback` 第 5 步会为每个包写
   `ROLLBACK <pkg> <ver>` + `END <pkg> <ver>`（版本号从 `RM_BEGIN` 取）。
   已有回归用例钉住：`tests/unit/test_cleanup.cpp` 的 `BatchRollbackRmBeginVersionExtraction`
   断言 WAL 里出现 `ROLLBACK mypkg 2.5.1` / `END mypkg 2.5.1`。
   "安装失败回滚"那一段本来就是写全的，只有移除段漏了。）

安装时断电:
  BEGIN_PKGS → BEGIN A → BACKUP ... 断电
  → rec: reverse_execute → Cache::load → COMMIT_PKGS

rollback 自身断电:
  BEGIN_PKGS → ... → RESTORE_DB → 断电
  → rec: 跳过全部 RESTORE_* 行, 只按"备份/目标还在不在"继续逆向其余正向行
  → COMMIT_PKGS
```
