#!/usr/bin/env python3
"""
gen_deps.py — Auto-generate dependencies for .lpkg packages.

=== 设计原则 ===

本工具直接替换（而非合并）metadata.json 中的 deps 和 needed_so 字段。
原因是：它探测的是运行时真实依赖（ELF DT_NEEDED + 脚本解释器），
扫描结果就是运行时依赖的唯一真相（source of truth）。

=== 输出字段 ===

  needed_so: ["libc.so.6", "libz.so.1", ...]
    原始探测结果：当前包所有 ELF 文件的 DT_NEEDED 条目。

  deps: ["glibc", "zlib", ...]
    needed_so 经 provider_map 解析得到的包名列表（无版本约束）。
    然后由 deprules/ 中的规则插件补充（脚本解释器、xwayland 注入等）。

=== 功能 ===
  • ELF 动态链接分析（pyelftools 优先，回退 readelf）
  • SONAME 收集（needed_so）与提供者解析（deps）
  • 可扩展规则系统（deprules/ 目录下的 .py 文件自动加载）
  • 流水线架构：一次解包，同时扫描 SONAME + NEEDED
  • 并行流水线 + dry-run 模式
"""

import os
import sys
import json
import re
import argparse
import shutil
import tempfile
import subprocess
from concurrent.futures import ThreadPoolExecutor, as_completed

# 规则系统
from deprules import discover_rules

# ---------------------------------------------------------------------------
# 权限自提升
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
except ImportError:
    print("[!] 错误: 需要 pyelftools 但未安装。", file=sys.stderr)
    print("    请运行: pip install pyelftools", file=sys.stderr)
    sys.exit(1)

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

    使用 pyelftools 解析，无子进程开销。
    """
    sonames = []
    needed = []

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


def extract_package_major(version_str):
    """从包版本号中提取主版本号（第一个数字段）。"""
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
# Phase 1：并行解包 + 扫描
# ---------------------------------------------------------------------------


def scan_package(lpkg, target_dir, extract_root):
    """
    Phase 1 工作单元。一次解包，一次性扫描所有文件：
      • ELF SONAME（提供者）+ DT_NEEDED（依赖）
      • .so 文件名回退提供者
      • 脚本解释器原始数据（由 deprules/shebang.py 解析映射）

    返回：
      lpkg, pkg_name, pkg_version,
      providers, provides_so, needs, script_deps, extract_dir
    """
    pkg_path = os.path.abspath(os.path.join(target_dir, lpkg))
    pkg_name = ''
    pkg_version = ''
    extract_dir = os.path.join(extract_root, lpkg)
    os.makedirs(extract_dir, exist_ok=True)

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
    provides_so = []
    needs = set()
    script_deps = set()

    for root, dirs, files in os.walk(content_dir):
        for fname in files:
            fpath = os.path.join(root, fname)
            if os.path.islink(fpath):
                continue

            if is_elf(fpath):
                sonames, needed = parse_elf_dynamic(fpath)

                for sn in sonames:
                    provides_so.append(sn)
                    providers.append({
                        'key': sn,
                        'pkg': pkg_name,
                        'pkg_version': pkg_version,
                    })

                if '.so' in fname:
                    provides_so.append(fname)
                    providers.append({
                        'key': fname,
                        'pkg': pkg_name,
                        'pkg_version': pkg_version,
                    })

                for n in needed:
                    needs.add(n)

            else:
                # 脚本解释器探测（原始数据，由 deprules/shebang.py 解析映射）
                interp = None
                try:
                    with open(fpath, 'rb') as f:
                        header = f.read(256)
                    first_line = header.split(b'\n')[0].decode('utf-8', 'ignore').strip()
                    if first_line.startswith('#!'):
                        parts = first_line[2:].split()
                        if parts:
                            interp = os.path.basename(parts[0])
                            if interp == 'env' and len(parts) > 1:
                                interp = next((p for p in parts[1:] if not p.startswith('-')), parts[1])
                except (OSError, UnicodeDecodeError):
                    pass
                if interp:
                    script_deps.add(interp)

    return {
        'lpkg': lpkg,
        'pkg_name': pkg_name,
        'pkg_version': pkg_version,
        'providers': providers,
        'provides_so': provides_so,
        'needs': needs,
        'script_deps': script_deps,
        'extract_dir': extract_dir,
    }


# ---------------------------------------------------------------------------
# Phase 2：并行解析 + 回填
# ---------------------------------------------------------------------------


def resolve_and_update(scan_result, provider_map, target_dir, dry_run=False, rules=None, rule_context=None):
    """
    Phase 2 工作单元。

    将包的 DT_NEEDED 解析到全局 provider_map，
    然后执行 deprules/ 中的规则插件（脚本解释器、xwayland 注入等），
    更新 metadata.json，若有变化则重新打包 .lpkg。
    """
    lpkg = scan_result['lpkg']
    pkg_name = scan_result['pkg_name']
    provides_so = scan_result.get('provides_so', [])
    needs = scan_result['needs']
    extract_dir = scan_result['extract_dir']

    if not pkg_name:
        return lpkg, [], 'no_pkg_name'

    deps = {}
    needed_so = []

    # --- 1) SONAME 解析 ---
    for soname in sorted(needs):
        provider = provider_map.get(soname)
        if provider and provider['pkg'] and provider['pkg'] != pkg_name:
            deps.setdefault(provider['pkg'])
            needed_so.append(soname)

    # --- 2) 执行规则插件 ---
    if rules:
        ctx = dict(rule_context or {})
        ctx['pkg_name'] = pkg_name
        ctx['pkg_version'] = scan_result.get('pkg_version', '')
        for rule_name, rule_desc, rule_fn in rules:
            try:
                rule_fn(scan_result, deps, needed_so, provider_map, ctx)
            except Exception as e:
                print(f'      [!] 规则 {rule_name} 失败 ({pkg_name}): {e}', file=sys.stderr)

    # --- 3) 格式化 ---
    dep_entries = sorted(deps.keys())
    needed_so_entries = sorted(needed_so)

    # --- 4) 读取 + 比较 ---
    meta_path = os.path.join(extract_dir, 'metadata.json')
    meta = read_metadata(meta_path)
    if not meta:
        return lpkg, dep_entries, 'no_metadata'

    old_provides = set(meta.get('provides', []))
    new_provides = sorted(old_provides | set(provides_so))

    old_deps = sorted(meta.get('deps', []))
    old_needed = sorted(meta.get('needed_so', []))
    if old_deps == dep_entries and old_needed == needed_so_entries and old_provides == set(new_provides):
        return lpkg, dep_entries, 'unchanged'

    if dry_run:
        return lpkg, dep_entries, 'would_update'

    # --- 5) 写入 ---
    meta['deps'] = dep_entries
    meta['needed_so'] = needed_so_entries
    meta['provides'] = new_provides
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
    parser.add_argument('directory', help='Directory containing .lpkg files')
    parser.add_argument('-j', '--jobs', type=int,
                        default=os.cpu_count() or 4,
                        help='Parallel workers (default: number of CPUs)')
    parser.add_argument('--dry-run', action='store_true',
                        help='Show what would change without modifying files')
    parser.add_argument('--no-file-detection', action='store_true',
                        help='Skip file(1) based script detection')
    parser.add_argument('--rules-dir', type=str, default=None,
                        help='Path to deprules/ directory (default: <script_dir>/deprules)')

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
          f'file(1): {"off" if args.no_file_detection else "on"}')

    # ==================================================================
    # 加载规则插件
    # ==================================================================
    rules_dir = args.rules_dir or os.path.join(os.path.dirname(os.path.abspath(__file__)), 'deprules')
    rules = []
    if os.path.isdir(rules_dir):
        print(f'[*] Loading rules from: {rules_dir}')
        rules = discover_rules(rules_dir)
        if rules:
            print(f'[*] Rules loaded: {len(rules)}')
        else:
            print('[*] No rules found, running in SONAME-only mode')
    else:
        print(f'[*] Rules directory not found: {rules_dir}')
    print()

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

    rule_context = {
        'dry_run': args.dry_run,
        'no_file_detection': args.no_file_detection,
        'rules_dir': rules_dir,
    }

    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futures = {
            ex.submit(resolve_and_update, r, provider_map, target_dir,
                       args.dry_run, rules, rule_context): r
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
    print('[*] Cleaning up temporary files...')
    shutil.rmtree(working_dir, ignore_errors=True)

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
