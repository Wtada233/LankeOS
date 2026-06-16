#!/usr/bin/env python3
import os
import sys
import json
import subprocess
import tempfile
import shutil

def run_cmd(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)

def convert_lpkg(pkg_path):
    print(f"[*] Converting {pkg_path}...")
    tmp_dir = tempfile.mkdtemp(prefix="lpkg_conv_")
    
    try:
        # 1. Extract
        res = run_cmd(f"sudo tar -I zstd -xf '{pkg_path}' -C '{tmp_dir}'")
        if res.returncode != 0:
            print(f"Error extracting: {res.stderr}")
            return

        # 2. Read existing metadata
        meta_path = os.path.join(tmp_dir, "metadata.json")
        if not os.path.exists(meta_path):
            print(f"Error: metadata.json not found in {pkg_path}")
            return
            
        with open(meta_path, 'r') as f:
            meta = json.load(f)
            
        deps = []
        deps_path = os.path.join(tmp_dir, "deps.txt")
        if os.path.exists(deps_path):
            with open(deps_path, 'r') as f:
                deps = [line.strip() for line in f if line.strip()]
            run_cmd(f"sudo rm '{deps_path}'")
            
        provides = []
        prov_path = os.path.join(tmp_dir, "provides.txt")
        if os.path.exists(prov_path):
            with open(prov_path, 'r') as f:
                provides = [line.strip() for line in f if line.strip()]
            run_cmd(f"sudo rm '{prov_path}'")
            
        man = ""
        man_path = os.path.join(tmp_dir, "man.txt")
        if os.path.exists(man_path):
            with open(man_path, 'r') as f:
                man = f.read()
            run_cmd(f"sudo rm '{man_path}'")
            
        # 3. Update metadata
        meta['deps'] = deps
        meta['provides'] = provides
        meta['man'] = man
        
        with open(meta_path, 'w') as f:
            json.dump(meta, f, indent=2)
            
        # 4. Repack
        repack_path = pkg_path + ".new"
        run_cmd(f"sudo chown -R root:root '{tmp_dir}'")
        res = run_cmd(f"sudo tar -I zstd -cf '{repack_path}' -C '{tmp_dir}' .")
        if res.returncode != 0:
            print(f"Error repacking: {res.stderr}")
            return
            
        run_cmd(f"sudo mv '{repack_path}' '{pkg_path}'")
        print(f"[+] Successfully converted {pkg_path}")
        
    finally:
        run_cmd(f"sudo rm -rf '{tmp_dir}'")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: convert_lpkg.py <pkg1.lpkg> [pkg2.lpkg ...]")
        sys.exit(1)
        
    for arg in sys.argv[1:]:
        if os.path.isfile(arg):
            convert_lpkg(os.path.abspath(arg))
        elif os.path.isdir(arg):
            for f in os.listdir(arg):
                if f.endswith(".lpkg"):
                    convert_lpkg(os.path.abspath(os.path.join(arg, f)))
