# LankeOS User Profile

if [ "$(tty)" = "/dev/tty1" ]; then
    export LANG=zh_CN.UTF-8
    export LC_ALL=zh_CN.UTF-8    
    exec sway
fi

if [ -f ~/.bashrc ]; then
  . ~/.bashrc
fi
