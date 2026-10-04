#!/usr/bin/env python3
"""
check_deps.py — 检查所有 .lpkg 包的依赖一致性。

检查项：
  1. deps 中引用的包名是否都存在
  2. deps 中是否有自引用
  3. needed_so 中的 SONAME 是否有包提供
  4. 未被依赖的孤包统计
  5. 循环依赖检测
"""

import os
import sys
import json
import tarfile
import argparse
from collections import defaultdict


# ── SONAME 规格（symbol version）的 Python 侧镜像 ──────────────────────────────
#
# **必须与 C++ 侧 `base/so_spec.cpp` 的判据同语义**（那是唯一实现）：
#   · `so_parse()`   ↔ `parse_so_spec()`：宽容解析（畸形 → 整串当裸 SONAME）；
#   · `so_satisfies()` ↔ `so_spec_satisfies()`：**保守**包含（need 带符号版本时，provider
#     必须也声明了符号版本且覆盖它；裸 provider 不算）。
#
# 两侧对着**同一组向量**各自断言（与 `dependency_name_of()` 同一套做法）：
#   · C++   ：tests/unit/test_so_spec.cpp
#   · Python：main/scripts/check_index_conformance.py 的 SO_SPEC_VECTORS
# 改这里之前先看那两组向量 —— 它们就是"镜像没漂移"的证据。
_SO_ASCII = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.+-")


def so_parse(spec):
    """规格串 → (裸 SONAME, 符号版本集合)。合法写法见 lpkg/README.md 的"SONAME 规格"一节。"""
    at = spec.find('@')
    if at < 0:
        return spec, frozenset()
    name, ver = spec[:at], spec[at + 1:]

    def sym_ok(tok):
        return bool(tok) and all(c in _SO_ASCII for c in tok)

    if not name or not ver:
        return spec, frozenset()  # 畸形 → 整串当裸名（与 C++ 的宽容分支一致）
    if ver.startswith('{'):
        if len(ver) < 3 or not ver.endswith('}'):
            return spec, frozenset()
        inner = ver[1:-1]
        if '{' in inner or '}' in inner:
            return spec, frozenset()
        toks = inner.split(',')
        if not all(sym_ok(t) for t in toks):
            return spec, frozenset()
        return name, frozenset(toks)
    if '{' in ver or '}' in ver or not sym_ok(ver):
        return spec, frozenset()
    return name, frozenset([ver])


def so_bare(spec):
    """规格串 → 裸 SONAME（按名建索引/查表用）。"""
    return so_parse(spec)[0]


def so_satisfies(provided, needed):
    """provider 的声明是否满足 need（**保守**语义，与 C++ `so_spec_satisfies()` 逐条一致）。"""
    p_name, p_syms = so_parse(provided)
    n_name, n_syms = so_parse(needed)
    if p_name != n_name:
        return False
    if not n_syms:
        return True  # 裸需求：库在就行
    if not p_syms:
        return False  # 保守：provider 没声明符号版本 ⇒ 拿不出任何符号版本
    return n_syms <= p_syms


def dependency_name_of(line):
    """
    一行 `deps` 元数据 → 依赖**包名**（去掉版本约束）。

    **必须与 C++ 侧 `vercmp/dep_parser.cpp` 的 `dependency_name_of()` 同语义** ——
    那是"从一行依赖串里取包名"的**唯一实现**，本函数是它的 Python 侧镜像。
    两侧对着**同一组向量**各自断言，任一侧漂移都会被抓住：
      · C++   ：tests/unit/test_version.cpp 的
                `DepParser.DependencyNameOfMatchesParseDepStrings`
      · Python：main/scripts/check_index_conformance.py

    本脚本原先按**整串**做集合成员判断，于是**仓库自己测试夹具里就有的**
    `"libB >= 2.0 < 3.0"` 会被报成"缺少名为 `libB >= 2.0 < 3.0` 的包" ——
    一份给出错误答案的检查比没有检查更糟。

    判据（与 C++ 一致）：包名 = **最早出现的**合法运算符之前那段，去两侧空白；
    没有运算符则整串就是包名。
    """
    # 与 C++ 的 `trim_copy` **逐字等价**：它只去**空格与制表符**，外加先剥一个行尾 `\r`
    # —— 别用 `str.strip()`，那个还会吃掉 `\n`/`\v`/`\f`，两侧就不再是同一条判据了。
    s = line[:-1] if line.endswith('\r') else line
    s = s.strip(' \t')
    for i, ch in enumerate(s):
        # 每个合法运算符都以 `<`/`>`/`=`/`!` 开头 —— 找最早出现的那个字符即可
        # （C++ 那边逐位置、按 `>=`,`<=`,`!=`,`==`,`>`,`<`,`=` 的顺序试，
        #   "位置最早优先"这一条与逐字符扫描等价）
        if ch in '<>=!':
            return s[:i].rstrip()
    return s


def read_metadata(lpkg_path):
    """从 .lpkg 中读取 metadata.json（支持 ./ 前缀或无前缀）。"""
    try:
        with tarfile.open(lpkg_path, 'r:*') as tf:
            for candidate in ('./metadata.json', 'metadata.json'):
                try:
                    member = tf.getmember(candidate)
                    return json.loads(tf.extractfile(member).read())
                except (KeyError, json.JSONDecodeError):
                    continue
    except Exception as e:
        return {'error': str(e)}
    return {'error': 'no metadata.json found'}


def main():
    parser = argparse.ArgumentParser(description='Check .lpkg dependency consistency')
    parser.add_argument('directory', help='Directory containing .lpkg files')
    args = parser.parse_args()

    target_dir = os.path.abspath(args.directory)
    lpkg_files = sorted(f for f in os.listdir(target_dir) if f.endswith('.lpkg'))
    if not lpkg_files:
        print(f'No .lpkg files found in {target_dir}')
        sys.exit(1)

    # 读取所有包信息
    packages = {}
    for lpkg in lpkg_files:
        path = os.path.join(target_dir, lpkg)
        meta = read_metadata(path)
        name = meta.get('name', '')
        if 'error' in meta:
            packages[lpkg] = {'lpkg': lpkg, 'error': meta['error']}
            continue
        if not name:
            name = lpkg.rsplit('-', 1)[0] if '-' in lpkg else lpkg.replace('.lpkg', '')
        packages[name] = {
            'lpkg': lpkg,
            'version': meta.get('version', '?'),
            'deps': set(meta.get('deps', []) or []),
            'needed_so': set(meta.get('needed_so', []) or []),
            # 8.0.0 拆分：SONAME 提供者看 **provides_soname**（provides 是虚拟 provider）
            'provides': set(meta.get('provides', []) or []),
            'provides_soname': set(meta.get('provides_soname', []) or []),
        }

    all_names = set(packages.keys())
    print(f'[*] 共 {len(packages)} 个包')
    print()

    # 1) deps 引用存在性
    print('=' * 60)
    print('1️⃣  deps 引用检查')
    print('=' * 60)
    missing = []
    self_refs = []
    for name, info in sorted(packages.items()):
        if 'error' in info:
            continue
        for dep in info['deps']:
            # 取**包名**再比：`deps` 里存的是带约束的原串（`"libB >= 2.0 < 3.0"`），
            # 拿整串去比会把每一条带约束的依赖都误报成"无此包"。
            dep_name = dependency_name_of(dep)
            if dep_name not in all_names:
                missing.append((name, dep_name, info['lpkg']))
            if dep_name == name:
                self_refs.append((name, info['lpkg']))

    if missing:
        print(f'\n   ❌ {len(missing)} 个缺失的依赖引用:')
        for p, d, l in sorted(missing):
            print(f'      {l}: 依赖 "{d}" — 无此包')
    else:
        print('   ✅ 所有 deps 引用都存在')

    if self_refs:
        print(f'\n   ⚠️  {len(self_refs)} 个自引用:')
        for p, l in sorted(self_refs):
            print(f'      {l}: 依赖自身')
    else:
        print('   ✅ 无自引用')

    # 2) SONAME 提供者
    print()
    print('=' * 60)
    print('2️⃣  SONAME 提供者检查')
    print('=' * 60)
    # 按**裸 SONAME** 建表（`X@{A,B}` 与 `X@A` 是同一个库的两个规格），
    # 再用保守包含判据筛 —— 自己精确查表会把"声明了符号版本"判成"无提供者"。
    pmap = {}
    for name, info in packages.items():
        if 'error' in info:
            continue
        for p in info['provides_soname']:
            pmap.setdefault(so_bare(p), []).append((p, name))

    missing_so = []
    for name, info in sorted(packages.items()):
        if 'error' in info:
            continue
        for sn in info['needed_so']:
            cands = pmap.get(so_bare(sn), [])
            if not any(so_satisfies(spec, sn) for spec, _owner in cands):
                missing_so.append((name, sn, info['lpkg']))

    if missing_so:
        print(f'\n   ⚠️  {len(missing_so)} 个无提供者的 SONAME:')
        for p, s, l in sorted(missing_so)[:30]:
            print(f'      {l}: needs "{s}"')
        if len(missing_so) > 30:
            print(f'      ... 还有 {len(missing_so) - 30} 个')
    else:
        print('   ✅ 所有 needed_so 都有提供者')

    # 3) 依赖关系统计
    print()
    print('=' * 60)
    print('3️⃣  依赖关系统计')
    print('=' * 60)
    depended = set()
    for name, info in packages.items():
        if 'error' in info:
            continue
        for dep in info['deps']:
            if dependency_name_of(dep) in all_names:
                depended.add(dependency_name_of(dep))

    core = {'glibc', 'gcc', 'linux', 'bash', 'coreutils', 'systemd', 'filesystem'}
    undepended = sorted(n for n in all_names if n not in depended and n not in core
                        and 'error' not in packages.get(n, {}))
    print(f'   总包数: {len(packages)}')
    print(f'   被依赖: {len(depended)}')
    print(f'   未被依赖（不含核心）: {len(undepended)}')
    if undepended:
        print('   ' + ', '.join(undepended[:15]))
        if len(undepended) > 15:
            print(f'   ... 共 {len(undepended)} 个')

    # 4) 循环依赖
    print()
    print('=' * 60)
    print('4️⃣  循环依赖检测')
    print('=' * 60)

    WHITE, GRAY, BLACK = 0, 1, 2
    color = {n: WHITE for n in all_names if 'error' not in packages.get(n, {})}
    cycles = []

    def dfs(node, path):
        if color.get(node, BLACK) == GRAY:
            ci = path.index(node)
            cycles.append(path[ci:] + [node])
            return
        if color.get(node, BLACK) != WHITE:
            return
        color[node] = GRAY
        for dep in packages.get(node, {}).get('deps', []):
            dep_name = dependency_name_of(dep)
            if dep_name in color:
                dfs(dep_name, path + [node])
        color[node] = BLACK

    for node in sorted(color.keys()):
        if color[node] == WHITE:
            dfs(node, [])

    if cycles:
        print(f'   ⚠️  {len(cycles)} 个循环依赖（部分属实，部分由 shebang 引发）:')
        for c in cycles[:10]:
            print(f'      {" → ".join(c)}')
        if len(cycles) > 10:
            print(f'      ... 还有 {len(cycles) - 10} 个')
    else:
        print('   ✅ 无循环依赖')

    # 汇总
    print()
    print('=' * 60)
    print('📊 汇总')
    print('=' * 60)
    print(f'   包总数:             {len(packages)}')
    print(f'   缺失依赖引用:        {len(missing)}')
    print(f'   自引用:              {len(self_refs)}')
    print(f'   缺失 SONAME 提供者:  {len(missing_so)}')
    print(f'   未被依赖的包:        {len(undepended)}')
    print(f'   循环依赖:            {len(cycles)}')

    if missing or cycles:
        print('\n   ⚠️  需关注的问题')
        sys.exit(1)
    else:
        print('\n   ✅ 依赖图一致')


if __name__ == '__main__':
    main()
