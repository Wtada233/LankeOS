#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

/**
 * 把 from 的 xattr 全部复制到 to（用 l* 变体：不跟随符号链接）。
 *
 * `std::filesystem::copy` **不复制 xattr**，而 `security.capability` 就是一个 xattr ——
 * ping/fping/traceroute/mtr 这类包靠**文件能力**而非 SUID，丢了它非 root 直接不能用。
 *
 * 语义是"**只写 from 有的键**"：逐个 `lsetxattr`，**从不删** `to` 上另有（from 没有的）键。
 * 这条性质被上层依赖：向一个**多包共用**的目录写 xattr 时，别的包设的键不会被抹掉。
 */
void copy_xattrs(const std::filesystem::path& from, const std::filesystem::path& to);

// ============ xattr 的逐键读写 ============
//
// 为什么需要这一组（而不是继续用 `copy_xattrs` 一把梭）：目录的 xattr 要进**事务**
// —— 写之前必须把**旧值**记进 WAL（否则回滚还原不了），所以调用方要能"逐键读旧值 → 记 →
// 逐键写新值"。`copy_xattrs` 是"from → to"的整份复制，表达不了"这一格要记旧值"。

/**
 * 列出 `p` 上的全部 xattr 键（lstat 语义：不跟随符号链接）。`p` 不存在 / 不支持 xattr
 * → 返回空表（**不是错误**：绝大多数目录没有任何 xattr）。
 */
std::vector<std::string> list_xattr_keys(const std::filesystem::path& p);

/**
 * 读 `p` 上某个键的值。**键不存在返回 nullopt**（与"值是空串"区分开：xattr 允许 0 长度
 * 的值，`lgetxattr` 返回 0 而键存在）。读失败（ENOTSUP 等）同样 nullopt。
 */
std::optional<std::vector<char>> read_xattr(const std::filesystem::path& p, const std::string& key);

/** 写一个键（覆盖或新建）。失败返回 false（调用方决定要不要告警/失败）。 */
bool write_xattr(const std::filesystem::path& p, const std::string& key,
                 const std::vector<char>& value);

/** 删一个键。**键本来就不存在** → 返回 false（与"删失败"同一个返回值：调用方只关心
 *  "删掉了没有"，两者都不需要额外处理）。 */
bool remove_xattr(const std::filesystem::path& p, const std::string& key);
