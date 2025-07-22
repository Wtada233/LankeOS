#include "archive.hpp"
#include "utils.hpp"
#include <archive.h>
#include <archive_entry.h>

bool extract_tar_zst(const std::string& archive_path, const std::string& output_dir) {
    struct archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    struct archive* ext = archive_write_disk_new();
    archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_FFLAGS);
    archive_write_disk_set_standard_lookup(ext);

    if (archive_read_open_filename(a, archive_path.c_str(), 10240) != ARCHIVE_OK) {
        archive_read_free(a);
        archive_write_free(ext);
        return false;
    }

    struct archive_entry* entry;
    int r;
    int count = 0;
    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        std::string path = output_dir + "/" + archive_entry_pathname(entry);
        archive_entry_set_pathname(entry, path.c_str());
        r = archive_write_header(ext, entry);
        if (r < ARCHIVE_OK) {
            break;
        }

        const void* buff;
        size_t size;
        la_int64_t offset;
        while ((r = archive_read_data_block(a, &buff, &size, &offset)) == ARCHIVE_OK) {
            if (archive_write_data_block(ext, buff, size, offset) < ARCHIVE_OK) {
                break;
            }
        }

        if (r < ARCHIVE_OK) break;
        archive_write_finish_entry(ext);

        if (++count % 100 == 0) {
            log_sync("正在解压文件: " + std::to_string(count) + " 个已处理");
        }
    }

    archive_read_close(a);
    archive_read_free(a);
    archive_write_close(ext);
    archive_write_free(ext);

    log_sync("解压完成，共处理 " + std::to_string(count) + " 个文件");
    return (r == ARCHIVE_EOF);
}
