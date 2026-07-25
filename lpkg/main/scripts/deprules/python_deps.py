"""
python_deps — Python 库依赖探测规则

扫描包中所有 .py 文件的 import 语句，通过维护的 import 名 → 系统包名
映射表自动添加 python-* 依赖。同时兜底添加 python 基础依赖。

=== 工作原理 ===

  1. 遍历 content/ 下所有 .py 文件
  2. 用正则提取 import X / from X import Y 语句的顶层模块名
  3. 查 PYTHON_IMPORT_MAP 表 → 添加对应系统包依赖
  4. 有 .py 文件但无已知映射时，兜底添加 python 依赖

=== 扩展映射表 ===

在 PYTHON_IMPORT_MAP 中添加一行即可：
    '顶层模块名': '系统包名',
"""

import os
import re

__rule_name__ = 'python_deps'
__rule_description__ = 'Python import → 系统包依赖映射'

# ---------------------------------------------------------------------------
# Python import → 系统包名映射表
# ---------------------------------------------------------------------------
# key   = Python import 语句中的顶层模块名
# value = LankeOS 系统包名
#
# 例如包中 .py 文件包含:
#   import cairo           → 自动添加 python-cairo 依赖
#   from gi.repository ... → 自动添加 python-gobject 依赖
#
# 当前可用的 Python 系统包:
#   python-cairo, python-gobject
#
# 添加新映射: 在下方字典中添加一项, 例如:
#   'numpy':  'python-numpy',
#   'PIL':    'python-pillow',
# =========================================================================

PYTHON_IMPORT_MAP = {
    'cairo': 'python-cairo',       # import cairo
    'gi':    'python-gobject',      # from gi.repository import Gtk, Gdk, Gio...
}

# 这些包拥有 .py 文件但不应自依赖 python
SELF_PROVIDING_PKGS = frozenset({
    'python',
})

# 正则: 提取 import/from 后的顶层模块名
# 跳过注释行 (#) 和相对导入 (from . ...)
_IMPORT_RE = re.compile(
    r'^(?!\s*#)\s*import\s+(\w+)',
    re.MULTILINE,
)
_FROM_RE = re.compile(
    r'^(?!\s*#)\s*from\s+(\w+)',
    re.MULTILINE,
)


# ---------------------------------------------------------------------------
# 辅助函数
# ---------------------------------------------------------------------------


def _parse_python_imports(filepath):
    """解析 .py 文件中的 import 语句，返回顶层模块名集合。"""
    try:
        with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()
    except OSError:
        return set()

    modules = set()

    # import X, Y, Z  → 提取所有顶层模块名
    for match in _IMPORT_RE.finditer(content):
        rest = match.group(1)
        # 处理同一行多个 import: "import os, sys, cairo"
        for part in rest.split(','):
            mod = part.strip().split()[0]  # 取第一个词（去掉 as 别名）
            mod = mod.split('.')[0]         # 只取顶层模块
            if mod and mod != '__future__':
                modules.add(mod)

    # from X import Y  → 提取 X
    for match in _FROM_RE.finditer(content):
        mod = match.group(1)
        if mod and mod != '__future__':
            modules.add(mod)

    return modules


# ---------------------------------------------------------------------------
# 规则入口
# ---------------------------------------------------------------------------


def rule(scan_result, deps, needed_so, provider_map, context):
    """扫描 .py 文件 import 语句，添加对应包依赖。"""
    pkg_name = scan_result.get('pkg_name', '')
    extract_dir = scan_result.get('extract_dir', '')
    content_dir = os.path.join(extract_dir, 'content')

    if not os.path.isdir(content_dir):
        return

    found_py_file = False
    imported_modules = set()

    for root, _dirs, files in os.walk(content_dir):
        for fname in files:
            if not fname.endswith('.py'):
                continue

            fpath = os.path.join(root, fname)
            if os.path.islink(fpath):
                continue

            found_py_file = True
            imported_modules.update(_parse_python_imports(fpath))

    if not found_py_file:
        return

    # --- ① 映射已知 Python 库依赖 ---
    for module_name in sorted(imported_modules):
        mapped_pkg = PYTHON_IMPORT_MAP.get(module_name)
        if mapped_pkg and mapped_pkg != pkg_name and mapped_pkg not in deps:
            deps[mapped_pkg] = None
            print(f'      ↳ [python_deps] {pkg_name}: import {module_name} → {mapped_pkg}')

    # --- ② 兜底: 有 .py 文件 → 需要 python 解释器 ---
    # (shebang 规则已处理带 #! 的脚本, 这里处理纯库 .py 文件)
    if pkg_name not in SELF_PROVIDING_PKGS:
        if 'python' not in deps:
            deps['python'] = None
