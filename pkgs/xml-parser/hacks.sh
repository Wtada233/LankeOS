#!/bin/bash
# XML::Parser 的 Makefile.PL use File::ShareDir::Install，构建前需装（cpan 非交互）。
# -T 跳过测试加速；PERL_MM_USE_DEFAULT 避免 cpan 首次运行交互配置。
export PERL_MM_USE_DEFAULT=1
cpan -T -i File::ShareDir::Install || exit 1
