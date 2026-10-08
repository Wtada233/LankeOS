#pragma once
#include <sys/types.h>

#include <cstdint>
#include <string_view>

/**
 * 非交互模式 —— 控制是否以及如何自动响应用户提示（**全局作用域**，不是
 * `constants::` 的成员：它是个运行模式，不是常量，且全仓 66 处按裸名使用）。
 *
 * 定义在 **base**（而不是 `config/config.hpp`）的理由：
 * `base/utils.hpp` 的确认提示要用它，而 base 是最底层 —— 放在 config 会让
 * `base → config` 反向依赖，同时 config 那一侧又依赖 base，两边成环。
 * 枚举本身与"配置"无关，只是"怎么回答提示"这个运行模式，下移不损失任何语义。
 */
enum class NonInteractiveMode {
    INTERACTIVE,  // 默认：交互式，等待用户输入
    YES,          // 自动回答"是"
    NO            // 自动回答"否"
};

/// 集中管理字符串常量：分隔符、JSON 键名、路径、命令名等。
namespace constants
{
// 分隔符字符常量
inline constexpr std::string_view NL = "\n";
inline constexpr char PIPE_CHAR = '|';
inline constexpr char COMMA_CHAR = ',';
inline constexpr char COLON_CHAR = ':';
inline constexpr char SEMICOLON_CHAR = ';';

// SONAME 规格（symbol version）的元字符 —— 见 `base/so_spec.hpp`：
//   `libc.so.6`（裸）、`libc.so.6@GLIBC_2.40`（单）、`libc.so.6@{GLIBC_2.40,GLIBC_2.39}`（多）
// ⚠️ 花括号里的逗号与 `COMMA_CHAR`（索引字段内的列表分隔符）是**同一个字符** ⇒ 切分必须
// 花括号感知（`split_so_list`）。这两个字段**不许含空白**，为的是保住"索引行零空白"。
inline constexpr char SO_SYMBOL_SEP = '@';
inline constexpr char SO_BRACE_OPEN = '{';
inline constexpr char SO_BRACE_CLOSE = '}';

// 包元数据 JSON 键名
inline constexpr std::string_view J_NAME = "name";
inline constexpr std::string_view J_VERSION = "version";
inline constexpr std::string_view J_RELEASE = "release";
inline constexpr std::string_view J_MAN = "man";
inline constexpr std::string_view J_DEPS = "deps";
inline constexpr std::string_view J_PROVIDES = "provides";
inline constexpr std::string_view J_PROVIDES_SONAME = "provides_soname";
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
/// 安装期"先写临时文件再 rename 到位"用的后缀（`<dst>.lpkgtmp`）。唯一出处。
inline constexpr std::string_view SUFFIX_LPKG_TMP = ".lpkgtmp";
inline constexpr std::string_view SUFFIX_LPKG_BAK = ".lpkg_bak_";
/// 移除时配置文件改名保留的后缀（pacman 的 `.pacsave` 对应物）。
/// 已存在时**移位不覆盖**：旧的先改名成 `<路径>.lpkgsave.<N>`（N 从 1 起取第一个空闲后缀，
/// pacman 的 shift_pacsave），再把本次的配置落到 `<路径>.lpkgsave` —— 两条 rename 各是一条
/// 可回滚的 SAVE_CONF WAL 行，且**移位必须严格先于本次改名**（否则回滚会把上一批的旧存档
/// rename 到配置原位、盖掉真配置）。见 `detail::OpSink::save_config` 与 ARCH §7.2.1。
inline constexpr std::string_view SUFFIX_LPKG_SAVE = ".lpkgsave";
inline constexpr std::string_view SUFFIX_MAN = ".man";

/// lpkg 自用文件名命名空间 —— **归档成员名**里出现这些名字一律整包拒绝。
///
/// 为什么需要它：这些都落在 lpkg **自己的 rename/落位**会撞上的名字上。
/// 包声明一个同名成员会与落位撞名，例如 `<dst>.lpkgtmp → <dst>` 的 rename 会盖掉包里的成员、
/// `.lpkgnew` 把"待用户审阅的配置"直接变成包内容、`.lpkgsave` 被 `save_config` 移位成
/// `<dst>.lpkgsave.<N>` 时把**另一个包**的同名文件挤开（归属当场脱节）；
/// `.lpkg_bak_` 更隐蔽：`cleanup_orphan_stashes()` 按这个前缀 + 尾段 pid 已死就 `remove_all`，
/// 包只要装出一个 `.lpkg_bak_<任意>_<已死 pid>/` 目录，**下一次任意 lpkg 命令就会把它静默删掉**。
///
/// 字段含义：`starts_with=false` → 分量**以** token **结尾**（追加在目标名之后的落位后缀）；
/// `starts_with=true` → 分量**以** token **开头**（自带包名与 pid 的变长名）。
/// 两者都**不是**子串匹配 —— `x.lpkgnewx` / `foo.lpkgtmp.txt` / `d.lpkg_bak_1` 是合法名字，
/// 必须照常解出（`tests/unit/test_archive_member_name_safety.cpp` 有明文的正面对照）。
///
/// ⚠️ **新增 lpkg 自用的落位名时，加进这一张表**：守卫（`archive/archive.cpp` 的
/// `member_name_rejection_message`，读侧解压与写侧 `packer.cpp` **共用同一份判据**）遍历它，
/// 不要在守卫里另列一遍 —— 当初的缺陷正是"常量头里有 `SUFFIX_LPKG_BAK`、守卫里只列了
/// 另外三个"。同理，`l10n` 的 `error.unsafe_member_suffix` 文案也要跟着列全。
struct ReservedMemberName {
    std::string_view token;
    bool starts_with;
};

inline constexpr ReservedMemberName RESERVED_MEMBER_NAMES[] = {
    {SUFFIX_LPKG_TMP, false},   // <dst>.lpkgtmp（先写临时文件再 rename 到位）
    {SUFFIX_LPKG_NEW, false},   // <dst>.lpkgnew（配置冲突，留给用户审阅）
    {SUFFIX_LPKG_SAVE, false},  // <dst>.lpkgsave（废弃/类型变化的配置改名保留）
    {SUFFIX_LPKG_BAK, true},    // .lpkg_bak_<pkg>_<pid>（每文件系统的 stash 根）
};

/// confhashes.db 取值列里 `<包名>` 与 `<sha256>` 之间的分隔符
/// （见 `Config::conf_hashes_db()`：键是逻辑路径，取值是 `"<pkg>:<sha256>"`）
inline constexpr std::string_view CONF_HASH_SEP = ":";

/// **SONAME 空间**的内部前缀：`needed_so` 的每条需求、以及每条 `provides_soname`，都带它。
///
/// 为什么需要命名空间：libsolv 的池里每个包都有一条**自提供**
/// （`<包名> = evr`），而"**不带版本的 provide 能满足任意 requires**"。若 SONAME 与包名
/// 共用一个命名空间，就会互相串 —— `needed_so: o` 会被**名叫 `o` 的包**满足。
///
/// 池里因此只有**两个**命名空间，与两个字段一一对应（**全程零字符串形状判断**）：
///   · **裸名** —— 「包名 + 虚拟能力」：包的自提供、以及 `provides` 的每一条。
///     `deps` 的需求走这一路 ⇒ 既能匹配**包名**、也能匹配**虚拟 provides**（虚拟包语义）。
///   · **`so:`** —— 「SONAME」：`needed_so` 的需求与 `provides_soname` 的每一条。
///     ⇒ `needed_so` 只能被 `provides_soname` 满足，包名永远进不来；反之亦然。
/// 裸名撞不进 `so:` 空间这件事由**两处**共同保证，缺一不可：
///   · **包名**不可能含 `:` —— `is_safe_path_component` 把 `:` 当**分帧字符**拒掉；
///   · **`provides` 的每一项**由读入处校验（`reject_reserved_provides_prefix`，
///     `pkg/install_common.cpp`）—— 它走的是与包名**同一条裸名路径**，而
///     `reject_unsafe_metadata_tokens` 只拒 `\0\n\r\t`、不拒 `:`，所以这道校验不能省：
///     少了它，一条 `provides: ["so:libfoo.so.1"]` 就能与 `needed_so: ["libfoo.so.1"]` 灌出
///     同一个 pool id —— 求解器当 SONAME 接受，安装期的 `soname_satisfied()`（只看
///     `provides_soname`）拒绝，重现"求解器说能装、安装期拒装"的分叉。
/// ⚠️ 它是**内部编码**：进了用户可见的冲突消息必须先剥掉（见 `decode_libsolv_message`）。
inline constexpr std::string_view POOL_SONAME_PREFIX = "so:";

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
// `metadata.json`（几 KB），超过就是畸形/恶意归档。**别再各写一份**。
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
