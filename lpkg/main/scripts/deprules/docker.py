"""
docker — Docker 运行时协议/命令依赖注入规则

docker 的运行时依赖大部分不在二进制 ABI（DT_NEEDED）里：
  • containerd — 通过 unix socket 通信（daemon 间协作，非二进制链接）
  • iproute2   — 网络命名空间 / 网桥搭建调用 ip 命令
  • iptables   — 防火墙/NAT 规则（nftables 后端）
  • libseccomp — seccomp 过滤（默认 profile 需要）
  • libtool    — 运行时脚本/链接辅助
  • nftables   — iptables 的 nftables 后端，docker 防火墙调用 nft 命令

这些是"协议/命令层"依赖，ELF 扫不出来，属于 deps 层（与 needed_so
的二进制 ABI 层分离）的职责，由规则显式注入。
"""

__rule_name__ = 'docker'
__rule_description__ = 'docker 运行时依赖注入（containerd/iproute2/iptables/libseccomp/libtool/nftables）'

DOCKER_RUNTIME_DEPS = ('containerd', 'iproute2', 'iptables', 'libseccomp', 'libtool', 'nftables')


def rule(scan_result, deps, needed_so, context):
    """为 docker 注入运行时协议/命令级依赖。"""
    pkg_name = scan_result.get('pkg_name', '')
    if pkg_name != 'docker':
        return deps, needed_so

    for dep in DOCKER_RUNTIME_DEPS:
        deps.setdefault(dep, None)

    pkg_ver = scan_result.get('pkg_version', '')
    print(f'      ↳ [docker] {pkg_name}-{pkg_ver}: 注入运行时依赖 '
          f'{", ".join(DOCKER_RUNTIME_DEPS)}')

    return deps, needed_so
