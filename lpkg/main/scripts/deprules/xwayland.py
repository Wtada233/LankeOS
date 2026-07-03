"""
xwayland — X11 依赖 → xwayland 注入规则

如果一个包通过 DT_NEEDED 依赖 X11 库（libX11.so.6 等），
但它本身不是 X 基础设施包，则自动添加 xwayland 作为依赖。
"""

__rule_name__ = 'xwayland'
__rule_description__ = 'X11 DT_NEEDED → xwayland 依赖注入'

# 已知 X11 相关 SONAME
X11_SONAMES = frozenset({
    'libX11.so.6', 'libXext.so.6', 'libXau.so.6', 'libXdmcp.so.6',
    'libxcb.so.1',
    'libxcb-composite.so.0', 'libxcb-damage.so.0', 'libxcb-dpms.so.0',
    'libxcb-dri2.so.0', 'libxcb-dri3.so.0', 'libxcb-glx.so.0',
    'libxcb-present.so.0', 'libxcb-randr.so.0', 'libxcb-record.so.0',
    'libxcb-render.so.0', 'libxcb-res.so.0', 'libxcb-screensaver.so.0',
    'libxcb-shape.so.0', 'libxcb-shm.so.0', 'libxcb-sync.so.1',
    'libxcb-xf86dri.so.0', 'libxcb-xfixes.so.0', 'libxcb-xinerama.so.0',
    'libxcb-xinput.so.0', 'libxcb-xkb.so.1', 'libxcb-xtest.so.0',
    'libxcb-xv.so.0', 'libxcb-xvmc.so.0',
    'libxcb-ewmh.so.2', 'libxcb-icccm.so.4', 'libxcb-image.so.0',
    'libxcb-keysyms.so.1', 'libxcb-render-util.so.0', 'libxcb-util.so.1',
    'libxcb-xrm.so.0', 'libxcb-keysyms.so.1',
    'libXcomposite.so.1', 'libXcursor.so.1', 'libXdamage.so.1',
    'libXfixes.so.3', 'libXi.so.6', 'libXinerama.so.1',
    'libXpresent.so.1', 'libXrandr.so.2', 'libXrender.so.1',
    'libXres.so.1', 'libXScrnSaver.so.1', 'libXtst.so.6',
    'libXv.so.1', 'libXxf86vm.so.1', 'libXfont2.so.2',
    'libxkbfile.so.1', 'libxshmfence.so.1',
    'libXmu.so.6', 'libXt.so.6', 'libXaw.so.7', 'libXpm.so.4',
    'libXft.so.2', 'libXtrans.so.2', 'libfontenc.so.1',
    'libXxf86dga.so.1',
})

# 不应该加 xwayland 的包（X 基础设施自身）
SKIP_PKGS = frozenset({
    'xwayland', 'xorg-server',
    'xorgproto', 'xtrans', 'xcb-proto',
    'libX11', 'libxcb', 'libxcb-stub', 'libXau', 'libXdmcp',
    'libXcomposite', 'libXcursor', 'libXdamage', 'libXext',
    'libXfixes', 'libXfont2', 'libXinerama', 'libXi',
    'libXpresent', 'libXrandr', 'libXrender', 'libXres',
    'libXScrnSaver', 'libXtst', 'libXv', 'libXxf86vm',
    'libxkbfile', 'libxshmfence', 'libxmu', 'libxt',
    'libice', 'libsm', 'xcb-util-wm', 'xcb-util', 'xcb-util-keysyms',
    'xkbcomp', 'xkeyboard-config', 'xauth', 'libxcvt',
    'fontenc', 'libpciaccess', 'util-macros',
    'libglvnd', 'mesa',
})


def rule(scan_result, deps, needed_so, provider_map, context):
    """检测 X11 SONAME → 注入 xwayland 依赖。"""
    pkg_name = scan_result.get('pkg_name', '')
    if pkg_name in SKIP_PKGS:
        return deps, needed_so

    # 获取包的 DT_NEEDED
    all_needed = needed_so or list(scan_result.get('needs', []))

    if any(soname in X11_SONAMES for soname in all_needed):
        if 'xwayland' not in deps:
            deps['xwayland'] = None
            pkg_ver = scan_result.get('pkg_version', '')
            print(f'      ↳ [xwayland] {pkg_name}-{pkg_ver}: 检测到 X11 依赖，注入 xwayland')

    return deps, needed_so
