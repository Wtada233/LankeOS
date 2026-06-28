#!/usr/bin/env python3
import os
import sys
import json
import hashlib
import subprocess
import tempfile
import argparse
import glob
from pathlib import Path

def calculate_sha256(file_path):
    sha256_hash = hashlib.sha256()
    with open(file_path, "rb") as f:
        for byte_block in iter(lambda: f.read(4096), b""):
            sha256_hash.update(byte_block)
    return sha256_hash.hexdigest()

def extract_metadata(archive_path):
    """Extracts metadata from metadata.json inside the archive."""
    deps = ""
    provides = ""
    needed_so = ""
    try:
        result = subprocess.run(['tar', '--use-compress-program=zstd', '-xf', archive_path, '--wildcards', '*metadata.json', '-O'],
                                capture_output=True, text=True)
        if result.returncode == 0 and result.stdout.strip():
            meta = json.loads(result.stdout)
            deps = ",".join(meta.get('deps', []))
            provides = ",".join(meta.get('provides', []))
            needed_so = ",".join(meta.get('needed_so', []))
    except Exception as e:
        print(f"  Warning: Failed to extract metadata from {archive_path}: {e}")

    return deps, provides, needed_so

def read_metadata_from_archive(archive_path):
    """Extracts metadata.json from the lpkg archive and returns name, version."""
    try:
        result = subprocess.run(
            ['tar', '--use-compress-program=zstd', '-xf', archive_path, '--wildcards', '*metadata.json', '-O'],
            capture_output=True, text=True
        )
        if result.returncode == 0 and result.stdout.strip():
            meta = json.loads(result.stdout)
            return meta.get('name', ''), meta.get('version', '')
    except Exception as e:
        print(f"  Warning: Failed to read metadata.json from {archive_path}: {e}")
    return None, None

class RepoManager:
    def __init__(self, config_path):
        self.config_path = config_path
        self.load_config()

    def load_config(self):
        if not os.path.exists(self.config_path):
            self.config = {
                "storage": {
                    "type": "s3",
                    "endpoint": "https://cos.ap-guangzhou.myqcloud.com",
                    "bucket": "example-bucket-123456789",
                    "access_key": "YOUR_ACCESS_KEY",
                    "secret_key": "YOUR_SECRET_KEY",
                    "region": "ap-guangzhou",
                    "path_prefix": ""
                },
                "architecture": "x86_64"
            }
            self.save_config()
            print(f"Created default config at {self.config_path}. Please edit it.")
        else:
            with open(self.config_path, 'r') as f:
                self.config = json.load(f)

    def save_config(self):
        with open(self.config_path, 'w') as f:
            json.dump(self.config, f, indent=4)

    def get_storage(self):
        st = self.config.get("storage", {})
        st_type = st.get("type", "s3")
        if st_type == "s3":
            import boto3
            from botocore.client import Config
            session = boto3.session.Session()
            return session.client('s3',
                                  region_name=st.get('region'),
                                  endpoint_url=st.get('endpoint'),
                                  aws_access_key_id=st.get('access_key'),
                                  aws_secret_access_key=st.get('secret_key'),
                                  config=Config(
                                      s3={'addressing_style': 'virtual'},
                                      signature_version='s3v4',
                                      request_checksum_calculation='when_required',
                                      response_checksum_validation='when_required'
                                  ))
        return None

    def push_packages(self, patterns):
        files = []
        for p in patterns:
            files.extend(glob.glob(p))
        
        if not files:
            print("No files matched patterns.")
            return

        arch = self.config.get("architecture", "x86_64")
        prefix = self.config["storage"].get("path_prefix", "").strip('/')
        if prefix: prefix += '/'

        index_local = self.download_index()
        index_data = self.parse_aggregated_index(index_local)

        for f in files:
            path = Path(f)
            if not path.is_file(): continue
            
            name, version = read_metadata_from_archive(str(path))
            if not name:
                print(f"Skipping {path.name}: Could not determine package name and version (missing metadata.json or invalid filename)")
                continue
            
            print(f"Processing {name} {version}...")
            sha256 = calculate_sha256(f)
            deps, provides, needed_so = extract_metadata(f)

            # Upload package as name/version.lpkg
            remote_pkg_path = f"{prefix}{arch}/{name}/{version}.lpkg"
            self.upload_file(str(path), remote_pkg_path)

            # Update index data
            if name not in index_data:
                index_data[name] = {"versions": {}}

            index_data[name]["versions"][version] = {
                "sha256": sha256,
                "deps": deps,
                "provides": provides,
                "needed_so": needed_so,
            }

        # Save and upload aggregated index
        self.write_aggregated_index(index_local, index_data)
        self.upload_file(index_local, f"{prefix}{arch}/index.txt")
        print("Done.")

    def delete_package(self, name, version=None):
        arch = self.config.get("architecture", "x86_64")
        prefix = self.config["storage"].get("path_prefix", "").strip('/')
        if prefix: prefix += '/'
        
        index_local = self.download_index()
        index_data = self.parse_aggregated_index(index_local)

        if version:
            if name in index_data and version in index_data[name]["versions"]:
                print(f"Deleting {name} version {version}...")
                remote_pkg = f"{prefix}{arch}/{name}/{version}.lpkg"
                self.delete_remote_file(remote_pkg)
                del index_data[name]["versions"][version]
                if not index_data[name]["versions"]:
                    del index_data[name]
            else:
                print(f"Version {version} of {name} not found.")
        else:
            print(f"Deleting all versions of {name}...")
            remote_pkg_root = f"{prefix}{arch}/{name}/"
            self.delete_remote_dir(remote_pkg_root)
            if name in index_data:
                del index_data[name]

        self.write_aggregated_index(index_local, index_data)
        self.upload_file(index_local, f"{prefix}{arch}/index.txt")
        print("Done.")

    def cleanup_repository(self):
        """Removes all versions from storage that are NOT in the index.txt"""
        print("Cleaning up historical packages...")
        arch = self.config.get("architecture", "x86_64")
        prefix = self.config["storage"].get("path_prefix", "").strip('/')
        if prefix: prefix += '/'
        
        index_local = self.download_index()
        index_data = self.parse_aggregated_index(index_local)
        
        remote_base = f"{prefix}{arch}/"
        all_pkgs = self.list_remote_dirs(remote_base)
        
        for pkg in all_pkgs:
            if pkg not in index_data:
                print(f"Package {pkg} is not in index, deleting entire package directory...")
                self.delete_remote_dir(f"{remote_base}{pkg}/")
                continue
            
            active_vers = index_data[pkg]["versions"]
            # List all .lpkg files in the package directory
            all_files = self.list_remote_files(f"{remote_base}{pkg}/")
            for f in all_files:
                if f.endswith('.lpkg'):
                    ver = f.rsplit('.', 1)[0]
                    if ver not in active_vers:
                        print(f"Deleting old version file: {pkg}/{f}...")
                        self.delete_remote_file(f"{remote_base}{pkg}/{f}")
        
        print("Cleanup complete.")

    def parse_aggregated_index(self, path):
        # 格式: name|ver:hash:deps:provides:needed_so;ver2:...|
        data = {}
        if os.path.exists(path):
            with open(path, 'r', encoding='utf-8') as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith('#'): continue
                    parts = line.split('|')
                    if len(parts) < 2: continue
                    name = parts[0]

                    for v_block in parts[1].split(';'):
                        v_info = v_block.split(':')
                        if len(v_info) < 2: continue

                        version = v_info[0]
                        hash_val = v_info[1]
                        deps = v_info[2] if len(v_info) > 2 else ""
                        # provides/needed_so 在版本块内（第 4、5 字段）
                        provides = v_info[3] if len(v_info) > 3 else ""
                        needed_so = v_info[4] if len(v_info) > 4 else ""

                        if name not in data:
                            data[name] = {"versions": {}}

                        data[name]["versions"][version] = {
                            "sha256": hash_val,
                            "deps": deps,
                            "provides": provides,
                            "needed_so": needed_so,
                        }
        return data

    def write_aggregated_index(self, path, data):
        # 新版格式: name|ver:hash:deps:provides:needed_so;ver2:...|
        with open(path, 'w', encoding='utf-8') as f:
            for name, info in data.items():
                blocks = []
                for v, vinfo in info["versions"].items():
                    provides = vinfo.get('provides', '')
                    needed_so = vinfo.get('needed_so', '')
                    blocks.append(f"{v}:{vinfo['sha256']}:{vinfo['deps']}:{provides}:{needed_so}")
                f.write(f"{name}|{';'.join(blocks)}|\n")

    def upload_file(self, local_path, remote_path):
        st = self.config["storage"]
        if st["type"] == "s3":
            client = self.get_storage()
            print(f"Uploading to S3: {remote_path}...")
            client.upload_file(local_path, st["bucket"], remote_path)
        elif st["type"] == "scp":
            print(f"Uploading via SCP: {remote_path}...")
            host = st["host"]
            user = st["user"]
            remote_base_path = st["remote_path"].rstrip('/')
            full_remote = f"{remote_base_path}/{remote_path}"
            remote_dir = os.path.dirname(full_remote)
            subprocess.run(["ssh", f"{user}@{host}", f"mkdir -p {remote_dir}"], check=True)
            subprocess.run(["scp", local_path, f"{user}@{host}:{full_remote}"], check=True)
        elif st["type"] == "local":
            dest = os.path.join(st["path"], remote_path)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            import shutil
            shutil.copy2(local_path, dest)
            print(f"  Copied to {dest}")

    def delete_remote_file(self, remote_path):
        st = self.config["storage"]
        if st["type"] == "s3":
            client = self.get_storage()
            client.delete_object(Bucket=st["bucket"], Key=remote_path)
        elif st["type"] == "scp":
            host = st["host"]
            user = st["user"]
            remote_base = st["remote_path"].rstrip('/')
            subprocess.run(["ssh", f"{user}@{host}", f"rm -f {remote_base}/{remote_path}"], check=True)
        elif st["type"] == "local":
            target = os.path.join(st["path"], remote_path)
            if os.path.exists(target): os.remove(target)

    def delete_remote_dir(self, remote_dir_prefix):
        st = self.config["storage"]
        if st["type"] == "s3":
            client = self.get_storage()
            paginator = client.get_paginator('list_objects_v2')
            pages = paginator.paginate(Bucket=st["bucket"], Prefix=remote_dir_prefix)
            for page in pages:
                if 'Contents' in page:
                    for obj in page['Contents']:
                        client.delete_object(Bucket=st["bucket"], Key=obj['Key'])
        elif st["type"] == "scp":
            host = st["host"]
            user = st["user"]
            remote_base = st["remote_path"].rstrip('/')
            subprocess.run(["ssh", f"{user}@{host}", f"rm -rf {remote_base}/{remote_dir_prefix}"], check=True)
        elif st["type"] == "local":
            target = os.path.join(st["path"], remote_dir_prefix)
            if os.path.exists(target): import shutil; shutil.rmtree(target)

    def list_remote_dirs(self, prefix):
        st = self.config["storage"]
        dirs = []
        if st["type"] == "s3":
            client = self.get_storage()
            paginator = client.get_paginator('list_objects_v2')
            pages = paginator.paginate(Bucket=st["bucket"], Prefix=prefix, Delimiter='/')
            for page in pages:
                if 'CommonPrefixes' in page:
                    for cp in page['CommonPrefixes']:
                        name = cp['Prefix'][len(prefix):].rstrip('/')
                        if name: dirs.append(name)
        elif st["type"] == "scp":
            host = st["host"]
            user = st["user"]
            remote_base = st["remote_path"].rstrip('/')
            full_path = f"{remote_base}/{prefix}"
            result = subprocess.run(["ssh", f"{user}@{host}", f"ls -1F {full_path} 2>/dev/null | grep '/$' | sed 's|/||'"], 
                                    capture_output=True, text=True)
            if result.returncode == 0:
                dirs = [d.strip() for d in result.stdout.split('\n') if d.strip()]
        elif st["type"] == "local":
            base = os.path.join(st["path"], prefix)
            if os.path.isdir(base):
                for entry in sorted(os.listdir(base)):
                    if os.path.isdir(os.path.join(base, entry)):
                        dirs.append(entry)
        return dirs

    def list_remote_files(self, prefix):
        st = self.config["storage"]
        files = []
        if st["type"] == "s3":
            client = self.get_storage()
            paginator = client.get_paginator('list_objects_v2')
            pages = paginator.paginate(Bucket=st["bucket"], Prefix=prefix, Delimiter='/')
            for page in pages:
                if 'Contents' in page:
                    for obj in page['Contents']:
                        name = obj['Key'][len(prefix):]
                        if name: files.append(name)
        elif st["type"] == "local":
            base = os.path.join(st["path"], prefix)
            if os.path.isdir(base):
                for entry in sorted(os.listdir(base)):
                    full = os.path.join(base, entry)
                    if os.path.isfile(full):
                        files.append(entry)
        return files

    def download_index(self):
        st = self.config["storage"]
        arch = self.config.get("architecture", "x86_64")
        prefix = self.config["storage"].get("path_prefix", "").strip('/')
        if prefix: prefix += '/'
        remote_path = f"{prefix}{arch}/index.txt"
        fd, local_path = tempfile.mkstemp()
        os.close(fd)
        try:
            if st["type"] == "s3":
                client = self.get_storage()
                client.download_file(st["bucket"], remote_path, local_path)
            elif st["type"] == "scp":
                host = st["host"]
                user = st["user"]
                remote_base_path = st["remote_path"].rstrip('/')
                subprocess.run(["scp", f"{user}@{host}:{remote_base_path}/{remote_path}", local_path], check=True)
            elif st["type"] == "local":
                src = os.path.join(st["path"], remote_path)
                if os.path.exists(src):
                    import shutil
                    shutil.copy2(src, local_path)
        except Exception:
            with open(local_path, 'w', encoding='utf-8') as f: pass
        return local_path

def init_local_repo(path, arch="x86_64"):
    """Initialize a local repo at the given path with the given architecture."""
    data_dir = os.path.join(path, arch)
    os.makedirs(data_dir, exist_ok=True)
    index_path = os.path.join(data_dir, "index.txt")
    if not os.path.exists(index_path):
        with open(index_path, 'w', encoding='utf-8') as f:
            f.write("# LankeOS local repo index\n")
    print(f"Initialized local repo at {data_dir}")
    return True


def main():
    parser = argparse.ArgumentParser(description="LankeOS Repository Manager")
    parser.add_argument("-c", "--config", default="lrepo-mgr.json", help="Path to config file")
    parser.add_argument("--path", metavar="DIR", help="Local repo directory (enables local mode, overrides config storage)")
    subparsers = parser.add_subparsers(dest="command")
    push_parser = subparsers.add_parser("push", help="Push packages to repository")
    push_parser.add_argument("patterns", nargs="+", help="File patterns/directories to push")
    delete_parser = subparsers.add_parser("delete", help="Delete package or specific version")
    delete_parser.add_argument("package", help="Package name or name:version")
    subparsers.add_parser("cleanup", help="Remove all historical versions not in index.txt")
    config_parser = subparsers.add_parser("config", help="View or modify config")
    config_parser.add_argument("--set", metavar="KEY=VALUE", nargs="+", help="Set config values")
    config_parser.add_argument("--show", action="store_true", help="Show current config")
    config_parser.add_argument("--show-secrets", action="store_true", help="Show current config with secrets")
    args = parser.parse_args()
    mgr = RepoManager(args.config)
    if args.path:
        mgr.config["storage"] = {"type": "local", "path": os.path.abspath(args.path)}
        init_local_repo(os.path.abspath(args.path), mgr.config.get("architecture", "x86_64"))
    if args.command == "push": mgr.push_packages(args.patterns)
    elif args.command == "delete":
        if ':' in args.package:
            name, ver = args.package.split(':', 1)
            mgr.delete_package(name, ver)
        else: mgr.delete_package(args.package)
    elif args.command == "cleanup": mgr.cleanup_repository()
    elif args.command == "config":
        if args.set:
            for item in args.set:
                key_path, value = item.split('=', 1)
                keys = key_path.split('.')
                d = mgr.config
                for k in keys[:-1]: d = d.setdefault(k, {})
                d[keys[-1]] = value
            mgr.save_config()
            print("Config updated.")
        elif args.show or args.show_secrets:
            cfg = json.loads(json.dumps(mgr.config))
            if not args.show_secrets:
                if "storage" in cfg:
                    if "secret_key" in cfg["storage"]: cfg["storage"]["secret_key"] = "********"
                    if "access_key" in cfg["storage"]: cfg["storage"]["access_key"] = cfg["storage"]["access_key"][:4] + "********"
            print(json.dumps(cfg, indent=4))
    else: parser.print_help()

if __name__ == "__main__": main()
