/**
 * test_upgrade_decision_table.cpp — 升级路径**决策表**（逐格见 `ARCH.md` §6.3）
 *
 * ── 这一层要解决什么 ─────────────────────────────────────────────────────────
 * 一次升级分三趟：② `backup_existing_files()`（让开）、③ `copy_package_files()`（写入）、
 * ⑤ `commit_without_file_ops()`（注册 + 废弃清除）。**"同一个路径该由哪一趟处理"原先只写
 * 在三处的注释里**，于是长出过漏格：第②步曾写"仅真正的目录由目录逻辑处理、跳过"，而"目录
 * 逻辑"是跑在**拷贝之后**的第⑤步 —— "盘上是真目录、新条目是文件/符号链接"那一格没人让开，
 * `rename` 撞 EISDIR，整条升级路径被预检拒掉（见 lpkg/CLAUDE.md §1.2）。
 *
 * 第②步把"路径 → 动作"抽成**一个**函数 `detail::decide_path()`（`main/src/pkg/op_sink.hpp`），
 * 三趟都只经它决定"归谁、做什么"，并在入口处检查后置条件（**恰好认领一次**）。
 *
 * ── 本文件的两半 ─────────────────────────────────────────────────────────────
 *   A. **模型层**（`DecisionTableModelTest`）：直接驱动 `decide_path()`，穷举事实空间 +
 *      黄金表（§3.2 逐行对照）+ 合理性不变量。**"恰好认领一次"的覆盖性断言在这里** ——
 *      它是纯函数，能穷举；而运行时那次检查只能覆盖"当次真的走过的格子"。
 *   B. **端到端**（`UpgradeDecisionTableTest`）：一个包 + 一次升级把 §3.2 的多行**一起**
 *      走一遍，断言每一行的盘面落点与决策表说的一致（模型说对了、盘面也要对）。
 *
 * ── `/etc` 的 dir→非目录 两腿（2026-09-25 修）────────────────────────────────
 * 这两格曾经**是坏的**：让开趟对 `/etc` 非目录条目一律早退（那条早退排在"真目录挡路"
 * 分支之前），盘上挡路的真目录没人让开 —— 普通文件条目 rename 撞 EISDIR、符号链接条目
 * 被 `error.dir_replaced_by_symlink` 守卫拒绝，升级**永远不可能成功**。
 *
 * 第②步（本文件落地的那次重构）**如实记录**了这个现状（黄金表里标【现状】两行、
 * 合理性不变量把那两格排除）；紧接着的第②步之后的一次**独立**改动修掉了它：
 * `/etc` 的非目录条目撞真目录时，让开趟先 `save_config` 把整树改名成 `<路径>.lpkgsave/`，
 * 写入趟再**就地**落位（普通文件与符号链接都是就地，**不**落 `.lpkgnew`；落点差异见
 * `ARCH.md` §6.3 的落点规则）。黄金表里那两行现在是**正常行**，合理性不变量不再排除任何格子。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/op_sink.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

using detail::ConfigDisposition;
using detail::PathAction;
using detail::PathDecision;
using detail::PathFacts;
using detail::PathPass;

namespace
{

// ============================================================================
// A. 模型层：事实空间 + 决策表
// ============================================================================

/// 事实空间的一格。**只有物理上可能的事实组合**进入枚举（`disk_is_dir` 蕴含 `disk_exists`）。
struct FactCell {
    bool is_config = false;
    bool entry_is_dir = false;
    bool entry_is_symlink = false;
    bool disk_exists = false;
    bool disk_is_dir = false;
    ConfigDisposition cfg = ConfigDisposition::InstallNew;
    // 第⑤趟（DB 旧键）才用到的事实
    bool obsolete = false;
    bool last_owner = false;
    bool new_dir_entry = false;
    /// 盘上那份是**符号链接**（2026-09-26 新增）。
    /// ⚠️ **必须留在最后**：本结构到处用**位置初始化**（`{true, false, true, true, false}`），
    /// 往中间插一个字段会让下面每一条 case 静默错位、而且错得很像"表算错了"。
    bool disk_is_symlink = false;
};

const char* cfg_name(ConfigDisposition c)
{
    switch (c) {
        case ConfigDisposition::InstallNew:
            return "InstallNew";
        case ConfigDisposition::KeepLocal:
            return "KeepLocal";
        case ConfigDisposition::SaveLpkgnew:
            return "SaveLpkgnew";
    }
    return "?";
}

const char* action_name(PathAction a)
{
    switch (a) {
        case PathAction::Unclaimed:
            return "Unclaimed";
        case PathAction::Noop:
            return "Noop";
        case PathAction::Stash:
            return "Stash";
        case PathAction::StashObsolete:
            return "StashObsolete";
        case PathAction::SaveConfig:
            return "SaveConfig";
        case PathAction::MakeDir:
            return "MakeDir";
        case PathAction::StashAndMkDir:
            return "StashAndMkDir";
        case PathAction::SaveConfigAndMkDir:
            return "SaveConfigAndMkDir";
        case PathAction::RegisterNew:
            return "RegisterNew";
        case PathAction::WriteInPlace:
            return "WriteInPlace";
        case PathAction::WriteLpkgnew:
            return "WriteLpkgnew";
        case PathAction::KeepOnDisk:
            return "KeepOnDisk";
        case PathAction::WriteDirMetadata:
            return "WriteDirMetadata";
        case PathAction::DropOwnership:
            return "DropOwnership";
        case PathAction::SaveConfigObsolete:
            return "SaveConfigObsolete";
        case PathAction::RemoveDir:
            return "RemoveDir";
    }
    return "?";
}

/// 一格的**可复现摘要**（失败信息里必须能一眼看出是哪一格）
std::string describe(const FactCell& c, bool in_archive)
{
    std::string s = in_archive ? "归档条目 " : "DB 旧键 ";
    s += c.is_config ? "/etc " : "非-/etc ";
    s += c.entry_is_dir ? "条目=目录 " : (c.entry_is_symlink ? "条目=符号链接 " : "条目=普通文件 ");
    s += "盘上=";
    s += !c.disk_exists
             ? "无"
             : (c.disk_is_dir ? "真目录" : (c.disk_is_symlink ? "符号链接" : "普通文件"));
    s += " cfg=";
    s += cfg_name(c.cfg);
    if (!in_archive) {
        s += c.obsolete ? " 废弃" : " 仍提供";
        s += c.last_owner ? " 最后持有者" : " 另有持有者";
        if (c.new_dir_entry) s += " 新版登记目录条目";
    }
    return s;
}

PathFacts facts_of(const FactCell& c, bool in_archive)
{
    PathFacts f;
    f.logical = c.is_config ? "/etc/probe" : "/usr/share/probe";
    if (c.entry_is_dir) f.logical += "/";
    f.in_archive = in_archive;
    f.is_config = c.is_config;
    f.entry_is_dir = c.entry_is_dir;
    f.entry_is_symlink = c.entry_is_symlink;
    f.disk_exists = c.disk_exists;
    f.disk_is_dir = c.disk_is_dir;
    f.disk_is_symlink = c.disk_is_symlink;
    f.cfg = c.cfg;
    f.obsolete = c.obsolete;
    f.last_owner = c.last_owner;
    f.new_dir_entry = c.new_dir_entry;
    return f;
}

/// 穷举事实空间（`disk_is_dir` 蕴含 `disk_exists`；目录条目不会是符号链接）
template <typename F>
void for_each_cell(F&& visit)
{
    const ConfigDisposition cfgs[] = {ConfigDisposition::InstallNew, ConfigDisposition::KeepLocal,
                                      ConfigDisposition::SaveLpkgnew};
    for (int is_config = 0; is_config <= 1; ++is_config)
        for (int entry_is_dir = 0; entry_is_dir <= 1; ++entry_is_dir)
            for (int entry_is_symlink = 0; entry_is_symlink <= 1; ++entry_is_symlink)
                for (int disk_exists = 0; disk_exists <= 1; ++disk_exists)
                    for (int disk_is_dir = 0; disk_is_dir <= 1; ++disk_is_dir)
                        for (int disk_is_symlink = 0; disk_is_symlink <= 1; ++disk_is_symlink)
                            for (const auto& cfg : cfgs)
                                for (int obsolete = 0; obsolete <= 1; ++obsolete)
                                    for (int last_owner = 0; last_owner <= 1; ++last_owner)
                                        for (int new_dir_entry = 0; new_dir_entry <= 1;
                                             ++new_dir_entry) {
                                            if (disk_is_dir && !disk_exists) continue;
                                            // 目录条目不可能是符号链接（scan_content_files 的形态）
                                            if (entry_is_dir && entry_is_symlink) continue;
                                            // 盘上那份是符号链接 ⇒ 它必须存在，且"真目录"与
                                            // "符号链接"互斥（lstat 语义，见 PathFacts）
                                            if (disk_is_symlink && (!disk_exists || disk_is_dir))
                                                continue;
                                            FactCell c;
                                            c.is_config = is_config;
                                            c.entry_is_dir = entry_is_dir;
                                            c.entry_is_symlink = entry_is_symlink;
                                            c.disk_exists = disk_exists;
                                            c.disk_is_dir = disk_is_dir;
                                            c.disk_is_symlink = disk_is_symlink;
                                            c.cfg = cfg;
                                            c.obsolete = obsolete;
                                            c.last_owner = last_owner;
                                            c.new_dir_entry = new_dir_entry;
                                            visit(c);
                                        }
}

/**
 * **恰好认领一次**（`ARCH.md` §5.4 不变量 1）——「恰好」的可执行形式。
 *
 * 逐个事实组合断言：
 *   · 归档条目（新版本会碰的路径）→ **让开趟与写入趟各认领恰一次**（可以是 `Noop`：
 *     "考虑过并决定不动"也是认领），登记趟不得染指；
 *   · DB 旧键（新版本不再发的路径）→ **登记趟认领恰一次**，让开/写入两趟不得染指。
 *
 * "无重复"由 `PathDecision` 的字段结构保证（一趟一个字段，不可能塞两个动作）；
 * "无遗漏"就是下面这几条 `EXPECT_NE(..., Unclaimed)`。穷举是这套断言的全部价值：
 * 运行时那次检查只能看到"当次真的走过的格子"，而**漏格的定义恰恰是"没人走到"**。
 */
TEST(DecisionTableModelTest, EveryFactCombinationIsClaimedExactlyOnce)
{
    size_t archive_cells = 0, old_key_cells = 0;
    for_each_cell([&](const FactCell& c) {
        {
            PathDecision d;
            ASSERT_NO_THROW(d = detail::decide_path(facts_of(c, /*in_archive=*/true)))
                << "归档条目这一格让 decide_path 抛了（表不该对任何合法事实组合抛）："
                << describe(c, true);
            const std::string ctx = describe(c, true);
            EXPECT_NE(d.let_go, PathAction::Unclaimed)
                << "归档条目的**让开动作**没有认领者（漏格）：" << ctx;
            EXPECT_NE(d.write, PathAction::Unclaimed)
                << "归档条目的**写入动作**没有认领者（漏格）：" << ctx;
            EXPECT_EQ(d.reg, PathAction::Unclaimed)
                << "归档条目被**登记趟**认领了（它跑在写入之后，抢这个路径就是漏格）：" << ctx;
            ++archive_cells;
        }
        {
            PathDecision d;
            ASSERT_NO_THROW(d = detail::decide_path(facts_of(c, /*in_archive=*/false)))
                << "DB 旧键这一格让 decide_path 抛了：" << describe(c, false);
            const std::string ctx = describe(c, false);
            EXPECT_NE(d.reg, PathAction::Unclaimed) << "DB 旧键没有被登记趟认领：" << ctx;
            EXPECT_EQ(d.let_go, PathAction::Unclaimed)
                << "DB 旧键被让开趟认领了（新版本不再发的路径不该由 ② 处理）：" << ctx;
            EXPECT_EQ(d.write, PathAction::Unclaimed)
                << "DB 旧键被写入趟认领了（新版本不再发的路径不该由 ③ 处理）：" << ctx;
            ++old_key_cells;
        }
    });
    // 别让枚举写错变成"一个格子都没跑"的假绿
    // 原始 2^9 × 3(cfg) = 1536，减去三条约束：
    //   `disk_is_dir ⇒ disk_exists`（去掉 1/4）、目录条目不是符号链接（去掉 1/4）、
    //   `disk_is_symlink ⇒ disk_exists ∧ ¬disk_is_dir`（8 种里去掉 3 种 ⇒ 去掉 3/8）。
    // 前两条互不相交于第三条的维度之外，逐维相乘：
    //   16(自由维) × 3(条目维) × 4(盘面三维) × 3(cfg) = 576
    EXPECT_EQ(archive_cells, 576u) << "事实空间的枚举数目变了（改了 for_each_cell 的维数？）";
    EXPECT_EQ(old_key_cells, 576u);
}

/** 取一格的决策（断言消息里带上摘要，失败时能直接复现） */
PathDecision decide_cell(const FactCell& c, bool in_archive, const char* what)
{
    const PathDecision d = detail::decide_path(facts_of(c, in_archive));
    EXPECT_EQ(d.action_of(PathPass::LetGo), d.let_go) << what;
    EXPECT_EQ(d.action_of(PathPass::Write), d.write) << what;
    EXPECT_EQ(d.action_of(PathPass::Register), d.reg) << what;
    return d;
}

/**
 * **黄金表**：`ARCH.md` §6.3 的每一行（含 `/etc` 的落点规则）逐条钉住。
 *
 * 这张表是"决策表 = 文档"的对照物：改了 `decide_path()` 的任何一格，这里必须一起改，
 * 改的时候就会被迫回答"文档是不是也要改"。**两格标了【现状】** —— 它们的值是**今天的行为**
 * 而不是文档承诺的语义（见文件头 ⚠️）。
 */
TEST(DecisionTableModelTest, GoldenTableMatchesArchSection63)
{
    struct Case {
        const char* what;
        FactCell cell;
        bool in_archive;
        PathAction let_go;
        PathAction write;
        PathAction reg;
    };
    const Case cases[] = {
        // ── §3.2 第 1 行：旧有、新也有（同类型）→ 搬进 stash（BACKUP）──────────────
        {"归档普通文件撞盘上普通文件（同类型替换）",
         {false, false, false, true, false},
         true,
         PathAction::Stash,
         PathAction::WriteInPlace,
         PathAction::Unclaimed},
        // ── §3.2 第 2 行：类型变更（file→dir / dir→file）→ 搬进 stash ─────────────
        {"归档目录撞盘上普通文件（file→dir，非 /etc）",
         {false, true, false, true, false},
         true,
         PathAction::StashAndMkDir,
         PathAction::WriteDirMetadata,
         PathAction::Unclaimed},
        {"归档目录撞盘上普通文件（file→dir，/etc）→ save_config 改名保留",
         {true, true, false, true, false},
         true,
         PathAction::SaveConfigAndMkDir,
         PathAction::WriteDirMetadata,
         PathAction::Unclaimed},
        {"归档普通文件撞盘上真目录（dir→file，非 /etc）→ 整树进 stash",
         {false, false, false, true, true},
         true,
         PathAction::Stash,
         PathAction::WriteInPlace,
         PathAction::Unclaimed},
        // §3.2 第 8 行的 `/etc` 腿（§3.2.2 第一行）：整树改名 `<目录>.lpkgsave/`
        // （**不进 stash** —— 进 stash 会在提交后被 remove_all 连内容一起清掉），
        // 路径让开之后新条目**就地**落位、**不**退 `.lpkgnew`。
        {"归档普通文件撞盘上真目录（dir→file，/etc）→ 整树改名 .lpkgsave 后就地落位",
         {true, false, false, true, true},
         true,
         PathAction::SaveConfig,
         PathAction::WriteInPlace,
         PathAction::Unclaimed},
        // ── /etc 的**类型变化**：原物改名 .lpkgsave（2026-09-26 统一）──────────────
        // 改前这一格是"符号链接一律按配置冲突处理"→ 原文件留原样、新链接退 .lpkgnew。
        // 改后与"dir→非目录"、"非目录→dir"统一：**只要类型换了，原物就留副本、新物就位**。
        {"归档符号链接撞盘上普通文件（/etc，file→symlink）→ 原物 .lpkgsave + 就地落链接",
         {true, false, true, true, false},
         true,
         PathAction::SaveConfig,
         PathAction::WriteInPlace,
         PathAction::Unclaimed},
        {"归档普通文件撞盘上符号链接（/etc，symlink→file）→ 原物 .lpkgsave + 就地落文件",
         {true, false, false, true, false, ConfigDisposition::InstallNew, false, false, false,
          true},
         true,
         PathAction::SaveConfig,
         PathAction::WriteInPlace,
         PathAction::Unclaimed},
        // ── /etc 的**类型未变**：才谈"用户改没改过"，仍按三哈希 / .lpkgnew ───────────
        {"归档符号链接撞盘上符号链接（/etc，类型未变）→ 不接管，退 .lpkgnew",
         {true, false, true, true, false, ConfigDisposition::InstallNew, false, false, false, true},
         true,
         PathAction::Noop,
         PathAction::WriteLpkgnew,
         PathAction::Unclaimed},
        // 同上，只是新形态是**符号链接**（与 dir→file 同一落点政策，见 `ARCH.md` §6.3）
        {"归档符号链接撞盘上真目录（dir→symlink，/etc）→ 整树改名 .lpkgsave 后就地落链接",
         {true, false, true, true, true},
         true,
         PathAction::SaveConfig,
         PathAction::WriteInPlace,
         PathAction::Unclaimed},
        {"归档符号链接撞盘上真目录（dir→symlink，非 /etc）→ 整树进 stash 后就地落链接",
         {false, false, true, true, true},
         true,
         PathAction::Stash,
         PathAction::WriteInPlace,
         PathAction::Unclaimed},
        {"归档符号链接撞盘上普通文件（非 /etc）→ 进 stash 后就地落链接",
         {false, false, true, true, false},
         true,
         PathAction::Stash,
         PathAction::WriteInPlace,
         PathAction::Unclaimed},
        // ── 路径本来不存在：只登记 NEW / NEW_DIR ───────────────────────────────────
        {"归档普通文件、盘上无（新文件）",
         {false, false, false, false, false},
         true,
         PathAction::RegisterNew,
         PathAction::WriteInPlace,
         PathAction::Unclaimed},
        {"归档目录、盘上无（新目录）",
         {false, true, false, false, false},
         true,
         PathAction::MakeDir,
         PathAction::WriteDirMetadata,
         PathAction::Unclaimed},
        {"归档目录、盘上已是真目录（只刷元数据）",
         {false, true, false, true, true},
         true,
         PathAction::Noop,
         PathAction::WriteDirMetadata,
         PathAction::Unclaimed},
        // ── §3.2 第 6/7 行：/etc 三哈希的两种"保留"结果 ───────────────────────────
        // 第③步（改执行顺序）之后，这三行的**让开动作**从 `Noop` 变成 `Stash`：让开趟把
        // 盘上那份**先搬进 stash**（这是 `/etc` 配置家族"先搬空再写入"的形态），三哈希的
        // `hash_local` 因此改从 **stash 副本**读（`ARCH.md` §6.3），判为保留时写入趟用
        // `un_stash` 搬回原位 —— 落点与可观测行为**不变**（写入动作那三列一字未动）。
        // 见 `installation_task.cpp` 里让开趟 `Stash` 分支的注释与 `OpSink::un_stash()`。
        {"归档普通文件撞盘上普通文件（/etc）+ 用户没改过 → 静默换新版",
         {true, false, false, true, false, ConfigDisposition::InstallNew},
         true,
         PathAction::Stash,
         PathAction::WriteInPlace,
         PathAction::Unclaimed},
        {"归档普通文件撞盘上普通文件（/etc）+ 包没改这个配置 → 保留用户文件、连 .lpkgnew 都不产生",
         {true, false, false, true, false, ConfigDisposition::KeepLocal},
         true,
         PathAction::Stash,
         PathAction::KeepOnDisk,
         PathAction::Unclaimed},
        {"归档普通文件撞盘上普通文件（/etc）+ 三者互异 → 落 .lpkgnew",
         {true, false, false, true, false, ConfigDisposition::SaveLpkgnew},
         true,
         PathAction::Stash,
         PathAction::WriteLpkgnew,
         PathAction::Unclaimed},
        // ── §3.2 第 3 行：非 /etc 的废弃条目 → 搬进 stash（REMOVE_OLD）─────────────
        {"废弃普通文件（非 /etc、最后持有者、盘上还在）→ REMOVE_OLD 进 stash",
         {false, false, false, true, false, ConfigDisposition::InstallNew, true, true},
         false,
         PathAction::Unclaimed,
         PathAction::Unclaimed,
         PathAction::StashObsolete},
        {"废弃普通文件但盘上已被新版本改成目录 → 不搬（否则刚建好的目录没了）",
         {false, false, false, true, true, ConfigDisposition::InstallNew, true, true},
         false,
         PathAction::Unclaimed,
         PathAction::Unclaimed,
         PathAction::Noop},
        {"废弃普通文件但新版本在 `<路径>/` 登记了目录条目 → 不搬（同上，symlink→dir）",
         {false, false, false, true, false, ConfigDisposition::InstallNew, true, true, true},
         false,
         PathAction::Unclaimed,
         PathAction::Unclaimed,
         PathAction::Noop},
        {"废弃普通文件但还有别的持有者 → 文件留在盘上",
         {false, false, false, true, false, ConfigDisposition::InstallNew, true, false},
         false,
         PathAction::Unclaimed,
         PathAction::Unclaimed,
         PathAction::Noop},
        {"废弃普通文件但盘上已经没有 → 没什么可搬",
         {false, false, false, false, false, ConfigDisposition::InstallNew, true, true},
         false,
         PathAction::Unclaimed,
         PathAction::Unclaimed,
         PathAction::Noop},
        // ── §3.2 第 4 行（2026-09-26 改）：/etc 的废弃**文件/链接** → 改名 .lpkgsave ────
        // 改前"保持原位、只撤所有权"；改后与"类型变化"、移除整包统一 —— /etc 下的东西
        // 永远不会被无声丢掉，也永远不会占着"新版本该用的那个名字"。
        {"废弃 /etc 配置文件 → 改名 .lpkgsave + 撤所有权",
         {true, false, false, true, false, ConfigDisposition::InstallNew, true, true},
         false,
         PathAction::Unclaimed,
         PathAction::Unclaimed,
         PathAction::SaveConfigObsolete},
        {"废弃 /etc 配置目录 → 仍是**只撤所有权、不碰盘**（有意例外：目录不搬不改名）",
         {true, true, false, true, true, ConfigDisposition::InstallNew, true, true},
         false,
         PathAction::Unclaimed,
         PathAction::Unclaimed,
         PathAction::DropOwnership},
        {"新版本仍提供的 /etc 条目 → 登记趟不动它（归 ②③）",
         {true, false, false, true, false, ConfigDisposition::InstallNew, false},
         false,
         PathAction::Unclaimed,
         PathAction::Unclaimed,
         PathAction::Noop},
        // ── 废弃目录：候选 rmdir（真正的 rmdir 另有"真目录 + 此刻为空"守卫）────────
        {"废弃目录（非 /etc、最后持有者）→ DIR_RM 候选",
         {false, true, false, true, true, ConfigDisposition::InstallNew, true, true},
         false,
         PathAction::Unclaimed,
         PathAction::Unclaimed,
         PathAction::RemoveDir},
        {"废弃目录但还有别的持有者 → 保留",
         {false, true, false, true, true, ConfigDisposition::InstallNew, true, false},
         false,
         PathAction::Unclaimed,
         PathAction::Unclaimed,
         PathAction::Noop},
    };

    for (const auto& c : cases) {
        const std::string ctx = std::string(c.what) + "｜" + describe(c.cell, c.in_archive);
        const PathDecision d = decide_cell(c.cell, c.in_archive, ctx.c_str());
        EXPECT_EQ(action_name(d.let_go), action_name(c.let_go)) << "让开动作不符：" << ctx;
        EXPECT_EQ(action_name(d.write), action_name(c.write)) << "写入动作不符：" << ctx;
        EXPECT_EQ(action_name(d.reg), action_name(c.reg)) << "登记动作不符：" << ctx;
    }
}

/**
 * **合理性不变量**：让开动作真的"让开了"。
 *
 *   · `RegisterNew`（无需让开）⇒ 盘上必须**不存在** —— 否则写入趟的 rename 会**没有任何
 *     BACKUP 行**地覆盖掉盘上那份（回滚无从还原）；
 *   · `WriteInPlace`（就地 rename）而盘上是真目录 ⇒ 让开动作必须**真的清路**
 *     （`Stash` / `StashAndMkDir` / `SaveConfigAndMkDir` / `SaveConfig`）。
 *
 * 这一条正是"让开趟必须跑在写入趟之前"的可执行形式：让开动作若被推给跑在拷贝之后的第⑤步，
 * 上面两条立刻破。**全空间都要满足**（`/etc` 的 dir→非目录 两格曾是这个不变量的反例，
 * 2026-09-25 修掉后已归位；见文件头）。
 */
TEST(DecisionTableModelTest, LetGoActuallyClearsTheWay)
{
    size_t checked = 0;
    for_each_cell([&](const FactCell& c) {
        // ── 归档条目侧：让开动作的落点必须与它声称的一致 ─────────────────────────
        {
            const PathDecision d = detail::decide_path(facts_of(c, /*in_archive=*/true));
            const std::string ctx = describe(c, true);
            ++checked;
            if (d.let_go == PathAction::RegisterNew) {
                EXPECT_FALSE(c.disk_exists)
                    << "表说「无需让开、只登记 NEW」，但盘上已有东西 —— 写入趟会无 BACKUP 地"
                       "覆盖它（回滚无从还原）："
                    << ctx;
            }
            // 「就地落位」= 写入趟对一个**真目录**做 rename → 恒 EISDIR。表里唯一能救它的
            // 就是让开趟的动作，所以**让开动作必须真的把路清掉**（三种清路动作用哪个都行）。
            // 这一条是"让开趟必须跑在写入趟之前"的可执行形式：让开若被推给跑在拷贝之后的
            // 第⑤步，`let_go` 就会是 `Noop`，断言立刻破。
            const bool clears_the_way =
                d.let_go == PathAction::Stash || d.let_go == PathAction::StashAndMkDir ||
                d.let_go == PathAction::SaveConfigAndMkDir || d.let_go == PathAction::SaveConfig;
            if (d.write == PathAction::WriteInPlace && c.disk_is_dir) {
                EXPECT_TRUE(clears_the_way)
                    << "表说「就地落位」（rename），而盘上是真目录、让开趟却没清路"
                       "（let_go="
                    << action_name(d.let_go) << "）—— rename 恒 EISDIR：" << ctx;
            }
        }
        // ── DB 旧键侧：废弃清除的三条落点各自的适用条件 ───────────────────────────
        {
            const PathDecision d = detail::decide_path(facts_of(c, /*in_archive=*/false));
            const std::string ctx = describe(c, false);
            if (d.reg == PathAction::StashObsolete) {
                EXPECT_TRUE(c.disk_exists && !c.disk_is_dir)
                    << "表说「把废弃文件搬进 stash」，但盘上不是「存在的非目录」—— 除了 rename "
                       "的错，还会把新版本已建好的目录搬走："
                    << ctx;
            }
            if (d.reg == PathAction::RemoveDir) {
                EXPECT_TRUE(c.entry_is_dir) << "只有目录键才可能被列为 rmdir 候选：" << ctx;
                EXPECT_TRUE(c.last_owner) << "还有别的持有者时不该动那个目录：" << ctx;
                EXPECT_TRUE(c.obsolete) << "新版本仍提供的目录不该被 rmdir：" << ctx;
            }
            if (d.reg == PathAction::DropOwnership) {
                // 2026-09-26 起收紧到**目录**：废弃的 /etc **文件/符号链接**改走
                // SaveConfigObsolete（改名 .lpkgsave），保持原位的只剩目录这一族
                // （有意例外，见 decide_path 里那一格的说明）。
                EXPECT_TRUE(c.is_config && c.obsolete && c.entry_is_dir)
                    << "DropOwnership 只给「废弃的 /etc **目录**」（不碰盘、只撤所有权）：" << ctx;
            }
            if (d.reg == PathAction::SaveConfigObsolete) {
                EXPECT_TRUE(c.is_config && c.obsolete && !c.entry_is_dir)
                    << "SaveConfigObsolete 只给「废弃的 /etc 文件/符号链接」（改名 .lpkgsave）："
                    << ctx;
            }
        }
    });
    EXPECT_EQ(checked, 576u) << "事实空间没被整片检查（枚举写错了？）";
}

}  // namespace

// ============================================================================
// B. 端到端：一个包 + 一次升级，把 §3.2 的多行一起走一遍
// ============================================================================

/**
 * v1 → v2 同时覆盖这些行（同一批次、同一趟里各走一次）：
 *   ① 同类型替换            `usr/share/same.txt`
 *   ② dir → file（非 /etc） `usr/share/f2d/` → `usr/share/f2d`
 *   ③ file → dir（非 /etc） `usr/share/d2s` → `usr/share/d2s/`
 *   ④ 废弃普通文件          `usr/share/gone.txt`
 *   ⑤ 废弃目录（搬空后 rmdir）`usr/share/gonedir/`
 *   ⑥ /etc InstallNew（用户没改过 → 静默换新版）`etc/silent.conf`
 *   ⑦ /etc KeepLocal（包没改这个配置 → 保留用户那份、连 .lpkgnew 都没有）`etc/keep.conf`
 *   ⑧ /etc SaveLpkgnew（三者互异 → 落 .lpkgnew）`etc/newer.conf`
 *   ⑨ /etc 废弃**文件** → 改名 `etc/obsolete.conf.lpkgsave` + 撤所有权（2026-09-26 改；
 *      改前是"保持原位"）
 *   ⑩ /etc 的 **dir → file**  `etc/swap_f/` → `etc/swap_f`
 *      → 整树改名 `etc/swap_f.lpkgsave/`（**不进 stash**，提交后仍在），新文件**就地**落位
 *   ⑪ /etc 的 **dir → symlink** `etc/swap_s/` → `etc/swap_s`
 *      → 同上；新链接**就地**落位（**不**落 `.lpkgnew`，见 `ARCH.md` §6.3）
 *
 * 断言的是**每一行的盘面落点**（模型层说对了不算数，盘面也得对），外加两条全局不变量：
 * 升级必须成功（决策表的后置条件不抛）+ 提交后无 stash 残留。
 */
class UpgradeDecisionTableTest : public IntegrationTestBase
{
protected:
    static void write_file(const fs::path& p, const std::string& content)
    {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << content;
    }

    static std::string read_file(const fs::path& p)
    {
        std::ifstream f(p);
        return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    }

    static std::string shape_of(const fs::path& p)
    {
        std::error_code ec;
        if (fs::is_symlink(p, ec)) return "symlink";
        if (fs::is_directory(p, ec)) return "dir";
        if (fs::is_regular_file(p, ec)) return "file";
        return "absent";
    }

    template <typename F>
    std::string pack(const std::string& name, const std::string& ver, F fill) const
    {
        const fs::path work = suite_work_dir / ("_pkg_" + name + "_" + ver);
        fs::create_directories(work / "content");
        fill(work / "content");
        const std::string path = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(path, work.string(), name, ver, {}, {}, "man " + name, {});
        return path;
    }

    int bak_residue() const
    {
        int n = 0;
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(test_root, ec)) {
            if (ec) break;
            if (e.path().filename().string().find(".lpkg_bak_") != std::string::npos) ++n;
        }
        return n;
    }

    static void fill_v1(const fs::path& c)
    {
        write_file(c / "usr/share/same.txt", "same v1\n");
        write_file(c / "usr/share/f2d/a.txt", "dir a v1\n");
        write_file(c / "usr/share/d2s", "file v1\n");
        write_file(c / "usr/share/gone.txt", "gone v1\n");
        write_file(c / "usr/share/gonedir/g.txt", "g v1\n");
        write_file(c / "etc/keep.conf", "K1\n");
        write_file(c / "etc/newer.conf", "N1\n");
        write_file(c / "etc/silent.conf", "S1\n");
        write_file(c / "etc/obsolete.conf", "O1\n");
        // ⑩⑪ 两个 `/etc` **目录**：新版本分别变成普通文件 / 符号链接
        write_file(c / "etc/swap_f/x.conf", "swap f v1\n");
        write_file(c / "etc/swap_s/y.conf", "swap s v1\n");
    }

    static void fill_v2(const fs::path& c)
    {
        write_file(c / "usr/share/same.txt", "same v2\n");
        // ② dir → file：整个目录树被搬进 stash 让开，新文件就地落位
        write_file(c / "usr/share/f2d", "file v2\n");
        // ③ file → dir：挡路文件进 stash，建目录
        write_file(c / "usr/share/d2s/b.txt", "dir b v2\n");
        // ④/⑤ 不再提供 gone.txt 与 gonedir/
        // ⑥ 用户没改过 → 静默换新版
        write_file(c / "etc/silent.conf", "S2\n");
        // ⑦ 与 v1 **逐字节相同** → hash_orig == hash_pkg → 保留用户那份
        write_file(c / "etc/keep.conf", "K1\n");
        // ⑧ 与 v1、与用户那份都不同 → 三者互异 → 落 .lpkgnew
        write_file(c / "etc/newer.conf", "N2\n");
        // ⑨ 不再提供 etc/obsolete.conf
        // ⑩ /etc 的 dir → file：整树改名 .lpkgsave，新文件就地落位
        write_file(c / "etc/swap_f", "swap f v2\n");
        // ⑪ /etc 的 dir → symlink：同上，新链接就地落位
        fs::create_directories(c / "etc");
        fs::create_symlink("swap_f", c / "etc/swap_s");
    }
};

TEST_F(UpgradeDecisionTableTest, OneUpgradeWalksEveryRowOfTheTable)
{
    const std::string pkg = "dt_all";
    ASSERT_NO_THROW(install_packages({pack(pkg, "1.0", fill_v1)}));
    Cache::instance().load();
    ASSERT_EQ(Cache::instance().get_installed_version(pkg), "1.0");
    ASSERT_EQ(shape_of(test_root / "usr/share/f2d"), "dir");
    ASSERT_EQ(shape_of(test_root / "usr/share/d2s"), "file");

    // 用户改动留痕：/etc 的三条腿靠它区分（InstallNew 要求"没改过"，其余两条要求"改过"），
    // 非 /etc 的两条腿靠它当"旧那份逐字节还原 / 被正确搬走"的锚点。
    write_file(test_root / "usr/share/same.txt", "same user\n");
    write_file(test_root / "usr/share/f2d/a.txt", "dir a user\n");
    write_file(test_root / "etc/keep.conf", "K user\n");
    write_file(test_root / "etc/newer.conf", "N user\n");
    write_file(test_root / "etc/swap_f/x.conf", "swap f user\n");

    const std::string v2 = pack(pkg, "2.0", fill_v2);
    ASSERT_NO_THROW(install_packages({v2}))
        << "决策表某一格判成了「没人认领」或「写不进去」（后置条件在 decide_path 入口就抛）";
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "2.0");

    // ── ① 同类型替换 ────────────────────────────────────────────────────────────
    EXPECT_EQ(read_file(test_root / "usr/share/same.txt"), "same v2\n");
    // ── ② dir → file：盘上目录整树进 stash 让开，新文件就地落位（非 /etc 不留 .lpkgsave）
    EXPECT_EQ(shape_of(test_root / "usr/share/f2d"), "file");
    EXPECT_EQ(read_file(test_root / "usr/share/f2d"), "file v2\n");
    EXPECT_FALSE(fs::exists(test_root / "usr/share/f2d.lpkgsave"))
        << "非 /etc 路径的挡路目录该走 stash，不该留下 .lpkgsave（那是 /etc 配置的语义）";
    // ── ③ file → dir ───────────────────────────────────────────────────────────
    EXPECT_EQ(shape_of(test_root / "usr/share/d2s"), "dir");
    EXPECT_EQ(read_file(test_root / "usr/share/d2s/b.txt"), "dir b v2\n");
    // ── ④ 废弃普通文件：搬进 stash，提交后随 stash 一起清掉 ───────────────────────
    EXPECT_EQ(shape_of(test_root / "usr/share/gone.txt"), "absent");
    EXPECT_FALSE(Cache::instance().is_file_owned_by("/usr/share/gone.txt", pkg));
    // ── ⑤ 废弃目录：文件先搬空 → 目录 rmdir ─────────────────────────────────────
    EXPECT_EQ(shape_of(test_root / "usr/share/gonedir"), "absent");
    // ── ⑥ /etc InstallNew：用户没改过 → 静默换新版，不产生 .lpkgnew ───────────────
    EXPECT_EQ(read_file(test_root / "etc/silent.conf"), "S2\n");
    EXPECT_EQ(shape_of(test_root / "etc/silent.conf.lpkgnew"), "absent")
        << "用户没改过的配置是**静默**换新版（pacman add.c 分支①），不该产生 .lpkgnew";
    // ── ⑦ /etc KeepLocal：保留用户那份，**连 .lpkgnew 都不产生** ─────────────────
    EXPECT_EQ(read_file(test_root / "etc/keep.conf"), "K user\n")
        << "包本身没改这个配置（旧记录 == 新包）→ 用户那份必须原样留着";
    EXPECT_EQ(shape_of(test_root / "etc/keep.conf.lpkgnew"), "absent")
        << "没有「新东西」要给用户审阅时不产生 .lpkgnew（否则 /etc 越堆越多）";
    // ── ⑧ /etc SaveLpkgnew：三者互异 → 新版落 .lpkgnew，用户那份留原位 ──────────
    EXPECT_EQ(read_file(test_root / "etc/newer.conf"), "N user\n")
        << "用户改过的配置**永不**被静默覆盖（`ARCH.md` §5.4 不变量 4）";
    EXPECT_EQ(read_file(test_root / "etc/newer.conf.lpkgnew"), "N2\n")
        << "新版应落在 .lpkgnew 里等用户审阅";
    // ── ⑨ /etc 废弃**文件**：改名 .lpkgsave + 撤所有权（2026-09-26 改；改前保持原位）──
    EXPECT_EQ(shape_of(test_root / "etc/obsolete.conf"), "absent")
        << "废弃配置不该再占着原路径（它已经不是新版本的一部分）";
    EXPECT_EQ(read_file(test_root / "etc/obsolete.conf.lpkgsave"), "O1\n")
        << "用户那份必须留成 .lpkgsave（既不是静默留在原位、更不是删掉）";
    EXPECT_FALSE(Cache::instance().is_file_owned_by("/etc/obsolete.conf", pkg))
        << "废弃条目的所有权必须已撤销（否则它会一直「属于」这个包）";
    // ── ⑩ /etc 的 dir → file：整树改名叫 .lpkgsave（内容一个不丢），新文件就地落位 ──
    EXPECT_EQ(shape_of(test_root / "etc/swap_f"), "file");
    EXPECT_EQ(read_file(test_root / "etc/swap_f"), "swap f v2\n");
    EXPECT_EQ(shape_of(test_root / "etc/swap_f.lpkgsave"), "dir")
        << "被占位的 /etc 目录必须**整树**改名保留成 .lpkgsave（是目录、内容一个不丢）——"
           "**不能进 stash**：stash 在提交后被 remove_all，用户那份配置会无声蒸发";
    EXPECT_EQ(read_file(test_root / "etc/swap_f.lpkgsave/x.conf"), "swap f user\n")
        << ".lpkgsave 里必须是用户那份配置（逐字节）";
    EXPECT_EQ(shape_of(test_root / "etc/swap_f.lpkgnew"), "absent")
        << "目录让开后新条目**就地**落位，不该退到 .lpkgnew";
    // ── ⑪ /etc 的 dir → symlink：同一落点政策，新链接就地落位 ─────────────────────
    EXPECT_EQ(shape_of(test_root / "etc/swap_s"), "symlink");
    EXPECT_EQ(fs::read_symlink(test_root / "etc/swap_s").string(), "swap_f");
    EXPECT_EQ(shape_of(test_root / "etc/swap_s.lpkgsave"), "dir");
    EXPECT_EQ(read_file(test_root / "etc/swap_s.lpkgsave/y.conf"), "swap s v1\n");
    EXPECT_EQ(shape_of(test_root / "etc/swap_s.lpkgnew"), "absent")
        << "目录让开后新链接**就地**落位，不该退到 .lpkgnew";
    // ── 全局：提交后无 stash 残留 ───────────────────────────────────────────────
    EXPECT_EQ(bak_residue(), 0) << "root 下仍留有 .lpkg_bak_* 残留";
}

/**
 * 回滚维度：同一批路径上的**中途失败**必须把每一行都逐字节还原 —— 决策表说"搬进 stash"
 * 的那些让开动作都必须是可回滚的（`BACKUP`），`/etc` 的"保持原位"那格则**根本没碰过盘**。
 *
 * 断点 `copy_after_wal_<pkg>` 挂在普通文件的 COPY 上（符号链接/目录分支今天没有断点，
 * 见 lpkg/CLAUDE.md §2），v2 里 `usr/share/same.txt` 必然走到它。
 */
TEST_F(UpgradeDecisionTableTest, InterruptedUpgradeRestoresEveryRow)
{
    const std::string pkg = "dt_rollback";
    ASSERT_NO_THROW(install_packages({pack(pkg, "1.0", fill_v1)}));
    Cache::instance().load();

    write_file(test_root / "usr/share/same.txt", "same user\n");
    write_file(test_root / "usr/share/f2d/a.txt", "dir a user\n");
    write_file(test_root / "etc/keep.conf", "K user\n");
    write_file(test_root / "etc/newer.conf", "N user\n");
    write_file(test_root / "etc/swap_f/x.conf", "swap f user\n");

    const std::string v2 = pack(pkg, "2.0", fill_v2);
    bool bp_hit = false;
    BreakpointManager::instance().set("copy_after_wal_" + pkg, [&bp_hit] {
        bp_hit = true;
        throw LpkgException("injected copy failure");
    });
    EXPECT_THROW(install_packages({v2}), LpkgException);
    BreakpointManager::instance().clear_all();
    EXPECT_TRUE(bp_hit) << "断点没命中：本用例没考到「让开已发生、写入未完成」的中间态";

    // 每个被让开的路径必须逐字节回到批次前（含**用户改过**的那份）
    EXPECT_EQ(shape_of(test_root / "usr/share/f2d"), "dir") << "被搬走的整棵目录没搬回来";
    EXPECT_EQ(read_file(test_root / "usr/share/f2d/a.txt"), "dir a user\n");
    EXPECT_EQ(shape_of(test_root / "usr/share/d2s"), "file") << "挡路文件没搬回来";
    EXPECT_EQ(read_file(test_root / "usr/share/d2s"), "file v1\n");
    EXPECT_EQ(read_file(test_root / "usr/share/same.txt"), "same user\n");
    EXPECT_EQ(read_file(test_root / "usr/share/gone.txt"), "gone v1\n")
        << "回滚后必须逐字节回到批次前（`ARCH.md` §5.4 不变量 3）";
    EXPECT_EQ(read_file(test_root / "usr/share/gonedir/g.txt"), "g v1\n");
    // /etc：三条腿"不碰盘"的那两条必须**一个字都没动**，InstallNew 那条要还原
    EXPECT_EQ(read_file(test_root / "etc/keep.conf"), "K user\n");
    EXPECT_EQ(read_file(test_root / "etc/newer.conf"), "N user\n");
    EXPECT_EQ(shape_of(test_root / "etc/newer.conf.lpkgnew"), "absent")
        << "回滚必须撤掉这次批次造出来的「请审阅」副本";
    EXPECT_EQ(read_file(test_root / "etc/silent.conf"), "S1\n");
    EXPECT_EQ(read_file(test_root / "etc/obsolete.conf"), "O1\n");
    EXPECT_EQ(shape_of(test_root / "etc/obsolete.conf.lpkgsave"), "absent")
        << "回滚必须撤销废弃条目那次改名（原名原样回来）";
    // /etc 的 dir→非目录 两腿：save_config 的整树改名必须被撤销（目录与用户那份都回来）
    EXPECT_EQ(shape_of(test_root / "etc/swap_f"), "dir") << "回滚必须撤销 save_config 的整树改名";
    EXPECT_EQ(read_file(test_root / "etc/swap_f/x.conf"), "swap f user\n");
    EXPECT_EQ(shape_of(test_root / "etc/swap_f.lpkgsave"), "absent")
        << "回滚必须撤掉这次批次造出来的 .lpkgsave";
    EXPECT_EQ(shape_of(test_root / "etc/swap_s"), "dir");
    EXPECT_EQ(read_file(test_root / "etc/swap_s/y.conf"), "swap s v1\n");
    EXPECT_EQ(shape_of(test_root / "etc/swap_s.lpkgsave"), "absent");
    EXPECT_EQ(bak_residue(), 0) << "回滚后不该留下 stash 残留";
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "1.0");
    EXPECT_TRUE(Cache::instance().is_file_owned_by("/etc/obsolete.conf", pkg))
        << "回滚后所有权必须回到 v1 的状态";
}
