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
    运行时依赖的权威表达——安装时 lpkg 据此校验提供者是否存在。

  deps: ["glibc", "zlib", ...]
    默认不自动生成（needed_so 一层即足够），由 deprules/ 中的
    规则插件按需填充（脚本解释器、meta 包保护、xwayland 注入等）。

=== 功能 ===
  • ELF 动态链接分析（pyelftools 优先，回退 readelf）
  • SONAME 收集（needed_so，过滤包自身提供的条目）
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


def _in_system_lib_dir(fpath, content_dir):
    """检查 ELF 文件是否在系统标准库路径下（/usr/lib/ 或 /lib/ 直接子级）。

    排除 /usr/lib/chromium/、/usr/lib/firefox/ 等应用内部捆绑库路径，
    这些路径中的 .so 不应作为系统级 SONAME 提供者。
    """
    try:
        rel = os.path.relpath(fpath, content_dir)
    except ValueError:
        return False
    return os.path.dirname(rel) in ('usr/lib', 'lib', 'usr/lib64', 'lib64')


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


def _update_lankebuild_json(pkg_name, needed_so, deps, provides, metadata_target, dry_run=False):
    """
    在 metadata_target 目录中递归查找匹配 name 字段的 LankeBUILD.json，
    更新其 needed_so、deps 和 provides 字段。
    """
    matches = []
    for root, dirs, files in os.walk(metadata_target):
        for f in files:
            if f == 'LankeBUILD.json':
                path = os.path.join(root, f)
                try:
                    with open(path, 'r', encoding='utf-8') as fh:
                        data = json.load(fh)
                    if data.get('name') == pkg_name:
                        matches.append(path)
                except (json.JSONDecodeError, OSError):
                    continue

    if not matches:
        print(f'      [WARN] metadata-target 中未找到包 "{pkg_name}" 的 LankeBUILD.json',
              file=sys.stderr)
        return

    if len(matches) > 1:
        print(f'      [WARN] 包 "{pkg_name}" 在 metadata-target 中找到 '
              f'{len(matches)} 个 LankeBUILD.json，将修改第一个')

    target_path = matches[0]

    if dry_run:
        print(f'      [~] metadata-target: 将更新 {target_path}')
        return

    with open(target_path, 'r', encoding='utf-8') as fh:
        data = json.load(fh)

    data['needed_so'] = needed_so
    data['deps'] = deps
    data['provides'] = provides

    with open(target_path, 'w', encoding='utf-8') as fh:
        json.dump(data, fh, indent=2, ensure_ascii=False)

    print(f'      [*] metadata-target: 已更新 {target_path}')


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
      provides_so, needs, script_deps, extract_dir
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
            'needs': set(), 'script_deps': set(),
            'all_sonames': set(),
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
            'needs': set(), 'script_deps': set(),
            'all_sonames': set(),
            'extract_dir': extract_dir,
        }

    provides_so = []
    all_sonames = set()      # 包内所有 ELF 的 SONAME（含捆绑库，用于自提供 NEEDED 跳过）
    needs = set()
    script_deps = set()

    for root, dirs, files in os.walk(content_dir):
        for fname in files:
            fpath = os.path.join(root, fname)

            if os.path.islink(fpath):
                target = os.readlink(fpath)
                if not os.path.isabs(target):
                    target = os.path.join(os.path.dirname(fpath), target)
                real_target = os.path.realpath(target)   # 或者用 os.path.abspath + 循环，但 realpath 递归解析
                if os.path.isfile(real_target) and is_elf(real_target):
                    # 只有 /usr/lib/、/lib/ 等标准路径中的 .so 符号链接才注册为提供者。
                    # 三个条件：① .so 命名（排除 chromium-browser 等可执行文件软链接）
                    #          ② 标准库路径（排除 /usr/lib/chromium/ 等捆绑路径）
                    #          ③ 链接指向 ELF
                    if '.so' in fname and _in_system_lib_dir(fpath, content_dir):
                        provides_so.append(fname)
                continue

            if is_elf(fpath):
                sonames, needed = parse_elf_dynamic(fpath)

                # all_sonames：记录包内所有 SONAME（无论路径），用于后续跳过自提供的 NEEDED
                # 这确保 firefox 的捆绑 libnss3.so 能正确跳过对系统 nss 包的依赖
                for sn in sonames:
                    all_sonames.add(sn)

                in_lib = _in_system_lib_dir(fpath, content_dir)

                # SONAME 提供者注册（仅系统标准库路径，排除捆绑库）
                if sonames and in_lib:
                    for sn in sonames:
                        provides_so.append(sn)
                elif in_lib and '.so' in fname:
                    # 无 SONAME 回退：部分老库（如 tcl 的 libtcl8.6.so）不设 SONAME
                    # 但文件名本身就是其他包的 DT_NEEDED 目标，注册文件名作为提供者
                    provides_so.append(fname)

                for n in needed:
                    if '/' in n:
                        n = os.path.basename(n)
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
        'provides_so': provides_so,
        'all_sonames': all_sonames,
        'needs': needs,
        'script_deps': script_deps,
        'extract_dir': extract_dir,
    }


# ---------------------------------------------------------------------------
# Phase 2：并行解析 + 回填
# ---------------------------------------------------------------------------


def resolve_and_update(scan_result, target_dir, dry_run=False, rules=None, rule_context=None, metadata_target=None):
    """
    Phase 2 工作单元。

    执行 deprules/ 中的规则插件（脚本解释器、xwayland 注入等），
    更新 metadata.json，若有变化则重新打包 .lpkg。
    """
    lpkg = scan_result['lpkg']
    pkg_name = scan_result['pkg_name']
    provides_so = scan_result.get('provides_so', [])
    needs = scan_result['needs']
    extract_dir = scan_result['extract_dir']

    if not pkg_name:
        return lpkg, [], 'no_pkg_name'

    deps = {}                          # 默认空：deps 由 deprules 规则填充，不再由 needed_so 解析
    needed_so = sorted(needs)          # ① 直接使用全部 DT_NEEDED

    # 当前包自身提供的 SONAME 全集（含捆绑库，如 firefox 的 /usr/lib/firefox/libnss3.so）
    # 只要包内任意文件提供了该 SONAME，就跳过依赖解析——运行时 RUNPATH 会找到它
    self_provided = scan_result.get('all_sonames', set())

    # ② （已移除）needed_so → provider_map 包名匹配生成 deps 的逻辑。
    #    needed_so 一层即足够：安装时 lpkg 直接校验 SONAME 提供者，
    #    包级 deps 由 deprules 规则按需填充。

    # ③ 过滤掉包自身提供的 needed_so（如 python 的 libpython3.13.so.1.0）
    # 使用 self_provided 而非全局 provider 判断，避免因捆绑包先入为主导致过滤失效
    needed_so = [n for n in needed_so if n not in self_provided]

    # --- 2) 执行规则插件 ---
    if rules:
        ctx = dict(rule_context or {})
        ctx['pkg_name'] = pkg_name
        ctx['pkg_version'] = scan_result.get('pkg_version', '')
        for rule_name, rule_desc, rule_fn in rules:
            try:
                rule_fn(scan_result, deps, needed_so, ctx)
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
    new_provides = sorted(set(provides_so))

    # --- LankeBUILD.json metadata 更新（如果 --metadata-target 已设置） ---
    if metadata_target and pkg_name:
        _update_lankebuild_json(pkg_name, needed_so_entries, dep_entries,
                                new_provides, metadata_target, dry_run)

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
        ['tar', '-I', 'zstd -22 --ultra', '-cf', repack_path, '-C', extract_dir, '.'],
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
    parser.add_argument('--tmp-dir', type=str, default=None,
                        help='Temporary directory for extraction (default: $LPKG_TMP_DIR or system temp)')
    parser.add_argument('--rules-dir', type=str, default=None,
                        help='Path to deprules/ directory (default: <script_dir>/deprules)')
    parser.add_argument('--metadata-target', type=str, default=None,
                        help='目录，包含 LankeBUILD.json 文件（递归查找），'
                             '根据 name 字段匹配并更新 needed_so、deps 和 provides）')

    args = parser.parse_args()
    target_dir = os.path.abspath(args.directory)

    if not os.path.isdir(target_dir):
        print(f'Error: {target_dir} is not a directory.', file=sys.stderr)
        sys.exit(1)

    lpkg_files = sorted(f for f in os.listdir(target_dir) if f.endswith('.lpkg'))
    if not lpkg_files:
        print(f'No .lpkg files found in {target_dir}.')
        return

    tmp_base = (args.tmp_dir or os.environ.get('LPKG_TMP_DIR') or
                tempfile.gettempdir())
    os.makedirs(tmp_base, exist_ok=True)
    working_dir = tempfile.mkdtemp(prefix='lpkg_dep_gen_', dir=tmp_base)
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
            ex.submit(resolve_and_update, r, target_dir,
                       args.dry_run, rules, rule_context, args.metadata_target): r
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
