# SPDX-License-Identifier: MIT

import atexit
import logging
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

from collections.abc import Collection, Mapping, Sequence
from typing import NamedTuple, Optional, Tuple


class Package(NamedTuple):
    srcdir: pathlib.Path
    module_path: pathlib.Path
    module_sources: Collection[str]


ROOT = pathlib.Path(__file__).parent.parent
WORKING_DIR = ROOT / '.bootstrap'
MODULES = WORKING_DIR / 'modules'
TMPDIR = WORKING_DIR / 'tmp'
EXTERNAL = ROOT / 'external'

PACKAGES = {
    # what we need
    'build': Package(
        EXTERNAL / 'build',
        EXTERNAL / 'build' / 'src',
        {'build'},
    ),
    'installer': Package(
        EXTERNAL / 'installer',
        EXTERNAL / 'installer' / 'src',
        {'installer'},
    ),
    # dependencies
    'flit_core': Package(
        EXTERNAL / 'flit' / 'flit_core',
        EXTERNAL / 'flit' / 'flit_core',
        {'flit_core'},
    ),
    'wheel': Package(
        EXTERNAL / 'wheel',
        EXTERNAL / 'wheel' / 'src',
        {'wheel'},
    ),
    'packaging': Package(
        EXTERNAL / 'packaging',
        EXTERNAL / 'packaging' / 'src',
        {'packaging'},
    ),
    'pyproject-hooks': Package(
        EXTERNAL / 'pyproject-hooks',
        EXTERNAL / 'pyproject-hooks' / 'src',
        {'pyproject_hooks'},
    ),
    'setuptools': Package(
        EXTERNAL / 'setuptools',
        EXTERNAL / 'setuptools',
        {'setuptools', 'pkg_resources', '_distutils_hack'},
    ),
}

EXTRA_PATH = [str(package.module_path) for package in PACKAGES.values()]
PACKAGE_PATH_ENV = {
    'PYTHONPATH': os.path.pathsep.join(EXTRA_PATH),
}


# copy sources to module dir and inject it into sys.path
MODULES.mkdir(parents=True)
for package in PACKAGES.values():
    for path in package.module_sources:
        # LankeOS：新版 setuptools 已删 pkg_resources，缺失的模块源跳过而不是炸掉
        src = package.module_path / path
        if not src.is_dir():
            continue
        shutil.copytree(
            src,
            MODULES / path,
        )
atexit.register(shutil.rmtree, MODULES)
sys.path.insert(0, str(MODULES))


# import what we need from the injected modules
import build  # noqa: E402
import pyproject_hooks  # noqa: E402


_logger = logging.getLogger(__name__)


def log(msg):
    _logger.info(msg)


def custom_runner(
    cmd: Sequence[str],
    cwd: Optional[str] = None,
    extra_environ: Optional[Mapping[str, str]] = None,
) -> None:
    extra_environ = dict(extra_environ) if extra_environ else {}
    extra_environ.update(PACKAGE_PATH_ENV)
    pyproject_hooks.default_subprocess_runner(cmd, cwd, extra_environ)


# HACK: this is required for setuptools to be able to use bdist_wheel,
# previously we installed the egg_info for wheel but as the project has been
# moved to PEP517 this is no longer possible. So now we hack setup.cfg to be
# aware that bdist_wheel can be loaded from wheel via our custom set PYTHONPATH.
def install_package_egginfo(name: str) -> None:
    package = PACKAGES[name]

    setup_cfg = package.srcdir / 'setup.cfg'
    # LankeOS：源是 sdist（无 .git），改前存原文，末尾靠它还原（Arch 用 git checkout）
    setup_cfg_original = setup_cfg.read_text()
    setup_cfg.write_text(setup_cfg_original.replace(
        'bdist_rpm\n', 'bdist_rpm\n\tbdist_wheel = wheel.bdist_wheel:bdist_wheel\n'))

    subprocess.check_call(
        [sys.executable, 'setup.py', 'egg_info'],
        env=os.environ | PACKAGE_PATH_ENV,
        cwd=package.srcdir,
    )
    shutil.copytree(
        package.module_path / f'{name}.egg-info',
        MODULES / f'{name}.egg-info',
    )

    setup_cfg.write_text(setup_cfg_original)


def build_package(name: str, outdir: pathlib.Path) -> pathlib.Path:
    log(f'Building {name}...')
    srcdir = PACKAGES[name].srcdir
    builder = build.ProjectBuilder(str(srcdir), runner=custom_runner)
    wheel = builder.build('wheel', str(outdir))
    return pathlib.Path(wheel)
