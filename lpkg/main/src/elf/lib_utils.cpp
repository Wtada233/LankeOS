#include "lib_utils.hpp"

#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <unistd.h>

#include <filesystem>

#include "base/utils.hpp"
#include "i18n/localization.hpp"

namespace fs = std::filesystem;

/**
 * 从 ELF 共享库文件中读取 DT_SONAME 字段
 * 遍历 .dynamic 节区查找 DT_SONAME 条目，返回其字符串值
 * 如果文件不是 ELF 格式或没有 SONAME，返回空字符串
 */
std::string get_elf_soname(const fs::path& path)
{
    // libelf 要求进程先调用 elf_version(EV_CURRENT)，否则 elf_begin 恒返回 NULL。
    // 此前只有 strip.cpp 调过它 → 安装期的 ldconfig 触发器（apply_soname_links）
    // 在"本进程没 strip 过"时**一个 SONAME 链接都不建，且完全静默**（TODO.md F2）。
    // 这里做一次性的惰性初始化，消除调用顺序依赖。
    static const bool elf_ready = (elf_version(EV_CURRENT) != EV_NONE);
    if (!elf_ready) return "";

    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return "";

    Elf* elf = elf_begin(fd, ELF_C_READ, nullptr);
    if (!elf) {
        close(fd);
        return "";
    }

    std::string soname = "";
    Elf_Scn* scn = nullptr;
    while ((scn = elf_nextscn(elf, scn)) != nullptr) {
        GElf_Shdr shdr;
        if (gelf_getshdr(scn, &shdr) == nullptr) continue;

        if (shdr.sh_type == SHT_DYNAMIC) {
            Elf_Data* data = elf_getdata(scn, nullptr);
            if (data) {
                const size_t ext_count = elf_section_entry_count(shdr.sh_size, shdr.sh_entsize);
                for (size_t i = 0; i < ext_count; ++i) {
                    GElf_Dyn dyn;
                    if (gelf_getdyn(data, i, &dyn) == nullptr) continue;  // 不读未初始化的 dyn
                    if (dyn.d_tag == DT_SONAME) {
                        soname = elf_strptr(elf, shdr.sh_link, dyn.d_un.d_val);
                        break;
                    }
                }
            }
        }
        if (!soname.empty()) break;
    }

    elf_end(elf);
    close(fd);
    return soname;
}

/**
 * 扫描指定库目录中的 ELF 共享库文件，为每个文件创建 SONAME 符号链接
 * 仅当目标链接不存在时才创建，避免覆盖用户已存在的文件
 */
void apply_soname_links(const fs::path& lib_dir)
{
    if (!fs::exists(lib_dir) || !fs::is_directory(lib_dir)) return;

    for (const auto& entry : fs::directory_iterator(lib_dir)) {
        if (!entry.is_regular_file()) continue;

        std::string soname = get_elf_soname(entry.path());
        if (!soname.empty()) {
            // SONAME 取自被扫描的库文件（不可信输入）：绝对路径或 `..` 会让
            // `lib_dir / soname` 逃出 lib_dir（fs::path 语义下绝对右值丢弃左值），
            // 从而以 root 在任意位置建符号链接（TODO.md X3）。只接受落在 lib_dir 内的。
            const fs::path lib_dir_n = lib_dir.lexically_normal();
            const fs::path link_path = (lib_dir_n / soname).lexically_normal();
            if (!path_within(link_path, lib_dir_n)) {
                log_warning(string_format("warning.soname_escapes_lib_dir", soname,
                                          entry.path().filename().string(), lib_dir.string()));
                continue;
            }
            // 仅在链接不存在时创建，避免与已存在的文件冲突
            if (!fs::exists(link_path) && !fs::is_symlink(link_path)) {
                try {
                    fs::create_symlink(entry.path().filename(), link_path);
                } catch (const std::exception& e) {
                    log_warning(string_format("warning.soname_link_failed",
                                              entry.path().filename().string(), link_path.string(),
                                              e.what()));
                }
            }
        }
    }
}
