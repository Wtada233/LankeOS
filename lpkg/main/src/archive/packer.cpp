#include "packer.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <sys/xattr.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#include "archive/archive.hpp"
#include "base/constants.hpp"
#include "base/exception.hpp"
#include "base/utils.hpp"
#include "config/config.hpp"
#include "crypto/hash.hpp"
#include "i18n/localization.hpp"
#include "nlohmann/json.hpp"

namespace fs = std::filesystem;

using json = nlohmann::json;

namespace
{
/** 将磁盘上的单个文件或目录添加到归档中，保留文件元数据和符号链接信息 */
void add_to_archive(struct archive* a, const fs::path& path, const std::string& entry_name,
                    const std::string& container_path)
{
    // 成员名守卫（与解压侧**共用**同一份判据）。这是名字进入归档的**唯一**漏斗，
    // 所以判在这里、而不是在各调用点：不判的话 `lpkg pack` 能产出一个自己的 extractor
    // 会整包拒收的包 —— 缺陷要拖到下游甚至用户手上才暴露，而该出声的地方是产生它的那一刻。
    // 常量名（content / hooks / metadata.json）天然合法，走同一条路只是纵深防御。
    if (const std::string why = member_name_rejection_message(entry_name, container_path);
        !why.empty())
        throw LpkgException(why);

    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return;

    // 用 libarchive 的 **disk reader** 填 entry：它会一并带上 xattr **和真正的 ACL 记录**
    // （PAX `SCHILY.acl.access` 等）。此前手工 copy_stat + 把 ACL 当裸 xattr 写
    // （`SCHILY.xattr.system.posix_acl_access`）是**不生效**的——libarchive 不会把裸 xattr
    // 当 ACL 应用（真 lpkg pack/install 后 `getfacl` 命名条目消失；bsdtar 用
    // `--xattrs --acls` 解也照样丢，证明问题在打包侧而非 lpkg 的解压/拷贝）。
    struct archive* disk = archive_read_disk_new();
    archive_read_disk_set_symlink_physical(disk);  // 不跟随符号链接（与 lstat 语义一致）
    struct archive_entry* entry = archive_entry_new();
    archive_entry_set_pathname(entry, path.c_str());  // disk reader 按此路径读元数据
    if (archive_read_disk_entry_from_file(disk, entry, -1, &st) < ARCHIVE_WARN) {
        archive_entry_free(entry);
        archive_read_free(disk);
        return;
    }
    archive_entry_set_pathname(entry, entry_name.c_str());  // 归档内改回目标名
    archive_read_free(disk);

    if (S_ISLNK(st.st_mode)) {
        char link_target[PATH_MAX];
        ssize_t len = readlink(path.c_str(), link_target, sizeof(link_target) - 1);
        // readlink 不保证 NUL 结尾：len == 缓冲上限即"被截断"。静默打包一个**错误**的链接
        // 目标不可接受 —— 拒绝（与 localization.cpp 处理截断的判据一致）。
        if (len == static_cast<ssize_t>(sizeof(link_target) - 1)) {
            archive_entry_free(entry);
            throw LpkgException(string_format("error.readlink_target_too_long", path.string()));
        }
        if (len != -1) {
            link_target[len] = '\0';
            archive_entry_set_symlink(entry, link_target);
        }
    }

    if (archive_write_header(a, entry) != ARCHIVE_OK) {
        const char* err = archive_error_string(a);  // 可能为 NULL：先取再兜底
        archive_entry_free(entry);
        throw LpkgException(string_format("error.archive_write_header_failed",
                                          err ? err : get_string("error.unknown")));
    }

    if (S_ISREG(st.st_mode)) {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) {  // lstat 成功但 open 失败（权限/竞态）→ 绝不能写出零填充文件
            archive_entry_free(entry);
            throw LpkgException(string_format("error.archive_open_failed", path.string()));
        }
        std::array<char, constants::PACK_IO_BUFFER_SIZE> buffer{};
        while (f.read(buffer.data(), buffer.size()) || f.gcount() > 0) {
            if (archive_write_data(a, buffer.data(), f.gcount()) < 0) {
                const char* err = archive_error_string(a);  // 可能为 NULL：先取再兜底
                archive_entry_free(entry);
                throw LpkgException(string_format("error.archive_write_data_failed",
                                                  err ? err : get_string("error.unknown")));
            }
        }
        // 读到一半的 I/O 错误会置 badbit（上面的循环就此结束）—— 不检查就会**静默**写出一份
        // 缺数据的包，还照常算 SHA256 报成功。判据与 crypto/hash.cpp 里那道一致。
        if (f.bad()) {
            archive_entry_free(entry);
            throw LpkgException(string_format("error.archive_read_failed", path.string()));
        }
    }

    archive_entry_free(entry);
}

/** 递归遍历目录并将所有文件和子目录添加到归档中 */
void add_dir_recursive(struct archive* a, const fs::path& dir, const std::string& archive_prefix,
                       const std::string& container_path)
{
    // **必须排序**：`recursive_directory_iterator` 的顺序由 readdir 决定（文件系统相关），
    // 而归档成员顺序直接决定 `.lpkg` 的哈希 —— 不排序则同一棵内容树在不同机器/文件系统上
    // 打出不同的 SHA256，破坏可复现构建（install_common.cpp 的对应扫描就显式排序）。
    std::vector<fs::path> paths;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) paths.push_back(entry.path());
    std::sort(paths.begin(), paths.end());  // 字典序天然把父目录排在子项之前
    for (const auto& p : paths) {
        fs::path rel = p.lexically_relative(dir);
        std::string entry_name = archive_prefix + "/" + rel.string();
        add_to_archive(a, p, entry_name, container_path);
    }
}
}  // namespace

/**
 * 打包完整的 lpkg 包文件
 * 步骤包括：
 *   1. 生成 metadata.json（包含名称、版本、依赖、提供和 man 信息）
 *   2. 添加 hooks 目录
 *   3. 添加内容文件（root 目录映射为 content/）
 * 最后计算并输出 SHA256 哈希值
 */
void pack_package(const std::string& output_filename, const std::string& source_dir,
                  const std::string& pkg_name, const std::string& pkg_version,
                  const std::vector<std::string>& deps, const std::vector<std::string>& provides,
                  const std::vector<std::string>& provides_soname, const std::string& man_content,
                  const std::vector<std::string>& needed_so)
{
    fs::path base_dir = source_dir;
    fs::path root_dir = base_dir / constants::DIR_CONTENT;
    fs::path hooks_dir = base_dir / constants::DIR_HOOKS;

    if (!fs::exists(root_dir)) {
        throw LpkgException(get_string("error.pack_root_not_found") + " " + root_dir.string());
    }

    struct archive* a = archive_write_new();
    archive_write_add_filter_zstd(a);
    archive_write_set_format_pax_restricted(a);

    if (archive_write_open_filename(a, output_filename.c_str()) != ARCHIVE_OK) {
        const char* err = archive_error_string(a);  // 可能为 NULL：先取再兜底
        throw LpkgException(
            string_format("error.archive_open_failed", err ? err : get_string("error.unknown")));
    }

    // 打包失败时丢弃半成品：残留的截断 .lpkg 会被误当成有效包（其哈希也算得出来）
    const auto discard_partial = [&output_filename] {
        std::error_code ec;
        fs::remove(output_filename, ec);
    };
    int close_rc = ARCHIVE_OK;

    try {
        log_info(get_string("info.pack_scanning"));

        // 1. 生成并添加 metadata.json
        fs::path tmp_meta = Config::get_tmp_dir() / constants::PKG_METADATA_FILE;
        ensure_dir_exists(tmp_meta.parent_path());
        {
            json meta;
            meta[std::string(constants::J_NAME)] = pkg_name;
            meta[std::string(constants::J_VERSION)] = pkg_version;
            meta[std::string(constants::J_DEPS)] = deps;
            meta[std::string(constants::J_PROVIDES)] = provides;
            meta[std::string(constants::J_PROVIDES_SONAME)] = provides_soname;
            meta[std::string(constants::J_NEEDED_SO)] = needed_so;
            meta[std::string(constants::J_MAN)] = man_content;

            // 用 `write_string_to_file`（带 open/写失败检查 + fsync + 原子 rename）而不是
            // 裸 `std::ofstream`：后者不查失败，磁盘满/IO 错误会留下**截断的 metadata.json**
            // 并被照常打进 `.lpkg`，而 `pack_package` 仍报成功、还给它算 SHA256。
            write_string_to_file(tmp_meta, meta.dump(2) + "\n");
        }
        add_to_archive(a, tmp_meta, std::string(constants::PKG_METADATA_FILE), output_filename);
        std::error_code ec;
        fs::remove(tmp_meta, ec);

        // 2. 添加 hooks 目录
        if (fs::exists(hooks_dir)) {
            add_dir_recursive(a, hooks_dir, std::string(constants::DIR_HOOKS), output_filename);
        }

        // 3. 添加内容文件（root 目录 -> content/）
        // 先添加目录条目本身
        add_to_archive(a, root_dir, std::string(constants::DIR_CONTENT), output_filename);
        add_dir_recursive(a, root_dir, std::string(constants::DIR_CONTENT), output_filename);

        close_rc = archive_write_close(a);
        archive_write_free(a);
    } catch (...) {
        archive_write_close(a);
        archive_write_free(a);
        discard_partial();
        throw;
    }

    // close 的返回值就是"整包是否真的写完落盘"：写失败（磁盘满/EIO）时 libarchive
    // 返回 ARCHIVE_FAILED/FATAL。不检查就会把**截断的 .lpkg 当成功**，还对截断内容
    // 算 SHA256 → farm 把该哈希写进索引，下游校验通过、装到一半才炸。
    // 阈值取 ARCHIVE_WARN：FAILED/FATAL 一律失败，WARN 只告警（与 archive.cpp 读侧一致）。
    if (close_rc < ARCHIVE_WARN) {
        discard_partial();
        throw LpkgException(string_format("error.archive_close_failed", output_filename));
    }
    if (close_rc < ARCHIVE_OK) {
        // 这里**没有**丢弃归档（随后照常算 SHA256、报成功），所以用不带"已丢弃"文案的
        // warning 键；`< ARCHIVE_WARN` 的上面那一支才是真丢弃，那一支仍用
        // error.archive_close_failed。
        log_warning(string_format("warning.archive_close_incomplete", output_filename));
    }

    std::string hash = calculate_sha256(output_filename);
    std::cout << get_string("info.pack_success") << " " << output_filename << std::endl;
    std::cout << get_string("info.sha256_label") << hash << std::endl;
}
