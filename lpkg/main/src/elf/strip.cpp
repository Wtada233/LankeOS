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

#include "base/fs_atomic.hpp"        // fsync_and_rename（原子落位只此一处，见 process_elf）
#include "base/path_predicates.hpp"  // is_symlink_no_follow（暂存路径的守卫，不抛）
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
                                 std::vector<uint8_t>& output_data, std::string& error_msg,
                                 size_t input_size, const std::string& source_path)
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
    // 被保留、且**带数据**的节区的累计字节数 —— 下面对每个这样的节区各复制一份自有缓冲
    // （`owned_data`），`elf_update` 还会再按 `sh_size` 布局输出。封顶判据在下面 `keep` 分支里。
    size_t retained_data_total = 0;
    // 被保留节区的 `sh_addralign` 之和 —— 见下面 `keep` 分支里"对齐填充"那条封顶。
    size_t retained_align_total = 0;

    while ((scn = elf_nextscn(in_elf, scn)) != nullptr) {
        size_t old_idx = elf_ndxscn(scn);
        GElf_Shdr shdr;
        if (gelf_getshdr(scn, &shdr) == nullptr) continue;
        // 用**有界**取值：`elf_strptr` + 判空挡不住"offset 在节区内但 NUL 在节区外"
        // （见 lib_utils.hpp 的 `elf_strtab_get`，那条洞是 fuzz 实测出来的）。
        const std::string name(elf_strtab_get(in_elf, shstrndx, shdr.sh_name));

        bool keep = true;
        // 过滤掉调试信息和注释节区
        if (name.starts_with(".debug") || name.starts_with(".rela.debug") ||
            name.starts_with(".rel.debug") || name == ".comment")
            keep = false;

        // **量级封顶（2026-10-03 修）**：`sh_size` 是**输入可控**的，而下面会把整份节区头
        // （含 `sh_size`）写进输出 ELF，再交给 `elf_update` 去布局 —— libelf 会**按这些尺寸
        // 创建/分配空间**。实测（fuzz harness 抓到、再最小复现）：一个 **1224 字节**的 ET_REL
        // 声明了 192 GB 的节区 ⇒ `elf_update` 去申请上百 GB ⇒ 容器 cgroup 的 shmem 冲到上限、
        // 全局 OOM。**合法 ELF 的节区数据必然落在文件内**，所以用输入大小封顶 ——
        // 与 `strip_elf_exec_dyn` 那条路是**同一判据**（那边判的是"重排后的输出不可能大于输入"）。
        // 判据写全：**带数据的节区**（SHT_NOBITS 的 off/size 在 ELF 里本就是无意义的）
        // 其 [sh_offset, sh_offset+sh_size) 必须整段落在输入文件内。既挡 `sh_size` 巨大
        // （最小复现里是 192 GB），也挡 `sh_offset` 巨大（同一份复现里第 8 节区 off=480 GB）。
        // 用减法而不是加法判越界，避免 `sh_offset + sh_size` 回绕。
        if (shdr.sh_type != SHT_NOBITS &&
            (shdr.sh_offset > input_size || shdr.sh_size > input_size - shdr.sh_offset)) {
            error_msg = get_string("error.strip_malformed_elf");
            return false;
        }
        // **`sh_addralign` 也必须封顶**（2026-10-03 由 fuzz harness 抓到、再用"只改这一个字段"
        // 的最小复现钉死）：`elf_update` 会**按 `sh_addralign` 对齐每个节区的落点** —— 一个
        // `sh_addralign = 2^56` 的节区就让输出文件的偏移跳到 64 PiB，memfd 随之被 `ftruncate`
        // 到 64 PiB 并**真实占掉几十 GB**（实测：宿主内存 7 秒掉 48 GB，进程自身 RSS 只有
        // 0.4 GB，每次都被内存上限杀掉）。这正是先前只封 `sh_offset`/`sh_size` 时漏掉的那一格。
        // 判据同族：**对齐值不可能大于整个输入文件**。`sh_entsize` 是同一类输入可控字段，一并封。
        if (shdr.sh_addralign > input_size || shdr.sh_entsize > input_size) {
            error_msg = get_string("error.strip_malformed_elf");
            return false;
        }
        if (keep) {
            // **聚合量级封顶（2026-10-03 修）**：上面的 `sh_offset`/`sh_size` 守卫只**逐节区**
            // 判"落在文件内"，挡不住**多个节区指向输入里同一片数据**、各自声明 ~input_size 的
            // 形态 —— k 个这样的节区让下面的 `owned_data.emplace_back` 与 `elf_update` 各拷贝
            // k 份 ⇒ 峰值 ~input²/128（实测：**223 KB** 的输入产出 **167 MB** 输出）。
            //
            // 判据与 `strip_elf_exec_dyn` 那条**是同一个不变量**（那边是
            // `e_shoff + e_shnum*shentsize > input_data.size()` 的封顶）：**strip 只删节区 ⇒
            // 产物不可能大于输入**；而良构 ELF 里各节区数据是输入文件内**互不重叠**的子区间，
            // 所以"被保留、且带数据的节区尺寸之和"也必然 ≤ 输入大小。SHT_NOBITS 无数据、不计。
            // 用减法判越界：上面的逐节区守卫已保证每个 `sh_size <= input_size`，且
            // `retained_data_total` 恒 ≤ input_size ⇒ 这个减法不回绕。
            if (shdr.sh_type != SHT_NOBITS) {
                if (shdr.sh_size > input_size - retained_data_total) {
                    error_msg = get_string("error.strip_malformed_elf");
                    return false;
                }
                retained_data_total += shdr.sh_size;
            }
            // **对齐填充的聚合封顶（2026-10-05 修）—— A3 的本体。**
            //
            // 上面每条判据都只盯**单个**输入可控的量：逐节区的 `sh_offset`/`sh_size` 要落在
            // 文件内、`sh_addralign ≤ input_size`、被保留节区的 `sh_size` 之和 ≤ input_size。
            // 而 `elf_update` 会**按 `sh_addralign` 对齐每个节区的落点** ⇒ N 个各自
            // `sh_addralign ≤ input_size`（逐节区守卫恰好放行）、`sh_size = 1`（聚合守卫也恰好
            // 放行）的节区就能把产物撑到 ~N × input_size：**64 KiB 输入产出 4 MiB 填充**
            // （1000 个 align=4096 的节区，见 tests/unit/test_elf_stripping.cpp 的
            // `RelAlignmentPaddingAmplificationIsRefusedNotMaterialized`）；输入再大就是 GB 级。
            //
            // ⚠️ **这条判据必须在 `elf_update` 之前。** 放大发生在 `elf_update` **内部**
            // （它按这些对齐值 ftruncate 那个 memfd / 铺出全部填充），等拿到产物尺寸再判已经晚
            // 了 —— 那时内存（memfd 是 shmem）已经被吃掉，正是本缺陷"strip 放大到 OOM"的现场。
            // 判据与上面 `retained_data_total` **同形**（输入可控字段的聚合上界），量级取
            // **输入大小**：合法文件的对齐值之和远小于文件本身（实测 128 个真实 `.o` —— 本仓库
            // 构建树的测试目标 + 若干 gcc 产物：`Σsh_addralign / 文件大小` 最大 **2.0%**、
            // 中位 **0.8%**），而放大形态需要它 ≫ 输入。`sh_addralign` 的逐节区
            // 守卫已保证每项 ≤ input_size ⇒ 这个减法不回绕。
            if (shdr.sh_addralign > input_size - retained_align_total) {
                error_msg = get_string("error.strip_malformed_elf");
                return false;
            }
            retained_align_total += shdr.sh_addralign;
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
    // **产物尺寸复核（2026-10-05 修）—— 纵深防御，主判据是上面那条对齐聚合封顶。**
    //
    // 产物 = ELF 头 + 节区表 + 各保留节区的数据 + **对齐填充**。前两项与第三项我们都有
    // 确切值（`retained_data_total`），于是这条复核等价于"**对齐填充不得超过输入文件本身**"
    // —— 合法文件里填充只占千分之几（实测同上），放大形态则是输入的几十倍。
    //
    // ⚠️ **为什么它只能当第二道**：判据在 `elf_update` **之后**，而放大恰恰发生在
    // `elf_update` **内部**（它按对齐值铺出全部填充、把 memfd ftruncate 到那个尺寸）——
    // 走到这里时内存已经被吃掉，正是本缺陷的现场。**真正拦住它的是上面那条 pre-update 的
    // 对齐聚合封顶**，这里再核一遍产物，防的是"判据没想到的第三种放大向量"。
    //
    // ⚠️ 判据**不能**写成朴素的 `size > input_size`（2026-10-05 实测**不成立**）：良构
    // ELF 里节区数据可以与被保留的其它区域重叠，重建后 libelf 把每个节区各铺一份 ⇒ 产物合法
    // 地变大。实测 `CraftedElfTest.RelSectionsWithinFileStillStrip` 的输入 **512** 字节、
    // 产物 **592** 字节（那条用例正是"正常形态必须照常 strip"的正面控制）。所以上界里必须
    // 带上"头 + 节区表 + 节区数据"这三项已知量。
    const size_t header_bytes =
        ((elf_class == ELFCLASS64) ? sizeof(Elf64_Ehdr) : sizeof(Elf32_Ehdr)) +
        to_keep.size() * ((elf_class == ELFCLASS64) ? sizeof(Elf64_Shdr) : sizeof(Elf32_Shdr));
    if (static_cast<size_t>(size) > input_size + header_bytes + retained_data_total) {
        // 这里在 `strip_elf_data` 之下，**拿不到路径**（它自己也没有）—— 所以路径由调用方
        // 一路传下来（`source_path`），只为让这条错误点名文件（`strip_binary` 的告警判据是
        // "失败且 error_msg 非空"，不点名等于没报）。`elf_strip_fuzz` 走 3 参重载时它是空串
        // （fuzz 不消费 error_msg）。
        error_msg = string_format("error.strip_output_grew", source_path, static_cast<size_t>(size),
                                  input_size);
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

    kept_sections.push_back({{}, 0});
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
        // 用**有界**取值：`elf_strptr` + 判空挡不住"offset 在节区内但 NUL 在节区外"
        // （见 lib_utils.hpp 的 `elf_strtab_get`，那条洞是 fuzz 实测出来的）。
        const std::string name(elf_strtab_get(in_elf, shstrndx, shdr.sh_name));

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
            kept_sections.push_back({shdr, old_idx});
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

    // 节区数据与节区表的落点必须**整体排在 ELF 头 + 程序头之后**。
    // 少了这道下界时（2026-10-03 由 `tests/fuzz/elf_strip_fuzz.cpp` 实测抓到）：一个
    // "既没有 SHF_ALLOC 数据、又没有非 ALLOC 载荷"的文件会算出 `current_offset == 0`，
    // 于是 `e_shoff == 0`；而 `kept_sections` **永远含节区 0**（null section，内容全零），
    // `write_shdr(0)` 正好写在第 0 字节 —— **把 ELF 头覆盖成全零**，函数却返回 true，
    // `process_elf` 据此把损坏内容写回原文件。程序头一并纳入下界（否则节区表会盖住 phdr）。
    // 复现输入（128 字节、只有一个 null 节区）→ 修复前：返回 true 且输出 64 字节全零。
    const size_t phdr_size = (elf_class == ELFCLASS64) ? sizeof(Elf64_Phdr) : sizeof(Elf32_Phdr);
    const size_t headers_size =
        std::max(static_cast<size_t>(ehdr.e_phoff + ehdr.e_phnum * phdr_size),
                 static_cast<size_t>(ehdr.e_ehsize));

    const uint64_t layout_base = std::max<uint64_t>(max_alloc_end, headers_size);
    if (layout_base > UINT64_MAX - constants::ELF_SECTION_ALIGN_MASK) {
        error_msg = get_string("error.strip_malformed_elf");
        return false;
    }
    uint64_t current_offset =
        (layout_base + constants::ELF_SECTION_ALIGN_MASK) & ~constants::ELF_SECTION_ALIGN_MASK;
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

    // 复制 ELF 头和程序头到输出缓冲区（长度同样必须落在**两个**缓冲内）。
    // 这里直接复用本函数开头算好的 `headers_size`：节区落点的下界要用它，两处各算一份
    // 迟早会漂移（`updated_ehdr` 是 `ehdr` 的副本，只改了 e_shoff/e_shnum/e_shstrndx）。
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
                    std::string& error_msg, const std::string& source_path)
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
        result = strip_elf_rel_object(in_elf, ehdr, shstrndx, elf_class, output_data, error_msg,
                                      input_data.size(), source_path);
    } else {
        result = strip_elf_exec_dyn(in_elf, ehdr, shstrndx, elf_class, input_data, output_data,
                                    error_msg);
    }
    return result;  // `in_elf` 由 RAII 收尾
}

// ⚠️ 这里曾有一个 **3 参重载**（只为当时不能改动的 `tests/fuzz/elf_strip_fuzz.cpp` 转发）。
// 2026-10-05 把那个 harness 的声明同步到上面这个 4 参签名后**已删除** —— 同一件事不留第二个
// 签名，否则下次改签名又会漏掉一边（"第二份实现必然漂移"）。

/** 读取 ELF 文件内容，调用 strip_elf_data 处理后写回原文件 */
bool process_elf(const fs::path& path, std::string& error_msg)
{
    std::ifstream is(path, std::ios::binary | std::ios::ate);
    if (!is) {
        error_msg = string_format("error.open_file_failed", path.string());
        return false;
    }
    std::streamsize size = is.tellg();
    // `tellg()` 失败返回 **-1**，而 `std::vector<uint8_t> buffer(size)` 会把它当
    // `size_t` 用一个天文数字去分配（bad_alloc / OOM）—— 同"输入可控尺寸进分配"那一族
    // （见本文件 `ar_member_declared_size_ok` 与 ET_REL 的聚合封顶）。这里来源是本机已存在
    // 文件的元数据、不是归档自报的不可信值，可达性低，但守卫同样只值一行。
    if (size < 0) {
        error_msg = string_format("error.file_read_failed", path.string());
        return false;
    }
    is.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    if (!is.read(reinterpret_cast<char*>(buffer.data()), size)) {
        error_msg = string_format("error.file_read_failed", path.string());
        return false;
    }
    is.close();

    std::vector<uint8_t> output_buffer;
    if (!strip_elf_data(buffer, output_buffer, error_msg, path.string())) return false;

    // **原子落位（2026-10-05 修）**：此前这里是
    // `std::ofstream os(path, std::ios::binary | std::ios::trunc);` —— **打开的那一刻原内容
    // 就没了**（`trunc` 是打开时生效的，与后面写不写得进去无关），而随后写入失败
    // （ENOSPC/EIO）只让本函数返回 false，调用方 `strip_binary` 又只把它降级成一条告警
    // ⇒ staging 里留着一个**被截断的 `.so`**，照样被打进 `.lpkg`。
    // **实测**（`StripTest.ElfOriginalStaysIntactWhenTheTmpWriteCannotEvenStart` 的修前跑）：
    // 写不进去时本函数照样返回 **true**、且原文件**已被就地重写**（内容与 strip 前不同）。
    //
    // 现在与同文件的 `process_archive` 统一：先写暂存文件 → 检查每一次写 → `fsync_and_rename`
    // 收尾（`base/fs_atomic.hpp` 明令："所有 `.tmp + fsync + rename` 的写入路径都必须走这里，
    // 不要各写一套"）。**成功 rename 之前原文件一个字节都不动** —— 这是本条修复的全部意义。
    //
    // 注：原子替换会把原 inode 换成新文件，所以**权限位必须显式带过去**（见下面 `orig_perms`），
    // 否则一个 0755 的可执行文件 strip 完就变成 0644、打进包里不可执行。`.a` 那条腿只处理
    // 静态库（惯例 0644），所以此前没暴露这个问题。
    //
    // ⚠️ **已知取舍（2026-10-05，有意保留）**：换 inode 也意味着**扩展属性不随行** ——
    // 尤其 `security.capability`（file capabilities），以及硬链接关系（新文件与别人不再共享
    // inode）。两条都不处理，理由：
    //   · 本函数只跑在**构建期的 staging 树**上（`builder.cpp` 对刚编译出来的产物调
    //     `strip_binary`），那一刻的产物不会有 `security.capability` —— 那份属性是**安装期**
    //     由配方/包内容带进去的，而 strip 根本不碰那里（安装期的可执行位/SUID 走
    //     `ARCHIVE_EXTRACT_PERM`，与这里无关）；
    //   · 本仓库的打包器**不编码硬链接**（配方一律用相对软链），`archive_strip_scan` 那条腿
    //     也早有同样取舍。
    // 真要保留 xattr，正确做法是在 rename 前把原文件的 xattr 逐个复制到暂存文件 —— 但那要
    // 处理 `security.*` 的权限与命名空间失败，而当前收益为零。**如果将来 strip 被用到构建树
    // 之外（例如对已安装的文件跑），这条必须重新评估。**
    const fs::path tmp_path = path.string() + ".lpkgtmp";
    // 暂存路径若是**符号链接**，`ofstream` 会**跟随**它把内容写进链接目标（构建树里的
    // `<文件>.lpkgtmp` 可以是上游/组件留下的任意链接，而本进程是 root）。仓库对 `.lpkgtmp`
    // 有同款守卫（`pkg/install_common.cpp` 的 `refuse_symlink_tmp_path`）—— 这里不去 include
    // pkg/ 的头（分层倒置），用 base 层的不抛谓词自己判（父链成环时也不抛）。
    if (is_symlink_no_follow(tmp_path)) {
        error_msg = string_format("error.strip_tmp_symlink", tmp_path.string());
        return false;
    }
    std::error_code perm_ec;
    const fs::perms orig_perms = fs::status(path, perm_ec).permissions();
    if (perm_ec) {
        error_msg = string_format("error.file_read_failed", path.string());
        return false;
    }

    {
        std::ofstream os(tmp_path, std::ios::binary | std::ios::trunc);
        // ⚠️ 每个失败分支**都要填 `error_msg`** —— 唯一的消费者是 `strip_binary` 里的
        // `if (!strip_file(...) && !error_msg.empty())`：空串 = 检测到了也一声不响。
        if (!os) {
            error_msg = string_format("error.open_file_failed", tmp_path.string());
            return false;
        }
        os.write(reinterpret_cast<const char*>(output_buffer.data()), output_buffer.size());
        // `ofstream` 不抛：短写 / ENOSPC / EIO 只置 badbit。不检查就会把**截断的**产物
        // rename 进正式位置。此刻原文件仍然完好（改动只落在暂存文件上），所以这里丢掉暂存、
        // 报错即可 —— 与修前的"原内容已毁"是本质区别。
        os.flush();
        if (!os) {
            os.close();
            error_msg = string_format("error.strip_write_failed", tmp_path.string());
            fs::remove(tmp_path);
            return false;
        }
    }
    // 新 inode 的权限位要显式恢复（理由见上面 `orig_perms`）。
    fs::permissions(tmp_path, orig_perms, perm_ec);
    if (perm_ec) {
        fs::remove(tmp_path);
        error_msg = string_format("error.strip_write_failed", path.string());
        return false;
    }
    try {
        fsync_and_rename(tmp_path, path);
    } catch (const std::exception&) {
        // fsync / rename 失败：原文件此刻仍是**完整的旧内容**（原子写的意义），丢掉暂存并
        // 报出来（调用方只告警，但至少点名了文件）。
        fs::remove(tmp_path);
        error_msg = string_format("error.strip_write_failed", path.string());
        return false;
    }
    return true;
}

/**
 * ar 成员**自报**尺寸的合法性判据（`archive_strip_scan` 与 `process_archive` **共用同一份**，
 * 别在两处各写一遍条件 —— 本仓库明文禁止第二份实现）。
 *
 * `archive_entry_size()` 返回 `la_int64_t`，是**归档自报**的不可信值（ar 头里的 size 字段是
 * 10 位十进制，上限约 9.3 GiB）。libarchive 对"成员头合法、成员体被截断"的归档**第一次
 * `archive_read_next_header` 就返回 OK**，于是紧随其后的 `std::vector<uint8_t> data(size)`
 * 会按那个自报值分配并逐字节清零 —— 一个 **约 72 字节**的文件就能触发 ~9.3 GiB 分配
 * （实测症状：OOM / `std::bad_alloc`）。**必须在分配之前**判掉。
 *
 * 上界取**结构性**的那一个：成员数据不可能大于整个归档文件本身 —— 所以调用方在进循环前
 * `fs::file_size` 取一次传进来，判据里不含任何魔数。**绝不能**复用
 * `constants::ARCHIVE_MEMBER_MAX_SIZE`（那是给 `metadata.json` 的 16 MiB）：静态库几百 MB
 * 完全合法，用那个常量会把正常的 `.a` 整库拒掉。
 *
 * 返回 false = 尺寸不可信（负数/未知，或大于归档文件本身）。
 */
static bool ar_member_declared_size_ok(la_int64_t declared, uint64_t archive_size)
{
    if (declared < 0) return false;
    return static_cast<uint64_t>(declared) <= archive_size;
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
    // 成员自报尺寸的封顶值（= 归档文件本身的大小）。取一次，供循环里逐成员判 —— 判据与
    // `process_archive` 共用 `ar_member_declared_size_ok`。
    std::error_code size_ec;
    const uint64_t archive_size = fs::file_size(path, size_ec);
    if (size_ec) {  // 刚打开成功却 stat 不到：保守地当失败，绝不无条件分配
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
        // 归档自报尺寸是**不可信输入**：先在分配之前判掉（见 `ar_member_declared_size_ok`）。
        const la_int64_t declared = archive_entry_size(entry);
        if (!ar_member_declared_size_ok(declared, archive_size)) {
            error_msg = get_string("error.strip_archive_broken");
            result = -1;
            break;
        }
        const size_t size = static_cast<size_t>(declared);
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
            if (strip_elf_data(data, stripped, inner, path.string()) && stripped.size() != size) {
                result = 1;
                break;  // 已足以判定"要重写"，不必读完
            }
        }
    }
    archive_read_free(a);
    return result;
}

// ---------------------------------------------------------------------------
// 归档符号索引（`/` 32 位、`/SYM64/` 64 位）：解析 → 重映射 → 回填
// ---------------------------------------------------------------------------
//
// **为什么必须重映射，而不是丢掉**（订正 2026-10-03 —— 原文写的是"重写就丢弃索引"，
// 那是错的）：重写会让 `.o` 成员变短 ⇒ 其后每个成员的偏移前移，**照抄**的旧索引全部失效,
// 链接器直接拒绝 —— 实测 `ld: error adding symbols: malformed archive`（同族症状还有
// `no more archived files`，本机 `bison` / `nspr` / `gcc` 三个包的 `.a` 曾这样被写坏）。
//
// binutils 的做法（`binutils/objcopy.c` 原文）：
//     if (strip_symbols == STRIP_ALL) obfd->has_armap = false;
//     else                            obfd->has_armap = ibfd->has_armap;  /* 交给 BFD 重新生成 */
// —— 只有 `--strip-all`（连符号一起剥）才不要索引；`--strip-debug` / `-g` /
// `--strip-unneeded` 都**保留并重建**。而 lpkg 剥的正是"调试信息那一档"（ET_REL 路径只丢
// `.debug*` / `.comment`，`.symtab` 保留），所以索引必须保留并重算。实测：`ar rcs` 出的归档
// 经 `strip --strip-debug` 后索引成员仍在、符号集不变、偏移被重算成新位置（b 2846→1358、
// c 5602→2626）；同一套判据跑 lpkg 现在的产物，1 / 8 / 30 成员三档的索引**逐条与 binutils
// 一致**，且每条都落在"真正定义该符号"的那个成员头上。
//
// ⚠️ **别把理由写成"没有索引就链不了"**（2026-10-03 我先这么写，实测**不成立**）：本机
// GNU ld 2.47 对**无索引**的归档会退化成**顺序扫描成员**，`ar rcS` 造的无索引库照样链得上
// （`nm`/`ar t` 也照常）。"保留索引"的理由是：**与 binutils 同档位的行为一致**、省掉链接器的
// 顺序扫描、不把一份正常形态的 `.a` 降级成畸形形态 —— 不是"否则链接失败"。
//
// 格式（bfd `do_slurp_coff_armap` 原话："all numeric information in a coff archive is
// always in big endian format, no matter the host or target"）：
//   `/`       `<n:4BE>` + n × `<成员头偏移:4BE>` + n 个 NUL 结尾的符号名
//   `/SYM64/` 同上，偏移是 8 字节（>4GiB 的归档，Irix 6 出身）
// 重建只需**换偏移**：符号名与顺序逐字节照旧（strip 不删 `.symtab` ⇒ 符号集不变）。
//
// **认不出的索引一律"整份归档不剥 + 告警"，绝不产出错误产物**（维护者 2026-10-03 定）：
//   · `__.SYMDEF` / `__.SYMDEF SORTED`（BSD ranlib 表）—— 数值字段是**宿主字节序**
//     （bfd 自己的注释："Probably we're using the wrong byte ordering"），跨平台没有可靠
//     判据，而 GNU ar 在 Linux/ELF 上也不产它；
//   · 畸形的 GNU 索引（计数/偏移/名字区任何一处对不上）。
// 处置都是**把整个库原样留着**（一个字节不动，索引照旧有效、链接照常）并返回失败让调用方
// 告警 —— 而不是"丢掉索引接着写"。丢索引在这里虽然还能链（链接器顺序扫描，见上），但那是
// 把一份正常形态的 `.a` 降级成畸形形态，而且我们**没法保证**重算出来的偏移是对的；
// 少剥一个库只是大几 KB，**不产出错误产物**优先（维护者 2026-10-03 定）。

constexpr std::string_view kArIndexName = "/";
constexpr std::string_view kArIndex64Name = "/SYM64/";
constexpr std::string_view kArLongNameTable = "//";
constexpr std::string_view kArBsdSymdef = "__.SYMDEF";
constexpr std::string_view kArBsdSymdefSorted = "__.SYMDEF SORTED";

/// 索引成员的处置（判据见 `process_archive` 开头）。
enum class ArIndexAction {
    Copy,     ///< 偏移仍然成立（成员没变短、也没丢 `//`）→ 原样照抄（保住索引这个优化）
    Rebuild,  ///< 偏移会变 → 按成员的新位置重算（binutils `strip --strip-debug` 的行为）
    Refuse,   ///< 认不出（BSD ranlib 表 / 畸形的 GNU 索引）→ **整份归档不剥** + 告警
};

/**
 * @brief 重写归档时**保留**（照原样再写一遍）的成员 —— 其余都被丢掉。
 *
 * 被丢的三种：`//` 长名表（libarchive 已把它的 size 归零，照抄只会写出一张空表 → 读回
 * "Invalid string table"）；索引成员（重建时由我们在最前面重写一份）；BSD ranlib 表。
 *
 * ⚠️ 丢掉任何一个都会让**其后成员整体前移** ⇒ 索引偏移必须重算 —— 这个判据与
 * `process_archive` 的 `index_invalid` 是**同一个**，别在两处各写一份条件。
 * （`Refuse` 走不到这里：那一路在写任何一个字节之前就返回了。）
 */
static bool ar_member_is_kept(std::string_view n, ArIndexAction action)
{
    if (n == kArLongNameTable) return false;
    if (n == kArIndexName || n == kArIndex64Name) return action == ArIndexAction::Copy;
    if (n == kArBsdSymdef || n == kArBsdSymdefSorted) return action == ArIndexAction::Copy;
    return true;
}

/// 原归档的**线性布局**：只按 60 字节成员头走一遍（外加把索引成员的数据读出来）。
/// 任何一步对不上就 `ok = false` —— 调用方据此退回"丢弃索引"。
struct ArRawLayout {
    bool ok = false;
    std::vector<uint64_t> header_offsets;  ///< 每个成员头的文件偏移，按文件顺序
    std::vector<std::string> names;        ///< 与 header_offsets 一一对应
    bool has_index = false;
    bool index_sym64 = false;
    std::vector<uint8_t> index_data;
};

/**
 * @brief 按 60 字节成员头线性走一遍原归档（不读内容成员，只读索引成员的数据）。
 *
 * 走完必须**恰好**落在文件末尾（`pos == file_size`）才算 `ok` —— 尾部有残留说明布局不是
 * 我们认得的形态（长名引用 `/123`、非标准填充……），此时索引偏移不可信，宁可退回丢弃。
 */
static ArRawLayout scan_ar_raw_layout(const fs::path& path, uint64_t file_size)
{
    ArRawLayout out;
    std::ifstream f(path, std::ios::binary);
    if (!f) return out;
    char magic[8];
    if (!f.read(magic, 8) || std::memcmp(magic, "!<arch>\n", 8) != 0) return out;

    uint64_t pos = 8;
    while (pos + 60 <= file_size) {
        if (f.seekg(static_cast<std::streamoff>(pos)).fail()) return out;
        char hdr[60];
        if (!f.read(hdr, 60)) return out;
        if (hdr[58] != '`' || hdr[59] != '\n') return out;  // 成员头哨兵

        std::string name(hdr, 16);
        while (!name.empty() && name.back() == ' ') name.pop_back();
        // 内容成员的名字带尾 `/`（`a.o/`）；`/`、`//`、`/SYM64/` 是特殊成员，原样留着。
        if (name.size() > 1 && name.back() == '/' && name.front() != '/') name.pop_back();
        if (!name.empty() && name.front() == '/' && name != kArIndexName &&
            name != kArIndex64Name && name != kArLongNameTable)
            return out;  // `/123`（长名表引用）之类：不是我们认得的形态

        uint64_t size = 0;
        for (int i = 48; i < 58; ++i) {
            const char c = hdr[i];
            if (c == ' ') continue;  // 数值字段右侧补空格
            if (c < '0' || c > '9') return out;
            size = size * 10 + static_cast<uint64_t>(c - '0');
        }
        if (pos + 60 + size > file_size) return out;

        out.header_offsets.push_back(pos);
        out.names.push_back(name);
        if (!out.has_index && (name == kArIndexName || name == kArIndex64Name)) {
            out.has_index = true;
            out.index_sym64 = (name == kArIndex64Name);
            out.index_data.resize(static_cast<std::size_t>(size));
            if (size > 0) {
                if (f.seekg(static_cast<std::streamoff>(pos + 60)).fail()) return out;
                if (!f.read(reinterpret_cast<char*>(out.index_data.data()),
                            static_cast<std::streamsize>(size)))
                    return out;
            }
        }
        pos += 60 + size + (size & 1);  // 成员内容按偶数字节对齐
    }
    out.ok = (pos == file_size);
    return out;
}

/// 索引里的一条记录：符号名 + 它指向的**成员头**偏移（原归档坐标）。
struct ArIndexEntry {
    std::string name;
    uint64_t offset = 0;
};

static uint32_t ar_be32(const std::vector<uint8_t>& d, std::size_t at)
{
    return (static_cast<uint32_t>(d[at]) << 24) | (static_cast<uint32_t>(d[at + 1]) << 16) |
           (static_cast<uint32_t>(d[at + 2]) << 8) | static_cast<uint32_t>(d[at + 3]);
}

static uint64_t ar_be64(const std::vector<uint8_t>& d, std::size_t at)
{
    uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) v = (v << 8) | d[at + i];
    return v;
}

/**
 * @brief 解析 GNU 符号索引体（`/` 或 `/SYM64/`）。
 *
 * **任何一处对不上都返回 false**（计数越界、偏移数组放不下、名字区走不完 n 个 NUL 结尾的
 * 串）—— 调用方据此退回"丢弃索引"，而不是猜一份可能错的偏移进去。
 */
static bool parse_gnu_ar_index(const std::vector<uint8_t>& d, bool sym64,
                               std::vector<ArIndexEntry>& out)
{
    const std::size_t width = sym64 ? 8 : 4;
    if (d.size() < 4) return false;
    const uint64_t n = ar_be32(d, 0);
    // `n == 0` 是**合法**的（`ar rcs` 对"没有任何全局符号"的库会写一张空表）：没有偏移要改，
    // 调用方按"原样照抄"处理 —— 别在这里当成畸形拒掉，那会让这种库整个剥不了。
    if (n > d.size()) return false;  // 每个条目至少 4 字节 ⇒ n 不可能超过总长
    const std::size_t names_at0 = 4 + width * static_cast<std::size_t>(n);
    if (names_at0 > d.size()) return false;

    out.clear();
    out.reserve(static_cast<std::size_t>(n));
    std::size_t names_at = names_at0;
    for (uint64_t i = 0; i < n; ++i) {
        const uint64_t off = sym64 ? ar_be64(d, 4 + 8 * static_cast<std::size_t>(i))
                                   : ar_be32(d, 4 + 4 * static_cast<std::size_t>(i));
        const std::size_t start = names_at;
        while (names_at < d.size() && d[names_at] != 0) ++names_at;
        if (names_at >= d.size()) return false;  // 没有 NUL 终止 ⇒ 名字区不是这个形态
        out.push_back(
            {std::string(reinterpret_cast<const char*>(d.data() + start), names_at - start), off});
        ++names_at;  // 跳过 NUL
    }
    return true;
}

/// 序列化偏移数组（回填用）：n 个 4/8 字节大端。
static std::vector<uint8_t> serialize_ar_offsets(bool sym64, const std::vector<uint64_t>& offsets)
{
    std::vector<uint8_t> d;
    const std::size_t width = sym64 ? 8 : 4;
    d.reserve(offsets.size() * width);
    for (const uint64_t v : offsets) {
        for (std::size_t i = 0; i < width; ++i) {
            d.push_back(static_cast<uint8_t>((v >> (8 * (width - 1 - i))) & 0xff));
        }
    }
    return d;
}

/// 序列化整份索引体。占位那份传全 0 的 offsets —— 大小与重建后**逐字节相同**
/// （计数与符号名都不变），所以回填只是原地改写那几个字节，不动任何成员的位置。
static std::vector<uint8_t> build_gnu_ar_index(bool sym64, const std::vector<ArIndexEntry>& entries,
                                               const std::vector<uint64_t>& offsets)
{
    // 先算总长、一次性分配、按偏移写入 —— 与"反复 push_back/insert 增长"逐字节等价，
    // 但**不触发 gcc 13 的 `-Wstringop-overflow` 误报**：`_GLIBCXX_ASSERTIONS`（build.conf
    // 默认带）会把 vector 的 insert/push_back 内联成检查版，gcc 13.2.1 据此算出"往大小为 0
    // 的区域写 2..SIZE_MAX 字节"的伪告警（实测：同一份源码 gcc 13.2.1 报错、gcc 16.2.0 干净；
    // 单独 `-D_GLIBCXX_ASSERTIONS` 即可触发，`-Werror` 下变成硬失败）。
    const std::vector<uint8_t> offs = serialize_ar_offsets(sym64, offsets);
    const uint32_t n = static_cast<uint32_t>(entries.size());
    std::size_t total = 4 + offs.size();
    for (const auto& e : entries) total += e.name.size() + 1;

    std::vector<uint8_t> d(total);
    std::size_t p = 0;
    d[p++] = static_cast<uint8_t>((n >> 24) & 0xff);
    d[p++] = static_cast<uint8_t>((n >> 16) & 0xff);
    d[p++] = static_cast<uint8_t>((n >> 8) & 0xff);
    d[p++] = static_cast<uint8_t>(n & 0xff);
    std::copy(offs.begin(), offs.end(), d.data() + p);
    p += offs.size();
    for (const auto& e : entries) {
        std::copy(e.name.begin(), e.name.end(), d.data() + p);
        p += e.name.size();
        d[p++] = 0;
    }
    return d;
}

/**
 * 处理静态库（ar 归档文件）：遍历其中的 .o 文件，对每个进行 ELF strip，
 * 重新打包为新的归档文件
 *
 * ⚠️ **重写会改变成员偏移 ⇒ 归档符号索引必须重算**（`/` 32 位、`/SYM64/` 64 位 GNU）。
 * 索引存的是一串"符号 → 成员头偏移"，偏移一旦错位链接器就硬失败：
 * `ld: error adding symbols: malformed archive`（本机 `bison` / `nspr` / `gcc`
 * 三个包的 `.a` 曾被写坏）。做法与 binutils `strip --strip-debug` 一致：
 * **保留索引成员、把偏移重映射到新位置**（符号名与顺序照旧 —— strip 不删 `.symtab`）。
 * 见上面那段"归档符号索引"的说明；**别退回"干脆丢掉索引"**（那是非正常形态，也偏离 binutils
 * 同档位的行为）。
 *
 * 会造成偏移改变的有两种，**判据必须两个都看**（2026-10-02 只看了前者）：
 *   ① 有 `.o` 成员在 strip 后**变短**（预扫的 `scan == 1`）；
 *   ② 归档里有 `//` 长名表 —— 它恒定被丢弃（见下），其后成员整体前移。
 * ② 正是"外来/构造归档"能钻的空子：一张没人引用的 `//` 表配上有效的 `/` 索引，成员没变短
 * 也照样让索引失效（子审计逐字节复刻验证过 stale 0 → 2）。因此 `//` 与索引**同一判据**。
 * 两种情形都走"重映射"这条路；只有**认不出索引**（畸形、BSD ranlib 表）才退回"整份归档
 * 不剥 + 告警"（`ArIndexAction::Refuse`）—— 见上面那段说明，**不是**"丢掉索引接着写"。
 */
bool process_archive(const fs::path& path, std::string& error_msg)
{
    // 先扫一遍：长名成员 → 放弃（写不出来）；真失败 → `error_msg` 已填、由调用方告警；
    // 有无 `//` 长名表 → 由 `has_long_name_table` 带出（它也决定索引能不能重映射）。
    bool has_long_name_table = false;
    const int scan = archive_strip_scan(path, error_msg, has_long_name_table);
    if (scan < 0) return false;
    const bool index_invalid = (scan == 1) || has_long_name_table;

    // 成员自报尺寸的封顶值（= 归档文件本身的大小）。与 `archive_strip_scan` 用**同一个**
    // `ar_member_declared_size_ok`（别在这里另写一份魔数/条件）。
    std::error_code size_ec;
    const uint64_t archive_size = fs::file_size(path, size_ec);
    if (size_ec) {
        error_msg = get_string("error.strip_archive_open");
        return false;
    }

    // 索引的处置（见上面"归档符号索引"那段）。映射必须在**写之前**算出来：索引成员占着
    // 输出归档的**第一个**位置（bfd 只看首成员的名字决定读哪张索引表），收工再反悔就得把
    // 整份归档挪位。原归档的线性布局只读 60 字节头，很便宜。
    ArIndexAction index_action = ArIndexAction::Copy;
    std::vector<ArIndexEntry> idx_entries;
    std::vector<std::size_t> idx_rank;  // 每个索引条目 → 该成员在"保留序列"里的序号
    ArRawLayout raw;
    std::size_t kept_count = 0;
    if (index_invalid) {
        raw = scan_ar_raw_layout(path, fs::file_size(path));
        if (!raw.ok) {
            // 连布局都认不出来 ⇒ 我们甚至不确定它有没有索引，那就一个字节都别动它
            // （同下面那条：宁可少剥一个库，也不产出"索引指向错位置"的 `.a`）。
            error_msg = get_string("error.strip_archive_broken");
            return false;
        }
        bool bsd_table = false;
        for (const auto& n : raw.names) {
            if (n == kArBsdSymdef || n == kArBsdSymdefSorted) bsd_table = true;
        }
        if (bsd_table || raw.has_index) {          // 有索引才谈得上"重算"或"拒绝"
            index_action = ArIndexAction::Refuse;  // 先按最坏打算
            std::vector<ArIndexEntry> parsed;
            if (!bsd_table && parse_gnu_ar_index(raw.index_data, raw.index_sym64, parsed)) {
                if (parsed.empty()) {
                    index_action = ArIndexAction::Copy;  // 空索引：没有偏移要改，照抄
                } else {
                    // "保留序列"的前缀计数：`kept_prefix[j]` = 前 j 个成员里有几个被保留
                    std::vector<std::size_t> kept_prefix(raw.names.size() + 1, 0);
                    for (std::size_t j = 0; j < raw.names.size(); ++j) {
                        kept_prefix[j + 1] =
                            kept_prefix[j] +
                            (ar_member_is_kept(raw.names[j], ArIndexAction::Rebuild) ? 1 : 0);
                    }
                    kept_count = kept_prefix.back();

                    // 逐条查指向：偏移必须**正好**落在某个成员头上、且那个成员会被保留。
                    // 任一条不满足 ⇒ 整份索引不用（Refuse）—— 绝不写一份"可能对"的偏移。
                    bool mapped = true;
                    idx_rank.reserve(parsed.size());
                    for (const auto& e : parsed) {
                        const auto it = std::find(raw.header_offsets.begin(),
                                                  raw.header_offsets.end(), e.offset);
                        if (it == raw.header_offsets.end()) {
                            mapped = false;
                            break;
                        }
                        const std::size_t j =
                            static_cast<std::size_t>(it - raw.header_offsets.begin());
                        if (!ar_member_is_kept(raw.names[j], ArIndexAction::Rebuild)) {
                            mapped = false;
                            break;
                        }
                        idx_rank.push_back(kept_prefix[j]);
                    }
                    if (mapped) {
                        index_action = ArIndexAction::Rebuild;
                        idx_entries = std::move(parsed);
                    }
                }
            }
            if (index_action == ArIndexAction::Refuse) {
                // **不产出错误产物**：这份索引我们改不动，那就整个库都别剥 —— 原文件一个
                // 字节不动、索引照旧有效、链接照常。warning 由调用方 `strip_binary` 打
                // （`error_msg` 非空即告警，且点名文件）。
                error_msg = string_format(
                    "error.strip_archive_index_unsupported",
                    bsd_table ? std::string(kArBsdSymdef)
                              : std::string(raw.index_sym64 ? kArIndex64Name : kArIndexName));
                return false;
            }
        }
    }
    const bool rebuild_index = (index_action == ArIndexAction::Rebuild);

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
    // **暂存路径不得是符号链接（2026-10-05 修）**：`archive_write_open_filename` 走的是
    // `open(O_CREAT|O_WRONLY|O_TRUNC)`，**跟随**末段链接 —— 构建树里若有一个
    // `usr/lib/libfoo.a.tmp` 指向别处（上游产物、配方留下的链接、上一次构建的残留都可能），
    // 本进程（root）会把整份新归档写进那个链接的**目标**；随后收尾的
    // `safe_rename(temp_path, path)` 又把**链接本身**改名成 `.a`。实测（修前跑
    // `StripTest.ArchiveTmpPathSymlinkIsRefusedNotFollowed`）：链接目标里赫然是
    // `!<arch>\n…` 整份归档，而 `fs::is_symlink(<lib>.a)` 为真。仓库对
    // `.lpkgtmp` 有同款守卫（`pkg/install_common.cpp` 的 `refuse_symlink_tmp_path`）——
    // 这里不去 include pkg/ 的头（分层倒置），用 base 层的不抛谓词自己判（中间段成环也不抛）。
    if (is_symlink_no_follow(temp_path)) {
        archive_read_free(a);
        archive_write_free(out);
        error_msg = string_format("error.strip_tmp_symlink", temp_path.string());
        return false;
    }
    if (archive_write_open_filename(out, temp_path.c_str()) != ARCHIVE_OK) {
        archive_read_free(a);
        archive_write_free(out);
        // 此前这里**不填 `error_msg`** ⇒ 调用方（`strip_binary`）的告警判据
        // `!strip_file(...) && !error_msg.empty()` 不成立 ⇒ 打不开暂存文件时一声不响。
        // 与同族其它分支（`error.strip_archive_open` 等）和本文件"失败必点名"的纪律对齐。
        error_msg = string_format("error.open_file_failed", temp_path.string());
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

    // 重建索引时，索引成员必须**第一个**写（bfd 的 `bfd_slurp_armap` 只看归档首成员的名字
    // 来决定读哪张索引表），可它的偏移要等成员全写完才晓得 —— 所以先写一份**大小逐字节
    // 相同**的占位（符号名与计数照旧、偏移先给 0），收尾再按 GNU ar 自己的做法**原地回填**
    // 那几个字节：只改索引成员内部的数据，不动任何成员的位置。
    std::vector<uint64_t> new_offsets;  // 与"保留的成员"一一对应，按写入顺序
    uint64_t pos = 8;                   // 归档 magic 之后
    if (rebuild_index) {
        const std::vector<uint8_t> placeholder = build_gnu_ar_index(
            raw.index_sym64, idx_entries, std::vector<uint64_t>(idx_entries.size(), 0));
        struct archive_entry* ie = archive_entry_new();
        archive_entry_set_pathname(ie, raw.index_sym64 ? "/SYM64/" : "/");
        archive_entry_set_size(ie, static_cast<la_int64_t>(placeholder.size()));
        archive_entry_set_filetype(ie, AE_IFREG);
        archive_entry_set_perm(ie, 0644);
        const int ih = archive_write_header(out, ie);
        const ssize_t iw =
            ih == ARCHIVE_OK ? archive_write_data(out, placeholder.data(), placeholder.size()) : -1;
        archive_entry_free(ie);
        if (ih != ARCHIVE_OK || iw < 0 || static_cast<std::size_t>(iw) != placeholder.size())
            return bail();
        pos += 60 + placeholder.size() + (placeholder.size() & 1);
    }

    while (true) {
        const int r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF) break;
        if (r < ARCHIVE_OK) return bail();
        const char* name = archive_entry_pathname(entry);
        const std::string_view nm = name ? std::string_view(name) : std::string_view();
        // 丢掉的成员（见 `ar_member_is_kept`）：`//` 长名表 —— libarchive 已把它的 size
        // **归零**，照抄只会写出一张空表（读回时 "Invalid string table" → FATAL）；
        // 索引成员 —— 重建时由我们写在最前面；BSD ranlib 表。**判据只有那一处**，别在这里
        // 另写一份条件（漏一个就会让索引与成员对不上）。
        // ⚠️ 丢任何一个都会让**其后成员整体前移** ⇒ 索引偏移必须重算（`index_invalid`）。
        if (!ar_member_is_kept(nm, index_action)) {
            archive_read_data_skip(a);
            continue;
        }
        // 同上：归档自报尺寸不可信，先在分配之前判掉（共用的 `ar_member_declared_size_ok`）。
        // 走到这里通常已由 `archive_strip_scan` / `scan_ar_raw_layout` 挡过一遍，这里是**纵深
        // 防御** —— 分配点只该信结构性上界，不该依赖上游判据的完备性。
        const la_int64_t declared = archive_entry_size(entry);
        if (!ar_member_declared_size_ok(declared, archive_size)) return bail();
        const size_t size = static_cast<size_t>(declared);
        std::vector<uint8_t> data(size);

        const ssize_t bytes_read = archive_read_data(a, data.data(), size);
        if (bytes_read < 0 || static_cast<size_t>(bytes_read) != size) return bail();

        // 这个成员头会落在输出归档的哪个偏移上（收尾回填索引要用的值）。
        const uint64_t member_at = pos;

        // 对 .o 目标文件进行 strip 处理
        if (nm.ends_with(".o")) {
            std::vector<uint8_t> stripped_data;
            std::string inner_error_msg;
            if (strip_elf_data(data, stripped_data, inner_error_msg, path.string())) {
                archive_entry_set_size(entry, stripped_data.size());
                if (archive_write_header(out, entry) != ARCHIVE_OK) return bail();
                const ssize_t written =
                    archive_write_data(out, stripped_data.data(), stripped_data.size());
                if (written < 0 || static_cast<size_t>(written) != stripped_data.size())
                    return bail();
                if (rebuild_index) {
                    new_offsets.push_back(member_at);
                    pos += 60 + stripped_data.size() + (stripped_data.size() & 1);
                }
                continue;
            }
        }

        if (archive_write_header(out, entry) != ARCHIVE_OK) return bail();
        const ssize_t written = archive_write_data(out, data.data(), size);
        if (written < 0 || static_cast<size_t>(written) != size) return bail();
        if (rebuild_index) {
            new_offsets.push_back(member_at);
            pos += 60 + size + (size & 1);
        }
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

    if (rebuild_index) {
        // **布局自检**：我们逐成员累加出来的位置必须与实际写出的文件长度**对得上**。
        // 对不上说明"60 字节成员头 + 偶数字节对齐"这条前提在这份归档上不成立，回填的偏移
        // 就不可信 —— 此时宁可整库不剥（原文件一个字节没动），也绝不写出一份错的索引。
        std::error_code sz_ec;
        const uint64_t actual = fs::file_size(temp_path, sz_ec);
        if (sz_ec || actual != pos || new_offsets.size() != kept_count) {
            error_msg = get_string("error.strip_archive_broken");
            fs::remove(temp_path);
            return false;
        }

        // 回填：新偏移 = 该成员在"保留序列"里的序号查出来的写入位置。
        // 索引成员的数据从文件偏移 `8(magic) + 60(成员头)` 开始，偏移数组在计数（4 字节）之后。
        std::vector<uint64_t> mapped(idx_entries.size(), 0);
        bool mapped_ok = true;
        for (std::size_t i = 0; i < idx_entries.size(); ++i) {
            if (idx_rank[i] >= new_offsets.size()) {
                mapped_ok = false;
                break;
            }
            mapped[i] = new_offsets[idx_rank[i]];
            if (!raw.index_sym64 && mapped[i] > 0xffffffffull) {  // 32 位索引装不下
                mapped_ok = false;
                break;
            }
        }
        const std::vector<uint8_t> off_bytes =
            mapped_ok ? serialize_ar_offsets(raw.index_sym64, mapped) : std::vector<uint8_t>();
        Fd fd(::open(temp_path.c_str(), O_WRONLY));
        if (!mapped_ok || !fd.ok() ||
            ::pwrite(fd.get(), off_bytes.data(), off_bytes.size(), 8 + 60 + 4) !=
                static_cast<ssize_t>(off_bytes.size())) {
            error_msg = get_string("error.strip_archive_broken");
            fs::remove(temp_path);
            return false;
        }
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
