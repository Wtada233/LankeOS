#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "strip.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <format>
#include <span>
#include <string_view>
#include <cstring>
#include <map>
#include <algorithm>

#include <libelf.h>
#include <gelf.h>
#include <archive.h>
#include <archive_entry.h>

#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

namespace fs = std::filesystem;

enum class FileType {
    Unknown,
    Executable,
    SharedLibrary,
    StaticLibrary,
    ObjectFile
};

// strip_file 中使用的辅助函数的前向声明
FileType identify_file_type(const fs::path& path);
bool process_elf(const fs::path& path, std::string& error_msg);
bool process_archive(const fs::path& path, std::string& error_msg);

#include "i18n/localization.hpp"
#include <iostream>

/**
 * 对指定文件执行 strip 操作，去除调试信息和符号表
 * 根据文件类型（ELF 可执行文件、共享库、目标文件、静态库）分发到对应的处理函数
 */
bool strip_file(const fs::path& path, std::string& error_msg) {
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
FileType identify_file_type(const fs::path& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return FileType::Unknown;

    Elf* elf = elf_begin(fd, ELF_C_READ, nullptr);
    if (!elf) { close(fd); return FileType::Unknown; }

    if (elf_kind(elf) == ELF_K_AR) {
        elf_end(elf); close(fd);
        return FileType::StaticLibrary;
    }

    GElf_Ehdr ehdr;
    if (gelf_getehdr(elf, &ehdr) == nullptr) {
        elf_end(elf); close(fd); return FileType::Unknown;
    }

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
        if (elf_getphdrnum(elf, &phnum) == 0) {
            for (size_t i = 0; i < phnum; ++i) {
                GElf_Phdr phdr;
                if (gelf_getphdr(elf, i, &phdr) != nullptr) {
                    if (phdr.p_type == PT_INTERP) {
                        has_pt_interp = true;
                        break;
                    }
                }
            }
        }

        bool has_soname = false;
        Elf_Scn* scn = nullptr;
        while ((scn = elf_nextscn(elf, scn)) != nullptr) {
            GElf_Shdr shdr; gelf_getshdr(scn, &shdr);
            if (shdr.sh_type == SHT_DYNAMIC) {
                Elf_Data* data = elf_getdata(scn, nullptr);
                if (data) {
                    size_t ext_count = shdr.sh_size / shdr.sh_entsize;
                    for (size_t i = 0; i < ext_count; ++i) {
                        GElf_Dyn dyn; gelf_getdyn(data, i, &dyn);
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

    elf_end(elf); close(fd);
    return type;
}

/**
 * 对可重定位文件（ET_REL .o 文件）进行 strip
 * 通过 libelf API 重建文件，只保留非调试节区，
 * 同时更新节区索引映射（包括 SHT_SYMTAB 中的符号索引）
 */
static bool strip_elf_rel_object(Elf* in_elf, const GElf_Ehdr& ehdr, size_t shstrndx, int elf_class,
                                 std::vector<uint8_t>& output_data) {
    int out_fd = memfd_create("strip_out", 0);
    if (out_fd < 0) return false;
    Elf* out_elf = elf_begin(out_fd, ELF_C_WRITE, nullptr);
    gelf_newehdr(out_elf, elf_class);
    GElf_Ehdr mutable_ehdr = ehdr;
    gelf_update_ehdr(out_elf, &mutable_ehdr);

    std::map<size_t, size_t> idx_map;
    idx_map[0] = 0; idx_map[SHN_ABS] = SHN_ABS; idx_map[SHN_COMMON] = SHN_COMMON; idx_map[SHN_UNDEF] = SHN_UNDEF;

    struct SectionInfo { Elf_Scn* old_scn; GElf_Shdr shdr; std::string name; size_t old_idx; };
    std::vector<SectionInfo> to_keep;
    Elf_Scn* scn = nullptr;
    size_t new_idx = 1;

    while ((scn = elf_nextscn(in_elf, scn)) != nullptr) {
        size_t old_idx = elf_ndxscn(scn);
        GElf_Shdr shdr; gelf_getshdr(scn, &shdr);
        const char* name_ptr = elf_strptr(in_elf, shstrndx, shdr.sh_name);
        std::string name = name_ptr ? name_ptr : "";

        bool keep = true;
        // 过滤掉调试信息和注释节区
        if (name.starts_with(".debug") || name.starts_with(".rela.debug") || name.starts_with(".rel.debug") || name == ".comment") keep = false;

        if (keep) {
            to_keep.push_back({scn, shdr, name, old_idx});
            idx_map[old_idx] = new_idx++;
        }
    }

    for (auto& info : to_keep) {
        Elf_Scn* new_scn = elf_newscn(out_elf);
        GElf_Shdr new_shdr = info.shdr;

        // 重映射节区链接索引
        if (idx_map.count(new_shdr.sh_link)) new_shdr.sh_link = idx_map[new_shdr.sh_link];
        else if (new_shdr.sh_link != 0) new_shdr.sh_link = SHN_UNDEF;

        if (new_shdr.sh_type == SHT_REL || new_shdr.sh_type == SHT_RELA || (new_shdr.sh_flags & SHF_INFO_LINK)) {
            if (idx_map.count(new_shdr.sh_info)) new_shdr.sh_info = idx_map[new_shdr.sh_info];
            else new_shdr.sh_info = 0;
        }

        gelf_update_shdr(new_scn, &new_shdr);
        Elf_Data* in_data = elf_getdata(info.old_scn, nullptr);
        Elf_Data* out_data = elf_newdata(new_scn);
        if (in_data) {
            *out_data = *in_data;
            // 更新符号表中的节区索引
            if (new_shdr.sh_type == SHT_SYMTAB) {
                size_t sym_count = out_data->d_size / gelf_fsize(out_elf, ELF_T_SYM, 1, EV_CURRENT);
                for (size_t i = 0; i < sym_count; ++i) {
                    GElf_Sym sym; gelf_getsym(out_data, i, &sym);
                    if (sym.st_shndx < SHN_LORESERVE) {
                        if (idx_map.count(sym.st_shndx)) sym.st_shndx = idx_map[sym.st_shndx];
                        else if (sym.st_shndx != SHN_UNDEF) sym.st_shndx = SHN_UNDEF;
                    }
                    gelf_update_sym(out_data, i, &sym);
                }
            }
        }
    }

    GElf_Ehdr updated_ehdr = ehdr;
    if (idx_map.count(shstrndx)) updated_ehdr.e_shstrndx = idx_map[shstrndx];
    else updated_ehdr.e_shstrndx = SHN_UNDEF;

    gelf_update_ehdr(out_elf, &updated_ehdr);
    elf_update(out_elf, ELF_C_WRITE);

    off_t size = lseek(out_fd, 0, SEEK_END);
    output_data.resize(size); lseek(out_fd, 0, SEEK_SET);
    [[maybe_unused]] ssize_t bytes_read = read(out_fd, output_data.data(), size);

    elf_end(out_elf); close(out_fd);
    return true;
}

/**
 * 对可执行文件和共享库（ET_EXEC / ET_DYN）进行 strip
 * 手动重建 ELF 布局，仅保留 SHF_ALLOC 节区以及必要的 .shstrtab 和字符串表，
 * 丢弃 .symtab、.strtab、.comment 和 .debug* 节区
 */
static bool strip_elf_exec_dyn(Elf* in_elf, const GElf_Ehdr& ehdr, size_t shstrndx, int elf_class,
                               const std::vector<uint8_t>& input_data, std::vector<uint8_t>& output_data) {
    struct KeptSection {
        GElf_Shdr shdr;
        size_t old_index;
        size_t new_index;
    };

    std::vector<KeptSection> kept_sections;
    std::map<size_t, size_t> el_idx_map;

    kept_sections.push_back({ {}, 0, 0 });
    el_idx_map[0] = 0; el_idx_map[SHN_ABS] = SHN_ABS; el_idx_map[SHN_COMMON] = SHN_COMMON; el_idx_map[SHN_UNDEF] = SHN_UNDEF;

    uint64_t max_alloc_end = 0;
    Elf_Scn* scn = nullptr;
    size_t current_new_idx = 1;

    while ((scn = elf_nextscn(in_elf, scn)) != nullptr) {
        size_t old_idx = elf_ndxscn(scn);
        GElf_Shdr shdr; gelf_getshdr(scn, &shdr);
        const char* name_ptr = elf_strptr(in_elf, shstrndx, shdr.sh_name);
        std::string name = name_ptr ? name_ptr : "";

        bool keep = true;
        // 保留 SHF_ALLOC 节区（运行时需要的节区）
        if (!(shdr.sh_flags & SHF_ALLOC)) {
            if (name != ".shstrtab" && shdr.sh_type != SHT_STRTAB) keep = false;
        }
        // 丢弃符号表、字符串表、注释和调试信息
        if (name == ".symtab" || name == ".strtab" || name == ".comment" || name.starts_with(".debug")) keep = false;

        if (keep) {
            kept_sections.push_back({ shdr, old_idx, current_new_idx });
            el_idx_map[old_idx] = current_new_idx;
            current_new_idx++;

            if ((shdr.sh_flags & SHF_ALLOC) && shdr.sh_type != SHT_NOBITS) {
                max_alloc_end = std::max(max_alloc_end, shdr.sh_offset + shdr.sh_size);
            }
        }
    }

    // 对非 SHF_ALLOC 节区重新排列，紧跟在已分配数据之后
    uint64_t current_offset = (max_alloc_end + 15) & ~15;
    for (size_t i = 1; i < kept_sections.size(); ++i) {
        auto& ks = kept_sections[i];
        if (!(ks.shdr.sh_flags & SHF_ALLOC)) {
            ks.shdr.sh_offset = current_offset;
            current_offset += ks.shdr.sh_size;
        }

        if (el_idx_map.count(ks.shdr.sh_link)) {
            ks.shdr.sh_link = el_idx_map[ks.shdr.sh_link];
        } else if (ks.shdr.sh_link != 0) {
            ks.shdr.sh_link = SHN_UNDEF;
        }

        if (ks.shdr.sh_type == SHT_REL || ks.shdr.sh_type == SHT_RELA || (ks.shdr.sh_flags & SHF_INFO_LINK)) {
            if (el_idx_map.count(ks.shdr.sh_info)) {
                ks.shdr.sh_info = el_idx_map[ks.shdr.sh_info];
            } else {
                ks.shdr.sh_info = 0;
            }
        }
    }

    size_t shentsize = (elf_class == ELFCLASS64) ? sizeof(Elf64_Shdr) : sizeof(Elf32_Shdr);
    GElf_Ehdr updated_ehdr = ehdr;
    updated_ehdr.e_shoff = (current_offset + 15) & ~15;
    updated_ehdr.e_shnum = kept_sections.size();

    if (el_idx_map.count(shstrndx)) updated_ehdr.e_shstrndx = el_idx_map[shstrndx];
    else updated_ehdr.e_shstrndx = SHN_UNDEF;

    output_data.resize(updated_ehdr.e_shoff + updated_ehdr.e_shnum * shentsize);

    for (size_t i = 1; i < kept_sections.size(); ++i) {
        const auto& ks = kept_sections[i];
        Elf_Scn* old_scn = elf_getscn(in_elf, ks.old_index);
        if (old_scn) {
            GElf_Shdr old_shdr; gelf_getshdr(old_scn, &old_shdr);
            if (old_shdr.sh_type != SHT_NOBITS && old_shdr.sh_size > 0) {
                // 确保不会越界读取（防御性边界检查）
                if (old_shdr.sh_offset + old_shdr.sh_size <= input_data.size()) {
                    std::memcpy(output_data.data() + ks.shdr.sh_offset, input_data.data() + old_shdr.sh_offset, ks.shdr.sh_size);
                }
            }
        }
    }

    // 复制 ELF 头和程序头到输出缓冲区
    size_t headers_size = updated_ehdr.e_phoff + updated_ehdr.e_phnum * ((elf_class == ELFCLASS64) ? sizeof(Elf64_Phdr) : sizeof(Elf32_Phdr));
    headers_size = std::max(headers_size, (size_t)updated_ehdr.e_ehsize);
    std::memcpy(output_data.data(), input_data.data(), std::min(headers_size, input_data.size()));

    // 写入更新后的 ELF 头（节区表偏移、数量和字符串表索引）
    auto write_ehdr = [&]<typename Hdr>() {
        Hdr* out = reinterpret_cast<Hdr*>(output_data.data());
        out->e_shoff = updated_ehdr.e_shoff; out->e_shnum = updated_ehdr.e_shnum; out->e_shstrndx = updated_ehdr.e_shstrndx;
    };
    if (elf_class == ELFCLASS64) write_ehdr.operator()<Elf64_Ehdr>();
    else write_ehdr.operator()<Elf32_Ehdr>();

    // 写入更新后的节区表
    auto write_shdr = [&]<typename Shdr>(size_t i) {
        Shdr* out = reinterpret_cast<Shdr*>(output_data.data() + updated_ehdr.e_shoff + i * shentsize);
        out->sh_name = kept_sections[i].shdr.sh_name; out->sh_type = kept_sections[i].shdr.sh_type; out->sh_flags = kept_sections[i].shdr.sh_flags;
        out->sh_addr = kept_sections[i].shdr.sh_addr; out->sh_offset = kept_sections[i].shdr.sh_offset; out->sh_size = kept_sections[i].shdr.sh_size;
        out->sh_link = kept_sections[i].shdr.sh_link; out->sh_info = kept_sections[i].shdr.sh_info; out->sh_addralign = kept_sections[i].shdr.sh_addralign;
        out->sh_entsize = kept_sections[i].shdr.sh_entsize;
    };
    for (size_t i = 0; i < kept_sections.size(); ++i) {
        if (elf_class == ELFCLASS64) write_shdr.operator()<Elf64_Shdr>(i);
        else write_shdr.operator()<Elf32_Shdr>(i);
    }
    return true;
}

/**
 * 对 ELF 二进制数据进行 strip 操作的入口
 * 根据 ELF 类型分派到对应的子函数
 */
bool strip_elf_data(const std::vector<uint8_t>& input_data, std::vector<uint8_t>& output_data, [[maybe_unused]] std::string& error_msg) {
    if (elf_version(EV_CURRENT) == EV_NONE) return false;

    Elf* in_elf = elf_memory(const_cast<char*>(reinterpret_cast<const char*>(input_data.data())), input_data.size());
    if (!in_elf) return false;

    GElf_Ehdr ehdr;
    if (gelf_getehdr(in_elf, &ehdr) == nullptr) { elf_end(in_elf); return false; }

    size_t shnum;
    if (elf_getshdrnum(in_elf, &shnum) != 0 || shnum == 0 || ehdr.e_shoff == 0) {
        elf_end(in_elf);
        return false;
    }

    size_t shstrndx;
    if (elf_getshdrstrndx(in_elf, &shstrndx) < 0) { elf_end(in_elf); return false; }

    int elf_class = gelf_getclass(in_elf);

    bool result = false;
    if (ehdr.e_type == ET_REL) {
        result = strip_elf_rel_object(in_elf, ehdr, shstrndx, elf_class, output_data);
    } else {
        result = strip_elf_exec_dyn(in_elf, ehdr, shstrndx, elf_class, input_data, output_data);
    }
    elf_end(in_elf);
    return result;
}

/** 读取 ELF 文件内容，调用 strip_elf_data 处理后写回原文件 */
bool process_elf(const fs::path& path, std::string& error_msg) {
    std::ifstream is(path, std::ios::binary | std::ios::ate);
    if (!is) return false;
    std::streamsize size = is.tellg();
    is.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    if (!is.read(reinterpret_cast<char*>(buffer.data()), size)) return false;
    is.close();

    std::vector<uint8_t> output_buffer;
    if (!strip_elf_data(buffer, output_buffer, error_msg)) return false;

    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os) return false;
    os.write(reinterpret_cast<const char*>(output_buffer.data()), output_buffer.size());
    return true;
}

/**
 * 处理静态库（ar 归档文件）：遍历其中的 .o 文件，对每个进行 ELF strip，
 * 重新打包为新的归档文件
 */
bool process_archive(const fs::path& path, [[maybe_unused]] std::string& error_msg) {
    struct archive* a = archive_read_new();
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, path.c_str(), 10240) != ARCHIVE_OK) {
        archive_read_free(a);
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
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char* name = archive_entry_pathname(entry);
        size_t size = archive_entry_size(entry);
        std::vector<uint8_t> data(size);

        archive_read_data(a, data.data(), size);

        // 对 .o 目标文件进行 strip 处理
        if (std::string_view(name).ends_with(".o")) {
            std::vector<uint8_t> stripped_data;
            std::string inner_error_msg;
            if (strip_elf_data(data, stripped_data, inner_error_msg)) {
                archive_entry_set_size(entry, stripped_data.size());
                archive_write_header(out, entry);
                archive_write_data(out, stripped_data.data(), stripped_data.size());
                continue;
            }
        }

        archive_write_header(out, entry);
        archive_write_data(out, data.data(), size);
    }

    archive_read_free(a);
    archive_write_close(out);
    archive_write_free(out);

    std::error_code ec;
    fs::rename(temp_path, path, ec);
    return !ec;
}
