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
void add_to_archive(struct archive* a, const fs::path& path, const std::string& entry_name)
{
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return;

    // 用 libarchive 的 **disk reader** 填 entry：它会一并带上 xattr **和真正的 ACL 记录**
    // （PAX `SCHILY.acl.access` 等）。此前手工 copy_stat + 把 ACL 当裸 xattr 写
    // （`SCHILY.xattr.system.posix_acl_access`）是**不生效**的——libarchive 不会把裸 xattr
    // 当 ACL 应用，实测（真 lpkg pack/install 后 `getfacl` 命名条目消失；bsdtar 用
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
        if (len != -1) {
            link_target[len] = '\0';
            archive_entry_set_symlink(entry, link_target);
        }
    }

    if (archive_write_header(a, entry) != ARCHIVE_OK) {
        archive_entry_free(entry);
        throw LpkgException(
            string_format("error.archive_write_header_failed", archive_error_string(a)));
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
                archive_entry_free(entry);
                throw LpkgException(
                    string_format("error.archive_write_data_failed", archive_error_string(a)));
            }
        }
    }

    archive_entry_free(entry);
}

/** 递归遍历目录并将所有文件和子目录添加到归档中 */
void add_dir_recursive(struct archive* a, const fs::path& dir, const std::string& archive_prefix)
{
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        fs::path rel = entry.path().lexically_relative(dir);
        std::string entry_name = archive_prefix + "/" + rel.string();
        add_to_archive(a, entry.path(), entry_name);
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
                  const std::string& man_content, const std::vector<std::string>& needed_so)
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
        throw LpkgException(string_format("error.archive_open_failed", archive_error_string(a)));
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
            meta[std::string(constants::J_NEEDED_SO)] = needed_so;
            meta[std::string(constants::J_MAN)] = man_content;

            std::ofstream f(tmp_meta);
            f << meta.dump(2) << std::endl;
        }
        add_to_archive(a, tmp_meta, std::string(constants::PKG_METADATA_FILE));
        std::error_code ec;
        fs::remove(tmp_meta, ec);

        // 2. 添加 hooks 目录
        if (fs::exists(hooks_dir)) {
            add_dir_recursive(a, hooks_dir, std::string(constants::DIR_HOOKS));
        }

        // 3. 添加内容文件（root 目录 -> content/）
        // 先添加目录条目本身
        add_to_archive(a, root_dir, std::string(constants::DIR_CONTENT));
        add_dir_recursive(a, root_dir, std::string(constants::DIR_CONTENT));

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
    // 算 SHA256 → farm 把该哈希写进索引，下游校验通过、装到一半才炸（TODO.md B2）。
    // 阈值取 ARCHIVE_WARN：FAILED/FATAL 一律失败，WARN 只告警（与 archive.cpp 读侧一致）。
    if (close_rc < ARCHIVE_WARN) {
        discard_partial();
        throw LpkgException(string_format("error.archive_close_failed", output_filename));
    }
    if (close_rc < ARCHIVE_OK) {
        log_warning(string_format("error.archive_close_failed", output_filename));
    }

    std::string hash = calculate_sha256(output_filename);
    std::cout << get_string("info.pack_success") << " " << output_filename << std::endl;
    std::cout << get_string("info.sha256_label") << hash << std::endl;
}
