"""
shebang — 脚本解释器依赖探测规则

将 Phase 1 收集的 shebang 原始数据映射为包名依赖，
并对没有 shebang 的文件补充 file(1) 识别。
处理 autoconf @VAR@ 占位符，跳过无效解释器名。
"""

import os
import re
import stat
import subprocess

__rule_name__ = 'shebang'
__rule_description__ = '脚本解释器依赖（shebang + file 识别）'

# ---------------------------------------------------------------------------
# 解释器映射表
# ---------------------------------------------------------------------------

SCRIPT_MAP = {
    'bash':     'bash',
    'sh':       'bash',
    'dash':     'bash',
    'ksh':      'bash',
    'zsh':      'bash',
    'perl':     'perl',
    'python':   'python',
    'python3':  'python',
    'ruby':     'ruby',
    'lua':      'lua',
    'luajit':   'lua',
    'gawk':     'gawk',
    'awk':      'gawk',
    'node':     'nodejs',
    'nodejs':   'nodejs',
    'wish':     'tcl',
    'tclsh':    'tcl',
    'expect':   'expect',
}

FILE_PATTERNS = [
    (re.compile(r'Perl\s*module|Perl5?\s*script', re.I),     'perl'),
    (re.compile(r'Python\s*script', re.I),                    'python'),
    (re.compile(r'Bourne-Again\s+shell\s+script', re.I),      'bash'),
    (re.compile(r'POSIX?\s+shell\s+script', re.I),            'bash'),
    (re.compile(r'Ruby\s*script', re.I),                      'ruby'),
    (re.compile(r'Lua\s*script', re.I),                       'lua'),
    (re.compile(r'awk\s*script', re.I),                       'gawk'),
    (re.compile(r'Tcl\s*script', re.I),                       'tcl'),
]

SCRIPT_EXTENSIONS = frozenset({
    '.pm', '.pl', '.py', '.rb', '.lua', '.tcl', '.sh', '.bash',
    '.cgi', '.fcgi', '.al', '.awk',
})


# ---------------------------------------------------------------------------
# 辅助函数
# ---------------------------------------------------------------------------


def _is_valid_pkg_name(name):
    """检查字符串看起来是否像合法的包名。"""
    return bool(re.match(r'^[a-zA-Z][a-zA-Z0-9._+\-]*$', name))


def _clean_interp(raw):
    """清理原始解释器名，处理 autoconf @VAR@ 占位符等。"""
    if not raw or not isinstance(raw, str):
        return None
    cleaned = raw.strip('@').lower()
    if not _is_valid_pkg_name(cleaned):
        return None
    return cleaned


def _detect_shebang(path):
    """读取 #! 行识别脚本解释器。"""
    try:
        with open(path, 'rb') as f:
            header = f.read(256)
        first_line = header.split(b'\n')[0].decode('utf-8', 'ignore').strip()
        if first_line.startswith('#!'):
            parts = first_line[2:].split()
            if parts:
                interp = os.path.basename(parts[0])
                if interp == 'env' and len(parts) > 1:
                    # 跳过 env 的选项参数（如 -S），找第一个非选项参数
                    interp = next((p for p in parts[1:] if not p.startswith('-')), parts[1])
                clean = _clean_interp(interp)
                if clean is not None:
                    return SCRIPT_MAP.get(clean)
    except (OSError, UnicodeDecodeError):
        pass
    return None


def _detect_file(path):
    """调用 file(1) 识别脚本类型。"""
    try:
        res = subprocess.run(
            ['file', '-b', path],
            capture_output=True, text=True, timeout=5,
        )
        output = res.stdout.strip()
        for pattern, pkg in FILE_PATTERNS:
            if pattern.search(output):
                return pkg
    except (subprocess.TimeoutExpired, OSError):
        pass
    return None


def _should_file_detect(path):
    """是否值得跑 file(1)：仅对可执行文件或已知扩展名。"""
    ext = os.path.splitext(path)[1].lower()
    if ext in SCRIPT_EXTENSIONS:
        return True
    try:
        st = os.lstat(path)
        return bool(st.st_mode & (stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH))
    except OSError:
        return False


# ---------------------------------------------------------------------------
# 规则入口
# ---------------------------------------------------------------------------


def rule(scan_result, deps, needed_so, provider_map, context):
    """将 Phase 1 + 补充扫描的脚本解释器数据映射为包名依赖。"""
    pkg_name = scan_result.get('pkg_name', '')
    script_deps = scan_result.get('script_deps', set())

    # Phase 1 已收集的 script_deps → 通过 SCRIPT_MAP 映射
    for raw_interp in sorted(script_deps):
        clean = _clean_interp(raw_interp)
        if clean is None:
            continue
        mapped = SCRIPT_MAP.get(clean)
        if mapped is None:
            continue
        if mapped != pkg_name and mapped not in deps:
            deps[mapped] = None

    # 补充扫描：file(1) 识别没有 shebang 的脚本文件
    no_file = context.get('no_file_detection', False)
    if not no_file:
        extract_dir = scan_result.get('extract_dir', '')
        content_dir = os.path.join(extract_dir, 'content')
        if os.path.isdir(content_dir):
            for root, dirs, files in os.walk(content_dir):
                for fname in files:
                    fpath = os.path.join(root, fname)
                    if os.path.islink(fpath):
                        continue
                    try:
                        with open(fpath, 'rb') as f:
                            if f.read(4) == b'\x7fELF':
                                continue
                    except OSError:
                        continue

                    interp = _detect_shebang(fpath)
                    if interp:
                        if interp != pkg_name and interp not in deps:
                            deps[interp] = None
                    elif _should_file_detect(fpath):
                        interp = _detect_file(fpath)
                        if interp and interp != pkg_name and interp not in deps:
                            deps[interp] = None
