#pragma once
#include <string_view>

/**
 * lpkg 全局常量命名空间
 * 集中管理所有字符串常量，包括分隔符、JSON 键名、路径、命令名等
 */
namespace constants {
    // 分隔符字符常量
    inline constexpr std::string_view NL = "\n";
    inline constexpr std::string_view TAB = "\t";
    inline constexpr char TAB_CHAR = '\t';
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
    inline constexpr std::string_view VER_DEFAULT = "0.0.0";
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
    inline constexpr std::string_view LOCAL_BIN = "local/bin";
    inline constexpr std::string_view LIBEXEC = "libexec";
    inline constexpr std::string_view DIR_ETC = "etc/";
    inline constexpr std::string_view DIR_ETC_PREFIX = "/etc/";

    // 脚本与 Shell 路径
    inline constexpr std::string_view POSTINST_SH = "postinst.sh";
    inline constexpr std::string_view PRERM_SH = "prerm.sh";
    inline constexpr std::string_view BIN_SH = "/bin/sh";

    // 文件后缀与扩展名
    inline constexpr std::string_view EXT_LPKG = ".lpkg";
    inline constexpr std::string_view EXT_ZST = ".zst";
    inline constexpr std::string_view EXT_LA = ".la";
    inline constexpr std::string_view SUFFIX_LPKG_NEW = ".lpkgnew";
    inline constexpr std::string_view SUFFIX_LPKG_BAK = ".lpkg_bak_";
    inline constexpr std::string_view SUFFIX_MAN = ".man";

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
    inline constexpr std::string_view CMD_SCAN = "scan";
    inline constexpr std::string_view CMD_DEPEND = "depend";

    // 默认值
    inline constexpr std::string_view DEFAULT_PACK_SOURCE = "/tmp/lankepkg";

    // ANSI 颜色码
    inline constexpr std::string_view COLOR_GREEN = "\033[1;32m";
    inline constexpr std::string_view COLOR_WHITE = "\033[1;37m";
    inline constexpr std::string_view COLOR_YELLOW = "\033[1;33m";
    inline constexpr std::string_view COLOR_RED = "\033[1;31m";
    inline constexpr std::string_view COLOR_RESET = "\033[0m";
}
