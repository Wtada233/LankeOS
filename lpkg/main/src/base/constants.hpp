#pragma once
#include <sys/types.h>

#include <cstdint>
#include <string_view>

/**
 * 非交互模式 —— 控制是否以及如何自动响应用户提示（**全局作用域**，不是
 * `constants::` 的成员：它是个运行模式，不是常量，且全仓 66 处按裸名使用）。
 *
 * 定义在 **base**（而不是 `config/config.hpp`）的理由（2026-10-03 修分层倒置）：
 * `base/utils.hpp` 的确认提示要用它，而 base 是最底层 —— 放在 config 会让
 * `base → config` 反向依赖，同时 config 那一侧又依赖 base，两边成环。
 * 枚举本身与"配置"无关，只是"怎么回答提示"这个运行模式，下移不损失任何语义。
 */
enum class NonInteractiveMode {
    INTERACTIVE,  // 默认：交互式，等待用户输入
    YES,          // 自动回答"是"
    NO            // 自动回答"否"
};

/**
 * lpkg 全局常量命名空间
 * 集中管理所有字符串常量，包括分隔符、JSON 键名、路径、命令名等
 */
namespace constants
{
// 分隔符字符常量
inline constexpr std::string_view NL = "\n";
inline constexpr std::string_view TAB = "\t";
inline constexpr char PIPE_CHAR = '|';
inline constexpr char COMMA_CHAR = ',';
inline constexpr char COLON_CHAR = ':';
inline constexpr char SEMICOLON_CHAR = ';';

// 包元数据 JSON 键名
inline constexpr std::string_view J_NAME = "name";
inline constexpr std::string_view J_VERSION = "version";
inline constexpr std::string_view J_RELEASE = "release";
inline constexpr std::string_view J_MAN = "man";
inline constexpr std::string_view J_DEPS = "deps";
inline constexpr std::string_view J_PROVIDES = "provides";
inline constexpr std::string_view J_NEEDED_SO = "needed_so";
inline constexpr std::string_view J_NO_STRIP = "no_strip";
inline constexpr std::string_view J_SOURCES = "sources";
inline constexpr std::string_view J_WORK_SOURCES = "work_sources";
inline constexpr std::string_view J_BUILD_DEPS = "build_deps";
inline constexpr std::string_view J_KEEP_FS_LAYOUT = "keep_fs_layout";

// 构建标志 JSON 键名（覆盖 build_defaults.hpp 的默认值，空串 = 用默认）
inline constexpr std::string_view J_CFLAGS = "cflags";
inline constexpr std::string_view J_CXXFLAGS = "cxxflags";
inline constexpr std::string_view J_LDFLAGS = "ldflags";
inline constexpr std::string_view J_MAKEFLAGS = "makeflags";
inline constexpr std::string_view J_LTO = "lto";

// 构建文件与脚本名
inline constexpr std::string_view LANK_BUILD_JSON = "LankeBUILD.json";
inline constexpr std::string_view LANK_BUILD_SCRIPT = "LankeBUILD";
inline constexpr std::string_view LANK_BUILD_PROCESSED = ".LankeBUILD_processed";

// 包元数据文件名
inline constexpr std::string_view PKG_METADATA_FILE = "metadata.json";

// 仓库与索引文件
inline constexpr std::string_view REPO_INDEX_FILE = "index.txt";
inline constexpr std::string_view REPO_INDEX_TMP = "repo_index.txt";
inline constexpr std::string_view PROTOCOL_FILE = "file://";
inline constexpr std::string_view VER_LATEST = "latest";
inline constexpr std::string_view CURRENT_DIR_PREFIX = "./";

// 内部目录名
inline constexpr std::string_view DIR_WORK = "work";
inline constexpr std::string_view DIR_HOOKS = "hooks";
inline constexpr std::string_view DIR_CONTENT = "content";

// 常见系统路径组件
inline constexpr std::string_view USR = "usr";
inline constexpr std::string_view USR_BIN = "usr/bin";
inline constexpr std::string_view USR_LIB = "usr/lib";
inline constexpr std::string_view USR_SBIN = "usr/sbin";
inline constexpr std::string_view USR_LIB64 = "usr/lib64";
inline constexpr std::string_view BIN = "bin";
inline constexpr std::string_view SBIN = "sbin";
inline constexpr std::string_view LIB = "lib";
inline constexpr std::string_view LIB64 = "lib64";
inline constexpr std::string_view INCLUDE = "include";
inline constexpr std::string_view SHARE_MAN = "share/man";
inline constexpr std::string_view DIR_ETC = "etc/";
inline constexpr std::string_view DIR_ETC_PREFIX = "/etc/";

// 脚本与 Shell 路径
inline constexpr std::string_view POSTINST_SH = "postinst.sh";
inline constexpr std::string_view PRERM_SH = "prerm.sh";
inline constexpr std::string_view BIN_BASH = "/bin/bash";

// 文件后缀与扩展名
inline constexpr std::string_view EXT_LPKG = ".lpkg";
inline constexpr std::string_view EXT_ZST = ".zst";
inline constexpr std::string_view EXT_LA = ".la";
inline constexpr std::string_view SUFFIX_LPKG_NEW = ".lpkgnew";
/// lpkg 版本 → libsolv EVR 的**发行修订号分隔符**（`+release` 在 EVR 里写成 `^^release`）。
/// 为什么是 `^`：libsolv 的 rpm 比较器把 caret 定义为"比基础版新、比任何真实下一段旧"，
/// 正是发行修订号的语义；且全串不含 `-` ⇒ libsolv 的 release 槽位永远为空、依赖匹配用的
/// `EVRCMP_MATCH_RELEASE` 特例分支不可能触发。**完整论证见 `vercmp/version.hpp`。**
inline constexpr std::string_view EVR_RELEASE_SEP = "^^";
/// 版本域里**不允许**出现的字符 —— 它们对 libsolv 的 EVR 解析都有特殊含义，混进 lpkg 版本
/// 就会让桥接的"编码 ↔ 解码"不再是一一对应（或让比较语义悄悄错位）：
///   `^` caret（本仓库拿它当 `+release` 的分隔符，见 `EVR_RELEASE_SEP`）；
///   `~` 预发布（lpkg 用 `-` 表达；`to_libsolv_evr` 会把 `-` 映射成它 ⇒ 原生的 `~` 会歧义）；
///   `:` epoch（libsolv 会把它前面当 epoch 切出去）。
/// 实测：真实索引 678 个版本里这三个字符**一个都没有**，所以拒它们不误伤任何现存包。
/// 落点校验（`is_safe_path_component`）与桥接（`to_libsolv_evr`）两处都拒。
inline constexpr std::string_view EVR_RESERVED_CHARS = "^~:";
/// 安装期"先写临时文件再 rename 到位"用的后缀（`<dst>.lpkgtmp`）。**唯一出处** ——
/// 此前它是 `archive.cpp` 的局部常量 + 两处裸字面量，正是"同一件事三处表达"的形态。
inline constexpr std::string_view SUFFIX_LPKG_TMP = ".lpkgtmp";
inline constexpr std::string_view SUFFIX_LPKG_BAK = ".lpkg_bak_";
/// 移除时配置文件改名保留的后缀（pacman 的 `.pacsave` 对应物）。
/// 已存在时**移位不覆盖**：旧的先改名成 `<路径>.lpkgsave.<N>`（N 从 1 起取第一个空闲后缀，
/// pacman 的 shift_pacsave），再把本次的配置落到 `<路径>.lpkgsave` —— 两条 rename 各是一条
/// 可回滚的 SAVE_CONF WAL 行，且**移位必须严格先于本次改名**（否则回滚会把上一批的旧存档
/// rename 到配置原位、盖掉真配置）。见 `detail::OpSink::save_config` 与 ARCH §7.2.1。
inline constexpr std::string_view SUFFIX_LPKG_SAVE = ".lpkgsave";
inline constexpr std::string_view SUFFIX_MAN = ".man";

/// confhashes.db 取值列里 `<包名>` 与 `<sha256>` 之间的分隔符
/// （见 `Config::conf_hashes_db()`：键是逻辑路径，取值是 `"<pkg>:<sha256>"`）
inline constexpr std::string_view CONF_HASH_SEP = ":";

// CLI 命令名
inline constexpr std::string_view CMD_INSTALL = "install";
inline constexpr std::string_view CMD_REMOVE = "remove";
inline constexpr std::string_view CMD_AUTOREMOVE = "autoremove";
inline constexpr std::string_view CMD_UPGRADE = "upgrade";
inline constexpr std::string_view CMD_REINSTALL = "reinstall";
inline constexpr std::string_view CMD_QUERY = "query";
inline constexpr std::string_view CMD_MAN = "man";
inline constexpr std::string_view CMD_PACK = "pack";
inline constexpr std::string_view CMD_BUILD = "build";
inline constexpr std::string_view CMD_FORCE_SOLVE = "force-solve-conflict";
inline constexpr std::string_view CMD_SCAN = "scan";
inline constexpr std::string_view CMD_DEPEND = "depend";
inline constexpr std::string_view CMD_REC = "rec";

// 默认值
inline constexpr std::string_view DEFAULT_PACK_SOURCE = "/tmp/lankepkg";

// ANSI 颜色码
inline constexpr std::string_view COLOR_GREEN = "\033[1;32m";
inline constexpr std::string_view COLOR_WHITE = "\033[1;37m";
// 下面两个对齐 pacman 的 `conf.c`（`colstr`）：`::` 前缀用 BOLDBLUE、正文用 BOLD。
inline constexpr std::string_view COLOR_BOLD = "\033[0;1m";
inline constexpr std::string_view COLOR_BOLDBLUE = "\033[1;34m";
inline constexpr std::string_view COLOR_YELLOW = "\033[1;33m";
inline constexpr std::string_view COLOR_RED = "\033[1;31m";
inline constexpr std::string_view COLOR_RESET = "\033[0m";

// 文件权限掩码与模式
inline constexpr mode_t PERM_MASK_ALL = 07777;
inline constexpr mode_t PERM_WAL_LOG = 0644;

// 随机后缀长度（字符集在 `base/utils.cpp` 的 `random_suffix()` 里）
inline constexpr size_t RANDOM_SUFFIX_LEN = 6;
// 网络下载
inline constexpr long CURL_CONNECT_TIMEOUT_SEC = 10;
inline constexpr long CURL_LOW_SPEED_LIMIT_BPS = 100;
inline constexpr long CURL_LOW_SPEED_TIME_SEC = 30;

// 归档成员大小上限 —— **唯一实现**，两处消费者共用：
//   · `archive.cpp` 的 `extract_file_from_archive`（按归档**自报**大小 resize 前判）；
//   · `install_common.cpp` 的 `read_package_metadata`（解析 JSON 前按盘上实际大小判）。
// 归档自报大小是**不可信输入**（GNU base-256 头能声明 1 TiB），无上限的 resize 会
// bad_alloc/abort 掉整个进程，而调用方只承诺抛 LpkgException。两边真正要读的都是
// `metadata.json`（几 KB），超过就是畸形/恶意归档。**别再各写一份**（2026-10-03 合并）。
inline constexpr size_t ARCHIVE_MEMBER_MAX_SIZE = 16 * 1024 * 1024;

// I/O 缓冲区大小
inline constexpr size_t ARCHIVE_BUFFER_SIZE = 10240;
inline constexpr size_t HASH_BUFFER_SIZE = 1024 * 1024;
inline constexpr size_t PACK_IO_BUFFER_SIZE = 8192;

// 备份重试
inline constexpr int UNIQUE_BAK_MAX_ATTEMPTS = 20;

// 进度上报间隔（文件数）

// ELF 段对齐掩码
inline constexpr uint64_t ELF_SECTION_ALIGN_MASK = 15;
}  // namespace constants
