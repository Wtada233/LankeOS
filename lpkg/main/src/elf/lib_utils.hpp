#pragma once
#include <gelf.h>
#include <libelf.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

// ============================================================================
// ELF 边界/计数校验（strip 与 lib_utils 共用）
// ============================================================================
// 这里处理的都是**上游构建产物**（不可信输入），畸形/构造的 ELF 必须只导致"放弃处理"，
// 不能让打包进程越界读写或除零（ASan 报过堆溢出与 SIGFPE）。

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
 * 直接算 `sh_size / sh_entsize` 会在 sh_entsize == 0 时 SIGFPE。
 */
inline size_t elf_section_entry_count(uint64_t sh_size, uint64_t sh_entsize)
{
    return sh_entsize == 0 ? 0 : static_cast<size_t>(sh_size / sh_entsize);
}

/**
 * @brief 从字符串表节区里**有界地**取一个字符串（offset 越界、或数据内找不到 NUL ⇒ 空串）。
 *
 * 不用 `elf_strptr` + 判空的原因：`elf_strptr(elf, index, off)` 只校验 `off` 落在该节区的
 * `d_size` **之内**，**不保证其后有 NUL 终止**，返回的是**裸 `const char*`**（文件映射上的
 * 指针）。于是 `std::string s = ptr;` 的 `strlen` 会一直读下去 —— 本节数据里没有 NUL 时，
 * **越过本节、读到文件其余部分，甚至读过映射末尾**。**判空拦不住它**：指针非 NULL，只是
 * **没有界**。
 *
 * 影响面（如果真触发）：`apply_soname_links` 在**安装后以 root 跑**，扫 `/usr/lib` 下**每个**
 * ELF，输入来自**不可信包**。同一形态在 `strip.cpp` 的两处节区名解析上也有。
 *
 * @param strtab_index 字符串表节区的**节区索引**（`.dynstr` 用 `sh_link`，节区名用 `e_shstrndx`）
 * @return 指向节区数据内部的 `string_view`（长度由 NUL 界出）；任何一处不满足即返回空
 */
inline std::string_view elf_strtab_get(Elf* elf, size_t strtab_index, uint64_t offset)
{
    Elf_Scn* scn = elf_getscn(elf, strtab_index);
    if (scn == nullptr) return {};
    GElf_Shdr shdr;
    if (gelf_getshdr(scn, &shdr) == nullptr || shdr.sh_type != SHT_STRTAB) return {};
    Elf_Data* data = elf_getdata(scn, nullptr);
    if (data == nullptr || data->d_buf == nullptr) return {};
    if (offset >= data->d_size) return {};
    const char* base = static_cast<const char*>(data->d_buf);
    const size_t off = static_cast<size_t>(offset);
    // **这一步正是 `elf_strptr` 不做的**：NUL 必须落在**本节的**数据之内。
    const void* nul = std::memchr(base + off, '\0', data->d_size - off);
    if (nul == nullptr) return {};
    return std::string_view(base + off, static_cast<const char*>(nul) - (base + off));
}

/**
 * @brief 为给定目录中的共享库生成/修正 SONAME 符号链接（ldconfig 语义）
 *
 * 幂等：链接指向当前目录里存在、且其 DT_SONAME 就等于链接名的库时不动它，否则重建；
 * 无人再提供该 SONAME 的悬空链接被清理（库被删除/替换后收尾）。实体文件不动。
 *
 * @param lib_dir       共享库所在目录
 * @param keep_dangling 可选的**注入判据**：返回 true 表示"这条悬空链接**不要删**"。
 *   存在的理由是"**别删属于某个包的链接**"——包可以刻意发一条
 *   `libfoo.so.1 -> libfoo.so.1.2.3`、而 `.1.2.3` 由**另一个**包提供且此刻还没装，
 *   删掉它没有任何机制会重建 ⇒ 运行期 `cannot open shared object file`。
 *   本函数住在 `elf/` 层，**不能**自己去问 `Cache`（那是 `db/`，反向依赖），所以由调用方
 *   注入：安装期的 ldconfig 触发器传一个查 `Cache` 归属的判据；构建期（staging）不传 ——
 *   那一侧算出来的逻辑键与宿主 DB 对不上，一律按"无主"处理，行为与改动前一致。
 */
void apply_soname_links(
    const std::filesystem::path& lib_dir,
    const std::function<bool(const std::filesystem::path&)>& keep_dangling = {});

/**
 * @brief 从 ELF 文件中提取 SONAME
 * @param path ELF 文件路径
 * @return SONAME 字符串，未找到则返回空字符串
 */
std::string get_elf_soname(const std::filesystem::path& path);
