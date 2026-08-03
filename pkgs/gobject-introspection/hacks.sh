#!/bin/bash
# gobject-introspection 的 meson.build 检查 setuptools（python 模块），构建前需装。
python -m ensurepip
pip3 install setuptools
