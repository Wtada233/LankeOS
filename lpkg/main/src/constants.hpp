#pragma once
#include <string_view>

namespace constants {
    // Delimiters
    inline constexpr std::string_view NL = "\n";
    inline constexpr std::string_view TAB = "\t";
    inline constexpr char TAB_CHAR = '\t';
    inline constexpr char PIPE_CHAR = '|';
    inline constexpr char COMMA_CHAR = ',';
    inline constexpr char COLON_CHAR = ':';

    // Build Files and Scripts
    inline constexpr std::string_view LANK_BUILD_JSON = "LankeBUILD.json";
    inline constexpr std::string_view LANK_BUILD_SCRIPT = "LankeBUILD";
    inline constexpr std::string_view LANK_BUILD_PROCESSED = ".LankeBUILD_processed";

    // Package Metadata Filenames
    inline constexpr std::string_view PKG_MAN_FILE = "man.txt";
    inline constexpr std::string_view PKG_DEPS_FILE = "deps.txt";
    inline constexpr std::string_view PKG_PROVIDES_FILE = "provides.txt";
    inline constexpr std::string_view PKG_FILES_FILE = "files.txt";

    // Repository and Index
    inline constexpr std::string_view REPO_INDEX_FILE = "index.txt";
    inline constexpr std::string_view REPO_INDEX_TMP = "repo_index.txt";
    inline constexpr std::string_view PROTOCOL_FILE = "file://";
    inline constexpr std::string_view VER_LATEST = "latest";
    inline constexpr std::string_view VER_DEFAULT = "0.0.0";
    inline constexpr std::string_view CURRENT_DIR_PREFIX = "./";

    // Internal Directory Names
    inline constexpr std::string_view DIR_WORK = "work";
    inline constexpr std::string_view DIR_ROOT = "root";
    inline constexpr std::string_view DIR_HOOKS = "hooks";
    inline constexpr std::string_view DIR_CONTENT = "content";

    // Common System Path Components
    inline constexpr std::string_view USR = "usr";
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

    // Scripts and Shell
    inline constexpr std::string_view POSTINST_SH = "postinst.sh";
    inline constexpr std::string_view PRERM_SH = "prerm.sh";
    inline constexpr std::string_view BIN_SH = "/bin/sh";

    // File Suffixes and Extensions
    inline constexpr std::string_view EXT_LPKG = ".lpkg";
    inline constexpr std::string_view EXT_ZST = ".zst";
    inline constexpr std::string_view EXT_LA = ".la";
    inline constexpr std::string_view SUFFIX_LPKG_NEW = ".lpkgnew";
    inline constexpr std::string_view SUFFIX_LPKG_BAK = ".lpkg_bak_";
    inline constexpr std::string_view SUFFIX_MAN = ".man";
    inline constexpr std::string_view SUFFIX_TXT = ".txt";
    inline constexpr std::string_view SUFFIX_DIRS = ".dirs";
    inline constexpr std::string_view SUFFIX_PROVIDES = ".provides";

    // CLI Commands
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

    // Default Values
    inline constexpr std::string_view DEFAULT_PACK_SOURCE = "/tmp/lankepkg";

    // ANSI Color Codes
    inline constexpr std::string_view COLOR_GREEN = "\033[1;32m";
    inline constexpr std::string_view COLOR_WHITE = "\033[1;37m";
    inline constexpr std::string_view COLOR_YELLOW = "\033[1;33m";
    inline constexpr std::string_view COLOR_RED = "\033[1;31m";
    inline constexpr std::string_view COLOR_RESET = "\033[0m";
}
