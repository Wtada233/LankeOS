"""
deprules — gen_deps.py 的规则插件系统。

每个 .py 文件是一个独立规则，导出以下接口：

    __rule_name__ = "短名称"              # 必填
    __rule_description__ = "描述"          # 必填

    def rule(scan_result, deps, needed_so, provider_map, context):
        '''
        修改 deps / needed_so（原地修改，无需返回值）。

        参数:
            scan_result : dict — Phase 1 扫描结果
            deps        : dict[str, None]  已解析的包依赖（SONAME 层）
            needed_so   : list[str]        已收集的 SONAME 列表
            provider_map: dict[str, dict]  SONAME → {pkg, pkg_version}
            context     : dict             包含 dry_run, rules_dir 等
        '''
        pass

规则文件按字母序执行。
"""

import importlib.util
import os
import sys


def discover_rules(rules_dir):
    """扫描 rules_dir 下的所有 .py 文件，返回按文件名排序的规则模块列表。"""
    if not os.path.isdir(rules_dir):
        return []

    files = sorted(
        f for f in os.listdir(rules_dir)
        if f.endswith('.py') and f != '__init__.py'
    )
    rules = []
    for f in files:
        filepath = os.path.join(rules_dir, f)
        name = os.path.splitext(f)[0]
        spec = importlib.util.spec_from_file_location(f'deprules.{name}', filepath)
        if spec is None or spec.loader is None:
            continue
        mod = importlib.util.module_from_spec(spec)
        mod.__package__ = 'deprules'
        try:
            spec.loader.exec_module(mod)
        except Exception as e:
            print(f'   [!!] 规则加载失败 {f}: {e}', file=sys.stderr)
            continue
        if hasattr(mod, 'rule'):
            rname = getattr(mod, '__rule_name__', name)
            rdesc = getattr(mod, '__rule_description__', '')
            rules.append((rname, rdesc, mod.rule))
            print(f'   [规则] {rname}: {rdesc}')
    return rules
