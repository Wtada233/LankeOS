# LankeOS Interactive Shell Config

export PS1='\[\033[01;32m\]\u@\h\[\033[00m\]:\[\033[01;34m\]\w\[\033[00m\]\$ '

alias ls='ls --color=auto'
alias ll='ls -alF'
alias l='ls -lah'
alias grep='grep --color=auto'

export PATH=/usr/bin:/usr/local/bin

# what a fancy logo! :D
fastfetch
