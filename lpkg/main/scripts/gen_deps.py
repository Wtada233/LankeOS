import os
import subprocess
import shutil
import tempfile
import re
import json
import argparse
import concurrent.futures
import sys

if os.geteuid() != 0:
    cmd = ["sudo", sys.executable] + sys.argv
    try:
        subprocess.run(cmd, check=True)
    except subprocess.CalledProcessError as e:
        print(f"Failed to re-execute with sudo: {e}", file=sys.stderr)
        sys.exit(e.returncode)
    sys.exit(0)

def run_cmd(cmd):
    """执行 shell 命令并返回 CompletedProcess"""
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)

def get_elf_needed(filepath):
    """获取 ELF 文件的 NEEDED 列表"""
    res = run_cmd(f"LC_ALL=C readelf -d '{filepath}' 2>/dev/null | grep '(NEEDED)'")
    return re.findall(r"\[(.*?)\]", res.stdout)

def get_elf_soname(filepath):
    """获取 ELF 文件的 SONAME"""
    res = run_cmd(f"LC_ALL=C readelf -d '{filepath}' 2>/dev/null | grep '(SONAME)'")
    return re.findall(r"\[(.*?)\]", res.stdout)

def is_elf(filepath):
    """通过 Magic Number 检查文件是否为 ELF"""
    res = run_cmd(f"head -c 4 '{filepath}' 2>/dev/null | hexdump -e '4/1 \"%02x\"'")
    return res.stdout.strip() == "7f454c46"

def read_pkg_metadata(pkg_path):
    """从 .lpkg 包中读取 metadata.json 获取包名"""
    try:
        result = subprocess.run(
            ['tar', '--use-compress-program=zstd', '-xf', pkg_path, '--wildcards', '*metadata.json', '-O'],
            capture_output=True, text=True
        )
        if result.returncode == 0 and result.stdout.strip():
            meta = json.loads(result.stdout)
            return meta.get('name', '')
    except Exception:
        pass
    # 回退到文件名解析
    fname = os.path.basename(pkg_path).replace(".lpkg", "")
    if "-" in fname:
        return fname.rsplit("-", 1)[0]
    return fname

def process_phase1(lpkg_info):
    """Phase 1: 扫描包内容并建立提供者映射 (SONAME -> Package)"""
    lpkg, target_dir, extract_root = lpkg_info
    pkg_path = os.path.abspath(os.path.join(target_dir, lpkg))
    pkg_name = read_pkg_metadata(pkg_path)
    extract_dir = os.path.join(extract_root, lpkg)
    os.makedirs(extract_dir, exist_ok=True)
    
    # 解压
    run_cmd(f"tar -I zstd -xf '{pkg_path}' -C '{extract_dir}'")
    
    content_dir = os.path.join(extract_dir, "content")
    local_providers = []
    
    # 查找所有 ELF 文件并记录 SONAME/文件名
    res = run_cmd(f"find '{content_dir}' -type f")
    for fpath in res.stdout.splitlines():
        if is_elf(fpath):
            # 记录内部 SONAME
            sonames = get_elf_soname(fpath)
            for sn in sonames:
                local_providers.append((sn, pkg_name))
            
            # 记录文件名作为 fallback (针对 .so)
            fname = os.path.basename(fpath)
            if ".so" in fname:
                local_providers.append((fname, pkg_name))
    return local_providers

def process_phase2(lpkg_info, provider_map, working_dir):
    """Phase 2: 分析 NEEDED 链接并更新 deps.txt，最后重新打包"""
    lpkg, target_dir, extract_root = lpkg_info
    pkg_path = os.path.abspath(os.path.join(target_dir, lpkg))
    pkg_name = read_pkg_metadata(pkg_path)
    extract_dir = os.path.join(extract_root, lpkg)
    content_dir = os.path.join(extract_dir, "content")
    
    needs = set()
    res = run_cmd(f"find '{content_dir}' -type f")
    for fpath in res.stdout.splitlines():
        if is_elf(fpath):
            needed = get_elf_needed(fpath)
            for n in needed:
                if n in provider_map:
                    dep_pkg = provider_map[n]
                    if dep_pkg != pkg_name:
                        needs.add(dep_pkg)
    
    # Update metadata.json
    meta_path = os.path.join(extract_dir, "metadata.json")
    if os.path.exists(meta_path):
        with open(meta_path, 'r') as f:
            meta = json.load(f)
        
        meta['deps'] = sorted(list(needs))
        with open(meta_path, 'w') as f:
            json.dump(meta, f, indent=2)
    
    # 重新打包
    repack_path = pkg_path + ".repacked"
    run_cmd(f"tar -I zstd -cf '{repack_path}' -C '{extract_dir}' .")
    run_cmd(f"mv '{repack_path}' '{pkg_path}'")
    return lpkg, sorted(list(needs))

def main():
    parser = argparse.ArgumentParser(description="Auto-generate dependencies for .lpkg files based on ELF dynamic links.")
    parser.add_argument("directory", help="The directory containing .lpkg files to process.")
    parser.add_argument("-j", "--jobs", type=int, default=8, help="Number of parallel jobs (default: 8).")
    
    args = parser.parse_args()
    target_dir = os.path.abspath(args.directory)
    
    if not os.path.isdir(target_dir):
        print(f"Error: {target_dir} is not a directory.")
        sys.exit(1)

    lpkg_files = [f for f in os.listdir(target_dir) if f.endswith(".lpkg")]
    if not lpkg_files:
        print(f"No .lpkg files found in {target_dir}.")
        sys.exit(0)

    working_dir = tempfile.mkdtemp(prefix="lpkg_dep_gen_")
    extract_root = os.path.join(working_dir, "extract")
    os.makedirs(extract_root, exist_ok=True)

    print(f"[*] Processing {len(lpkg_files)} packages in {target_dir}...")
    print(f"[*] Temp working directory: {working_dir}")

    provider_map = {}
    pkg_infos = [(f, target_dir, extract_root) for f in lpkg_files]

    # Phase 1: 并行建立映射
    print(f"[*] Phase 1: Building provider map...")
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {executor.submit(process_phase1, info): info for info in pkg_infos}
        for i, future in enumerate(concurrent.futures.as_completed(futures), 1):
            for sn, pkg in future.result():
                if sn not in provider_map:
                    provider_map[sn] = pkg
            if i % 10 == 0 or i == len(lpkg_files):
                print(f"    Scanning: {i}/{len(lpkg_files)} packages indexed.")

    print(f"[*] Map initialized with {len(provider_map)} dynamic library entries.")

    # Phase 2: 并行分析并回填
    print(f"[*] Phase 2: Analyzing dependencies and repacking...")
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {executor.submit(process_phase2, info, provider_map, working_dir): info for info in pkg_infos}
        for i, future in enumerate(concurrent.futures.as_completed(futures), 1):
            lpkg, deps = future.result()
            if deps:
                print(f"    [+] {lpkg} updated with: {', '.join(deps)}")
            if i % 10 == 0 or i == len(lpkg_files):
                print(f"    Progress: {i}/{len(lpkg_files)} packages processed.")

    # 清理
    print(f"[*] Cleaning up temporary files...")
    run_cmd(f"rm -rf '{working_dir}'")
    print("[*] All tasks completed successfully!")

if __name__ == "__main__":
    main()
