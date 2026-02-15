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
    """Extracts deps.txt and provides.txt from the archive using tar command."""
    deps = ""
    provides = ""
    try:
        # Try to extract deps.txt (using wildcard to handle potential ./ prefix)
        result = subprocess.run(['tar', '--use-compress-program=zstd', '-xf', archive_path, '--wildcards', '*deps.txt', '-O'], 
                                capture_output=True, text=True)
        if result.returncode == 0:
            deps = result.stdout.strip().replace('\n', ',')
        
        # Try to extract provides.txt
        result = subprocess.run(['tar', '--use-compress-program=zstd', '-xf', archive_path, '--wildcards', '*provides.txt', '-O'], 
                                capture_output=True, text=True)
        if result.returncode == 0:
            provides = result.stdout.strip().replace('\n', ',')
        
        if not deps:
            print(f"  Note: No dependencies found in {os.path.basename(archive_path)}")
    except Exception as e:
        print(f"  Warning: Failed to extract metadata from {archive_path}: {e}")
    
    return deps, provides

def parse_pkg_filename(filename):
    # Matches name-version.tar.zst or name-version.lpkg
    # Simple version: everything before the last dash is name, after is version (until .extension)
    if not (filename.endswith('.tar.zst') or filename.endswith('.lpkg')):
        return None, None
    
    base = filename.rsplit('.', 1)[0]
    if filename.endswith('.tar.zst'):
        base = base.rsplit('.', 1)[0]
    
    if '-' not in base:
        return None, None
    
    name, version = base.rsplit('-', 1)
    return name, version

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
                "architecture": "amd64"
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
                                      # 禁用掉引起该问题的 checksum 计算
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

        st_type = self.config["storage"]["type"]
        arch = self.config.get("architecture", "amd64")
        prefix = self.config["storage"].get("path_prefix", "").strip('/')
        if prefix: prefix += '/'

        index_local = self.download_index()
        index_entries = self.parse_index(index_local)

        for f in files:
            path = Path(f)
            if not path.is_file(): continue
            
            name, version = parse_pkg_filename(path.name)
            if not name:
                print(f"Skipping {path.name}: Invalid filename format (expected name-version.tar.zst)")
                continue
            
            print(f"Processing {name} {version}...")
            sha256 = calculate_sha256(f)
            deps, provides = extract_metadata(f)
            
            # Upload package
            remote_pkg_path = f"{prefix}{arch}/{name}/{version}/app.tar.zst"
            self.upload_file(str(path), remote_pkg_path)
            
            # Upload hash.txt
            with tempfile.NamedTemporaryFile(mode='w', delete=False, encoding='utf-8') as tmp:
                tmp.write(sha256)
                tmp_path = tmp.name
            self.upload_file(tmp_path, f"{prefix}{arch}/{name}/{version}/hash.txt")
            os.unlink(tmp_path)

            # Upload latest.txt
            with tempfile.NamedTemporaryFile(mode='w', delete=False, encoding='utf-8') as tmp:
                tmp.write(version)
                tmp_path = tmp.name
            self.upload_file(tmp_path, f"{prefix}{arch}/{name}/latest.txt")
            os.unlink(tmp_path)

            # Update index entry
            index_entries[name] = f"{name}|{version}|{sha256}|{deps}|{provides}"

        # Save and upload index
        new_index_content = "\n".join(index_entries.values()) + "\n"
        with open(index_local, 'w', encoding='utf-8') as f:
            f.write(new_index_content)
        
        self.upload_file(index_local, f"{prefix}{arch}/index.txt")
        print("Done.")

    def delete_package(self, name, version=None):
        arch = self.config.get("architecture", "amd64")
        prefix = self.config["storage"].get("path_prefix", "").strip('/')
        if prefix: prefix += '/'
        
        index_local = self.download_index()
        index_entries = self.parse_index(index_local)

        if version:
            print(f"Deleting {name} version {version}...")
            remote_pkg_dir = f"{prefix}{arch}/{name}/{version}/"
            self.delete_remote_dir(remote_pkg_dir)
            # If this was the version in index, remove it from index
            if name in index_entries:
                entry_ver = index_entries[name].split('|')[1]
                if entry_ver == version:
                    del index_entries[name]
                    print(f"Removed {name} from index (was version {version})")
        else:
            print(f"Deleting all versions of {name}...")
            remote_pkg_root = f"{prefix}{arch}/{name}/"
            self.delete_remote_dir(remote_pkg_root)
            if name in index_entries:
                del index_entries[name]
                print(f"Removed {name} from index")

        # Save and upload index
        new_index_content = "\n".join(index_entries.values()) + "\n"
        with open(index_local, 'w', encoding='utf-8') as f:
            f.write(new_index_content)
        self.upload_file(index_local, f"{prefix}{arch}/index.txt")
        print("Done.")

    def cleanup_repository(self):
        """Removes all versions from storage that are NOT in the index.txt"""
        print("Cleaning up historical packages...")
        arch = self.config.get("architecture", "amd64")
        prefix = self.config["storage"].get("path_prefix", "").strip('/')
        if prefix: prefix += '/'
        
        index_local = self.download_index()
        index_entries = self.parse_index(index_local)
        
        # Build map of name -> active_version
        active_versions = {}
        for name, line in index_entries.items():
            active_versions[name] = line.split('|')[1]

        # List all packages in storage
        remote_base = f"{prefix}{arch}/"
        all_pkgs = self.list_remote_dirs(remote_base)
        
        for pkg in all_pkgs:
            if pkg not in active_versions:
                print(f"Package {pkg} is not in index, deleting entire package directory...")
                self.delete_remote_dir(f"{remote_base}{pkg}/")
                continue
            
            # Check versions of this package
            active_ver = active_versions[pkg]
            all_vers = self.list_remote_dirs(f"{remote_base}{pkg}/")
            for ver in all_vers:
                if ver != active_ver:
                    print(f"Deleting old version: {pkg} {ver}...")
                    self.delete_remote_dir(f"{remote_base}{pkg}/{ver}/")
        
        print("Cleanup complete.")

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
            # Ensure remote directory exists
            remote_dir = os.path.dirname(full_remote)
            subprocess.run(["ssh", f"{user}@{host}", f"mkdir -p {remote_dir}"], check=True)
            subprocess.run(["scp", local_path, f"{user}@{host}:{full_remote}"], check=True)

    def delete_remote_dir(self, remote_dir_prefix):
        st = self.config["storage"]
        if st["type"] == "s3":
            client = self.get_storage()
            # S3 delete objects with prefix
            paginator = client.get_paginator('list_objects_v2')
            pages = paginator.paginate(Bucket=st["bucket"], Prefix=remote_dir_prefix)
            
            for page in pages:
                if 'Contents' in page:
                    for obj in page['Contents']:
                        print(f"  Deleting: {obj['Key']}")
                        client.delete_object(Bucket=st["bucket"], Key=obj['Key'])
        elif st["type"] == "scp":
            host = st["host"]
            user = st["user"]
            remote_base = st["remote_path"].rstrip('/')
            subprocess.run(["ssh", f"{user}@{host}", f"rm -rf {remote_base}/{remote_dir_prefix}"], check=True)

    def list_remote_dirs(self, prefix):
        st = self.config["storage"]
        dirs = []
        if st["type"] == "s3":
            client = self.get_storage()
            paginator = client.get_paginator('list_objects_v2')
            # Use delimiter to get common prefixes (like directories)
            pages = paginator.paginate(Bucket=st["bucket"], Prefix=prefix, Delimiter='/')
            for page in pages:
                if 'CommonPrefixes' in page:
                    for cp in page['CommonPrefixes']:
                        # cp['Prefix'] is like "prefix/pkg/" or "prefix/pkg/ver/"
                        name = cp['Prefix'][len(prefix):].rstrip('/')
                        if name:
                            dirs.append(name)
        elif st["type"] == "scp":
            host = st["host"]
            user = st["user"]
            remote_base = st["remote_path"].rstrip('/')
            full_path = f"{remote_base}/{prefix}"
            result = subprocess.run(["ssh", f"{user}@{host}", f"ls -1F {full_path} 2>/dev/null | grep '/$' | sed 's|/||'"], 
                                    capture_output=True, text=True)
            if result.returncode == 0:
                dirs = [d.strip() for d in result.stdout.split('\n') if d.strip()]
        return dirs

    def download_index(self):
        st = self.config["storage"]
        arch = self.config.get("architecture", "amd64")
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
        except Exception:
            # If index doesn't exist, start with empty
            with open(local_path, 'w', encoding='utf-8') as f:
                pass
        
        return local_path

    def parse_index(self, path):
        entries = {}
        if os.path.exists(path):
            with open(path, 'r', encoding='utf-8') as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith('#'): continue
                    name = line.split('|')[0]
                    entries[name] = line
        return entries

def main():
    parser = argparse.ArgumentParser(description="LankeOS Repository Manager")
    parser.add_argument("-c", "--config", default="lrepo-mgr.json", help="Path to config file")
    subparsers = parser.add_subparsers(dest="command")

    push_parser = subparsers.add_parser("push", help="Push packages to repository")
    push_parser.add_argument("patterns", nargs="+", help="File patterns/directories to push")

    delete_parser = subparsers.add_parser("delete", help="Delete package or specific version from repository")
    delete_parser.add_argument("package", help="Package name or name:version")

    subparsers.add_parser("cleanup", help="Remove all historical versions not in index.txt")

    config_parser = subparsers.add_parser("config", help="View or modify config")
    config_parser.add_argument("--set", metavar="KEY=VALUE", nargs="+", help="Set config values (e.g. storage.access_key=XXX)")
    config_parser.add_argument("--show", action="store_true", help="Show current config (hides secrets by default)")
    config_parser.add_argument("--show-secrets", action="store_true", help="Show current config with secrets")

    args = parser.parse_args()

    mgr = RepoManager(args.config)

    if args.command == "push":
        mgr.push_packages(args.patterns)
    elif args.command == "delete":
        if ':' in args.package:
            name, ver = args.package.split(':', 1)
            mgr.delete_package(name, ver)
        else:
            mgr.delete_package(args.package)
    elif args.command == "cleanup":
        mgr.cleanup_repository()
    elif args.command == "config":
        if args.set:
            for item in args.set:
                key_path, value = item.split('=', 1)
                keys = key_path.split('.')
                d = mgr.config
                for k in keys[:-1]:
                    d = d.setdefault(k, {})
                d[keys[-1]] = value
            mgr.save_config()
            print("Config updated.")
        elif args.show or args.show_secrets:
            cfg = json.loads(json.dumps(mgr.config)) # Deep copy
            if not args.show_secrets:
                if "storage" in cfg:
                    if "secret_key" in cfg["storage"]: cfg["storage"]["secret_key"] = "********"
                    if "access_key" in cfg["storage"]: cfg["storage"]["access_key"] = cfg["storage"]["access_key"][:4] + "********"
            print(json.dumps(cfg, indent=4))
    else:
        parser.print_help()

if __name__ == "__main__":
    main()
