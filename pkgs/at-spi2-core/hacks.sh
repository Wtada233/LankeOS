#!/bin/bash
# g-ir-scanner（gobject-introspection 的 giscanner/utils.py）import distutils，
# python 3.12+ 已把 distutils 从 stdlib 移除。setuptools 提供 distutils shim。
python -m ensurepip
pip3 install setuptools
