/**
 * test_phase_log_messages.cpp — "按包分阶段"的进度日志必须**点名包**
 *
 * 一次多包批次（`lpkg install A B C`、或一次升级几百个包）里，这些阶段日志会一个包接一个包
 * 地刷，但文本原来都不带包名，用户分不清在装谁（"解压什么？你得说清楚"）：
 *   `正在解压到临时目录...` / `正在解压文件: 300 个已处理` / `解压完成，共处理 300 个文件`
 *   `正在检查依赖...` / `正在复制文件到系统...` / `正在替换: 1.0 → 2.0, 3 个旧文件待处理`
 * 本文件 pin：上面每一行本身都必须点名包（与 `info.running_hook` 同一收口口径）。
 *
 * 断言刻意**落在那一行本身**：只断言"整段输出里含包名"是空转 —— 安装本来就会打印
 * `开始安装 <包名>`，无论这些日志改没改都会绿（同 test_hook_log_message.cpp 的教训）。
 *
 * 定位"那一行"用的是 **l10n 模板本身**（把模板按占位符切成字面片段，要求某一行按顺序含全部
 * 片段、且**第一个占位符的位置上正是包名**），而不是写死语言关键词：
 *   · 与语言无关：zh/en 模板各自定位到各自的行，测试不用维护两套关键词；
 *   · 造红时仍定位得到：模板退回"不带包名"的旧形态（没有占位符）时，
 *     "包名必须落在第一个占位符处"无从满足 → 匹配不到行 → 红，
 *     而不是变成"找不到行"的另一种红（两种红的诊断价值不同）。
 *   · 顺带钉住参数落在**对的槽位**上（`string_format` 是 std::vformat，槽位顺序即用户看到的语序）。
 *
 * 造红说明（踩过一次，值得记）：l10n 是**运行时加载**的，查找顺序为 `<exe>/../l10n` →
 * `<exe>/../main/l10n` → `<exe>/../src/l10n` → 安装路径。容器里跑测试命中的是
 * **`/app/main/l10n`**（同步进容器的源码树），改 `/usr/share/lpkg/l10n` 造不出红；
 * 且容器里 `LANG` 为空 ⇒ 生效的是 **`en.txt`**，只改 `zh.txt` 同样造不出红。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

class PhaseLogMessageTest : public IntegrationTestBase
{
protected:
    /// pack_with_files 的落盘目录序号：保证每个测试包各占一套路径（见该函数的说明）
    int pack_seq_ = 0;

    void SetUp() override
    {
        IntegrationTestBase::SetUp();
        setup_local_mirror();  // 本地空镜像：让 repo.load_index 走本地、不联网
    }

    /**
     * 打一个含 @p file_count 个文件的包。
     *
     * 文件数要**大于** `constants::PROGRESS_INTERVAL_FILES`（100）才会出现"每 100 个刷一次"
     * 的进度行（这里取 300 ⇒ 至少刷两次），不必造 5000 个文件那么夸张。
     *
     * 落盘目录按调用序号**每次都不同**（而不是按包名/版本）：两个测试包落同一路径会撞上
     * 批次的文件冲突预检（`check_batch_file_conflicts` 直接拒绝整批），而且文件名/路径与
     * 包名无关，也就堵掉了"靠路径蹭到包名"的假绿。
     */
    std::string pack_with_files(const std::string& name, const std::string& version, int file_count,
                                const std::vector<std::string>& deps = {})
    {
        const fs::path work = suite_work_dir / ("_pkg_" + name + "-" + version);
        const fs::path files_dir =
            work / "content" / "usr" / "share" / ("phasefiles" + std::to_string(++pack_seq_));
        fs::create_directories(files_dir);
        for (int i = 0; i < file_count; ++i)
            std::ofstream(files_dir / ("f" + std::to_string(i))) << "x\n";
        const std::string path = (pkg_dir / (name + "-" + version + ".lpkg")).string();
        pack_package(path, work.string(), name, version, deps, {}, "man " + name, {});
        return path;
    }

    /**
     * RAII 捕获 stdout。
     *
     * 不用裸 `CaptureStdout()` + `GetCapturedStdout()`：一旦 install_packages 抛异常，
     * 裸写法会让 gtest 的 stdout 捕获器**泄漏**，异常消息与后续测试的 `[ RUN ]` 全被吞进
     * 捕获用的临时文件里，终端只剩一条莫名其妙的
     * `[ FATAL ] Only one stdout capturer can exist at a time.`（本测试初版就踩了这个：
     * 一次包间文件冲突被伪装成 gtest 内部崩溃）。析构里 release，异常时也看得见真相。
     *
     * `stop()` 必须在**断言之前**调用：捕获期内 gtest 自己的失败消息（"文件:行 + 说明"）
     * 也走 stdout，会被一起吞掉 —— 只能看到 `[ FAILED ]` 却看不到为什么。
     */
    struct CaptureOut {
        std::string out;
        bool stopped = false;

        CaptureOut()
        {
            testing::internal::CaptureStdout();
        }

        void stop()
        {
            if (!stopped) {
                out = testing::internal::GetCapturedStdout();
                stopped = true;
            }
        }

        ~CaptureOut()
        {
            stop();
        }

        CaptureOut(const CaptureOut&) = delete;
        CaptureOut& operator=(const CaptureOut&) = delete;
    };

    /// 把模板文本按 `{...}` 占位符切成字面片段（占位符本身丢掉，`{` 无配对 `}` 时按字面）
    static std::vector<std::string> literal_segments(const std::string& text)
    {
        std::vector<std::string> segs;
        std::string cur;
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '{') {
                const size_t close = text.find('}', i);
                if (close != std::string::npos) {
                    segs.push_back(cur);
                    cur.clear();
                    i = close;
                    continue;
                }
            }
            cur.push_back(text[i]);
        }
        segs.push_back(cur);
        return segs;
    }

    /**
     * 在输出里定位"渲染自 l10n 键 @p key 的那一行"，返回该行；匹配不到返回空串。
     *
     * 约定：这些阶段消息的**第一个占位符就是被处理包的名字**（本文件六条消息中英文都如此）。
     * 于是判据是"某一行的 head 之后**紧跟**包名，其余字面片段再按顺序出现"—— 剩下那些计数
     * 占位符当通配处理（个数由 `PROGRESS_INTERVAL_FILES`、归档成员数决定，测试不该钉死）。
     */
    static std::string phase_line(const std::string& out, const std::string& key,
                                  const std::string& pkg)
    {
        const std::string tmpl = get_string(key);
        const size_t open = tmpl.find('{');
        const size_t close = (open == std::string::npos) ? std::string::npos : tmpl.find('}', open);
        // 模板里根本没有占位符 ⇒ 这一版消息没地方放包名 ⇒ 不给匹配（断言据此报红）
        if (close == std::string::npos) return std::string{};
        const std::string head = tmpl.substr(0, open);
        const std::vector<std::string> tail = literal_segments(tmpl.substr(close + 1));

        const auto matches = [&](const std::string& line) {
            const size_t hit = line.find(head);
            if (hit == std::string::npos) return false;
            size_t pos = hit + head.size();
            // 包名必须正落在第一个占位符的槽位上（顺序写错/槽位写错都算没点名）
            if (line.compare(pos, pkg.size(), pkg) != 0) return false;
            pos += pkg.size();
            for (const auto& seg : tail) {
                const size_t next = line.find(seg, pos);
                if (next == std::string::npos) return false;
                pos = next + seg.size();
            }
            return true;
        };

        std::string cur;
        for (const char c : out) {
            if (c == '\n') {
                if (matches(cur)) return cur;
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        return matches(cur) ? cur : std::string{};
    }

    /** 断言：@p key 这条消息渲染出的**那一行**必须点名 @p pkg */
    static void expect_phase_names_package(const std::string& out, const std::string& key,
                                           const std::string& pkg, const char* phase)
    {
        const std::string line = phase_line(out, key, pkg);
        ASSERT_FALSE(line.empty())
            << "阶段日志 \"" << phase << "\" 的那一行没有点名包（该多包批次里分不清在装谁）。\n"
            << "  模板(" << key << "): " << get_string(key) << "\n"
            << "完整输出:\n"
            << out;
    }
};

TEST_F(PhaseLogMessageTest, InstallPhaseLinesNameThePackage)
{
    // 先装一个依赖提供者：被测包带 deps 时 ensure_dependencies_satisfied 才会打印
    // info.checking_deps（deps 为空时它直接早退，压根不会有那行）。
    ASSERT_NO_THROW(install_packages({pack_with_files("zqphase-dep", "1.0", 1)}));

    // 包名与依赖名无前缀关系，避免"find(包名)"蹭到别的行
    const std::string pkg = "zqphasepkg";
    const std::string pkg_file = pack_with_files(pkg, "1.0", 300, {"zqphase-dep"});

    CaptureOut cap;
    install_packages({pkg_file});
    cap.stop();
    const std::string& out = cap.out;

    // 解压三条：起始 / 进度（300 个文件 ⇒ 每 100 个刷一次，至少两行）/ 完成
    expect_phase_names_package(out, "info.extracting_to_tmp", pkg, "解压到临时目录");
    expect_phase_names_package(out, "info.extracting", pkg, "解压进度");
    expect_phase_names_package(out, "info.extract_complete", pkg, "解压完成");
    expect_phase_names_package(out, "info.checking_deps", pkg, "检查依赖");
    expect_phase_names_package(out, "info.copying_files", pkg, "复制文件到系统");
}

TEST_F(PhaseLogMessageTest, UpgradeReplaceLineNamesThePackage)
{
    const std::string pkg = "zqphaseupg";
    ASSERT_NO_THROW(install_packages({pack_with_files(pkg, "1.0", 1)}));

    // 同包名装更新版本 = 升级路径：`正在替换: 1.0 → 2.0, N 个旧文件待处理` 必须点名
    const std::string upgrade_file = pack_with_files(pkg, "2.0", 1);
    CaptureOut cap;
    install_packages({upgrade_file});
    cap.stop();
    const std::string& out = cap.out;

    expect_phase_names_package(out, "info.upgrade_old_files_check", pkg, "替换旧文件");
}
