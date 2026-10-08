/**
 * test_wal_dir_meta_strict.cpp — `DIR_META` 行的元数据解析必须**严格**（整串十进制）
 *
 * ## 这个文件补的是什么缺口
 * `wal_op.cpp` 的 `apply_dir_meta()`（匿名命名空间，测试看不见）负责把 `DIR_META` /
 * `NEW_DIR` 行里记的 `<mode> <uid> <gid>` 施加到目录上。它此前用 `std::stoul` 解析，
 * 而 `stoul` 恰好对两类**坏输入**都不抛（于是"解析失败 → 保持 -1 哨兵 = 不改"这条设计
 * 路径形同虚设）：
 *   · `stoul("-1")` **不抛**、按模回绕成 `ULONG_MAX`，`& 07777` 之后正好是 `07777`
 *     （setuid + setgid + sticky + 世界可写）—— 一条写坏的 WAL 行就能把**任意目录**改成
 *     完全开放；
 *   · `stoul("1777junk")` **不抛**，尾随垃圾被静默忽略，取到 `1777`。
 * 现已换成 `parse_decimal_strict`（`std::from_chars` + "整串必须是数字"）。
 *
 * ## 为什么走公开路径
 * `parse_decimal_strict` / `apply_dir_meta` 都在 `wal_op.cpp` 的匿名命名空间里，测试不可直调。
 * 所以手搓一行 `DIR_META <path> <mode> <uid> <gid>`（分帧见 `wal_op.cpp` 顶部注释：
 * tail=3，三个尾字段从右往左切）+ 一个真实目录，跑 `wal::reverse_execute({op})`，
 * 再读回目录的 mode。这是本仓库既有的"手搓 WAL 行 + reverse_execute"写法
 * （`tests/unit/test_undo_table.cpp` 的 `make_op` / `parse_op`，勿自己发明第三套）。
 *
 * ## 断言只落在**盘面**上
 * 坏行按设计是**静默**的（保持哨兵 = 不改、不回滚失败、不告警），所以这里**不**断言日志；
 * 唯一能分辨"修复前 vs 修复后"的观测点就是"目录 mode 有没有被动过"。
 *
 * ## 每条断言"在什么缺陷下会红"
 * 见各 TEST 内注释 —— 核心是：修复前 `-1` 会把目录变成 `07777`、`1777junk` 会变成 `1777`
 * （都以十进制进 `mode_t`），而修复后两者都必须**原样不动**；对照的合法行则**必须**生效，
 * 否则"什么都不做也绿"就成了恒真空转。
 */

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <string>
#include <vector>

#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/wal_op.hpp"

namespace fs = std::filesystem;

class WalDirMetaStrictTest : public ::testing::Test
{
protected:
    fs::path suite_dir;
    fs::path test_root;

    void SetUp() override
    {
        suite_dir = fs::absolute("tmp_wal_dir_meta_strict_test_" + std::to_string(::getpid()));
        if (fs::exists(suite_dir)) fs::remove_all(suite_dir);
        test_root = suite_dir / "root";
        fs::create_directories(test_root);

        // 与 test_undo_table.cpp / test_wal_core.cpp 同一套沙盒：root 指向沙盒 ⇒
        // reverse_execute 的路径 confinement 生效（root != "/"），WAL 行必须落在 root 内。
        Config::instance().set_root_path(test_root.string());
        Config::instance().set_testing_mode(true);
        Config::instance().init_filesystem();
        Cache::instance().load();
    }

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        fs::remove_all(suite_dir);
    }

    fs::path in_root(const std::string& rel) const
    {
        return test_root / rel;
    }

    /// 建一个**真实目录**（DIR_META 的 guard 是 RealDir），并把 mode 钉成 base（默认 0700）
    fs::path make_dir(const std::string& rel, mode_t base = 0700)
    {
        const fs::path d = in_root(rel);
        fs::create_directories(d);
        ::chmod(d.c_str(), base);
        return d;
    }

    /// 目录当前的权限位（只取 07777，与 apply_dir_meta 的上界同一口径）
    static mode_t mode_of(const fs::path& d)
    {
        struct stat st{};
        EXPECT_EQ(::lstat(d.c_str(), &st), 0) << "取证无效：目录不存在或不可 lstat：" << d;
        return static_cast<mode_t>(st.st_mode & 07777);
    }

    /**
     * 手搓一行 `DIR_META <path> <mode> <uid> <gid>` 并跑 reverse_execute。
     * mode/uid/gid 都给**字符串**（而不是整数）：坏行要能表达 `-1` / `1777junk` 这类
     * "不是合法十进制"的字面量 —— 这正是被测的输入。
     */
    wal::RollbackStats run_dir_meta(const fs::path& dir, const std::string& mode_field,
                                    const std::string& uid = "0", const std::string& gid = "0")
    {
        const std::string line =
            "DIR_META " + dir.string() + " " + mode_field + " " + uid + " " + gid;
        const wal::WALOp op = wal::parse_op(line);
        EXPECT_TRUE(op.is_valid()) << "构造的 WAL 行没解析成功：" << line;
        EXPECT_TRUE(op.type == wal::WALOpType::DIR_META) << "行类型不是 DIR_META：" << line;
        return wal::reverse_execute({op});
    }
};

// ============================================================================
// 对照组：**合法行必须真的生效**（否则下面"坏行不改"的绿就是恒真空转）
// ============================================================================

TEST_F(WalDirMetaStrictTest, ValidDecimalModeIsApplied)
{
    const fs::path dir = make_dir("usr/share/valid", 0700);
    ASSERT_EQ(mode_of(dir), static_cast<mode_t>(0700)) << "取证无效：初始 mode 没钉住";

    // WAL 里的 mode 字段是 `st_mode & 07777` 的**十进制**：0750 记作 488（同 test_undo_table）。
    run_dir_meta(dir, "488");

    // 0750 = owner rwx + group r-x。
    // 缺陷下会红：若 apply_dir_meta 的解析/施加被改成 no-op，或"合法值"也落进哨兵分支，
    // mode 会停在 0700 ⇒ 本断言从 0750 变 0700。它就是"防什么都不做也绿"的那一半。
    EXPECT_EQ(mode_of(dir), static_cast<mode_t>(0750))
        << "合法的 mode 十进制必须被解析并 chmod 到目录上";
}

// ============================================================================
// `-1`：修复前 `stoul("-1")` 回绕成 ULONG_MAX、`& 07777` = 07777 → 目录被完全开放
// ============================================================================

TEST_F(WalDirMetaStrictTest, NegativeModeDoesNotOpenUpTheDirectory)
{
    const fs::path dir = make_dir("usr/share/neg", 0700);

    run_dir_meta(dir, "-1");

    // 修复后：`parse_decimal_strict("-1")` 返回 false（from_chars 对无符号不接受负号）
    // → mode 保持 -1 哨兵 → 不 chmod → 目录原样。
    // 缺陷下会红：修复前 mode 被设成 07777（把没有 '-' 前导的负数当模回绕），
    // 下面两条同时翻（EQ 0700 变 07777；NE 07777 同理）。
    EXPECT_EQ(mode_of(dir), static_cast<mode_t>(0700))
        << "含负号的 mode 必须被拒（保持哨兵 = 不改），绝不能被回绕成一个权限值";
    EXPECT_NE(mode_of(dir), static_cast<mode_t>(07777))
        << "07777 = setuid+setgid+sticky+世界可写 —— 这正是 stoul('-1') 回绕出来的危险值";
}

// ============================================================================
// `1777junk`：修复前 `stoul` 静默忽略尾随垃圾、取 1777 → 改掉目录 mode
// ============================================================================

TEST_F(WalDirMetaStrictTest, TrailingJunkModeIsRejected)
{
    const fs::path dir = make_dir("usr/share/junk", 0700);

    run_dir_meta(dir, "1777junk");

    // 修复后：`parse_decimal_strict` 要求 `ptr == last`（整串都是数字）→ false → 不改。
    // 缺陷下会红：修复前 stoul("1777junk") 取到 1777（十进制进 mode_t 即 0o3361），
    // mode 从 0700 变掉 ⇒ 第一条 EXPECT_EQ 翻。
    EXPECT_EQ(mode_of(dir), static_cast<mode_t>(0700))
        << "带尾随垃圾的 mode 必须被整串校验拒掉，而不是静默取前缀数字";
    EXPECT_NE(mode_of(dir), static_cast<mode_t>(1777))
        << "stoul('1777junk') 会静默取到十进制 1777 —— 这条点名那个具体的坏值";
}

// ============================================================================
// 越界大数：合法十进制但 > 07777 → 同样必须保持哨兵（"不改"），不是截断成别的值
// ============================================================================

TEST_F(WalDirMetaStrictTest, OutOfRangeModeIsRejectedNotTruncated)
{
    // ① 能装进 unsigned long long、但 > 07777 的值：from_chars 成功、`v <= 07777` 失败 → 哨兵。
    //    缺陷下会红：若实现漏了那条 `v <= 07777` 上界（或对越界值直接截断 `& 07777`），
    //    100000 & 07777 = 1696（0o3240）⇒ 目录 mode 被改掉，EXPECT_EQ 翻。
    const fs::path d1 = make_dir("usr/share/big1", 0700);
    run_dir_meta(d1, "100000");
    EXPECT_EQ(mode_of(d1), static_cast<mode_t>(0700))
        << "超过 07777 的 mode 必须留哨兵（不改），不得截断成别的权限值";
    EXPECT_NE(mode_of(d1), static_cast<mode_t>(100000 & 07777))
        << "被截断成 100000 & 07777（= 1696）正是'上界没判'的特征";

    // ② 连 unsigned long long 都装不下的更大值：from_chars 返回 result_out_of_range → 哨兵。
    const fs::path d2 = make_dir("usr/share/big2", 0700);
    run_dir_meta(d2, "99999999999999999999999");
    EXPECT_EQ(mode_of(d2), static_cast<mode_t>(0700)) << "溢出 unsigned long long 的值同样必须被拒";
}
