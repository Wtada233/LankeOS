#!/bin/bash
# Run as root
ln -sf /usr/bin/python3 /usr/bin/python
python -m ensurepip
pip3 install mako
