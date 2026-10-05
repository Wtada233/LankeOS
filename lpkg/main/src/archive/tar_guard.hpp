#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>

struct archive_entry;

/**
 * 畸形 / 敌意 tar 的结构性守卫 —— **跨成员有状态**，一个归档一个实例。
 *
 * tar 是 1979 年的**纯顺序**格式：没有中央目录、没有成员索引、没有一个字段被强制校验。
 * 头就是 512 字节的定长块，`name` 只是一个 100 字节的字符串字段 —— 于是下面这些形态在
 * 格式上**完全合法**，只能由消费者自己拒：
 *
 *   · **重名成员**：同一路径出现两次，后一份静默覆盖前一份。流式读（`archive_read_data`）
 *     拿到第一份、解压落盘是最后一份 ⇒ **校验与使用读了不同对象**。
 *     本仓库真实踩过：`metadata.json` 放两份（第一份照索引写、第二份写 payload）就能同时
 *     骗过「与索引逐字段比对」和「装进 DB 的那份」（见 `pkg/package_manager.cpp` 的
 *     `verify_package_metadata` 与 `pkg/installation_task.cpp` 的 `read_package_metadata`）。
 *   · **`.` / `..` 分量**：`ARCHIVE_EXTRACT_SECURE_NODOTDOT` 校验的是**本实现重写后**的
 *     路径，不是原始成员名 —— 兜不住"名字本身带 `..`"这件事。
 *   · **链接目标**：三个 `ARCHIVE_EXTRACT_SECURE_*` 开关**都不检查 link target 的文本**。
 *   · **控制字符**：会把异常消息自己切成两行、或用 ANSI 序列污染终端。
 *   · **超长路径**：libarchive 已知会在超长路径上绕过它自己的符号链接检查
 *     （libarchive issue #744/#745）。
 *
 * 违规一律抛 `UnsafeArchiveException`（**整包拒绝**）—— 与成员名守卫、特殊文件类型守卫
 * 同一粒度。理由同 `archive.hpp` 里那条：归档已经表达了畸形/恶意意图，"跳过该成员继续"
 * 只会让用户拿到一个装了一半、某文件莫名消失的包；而 `UnsafeArchiveException` 是构建期
 * **不会**被"宽容普通失败"吞掉的那一类（见 `base/exception.hpp`）。
 *
 * ── 判据的接入点：只有 `extract_tar_zst`（2026-10-05 的取舍）──────────────────
 *
 * **为什么重名成员只需要守解压这一条路**：安装流程里**每个**包都要经 `extract_tar_zst`
 * 解压进 `tmp_pkg_dir_`，而这里是**整包拒绝**。于是"校验读第一份、安装用最后一份"这条链
 * 在**安装完成之前**就断了 —— 无论哪一份是伪造的：
 *   · 第一份说谎 ⇒ `verify_package_metadata`（逐字段比索引）当场拒；
 *   · 第二份说谎 ⇒ 校验放行，但随后的解压拒绝整包，**装不进去**。
 * 两个方向都到不了"磁盘上装了个没校验过的东西"。
 *
 * 原本还打算把守卫接进 `extract_file_from_archive`（流式取 `metadata.json`），并让它
 * **扫到 EOF** 才能发现后面的重名成员。**实测代价后放弃**：`packer.cpp` 把 `metadata.json`
 * 写在**第一个**成员，所以该函数正常情况下读一个头就返回；改成扫到 EOF 会让**每次**元数据
 * 读取都把整个包解压一遍（本地 `.lpkg` 候选、索引校验各一次，随后真正的解压再一次）。
 * 861 个真实包里最大的是 164 MiB（libreoffice）—— 那是把每条安装路径的 I/O 翻倍的代价，
 * 换来的却只是"与解压期守卫重复"的纵深防御。**宁可少一层冗余，也不要一条在热路径上悄悄
 * 变慢的检查**（这类"防御性但昂贵"的判据最容易在别处被人删掉，留下一半）。
 * 将来若真要在那条路上也判重名，正确做法是让它也走一次完整扫描并**显式承担**这个成本，
 * 而不是像最初设想的那样顺手加上。
 *
 * ── 明确**不采纳**的通用加固建议（每条都有理由，别照着通用清单"补全"）────────
 *
 * 1. **不剥 setuid/setgid**：本仓库的真实包**依赖它**（实测 `dbus`/`linux-pam`/`shadow`/
 *    `util-linux`/`sudo` 里的 `dbus-daemon-launch-helper`、`unix_chkpwd`、`passwd`、
 *    `sudo`、`su`、`mount`、`chfn/chsh`、`newuidmap/newgidmap`、`wall`(setgid) 全是
 *    setuid/setgid）。剥掉 = 系统坏掉。libarchive 的 `ARCHIVE_EXTRACT_PERM|OWNER` 原样恢复
 *    这些位是**有意**的。
 * 2. **不拒绝对目标的符号链接**：`--root` 下包发 `<root>/usr/bin/foo -> /etc/foo` 合法且
 *    必要（`tests/unit/test_archive_confinement.cpp` 的 `AbsoluteSymlinkTargetIsPreservedAsIs`
 *    钉着）。相对目标则按"符号所在目录 + 目标"**词法归一化后是否仍在根内**判 —— 这样
 *    `usr/bin/foo -> ../lib/foo`（合法、常见）放行，`usr/bin/x -> ../../../../etc/passwd`
 *    （逃出根）拒绝。用「目标里有没有 `..`」当判据会误杀前者。
 * 3. **不拒反斜杠**：通用清单里"反斜杠 = 跨平台 zip-slip"排得很前，但**误伤真实包** ——
 *    `systemd` 的成员 `system-systemd\x2dmute\x2dconsole.slice` 文件名里**字面就带反斜杠**
 *    （unit 名转义惯例），而它是 base 包。全仓 861 个包扫下来仅此一例，扫出来的当天就删掉了
 *    这条判据（见 `tar_guard.cpp` 里的记录与绊线用例）。
 * 4. **不改文件类型黑名单为白名单**：`archive.cpp` 里那条注释（本函数同时服务**源码
 *    tarball** 解压，某些 tar 变体的 filetype 可能是 0 或本代码不认识的值）成立，白名单会
 *    误伤合法源码包。黑名单已覆盖 FIFO/字符/块/socket —— 保持不变。
 * 5. **不拒非 UTF-8 名字**：上游源码 tarball 里带 Latin-1 等历史编码的文件名真实存在；
 *    只拒**控制字符**（含 ESC），那才是日志/终端注入的载体。
 * 6. **不做资源上界**（成员数 / 解压比 / 累计尺寸）：维护者 2026-10-05 明确拍板不加。
 *
 * ── 已核实为**不可实现**的（写在这里，免得下轮当"漏了"再报一次）─────────────
 * · **内嵌 NUL**：`archive_entry_pathname()` 返回的是 **C 串**，NUL 之后的字节在 libarchive
 *   的 API 层面观测不到，本模块无从判定。按仓库纪律（不为走不到的分支写断言），
 *   只记录为已知边界，不写检查、不写用例。
 * · **PAX/GNU 扩展头与 ustar 头不一致**：libarchive 已经把 PAX 记录**合并**进 entry，
 *   两个来源的原始值都拿不到，同样无法在本层判定。
 */
class TarGuard
{
public:
    /**
     * 单个成员的头级检查。头已读、尚未写盘时调用。
     *
     * @param entry        libarchive 的成员
     * @param raw_name     成员的**原始**名字（未归一化；`archive_entry_pathname` 的返回值）
     * @param relative     `member_path_relative()` 归一化后的**根内相对路径**（目录条目带尾斜杠）
     * @param archive_path 仅用于错误消息：用户要知道是哪个归档被拒
     * @throw UnsafeArchiveException 命中任一判据（整包拒绝）
     */
    /// 注：`entry` **不是 const** —— libarchive 的 `archive_entry_filetype` / `_symlink`
    /// 签名收的是非 const 指针（函数本身不改内容）。这里照它的签名走，别加 const 再强转。
    void check(struct archive_entry* entry, std::string_view raw_name, const std::string& relative,
               const std::filesystem::path& archive_path);

private:
    /// 已见过的**归一化相对路径**（重名判据）。目录条目带尾斜杠，与文件天然不同键。
    std::unordered_set<std::string> seen_;
};
