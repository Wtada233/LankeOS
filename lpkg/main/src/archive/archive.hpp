#pragma once

#include <filesystem>
#include <string>
#include <string_view>

/**
 * @brief 归档成员名"危险"判定 —— `pack`（写侧）与解压（读侧）**共用的唯一判据**。
 *
 * 成员名会变成 WAL 行的**字面内容**（`op + " " + src + " → " + bak`），而 WAL 是行式协议，
 * 两侧都不转义 —— 含 `\n` 的名字能伪造 WAL 行、含字面 `" → "` 的名字能破坏箭头分帧，
 * `.lpkgtmp`/`.lpkgnew` 则会与 lpkg 自己的 rename/落位撞名。伤害发生在解压**之后**，
 * 唯一能挡的地方就是名字进入系统之前（判据的完整推导见 `archive.cpp` 的长注释）。
 *
 * 返回**空串** = 名字合法；否则返回**已填好参数**的错误消息（可直接交给异常）。
 *
 * @param member         待判定的成员名（未归一化，含前导 `./`·`/` 也不误判）
 * @param container_path 仅用于消息：解压侧传归档路径，打包侧传**输出**文件路径。
 *                       打包侧也要判的理由：不判的话 `lpkg pack` 能产出自己的 extractor
 *                       会拒收的包 —— 缺陷要拖到下游、甚至用户手上才暴露。
 *
 * 只判**内容**，不负责剥前导 `./`·`/`：那是解压侧"成员名 → 解压根内相对路径"的事，
 * 打包侧的名字本来就是相对的。
 */
std::string member_name_rejection_message(std::string_view member,
                                          const std::string& container_path);

/**
 * @brief 把成员名里的不可见字节渲染成可见形式（`\xHH`），供**异常消息**使用。
 *
 * 危险名字**本身就是攻击载荷**，不能原样进消息：含 ANSI 转义的名字会再污染一份终端，
 * 含 `\n` 的名字会把异常消息**自己**切成两行 —— 错误消息被行式消费的地方（日志、CLI
 * 输出）就重演了这里正在修的同一个 bug。
 *
 * 2026-10-05 起对外可见（原本是 `archive.cpp` 的 `static`）：`archive/tar_guard.cpp`
 * 也要用它，而"同一件事的第二份实现"正是这个仓库反复踩的坑。
 */
std::string escape_member_name(std::string_view name);

/**
 * @brief 解压 .tar.zst 存档到指定目录
 * @param archive_path 存档文件路径
 * @param output_dir   输出目录
 * @param label        被解压对象的**标签**，用于进度/完成日志点名（安装时是包名，
 *                     构建时是源码归档文件名）。多包批次里解压日志一个包接一个包地刷，
 *                     不带标签就分不清在解压谁。标签由调用方给（调用方知道自己在处理谁），
 *                     不从这里回头去猜。
 */
void extract_tar_zst(const std::filesystem::path& archive_path,
                     const std::filesystem::path& output_dir, const std::string& label);

/**
 * @brief 从存档中提取单个文件的内容（不完整解压）
 * @param archive_path  存档文件路径
 * @param internal_path 存档内的文件路径
 * @return 文件内容的字符串
 */
std::string extract_file_from_archive(const std::filesystem::path& archive_path,
                                      const std::string& internal_path);
