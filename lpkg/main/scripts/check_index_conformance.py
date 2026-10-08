#!/usr/bin/env python3
"""
check_index_conformance.py — 索引格式的**跨语言一致性检查**（Python 侧）

索引格式 `name|ver:hash:deps:provides:needed_so;ver2:…|` 有两份读者：
  · **C++**：`base/utils.cpp` 的 `parse_repo_index_line()`（lpkg 唯一的解析器）
  · **Python**：`lrepo-mgr.py` 的 `parse_aggregated_index()`（`push` 读-改-写整份索引、
    `cleanup` 按索引删文件 —— 读丢一条就会**删掉仍被引用的包文件**）

本脚本与 C++ 侧的 `tests/unit/test_repo_index_conformance.cpp` **对着同一份 fixture、
同一份期望**各自断言。两边都钉在同一个契约上 ⇒ 任一侧漂移都会被其中一边抓住。
（不把 lpkg 的解析器做成 CLI 让 Python 去调，也不在容器里装 python3：那份机器成本
 对一处**当前不可达**的潜在分叉不值得 —— 真实索引里两种分叉形态都是 0 次出现。
 契约靠"同一份期望"维持，而不是靠互相调用。）

用法：`python3 main/scripts/check_index_conformance.py`（退出码非零 = 不一致）
"""

import importlib.util
import pathlib
import sys

# ── 与 C++ 侧逐字相同的 fixture ──────────────────────────────────────────────
# ⚠️ **版本块恰好 6 个字段**、**没有行级 provides**（第 3 个 `|` 段不再定义）。字段数不是 6 的
# 版本块被**跳过**（不是被误读），与 C++ 侧同判据 —— 不做任何兼容读取。
FIXTURE = (
    "# 注释行与空行应被两侧同样忽略\n"
    "\n"
    # ① 常规：6 字段齐全（deps / provides / provides_soname / needed_so）
    "foo|1.0:aaaa:dep1,dep2:libssl:libfoo.so.1:libc.so.6|\n"
    # ② 各字段可为空（尾随空字段照样算一个字段）
    "bar|2.0:bbbb:::libbar.so.2:|\n"
    # ③ 版本块之间用 `;`；同一行两个版本共享包名
    "multi|1.0:aaaa:::libm.so.1:;2.0:bbbb::::libm.so.2|\n"
    # ④ 只有版本号、字段数**不是 6** → **整块跳过**（不做兼容读取）
    "qux|4.0\n"
    # ⑤ SONAME 规格（symbol version）：**花括号里的逗号不是字段分隔符**，两侧都必须原样保留
    "ver|3.0:cccc:::libc.so.6@{GLIBC_2.40,GLIBC_2.39}:libm.so.6@GLIBC_2.2.5|\n"
)

# ── 与 C++ 侧逐字相同的期望 ─────────────────────────────────────────────────
EXPECTED = {
    "foo": {"1.0": {"sha256": "aaaa", "deps": "dep1,dep2", "provides": "libssl",
                    "provides_soname": "libfoo.so.1", "needed_so": "libc.so.6"}},
    "bar": {"2.0": {"sha256": "bbbb", "deps": "", "provides": "",
                    "provides_soname": "libbar.so.2", "needed_so": ""}},
    "multi": {"1.0": {"sha256": "aaaa", "deps": "", "provides": "",
                      "provides_soname": "libm.so.1", "needed_so": ""},
              "2.0": {"sha256": "bbbb", "deps": "", "provides": "",
                      "provides_soname": "", "needed_so": "libm.so.2"}},
    "ver": {"3.0": {"sha256": "cccc", "deps": "", "provides": "",
                    "provides_soname": "libc.so.6@{GLIBC_2.40,GLIBC_2.39}",
                    "needed_so": "libm.so.6@GLIBC_2.2.5"}},
}


def load_lrepo_mgr():
    """`lrepo-mgr.py` 文件名带连字符，import 不了，只能按路径加载。"""
    path = pathlib.Path(__file__).with_name("lrepo-mgr.py")
    spec = importlib.util.spec_from_file_location("lrepo_mgr", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ── 依赖名提取：与 C++ 侧 `vercmp/dep_parser.cpp` 的 `dependency_name_of()` 同语义 ──
# 向量与 `tests/unit/test_version.cpp` 的
# `DepParser.DependencyNameOfMatchesParseDepStrings` **逐条相同**。
# 消费方是 `check_deps.py`（它原先按整串比 ⇒ 每条带约束的依赖都被误报成"无此包"）。
NAME_VECTORS = [
    ("glibc", "glibc"),                       # 无约束
    ("cmake >= 3.20", "cmake"),               # 常规带空格
    ("cmake>=3.20", "cmake"),                 # 约束紧贴包名（纯空白规则在此给出整串）
    ("foo!=1.0", "foo"),                      # `!` 打头的二元运算符
    ("libfoo <= 2.0 >= 1.0", "libfoo"),       # 复合约束
    ("  cmake  ", "cmake"),                   # 前后空白
    ("cmake  >=  3.20", "cmake"),             # 运算符两侧多余空格
    ("ninja", "ninja"),
    ("gcc-libs", "gcc-libs"),
    ("provb>=2.0 <3.0", "provb"),             # 复合且第一段紧贴
    ("provb\r", "provb"),                     # CRLF 尾巴必须剥掉（C++ 的 trim_copy 不碰 \r）
    ("provb>=2.0\r", "provb"),
]


# ── SONAME 规格（symbol version）：与 C++ 侧 `base/so_spec.cpp` 同语义 ──────────
# 向量与 `tests/unit/test_so_spec.cpp` 的真值表**逐条相同**。
# 消费方是 `check_deps.py` 的 `so_satisfies()`（它原先按整串比 ⇒ 带符号版本的声明
# 一律被误报成"无提供者"）。
SO_SPEC_VECTORS = [
    # (provider 声明, need 声明, 期望满足?)
    ("X", "X", True),
    ("X@{A,B}", "X", True),        # 库在就行，声明得更细不打破裸需求
    ("X@{A,B}", "X@A", True),      # 覆盖
    ("X@{A,B}", "X@{A,B}", True),
    ("X@{A,B}", "X@{B,A}", True),  # 集合语义，与书写次序无关
    ("X@{A,B}", "X@{A,C}", False),  # 缺 C
    ("X", "X@A", False),           # **保守语义**：没声明就是没声明
    ("X@A", "X@B", False),
    ("X@{A}", "Y@A", False),       # SONAME 不同
    ("X@{A,B", "X@{A,B", True),    # 畸形 → 整串当裸名 ⇒ 只有逐字相同才算
    ("X", "X@{A,B", False),
]


def check_dependency_name_vectors() -> list:
    """`check_deps.py` 的名提取 vs 同一组向量。返回失败描述列表（空 = 通过）。"""
    path = pathlib.Path(__file__).with_name("check_deps.py")
    spec = importlib.util.spec_from_file_location("check_deps", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    failures = []
    for line, want in NAME_VECTORS:
        have = mod.dependency_name_of(line)
        if have != want:
            failures.append(f"dependency_name_of({line!r})：期望 {want!r}，实得 {have!r}")
    return failures


def check_so_spec_vectors() -> list:
    """`check_deps.py` 的 SONAME 规格判据 vs 同一组向量。返回失败描述列表（空 = 通过）。"""
    path = pathlib.Path(__file__).with_name("check_deps.py")
    spec = importlib.util.spec_from_file_location("check_deps", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    failures = []
    for provided, needed, want in SO_SPEC_VECTORS:
        have = mod.so_satisfies(provided, needed)
        if have != want:
            failures.append(
                f"so_satisfies({provided!r}, {needed!r})：期望 {want}，实得 {have}")
    return failures


def main() -> int:
    mod = load_lrepo_mgr()
    got = mod.RepoManager.parse_aggregated_index(FIXTURE)

    failures = []
    for name, versions in EXPECTED.items():
        if name not in got:
            failures.append(f"包 {name!r} 整条缺失（解析器把它丢了）")
            continue
        for ver, fields in versions.items():
            if ver not in got[name]["versions"]:
                failures.append(f"包 {name!r} 的版本 {ver!r} 缺失（版本块被丢弃）")
                continue
            for k, want in fields.items():
                have = got[name]["versions"][ver].get(k)
                if have != want:
                    failures.append(
                        f"{name} {ver} 的 {k}：期望 {want!r}，实得 {have!r}")
    for name in got:
        if name not in EXPECTED:
            failures.append(f"多出包 {name!r}（真实索引里不该有）")

    failures += check_dependency_name_vectors()
    failures += check_so_spec_vectors()

    if failures:
        print("格式一致性检查 **失败**（Python 侧与 C++ 侧的契约不一致）：")
        for f in failures:
            print("  ·", f)
        print("\n契约在两处，必须同时改：")
        print("  · 索引格式  C++: main/src/base/utils.cpp 的 parse_repo_index_line()")
        print("              Py : main/scripts/lrepo-mgr.py 的 parse_aggregated_index()")
        print("  · 依赖名    C++: main/src/vercmp/dep_parser.cpp 的 dependency_name_of()")
        print("              Py : main/scripts/check_deps.py 的 dependency_name_of()")
        print("  · SONAME 规格 C++: main/src/base/so_spec.cpp 的 parse_so_spec()/so_spec_satisfies()")
        print("              Py : main/scripts/check_deps.py 的 so_parse()/so_satisfies()")
        print("  期望的单一出处: 本文件 + tests/unit/test_repo_index_conformance.cpp")
        print("                          tests/unit/test_version.cpp")
        return 1

    print(f"格式一致性检查通过（索引 {len(EXPECTED)} 个包 / "
          f"{sum(len(v) for v in EXPECTED.values())} 个版本块；"
          f"依赖名 {len(NAME_VECTORS)} 组向量；SONAME 规格 {len(SO_SPEC_VECTORS)} 组向量）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
