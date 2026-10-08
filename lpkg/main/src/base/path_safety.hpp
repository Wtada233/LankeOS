#pragma once

#include <filesystem>
#include <string_view>

/**
 * 路径 p 是否在 root 之内（含 root 自身）。两边取 lexically_normal、root 尾部分隔符
 * 归一后，按目录边界比较（p == root，或以 root + "/" 开头）。
 *
 * **不要手写 `root + "/"` 前缀拼接**：root == "/" 时得到 "//"，而 lexically_normal
 * 后的路径不可能以 "//" 开头 → 判定恒为 false，把所有路径都判成"不在根内"。
 * root_dir == "/" 恰恰是常规安装（不带 --root）的形态。
 */
bool path_within(const std::filesystem::path& p, const std::filesystem::path& root);

/**
 * 同 `path_within`，但**加一层 canonical 复核**（只解析父目录）—— 用于"路径来自纯文本、
 * 可能被篡改/写坏"的场合（WAL 行、`--root` 下的落位目标）。**永不抛。**
 *
 * ① **词法级**：`path_within(p, root)`。不碰文件系统，负责挡掉 `../` 逃逸与"绝对路径就指在
 *    root 之外"（WAL 行是纯文本，`NEW /etc/sudoers` 在坏 WAL 里只是一行字）。
 * ② **canonical 复核**（尽力而为，**只解析父目录**）：`weakly_canonical(root)` 与
 *    `weakly_canonical(p.parent_path())` 都成功时再比一次 —— 挡的是词法上合法、实际却穿透
 *    出去的那种：`<root>/evil -> /etc` 配上 `<root>/evil/shadow`。
 *    **末段不解析**：rename/unlink/rmdir 与"落位"都不跟随末段链接，而 `--root` 安装里包发
 *    绝对目标链接（`<root>/usr/bin/foo -> /etc/foo`）完全合法 —— 解析它会误伤。
 *
 * **解不开就不判越界**（`ec != 0` → 放行）：这条判据跑在回滚/恢复路径上，绝不允许因为"这个
 * 路径暂时解不开"而拒绝一条合法的行。典型是 ELOOP 自环（那份备份必须能被逆操作搬回原位，
 * `tests/integration/test_symlink_loop_install.cpp` 钉着）与"中间段尚未重建的 `DIR_RM`"。
 * 空 `p` / 空 `root` 一律放行（含义由调用方自己处理）。
 *
 * 本函数是 `db/wal_op.cpp` 里那份 `path_within_root()`（参数顺序相反：这里是
 * `(p, root)`）与"落位目标的祖先链约束"**共用**的唯一实现 —— 此前那两处各写了一套分量比较，
 * 只靠注释声明"用同一套剥离规则"，必然漂移。
 */
bool path_within_resolved(const std::filesystem::path& p, const std::filesystem::path& root);

/**
 * `dir` **实际解析到的那个目录**是否落在 `root` 内 —— **整条路径都解析（含末段）**。
 *
 * 与 `path_within_resolved` 的分工：那个刻意**不解析末段**（用于 rename/unlink/落位那种
 * "要处置的就是这个名字"的场合）；本函数要问的恰恰相反 —— "这条路径最后落在哪个目录"，
 * 用于**跟随语义**使用该目录的场合（典型：`apply_soname_links()` 会在其中 `create_symlink` /
 * `fs::remove`，而它用 `is_directory_follow` 判定入参）。
 *
 * 为什么需要它：`<root>/usr/lib` 本身可以是一条**包发的符号链接**
 * （末段不解析是有意的，见 §5.4 不变量 6），若它指向 root 之外，提交后的触发器就会在宿主的
 * 那个目录里建/删 SONAME 链接 —— `--root` 的隔离被"提交后阶段"穿透。
 *
 * 解不开（ELOOP / 不存在）→ 返回 true（放行）：与其余 confinement 同一条纪律，判定不得因
 * "解不开"而拒掉合法操作。
 */
bool path_resolves_within(const std::filesystem::path& dir, const std::filesystem::path& root);

/**
 * 单个路径分量是否安全。**拒绝**：空串、"."、".."、含 '/' 或 NUL，以及**分帧/空白字符**
 * —— `|` `;` `,` `:`（包名/版本会被写进 `pkgs`、索引行、`files.db` 等"行式 + 分隔符"
 * 状态文件，带进去就把一条记录重新分帧成另一条）与空格 / TAB / LF / CR（会破坏 WAL 的
 * 空格分帧里程碑字段）。完整判据与理由见 `base/utils.cpp` 的 `is_safe_path_component()`。
 *
 * 包名与版本号来自**不可信来源**（远端索引、.lpkg 内的 metadata.json），而它们会被
 * 直接当成路径分量拼进 tmp_pkg_dir() / dep_dir() / docs_dir() / 下载 URL，
 * 一个 `../` 就能以 root 写到这些目录之外。
 */
bool is_safe_path_component(std::string_view s);
