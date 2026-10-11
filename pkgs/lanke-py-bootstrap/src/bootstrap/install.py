# SPDX-License-Identifier: MIT

import sys

import bootstrap  # noqa: F401

# bootstrap injects the extra sources
import installer.__main__  # noqa: E402


if __name__ == '__main__':
    installer.__main__._main(sys.argv[1:], 'python -m bootstrap.install')
