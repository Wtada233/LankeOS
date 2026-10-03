#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "strip.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/utils.hpp"
#include "i18n/localization.hpp"
#include "lib_utils.hpp"

namespace fs = std::filesystem;

enum class FileType { Unknown, Executable, SharedLibrary, StaticLibrary, ObjectFile };

namespace
{
/**
 * `open()` / `memfd_create()` 拿到的文件描述符：RAII。
 *
 * 为什么加它：本文件里这两个 fd 各有**多个早退点**（畸形 ELF 的每道守卫都要提前 return），
 * 每个早退都得手写一遍 `elf_end` + `close`。那种约定**靠人守** —— 新加一个 `return` 忘了补
 * 就漏资源。改成 RAII 之后，漏不漏由**语言**保证。
 *
 * **声明顺序是承重的**：`ElfHandle` 必须在 `Fd` **之后**声明 —— 局部对象按**逆序**析构，
 * 于是 `elf_end` 先跑、`close` 后跑（libelf 的 `elf_end` 还要用那个 fd）。
 *
 * 同款惯用法见 `archive/archive.cpp` 的 `ArchiveReadHandle` / `ArchiveWriteHandle`。
 */
class Fd
{
public:
    explicit Fd(int fd) : fd_(fd)
    {
    }
    ~Fd()
    {
        if (fd_ >= 0) ::close(fd_);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const
    {
        return fd_;
    }
    bool ok() const
    {
        return fd_ >= 0;
    }

private:
    int fd_;
};

/// `libelf` 句柄：RAII。**声明在 `Fd` 之后**（见上：析构逆序 ⇒ 先 `elf_end`）。
class ElfHandle
{
public:
    explicit ElfHandle(Elf* e) : elf_(e)
    {
    }
    ~ElfHandle()
    {
        if (elf_) elf_end(elf_);
    }
    ElfHandle(const ElfHandle&) = delete;
    ElfHandle& operator=(const ElfHandle&) = delete;
    Elf* get() const
    {
        return elf_;
    }
    explicit operator bool() const
    {
        return elf_ != nullptr;
    }

private:
    Elf* elf_;
};
}  // namespace

// strip_file 中使用的辅助函数的前向声明
FileType identify_file_type(const fs::path& path);
bool process_elf(const fs::path& path, std::string& error_msg);
bool process_archive(const fs::path& path, std::string& error_msg);

/**
 * 对指定文件执行 strip 操作，去除调试信息和符号表
 * 根据文件类型（ELF 可执行文件、共享库、目标文件、静态库）分发到对应的处理函数
 */
bool strip_file(const fs::path& path, std::string& error_msg)
{
    if (!fs::exists(path)) {
        error_msg = string_format("error.strip_file_not_exist", path.string());
        return false;
    }

    if (elf_version(EV_CURRENT) == EV_NONE) {
        error_msg = get_string("error.strip_libelf_mismatch");
        return false;
    }

    FileType type = identify_file_type(path);

    switch (type) {
        case FileType::Executable:
        case FileType::SharedLibrary:
        case FileType::ObjectFile:
            return process_elf(path, error_msg);
        case FileType::StaticLibrary:
            return process_archive(path, error_msg);
        default:
            error_msg = get_string("error.strip_unknown_type");
            return false;
    }
}

/**
 * 识别文件类型：通过 libelf 读取 ELF 头信息，区分可执行文件、共享库、
 * 目标文件、静态库（ar 归档）等类型。
 * 对于 ET_DYN 类型，通过检查 PT_INTERP 和 DT_SONAME 来区分可执行文件和共享库
 */
FileType identify_file_type(const fs::path& path)
{
    Fd fd(::open(path.c_str(), O_RDONLY));
    if (!fd.ok()) return FileType::Unknown;

    // 声明顺序：`Fd` 在前、`ElfHandle` 在后 ⇒ 析构逆序 ⇒ `elf_end` 先于 `close`。
    // 下面所有早退都不再需要手写清理（原先 3 处各写一遍）。
    ElfHandle elf(elf_begin(fd.get(), ELF_C_READ, nullptr));
    if (!elf) return FileType::Unknown;

    if (elf_kind(elf.get()) == ELF_K_AR) return FileType::StaticLibrary;

    GElf_Ehdr ehdr;
    if (gelf_getehdr(elf.get(), &ehdr) == nullptr) return FileType::Unknown;

    FileType type = FileType::Unknown;
    if (ehdr.e_type == ET_EXEC) {
        type = FileType::Executable;
    } else if (ehdr.e_type == ET_REL) {
        type = FileType::ObjectFile;
    } else if (ehdr.e_type == ET_DYN) {
        // 区分 PIE 可执行文件和共享库：
        // 有 PT_INTERP 且无 DT_SONAME 的视为可执行文件
        size_t phnum;
        bool has_pt_interp = false;
        if (elf_getphdrnum(elf.get(), &phnum) == 0) {
            for (size_t i = 0; i < phnum; ++i) {
                GElf_Phdr phdr;
                if (gelf_getphdr(elf.get(), i, &phdr) != nullptr) {
                    if (phdr.p_type == PT_INTERP) {
                        has_pt_interp = true;
                        break;
                    }
                }
            }
        }

        bool has_soname = false;
        Elf_Scn* scn = nullptr;
        while ((scn = elf_nextscn(elf.get(), scn)) != nullptr) {
            GElf_Shdr shdr;
            if (gelf_getshdr(scn, &shdr) == nullptr) continue;
            if (shdr.sh_type == SHT_DYNAMIC) {
                Elf_Data* data = elf_getdata(scn, nullptr);
                if (data) {
                    const size_t ext_count = elf_section_entry_count(shdr.sh_size, shdr.sh_entsize);
                    for (size_t i = 0; i < ext_count; ++i) {
                        GElf_Dyn dyn;
                        if (gelf_getdyn(data, i, &dyn) == nullptr) continue;
                        if (dyn.d_tag == DT_SONAME) {
                            has_soname = true;
                            break;
                        }
                    }
                }
            }
        }

        if (has_pt_interp && !has_soname) {
            type = FileType::Executable;
        } else {
            type = FileType::SharedLibrary;
        }
    }

    return type;  // `elf`/`fd` 由 RAII 收尾（原先这里还要手写 elf_end + close）
}

/**
 * 对可重定位文件（ET_REL .o 文件）进行 strip
 * 通过 libelf API 重建文件，只保留非调试节区，
 * 同时更新节区索引映射（包括 SHT_SYMTAB 中的符号索引和 SHT_GROUP 中的节区索引）
 */
static bool strip_elf_rel_object(Elf* in_elf, const GElf_Ehdr& ehdr, size_t shstrndx, int elf_class,
                                 std::vector<uint8_t>& output_data, std::string& error_msg)
{
    Fd out_fd(memfd_create("strip_out", 0));
    if (!out_fd.ok()) {
        error_msg = get_string("error.strip_object_failed");
        return false;
    }
    // 守卫 + **别名**：函数体里 `out_elf` 的几十处用法一行不改，只把"每条早退都要手写
    // `elf_end` + `close`"（原先 3 处）交给析构去保证。顺序同 identify_file_type：
    // `Fd` 先声明 ⇒ 后析构。
    ElfHandle out_elf_guard(elf_begin(out_fd.get(), ELF_C_WRITE, nullptr));
    Elf* out_elf = out_elf_guard.get();
    gelf_newehdr(out_elf, elf_class);
    GElf_Ehdr mutable_ehdr = ehdr;
    gelf_update_ehdr(out_elf, &mutable_ehdr);

    std::map<size_t, size_t> idx_map;
    idx_map[0] = 0;
    idx_map[SHN_ABS] = SHN_ABS;
    idx_map[SHN_COMMON] = SHN_COMMON;
    idx_map[SHN_UNDEF] = SHN_UNDEF;

    struct SectionInfo {
        Elf_Scn* old_scn;
        GElf_Shdr shdr;
        std::string name;
        size_t old_idx;
    };
    std::vector<SectionInfo> to_keep;
    Elf_Scn* scn = nullptr;
    size_t new_idx = 1;

    while ((scn = elf_nextscn(in_elf, scn)) != nullptr) {
        size_t old_idx = elf_ndxscn(scn);
        GElf_Shdr shdr;
        if (gelf_getshdr(scn, &shdr) == nullptr) continue;
        const char* name_ptr = elf_strptr(in_elf, shstrndx, shdr.sh_name);
        std::string name = name_ptr ? name_ptr : "";

        bool keep = true;
        // 过滤掉调试信息和注释节区
        if (name.starts_with(".debug") || name.starts_with(".rela.debug") ||
            name.starts_with(".rel.debug") || name == ".comment")
            keep = false;

        if (keep) {
            to_keep.push_back({scn, shdr, name, old_idx});
            idx_map[old_idx] = new_idx++;
        }
    }

    // 下面会改写符号表 / SHT_GROUP 数据。libelf 的 Elf_Data 指向**输入缓冲区**，
    // 直接 `*out_data = *in_data` 就是就地修改调用方的输入（.a 成员的原始数据）——
    // 失败路径下会把半改写的成员写回归档。故每个节区先复制一份自有缓冲。
    std::vector<std::vector<uint8_t>> owned_data;
    owned_data.reserve(to_keep.size());

    for (auto& info : to_keep) {
        Elf_Scn* new_scn = elf_newscn(out_elf);
        GElf_Shdr new_shdr = info.shdr;

        // 重映射节区链接索引
        if (idx_map.count(new_shdr.sh_link))
            new_shdr.sh_link = idx_map[new_shdr.sh_link];
        else if (new_shdr.sh_link != 0)
            new_shdr.sh_link = SHN_UNDEF;

        // SHT_GROUP 的 sh_info 是符号表索引，不是节区索引，不应重映射
        if (new_shdr.sh_type == SHT_REL || new_shdr.sh_type == SHT_RELA ||
            ((new_shdr.sh_flags & SHF_INFO_LINK) && new_shdr.sh_type != SHT_GROUP)) {
            if (idx_map.count(new_shdr.sh_info))
                new_shdr.sh_info = idx_map[new_shdr.sh_info];
            else
                new_shdr.sh_info = 0;
        }

        gelf_update_shdr(new_scn, &new_shdr);
        Elf_Data* in_data = elf_getdata(info.old_scn, nullptr);
        Elf_Data* out_data = elf_newdata(new_scn);
        if (in_data) {
            *out_data = *in_data;
            // **只为真有数据的节区复制自有缓冲**：SHT_NOBITS（.bss 等）在 libelf 里
            // d_buf == NULL 而 d_size > 0 —— 按 d_size 从 NULL 构造 vector 是 UB
            // （bad_alloc/崩溃），会让整个 strip 失败（LLVM 的 .o 带 .bss，实测卡住其构建）。
            // 这类节区没有数据可改写，直接沿用描述符即可。
            if (in_data->d_buf != nullptr && in_data->d_size > 0) {
                const auto* begin = static_cast<const uint8_t*>(in_data->d_buf);
                owned_data.emplace_back(begin, begin + in_data->d_size);
                out_data->d_buf = owned_data.back().data();
            }
            // 更新符号表中的节区索引
            if (new_shdr.sh_type == SHT_SYMTAB) {
                size_t sym_count = out_data->d_size / gelf_fsize(out_elf, ELF_T_SYM, 1, EV_CURRENT);
                for (size_t i = 0; i < sym_count; ++i) {
                    GElf_Sym sym;
                    gelf_getsym(out_data, i, &sym);
                    if (sym.st_shndx < SHN_LORESERVE) {
                        if (idx_map.count(sym.st_shndx))
                            sym.st_shndx = idx_map[sym.st_shndx];
                        else if (sym.st_shndx != SHN_UNDEF)
                            sym.st_shndx = SHN_UNDEF;
                    }
                    gelf_update_sym(out_data, i, &sym);
                }
            }
            // 重映射 SHT_GROUP 数据中的节区索引
            if (new_shdr.sh_type == SHT_GROUP) {
                Elf32_Word* group_data = static_cast<Elf32_Word*>(out_data->d_buf);
                size_t group_count = out_data->d_size / sizeof(Elf32_Word);
                for (size_t i = 1; i < group_count; ++i) {
                    if (idx_map.count(group_data[i]))
                        group_data[i] = idx_map[group_data[i]];
                    else
                        group_data[i] = 0;
                }
            }
        }
    }

    GElf_Ehdr updated_ehdr = ehdr;
    if (idx_map.count(shstrndx))
        updated_ehdr.e_shstrndx = idx_map[shstrndx];
    else
        updated_ehdr.e_shstrndx = SHN_UNDEF;

    gelf_update_ehdr(out_elf, &updated_ehdr);
    // elf_update 失败（内存/IO）返回 < 0，此时 memfd 里是**部分写入**的内容——
    // 不检查就会把截断的 ELF 当作 strip 成功写回原文件（静默损坏二进制）
    if (elf_update(out_elf, ELF_C_WRITE) < 0) {
        error_msg = get_string("error.strip_object_failed");
        return false;
    }

    // `lseek` 失败返回 -1：**必须查**。此前直接 `output_data.resize(size)` —— `resize(-1)` 转成
    // `size_t` 是个天文数字，抛的是 `std::length_error`/`bad_alloc`（不是 `LpkgException`），
    // 用户在 `strip_binary` 里看到的是没头没脑的一句内存错误，而不是"哪个文件 strip 失败"。
    // 回绕到 `SEEK_SET` 的那次 `lseek` 同样要查（失败则 `read` 从当前 EOF 位置读 → 0 字节）。
    const off_t size = lseek(out_fd.get(), 0, SEEK_END);
    if (size < 0) {
        error_msg = get_string("error.strip_object_failed");
        return false;
    }
    output_data.resize(static_cast<size_t>(size));
    if (lseek(out_fd.get(), 0, SEEK_SET) < 0) {
        error_msg = get_string("error.strip_object_failed");
        return false;
    }
    ssize_t bytes_read = read(out_fd.get(), output_data.data(), size);
    if (bytes_read != size) {
        error_msg = get_string("error.strip_object_failed");
        return false;
    }

    return true;  // `out_elf`/`out_fd` 由 RAII 收尾
}

/**
 * 对可执行文件和共享库（ET_EXEC / ET_DYN）进行 strip
 * 手动重建 ELF 布局，仅保留 SHF_ALLOC 节区以及必要的 .shstrtab 和字符串表，
 * 丢弃 .symtab、.strtab、.comment 和 .debug* 节区
 */
static bool strip_elf_exec_dyn(Elf* in_elf, const GElf_Ehdr& ehdr, size_t shstrndx, int elf_class,
                               const std::vector<uint8_t>& input_data,
                               std::vector<uint8_t>& output_data, std::string& error_msg)
{
    struct KeptSection {
        GElf_Shdr shdr;
        size_t old_index;
        size_t new_index;
    };

    // ⚠️ `e_ehsize` 是**输入可控**的字段，而下面 `write_ehdr` 会无条件写入**整个**
    // `Elf32_Ehdr` / `Elf64_Ehdr`（52 / 64 字节），那个 `memcpy` 的守卫却取自
    // `max(e_phoff + e_phnum*phsize, e_ehsize)` —— 把 `e_ehsize` 改成 4 就能让它通过，
    // 于是往只有几十字节的输出缓冲区里写 52/64 字节（实测 ASan：`heap-buffer-overflow
    // WRITE of size 2`，非 ASan 下是堆损坏 → SIGABRT → 目标文件已被截断成 0 字节）。
    // 规范里 `e_ehsize` 恒等于 ELF 头自身的大小，对不上就是畸形输入 —— 直接拒。
    const size_t ehdr_size = (elf_class == ELFCLASS64) ? sizeof(Elf64_Ehdr) : sizeof(Elf32_Ehdr);
    if (ehdr.e_ehsize != ehdr_size) {
        error_msg = get_string("error.strip_malformed_elf");
        return false;
    }

    std::vector<KeptSection> kept_sections;
    std::map<size_t, size_t> el_idx_map;

    kept_sections.push_back({{}, 0, 0});
    el_idx_map[0] = 0;
    el_idx_map[SHN_ABS] = SHN_ABS;
    el_idx_map[SHN_COMMON] = SHN_COMMON;
    el_idx_map[SHN_UNDEF] = SHN_UNDEF;

    uint64_t max_alloc_end = 0;
    Elf_Scn* scn = nullptr;
    size_t current_new_idx = 1;

    while ((scn = elf_nextscn(in_elf, scn)) != nullptr) {
        size_t old_idx = elf_ndxscn(scn);
        GElf_Shdr shdr;
        if (gelf_getshdr(scn, &shdr) == nullptr) continue;
        const char* name_ptr = elf_strptr(in_elf, shstrndx, shdr.sh_name);
        std::string name = name_ptr ? name_ptr : "";

        bool keep = true;
        // 保留 SHF_ALLOC 节区（运行时需要的节区）
        if (!(shdr.sh_flags & SHF_ALLOC)) {
            if (name != ".shstrtab" && shdr.sh_type != SHT_STRTAB) keep = false;
        }
        // 丢弃符号表、字符串表、注释和调试信息
        if (name == ".symtab" || name == ".strtab" || name == ".comment" ||
            name.starts_with(".debug"))
            keep = false;

        if (keep) {
            kept_sections.push_back({shdr, old_idx, current_new_idx});
            el_idx_map[old_idx] = current_new_idx;
            current_new_idx++;

            if ((shdr.sh_flags & SHF_ALLOC) && shdr.sh_type != SHT_NOBITS) {
                // `sh_offset + sh_size` 是输入可控的 64 位加法，回绕会把 max_alloc_end 变小
                // —— 下面的重排基于它，越界写就从此处开始。超限即拒（畸形输入）。
                if (shdr.sh_offset > UINT64_MAX - shdr.sh_size) {
                    error_msg = get_string("error.strip_malformed_elf");
                    return false;
                }
                max_alloc_end = std::max(max_alloc_end, shdr.sh_offset + shdr.sh_size);
            }
        }
    }

    // 对非 SHF_ALLOC 节区重新排列，紧跟在已分配数据之后
    if (max_alloc_end > UINT64_MAX - constants::ELF_SECTION_ALIGN_MASK) {
        error_msg = get_string("error.strip_malformed_elf");
        return false;
    }
    uint64_t current_offset =
        (max_alloc_end + constants::ELF_SECTION_ALIGN_MASK) & ~constants::ELF_SECTION_ALIGN_MASK;
    for (size_t i = 1; i < kept_sections.size(); ++i) {
        auto& ks = kept_sections[i];
        if (!(ks.shdr.sh_flags & SHF_ALLOC)) {
            ks.shdr.sh_offset = current_offset;
            // `sh_size` 是输入可控的：一个保留下来（例如名叫 `.shstrtab`）的非 ALLOC 节区配
            // 一个巨大的 `sh_size`，会让累加**回绕** → `e_shoff` 随之变成极小值 → 下面的
            // `resize()` 分配小缓冲，而写节区表仍按 `e_shoff + i*shentsize` 定位 → 堆越界写
            // （NOBITS 节区的复制在下面被跳过，那道 elf_range_within 守卫因此不覆盖它）。
            // 累加前显式查溢出，超限即拒。
            if (ks.shdr.sh_size > UINT64_MAX - current_offset) {
                error_msg = get_string("error.strip_malformed_elf");
                return false;
            }
            current_offset += ks.shdr.sh_size;
        }

        if (el_idx_map.count(ks.shdr.sh_link)) {
            ks.shdr.sh_link = el_idx_map[ks.shdr.sh_link];
        } else if (ks.shdr.sh_link != 0) {
            ks.shdr.sh_link = SHN_UNDEF;
        }

        // SHT_GROUP 的 sh_info 是符号表索引，不是节区索引，不应重映射
        if (ks.shdr.sh_type == SHT_REL || ks.shdr.sh_type == SHT_RELA ||
            ((ks.shdr.sh_flags & SHF_INFO_LINK) && ks.shdr.sh_type != SHT_GROUP)) {
            if (el_idx_map.count(ks.shdr.sh_info)) {
                ks.shdr.sh_info = el_idx_map[ks.shdr.sh_info];
            } else {
                ks.shdr.sh_info = 0;
            }
        }
    }

    size_t shentsize = (elf_class == ELFCLASS64) ? sizeof(Elf64_Shdr) : sizeof(Elf32_Shdr);
    GElf_Ehdr updated_ehdr = ehdr;
    // 对齐的 `+15` 仍可能回绕（`current_offset` 虽已被上面两道守卫约束，但离上界只差 15
    // 也够回绕），一并挡住。
    if (current_offset > UINT64_MAX - 15) {
        error_msg = get_string("error.strip_malformed_elf");
        return false;
    }
    updated_ehdr.e_shoff = (current_offset + 15) & ~15;

    // ⚠️ `e_shnum` 是 **16 位**字段：节区数一旦到 `SHN_LORESERVE`(0xff00) 就得改用
    // SHN_XINDEX 扩展编号（真值放 `section[0].sh_size`、`e_shnum` 写 0），本函数没有实现
    // 那套。直接赋 `size_t` 会**截断**，而下面的 `output_data.resize()` 用截断后的数、
    // 写节区表的循环却用**未截断**的 `kept_sections.size()` —— 于是写到缓冲区之外
    // （实测 ASan：64 字节缓冲区上 `heap-buffer-overflow WRITE of size 4`）。
    // 超限即拒（畸形/超大输入，不是我们要 strip 的东西）。
    if (kept_sections.size() >= SHN_LORESERVE) {
        error_msg = get_string("error.strip_malformed_elf");
        return false;
    }
    updated_ehdr.e_shnum = kept_sections.size();

    if (el_idx_map.count(shstrndx))
        updated_ehdr.e_shstrndx = el_idx_map[shstrndx];
    else
        updated_ehdr.e_shstrndx = SHN_UNDEF;

    // resize 的实参也要防回绕：`e_shoff + e_shnum*shentsize` 若回绕成小数，就会分配小于写
    // 节区表所需的空间（下面 write_shdr 会越界写）。e_shnum 已被 SHN_LORESERVE 上限约束，
    // 这里只需防加法本身回绕。
    if (updated_ehdr.e_shnum > (UINT64_MAX - updated_ehdr.e_shoff) / shentsize) {
        error_msg = get_string("error.strip_malformed_elf");
        return false;
    }
    // **量级**也要封顶（2026-10-03 修）：上面的守卫只防**回绕**，不防"不回绕但巨大"。
    // `e_shoff` 是 `align(max_alloc_end)` + 保留节区 `sh_size` 之和推出的，而 `sh_size` 来自
    // 输入、只被"加法不回绕"约束 —— 一个 `1<<40` 级的 `sh_size` 会让 `resize` 去分配几十 TiB，
    // 那是 `std::length_error`/`bad_alloc`（或 OOM），**不是**这里该给的"畸形 ELF → 放弃 strip"。
    // strip 只会**删**节区，产物**不可能大于输入** ⇒ 用输入大小封顶（`>` 不是 `>=`：全保留时
    // 两者可相等）。回绕守卫已保证这里加法不回绕，所以这个比较本身安全。
    if (updated_ehdr.e_shoff + updated_ehdr.e_shnum * shentsize > input_data.size()) {
        error_msg = get_string("error.strip_malformed_elf");
        return false;
    }
    output_data.resize(updated_ehdr.e_shoff + updated_ehdr.e_shnum * shentsize);

    for (size_t i = 1; i < kept_sections.size(); ++i) {
        const auto& ks = kept_sections[i];
        Elf_Scn* old_scn = elf_getscn(in_elf, ks.old_index);
        if (old_scn) {
            GElf_Shdr old_shdr;
            if (gelf_getshdr(old_scn, &old_shdr) == nullptr) continue;
            if (old_shdr.sh_type != SHT_NOBITS && old_shdr.sh_size > 0) {
                // 源区间（输入缓冲）与目标区间（重排后的输出缓冲）都必须完整落在各自
                // 缓冲内。校验用 elf_range_within（饱和比较，加法不会回绕）。
                // 不一致 = 输入 ELF 的节区布局与重排结果矛盾（构造/畸形文件）→
                // 放弃 strip 并返回 false，由调用方告警并**保留原文件**：
                // 绝不部分拷贝出损坏产物，更不能越界写（曾实测 128 字节区域写 4320）。
                if (!elf_range_within(old_shdr.sh_offset, old_shdr.sh_size, input_data.size()) ||
                    !elf_range_within(ks.shdr.sh_offset, ks.shdr.sh_size, output_data.size())) {
                    // **必须填 error_msg**：上面注释写着"由调用方告警"，而调用方的判据是
                    // `!strip_file(...) && !error_msg.empty()`（strip_binary）—— 空串 = 拒绝得
                    // 一声不响（静默跳过、原文件保留但用户毫不知情）。与 process_archive 的
                    // `bail()` 同一个纪律（2026-10-03 修）。
                    error_msg = get_string("error.strip_malformed_elf");
                    return false;
                }
                std::memcpy(output_data.data() + ks.shdr.sh_offset,
                            input_data.data() + old_shdr.sh_offset, ks.shdr.sh_size);
            }
        }
    }

    // 复制 ELF 头和程序头到输出缓冲区（长度同样必须落在**两个**缓冲内）
    size_t headers_size = updated_ehdr.e_phoff +
                          updated_ehdr.e_phnum *
                              ((elf_class == ELFCLASS64) ? sizeof(Elf64_Phdr) : sizeof(Elf32_Phdr));
    headers_size = std::max(headers_size, (size_t)updated_ehdr.e_ehsize);
    if (!elf_range_within(0, headers_size, input_data.size()) ||
        !elf_range_within(0, headers_size, output_data.size())) {
        // 同上：拒绝必须带原因，否则调用方静默跳过（2026-10-03 修）。
        error_msg = get_string("error.strip_malformed_elf");
        return false;
    }
    std::memcpy(output_data.data(), input_data.data(), headers_size);

    // 写入更新后的 ELF 头（节区表偏移、数量和字符串表索引）
    auto write_ehdr = [&]<typename Hdr>() {
        Hdr* out = reinterpret_cast<Hdr*>(output_data.data());
        out->e_shoff = updated_ehdr.e_shoff;
        out->e_shnum = updated_ehdr.e_shnum;
        out->e_shstrndx = updated_ehdr.e_shstrndx;
    };
    if (elf_class == ELFCLASS64)
        write_ehdr.operator()<Elf64_Ehdr>();
    else
        write_ehdr.operator()<Elf32_Ehdr>();

    // 写入更新后的节区表
    auto write_shdr = [&]<typename Shdr>(size_t i) {
        Shdr* out =
            reinterpret_cast<Shdr*>(output_data.data() + updated_ehdr.e_shoff + i * shentsize);
        out->sh_name = kept_sections[i].shdr.sh_name;
        out->sh_type = kept_sections[i].shdr.sh_type;
        out->sh_flags = kept_sections[i].shdr.sh_flags;
        out->sh_addr = kept_sections[i].shdr.sh_addr;
        out->sh_offset = kept_sections[i].shdr.sh_offset;
        out->sh_size = kept_sections[i].shdr.sh_size;
        out->sh_link = kept_sections[i].shdr.sh_link;
        out->sh_info = kept_sections[i].shdr.sh_info;
        out->sh_addralign = kept_sections[i].shdr.sh_addralign;
        out->sh_entsize = kept_sections[i].shdr.sh_entsize;
    };
    for (size_t i = 0; i < kept_sections.size(); ++i) {
        if (elf_class == ELFCLASS64)
            write_shdr.operator()<Elf64_Shdr>(i);
        else
            write_shdr.operator()<Elf32_Shdr>(i);
    }
    return true;
}

/**
 * 对 ELF 二进制数据进行 strip 操作的入口
 * 根据 ELF 类型分派到对应的子函数
 */
bool strip_elf_data(const std::vector<uint8_t>& input_data, std::vector<uint8_t>& output_data,
                    std::string& error_msg)
{
    // 本函数**每个失败分支都必须填 `error_msg`** —— 消费者的判据是
    // `!strip_file(...) && !error_msg.empty()`（`strip_binary`），空串 = 拒绝得一声不响
    // （既不剥、也不告警）。2026-10-03 把这几个分支补齐。
    if (elf_version(EV_CURRENT) == EV_NONE) {
        error_msg = get_string("error.strip_libelf_mismatch");
        return false;
    }

    ElfHandle in_elf_guard(elf_memory(
        const_cast<char*>(reinterpret_cast<const char*>(input_data.data())), input_data.size()));
    Elf* in_elf = in_elf_guard.get();  // 守卫 + 别名，同上；下面 4 处手写 elf_end 全部去掉
    if (!in_elf) {
        error_msg = get_string("error.strip_malformed_elf");
        return false;
    }

    GElf_Ehdr ehdr;
    if (gelf_getehdr(in_elf, &ehdr) == nullptr) {
        error_msg = get_string("error.strip_malformed_elf");
        return false;
    }

    // 端序检查（2026-10-03 修）：本文件的写出路径（`write_ehdr` / `write_shdr`）用
    // `reinterpret_cast<Hdr*>` 按**本机字节序**改写 ELF 头与节区表，而节区数据是从输入
    // 原样 `memcpy` 的。于是**大端** ELF（`EI_DATA == ELFDATA2MSB`）会被写成一个
    // "头/节区表是本机序、节区数据仍是大端"的**混合端序**文件 —— 静默损坏。
    // 我们不做端序转换（x86_64 发行版遇不到，不值得那套复杂度），改为**拒绝 strip 非本机
    // 端序**：调用方（`strip_binary`）会把它降级为"跳过 strip + 告警"，原文件原样保留。
    // `input_data[EI_DATA]`（偏移 5，非 ELF 文件在 `gelf_getehdr` 那步已返回）是权威判据 ——
    // libelf 会替我们按 `EI_DATA` 翻译字段，所以**不能**靠 `GElf_Ehdr` 判断原文件端序。
    if (input_data.size() < EI_NIDENT ||
        input_data[EI_DATA] !=
            ((std::endian::native == std::endian::little) ? ELFDATA2LSB : ELFDATA2MSB)) {
        error_msg = get_string("error.strip_foreign_endian");
        return false;
    }

    size_t shnum;
    if (elf_getshdrnum(in_elf, &shnum) != 0) {
        error_msg = get_string("error.strip_malformed_elf");
        return false;
    }
    // **没有节区表 ⇒ 静默返回**（2026-10-03 订正：一度给它加了 `error.strip_no_sections`
    // 告警，**那是错的**）。"没有节区表"是**正常形态**、不是异常：被完整 strip 过的二进制
    // 就长这样 —— 本仓库自己的 `make docker` 产物 `build/lpkg-docker` 实测 `readelf -h`
    // 就是 `Number of section headers: 0`（UPX 打包的二进制同理）。strip 是**尽力而为**的
    // 构建步骤，对"没什么可剥"的文件出声只会让每次构建都刷一行无意义的告警。
    // 判据：**没有可剥的东西 ≠ 失败**。
    if (shnum == 0 || ehdr.e_shoff == 0) return false;

    size_t shstrndx;
    if (elf_getshdrstrndx(in_elf, &shstrndx) < 0) {
        error_msg = get_string("error.strip_malformed_elf");
        return false;
    }

    int elf_class = gelf_getclass(in_elf);

    bool result = false;
    if (ehdr.e_type == ET_REL) {
        result = strip_elf_rel_object(in_elf, ehdr, shstrndx, elf_class, output_data, error_msg);
    } else {
        result = strip_elf_exec_dyn(in_elf, ehdr, shstrndx, elf_class, input_data, output_data,
                                    error_msg);
    }
    return result;  // `in_elf` 由 RAII 收尾
}

/** 读取 ELF 文件内容，调用 strip_elf_data 处理后写回原文件 */
bool process_elf(const fs::path& path, std::string& error_msg)
{
    std::ifstream is(path, std::ios::binary | std::ios::ate);
    if (!is) {
        error_msg = string_format("error.open_file_failed", path.string());
        return false;
    }
    std::streamsize size = is.tellg();
    is.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    if (!is.read(reinterpret_cast<char*>(buffer.data()), size)) {
        error_msg = string_format("error.file_read_failed", path.string());
        return false;
    }
    is.close();

    std::vector<uint8_t> output_buffer;
    if (!strip_elf_data(buffer, output_buffer, error_msg)) return false;

    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    // ⚠️ 每个失败分支**都要填 `error_msg`** —— 唯一的消费者是 `strip_binary` 里的
    // `if (!strip_file(...) && !error_msg.empty())`（本文件末尾的 `strip_binary`；订正
    // 2026-10-03：这里原写 `strip.cpp:847`，那行号早已漂移，改成不依赖行号的表述）：空串 =
    // 检测到了也一声不响。`process_archive` 一直填，这里此前（含我最初只补返回值的那版）没填，
    // 是两处不一致。
    if (!os) {
        error_msg = string_format("error.open_file_failed", path.string());
        return false;
    }
    os.write(reinterpret_cast<const char*>(output_buffer.data()), output_buffer.size());
    // `ofstream` 不抛：短写 / ENOSPC / EIO 只置 badbit。不检查就会把**截断的**产物当成
    // strip 成功；而文件已用 trunc 打开 —— 此刻原内容已经没了，失败必须报出来（带路径）。
    os.flush();
    if (!os) {
        error_msg = string_format("error.strip_write_failed", path.string());
        return false;
    }
    return true;
}

/**
 * 预扫静态库：判断它**值不值得重写**，并挡掉我们写不出来的形态。
 *
 * **为什么必须先扫一遍**：符号表成员（名为 `/`）恒是整个归档的**第一个**成员，而它记录的
 * 是一串"符号 → 字节偏移"，那些偏移只在**没有任何成员改变长度**时才继续成立。读到 `/` 时
 * 我们尚未知道后面有没有 `.o` 成员会在 strip 后变短，所以必须先看一遍全库再决定写出方案。
 *
 * 返回 1 = 有成员会变短（需重写）；0 = 没有（原库可**原样保留**，有效的符号表也就自然保住）；
 * -1 = 放弃这个库。**`error_msg` 空 = 有意的跳过**（见下），非空 = 真失败（调用方会告警）。
 *
 * 出参 `has_long_name_table`：归档里是否带 `//` 长名表成员。**必须带出去** —— 重写时 `//`
 * 恒定被丢弃（libarchive 已把它的 size 归零，照抄只会写出一张空表），而丢掉它会让其后所有
 * 成员前移 ⇒ 照抄的符号索引（`/`、`/SYM64/`、`__.SYMDEF`）偏移全失效。所以"要不要丢索引"
 * 不能只看 `scan`，还得看这里。（**别**假设"有 `//` 就一定存在 >15 字节的名字"—— 外来/构造
 * 的归档完全可以带一张没人引用的 `//` 表，子审计逐字节复刻验证过索引 stale 0 → 2。）
 *
 * 有意的跳过：成员名超过 15 字节。GNU/SVR4 ar 的名字字段只有 16 字节（含结尾 `/`），
 * libarchive 的 ar 写入器写不了更长的名字 —— 实测它会让 `archive_write_header` 失败，
 * 而且**继续写下去的成员会被静默丢掉**（实测：长名成员在输出里整个消失，只剩后面的）。
 * 这种库我们不碰、原样留着。这是**工具的固有限制**而不是错误，所以不告警（否则 llvm 那种
 * 一堆 `libclang_rt.*.a`（成员名如 `asan_preinit.cpp.o`）每次构建都会刷屏）。
 */
static int archive_strip_scan(const fs::path& path, std::string& error_msg,
                              bool& has_long_name_table)
{
    constexpr std::size_t AR_NAME_MAX = 15;  // 16 字节字段去掉结尾的 `/`
    has_long_name_table = false;

    struct archive* a = archive_read_new();
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, path.c_str(), constants::ARCHIVE_BUFFER_SIZE) != ARCHIVE_OK) {
        archive_read_free(a);
        error_msg = get_string("error.strip_archive_open");
        return -1;
    }
    int result = 0;
    struct archive_entry* entry;
    while (true) {
        const int r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF) break;
        if (r < ARCHIVE_OK) {
            error_msg = get_string("error.strip_archive_broken");
            result = -1;
            break;
        }
        const char* cname = archive_entry_pathname(entry);
        const std::string name = cname ? cname : std::string();
        // `//` 长名表：libarchive 原样返回它（size 已归零）。记下来给 process_archive —— 重写
        // 时它会被丢弃，其后成员前移，索引必须跟着重写（判据详见函数头注释）。
        if (name == "//") has_long_name_table = true;
        if (name != "/" && name != "//" && name.size() > AR_NAME_MAX) {
            result = -1;  // 写不出来 → 整个库放弃（有意，不告警：见函数注释）
            break;
        }
        const size_t size = archive_entry_size(entry);
        std::vector<uint8_t> data(size);
        const ssize_t got = archive_read_data(a, data.data(), size);
        if (got < 0 || static_cast<size_t>(got) != size) {
            error_msg = get_string("error.strip_archive_broken");
            result = -1;
            break;
        }
        if (std::string_view(name).ends_with(".o")) {
            std::vector<uint8_t> stripped;
            std::string inner;
            if (strip_elf_data(data, stripped, inner) && stripped.size() != size) {
                result = 1;
                break;  // 已足以判定"要重写"，不必读完
            }
        }
    }
    archive_read_free(a);
    return result;
}

/**
 * 处理静态库（ar 归档文件）：遍历其中的 .o 文件，对每个进行 ELF strip，
 * 重新打包为新的归档文件
 *
 * ⚠️ **只要重写会改变成员偏移，就必须丢掉归档符号索引**（`/` 32 位、`/SYM64/` 64 位 GNU、
 * `__.SYMDEF` / `__.SYMDEF SORTED` BSD）：索引存的是一串"符号 → 字节偏移"，偏移一旦错位就是
 * **链接硬失败** —— `ld: error adding symbols: no more archived files`（本机 `bison` /
 * `nspr` / `gcc` 三个包的 `.a` 曾被这样写坏）。丢掉索引后 `ld` 会退化成顺序扫描全部成员，
 * 链接照常；要索引的一方可以自己 `ranlib`。
 *
 * 会造成偏移改变的有两种，**判据必须两个都看**（2026-10-02 只看了前者）：
 *   ① 有 `.o` 成员在 strip 后**变短**（预扫的 `scan == 1`）；
 *   ② 归档里有 `//` 长名表 —— 它恒定被丢弃（见下），其后成员整体前移。
 * ② 正是"外来/构造归档"能钻的空子：一张没人引用的 `//` 表配上有效的 `/` 索引，成员没变短
 * 也照样让索引失效（子审计逐字节复刻验证过 stale 0 → 2）。因此 `//` 与索引**同一判据**。
 */
bool process_archive(const fs::path& path, std::string& error_msg)
{
    // 先扫一遍：长名成员 → 放弃（写不出来）；真失败 → `error_msg` 已填、由调用方告警；
    // 有无 `//` 长名表 → 由 `has_long_name_table` 带出（它也决定索引能不能保留）。
    bool has_long_name_table = false;
    const int scan = archive_strip_scan(path, error_msg, has_long_name_table);
    if (scan < 0) return false;
    const bool index_invalid = (scan == 1) || has_long_name_table;

    struct archive* a = archive_read_new();
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, path.c_str(), constants::ARCHIVE_BUFFER_SIZE) != ARCHIVE_OK) {
        archive_read_free(a);
        error_msg = get_string("error.strip_archive_open");
        return false;
    }

    struct archive* out = archive_write_new();
    archive_write_set_format_ar_svr4(out);

    fs::path temp_path = path.string() + ".tmp";
    if (archive_write_open_filename(out, temp_path.c_str()) != ARCHIVE_OK) {
        archive_read_free(a);
        archive_write_free(out);
        return false;
    }

    struct archive_entry* entry;
    // 逐成员读到 EOF。**不能**用 `== ARCHIVE_OK` 当循环条件：那样第一个 WARN/FATAL 成员
    // 就静默结束循环，若碰巧是首成员则写出一个**空归档**，随后照样 rename 覆盖掉原 `.a`
    // —— 静态库被悄悄清空、构建还报成功（2026-10-02 实测复现：50+ 字节的畸形 ar 变成
    // 8 字节 `!<arch>\n`，`strip_binary` 连告警都不打）。任何非 OK ⇒ 丢弃临时文件、
    // 保留原库、返回 false（调用方 `strip_binary` 只告警，绝不因此失败整个构建）。
    //
    // 收尾统一走 `bail()`：**必须填 `error_msg`** —— 调用方的告警判据是
    // `!strip_file(...) && !error_msg.empty()`，此前这些分支一个都不设它，于是**任何失败
    // 都不出声**（实测：成员名超过 15 字节的静态库整库静默不剥离，谁也不告诉）。
    const auto bail = [&]() {
        if (error_msg.empty()) error_msg = get_string("error.strip_archive_broken");
        archive_read_free(a);
        archive_write_close(out);
        archive_write_free(out);
        fs::remove(temp_path);
        return false;
    };

    while (true) {
        const int r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF) break;
        if (r < ARCHIVE_OK) return bail();
        const char* name = archive_entry_pathname(entry);
        const std::string_view nm = name ? std::string_view(name) : std::string_view();
        // 长名表 `//`：libarchive 已把成员名解析成完整长名，并把 `//` 成员的 size **归零** ——
        // 照抄只会写出一张 size=0 的空表（读回时 "Invalid string table" → FATAL），必须丢。
        // ⚠️ **丢 `//` 会让其后成员整体前移**，所以它**不能**单独决定：只要归档里有 `//`，下面
        // 那份索引的偏移就全部失效（判据由预扫统一给出，见 process_archive 的 `index_invalid`）。
        // 订正 2026-10-03：原文写"走到这里说明所有成员名都 ≤15 字节，长名表根本用不上"——
        // **不成立**：外来/构造的归档可以带一张没人引用的 `//` 表，此时成员名确实都 ≤15，
        // 但丢掉 `//` 仍会让索引失效（子审计逐字节复刻验证过 stale 0 → 2）。
        if (nm == "//") {
            archive_read_data_skip(a);
            continue;
        }
        // 归档符号索引：`/`（32 位 GNU）、`/SYM64/`（64 位 GNU）、`__.SYMDEF` /
        // `__.SYMDEF SORTED`（BSD ranlib 表）—— 记录的都是"符号 → 字节偏移"。
        // **只要有一个成员变短、或归档里丢了 `//` 长名表**，那串偏移就全失效，必须一起丢
        // （此前只认 `/`：`/SYM64/` / `__.SYMDEF` 会被原样照抄 → 偏移错位）。若两者都不成立
        // （预扫给 0 且无 `//`），偏移仍然成立，照抄即可（保住索引这个优化）。
        // 注：`__.SYMDEF SORTED` 有 16 字节，会被预扫的"名字 >15 → 放弃整个库"先拦下，
        // 故当前走不到这里；一并列出是为了判据对齐（BSD 的排序表同样是索引）。
        if ((nm == "/" || nm == "/SYM64/" || nm == "__.SYMDEF" || nm == "__.SYMDEF SORTED") &&
            index_invalid) {
            archive_read_data_skip(a);
            continue;
        }
        const size_t size = archive_entry_size(entry);
        std::vector<uint8_t> data(size);

        const ssize_t bytes_read = archive_read_data(a, data.data(), size);
        if (bytes_read < 0 || static_cast<size_t>(bytes_read) != size) return bail();

        // 对 .o 目标文件进行 strip 处理
        if (nm.ends_with(".o")) {
            std::vector<uint8_t> stripped_data;
            std::string inner_error_msg;
            if (strip_elf_data(data, stripped_data, inner_error_msg)) {
                archive_entry_set_size(entry, stripped_data.size());
                if (archive_write_header(out, entry) != ARCHIVE_OK) return bail();
                const ssize_t written =
                    archive_write_data(out, stripped_data.data(), stripped_data.size());
                if (written < 0 || static_cast<size_t>(written) != stripped_data.size())
                    return bail();
                continue;
            }
        }

        if (archive_write_header(out, entry) != ARCHIVE_OK) return bail();
        const ssize_t written = archive_write_data(out, data.data(), size);
        if (written < 0 || static_cast<size_t>(written) != size) return bail();
    }

    // **完整性判据（2026-10-02）**：libarchive 的 ar 读取器在"magic 之后读不出成员头"时
    // **不报错**，而是直接返回 ARCHIVE_EOF（实测：`!<arch>\n` + 54 字节垃圾 与 合法的 8 字节
    // 空归档，两者 next_header 都立即 EOF、成员数都是 0）—— 于是整个库被吞掉、写出一个
    // **空归档**、rename 覆盖原文件、返回 true（端到端实测：`lpkg build` 报成功，`.lpkg` 里
    // 的 `.a` 只剩 8 字节）。判据取"**解析器有没有把整个文件消费掉**"：合法归档在 EOF 时
    // `filter_bytes == 文件大小`（实测 1370→1370、2668→2668；畸形的是 8→62）。
    // 任何未消费的尾部 = 我们读不懂的内容 = 不能悄悄丢掉。
    const la_int64_t consumed = archive_filter_bytes(a, -1);
    archive_read_free(a);
    if (consumed < 0 || consumed != static_cast<la_int64_t>(fs::file_size(path))) {
        error_msg = get_string("error.strip_archive_broken");
        archive_write_close(out);
        archive_write_free(out);
        fs::remove(temp_path);
        return false;
    }

    // close 的返回值就是"整包是否真的写完落盘"：写失败（磁盘满/EIO）会返回
    // ARCHIVE_FAILED/FATAL。此前**完全忽略**它，于是用**截断的**归档覆盖原 `.a`（与
    // `packer.cpp` 对 close 的处置对齐 —— 那里检查得很仔细，这里漏了）。
    const int close_rc = archive_write_close(out);
    archive_write_free(out);
    if (close_rc != ARCHIVE_OK) {
        error_msg = get_string("error.strip_archive_broken");
        fs::remove(temp_path);
        return false;
    }

    try {
        ::safe_rename(temp_path, path);
        return true;
    } catch (const std::exception&) {
        fs::remove(temp_path);
        // 拒绝必须带原因（同上：空串 = 调用方静默跳过）。2026-10-03 修。
        error_msg = get_string("error.strip_archive_broken");
        return false;
    }
}

/**
 * 对二进制文件执行 strip 操作
 * 失败时仅记录警告而不中断流程
 *
 * （2026-09-26 从 `base/utils.cpp` 搬来：见 `strip.hpp` 里的说明 —— 它属于本层。）
 */
void strip_binary(const fs::path& path)
{
    // strip 是**尽力而为**的步骤：出任何问题都只该让包大一点，绝不能失败整个构建。
    // 曾因 strip 内部异常（SHT_NOBITS 的 d_buf 为 NULL → bad_alloc）逃出本函数，
    // 把 lankebuild_package 阶段整个打死（llvm 白跑一次）。这里兜住所有异常。
    try {
        std::string error_msg;
        if (!strip_file(path, error_msg) && !error_msg.empty()) {
            log_warning(string_format("warning.strip_failed", path.string(), error_msg));
        }
    } catch (const std::exception& e) {
        log_warning(string_format("warning.strip_failed", path.string(), e.what()));
    } catch (...) {
        log_warning(
            string_format("warning.strip_failed", path.string(), get_string("error.unknown")));
    }
}
