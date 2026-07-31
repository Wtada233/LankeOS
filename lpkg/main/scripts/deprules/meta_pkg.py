"""
meta_pkg — 元包（meta-package）保护规则

元包如 base、base-devel 不包含 ELF，手工维护 deps 列表。
此规则检测已知元包，在 gen_deps 覆盖前恢复其手工维护的 deps。
"""

import os
import json

__rule_name__ = 'meta_pkg'
__rule_description__ = '保护元包（base、base-devel）的手工维护依赖不被自动覆盖'

META_PACKAGES = frozenset({'base', 'base-devel'})


def rule(scan_result, deps, needed_so, provider_map, context):
    pkg_name = scan_result.get('pkg_name', '')
    if pkg_name not in META_PACKAGES:
        return

    # 读取原始 metadata.json 中的手工 deps
    extract_dir = scan_result.get('extract_dir', '')
    meta_path = os.path.join(extract_dir, 'metadata.json')
    if not os.path.isfile(meta_path):
        return

    try:
        with open(meta_path, 'r', encoding='utf-8') as f:
            meta = json.load(f)
    except (json.JSONDecodeError, OSError):
        return

    orig_deps = meta.get('deps', [])
    if not orig_deps:
        return

    # 恢复手工 deps（清空自动分析结果，填入原始值）
    deps.clear()
    for d in orig_deps:
        deps[d] = None

    needed_so.clear()

    print(f'      [meta_pkg] {pkg_name}: 已保护 {len(orig_deps)} 个手工依赖')
