#include "downloader.hpp"

#include <curl/curl.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>

#include "base/exception.hpp"
#include "base/utils.hpp"
#include "i18n/localization.hpp"
#include "ui/term.hpp"

namespace fs = std::filesystem;

/** 定义在 `main_cli.cpp`，由 SIGINT 处理函数设置（`SigIntGuard` 生命周期内生效）。
 *  本文件只在进度回调里读它 —— 那是下载期间唯一能看见 Ctrl+C 的地方。 */
extern std::atomic<bool> sigint_graceful;

/** HTTP 下载回调函数，将 curl 接收到的数据写入 ostream 输出流 */
size_t write_data_cpp(void* ptr, size_t size, size_t nmemb, void* stream)
{
    std::ostream* out = static_cast<std::ostream*>(stream);
    size_t bytes = size * nmemb;
    out->write(static_cast<char*>(ptr), bytes);
    return out->good() ? bytes : 0;
}

namespace
{
/// curl 进度回调的上下文：目标行 + 速率/ETA 的计时基准。
struct DlProgress {
    ui::Line* line = nullptr;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last = start;
    int last_pct = -1;  ///< 与上一帧相同就跳过：小文件会在毫秒内回调好几次、每帧都是 100%
};

/**
 * 下载进度：`pkg/1.0.lpkg   968.5 KiB  4.57 MiB/s 00:02  [####----]  62%`
 * （左 = 包名/文件名，中 = 已收字节 + 速率 + ETA，右 = 进度条；非 TTY 全部降级为一行纯文本）。
 *
 * **节流**：curl 每次收到数据都回调，逐次重绘等于每条数据一个 write 系统调用 —— 上限
 * 10 次/秒（完成时不受节流，保证 100% 那一帧一定画出来）。
 */
int progress_callback(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                      [[maybe_unused]] curl_off_t ultotal, [[maybe_unused]] curl_off_t ulnow)
{
    auto* st = static_cast<DlProgress*>(clientp);
    // **Ctrl+C 的唯一出口**（2026-10-03 补）：`lpkg build` 下载源码是**进程内** curl
    // （没有子进程接收信号），而这个回调是下载期间唯一被反复调到的地方 —— 在这里看一眼
    // 优雅退出标志并**返回非 0**，curl 会以 `CURLE_ABORTED_BY_CALLBACK` 中止传输
    // （调用方据此抛 `UserAbort`，走带清理的异常路径）。不这么做的话，`SIGINT` 只是置了个
    // 标志位，`build` 会**继续下完**整个源码包 —— 用户按 Ctrl+C 看起来毫无反应。
    if (sigint_graceful.load()) return 1;
    if (!st || !st->line || dltotal <= 0) return 0;  // 服务端没给长度 → 算不出百分比

    const double pct = 100.0 * static_cast<double>(dlnow) / static_cast<double>(dltotal);
    const int pct_i = static_cast<int>(pct);
    if (pct_i == st->last_pct) return 0;  // 同一帧不重画

    const auto now = std::chrono::steady_clock::now();
    const bool done = dlnow >= dltotal;
    if (!done &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - st->last).count() < 100)
        return 0;
    st->last = now;
    st->last_pct = pct_i;

    const double secs = std::chrono::duration<double>(now - st->start).count();
    const double rate = secs > 0.0 ? static_cast<double>(dlnow) / secs : 0.0;
    std::string mid =
        ui::human_size(static_cast<std::uint64_t>(dlnow)) + "  " + ui::human_rate(rate);
    if (rate > 0.0) mid += "  " + ui::human_time(static_cast<double>(dltotal - dlnow) / rate);
    st->line->progress(pct, mid);
    return 0;
}
}  // namespace

/** CURL 句柄的自定义删除器，用于智能指针自动清理 */
struct CurlDeleter {
    void operator()(CURL* curl) const
    {
        if (curl) {
            curl_easy_cleanup(curl);
        }
    }
};
using CurlHandle = std::unique_ptr<CURL, CurlDeleter>;

/** 查找系统 CA 证书包路径，依次检测常见发行版的证书文件位置 */
const char* find_ca_bundle()
{
    static constexpr std::array paths = {
        std::string_view{"/etc/ssl/certs/ca-certificates.crt"},  // Debian/Ubuntu/Arch/Alpine
        std::string_view{"/etc/pki/tls/certs/ca-bundle.crt"},    // RHEL/CentOS/Fedora
        std::string_view{"/etc/ssl/ca-bundle.pem"},              // OpenSUSE
        std::string_view{"/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem"},  // New
                                                                                // Fedora/RHEL
        std::string_view{"/etc/ssl/cert.pem"}                                   // Others
    };
    for (auto path : paths) {
        if (access(path.data(), R_OK) == 0) {
            return path.data();
        }
    }
    return nullptr;
}

/** 下载单个文件，支持进度条显示和 CA 证书验证，失败时抛出异常 */
void download_file(const std::string& url, const fs::path& output_path, bool show_progress)
{
    CurlHandle curl(curl_easy_init());
    if (!curl) {
        throw LpkgException(string_format("error.download_failed", url));
    }

    std::ofstream ofile(output_path, std::ios::binary);
    if (!ofile) {
        throw LpkgException(string_format("error.create_file_failed", output_path.string()));
    }

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_data_cpp);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &ofile);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_FAILONERROR, 1L);

    // 找不到 CA 证书包时**硬失败**：曾静默把 VERIFYPEER/VERIFYHOST 关掉，等于让包与
    // index.txt 一起落到 MITM 手里（哈希也来自同一个被劫持的索引，防线全失）。
    // 缺 CA 包是可修的部署问题，不该用"降低安全性"来绕过。
    if (const char* ca_path = find_ca_bundle()) {
        curl_easy_setopt(curl.get(), CURLOPT_CAINFO, ca_path);
    } else {
        throw LpkgException(string_format("error.no_ca_bundle", url));
    }

    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, constants::CURL_CONNECT_TIMEOUT_SEC);
    // 低速超时：平均速度低于 CURL_LOW_SPEED_LIMIT_BPS 字节/秒、持续 CURL_LOW_SPEED_TIME_SEC 秒
    // 即中止（防"连上了但不动"的挂死）。阈值集中在 base/constants.hpp，别再硬编码。
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, constants::CURL_LOW_SPEED_LIMIT_BPS);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, constants::CURL_LOW_SPEED_TIME_SEC);

    // 进度行：左 = `<包名>/<文件名>`（下载落点的父目录名 + 文件名），右 = 进度条。
    // 落点常是 `.part`（构建期"先下到 .part 再 rename"，见 download_and_prepare_sources）——
    // 显示时剥掉那个后缀，用户该看到的是目标文件名，不是我们的临时名。
    std::string fname = output_path.filename().string();
    if (fname.ends_with(".part")) fname.resize(fname.size() - 5);
    const std::string parent = output_path.parent_path().filename().string();
    ui::Line line;
    DlProgress prog;
    if (show_progress) line = ui::Line(parent.empty() ? fname : parent + "/" + fname);
    // 回调**总是**装上（`show_progress=false` 时只是不刷进度行）：它是下载期间唯一能看见
    // Ctrl+C 的地方 —— 见 progress_callback 的说明。装 NOPROGRESS=1 会让 curl 干脆不调它。
    prog.line = show_progress ? &line : nullptr;
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &prog);

    CURLcode res = curl_easy_perform(curl.get());
    // TTY：把进度条那一行原地收在 100%（失败则换成 [FAILED]）；非 TTY：一行纯文本。
    if (show_progress) {
        if (res == CURLE_OK)
            line.finish_progress();
        else
            line.finish(ui::ok(false));
    }

    // 用户取消 ≠ 下载失败：`CURLE_ABORTED_BY_CALLBACK` 只可能由我们的回调（Ctrl+C）产生，
    // 报成"下载失败"会让上层把它当 I/O 错误、也让用户看不懂（见 progress_callback）。
    if (res == CURLE_ABORTED_BY_CALLBACK && sigint_graceful.load())
        throw UserAbort(get_string("info.sigint_aborted"));

    if (res != CURLE_OK) {
        throw LpkgException(string_format("error.download_failed", url) + ": " +
                            curl_easy_strerror(res));
    }

    // 本地写盘错误（磁盘满/EIO）只会在 flush/close 时暴露：不检查就会留下被静默
    // 截断的文件，而 download_with_retries 会把它当成"下载成功"（历史 TODO.md B4）。
    ofile.flush();
    if (!ofile) {
        throw LpkgException(string_format("error.create_file_failed", output_path.string()));
    }
    ofile.close();
}

/** 带重试机制的下载函数，最多重试 max_retries 次，每次失败后清理临时文件 */
void download_with_retries(const std::string& url, const fs::path& output_path, int max_retries,
                           bool show_progress)
{
    // max_retries <= 0 曾让循环体一次都不执行 → 静默返回且什么都没下载（调用方以为成功）
    if (max_retries < 1) max_retries = 1;
    for (int i = 0; i < max_retries; ++i) {
        try {
            download_file(url, output_path, show_progress);
            return;
        } catch (const UserAbort&) {
            // **取消不是可重试的失败**：`UserAbort` 是 `LpkgException` 的子类，被下面那个宽
            // catch 接住的话，Ctrl+C 会变成"删掉已下了一半的文件 → 打一条「正在重试」→ 接着
            // 重试 5 次"。而重试期间 `sigint_graceful` 仍为真、进度回调立刻又返回 1 ⇒ 空转。
            // 必须单独先接住、直接穿透（与 main_cli.cpp 的 catch 顺序同款）。
            // 用例：tests/unit/test_download_cancel.cpp（两个锚点：不得出现重试告警 +
            // 已落位的输出文件不得被删）。
            throw;
        } catch (const LpkgException& e) {
            // 用 ec 重载：抛型 `fs::remove` 若失败（文件本就不存在/权限问题）会**顶替**原异常，
            // 于是原始失败原因被丢掉、且 `throw;` 的后续重试逻辑被跳过。清理失败无所谓，
            // 绝不能覆盖正在处理的错误。
            std::error_code ec;
            fs::remove(output_path, ec);  // 清理失败的下载文件
            if (i < max_retries - 1) {
                log_warning(string_format("info.retrying", e.what()));
            } else {
                throw;  // 最后一次重试失败，向上抛出异常
            }
        }
    }
}
