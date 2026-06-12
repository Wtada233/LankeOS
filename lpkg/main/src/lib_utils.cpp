#include "lib_utils.hpp"
#include <libelf.h>
#include <gelf.h>
#include <fcntl.h>
#include <unistd.h>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

std::string get_elf_soname(const fs::path& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return "";

    Elf* elf = elf_begin(fd, ELF_C_READ, nullptr);
    if (!elf) { close(fd); return ""; }

    std::string soname = "";
    Elf_Scn* scn = nullptr;
    while ((scn = elf_nextscn(elf, scn)) != nullptr) {
        GElf_Shdr shdr;
        if (gelf_getshdr(scn, &shdr) == nullptr) continue;
        
        if (shdr.sh_type == SHT_DYNAMIC) {
            Elf_Data* data = elf_getdata(scn, nullptr);
            if (data) {
                size_t ext_count = shdr.sh_size / shdr.sh_entsize;
                for (size_t i = 0; i < ext_count; ++i) {
                    GElf_Dyn dyn;
                    gelf_getdyn(data, i, &dyn);
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

void apply_soname_links(const fs::path& lib_dir) {
    if (!fs::exists(lib_dir) || !fs::is_directory(lib_dir)) return;

    for (const auto& entry : fs::directory_iterator(lib_dir)) {
        if (!entry.is_regular_file()) continue;

        std::string soname = get_elf_soname(entry.path());
        if (!soname.empty()) {
            fs::path link_path = lib_dir / soname;
            // Create symlink only if it doesn't exist to avoid conflict with existing proper files
            if (!fs::exists(link_path) && !fs::is_symlink(link_path)) {
                try {
                    fs::create_symlink(entry.path().filename(), link_path);
                } catch (...) {
                    // Ignore errors during symlink creation
                }
            }
        }
    }
}
