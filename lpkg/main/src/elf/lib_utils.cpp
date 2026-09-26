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
                        // libelf 在 offset 越出 .dynstr、sh_link 不是 SHT_STRTAB、或该节不存在时
                        // **返回 NULL**。直接赋给 std::string 是 UB（libstdc++ 下等价
                        // strlen(nullptr) → SIGSEGV），而本函数对 usr/lib 下**每个** ELF 调用，
                        // 输入来自不可信包 → 一个畸形 .so 就能让安装后的 ldconfig 触发器
                        // （root）或构建进程在事务中途段错误。strip.cpp 对同一 API 是判空的
                        // （`name_ptr ? name_ptr : ""`），这里与之对齐。
                        const char* s = elf_strptr(elf, shdr.sh_link, dyn.d_un.d_val);
                        soname = s ? s : "";
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
    // **判定**一律不抛（见 base/utils.hpp 的谓词族说明）：本函数的**两个调用点传的都是包
    // 内容** —— `trigger.cpp` 传目标 root 的 `<root>/usr/lib`，`builder.cpp` 传构建 staging 的
    // `<staging>/usr/lib`。盘上（或 staging 里）有**符号链接环**时，抛出型判定会让任何提供
    // `usr/lib/**.so*` 的包在**提交之后**的触发器里炸：包已落地、DB 已提交，命令却报失败。
    // 措辞订正 2026-09-26：原文写"判定一律不抛"，但下面 `fs::directory_iterator(lib_dir)`
    // 是**抛型**构造（没有 ec 重载）—— 它靠**紧邻的上面这一行**守卫：`is_directory_follow`
    // 解不开就返回 false 提前退出，所以迭代只在 lib_dir 确实解析到目录时才进入。
    // 这是"判定不抛 + 迭代有守卫"，不是"整段不可能抛"。
    if (!is_directory_follow(lib_dir)) return;

    for (const auto& entry : fs::directory_iterator(lib_dir)) {
        // **保持"跟随"语义、只把"抛"换成"判否"**（不要换成 lstat 语义）：`libfoo.so ->
        // libfoo.so.1.2.3` 这类链接本就该被本函数处理（修正指错的 SONAME 链接正是它的职责），
        // 换成 lstat 会把这些条目一并跳过 = 静默丢掉一段行为。
        // `directory_entry::is_regular_file()` 走 `status()`，对环抛 ELOOP（实测 code=40）。
        std::error_code entry_ec;
        if (!fs::is_regular_file(entry.path(), entry_ec) || entry_ec) continue;

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
            // 不抛谓词（2026-09-26 修）：本函数的两个调用点传进来的都是**包内容**
            // （trigger.cpp 的目标 root、builder.cpp 的 staging），而包的 `usr/lib` 下完全
            // 可以有自环/两跳环 —— `fs::is_symlink` 对**中间段**成环抛 ELOOP，那一刻包已经
            // 落地、DB 已经提交，命令却报失败（本文件上方那句"判定一律不抛"的横幅此前并不
            // 成立，见 CLAUDE.md 的记账）。
            if (is_symlink_no_follow(link_path)) {
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
            } else if (exists_follow(link_path)) {
                // 实体文件/目录：包自己的产物，绝不覆盖（原行为）。不抛：soname 取自 ELF
                // （不可信输入），盘上那个名字完全可能被一个环占着 —— 那也只是"不覆盖"。
                continue;
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
