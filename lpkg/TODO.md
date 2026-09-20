# lpkg 缺陷修复清单

> 本文档是**一次全量代码审查（6 路并行评审 + 主审逐条复核）的落地清单**，不是架构规范。
> 架构约束见 `ARCH.md`——注意：源码注释里出现的 "TODO.md §N" 指的是**改名前的旧文件**，
> 其内容已并入 ARCH.md（`git show ffb324ee --stat` 可见 `lpkg/{TODO.md => ARCH.md}`）。
>
> **修复准则**
> 1. **能全局修的不局部修**：同一个 bug 在多处出现要一起修；能从实现里抽出公共函数就抽。
> 2. **能优雅的不要突出**：尽量融入原逻辑，不要多出一个突兀的 `if` block。
> 3. 每条修复都附**回归测试**（ARCH.md：遇到什么 bug 就写什么测试）；测试跑在 docker 内
>    （`make test`，容器由 make 自行创建/启动，主机无需装依赖）。
>
> 标记：`[高]/[中]/[低]` = 严重度；「日常」= 普通使用就会踩到；「边界」= 需要特定输入/崩溃路径。
> 审查结论中"已核对不是 bug"的部分不在此列（见审查摘要）。

---

## 已完成

| 批次 | 内容 | 回归测试 |
|------|------|----------|
| A1/A2/A3 | WAL 分帧（尾字段从右锚定）+ 独立 `WALOpType::INVALID` 哨兵 + 回滚守卫（`batch_rollback`/`continue_cleanup` 返回"已收尾"才清理 DB 备份） | `tests/unit/test_wal_framing.cpp`（9+5）、`tests/integration/test_wal_rollback_guards.cpp`（6，含**含空格 root** 的端到端）；3 个旧哨兵测试改契约 |
| B1–B4 | 抽出 `fsync_and_rename()`（utils/cache×2/wal_op 四处共用，fsync 失败即抛）；packer 检查 `archive_write_close` 并丢弃半成品；builder 处理脚本改走 `write_string_to_file`；downloader 检查输出流终态 | 既有 pack/download 测试 + 全量回归 |
| C1–C3 | `safe_name_from_url()`（拒绝 `.`/`..`/空，杜绝 `remove_all(work_root/"..")` 删掉整个构建目录）；构建脚本路径 `shell_quote`；DB 锁 fd 加 `O_CLOEXEC` | `test_builder_executor.cpp::SafeName*`（5） |

| D1–D3 + **D6** | 依赖串统一 trim、丢弃空名/纯操作符片段；索引复合约束按"操作符开头"续接合并；**复合约束的 `,` 不再被吞进版本号**；`install_packages` 增加"目标必须真的进计划"兜底（消灭整族静默假成功） | `test_build_deps.cpp`（2）、`test_repo_index_parsing.cpp`（3）、`test_solver_regressions.cpp::Unreachable*/AlreadyInstalled*` |
| E1–E3 | `--force` 去掉"整批为空"特例（混合目标不再漏重装）；能力目标同时匹配 `provides`（按 SONAME 安装 = 显式请求，hold 住不再被 autoremove 删）；`autoremove` 永不删核心包 | `test_solver_regressions.cpp`（3） |
| F1/F2 + **F4** | `upgrade_packages` 补 `run_all()`（升级也 flush 触发器）；`lib_utils` 惰性 `elf_version`（安装期 ldconfig 触发器不再空转）；缺 `triggers.conf` 时告警一次 | `test_solver_regressions.cpp::UpgradeFlushes*`（真编译 .so + 断言 SONAME 链接被重建） |

| X1–X7 | `strip.cpp` 三处内存安全（共用 `elf_range_within` 饱和比较 + `elf_section_entry_count` 安全除法，校验不过即 `return false` 保留原文件）；归档成员名归一化 + 硬链接目标强约束；SONAME 链接路径 `path_within` 校验；包名/版本号 `is_safe_path_component` 校验；归档自报大小上限；**恢复区域改为"第一个未配对 BEGIN_PKGS"**（`continue_post_commit_cleanup` 与 `recover_packages` 两处）；WAL 父目录 fsync | `test_archive_confinement.cpp`(6)、`test_elf_stripping.cpp::CraftedElfTest`(4)、`test_solver_regressions.cpp`(2)、`test_wal_rollback_guards.cpp::RecoveryOfLaterBatch*` |
| C4 / G1 | 源码下载改"先下 `.part` 再 rename"（被中断的构建不再留下"已下载好"的截断包）；`query <目录>` 不带尾斜杠也能查到归属 | `test_builder_executor.cpp`(2)、`test_reinstall_query.cpp`(1) |
| **Y1**（用户实测报告） | **多包移除失去跨包原子性**：`remove a b c` / autoremove / force-solve 都是逐包 `remove_package()`，等于**每包一个批次** → 中途 Ctrl+C 只回滚当前包，之前已提交的删除保持删除。修法：抽出 `remove_packages_checked()`（一个批次，全部删完才写 CLEANUP）作为**唯一**多包移除实现，`remove_package`/`autoremove`/`remove_package_recursive`/`force_solve_conflict`/`main` 全部改走它 | `test_solver_regressions.cpp::MultiPackageRemove*`、`::AutoremoveRollsBack*`（在"逐包一批"的旧实现上确认会红） |
| H1（按用户要求） | `lrepo-mgr.py` **整体删除在线模式**（S3/SCP/远程索引下载），只保留本地仓库构建；索引改为本地直接读写 + `.tmp` rename 原子替换；**读失败硬失败**（不再与"新仓库"混淆）；空索引时 `cleanup` 拒绝删除；`cleanup --dry-run` | 手工端到端验证（push 保留既有索引 / 索引不可读时 exit 1 / 空索引不清理 / dry-run） |

| **Y2**（用户决策） | **remove 的 cleanup 统一到"批次提交后"**（与 install/upgrade 同款）：抽出 `finish_committed_batch()` 供三条路径共用；stash（= 移除的唯一回滚来源）活到提交之后才清。语义变化：**只要批次未提交，remove/autoremove 中途中断一律整批恢复**（旧设计在"全部包已删完、尚未提交"窗口里已写 CLEANUP → 中断不回滚、包保持已删）。清理失败改为仅告警（批次已提交、DB 一致），残留由 WAL 的 CLEANUP 记录 + `rec` 续传 | 新增 `RemoveInterruptedBeforeCommitRollsBackEverything`（在"cleanup 挪回批次内"的旧设计上确认会红）；按新语义更新 `CleanupFailureAfterCommitDoesNotUndoRemoval`、`RemoveWritesSingleCleanupPerStash` |
> 验证方式：每条新测试都先在**回退 src 改动的旧代码**上跑过一遍，确认会红（C1 为一类回归测试，A 组为端到端行为级）；D/E/F 组：13 个新测试里 **10 个在旧代码上确认会红**（另 2 个是刻意的正向对照，1 个受 libelf 全局状态影响）。共 608 个用例全绿。


---

## 一、日常使用就会踩到（按修复批次分组）

### A. WAL 分帧与哨兵 —— 一次改动覆盖 3 条

同源根因：**序列化格式用了未转义的分隔符，而字段内容里允许出现该字符**（详见 D 组）。
修法：把参数切分收进 `parse_op`（类型感知：单参数取整段剩余；多参数从右往左数固定字段），
并给解析失败的行一个**独立的** `WALOpType::INVALID`，替掉当前"`type=BEGIN_PKGS` 哨兵 + `arg1="__INVALID__"`
字符串"的把戏。这样同时消掉下面三条，并让全仓 5 处 `arg1 == "__INVALID__"` 判断收敛成类型判断。

- [x] **A1** `[高]` **WAL 空格分隔导致回滚静默跳过 DB 恢复**（日常）
  `src/db/wal_op.cpp:106-116` `split_line()` 只对 `A → B` 箭头形式保空格，`NEW/NEW_DIR/DIR_RM/DB/DBNEW/DBRM`
  一律按空格切；`cache.cpp:436/478` 写的 `DB <path> <milestone>` 路径含空格即被截断 →
  `wal_op.cpp:189-192` 重算出的备份名不存在 → `:327-340` "bak 不存在 → 跳过" → **DB 回滚被静默跳过**，
  随后 `cleanup_db_backups()` 把真备份删掉。
  必然触发：`lpkg --root "路径含空格" install X`（`config.cpp:54-75` 把 state_dir 重定基其下）。
  *测试*：各 op 类型带空格路径的 parse/reverse 往返；`--root` 含空格的端到端回滚。

- [x] **A2** `[高]` **破损尾部行被解析成 BEGIN_PKGS 哨兵 → 整批回滚失效 + DB 备份被删**（边界）
  `src/db/wal_op.cpp:133-136` 的哨兵 `type=BEGIN_PKGS` 与真实类型撞车，而 `extract_current_batch_ops`
  的反向扫描（`:412-420`）漏了 `__INVALID__` 判断（同函数收集循环有）→ `ops` 为空 →
  `batch_rollback` 在 `:523` 直接 return（不写 COMMIT_PKGS、不回滚），紧接着
  `batch_transaction.hpp:97` **无条件** `cleanup_db_backups()` 删掉本批 DB 备份 → 文件可还原、DB 永不可还原。
  *测试*：尾部撕裂行 + 后续 `recover_packages()`；断言 DB 备份未被误删、批次仍可恢复。

- [x] **A3** `[中]` **未回滚就不该清理 DB 备份**（边界）
  `src/db/batch_transaction.hpp:84-98`：`batch_rollback` 空转（`ops.empty()`）时也应保留备份，
  只有"确实消费了备份"的路径才 `cleanup_db_backups()` + `trim_completed()`。
  修法：让 `batch_rollback` 返回是否真正执行了回滚，由调用方据此决定清理。

### B. 写错误一律不许静默成功

同源根因：**`.tmp` → fsync → rename 的助手丢了 fsync 返回值**，以及各处"写完不检查流状态"。
修法：**加固唯一的原子写助手**（检查 write/fsync/rename，失败即抛），让 `cache.cpp`、`wal_op.cpp`、
`utils.cpp` 的复制实现全部改走它；再把"写失败仍报成功"的几处补齐检查。

- [x] **B1** `[中]` `src/db/cache.cpp:356-364`（`atomic_write_with_fsync`）与
  `src/db/wal_op.cpp:483-488`（`write_string_file_wal`）、`src/base/utils.cpp:358-374`（`write_string_to_file`）：
  `open(tmp)` 失败即**完全不 fsync**，`fsync()` 返回值一律丢弃，却照样 `rename` → ENOSPC/EIO 时
  截断内容成为 DB 正文，下一轮还被当作"备份"。
- [x] **B2** `[中]` `src/archive/packer.cpp:144-152`：`archive_write_close` 返回值丢弃（实测写失败返回 -30），
  随后照样打印 pack success 并对**截断后的文件**算 SHA256 → farm 把该哈希写进索引，下游校验通过。
  异常分支也不删半成品。
- [x] **B3** `[中]` `src/build/builder.cpp:320-325`：处理后的构建脚本写出时不检查流状态（违反
  `utils.cpp` 自己立的规矩）。
- [x] **B4** `[低]` `src/archive/downloader.cpp:78-121`：输出流从不显式 flush/close、也不检查最终状态。
  *测试*：注入写失败（只读/满的目录、`/dev/full`）断言抛错而非"成功"。

### C. 构建路径

- [x] **C1** `[高]` **从 git URL 推导的 `remove_all` 会删掉整个构建目录**（边界，输入由 tracker 机器生成）
  `src/build/builder_executor.cpp:283-290`：URL 以 `/..` 结尾 → `filename()==".."` → `dest==work_root/".."`
  → 实测把父目录**所有条目**删光（含 `LankeBUILD.json`、已下载源码）；以 `/` 结尾 → `name` 为空 → 清空 `work_root`。
  修法：抽一个"URL → 安全目录名"的公共函数（`clone_git_source` 与 `download_one` 共用），
  拒绝空/`.`/`..` 等非法结果。
- [x] **C2** `[中]` `src/build/builder_executor.cpp:564-568`：构建脚本路径**未加引号**裸拼进 `bash -c`
  （紧邻的 flag 却调了同文件的 `shell_quote()`）→ 目录含空格构建失败、含 `;`/`$()` 以 root 注入。
  修法：直接用现成的 `shell_quote()`。
- [x] **C3** `[中低]` `src/base/utils.cpp:238`：DB 锁 fd 缺 `O_CLOEXEC`，fork/exec 的子进程全部继承
  （`lpkg build` 全程持锁）→ 比 lpkg 活得久的子进程会让之后每次 lpkg 报 `error.db_locked`。
- [x] **C4** `[中低]` `src/build/builder_executor.cpp:421-431`：被中断的构建留下的截断源码包被
  `if (!fs::exists(dest))` 永久信任；自动解压失败还只降级为 warning。

### D. 依赖解析：消灭"静默假成功"

同源根因：**空包名依赖 → libsolv `ID_EMPTY` → 既不解析也不报错 → 事务为空 → 打印"所有包都已安装"、exit 0**。
落点 `src/pkg/package_manager.cpp:171-174`。三层一起修（缺一层都还会漏）。

- [x] **D1** `[高]` `src/vercmp/dep_parser.cpp`：解析出**空名**的依赖一律丢弃，并在两处分支统一 trim
  （当前只弹尾部空格、无操作符分支连尾部都不弹 → 手写 `build_deps` 多一个空格就变成"找不到包 ` cmake`"）。
- [x] **D2** `[高]` `src/repo/repository.cpp:100-105`：索引按 `,` 拆 deps，而依赖语法本身用 `,` 表达复合约束
  （`tests/integration/test_build_deps.cpp:50` 把 `"cmake >= 3.20, < 4.0"` 钉成合法）→ 拆出的 `"< 4.0"`
  成了空名依赖。修法：以"非操作符开头"为判据把续接片合回前一条（**向后兼容，无需改索引格式**）。
- [x] **D3** `[高]` `src/pkg/package_manager.cpp:171-174`：`plan.empty()` 直接宣布"均已安装"——
  请求的目标若没进计划应当**报错**（这是整族静默假成功的公共兜底）。`upgrade` 同理。
- [x] **D4** `[中]` `src/repo/repository.cpp:59-67`：`load_index` 只在**文件不存在**时告警；文件存在但为空/
  是目录时 open 不检查、解析出的包数不校验 → 0 个包且零诊断 → `lpkg upgrade` 打印"所有包都已是最新版本"、exit 0。
  加重项：`src/config/config.cpp:219-226` 取 `mirror.conf` 首行原样使用（不处理注释/`\r`/空白）。
- [x] **D5** `[中]` `src/pkg/solver.cpp:130-137`：问题分类先看名字形状、后看 JOB/PKG，顶层目标
  `libghost.so.1` 被误判成可容忍的 missing_so；配 `--missing-so-no-error` → 求解成功但事务为空、exit 0。

- [x] **D6** `[高]`（**写 D2 测试时发现的额外 bug**）`src/vercmp/dep_parser.cpp`：版本扫描只在
  遇到操作符时停止，**不认复合约束的 `,` 分隔符** → `"cmake >= 3.20, < 4.0"` 解析出
  `constraints[0].version == "3.20,"`（带逗号）。版本比较对一个带逗号的版本号必然失败 →
  依赖被误判为"不满足"（触发无谓升级/重装或报依赖无提供者）。既有 `CompoundConstraint`
  测试只断言了 name，没断言 version，所以一直没暴露。已修：`,` 参与分帧 + 约束间跳过 `,`。
- [x] **F4** `[中]`（**写 F1 测试时发现的额外 bug**）`src/trigger/trigger.cpp:41`：`/etc/lpkg/triggers.conf`
  **缺失时直接 return 且不告警** → 所有触发器静默失效（连内部 ldconfig 分支也不执行，因为它
  同样由配置里的命令名驱动）。已修：首次缺配置时告警一次（**不置 `config_loaded`**，
  否则之后创建的配置文件永远不会被加载 —— 我第一版就是这么写的，当场打挂了
  `FeatureTest.TriggerActivation`）。

### E. 求解与包管理行为

- [x] **E1** `[高]` `src/pkg/solver.cpp:562-570`：`--force` 的补回只在**整批事务为空**时触发 →
  `lpkg install --force A B`（A 已当前版本）**静默不重装 A**。修法：去掉"整批为空"特例，
  改为对每个目标做"已在 order 中则跳过"的补回（顺带去重）。
- [x] **E2** `[高]` `src/pkg/install_common.cpp:276-282`：`is_explicit_target` 拿**原始目标串**与解析出的
  包名精确比较 → 按能力/SONAME 安装（`lpkg install libssl`）被记成"依赖"、不 hold →
  **紧接着一条 `autoremove` 就把它删掉**；能力目标上的 `--force` 也一起失效。
  修法：判定同时匹配目标的 `provides`。
- [x] **E3** `[中]` `src/pkg/package_manager.cpp:568`：`autoremove` 走 `remove_package(n, true)`，
  `is_essential` 只在 `force=false` 时查 → 列进 `/etc/lpkg/essential` 的包会被自动删除。
- [x] **E4** `[中]` `src/pkg/installation_task.cpp:252-271` + `:662-683`：**文件→目录的升级路径未处理**
  （目录条目见"已存在"就跳过，随后 `ensure_dir_exists` 抛 `error.path_not_dir`，整批中止），
  Python 包 `foo.py → foo/__init__.py` 这类真实升级直接卡死。反向（目录→符号链接）是显式处理的，
  按同样方式补齐即可。

### F. 触发器子系统（两条同源：看着接上了，实际大面积不生效）

- [x] **F1** `[中]` **`upgrade` 不 flush 触发器**：`run_all()` 全仓只有 `package_manager.cpp:286`
  一个调用点（在 `install_packages` 尾部），`upgrade_packages` 尾部（`:778-783`）漏了；
  而 `check_file()` 在升级过程中照常累积 → 进程退出即丢弃。后果：升级后
  `glib-compile-schemas` / `systemctl daemon-reload` / `gtk-update-icon-cache` 都不跑（schema 不可见、
  unit 不生效、图标缓存陈旧）。`reinstall` 因委托 `install_packages` 不受影响。
  *测试*：接线级——断言 `upgrade` 路径确实调用过 `run_all`（testing 模式下可断言日志/加只读访问器）。
- [x] **F2** `[中]` **`ldconfig` 触发器在安装进程里必然空转**：`src/elf/lib_utils.cpp:24` 未调
  `elf_version(EV_CURRENT)`，libelf 会令 `elf_begin` 恒返回 NULL（实测）→ `get_elf_soname` 恒空 →
  `apply_soname_links` 一个链接不建。全仓 `elf_version` 只在 `strip.cpp:49/441`，即"必须先 strip 过"。
  修法：在 `lib_utils.cpp` 内做一次性的惰性初始化，消除调用顺序依赖。
- [x] **F3** `[中]` `src/trigger/trigger.cpp:111-115`：外部触发器不 chroot、不做 root 重定基 →
  `--root /mnt/base` 安装时命令打在**宿主**上（同函数内 ldconfig 分支用的是 `root_dir()`）。
  修法：把 `install_common.cpp` 里已有的 chroot+unshare+exec 逻辑抽成公共助手，两处共用。

### G. CLI 行为

- [x] **G1** `[中]` `src/pkg/package_manager.cpp:925`：`query` 不对目录补尾斜杠，而目录以 `/usr/bin/`
  形式注册 → `lpkg query /usr/bin` 报"不属于任何包"。
- [x] **G2** `[低]` `src/pkg/depend_scanner.cpp:285-292`：用反向依赖表当存在性判断 → 无依赖者的已存在包
  被打印成 `not found in repository`；`:282` 的 `bool /*show_all*/` 说明 `--all` 被丢弃（`main.cpp:212` 在传）。
- [x] **G3** `[低]` `src/pkg/depend_scanner.cpp:98-102`：遇已安装中间节点即停止展开，`depend install`
  的"将要装什么"答案是错的。
- [x] **G4** `[中]` `src/pkg/package_manager.cpp:453-480`：拒绝类操作 log-and-return、**退出码仍为 0**
  → 脚本无法区分"删掉了"和"被拒绝"。（**待定**：属 CLI 契约决策，需确认是否要改为非零退出。）

### H. 仓库运维脚本

- [x] **H1** `[高]` `main/scripts/lrepo-mgr.py:355-370`：`download_index()` 吞掉**任何**异常并返回空索引 →
  `push` 只把本次推的包写进 index.txt 上传（**其他包的索引条目全部消失**）；`cleanup` 按"不在索引里
  就删整个目录"→ 一次网络抖动/凭证过期就 `delete_remote_dir` 掉整个架构目录，且无二次确认。
  修法：区分"索引不存在"与"下载失败"，后者硬失败；`cleanup` 加确认。
- [x] **H2**（**已随需求消失**：按你的要求在线模式整体删除，scp 后端连同 `rm -rf` 拼接一并移除）原记录：`lrepo-mgr.py:264-283` scp 后端把 config 值裸拼进远端
  `rm -rf` / `mkdir -p`，未加引号。

---

## 二、高危但属边界情况（同样要修）

- [x] **X1** `[高]` `src/elf/strip.cpp` 三处内存安全，入口是 `builder.cpp` 对 staging 里任何
  `\x7fELF` 文件的 strip（即上游构建产物）。三处同源（**只按输入大小夹紧、从不校验输出缓冲**），一起修：
  - `:395-396` 堆溢出：`memcpy(out.data(), in.data(), std::min(headers_size, in.size()))` 的
    `headers_size` 可远大于 `output_data.size()`（`:373` 按新节区布局算）→ ASan 实测 128 字节区域写 4320。
  - `:384` 越界：`old_shdr.sh_offset + old_shdr.sh_size <= input_data.size()` 的 uint64 加法可回绕，
    且目标偏移不查。
  - `:128`、`src/elf/lib_utils.cpp:40`：`sh_size / sh_entsize`，`sh_entsize==0` → SIGFPE。
  修法：抽一个"区间校验 + 饱和加法"的公共助手，两文件的安全除法也共用。
- [x] **X2** `[中]` `src/archive/archive.cpp:87`（硬链接 `:99-101`）：归档成员用绝对路径可逃出解压根目录
  （`output_dir / "/etc/x" == "/etc/x"`，`fs::path` 语义已实测）。按评审意见**已从严重降为中危**
  （对"装一个包"不构成提权）；仍成立的是：`--root` 边界被破坏（落点在宿主）、写在 `prepare()` 里
  **早于 WAL 的 BEGIN**（不进 `file_db`、`query` 看不到、`remove` 删不掉、回滚后仍在）、硬链接成员
  能给已存在文件起别名。修法：`ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS` + 成员路径归一化 + 拒绝逃逸的硬链接目标。
- [x] **X3** `[中]` `src/elf/lib_utils.cpp:72-76`：`DT_SONAME` 未校验就当路径用（同一个 `operator/` 陷阱），
  可在任意位置建符号链接。同样已降级；修法：`path_within()` 校验。
- [x] **X4** `[中]` `src/pkg/installation_task.cpp:359-361` + `package_manager.cpp:127-135`：包名/版本号
  未校验就当路径用（版本来自远端索引/CLI，包名来自 .lpkg 的 `metadata.json`）→ 下载落点与
  `tmp_pkg_dir_`/`dep_dir()/name`/`docs_dir()`/`hooks_dir()` 全部可穿越。修法：包名/版本的白名单校验（单一助手）。
- [x] **X5** `[中]` `src/archive/archive.cpp:172-178`：相信归档自报大小 `content.resize(size)`（
  GNU base-256 头可称 1 TiB）→ `std::bad_alloc`/abort；`strip.cpp:337-373` 同类。修法：上限校验 + 异常归一为 `LpkgException`。
- [x] **X6** `[中]` `src/db/batch_transaction.hpp:53-61` + `src/db/recover.cpp:210-258`："
  同时只有一个未提交批次"从未被强制，两个未提交 `BEGIN_PKGS` 会让恢复区域横跨两批，
  `has_cleanup` 分支把前一批**还没还原**的文件 `remove_all` 掉。
- [x] **X7** `[中]` `src/db/transaction_log.cpp:34`、`src/db/recover.cpp:296/351`：WAL 创建/截断/删除
  都不 fsync 父目录（违反 I-FSYNC-3）。

---

## 三、暂缓（低危 / 需单独决策，已记录）

- `[低]` `installation_task.cpp:636`：`/etc` 下"指向目录的符号链接"绕过配置保护分支。
- `[低]` `installation_task.cpp:835-841`：`hooks_dir/<pkg>` 安装/升级前不清空 → 新版本删掉的
  `prerm.sh`/`postinst.sh` 继续被执行。
- `[低]` `install_common.cpp:165-167`（`run_hook` 的 `waitpid` 返回值不查、`status` 未初始化 = UB）
  与 `utils.cpp:155`（不重试 EINTR）。注：glibc `std::signal` 带 `SA_RESTART`，SIGINT 触发 EINTR 概率低。
- `[低]` `builder.cpp:305-308`：`std::system("bash \"<hacks.sh>\"")`（双引号不挡命令替换、`rc` 是原始
  wait status、cwd 不对）——当前 `pkgs/` 无任何 `hacks.sh`，纯潜伏。
- `[低]` `builder.cpp:362-365`：输出 `.lpkg` 名由未校验的 `cfg.name`/版本拼成，可逃出 build 目录。
- `[低]` `builder_config.cpp:94-104`：`build.conf` 的 `CFLAGS=""` 不被当作"未设置"（MAKEFLAGS 会），
  导致 `{CFLAGS}` 与注入环境变量不一致。
- `[低]` `builder_executor.cpp:36-47`：`parse_git_url` 按最后一个 `@` 切分，认证型 URL 解析坏。
- `[低]` `builder_executor.cpp:446-451` / `downloader.cpp:128-141`：解压失败只 warning；`max_retries=0` 静默返回。
- `[低]` `packer.cpp:57-67`：`lstat` 成功但 `open` 失败时按 stat 大小写头、无数据 → 零填充文件。
- `[低]` `downloader.cpp:89-96`：找不到 CA 包时静默关闭 TLS 校验（注释写明是权衡，建议改硬失败）。
- `[低]` `strip.cpp:242-252/263`、`:124,130`、`lib_utils.cpp:35/42`：`const_cast` 改写 const 输入、
  `elf_update` 返回值忽略后按部分长度 resize（静默截断原二进制）、`gelf_get*` 未查。
- `[低]` `version.cpp:75-83`：`from_libsolv_evr` 只还原 `-` 之前的 `~`，`<ver>+<rel 含 ->` 不往返。
- `[低]` `repository.cpp:76-79`：索引第三字段（包级 provides）farm 会写、lpkg 没人读。
- `[低]` `config.cpp:144-153`：`has_system_soname` 未做路径约束（只让质量守卫放行，非提权）。
- `[低]` `localization.cpp:51-57`（缺键不回退英文）、`:30-33`（`readlink` 截断当成功）。
- `[低]` `utils.cpp:540,585`：`std::stoi` 接受数字前缀 → `/tmp/lpkg_1234junk` 会被当自己的目录删。
- `[低]` `scanner.cpp`：范围 for 自增在 try 之外；`fs::relative` 解析符号链接导致假孤儿。
- `[低]` `test_breakpoints.cpp:28-37`：action 抛异常时 `erase` 不可达；测试设施。
- `[低]` `depend_scanner.cpp:169-173`：SONAME 只记第一个提供者 → 影响面少算一半。
- `[低]` `solver.cpp:589-593`：`repo_revrequires` 只取第一个提供者（仅测试引用）。
- `[低]` `main.cpp`：`lpkg build a b c` 静默忽略多余参数；`--yes --no` 冲突时静默取 no；
  `curl_global_init` 返回值未查。

---

## 四、根因观察

**"未转义分隔符"在三个格式里重复出现**：WAL 用空格分帧（A1）、索引 deps 用逗号连接（D2）、
索引字段用 `:` 分隔（`:76-79` 的第三字段）。建议不要三条各修各的——A 组改的是**解码器**（向后兼容），
D2 改的是**续接判据**，但如果将来还要加字段，应先定一条统一规矩（转义或长度前缀），否则下一个格式还会重演。

---

## 五、第二轮 fresh-eyes 审查（2026-09-20）新增

### 已修（本轮）

- [x] **Z1 `[中]` WAL 里程碑含空格的残余分帧洞**（`is_safe_path_component` 现拒绝空白）：
  包名会进入里程碑字段（`DB <path> <pkg>:<state>`），带空格时 `reverse_execute` 推出的
  备份名与实际不符 → DB 回滚被静默跳过、真备份随后被 `cleanup_db_backups` 删掉
  （与 A1 同类失效）。触发：`lpkg install ./x.lpkg` 且 metadata.json 的 name 含空格。
  修法一行：`is_safe_path_component` 拒绝 ` \t\n\r`（依赖 WAL 分帧契约"尾字段不含空格"必须被**强制**而非假设）。
- [x] **Z2 `[中]` 测试套件自身的沙盒逃逸**（3 处）：`test_cleanup.cpp` 里
  `RecWithPartialCleanupContinues` / `RecWithPartiallyCleanedDirBakReversesWholeBatch` 的
  `BACKUP` **src 未加 test_root 前缀** → `reverse_execute` 按字面绝对路径 rename，写到**真实
  `/usr/bin`**（容器里实测留下 `/usr/bin/c`）。三处已加前缀；两个用例同时按新语义重写
  （旧的"CLEANUP 在批次内 → 续删不回滚"已随分岔删除而失效）。
- [x] **Z3 `[低]` 既有 `AtomicRemoveTest.RemoveWithDependentsBlocked` 现已通过**（不再复现）。

### 待定 / 待修（有实测证据，未改动）

- [x] **Z4 `[中高]` 删除 has_cleanup 分岔的代价（**需要你拍板**）**：HEAD（已发布）的
  `remove_package` 把 CLEANUP 写在**批次内**并紧接物理删除 stash。若崩溃落在"首条 CLEANUP
  到 COMMIT_PKGS"之间，WAL 里 `BACKUP` 的目标**已被删掉**；新代码一律 `reverse_execute`
  → BACKUP 跳过（文件保持已删）、`DBRM` 复活 deps/needed_so/man、`DB …:removed` 把 DB 回滚成
  "已安装" → **DB 说装了、文件没了、元数据复活**（实测复现）。理由说明：`recover_packages()`
  是遗留 WAL 的**消费者**、它并不"清空"WAL，所以"更新 lpkg 后不存在遗留事务"这个前提只在
  "lpkg 自身经 lpkg 升级"时成立（那时是旧二进制先 rec）。
  两个选项：**(a) 恢复一个窄守卫**（仅当未提交批次内**确实含 CLEANUP** 时才"续删+提交"——
  新代码永不产生该形状，故零代价）；**(b) 保持删除**，并在 ARCH.md 注明"手工替换 lpkg 且
  存在被中断的 remove WAL 时不支持跨版本恢复"。
- [x] **Z5 `[高]` 已闭合（根因是我自己在 X6 里改错的一行）**：恢复的**区域起点**必须取"**第一个**
  未配对 `BEGIN_PKGS`"，我在 X6 里改成了"**最后一个**"，于是每轮都在处理最近那批（其行仍留在 WAL
  里），更早那批**永远轮不到** —— 实测两轮下来文件一次都没被还原（"逐 pass 收敛"是假的）。
  当初担心的"区域横跨两个未提交批次"之所以危险，只因当时那条 `has_cleanup ⇒ continue_cleanup`
  分支会对整个区域 `remove_all`（会删掉前一批尚未还原的 stash）；该分支已删，因此一次性逆序回滚
  整个未提交区域**正确且一轮收敛**（实测：pass 0 还原+purge+commit，pass 1 幂等）。
  同时落地的另两项：`cleanup_orphan_stashes()` 接收并跳过 `wal::referenced_stash_roots()`（WAL 仍
  引用的 stash 绝不回收）；`trim_completed()` 保留起点同样改为第一个未配对。
  回归：`TwoUncommittedBatchesConvergeInOnePass`（生产 stash 布局 + 真实启动序列两轮 + 幂等 + 无残留）；
  旧的 `RecoveryOfLaterBatchKeepsEarlierBatchsBackups` 已删除（它断言的是区域起点取错后的产物）。

- [x] **Z6 `[中]` 恢复卡死**：`reverse_execute` 的 BACKUP 逆操作直接 `safe_rename` 到
  `arg1`，若目标父目录不存在（或路径不可满足）会**抛异常**，`recover_packages` 随之失败 →
  每次启动都重试、所有 lpkg 命令都起不来。修法：还原前 `create_directories(父目录)`，
  或把不可满足的还原降级为告警+跳过（与"bak 不存在 → 跳过"一致）。
- [x] **Z7 `[低]` `hooks_dir/<pkg>` 在可回滚批次内被 `remove_all` 且无 WAL 记录**
  （`package_manager.cpp:473`）：批次回滚后 A 的 hooks 永久丢失 → 之后 remove/upgrade
  静默跳过钩子。修法：把 hooks 删除挪到提交后（与 stash 清理同阶段）。
- [x] **Z8 `[低]` `autoremove` 在批次回滚后仍打印"完成"并 exit 0**（与 G4 同类）。

---

## 六、低危批：已修清单（2026-09-20 第三轮）

- [x] `std::stoi` 数字前缀（两处）→ 新增 `parse_pid_strict`（全数字才接受；`/tmp/lpkg_1234junk` 不再被误删）
- [x] i18n：**按 key** 回退英文（此前缺单个 key 直接吐 `[MISSING_STRING]`）；`readlink` 截断不再当成功
- [x] `parse_git_url`：ref 必须位于最后一个 `/` 之后（`git+ssh://git@host/x.git@v1` 与 `git+https://user@host/repo.git` 都能正确解析）
- [x] `download_with_retries`：`max_retries<1` 视作 1（此前静默什么都不下载）
- [x] `packer`：普通文件 `open` 失败即报错（此前静默写出零填充文件）
- [x] `build.conf` 的空 `CFLAGS/CXXFLAGS/LDFLAGS` 视同未设置（与 MAKEFLAGS 一致，`{CFLAGS}` 与环境变量不再不一致）
- [x] `from_libsolv_evr`：release 段的 `~` 同样还原（`<ver>+<rel 含 ->` 现在往返一致）
- [x] 索引**包级 provides**（每行第 3 字段）作为版本级为空的兜底（此前该字段被完全忽略）
- [x] `has_system_soname`：候选路径必须落在 `root/usr/lib` 内（`path_within`）
- [x] `repo_revrequires`：判定提供者时看**所有** provides，不再只取 `find_provider` 的第一个
- [x] `run_hook`：`waitpid` 返回值必查 + EINTR 重试（此前读未初始化 `status` = UB）
- [x] `test_breakpoints`：**先 erase 再执行** action（此前 action 抛异常时 erase 不可达 → 断点重复触发）
- [x] `builder`：输出 `.lpkg` 文件名先过 `is_safe_path_component`（`"name":"../x"` 不再写到构建目录外）
- [x] **TLS 硬失败**：找不到 CA 包时**拒绝下载**（此前静默关闭 VERIFYPEER/HOST，等于把包与索引交给 MITM）
- [x] `/etc` 指向目录的符号链接不再绕过配置保护（两处 `is_directory` 跟随链接的漏洞）
- [x] **G4**：拒绝类操作的报错落在 **CLI 边界**（`remove_packages()` → 抛错、退出码非零）；库层 `remove_package` 保持"打印原因后返回"的友好语义 → 既有 9 个"阻止移除"测试不受影响

## 七、低危批（第三轮）—— 全部已修

- [x] **G2**：`scan_remove_tree` 的存在性判定改为"**仓库索引里有 或 图上出现过**"（并集）——
  只看反向依赖表时，"仓库里存在但无人依赖"的包会被误报成 `not found in repository`；
  `--all` 也不再被丢弃（与 `build_install_tree` 的 show_all 同义：把其余仓库包作为
  "不受影响"列出）。新增 `repo_package_names()` / `repo_has_package()` 两个助手。
- [x] **G3**：`resolve_transitive_deps` 不再在"已安装的中间节点"处提前 return →
  `depend install` 能显示已装包背后未安装的依赖（visited 已保证无环）。
- [x] `scan/scanner.cpp` 两处：`fs::relative` → `lexically_relative`（后者解析符号链接，
  非 / root 下 `lib64 -> lib` 会与登记属主键对不上 → 假孤儿）；范围 for 改为显式迭代器 +
  `increment(ec)`（范围 for 的自增在循环体之外，目录在遍历中被删会让**整趟**扫描失败）。
- [x] `main.cpp` 三小项：`curl_global_init` 返回值必查；`--yes` 与 `--no` 同给 → 报错
  （此前静默取 no）；`lpkg build a b c` → 报错（此前静默只编 a）。
- [x] **hooks 陈旧**：`InstallationTask` 记录本次写入的 hook 名单（`get_hook_files()`），
  `finish_committed_batch` 在**提交后**剪掉新版本不再提供的 hook（新版本没有 hooks 则整目录清）。
  刻意不放在批次内——那会重演 Z7 的"回滚丢钩子"。
  回归：`UpgradePrunesHooksDroppedByNewVersion`（禁用剪枝时实测变红）。
- [x] `strip.cpp` / `lib_utils`：4 处 `gelf_getshdr` 一律检查返回值；`elf_update` 返回值必查
  （失败时 memfd 是部分内容 → 曾会把截断的 ELF 当 strip 成功写回）；`strip_elf_rel_object`
  不再 `*out_data = *in_data` 借用输入缓冲（下文会改写符号表/组数据 → 等于就地修改调用方
  数据），改为每个节区先复制一份自有缓冲。

---

## 八、第四轮：D4/D5/E4/F3 + 一处误标订正（全部已修，630/630）

- [x] **D4**：`load_index` 现在对"文件存在但打不开（权限/竟是目录）"与"解析出 **0 个包**"各发一条告警
  （此前完全静默 → `lpkg upgrade` 打印"所有包都已是最新版本"、exit 0）；`mirror.conf` 首行统一归一
  （去 `#` 注释、去 `\r`/首尾空白 —— CRLF 写的配置此前会得到 `https://mirror/\r/`，下载必然失败并落进空索引路径）。
- [x] **D5**：`collect_problems` 改为**先判 JOB/PKG、再看名字形状** —— 形似 SONAME 的**顶层请求**
  现在归入 `missing_target`（硬报错），不再被 `--missing-so-no-error` 吞成"求解成功但事务为空"。
- [x] **E4**：**文件 → 目录**的升级（python 包 `foo.py → foo/__init__.py` 的真实场景）。这条测试驱动出
  **两个**真缺陷：① `physical_path` 以 `/` 结尾，而 `fs::exists("/x/foo/")` 对**已存在的普通文件**
  返回 false（尾斜杠要求它是目录）→ 备份阶段误判"不存在"、既不备份也不删除，随后
  `ensure_dir_exists` 抛 `Failed to create directory: File exists` 让整批中止；② 修好①后
  `commit_without_file_ops` 的"废弃旧文件"阶段又把**刚建好的新目录**当旧文件搬进 stash →
  升级"成功"但目录消失。两处都已修（判存在先去掉尾斜杠；阶段 1 跳过现在已是目录的路径）。
- [x] **F3**：新增 `run_shell_in_root()`（`root_dir != "/"` 时 fork+unshare+mount-private+chroot 后执行），
  外部触发器改走它 —— 此前 `lpkg --root /mnt/base install ...` 会把 `systemctl daemon-reload` /
  `glib-compile-schemas /usr/share/...` / `gtk-update-icon-cache` **打在宿主上**，目标 root 反而没更新。
- [x] **订正**：`depend_scanner.cpp:169-173`（同一 SONAME 只记**第一个**提供者 → 反向图少算一半影响面）
  我此前在 §六 里误标为已修，实际没修；**现已修**（`soname_provider` 改为多值集合，消费处对所有提供者连边）。
- 后续可做（非缺陷，仅去重）：`run_hook` 里有一套等价于 `run_shell_in_root` 的 chroot/fork 代码，
  可以合并成一处（改动涉及 hook 的执行方式，未纳入本轮）。
