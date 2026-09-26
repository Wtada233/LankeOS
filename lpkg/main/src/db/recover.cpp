/**
 * recover.cpp — WAL 恢复与清理
 *
 * recover_packages(): 紧急恢复 — 仅在进程因崩溃（断电/OOM/SIGKILL）
 * 而未能执行 catch 中的 batch_rollback() 时使用。
 *
 * trim_completed(): 清理已完成批次的 WAL 日志行，释放磁盘空间。
 */

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <set>
#include <vector>

#include "base/constants.hpp"
#include "base/utils.hpp"
#include "cache.hpp"
#include "config/config.hpp"
#include "i18n/localization.hpp"
#include "transaction_log.hpp"
#include "wal_op.hpp"

namespace fs = std::filesystem;

namespace wal
{

std::set<fs::path> referenced_stash_roots()
{
    std::set<fs::path> roots;
    std::ifstream file(wal_log_path());
    if (!file.is_open()) return roots;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        auto op = parse_op(line);
        if (!op.is_valid()) continue;
        if ((op.type == WALOpType::BACKUP || op.type == WALOpType::REMOVE_OLD) && !op.arg2.empty())
            roots.insert(stash_root_of_bak(op.arg2).lexically_normal());
        // `UNSTASH <bak> → <orig>`：**stash 侧是 arg1**（与 BACKUP 相反 —— BACKUP 的 arg1 是
        // 原位、arg2 是 stash 侧）。实践上它总与配对的 BACKUP 同根、已被上面那行收集，
        // 所以这里是**冗余**的；但"WAL 仍引用哪些 stash 根"这个集合的定义应该是"凡是 WAL 里
        // 出现过的 stash 侧路径"，不该依赖"总有配对的 BACKUP"这条论证 —— 机制 > 论证。
        else if (op.type == WALOpType::UNSTASH && !op.arg1.empty())
            roots.insert(stash_root_of_bak(op.arg1).lexically_normal());
        else if (op.type == WALOpType::CLEANUP && !op.arg1.empty())
            roots.insert(stash_root_of_bak(op.arg1).lexically_normal());
    }
    return roots;
}

}  // namespace wal

// ============================================================================
// 共用小工具：读 WAL / 批次配对扫描
// ============================================================================

/**
 * 读 WAL 的全部行（丢弃空行与行尾 \r）。
 *
 * 返回 nullopt = "没有可处理的日志"（路径不存在 / 打不开）；返回**空** vector = "文件存在但
 * 为空"。两者不能合并成一个返回值：trim_completed 对空文件要 remove、对"打不开"只是返回，
 * 而 recover_packages 对两者都直接返回。
 *
 * 判定不抛：WAL 路径被符号链接环占着时 `fs::exists` 会抛 —— 而这是在**崩溃恢复**的入口上，
 * 抛出去等于恢复永远跑不完（见 base/utils.hpp 的谓词说明）。
 */
static std::optional<std::vector<std::string>> read_wal_lines(const std::string& wpath)
{
    if (!exists_follow(wpath)) return std::nullopt;

    std::ifstream file(wpath);
    if (!file.is_open()) return std::nullopt;

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        if (line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

/**
 * WAL 的批次配对情况 —— **前向 depth 记账的唯一实现**。
 *
 * 三处曾各写一份：continue_post_commit_cleanup、recover_packages、trim_completed。
 * 现在只有这里定义"什么是未提交区域"，其余各处都读它的结果。
 *
 * "未提交区域起点"必须是**第一个**未配对 BEGIN_PKGS，而不是最后一个：一次崩溃可能留下
 * **两个**未提交批次（前一批回滚自身失败后同进程又开了新批次），按"最后一个"裁剪/回滚会让
 * 更早那批的 BEGIN/BACKUP 行被当成已完成内容处理 —— 恢复依据就此消失；而且已提交批次的行
 * 仍留在 WAL 里、下一轮扫描的起点仍落在它上面，更早那批永远轮不到（实测两轮下来文件一次都
 * 没被还原，TODO.md X6/Z5）。配对成功的批次（depth 回到 0）即清空起点。
 */
struct BatchPairing {
    ssize_t unpaired_begin = -1;  // **第一个**未配对 BEGIN_PKGS 的行号；-1 = 无未提交区域
    ssize_t last_commit = -1;     // **最后一个** COMMIT_PKGS 的行号；-1 = 一条都没有
};

static BatchPairing scan_batch_pairing(const std::vector<std::string>& lines)
{
    BatchPairing pairing;
    int depth = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        auto op = wal::parse_op(lines[i]);
        if (!op.is_valid()) continue;
        if (op.type == wal::WALOpType::BEGIN_PKGS) {
            if (depth == 0) pairing.unpaired_begin = static_cast<ssize_t>(i);
            ++depth;
        } else if (op.type == wal::WALOpType::COMMIT_PKGS) {
            pairing.last_commit = static_cast<ssize_t>(i);
            if (depth > 0 && --depth == 0) pairing.unpaired_begin = -1;  // 该批次已配对提交
        }
    }
    return pairing;
}

// ============================================================================
// 续传 post-commit 清理
// ============================================================================

/**
 * 删除"已提交批次"残留的 .lpkg_bak（post-commit 清理的崩溃续传）。
 *
 * 背景：install/upgrade 的 .lpkg_bak 清理发生在 COMMIT_PKGS 之后（I-BAK-2），
 * 通过 cleanup_baks 写 CLEANUP 行（write-ahead）。若崩溃在"COMMIT 之后、
 * 清理完成之前"，磁盘上残留 bak，其记录（BACKUP/CLEANUP 行）仍在 WAL 中——
 * 本函数据此续删，保证随后 trim 能安全地把完成事务连同清理记录一起清掉，
 * 不再出现孤儿 .lpkg_bak（曾因 install 清理裸 fs::remove 无 WAL 且 trim 清空
 * 整个文件，导致残留永远留在磁盘）。
 *
 * 只处理"已提交区域"（**第一个**未配对 BEGIN_PKGS 之前）的 BACKUP/REMOVE_OLD dst，
 * 以及尾部 post-commit CLEANUP 行引用的 bak；绝不动未提交批次的 bak
 * （那是 reverse_execute 回滚要用的）。
 */
static void continue_post_commit_cleanup(const std::vector<std::string>& lines)
{
    // 1. 定位：**第一个**未配对 BEGIN_PKGS（未提交区域起点）与最后一个 COMMIT_PKGS。
    //
    //    夹在两个未提交批次之间的 bak 属于**还没还原**的前一批，若按"已提交区域"删掉就是
    //    永久丢文件（TODO.md X6）；配对记账的语义见 scan_batch_pairing 的说明。
    const BatchPairing pairing = scan_batch_pairing(lines);
    const ssize_t unpaired = pairing.unpaired_begin;
    const ssize_t last_commit = pairing.last_commit;

    // 2. 收集 bak：
    //    - 已提交区域（[0, unpaired) 或全部）的 BACKUP/REMOVE_OLD dst；
    //    - post-commit 尾部（最后一个 COMMIT_PKGS 之后、未提交批次之前）的
    //      CLEANUP 行引用的 bak。未提交批次内的东西一个都不碰（回滚要用）。
    std::vector<fs::path> baks;
    const size_t region_end = (unpaired < 0) ? lines.size() : static_cast<size_t>(unpaired);
    for (size_t i = 0; i < region_end; ++i) {
        auto op = wal::parse_op(lines[i]);
        if (!op.is_valid()) continue;
        if ((op.type == wal::WALOpType::BACKUP || op.type == wal::WALOpType::REMOVE_OLD) &&
            !op.arg2.empty())
            baks.push_back(op.arg2);
    }
    const size_t tail_start = (last_commit < 0) ? 0 : static_cast<size_t>(last_commit) + 1;
    for (size_t i = tail_start; i < region_end; ++i) {
        auto op = wal::parse_op(lines[i]);
        if (!op.is_valid()) continue;
        if (op.type == wal::WALOpType::CLEANUP && !op.arg1.empty()) baks.push_back(op.arg1);
    }
    if (baks.empty()) return;

    // 3. 归一为 stash 根再整目录 remove_all（文件备份在 stash 内；CLEANUP 行即 stash 根）
    std::vector<fs::path> roots;
    for (auto& bak : baks) {
        roots.push_back(wal::stash_root_of_bak(bak));
    }
    std::ranges::sort(roots);
    auto last = std::unique(roots.begin(), roots.end());
    roots.erase(last, roots.end());

    // 4. 删除仍存在的 stash 根（幂等：已删的跳过）
    for (const auto& root : roots) {
        // lstat 语义：**任何**占着这个名字的东西（含悬空/自环符号链接）都要 remove_all ——
        // 原来写成 `fs::exists || fs::is_symlink`，那在符号链接环上会抛 filesystem_error
        // （见 base/utils.hpp 的谓词说明），把 post-commit 清理打断。
        if (!exists_no_follow(root)) continue;
        std::error_code ec;
        fs::remove_all(root, ec);
        if (ec) log_warning(string_format("warning.cleanup_failed", root.string()));
    }
}

// ============================================================================
// recover_packages — 断电/崩溃恢复
// ============================================================================

/** 批次里第一个包级 BEGIN/RM_BEGIN 的包名（告警定位用）；没有则空串 */
static std::string first_package_of(const std::vector<wal::WALOp>& ops)
{
    for (const auto& op : ops) {
        if ((op.type == wal::WALOpType::BEGIN || op.type == wal::WALOpType::RM_BEGIN) &&
            !op.arg1.empty())
            return op.arg1;
    }
    return {};
}

/**
 * 反序回滚整段未提交区域 [region_start, EOF)，并做该段的收尾（stash 收尸 / 重载 Cache /
 * 写 COMMIT_PKGS）。返回"是否回滚失败"——调用方据此决定要不要跳过 DB 备份清理。
 *
 * 区域是**一整段**而不是"每批一次"：CLEANUP 只可能出现在事务之外（ARCH.md §11.3，旧版
 * 二进制写入的"批次内 CLEANUP"形状不在支持范围），故未提交批次一律整体逆序回滚。
 * **回滚失败不得卡死整个恢复**：告警、保持现场（WAL 与备份原样保留，下次 rec 可重试），
 * 并把失败上报给调用方；此后不清理 DB 备份（那是重试的依据）。
 */
static bool rollback_uncommitted_region(const std::vector<std::string>& lines, size_t region_start)
{
    // a) 解析操作行
    std::vector<wal::WALOp> ops;
    for (size_t i = region_start; i < lines.size(); ++i) {
        auto op = wal::parse_op(lines[i]);
        if (op.is_valid()) ops.push_back(op);
    }
    if (ops.empty()) return false;

    const std::string first_pkg = first_package_of(ops);
    try {
        wal::reverse_execute(ops, true);
    } catch (const std::exception& e) {
        log_warning(string_format("warning.rollback_remove_failed", first_pkg, e.what()) +
                    " [rec: 该批次保持未提交，可重试]");
        return true;
    }
    wal::purge_consumed_stashes(ops);  // stash 里的文件已还原 → 清空 stash 根
    // 容忍"pkgs/holdpkgs 不存在"：reverse_execute 已把能从备份还原的库都还原了，这里再
    // 撞上一个缺失的库就说明它连备份都没有（真的丢了）—— 一条缺失记录不该把整个恢复
    // 作废（stash 收尸、WAL 收尾、备份清理都还要做），但它**不静默**：load 会逐个告警；
    // 正常操作路径（install/remove 入口的 Cache::load()）仍是硬错误。
    Cache::instance().load(/*tolerate_missing_set_files=*/true);
    wal::commit_batch();
    return false;
}

void recover_packages()
{
    // 0. 读取所有行。"没有可处理的日志"（路径不存在/打不开/为空）→ 没有任何事要做。
    auto maybe_lines = read_wal_lines(wal::wal_log_path());
    if (!maybe_lines || maybe_lines->empty()) return;
    const std::vector<std::string>& lines = *maybe_lines;

    // 1. 续传 post-commit 清理：删除已提交批次残留的 .lpkg_bak。
    //     必须在处理未提交批次之前做，且必须早于任何 trim——一旦 trim 把完成事务
    //     连同其 BACKUP 行清掉，残留 bak 的来源记录就没了。
    continue_post_commit_cleanup(lines);

    // 2. 状态机扫描：未提交区域 = [**第一个**未配对 BEGIN_PKGS, EOF)
    //    （BEGIN_PKGS 开批、COMMIT_PKGS 配对；扫描语义见 scan_batch_pairing）
    const ssize_t unpaired_begin = scan_batch_pairing(lines).unpaired_begin;

    if (unpaired_begin < 0) {
        // 没有未完成的批次，清理整个日志。
        // 同时清理孤儿 .lpkg_db_bak_before：已提交批次崩溃在"COMMIT_PKGS 之后、
        // post-batch cleanup 之前"时留下的备份，此处一并清掉（启动时无活动批次，
        // DBLock 保证单进程，安全）。
        trim_completed();
        cleanup_db_backups();
        return;
    }

    // 3. 一次逆序回滚整段未提交区域
    const bool any_batch_failed =
        rollback_uncommitted_region(lines, static_cast<size_t>(unpaired_begin));

    // 4. 清理残留的 .lpkg_db_bak_before:* 备份文件。
    //    有批次恢复失败时**跳过**：那些备份是重试还原 DB 的唯一依据（TODO.md A3 同理）。
    if (!any_batch_failed) cleanup_db_backups();
}

// ============================================================================
// trim_completed — 清理已完成的 WAL 条目
// ============================================================================

/**
 * "post-commit 清理还没做完" 的判据（I-BAK-2 的 CLEANUP 行写在事务之外；已提交批次的
 * .lpkg_bak 也可能因清理失败/崩溃仍未删除）。
 *
 * trim 只清理"完全完成"的内容：只要有已提交批次的 BACKUP/REMOVE_OLD 目标、或 CLEANUP 行
 * 引用的 bak **仍占着盘上那个名字**，就返回 true（调用方据此**保留整个 WAL**：BACKUP 上下文
 * 还在，recover 才能续传），只有 bak 全删干净了才 false。
 *
 * lstat 语义（exists_no_follow），**不是** `fs::exists || fs::is_symlink`：行里的 bak 路径
 * 可能正是**符号链接环**（原路径是环，被 rename 进 stash 后依然是个环）—— 那条写法在它上面
 * 抛 filesystem_error，于是**每次启动的 trim 都失败**、WAL 永远裁剪不掉（见 base/utils.hpp
 * 的谓词说明）。判据本身不变。
 */
static bool post_commit_cleanup_pending(const std::vector<std::string>& lines)
{
    for (const auto& line : lines) {
        auto op = wal::parse_op(line);
        if (!op.is_valid()) continue;
        if ((op.type == wal::WALOpType::BACKUP || op.type == wal::WALOpType::REMOVE_OLD) &&
            !op.arg2.empty() && exists_no_follow(op.arg2))
            return true;
        if (op.type == wal::WALOpType::CLEANUP && !op.arg1.empty() && exists_no_follow(op.arg1))
            return true;
    }
    return false;
}

/**
 * 把 [keep_from, EOF) 的行重写回 WAL，丢掉前面的已完成批次。
 *
 * 保留下来的未提交批次是恢复数据，必须先 fsync 再 rename（I-FSYNC-5：write 用 .tmp + fsync
 * + rename），否则断电可能丢失恢复点。
 */
static void rewrite_wal_tail(const std::string& wpath, const std::vector<std::string>& lines,
                             size_t keep_from)
{
    std::string tmp_path = wpath + ".trim_tmp";
    {
        std::ofstream out(tmp_path);
        for (size_t i = keep_from; i < lines.size(); ++i) {
            out << lines[i] << "\n";
        }
        out.flush();
        if (!out) throw LpkgException(string_format("error.db_write_failed", tmp_path));
    }
    {
        int fd = ::open(tmp_path.c_str(), O_WRONLY);
        if (fd >= 0) {
            ::fsync(fd);
            ::close(fd);
        }
    }

    safe_rename(tmp_path, wpath);
}

void trim_completed()
{
    const std::string wpath = wal::wal_log_path();
    auto maybe_lines = read_wal_lines(wpath);  // 判定不抛；见 base/utils.hpp 的谓词说明
    if (!maybe_lines) return;

    const std::vector<std::string>& lines = *maybe_lines;
    if (lines.empty()) {
        // 空文件 → 删除
        std::error_code ec;
        fs::remove(wpath, ec);
        return;
    }

    // 找**第一个**未配对 BEGIN_PKGS 的位置（= 需保留区域的起点）。
    // 语义（含"为什么必须是第一个"）与 continue_post_commit_cleanup、recover_packages
    // 完全一致 —— 三处共用 scan_batch_pairing 这一份记账。
    const ssize_t unpaired_begin = scan_batch_pairing(lines).unpaired_begin;

    if (unpaired_begin < 0) {
        // 所有事务都已提交 —— 但**不无条件清空**：先确认 post-commit 清理没有留下未删的 bak。
        if (post_commit_cleanup_pending(lines))
            return;  // 清理未完成 → 保留整个文件，等 recover 续传
        // 清理完成 → 清空整个日志文件（全是完成事务/历史清理记录）
        std::ofstream(wpath, std::ios::trunc).close();
        return;
    }

    // 保留从 unpaired_begin 开始的所有行
    if (unpaired_begin == 0) {
        return;  // 没有需要清理的已完成批次
    }
    rewrite_wal_tail(wpath, lines, static_cast<size_t>(unpaired_begin));
}

// ============================================================================
// cleanup_db_backups — 清理孤立的 .lpkg_db_bak_before:* 文件
// ============================================================================

namespace
{
/**
 * WAL 里是否还留着未配对的 BEGIN_PKGS（= 存在未提交批次）。
 *
 * 读不到 WAL 文件时保守返回 true（"有"）——宁可让备份多留一会儿，也不误删唯一还原点。
 * 文件不存在 / 为空则明确是"没有"（正常路径：trim 之后）。
 *
 * 配对判定复用 scan_batch_pairing（唯一实现），**行读取与规范化也复用 read_wal_lines**
 * （同一个唯一实现）。
 *
 * ⚠️ **订正 2026-09-26：原先这里刻意"原样收集行、不剥 \r"，理由是"守卫的答案不该取决于行的
 * 规范化"。实测表明那个理由**站反了** —— 不剥 \r 给出的答案在**危险的那一侧**：
 *
 *   · `BEGIN_PKGS` / `COMMIT_PKGS` 是**裸行**（`begin_batch()` → `w.log("BEGIN_PKGS")`、
 *     `commit_batch()` → `log_wal_line("COMMIT_PKGS")`，**不带任何载荷**）。
 *   · 于是 CRLF 行的 `\r` 粘在**类型 token** 上：`parse_op("BEGIN_PKGS\r")` 先按第一个空格切
 *     类型（这里压根没有空格）→ `walop_type_from_name("BEGIN_PKGS\r")` 未知 → **INVALID**
 *     （已有用例 `test_wal_core.cpp::ParseOpWithTrailingWhitespace` 钉住这条）。
 *   · 配对扫描只认 BEGIN/COMMIT 类型 ⇒ 整份 CRLF WAL 里**一个批次都看不见** ⇒
 *     `unpaired_begin = -1` ⇒ 守卫判"没有未提交批次" ⇒ **把唯一还原点删光**。
 *     而同一份 WAL 上其余三个消费者（recover / trim / continue_post_commit_cleanup）走
 *     read_wal_lines、剥了 \r，看到的是"有未提交批次" —— 同一份输入，两个相反的答案。
 *
 * 可达性：lpkg 只写 `\n`（`log_wal_line` 恒拼 `"\n"`）⇒ 今天不可达。但守卫存在的**全部意义**
 * 就是"拿不准时偏保守"，让它在一份被外部工具/编辑器改成 CRLF 的 WAL 上反转成"删备份"，
 * 与它的既定偏向正好相反。剥 `\r` 只会让它**更保守**（多留备份），同向。
 *
 * 注意：仅当类型后面**没有载荷**时 `\r` 才会粘到类型 token 上。生产写入的正是裸形态，
 * 所以这里必须用裸形态复现（部分既有用例写的是 `BEGIN_PKGS 1` 这种带载荷的形态 ——
 * 那种形态下 `\r` 落在 arg1 里，类型照样解析成功，**测不出这个缺陷**）。
 */
bool wal_has_unpaired_batch()
{
    const std::string path = wal::wal_log_path();
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    const auto lines = read_wal_lines(path);
    // lines == nullopt 把两种情形合在了一起（不存在 / 打不开），用 exists 拆开：
    // 不存在 → "没有"（正常路径：trim 之后）；存在却打不开 → 保守判"有"。
    if (!lines) return exists;
    return scan_batch_pairing(*lines).unpaired_begin >= 0;
}
}  // namespace

void cleanup_db_backups()
{
    // **有未提交批次时一律不清理**：*.lpkg_db_bak_before:* 是重试还原 DB 的**唯一**依据。
    //
    // 守卫放在本函数而不是各调用点，是因为调用点有三个而此前只有一个带守卫：
    //   ① recover_packages() 自己判 any_batch_failed 后跳过 —— 唯一正确的那处；
    //   ② main.cpp 的 `lpkg rec` 分支：recover_packages() 之后**无条件**再调一次，
    //      于是"恢复失败"时刚被特意保留的还原点立刻被删光，CLI 还照打"恢复完成"；
    //   ③ finish_committed_batch()：任何一次成功提交都会把**上一个未配对批次**的重试
    //      依据一并扫掉（trim_completed 只删已配对块，未配对区域仍在，但备份没了）。
    // 后果是"文件被逆向还原了、DB 却停在已安装"的不可恢复状态。
    if (wal_has_unpaired_batch()) return;

    std::error_code ec;
    // 递归扫描 DBRM 创建的备份。除 state_dir（deps/、needed_so/ 等子目录）外，
    // man 备份由 write_string_file_wal 写在 docs/ 目录（state_dir 之外），
    // 漏扫会导致每次安装/升级都残留 *.man.lpkg_db_bak_before:* 文件。
    for (const fs::path& base : {Config::instance().state_dir(), Config::instance().docs_dir()}) {
        // 判定不抛（ELOOP 会让 fs::exists 抛，见 base/utils.hpp 的谓词说明）；仍用**跟随**
        // 语义（原来是 exists + is_directory），状态目录可以是符号链接。
        if (!is_directory_follow(base)) continue;

        for (const auto& entry : fs::recursive_directory_iterator(base, ec)) {
            if (ec) break;

            const std::string fname = entry.path().filename().string();
            if (fname.find(".lpkg_db_bak_before:") != std::string::npos) {
                fs::remove(entry.path(), ec);
            }
        }
        ec.clear();
    }
}
