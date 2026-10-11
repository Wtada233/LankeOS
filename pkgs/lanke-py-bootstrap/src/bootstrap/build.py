# SPDX-License-Identifier: MIT

import argparse
import json
import logging
import pathlib
import shutil
import sys

from typing import Dict, Optional
from collections.abc import Sequence

import bootstrap


def main_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '--outdir',
        '-o',
        type=str,
        default='dist',
        help='output directory (defaults to `dist`)',
    )
    return parser


def main(cli_args: Sequence[str], prog: Optional[str] = None):
    parser = main_parser()
    if prog:
        parser.prog = prog
    args, unknown = parser.parse_known_args(cli_args)

    logging.basicConfig(level=logging.INFO)

    outdir = pathlib.Path(args.outdir).absolute()
    if outdir.exists():
        if not outdir.is_dir():
            raise NotADirectoryError(f"{str(outdir)} exists and it's not a directory")
        shutil.rmtree(outdir)
    outdir.mkdir(parents=True)

    bootstrap.install_package_egginfo('setuptools')

    artifacts = {
        package: bootstrap.build_package(package, outdir)
        for package in bootstrap.PACKAGES
    }
    # and finally, we generate the artifact metadata
    outdir.joinpath('artifacts.json').write_text(json.dumps({
        package: path.name for package, path in artifacts.items()
    }))

    print(f'Written wheels to `{str(args.outdir)}`:' + ''.join(sorted(
        f'\n\t{artifact.name}' for artifact in artifacts.values()
    )))


if __name__ == '__main__':
    main(sys.argv[1:], 'python -m bootstrap.build')
