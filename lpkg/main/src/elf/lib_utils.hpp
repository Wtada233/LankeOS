#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

// ============================================================================
// ELF 边界/计数校验（strip 与 lib_utils 共用）
// ============================================================================
// 这里处理的都是**上游构建产物**（不可信输入），畸形/构造的 ELF 必须只导致"放弃处理"，
// 不能让打包进程越界读写或除零（TODO.md X1：ASan 实测过堆溢出与 SIGFPE）。

/**
 * @brief [off, off+size) 是否完整落在容量 limit 之内。
 *
 * 用 `size <= limit - off` 而非 `off + size <= limit`：后者在 uint64 下会**回绕**
 * （sh_size = 2^64-0x1000 这类值让检查通过，随后就是越界 memcpy）。
 */
inline bool elf_range_within(uint64_t off, uint64_t size, size_t limit)
{
    return off <= limit && size <= static_cast<uint64_t>(limit) - off;
}

/**
 * @brief 节区条目数（如 SHT_DYNAMIC 的 Dyn 个数）；sh_entsize == 0 时返回 0。
 *
 * 直接算 `sh_size / sh_entsize` 会在 sh_entsize == 0 时 SIGFPE（实测）。
 */
inline size_t elf_section_entry_count(uint64_t sh_size, uint64_t sh_entsize)
{
    return sh_entsize == 0 ? 0 : static_cast<size_t>(sh_size / sh_entsize);
}

/**
 * @brief 为给定目录中的共享库生成/修正 SONAME 符号链接（ldconfig 语义）
 *
 * 幂等：链接指向当前目录里存在、且其 DT_SONAME 就等于链接名的库时不动它，否则重建；
 * 无人再提供该 SONAME 的悬空链接被清理（库被删除/替换后收尾）。实体文件不动。
 *
 * @param lib_dir 共享库所在目录
 */
void apply_soname_links(const std::filesystem::path& lib_dir);

/**
 * @brief 从 ELF 文件中提取 SONAME
 * @param path ELF 文件路径
 * @return SONAME 字符串，未找到则返回空字符串
 */
std::string get_elf_soname(const std::filesystem::path& path);
