import os
import subprocess
import shutil
import tempfile
import re
import argparse
import concurrent.futures
import sys

def run_cmd(cmd):
    """执行 shell 命令并返回 CompletedProcess"""
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)

def get_elf_needed(filepath):
    """获取 ELF 文件的 NEEDED 列表"""
    res = run_cmd(f"sudo LC_ALL=C readelf -d '{filepath}' 2>/dev/null | grep '(NEEDED)'")
    return re.findall(r"\[(.*?)\]", res.stdout)

def get_elf_soname(filepath):
    """获取 ELF 文件的 SONAME"""
    res = run_cmd(f"sudo LC_ALL=C readelf -d '{filepath}' 2>/dev/null | grep '(SONAME)'")
    return re.findall(r"\[(.*?)\]", res.stdout)

def is_elf(filepath):
    """通过 Magic Number 检查文件是否为 ELF"""
    res = run_cmd(f"sudo head -c 4 '{filepath}' 2>/dev/null | hexdump -e '4/1 \"%02x\"'")
    return res.stdout.strip() == "7f454c46"

def process_phase1(lpkg_info):
    """Phase 1: 扫描包内容并建立提供者映射 (SONAME -> Package)"""
    lpkg, target_dir, extract_root = lpkg_info
    pkg_path = os.path.abspath(os.path.join(target_dir, lpkg))
    # --- 修复：对齐包管理器解析逻辑 ---
    # 去掉 .lpkg 后，从最后一个 '-' 处切割，取前半部分
    raw_name = lpkg.replace(".lpkg", "")
    if "-" in raw_name:
        pkg_name = raw_name.rsplit("-", 1)[0]
    else:
        pkg_name = raw_name
    # ----------------------------------
    extract_dir = os.path.join(extract_root, lpkg)
    os.makedirs(extract_dir, exist_ok=True)
    
    # 解压
    run_cmd(f"sudo tar -I zstd -xf '{pkg_path}' -C '{extract_dir}'")
    
    content_dir = os.path.join(extract_dir, "content")
    local_providers = []
    
    # 查找所有 ELF 文件并记录 SONAME/文件名
    res = run_cmd(f"sudo find '{content_dir}' -type f")
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
    # --- 修复：对齐包管理器解析逻辑 ---
    raw_name = lpkg.replace(".lpkg", "")
    if "-" in raw_name:
        pkg_name = raw_name.rsplit("-", 1)[0]
    else:
        pkg_name = raw_name
    # ----------------------------------
    extract_dir = os.path.join(extract_root, lpkg)
    content_dir = os.path.join(extract_dir, "content")
    
    needs = set()
    res = run_cmd(f"sudo find '{content_dir}' -type f")
    for fpath in res.stdout.splitlines():
        if is_elf(fpath):
            needed = get_elf_needed(fpath)
            for n in needed:
                if n in provider_map:
                    dep_pkg = provider_map[n]
                    if dep_pkg != pkg_name:
                        needs.add(dep_pkg)
    
    # 强制覆盖 deps.txt
    deps_path = os.path.join(extract_dir, "deps.txt")
    final_deps = sorted(list(needs))
    
    # 使用临时文件中转以处理权限问题
    temp_deps = os.path.join(working_dir, f"deps_gen_{lpkg}.txt")
    with open(temp_deps, "w") as f:
        for d in final_deps:
            f.write(d + "\n")
    
    run_cmd(f"sudo rm -f '{deps_path}'")
    run_cmd(f"sudo cp '{temp_deps}' '{deps_path}'")
    
    # 关键：重新打包前恢复 root 所有权，保持包内文件原始权限
    run_cmd(f"sudo chown -R root:root '{extract_dir}'")
    
    # 重新打包
    repack_path = pkg_path + ".repacked"
    run_cmd(f"sudo tar -I zstd -cf '{repack_path}' -C '{extract_dir}' .")
    run_cmd(f"sudo mv '{repack_path}' '{pkg_path}'")
    return lpkg, final_deps

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
    run_cmd(f"sudo rm -rf '{working_dir}'")
    print("[*] All tasks completed successfully!")

if __name__ == "__main__":
    main()
