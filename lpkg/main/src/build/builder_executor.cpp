#include "builder_executor.hpp"
#include "downloader.hpp"
#include "archive.hpp"
#include "base/exception.hpp"
#include "i18n/localization.hpp"
#include "base/utils.hpp"
#include "base/constants.hpp"

#include <fstream>
#include <array>

namespace fs = std::filesystem;

/**
 * 下载并准备构建所需的源码
 * 将 sources 中的归档文件自动解压到工作目录，
 * 将 work_sources 中的文件直接复制到工作目录
 */
std::vector<fs::path>
download_and_prepare_sources(const std::vector<std::string>& sources,
                             const std::vector<std::string>& work_sources,
                             const fs::path& build_dir,
                             const fs::path& work_root) {
    std::vector<fs::path> downloaded_files;

    auto download_one = [&](const std::string& url) -> fs::path {
        fs::path filename = fs::path(url).filename();
        fs::path dest = build_dir / filename;
        if (!fs::exists(dest)) {
            download_with_retries(url, dest, 3, true);
            downloaded_files.push_back(dest);
        } else {
            log_info(string_format("info.source_exists", filename.string()));
        }
        return dest;
    };

    for (const auto& url : sources) {
        fs::path dest = download_one(url);
        fs::path filename = dest.filename();

        std::string ext = dest.extension().string();
        if (ext == ".gz" || ext == ".bz2" || ext == ".xz"
            || ext == ".zst" || ext == ".tgz" || ext == ".tar"
            || ext == ".zip") {
            log_info(string_format("info.auto_extracting", filename.string()));
            try {
                extract_tar_zst(dest, work_root);
            } catch (const std::exception& e) {
                log_warning(string_format("warning.auto_extract_failed",
                                          filename.string(), e.what()));
            }
        }
    }

    for (const auto& url : work_sources) {
        fs::path dest = download_one(url);
        fs::path filename = dest.filename();
        fs::path target_path = work_root / filename;

        log_info(string_format("info.copying_to_workdir", filename.string()));
        try {
            if (fs::exists(target_path)) {
                fs::remove(target_path);
            }
            fs::copy_file(dest, target_path, fs::copy_options::overwrite_existing);
        } catch (const std::exception& e) {
            throw LpkgException(string_format("error.copy_work_source_failed",
                                              filename.string(), std::string(e.what())));
        }
    }

    return downloaded_files;
}

/**
 * 检测工作目录中的源码树结构
 * 如果工作目录中只有一个子目录，则返回该子目录作为源码根目录（常见的 tarball 解压后单目录结构）
 * 否则返回工作目录本身
 */
fs::path detect_source_tree(const fs::path& work_root) {
    if (!fs::exists(work_root) || !fs::is_directory(work_root)) {
        return work_root;
    }

    int dir_count = 0;
    fs::path lone_dir;

    for (const auto& entry : fs::directory_iterator(work_root)) {
        if (entry.is_directory()) {
            lone_dir = entry.path();
            ++dir_count;
        } else {
            // 顶层有文件说明不是单目录结构
            return work_root;
        }
    }

    if (dir_count == 1) {
        log_info(string_format("info.detected_source_tree",
                               lone_dir.filename().string()));
        return lone_dir;
    }
    return work_root;
}

/**
 * 读取构建脚本内容，并进行变量替换
 * 将脚本中的 {PKG_NAME}、{SRC_DIR} 等占位符替换为实际值
 */
std::string process_build_script(const fs::path& script_path,
                                 const std::map<std::string, std::string>& vars) {
    std::string content;
    {
        std::ifstream f(script_path);
        content.assign(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
    }
    for (const auto& [from, to] : vars) {
        string_replace_all(content, from, to);
    }
    return content;
}

/**
 * 执行构建阶段的 shell 脚本
 * source 处理后的构建脚本，然后调用指定的 phase_name 函数
 * 构建失败时清理临时脚本并抛出异常
 */
void execute_build_phase(const std::string& phase_name,
                         const fs::path& work_dir,
                         const fs::path& processed_script_path) {
    log_info(string_format("info.executing_phase", phase_name));
    std::string cmd = ". " + fs::absolute(processed_script_path).string()
                      + " && " + phase_name;
    int ret = run_shell(cmd, work_dir);
    if (ret != 0) {
        fs::remove(processed_script_path);
        throw LpkgException(string_format("error.build_phase_failed",
                                          phase_name, std::to_string(ret)));
    }
}
