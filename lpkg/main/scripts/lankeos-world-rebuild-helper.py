#!/usr/bin/env python3
"""
lankeos-world-rebuild-helper.py — LankeOS world rebuild orchestrator.

从远程索引解析包依赖关系，按自底向上顺序引导用户逐包重建。
对每个包：cd 进入目录 → 打开 shell 让用户修改 → 自动运行 lpkg build。
"""

import sys
import os
import glob
import subprocess
import urllib.request
import re
import collections
import argparse

# ---------------------------------------------------------------------------
# 索引解析
# ---------------------------------------------------------------------------

def fetch_index(url):
    """下载并返回 index.txt 内容"""
    print(f"[*] 正在获取索引: {url}")
    try:
        resp = urllib.request.urlopen(url, timeout=30)
        data = resp.read().decode("utf-8")
    except Exception as e:
        print(f"[!] 无法获取索引: {e}", file=sys.stderr)
        sys.exit(1)
    return data


def parse_index(content):
    """
    解析 index.txt，返回:
      - packages: { name → {version, deps: [name], needed_so: [soname]} }
        (取最新版本)
      - provides: { capability → [pkg_name] }
    """
    packages = {}
    provides = collections.defaultdict(list)

    for line in content.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue

        parts = line.split("|")
        if len(parts) < 2:
            continue

        pkg_name = parts[0]
        version_block = parts[1]
        # 可能的包级 provides (第三段)
        pkg_level_provides = parts[2] if len(parts) > 2 else ""

        # 版本块: version:hash:deps:provides:needed_so;version2:...
        for vinfo in version_block.split(";"):
            vinfo = vinfo.strip()
            if not vinfo:
                continue
            vparts = vinfo.split(":")
            if not vparts:
                continue
            version = vparts[0]

            deps_str = vparts[2] if len(vparts) > 2 else ""
            prov_str = vparts[3] if len(vparts) > 3 else ""
            needed_str = vparts[4] if len(vparts) > 4 else ""

            # 只要最新版本（按段落顺序最后一个）
            # 依赖解析：提取 deps 中的包名（去掉版本约束）
            dep_names = set()
            if deps_str:
                for dep_part in deps_str.split(","):
                    dep_part = dep_part.strip()
                    if not dep_part:
                        continue
                    # 去掉版本约束（如 glibc>=2.35 → glibc）
                    m = re.match(r"([A-Za-z0-9_+.-]+)", dep_part)
                    if m:
                        dep_names.add(m.group(1))

            # provides: 逗号分隔的 capabilities
            prov_caps = set()
            for p in prov_str.split(","):
                p = p.strip()
                if p:
                    prov_caps.add(p)
            for p in pkg_level_provides.split(","):
                p = p.strip()
                if p:
                    prov_caps.add(p)

            # needed_so: 逗号分隔的 SONAME 列表
            needed_so = set()
            for s in needed_str.split(","):
                s = s.strip()
                if s:
                    needed_so.add(s)

            # 记录
            packages[pkg_name] = {
                "version": version,
                "deps": dep_names,
                "provides": prov_caps,
                "needed_so": needed_so,
            }

            for cap in prov_caps:
                provides[cap].append(pkg_name)

    return packages, provides


# ---------------------------------------------------------------------------
# 依赖图构建
# ---------------------------------------------------------------------------

def resolve_needed_so(packages, provides):
    """
    构建链接依赖图。

    deps 和 needed_so 是 ELF DT_NEEDED 扫描的两种表示：
      - needed_so: RAW 数据（"libc.so.6"）
      - deps:      包级汇总（"glibc"），由 needed_so 解析 provider 得到
    它们是同一回事，不能叠加。用 needed_so 重建 provider 链路即可。
    """
    resolved = {}
    for name, info in packages.items():
        link_deps = set()
        for soname in info["needed_so"]:
            for prov in provides.get(soname, []):
                if prov != name:
                    link_deps.add(prov)
        link_deps.discard(name)

        resolved[name] = {
            "version": info["version"],
            "link_deps": link_deps,
        }
    return resolved


# ---------------------------------------------------------------------------
# 拓扑排序（自底向上）
# ---------------------------------------------------------------------------

def topo_sort(resolved):
    """
    Kahn 算法拓扑排序 + 循环检测。
    返回:
      - order: 自底向上的有序列表（依赖者靠后）
      - cycles: { pkg → [dep] } 被切断的循环依赖边（仅切实际构成循环的边）
    """
    # graph: pkg → set(link_deps) — 仅基于 ELF DT_NEEDED 的链接依赖
    graph = {}
    for name, info in resolved.items():
        graph[name] = set(info["link_deps"])
    for name in list(graph.keys()):
        for dep in graph[name]:
            graph.setdefault(dep, set())

    all_nodes = set(graph.keys())

    # reverse_graph: dep → set(packages that depend on it)
    rev = collections.defaultdict(set)
    for pkg, deps in graph.items():
        for dep in deps:
            rev[dep].add(pkg)

    # 入度 = 本包依赖数；队列从无依赖的包开始
    in_deg = {n: len(graph[n]) for n in all_nodes}
    queue = collections.deque(n for n in all_nodes if in_deg[n] == 0)
    order = []
    cycles = collections.defaultdict(list)

    while queue:
        node = queue.popleft()
        order.append(node)
        for depender in rev.get(node, set()):
            in_deg[depender] -= 1
            if in_deg[depender] == 0:
                queue.append(depender)

    # --- 循环检测：在剩余子图中 DFS 找实际环路 ---
    remaining = {n for n in all_nodes if in_deg.get(n, 0) > 0}

    def find_cycle(start, subgraph):
        """从 start 出发 DFS，找到一条回到 start 的路径"""
        visited = set()
        path = []

        def dfs(u):
            if u in visited:
                return False
            visited.add(u)
            for v in subgraph.get(u, set()):
                if v == start:
                    path.append((u, v))
                    return True
                if dfs(v):
                    path.append((u, v))
                    return True
            return False

        if dfs(start):
            return path[::-1]  # 正向路径
        return []

    # 反复处理剩余节点，每次切一条环边
    max_iter = len(all_nodes)  # 安全上限
    for _ in range(max_iter):
        if not remaining:
            break
        node = next(iter(remaining))
        subgraph = {n: (graph[n] & remaining) for n in remaining}
        cycle_path = find_cycle(node, subgraph)
        if not cycle_path:
            # 不在环中 —— 直接入队
            in_deg[node] = 0
            queue.append(node)
            while queue:
                n = queue.popleft()
                if n in remaining:
                    remaining.remove(n)
                    order.append(n)
                    for depender in rev.get(n, set()):
                        if depender in remaining:
                            in_deg[depender] -= 1
                            if in_deg[depender] == 0:
                                queue.append(depender)
            continue
        # 切最后一条边 (from → to)
        src, dst = cycle_path[-1]
        cycles[src].append(dst)
        in_deg[src] -= 1
        if in_deg[src] == 0:
            queue.append(src)
            while queue:
                n = queue.popleft()
                remaining.discard(n)
                order.append(n)
                for depender in rev.get(n, set()):
                    if depender in remaining:
                        in_deg[depender] -= 1
                        if in_deg[depender] == 0:
                            queue.append(depender)

    return order, cycles


# ---------------------------------------------------------------------------
# 构建循环
# ---------------------------------------------------------------------------

def build_loop(order, cycles, args):
    """逐包执行构建循环"""
    total = len(order)
    shell = os.environ.get("SHELL", "/bin/sh")

    original_cwd = os.getcwd()

    for idx, pkg in enumerate(order, 1):
        pkg_dir = os.path.join(original_cwd, pkg.lower())
        has_cycle = pkg in cycles

        print(f"\n{'='*60}")
        print(f"[{idx}/{total}] 包: {pkg}")
        print(f"{'='*60}")

        if has_cycle:
            cycle_deps = cycles[pkg]
            print(f"  ⚠  循环依赖警告: {pkg} 中存在已切断的循环依赖边")
            print(f"  ⚠  如果构建失败，请检查相关依赖是否已正确构建。")

        if not os.path.isdir(pkg_dir):
            print(f"  [!] 目录不存在，跳过: {pkg_dir}")
            continue

        # 验证目录中含有 LankeBUILD 和 LankeBUILD.json
        build_script = os.path.join(pkg_dir, "LankeBUILD")
        build_json = os.path.join(pkg_dir, "LankeBUILD.json")
        if not os.path.isfile(build_script):
            print(f"\n  [!] 错误: {pkg_dir}/LankeBUILD 不存在，不是合理的包目录。")
            sys.exit(1)
        if not os.path.isfile(build_json):
            print(f"\n  [!] 错误: {pkg_dir}/LankeBUILD.json 不存在，不是合理的包目录。")
            sys.exit(1)

        # 自增 LankeBUILD.json 中的 release 号
        import json as _json
        with open(build_json, "r") as _f:
            _cfg = _json.load(_f)
        _old_release = _cfg.get("release", 1)
        _cfg["release"] = _old_release + 1
        with open(build_json, "w") as _f:
            _json.dump(_cfg, _f, indent=2, ensure_ascii=False)
        print(f"  [~] release: {_old_release} → {_cfg['release']}")

        os.chdir(pkg_dir)

        # 第一步：开 shell 让用户修改
        print(f"  [*] 已进入 {pkg_dir}")
        print(f"  请在 shell 中完成修改后输入 exit 或 Ctrl-D 退出。")
        print(f"  退出后将自动执行构建。")
        print()
        subprocess.run([shell])

        # 构建重试循环
        while True:
            print(f"  [*] 正在构建 {pkg}...")
            print()
            rc = subprocess.run(["lpkg", "build", pkg_dir])

            if rc.returncode == 0:
                print(f"  [✓] {pkg} 构建成功")
                # 找到刚刚构建的 .lpkg 文件
                import glob as _glob
                lpkg_files = _glob.glob(os.path.join(pkg_dir, "*.lpkg"))
                if not lpkg_files:
                    print(f"  [!] 未找到 .lpkg 文件，跳过安装")
                    break
                lpkg_to_install = lpkg_files[0]
                print(f"  [*] 正在安装 {os.path.basename(lpkg_to_install)}...")
                inst_rc = subprocess.run(["lpkg", "install", lpkg_to_install])
                if inst_rc.returncode == 0:
                    print(f"  [✓] {pkg} 安装成功")
                else:
                    print(f"  [!] lpkg install 失败 (code={inst_rc.returncode})")
                break  # 无论安装是否成功，都进入下一个包

            # 构建失败 → 开 shell 让用户修复
            print(f"  [!] lpkg build 失败 (code={rc.returncode})")
            print(f"  [*] 正在打开 shell，请修复后输入 exit 退出。")
            print(f"  [*] 退出后将自动重试构建。")
            print()
            subprocess.run([shell])

    print(f"\n{'='*60}")
    print(f"全部完成! 共处理 {total} 个包。")
    print(f"{'='*60}")


# ---------------------------------------------------------------------------
# 主入口
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="LankeOS world rebuild helper — 按依赖顺序逐包重建。",
    )
    parser.add_argument(
        "index_url",
        nargs="?",
        default="https://lankerepo.wtada233.top/x86_64/index.txt",
        help="仓库索引 URL（默认: https://lankerepo.wtada233.top/x86_64/index.txt）",
    )
    parser.add_argument(
        "--skip-confirm", action="store_true",
        help="跳过用户确认，直接开始构建",
    )
    args = parser.parse_args()

    # 1. 获取索引
    content = fetch_index(args.index_url)

    # 2. 解析
    print("[*] 正在解析索引...")
    packages, provides = parse_index(content)
    print(f"    → {len(packages)} 个包, {len(provides)} 个 capabilities")

    # 3. 构建依赖图
    print("[*] 正在解析 needed_so → package 依赖...")
    resolved = resolve_needed_so(packages, provides)

    # 4. 拓扑排序
    print("[*] 正在计算构建顺序...")
    order, cycles = topo_sort(resolved)

    # 显示循环依赖
    if cycles:
        print(f"\n  [!] 检测到 {len(cycles)} 个循环依赖（已自动切断）:")
        for pkg, deps in sorted(cycles.items()):
            for d in deps:
                print(f"       {pkg} ↔ {d}（已切断 {pkg} → {d}）")

    # 5. 显示顺序
    print(f"\n  构建顺序 ({len(order)} 个包，自底向上):")
    for i, pkg in enumerate(order, 1):
        marker = " ⚠" if pkg in cycles else ""
        print(f"    {i:3d}. {pkg}{marker}")

    # 6. 用户确认
    if not args.skip_confirm:
        print()
        answer = input("[?] 按以上顺序开始重建？[Y/n] ").strip().lower() or "y"
        if answer.startswith("n"):
            print("  已取消。")
            sys.exit(0)

    # 7. 执行构建循环
    build_loop(order, cycles, args)


if __name__ == "__main__":
    main()
