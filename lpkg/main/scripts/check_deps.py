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
            'provides': set(meta.get('provides', []) or []),
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
            if dep not in all_names:
                missing.append((name, dep, info['lpkg']))
            if dep == name:
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
    pmap = {}
    for name, info in packages.items():
        if 'error' in info:
            continue
        for p in info['provides']:
            pmap.setdefault(p, set()).add(name)

    missing_so = []
    for name, info in sorted(packages.items()):
        if 'error' in info:
            continue
        for sn in info['needed_so']:
            if not pmap.get(sn):
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
            if dep in all_names:
                depended.add(dep)

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
            if dep in color:
                dfs(dep, path + [node])
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
