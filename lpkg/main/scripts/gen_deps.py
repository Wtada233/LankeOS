#!/usr/bin/env python3
"""
gen_deps.py — Auto-generate dependencies for .lpkg packages.

=== 设计原则 ===

本工具直接替换（而非合并）metadata.json 中的 deps 字段。
原因是：它探测的是运行时真实依赖（ELF DT_NEEDED + 脚本解释器），
扫描结果就是运行时依赖的唯一真相（source of truth）。
任何不在探测结果中的旧 dep 要么是过时的手工元数据、
要么是肉眼维护遗漏的条目——直接覆盖消除了这些噪音。
如果需要保留额外的元数据依赖，请在运行本工具后手动添加。

=== 功能 ===
  • ELF 动态链接分析（pyelftools 优先，回退 readelf）
  • 主版本约束推导（glibc 2.42 → ">= 2.0.0"）
  • 脚本解释器探测（shebang + file(1) 补充）
  • 流水线架构：一次解包，同时扫描 SONAME + NEEDED
  • 并行流水线 + dry-run 模式

=== 版本约束说明 ===

依赖的版本约束提取自**提供者包的版本号**（metadata.json 中的 version
字段的第一个数字段），而非 SONAME 的 ABI 编号。例如：
  glibc 2.42 提供 libc.so.6  → 依赖写为 "glibc >= 2.0.0"
  gcc 16.1.0 提供 libstdc++.so.6 → 依赖写为 "gcc >= 16.0.0"

SONAME 的编号（libc.so.6 中的 6）是 ABI 兼容性标记，与包的发行版本
是两套独立的编号体系——将二者混用会导致荒谬的约束（如 "glibc >= 6.0.0"）。

约束格式 ">= X.0.0" 不是严格区间（会允许 X+1.0.0），但结合 SONAME
的 ABI 隔离在实践中是安全的：如果主版本升级到 X+1，SONAME 会变为
libfoo.so.X+1，产生不同的 NEEDED 条目。
"""

import os
import sys
import json
import re
import argparse
import shutil
import stat
import tempfile
import subprocess
from concurrent.futures import ThreadPoolExecutor, as_completed

# ---------------------------------------------------------------------------
# 权限自提升
# ---------------------------------------------------------------------------
# 本脚本必须以 root 运行，原因有二：
#   1. SUID 文件读取：dbus-daemon-launch-helper 等 SUID 二进制的权限为
#      --s--x--x（仅执行，无读权限），非 root 无法打开读取其 ELF 头。
#   2. 所有者保真：解压后直接 repack 时，如果以非 root 运行，包内所有
#      文件的所有者会被污染为普通用户的 UID（而非原始的 root:root）。
#      此处自动通过 sudo 重新执行，并将子进程的退出码透传给调用方。
#
# 环境变量 LPKG_DEP_GEN_NO_SUDO=1 可跳过此提权，用于无 sudo 环境。
# ---------------------------------------------------------------------------

_ORIGINAL_ARGV = sys.argv.copy()

if os.geteuid() != 0 and not os.environ.get('LPKG_DEP_GEN_NO_SUDO'):
    cmd = ['sudo', '--preserve-env=LPKG_DEP_GEN_NO_SUDO', sys.executable] + sys.argv
    print('[*] 需要 root 权限（SUID 文件读取 + 所有者保真），正在通过 sudo 重新执行...')
    try:
        proc = subprocess.run(cmd)
    except FileNotFoundError:
        print('[*] sudo 不可用，将以普通用户运行（SUID 文件 + 所有者污染可能导致 repack 失败）')
    else:
        sys.exit(proc.returncode)

# ---------------------------------------------------------------------------
# 可用性检测
# ---------------------------------------------------------------------------

try:
    from elftools.elf.elffile import ELFFile
    from elftools.elf.dynamic import DynamicSection
    HAVE_PYELFFTOOLS = True
except ImportError:
    HAVE_PYELFFTOOLS = False

# ---------------------------------------------------------------------------
# 解释器映射表
# ---------------------------------------------------------------------------

# shebang 解释器 → 包名
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

# file(1) 输出模式 → 包名（用于无 shebang 的脚本/模块）
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

# 只有命中这些扩展名的文件才值得跑 file(1)（性能考量）
SCRIPT_EXTENSIONS = frozenset({
    '.pm', '.pl', '.py', '.rb', '.lua', '.tcl', '.sh', '.bash',
    '.cgi', '.fcgi', '.al', '.awk',
})

# ---------------------------------------------------------------------------
# 基础工具函数
# ---------------------------------------------------------------------------


def is_elf(path):
    """纯 Python 检查 ELF magic，不涉及子进程。"""
    try:
        with open(path, 'rb') as f:
            return f.read(4) == b'\x7fELF'
    except OSError:
        return False


def parse_elf_dynamic(path):
    """
    解析 ELF .dynamic 段，一次性返回 (sonames, needed)。

    优先使用 pyelftools（无子进程）；回退到 readelf 子进程。
    """
    sonames = []
    needed = []

    if HAVE_PYELFFTOOLS:
        try:
            with open(path, 'rb') as f:
                elf = ELFFile(f)
                for sec in elf.iter_sections():
                    if isinstance(sec, DynamicSection):
                        for tag in sec.iter_tags():
                            if tag.entry.d_tag == 'DT_NEEDED':
                                needed.append(tag.needed)
                            elif tag.entry.d_tag == 'DT_SONAME':
                                sonames.append(tag.soname)
            return sonames, needed
        except Exception:
            return [], []

    # Fallback: 子进程 readelf
    try:
        res = subprocess.run(
            ['readelf', '-d', path],
            capture_output=True, text=True, timeout=10,
        )
        if res.returncode != 0:
            return [], []
        for line in res.stdout.splitlines():
            m = re.search(r'\(NEEDED\)\s+\[(.*?)\]', line)
            if m:
                needed.append(m.group(1))
            m = re.search(r'\(SONAME\)\s+\[(.*?)\]', line)
            if m:
                sonames.append(m.group(1))
    except (subprocess.TimeoutExpired, OSError):
        pass

    return sonames, needed


def extract_package_major(version_str):
    """
    从包版本号中提取主版本号（第一个数字段）。

    约束来自提供者包的 version（而非 SONAME），因为 SONAME
    的 ABI 版本与包的发行版本是完全独立的两个编号体系：
      glibc 2.42 提供 libc.so.6  → 锁 >= 2.0.0，不是 >= 6.0.0
      gcc 16.1.0 提供 libstdc++.so.6 → 锁 >= 16.0.0，不是 >= 6.0.0

    '2.42'      → '2'
    '16.1.0'    → '16'
    '127'       → '127'
    '' | None   → None
    """
    if not version_str:
        return None
    m = re.match(r'(\d+)', version_str)
    return m.group(1) if m else None


def read_metadata(path):
    """安全地读取并解析 metadata.json。"""
    try:
        with open(path, 'r', encoding='utf-8') as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        return None


# ---------------------------------------------------------------------------
# 脚本解释器探测
# ---------------------------------------------------------------------------


def detect_interpreter_shebang(path):
    """读取 #! 行识别脚本解释器。零子进程开销。"""
    try:
        with open(path, 'rb') as f:
            header = f.read(256)
        first_line = header.split(b'\n')[0].decode('utf-8', 'ignore').strip()
        if first_line.startswith('#!'):
            parts = first_line[2:].split()
            if parts:
                interp = os.path.basename(parts[0])
                # 处理 /usr/bin/env python3
                if interp == 'env' and len(parts) > 1:
                    interp = parts[1]
                return SCRIPT_MAP.get(interp)
    except (OSError, UnicodeDecodeError):
        pass
    return None


def detect_interpreter_file(path):
    """调用 file(1) 识别脚本类型，仅用于无 shebang 的脚本/模块文件。"""
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


def should_file_detect(path):
    """
    判断一个文件是否值得跑 file(1)。
    仅对可执行文件或已知脚本扩展名触发，避免不必要的子进程开销。
    """
    ext = os.path.splitext(path)[1].lower()
    if ext in SCRIPT_EXTENSIONS:
        return True
    try:
        st = os.lstat(path)
        return bool(st.st_mode & (stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH))
    except OSError:
        return False


# ---------------------------------------------------------------------------
# Phase 1：并行解包 + 扫描
# ---------------------------------------------------------------------------


def scan_package(lpkg, target_dir, extract_root):
    """
    Phase 1 工作单元。

    一次解包，一次性扫描所有文件：
      • ELF SONAME（提供者）+ DT_NEEDED（依赖）
      • .so 文件名回退提供者
      • 脚本解释器依赖（shebang + file(1) 补充）

    返回：
      lpkg, pkg_name, pkg_version,
      providers: list of {key, pkg, major}
      needs: set of DT_NEEDED 字符串
      script_deps: set of 包名
      extract_dir
    """
    pkg_path = os.path.abspath(os.path.join(target_dir, lpkg))
    pkg_name = ''
    pkg_version = ''
    extract_dir = os.path.join(extract_root, lpkg)
    os.makedirs(extract_dir, exist_ok=True)

    # 解包
    ret = subprocess.run(
        ['tar', '-I', 'zstd', '-xf', pkg_path, '-C', extract_dir],
        capture_output=True,
    )
    if ret.returncode != 0:
        return {
            'lpkg': lpkg, 'pkg_name': '', 'pkg_version': '',
            'providers': [], 'needs': set(), 'script_deps': set(),
            'extract_dir': extract_dir,
        }

    # 从已解压目录读取 metadata（不再重新 tar 读取）
    meta = read_metadata(os.path.join(extract_dir, 'metadata.json'))
    if meta:
        pkg_name = meta.get('name', '') or ''
        pkg_version = meta.get('version', '') or ''

    content_dir = os.path.join(extract_dir, 'content')
    if not os.path.isdir(content_dir):
        return {
            'lpkg': lpkg, 'pkg_name': pkg_name, 'pkg_version': pkg_version,
            'providers': [], 'needs': set(), 'script_deps': set(),
            'extract_dir': extract_dir,
        }

    providers = []
    needs = set()
    script_deps = set()

    # 一次 os.walk 遍历
    for root, dirs, files in os.walk(content_dir):
        for fname in files:
            fpath = os.path.join(root, fname)
            if os.path.islink(fpath):
                continue  # 符号链接跳过，避免重复计数

            if is_elf(fpath):
                # 每个 ELF 文件只解析一次 .dynamic
                sonames, needed = parse_elf_dynamic(fpath)

                # 记录 SONAME 提供者
                for sn in sonames:
                    providers.append({
                        'key': sn,
                        'pkg': pkg_name,
                        'pkg_version': pkg_version,
                    })

                # .so 文件名作为回退提供者（捕获没有 SONAME 的库）
                if '.so' in fname:
                    providers.append({
                        'key': fname,
                        'pkg': pkg_name,
                        'pkg_version': pkg_version,
                    })

                # 收集 DT_NEEDED
                for n in needed:
                    needs.add(n)

            else:
                # --- 脚本解释器探测 ---
                interp = detect_interpreter_shebang(fpath)
                if interp:
                    script_deps.add(interp)
                elif should_file_detect(fpath):
                    interp = detect_interpreter_file(fpath)
                    if interp:
                        script_deps.add(interp)

    return {
        'lpkg': lpkg,
        'pkg_name': pkg_name,
        'pkg_version': pkg_version,
        'providers': providers,
        'needs': needs,
        'script_deps': script_deps,
        'extract_dir': extract_dir,
    }


# ---------------------------------------------------------------------------
# Phase 2：并行解析 + 回填
# ---------------------------------------------------------------------------


def resolve_and_update(scan_result, provider_map, target_dir, dry_run=False):
    """
    Phase 2 工作单元。

    将包的 DT_NEEDED + 脚本依赖解析到全局 provider_map，
    更新 metadata.json（直接替换 deps），
    若有变化则重新打包 .lpkg。

    返回 (lpkg_name, deps_list, status_string)。
    """
    lpkg = scan_result['lpkg']
    pkg_name = scan_result['pkg_name']
    needs = scan_result['needs']
    script_deps = scan_result['script_deps']
    extract_dir = scan_result['extract_dir']

    if not pkg_name:
        return lpkg, [], 'no_pkg_name'

    deps = {}  # 包名 → 约束字符串（或 None）

    # --- 1) 解析 ELF DT_NEEDED → 提供者包 ---
    for soname in sorted(needs):
        provider = provider_map.get(soname)
        if provider and provider['pkg'] and provider['pkg'] != pkg_name:
            # 版本约束来自提供者包的 version（而非 SONAME）：
            # SONAME 的 ABI 编号与包版本是完全独立的体系。
            # glibc 2.42 → libc.so.6 → 锁 >= 2.0.0，不是 >= 6.0.0
            pkg_ver = provider.get('pkg_version')
            major = extract_package_major(pkg_ver) if pkg_ver else None
            if major:
                deps[provider['pkg']] = f">= {major}.0.0"
            else:
                deps.setdefault(provider['pkg'])

    # --- 2) 解析脚本解释器 → 包 ---
    for dep_pkg in sorted(script_deps):
        if dep_pkg != pkg_name and dep_pkg not in deps:
            deps[dep_pkg] = None

    # --- 3) 格式化依赖列表 ---
    dep_entries = sorted(
        f"{pkg} {constraint}" if constraint else pkg
        for pkg, constraint in deps.items()
    )

    # --- 4) 读取当前 metadata（从已解压目录，不再重新从 tar 读） ---
    meta_path = os.path.join(extract_dir, 'metadata.json')
    meta = read_metadata(meta_path)
    if not meta:
        return lpkg, dep_entries, 'no_metadata'

    old_deps = sorted(meta.get('deps', []))
    if old_deps == dep_entries:
        return lpkg, dep_entries, 'unchanged'

    if dry_run:
        return lpkg, dep_entries, 'would_update'

    # --- 5) 直接替换 deps（不合并）---
    # 设计决定：扫描结果是运行时依赖的唯一真相，见模块 docstring。
    meta['deps'] = dep_entries
    with open(meta_path, 'w', encoding='utf-8') as f:
        json.dump(meta, f, indent=2, ensure_ascii=False)

    # --- 6) 重新打包 ---
    pkg_path = os.path.abspath(os.path.join(target_dir, lpkg))
    repack_path = pkg_path + '.repacked'
    ret = subprocess.run(
        ['tar', '-I', 'zstd', '-cf', repack_path, '-C', extract_dir, '.'],
        capture_output=True, text=True,
    )
    if ret.returncode != 0:
        stderr_short = ret.stderr.strip().split('\n')[-1] if ret.stderr else '(no stderr)'
        return lpkg, dep_entries, f'repack_failed: {stderr_short}'
    os.replace(repack_path, pkg_path)

    return lpkg, dep_entries, 'updated'


# ---------------------------------------------------------------------------
# CLI 入口
# ---------------------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(
        description='Auto-generate dependencies for .lpkg packages based on '
                    'ELF dynamic links and script interpreters.',
    )
    parser.add_argument('directory',
                        help='Directory containing .lpkg files')
    parser.add_argument('-j', '--jobs', type=int,
                        default=os.cpu_count() or 4,
                        help='Parallel workers (default: number of CPUs)')
    parser.add_argument('--dry-run', action='store_true',
                        help='Show what would change without modifying files')
    parser.add_argument('--no-file-detection', action='store_true',
                        help='Skip file(1) based script detection')

    args = parser.parse_args()
    target_dir = os.path.abspath(args.directory)

    if not os.path.isdir(target_dir):
        print(f'Error: {target_dir} is not a directory.', file=sys.stderr)
        sys.exit(1)

    lpkg_files = sorted(f for f in os.listdir(target_dir) if f.endswith('.lpkg'))
    if not lpkg_files:
        print(f'No .lpkg files found in {target_dir}.')
        return

    working_dir = tempfile.mkdtemp(prefix='lpkg_dep_gen_')
    extract_root = os.path.join(working_dir, 'extract')
    os.makedirs(extract_root, exist_ok=True)

    print(f'[*] Processing {len(lpkg_files)} packages in {target_dir}')
    if args.dry_run:
        print('[*] DRY RUN — no files will be modified')
    print(f'[*] Workers: {args.jobs}  '
          f'pyelftools: {"yes" if HAVE_PYELFFTOOLS else "no (fallback readelf)"}  '
          f'file(1): {"off" if args.no_file_detection else "on"}')

    # ==================================================================
    # Phase 1: 并行解包 + 扫描
    # ==================================================================
    print('[*] Phase 1: Scanning packages...')
    all_results = []
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futures = {
            ex.submit(scan_package, f, target_dir, extract_root): f
            for f in lpkg_files
        }
        for i, future in enumerate(as_completed(futures), 1):
            result = future.result()
            all_results.append(result)
            if i % 10 == 0 or i == len(lpkg_files):
                print(f'   Scan: {i}/{len(lpkg_files)}')

    # 构建全局提供者映射
    # first-wins 策略：第一个注册某 SONAME/.so 文件名的包被视为权威提供者。
    # 在 LFS 中每个 .so 属于独立的包，因此在实践中这个启发式是安全的。
    provider_map = {}
    for result in all_results:
        for prov in result['providers']:
            key = prov['key']
            if key and key not in provider_map:
                provider_map[key] = {
                    'pkg': prov['pkg'],
                    'pkg_version': prov['pkg_version'],
                }

    print(f'[*] Provider map: {len(provider_map)} entries (SONAME + .so filenames)')

    # ==================================================================
    # Phase 2: 并行解析 + 回填
    # ==================================================================
    print('[*] Phase 2: Resolving dependencies...')
    counts = {
        'updated': 0, 'would_update': 0, 'unchanged': 0,
        'no_metadata': 0, 'no_pkg_name': 0, 'repack_failed': 0,
    }

    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futures = {
            ex.submit(resolve_and_update, r, provider_map, target_dir, args.dry_run): r
            for r in all_results
        }
        for i, future in enumerate(as_completed(futures), 1):
            lpkg_name, deps, status = future.result()
            if status == 'updated':
                counts['updated'] += 1
            elif status == 'would_update':
                counts['would_update'] += 1
            elif status == 'unchanged':
                counts['unchanged'] += 1
            elif status == 'no_metadata':
                counts['no_metadata'] += 1
            elif status == 'no_pkg_name':
                counts['no_pkg_name'] += 1
            else:
                counts['repack_failed'] += 1

            if status == 'updated':
                print(f'   [+] {lpkg_name}: {", ".join(deps) if deps else "(no deps)"}')
            elif status == 'would_update':
                print(f'   [~] {lpkg_name}: would set deps={deps}')
            elif status == 'no_metadata':
                print(f'   [!] {lpkg_name}: missing metadata.json', file=sys.stderr)
            elif status.startswith('repack_failed'):
                detail = status.split(':', 1)[1].strip() if ':' in status else ''
                print(f'   [!!] {lpkg_name}: repack failed — {detail}', file=sys.stderr)

            if i % 10 == 0 or i == len(lpkg_files):
                print(f'   Progress: {i}/{len(lpkg_files)}')

    # 清理
    if not args.dry_run:
        print('[*] Cleaning up temporary files...')
        shutil.rmtree(working_dir, ignore_errors=True)
    else:
        print(f'[*] Temporary files kept at: {working_dir}')

    # 汇总
    print()
    print('[*] Summary:')
    print(f'   Updated:         {counts["updated"]}')
    print(f'   No change:       {counts["unchanged"]}')
    if counts['would_update']:
        print(f'   Would update:    {counts["would_update"]} (--dry-run)')
    if counts['no_metadata']:
        print(f'   No metadata:     {counts["no_metadata"]}')
    if counts['no_pkg_name']:
        print(f'   No pkg name:     {counts["no_pkg_name"]}')
    if counts['repack_failed']:
        print(f'   Repack failed:   {counts["repack_failed"]}')
    print('[*] Done.')


if __name__ == '__main__':
    main()
