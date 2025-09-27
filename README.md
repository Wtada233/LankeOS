# LankeOS
The public repo of LankeOS.
LankeOS的公用仓库。

## 关于LankeOS
 - LankeOS是一个基于Arch Linux的发行版
 - 内置lpkg,防火墙等实用软件（防火墙还没有弄好）
 - 我们的理念是让每一台计算机都能够便携地运行这个操作系统，提供即插即用的体验。

## FAQ
 - Q: LankeOS和Arch Linux是什么关系？
 - A: LankeOS是基于Arch Linux的文件系统并添加一些组件的“魔改版系统”，所以不是一个Arch Linux的fork，事实上它并没有修改或使用Arch Linux的源码，而是在Arch Linux的基础上添加了更多软件。
 - Q: 为什么无法使用lpkg？
 - A: 目前开发者都是学生党买不起服务器，默认镜像是127.0.0.1,想要使用可以自行搭建服务器
 - Q: 这个项目的规模怎么样？
 - A: 这是一个完全业余的项目，如果你愿意，也可以参加！
 - Q: 使用遇到问题怎么办？
 - A: 千万别去Arch论坛！我们对系统的魔改可能造成一些兼容性问题，如果你清楚Manjaro用户不应该去Arch论坛寻求帮助，那么你应该同样清楚关于LankeOS的正确求助方式：给这个仓库提issue
 - Q: Arch Linux是滚动发行版，为什么LankeOS有版本号？
 - A: LankeOS有版本号是因为目前没有架设服务器，只能这样推送lpkg等软件的更新。如果你使用旧版的LankeOS，你可以重新安装或者手动编译新版的组件。使用maint脚本可以保证Arch Linux提供的所有软件更新到最新。
