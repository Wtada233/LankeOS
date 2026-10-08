#pragma once

#include <string>
#include <string_view>
#include <vector>

// ============================================================================
// SONAME 规格（symbol version）：`libc.so.6` / `libc.so.6@GLIBC_2.40`
//                              / `libc.so.6@{GLIBC_2.40,GLIBC_2.39}`
// ============================================================================
//
// `provides_soname`（本包**导出**）与 `needed_so`（本包**需要**）两个字段共用这套语法。
// 动机：ELF 的符号版本（glibc 的 `GLIBC_2.40`、libstdc++ 的 `GLIBCXX_3.4.x`、Qt 私有 API）
// 是**同一 SONAME 内部**的 ABI 细节 —— 只按裸 SONAME 匹配时，"二进制要求某个符号版本、
// 而提供者拿不出来"这件事**完全看不见**，只能等运行期崩。
//
// **语法**（严格，读入处校验；见 `so_spec_wellformed`）：
//   `名字`                      → 裸 SONAME（不声明任何符号版本）
//   `名字@版本`                 → 单个符号版本
//   `名字@{版本1,版本2,…}`      → 多个符号版本；花括号**只能在末尾、只出现一次、不许嵌套**
//   符号名字符集 = `[A-Za-z0-9_.+-]`（`-` / `+` 也允许，见 `so_spec.cpp::symbol_ok`）；
//   **规格里不许有空白**（见下）。
//
// 不许空白的原因：这两个字段在 `index.txt` 里是**逗号分隔的表**，而整行今天**没有空白
// 字符**（7412 个条目里一个都没有）。允许空格会把这个性质悄悄破坏掉——`wc -w`、
// 无参 `split()`、`awk '{print $k}'` 这些惯用法立刻变味。所以校验直接拒空白，把"零空白"
// 从"碰巧如此"变成**可执行的不变量**。
//
// **逗号怎么办**：花括号里的逗号与字段分隔符是同一个字符，所以切分**必须**花括号感知
// （`split_so_list`）。⚠️ 全仓只有两处切分点（lpkg 的 `repository.cpp`、farm 的
// `graph.rs::split_field`）——两边必须实现**同一条规则**，由两侧共用的 conformance fixture
// （`main/scripts/check_index_conformance.py` + `tests/unit/test_repo_index_conformance.cpp`）
// 钉住。写入者不需要任何改动：元素里带花括号也照 join 不误。

/// 解析结果：裸 SONAME + 符号版本列表（**声明序**保留，去重）。
struct SoSpec {
    std::string soname;                ///< 不含 `@…` 的部分
    std::vector<std::string> symbols;  ///< 符号版本（空 = 裸声明）
    bool bare() const
    {
        return symbols.empty();
    }
};

/**
 * **宽容**解析（判定路径用）：整个规格不合法时，把**整串**当成裸 SONAME（`symbols` 为空），
 * 不抛异常。
 *
 * 为什么宽容：判定路径（求解器池、反向依赖、匹配谓词）会遇到**任意**字符串 —— 那里没有
 * "拒绝"的位置（一条畸形记录不该让整次求解崩掉），而它本来就该落回"整串相等"这个 8.0.0
 * 之前的语义。**严格校验在读入处**（`so_spec_wellformed`），别把两件事混起来。
 */
SoSpec parse_so_spec(std::string_view raw);

/// 裸 SONAME（= `parse_so_spec(raw).soname`）—— 按名建索引 / 查表 / 拼系统库路径时用。
std::string so_spec_key(std::string_view raw);

/**
 * **规范化**回写：裸 → `名字`；单符号 → `名字@V`；多符号 → `名字@{V1,V2}`（符号已排序）。
 *
 * 它的用途只有一个：**灌池时的 id**。`needed_so: X@{B,A}` 与 `provides_soname: X@{A,B}`
 * 必须落在**同一个** id 上，否则"同义不同串"会在池里分成两个能力、永远匹配不上。
 * 注意它**不**用来改写存储（metadata/索引/DB 一律原样保留用户写法）。
 */
std::string format_so_spec(const SoSpec& spec);

/// 语法是否合法（严格；读入处据此拒绝并点名）。规则见文件头。
bool so_spec_wellformed(std::string_view raw);

/**
 * **花括号感知**的逗号切分（索引/元数据里那两个字段的列表）。
 *
 * `libc.so.6@{GLIBC_2.40,GLIBC_2.39},libm.so.6` → 两条，**不是**三条。花括号不配对时退化成
 * 普通逗号切分（此时读入处的严格校验已经先把这一行/这个包拒了，切分不必再报错）。
 * 空片段丢弃、两端空白剥掉（走 `trim_copy`）—— 与它取代的 `split_comma_list` /
 * `split_dep_field` 旧行为逐条一致，所以它是**直接替换**，不是新增一层。
 *
 * 返回 `std::string`（不是 `string_view`）：与两个被替换的函数同型，调用点零改动，也不引入
 * "切出来的视图必须活得比输入久"这类生命周期坑。
 */
std::vector<std::string> split_so_list(std::string_view field);
/**
 * provider 的声明是否满足 need 的声明（**保守**语义，见 ARCH.md）：
 *   · SONAME 必须相同；
 *   · need 裸 → 只要库在就算满足（provider 声明没声明符号版本都行）；
 *   · need 带符号版本 → provider **必须也声明**，且 need 的集合是 provider 的子集。
 *
 * ⚠️ 那条"provider 裸声明不算满足"是**有意**的：今天所有包都是裸的，若裸
 * provider 放行，任何带 `@` 的 need 都会被随便一个 provider 满足 ⇒ 这个特性形同虚设。
 */
bool so_spec_satisfies(std::string_view provided, std::string_view needed);

/**
 * 灌 libsolv 池用的**条目**展开（`solver.cpp` 里每个条目经 `scoped_soname()` 进池）：
 *   · `providing = true`（`provides_soname`）→ `X` + `X@V1` + `X@V2` …
 *     **裸条目一定要有**：否则裸 need 会被一个"声明了符号版本"的 provider 打破；
 *   · `providing = false`（`needed_so`）→ 裸 → `X`；带版本 → **只有** `X@V1…`（**不带** `X`）
 *     —— 这就是上面那条保守语义在池里的表达。
 */
std::vector<std::string> expand_so_spec(std::string_view raw, bool providing);
