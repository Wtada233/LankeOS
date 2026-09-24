#include "lib_utils.hpp"

#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <unistd.h>

#include <cctype>
#include <filesystem>
#include <string_view>

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
 * 链接名是否形如**我们生成的** SONAME 链接：`<name>.so.<数字…>`。
 *
 * 用来把"本函数自己建的链接"与用户/其他工具建的链接区分开（清理那一遍只碰前者）。
 */
static bool looks_like_soname_link(std::string_view name)
{
    const size_t pos = name.rfind(".so.");
    if (pos == std::string_view::npos) return false;
    const std::string_view ver = name.substr(pos + 4);
    return !ver.empty() && std::isdigit(static_cast<unsigned char>(ver.front())) != 0;
}

/**
 * 链接是否已经**正确**：目标存在，且目标的 DT_SONAME 就等于链接名。
 *
 * 这是"存在即跳过"的替代判据（ldconfig 的语义）。注意别退回
 * `fs::exists(link) || fs::is_symlink(link)`：`fs::exists` 会**跟随**符号链接，悬空链接
 * 的 exists() 是 false，而 is_symlink() 是 true —— 两个条件一个不成立、一个成立，
 * 于是悬空链接既不重建也不清理，可以永久留在 /usr/lib（依赖它的二进制报
 * cannot open shared object file）。
 */
static bool link_already_correct(const fs::path& link_path, const std::string& soname)
{
    std::error_code ec;
    const fs::path target = fs::read_symlink(link_path, ec);
    if (ec) return false;
    // fs::path 的 `/` 语义：右值若为绝对路径则丢弃左值 —— 相对/绝对目标都能正确解析
    const fs::path resolved = link_path.parent_path() / target;
    if (!fs::exists(resolved, ec) || ec) return false;
    return get_elf_soname(resolved) == soname;
}

/**
 * 扫描指定库目录中的 ELF 共享库文件，为每个文件创建/修正 SONAME 符号链接。
 *
 * 判据是"链接**正确**"而不是"链接**存在**"（= ldconfig 的行为）：链接要指向当前目录里
 * 存在、且其 DT_SONAME 就等于链接名的库；指错（悬空 / 指向别的库）就删掉重建。
 * 此外清理掉"无人再提供该 SONAME"的悬空链接 —— 删库文件的那一侧只有它能收尾。
 *
 * **实体文件一律不动**：包自己就提供 `libfoo.so.1` 这个真文件时，把它换成链接等于
 * 覆盖包产物（旧行为如此，保持不变）。
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
            if (fs::is_symlink(link_path)) {
                // 链接已存在：正确就不动（避免无谓的 inode/时间戳抖动）；
                // 否则删掉重建 —— 升级到"同 SONAME、不同文件名"后指向已删旧文件的悬空
                // 链接在这一步被纠正（旧判据"存在即跳过"会让它永久悬空）
                if (link_already_correct(link_path, soname)) continue;
                std::error_code rm_ec;
                fs::remove(link_path, rm_ec);
                if (rm_ec) {
                    log_warning(string_format("warning.soname_link_failed",
                                              entry.path().filename().string(), link_path.string(),
                                              rm_ec.message()));
                    continue;
                }
            } else if (fs::exists(link_path)) {
                continue;  // 实体文件/目录：包自己的产物，绝不覆盖（原行为）
            }
            try {
                fs::create_symlink(entry.path().filename(), link_path);
            } catch (const std::exception& e) {
                log_warning(string_format("warning.soname_link_failed",
                                          entry.path().filename().string(), link_path.string(),
                                          e.what()));
            }
        }
    }

    // 第二遍：清理**悬空**的 SONAME 链接（它对应的库已经不在这两个目录里了）。
    // 只认"我们生成的形状"：目标是不带目录的裸文件名（create_symlink 的产物就是同目录
    // 裸名），链接名形如 <name>.so.<数字>；指向绝对路径/其他目录的悬空链接一律不碰
    // （multiarch 之类布局里那可能是刻意的）。
    for (const auto& entry : fs::directory_iterator(lib_dir)) {
        if (!entry.is_symlink()) continue;
        if (!looks_like_soname_link(entry.path().filename().string())) continue;
        std::error_code ec;
        const fs::path target = fs::read_symlink(entry.path(), ec);
        if (ec || target.has_parent_path()) continue;
        if (fs::exists(entry.path(), ec)) continue;  // 能解析 → 不是悬空
        fs::remove(entry.path(), ec);
        if (ec) {
            log_warning(string_format("warning.soname_link_failed", target.string(),
                                      entry.path().string(), ec.message()));
        }
    }
}
