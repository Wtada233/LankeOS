#!/bin/bash
# Run as root
python -m ensurepip
pip3 install setuptools
# PEP 517 wheel 构建/安装（Arch 同款：python -m build --wheel + python -m installer）
pip3 install build installer wheel
