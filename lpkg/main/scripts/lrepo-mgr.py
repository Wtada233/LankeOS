#!/usr/bin/env python3
"""
lrepo-mgr.py — LankeOS 仓库管理（**纯本地模式**）

仓库布局（index.txt 的格式由 lpkg 拥有，本脚本只转录、不扩展）：

    <repo>/<arch>/index.txt            聚合索引（每行一个包）
    <repo>/<arch>/<name>/<version>.lpkg 包文件

索引行格式：

    name|ver:sha256:deps:provides:needed_so;ver2:...|

用法：

    lrepo-mgr.py --path /srv/lankerepo push ./pkgs/*.lpkg
    lrepo-mgr.py --path /srv/lankerepo delete foo
    lrepo-mgr.py --path /srv/lankerepo delete foo:1.2.3
    lrepo-mgr.py --path /srv/lankerepo cleanup [--dry-run]

> 曾支持 S3 / SCP 在线推送（`storage.type` 配置 + 远程索引下载）。该模式已**整体移除**：
> 远程推送依赖一次"下载当前索引 → 本地改 → 上传"，而下载失败会被静默当成"空索引"，
> 于是 push 会用只含本次包的新索引覆盖线上索引（其余包的条目全部消失），cleanup 更会
> 据此删掉整个架构目录。现在只保留本地仓库构建：索引读写都在本地文件上做，
> 读失败**硬失败**（不再与"新仓库"混淆），写索引走 .tmp + rename 原子替换。
"""

import argparse
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


def calculate_sha256(file_path):
    sha256_hash = hashlib.sha256()
    with open(file_path, "rb") as f:
        for byte_block in iter(lambda: f.read(4096), b""):
            sha256_hash.update(byte_block)
    return sha256_hash.hexdigest()


def _read_archive_metadata(archive_path):
    """从 .lpkg 中抽出 metadata.json（tar --use-compress-program=zstd -O），返回 dict 或 None。"""
    try:
        result = subprocess.run(
            ['tar', '--use-compress-program=zstd', '-xf', archive_path,
             '--wildcards', '*metadata.json', '--exclude', 'content/*', '-O'],
            capture_output=True, text=True)
        if result.returncode == 0 and result.stdout.strip():
            return json.loads(result.stdout)
    except Exception as e:
        print(f"  Warning: Failed to read metadata.json from {archive_path}: {e}")
    return None


def read_metadata_from_archive(archive_path):
    """返回 (name, version)；无法读取时返回 (None, None)。"""
    meta = _read_archive_metadata(archive_path)
    if meta is None:
        return None, None
    return meta.get('name', ''), meta.get('version', '')


def extract_metadata(archive_path):
    """返回 (deps, provides, needed_so)，均为逗号连接的字符串。"""
    meta = _read_archive_metadata(archive_path)
    if meta is None:
        return "", "", ""
    return (",".join(meta.get('deps', [])),
            ",".join(meta.get('provides', [])),
            ",".join(meta.get('needed_so', [])))


class RepoManager:
    """本地仓库管理。所有操作都落在 <repo>/<arch>/ 下，不涉及任何网络/远端。"""

    def __init__(self, config_path, repo_path=None):
        self.config_path = config_path
        self.load_config()
        if repo_path:
            self.config["repo_path"] = os.path.abspath(repo_path)

    # ── 配置 ────────────────────────────────────────────────────────────

    def load_config(self):
        if not os.path.exists(self.config_path):
            self.config = {"repo_path": os.path.abspath("lankerepo"),
                           "architecture": "x86_64"}
            self.save_config()
            print(f"Created default config at {self.config_path}. Please edit it.")
        else:
            with open(self.config_path, 'r') as f:
                self.config = json.load(f)
            if "storage" in self.config:
                # 旧配置（在线模式）里的 storage 段已无用；保留 path 作为仓库根
                old = self.config.pop("storage") or {}
                if old.get("type") == "local" and old.get("path"):
                    self.config.setdefault("repo_path", old["path"])
                self.save_config()

    def save_config(self):
        with open(self.config_path, 'w') as f:
            json.dump(self.config, f, indent=4)

    # ── 路径 ────────────────────────────────────────────────────────────

    def arch(self):
        return self.config.get("architecture", "x86_64")

    def arch_dir(self):
        return Path(self.config["repo_path"]) / self.arch()

    def index_path(self):
        return self.arch_dir() / "index.txt"

    def ensure_layout(self):
        self.arch_dir().mkdir(parents=True, exist_ok=True)
        if not self.index_path().exists():
            with open(self.index_path(), 'w', encoding='utf-8') as f:
                f.write("# LankeOS repo index\n")

    # ── 索引读写 ────────────────────────────────────────────────────────

    def read_index(self):
        """读取本地索引。文件不存在 = 全新仓库（空索引）；**存在但读不了/解析不了 = 硬失败**。

        这两者绝不能混为一谈：旧在线模式把"下载失败"当成"空索引"，随即用只含本次包的
        索引覆盖线上索引（其余包条目全部消失）。本地模式下同理——读失败必须中止，
        而不是让调用方在"空索引"上继续做增删。
        """
        path = self.index_path()
        if not path.exists():
            return {}
        try:
            with open(path, 'r', encoding='utf-8') as f:
                content = f.read()
        except OSError as e:
            sys.exit(f"ERROR: 无法读取索引 {path}: {e}（拒绝在未知状态下继续）")
        try:
            return self.parse_aggregated_index(content)
        except Exception as e:
            sys.exit(f"ERROR: 索引 {path} 解析失败: {e}（拒绝在未知状态下继续）")

    def write_index(self, data):
        """原子写索引（.tmp + rename）：中断不会留下半截索引。"""
        self.ensure_layout()
        path = self.index_path()
        tmp = str(path) + ".tmp"
        with open(tmp, 'w', encoding='utf-8') as f:
            for name, info in data.items():
                blocks = []
                for v, vinfo in info["versions"].items():
                    blocks.append(f"{v}:{vinfo['sha256']}:{vinfo['deps']}:"
                                  f"{vinfo.get('provides', '')}:{vinfo.get('needed_so', '')}")
                f.write(f"{name}|{';'.join(blocks)}|\n")
        os.replace(tmp, path)

    @staticmethod
    def parse_aggregated_index(content):
        """格式: name|ver:hash:deps:provides:needed_so;ver2:...|"""
        data = {}
        for line in content.splitlines():
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split('|')
            if len(parts) < 2:
                continue
            name = parts[0]
            for v_block in parts[1].split(';'):
                v_info = v_block.split(':')
                if len(v_info) < 2:
                    continue
                data.setdefault(name, {"versions": {}})
                data[name]["versions"][v_info[0]] = {
                    "sha256": v_info[1],
                    "deps": v_info[2] if len(v_info) > 2 else "",
                    "provides": v_info[3] if len(v_info) > 3 else "",
                    "needed_so": v_info[4] if len(v_info) > 4 else "",
                }
        return data

    # ── 命令 ────────────────────────────────────────────────────────────

    def push_packages(self, patterns):
        files = []
        for p in patterns:
            files.extend(glob.glob(p))
        files = [f for f in files if Path(f).is_file()]
        if not files:
            print("No files matched patterns.")
            return

        self.ensure_layout()
        index_data = self.read_index()   # 读失败即在此中止（不会覆盖成"只含本次包"）
        pushed = 0

        for f in files:
            path = Path(f)
            name, version = read_metadata_from_archive(str(path))
            if not name or not version:
                print(f"Skipping {path.name}: could not determine name/version "
                      f"(missing metadata.json?)")
                continue

            dest_dir = self.arch_dir() / name
            dest_dir.mkdir(parents=True, exist_ok=True)
            dest = dest_dir / f"{version}.lpkg"

            # 先按包内容算哈希，再落盘/写索引：索引里的 sha256 必须对应真实的包内容
            sha256 = calculate_sha256(f)
            deps, provides, needed_so = extract_metadata(f)

            print(f"Publishing {name} {version} -> {dest}")
            if path.resolve() != dest.resolve():
                shutil.copy2(str(path), str(dest))  # 复制而非移动：源文件通常还在构建目录

            index_data.setdefault(name, {"versions": {}})
            index_data[name]["versions"][version] = {
                "sha256": sha256, "deps": deps,
                "provides": provides, "needed_so": needed_so,
            }
            pushed += 1

        # 包文件全部就位后再更新索引
        self.write_index(index_data)
        print(f"Done. {pushed} package(s) published, index: {self.index_path()}")

    def delete_package(self, name, version=None):
        index_data = self.read_index()

        if version:
            if name in index_data and version in index_data[name]["versions"]:
                print(f"Deleting {name} {version}...")
                self._remove_file(self.arch_dir() / name / f"{version}.lpkg")
                del index_data[name]["versions"][version]
                if not index_data[name]["versions"]:
                    del index_data[name]
                    self._remove_empty_dir(self.arch_dir() / name)
            else:
                print(f"Version {version} of {name} not found.")
                return
        else:
            print(f"Deleting all versions of {name}...")
            self._remove_dir(self.arch_dir() / name)
            index_data.pop(name, None)

        self.write_index(index_data)
        print("Done.")

    def cleanup_repository(self, dry_run=False):
        """删除不在索引里的历史包文件。先列清单（--dry-run 只看不动）。"""
        print(f"Cleaning up {self.arch_dir()} ...")
        index_data = self.read_index()   # 读失败即中止：绝不基于"空索引"清理
        if not index_data:
            print("索引为空（或只有一个注释行）：不做任何删除。")
            return

        removed = 0
        for pkg_dir in sorted(p for p in self.arch_dir().iterdir() if p.is_dir()):
            if pkg_dir.name not in index_data:
                print(f"  [not in index] delete directory {pkg_dir.name}/")
                if not dry_run:
                    self._remove_dir(pkg_dir)
                removed += 1
                continue
            active = index_data[pkg_dir.name]["versions"]
            for f in sorted(pkg_dir.glob("*.lpkg")):
                ver = f.name.rsplit('.', 1)[0]
                if ver not in active:
                    print(f"  [stale version] delete {pkg_dir.name}/{f.name}")
                    if not dry_run:
                        self._remove_file(f)
                    removed += 1
        print(f"Cleanup {'(dry-run) ' if dry_run else ''}complete: {removed} item(s).")

    # ── 本地文件操作 ────────────────────────────────────────────────────

    @staticmethod
    def _remove_file(p):
        if Path(p).exists():
            os.remove(p)

    @staticmethod
    def _remove_empty_dir(p):
        try:
            os.rmdir(p)
        except OSError:
            pass

    @staticmethod
    def _remove_dir(p):
        if Path(p).is_dir():
            shutil.rmtree(p)


def main():
    parser = argparse.ArgumentParser(
        description="LankeOS repository manager (local repo only)")
    parser.add_argument("-c", "--config", default="lrepo-mgr.json", help="Path to config file")
    parser.add_argument("--path", metavar="DIR",
                        help="Local repository root (overrides config repo_path)")
    subparsers = parser.add_subparsers(dest="command")

    push_parser = subparsers.add_parser("push", help="Publish .lpkg files into the repository")
    push_parser.add_argument("patterns", nargs="+", help="File patterns to publish")
    delete_parser = subparsers.add_parser("delete", help="Delete package or specific version")
    delete_parser.add_argument("package", help="Package name or name:version")
    cleanup_parser = subparsers.add_parser("cleanup",
                                           help="Remove versions/dirs not present in index.txt")
    cleanup_parser.add_argument("--dry-run", action="store_true", help="Only list what would go")
    config_parser = subparsers.add_parser("config", help="View or modify config")
    config_parser.add_argument("--set", metavar="KEY=VALUE", nargs="+", help="Set config values")
    config_parser.add_argument("--show", action="store_true", help="Show current config")

    args = parser.parse_args()
    mgr = RepoManager(args.config, args.path)
    mgr.ensure_layout()

    if args.command == "push":
        mgr.push_packages(args.patterns)
    elif args.command == "delete":
        if ':' in args.package:
            name, ver = args.package.split(':', 1)
            mgr.delete_package(name, ver)
        else:
            mgr.delete_package(args.package)
    elif args.command == "cleanup":
        mgr.cleanup_repository(dry_run=args.dry_run)
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
        elif args.show:
            print(json.dumps(mgr.config, indent=4))
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
