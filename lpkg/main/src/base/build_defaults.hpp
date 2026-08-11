#pragma once

#include <string>
#include <string_view>
#include <thread>

/**
 * lpkg 构建默认标志 — fallback 默认配置。
 *
 * 参考 Arch Linux makepkg.conf，但 ISA 基线默认 **x86-64-v3**（发行版打包基准）：
 *   绝不能默认 -march=native——native 会把构建宿主机的 CPU 特性烙进二进制，
 *   打包出的包在其它机器上可能 SIGILL，且违反"一个仓库包，任意满足基线的机器可用"。
 *
 * v3 基线（AVX2、FMA、BMI1/2 等）覆盖 2013 年以后的绝大部分主流 x86-64 机器；
 * 宿主比 v3 更高的特性（如 Alder Lake 的 AVX-VNNI 等）**不**进入包——用 v3 保证可移植。
 * 需要更高基线时用 LankeBUILD.json 的 cflags 覆盖（如 -march=x86-64-v4）或换机器构建。
 *
 * 每个包可通过 LankeBUILD.json 的 cflags / cxxflags / ldflags / makeflags / lto
 * 字段覆盖这些默认值（字符串字段为空 = 使用此处默认）。
 */
namespace build_defaults
{

/// 默认 CFLAGS（Arch makepkg.conf flags，ISA 基线 x86-64-v3）
inline constexpr std::string_view CFLAGS =
    "-march=x86-64-v3 -mtune=generic -O2 -pipe -fno-plt -fexceptions "
    "-Wp,-D_FORTIFY_SOURCE=3 -Wformat -Werror=format-security "
    "-fstack-clash-protection -fcf-protection "
    "-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer";

/// 默认 CXXFLAGS = CFLAGS + -Wp,-D_GLIBCXX_ASSERTIONS
inline constexpr std::string_view CXXFLAGS =
    "-march=x86-64-v3 -mtune=generic -O2 -pipe -fno-plt -fexceptions "
    "-Wp,-D_FORTIFY_SOURCE=3 -Wformat -Werror=format-security "
    "-fstack-clash-protection -fcf-protection "
    "-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer "
    "-Wp,-D_GLIBCXX_ASSERTIONS";

/// 默认 LDFLAGS
inline constexpr std::string_view LDFLAGS =
    "-Wl,-O1 -Wl,--sort-common -Wl,--as-needed -Wl,-z,relro -Wl,-z,now "
    "-Wl,-z,pack-relative-relocs";

/// LTO 标志（仅当 LankeBUILD.json 设 "lto": true 时追加到 CFLAGS/CXXFLAGS/LDFLAGS）
inline constexpr std::string_view LTOFLAGS = "-flto=auto";

/// 默认 make 并行度：-j<逻辑核心数>（等价 makepkg.conf 的 MAKEFLAGS="-j$(nproc)"）
inline std::string default_makeflags()
{
    unsigned n = std::thread::hardware_concurrency();
    if (n == 0) n = 1;
    return "-j" + std::to_string(n);
}

/**
 * 一次构建解析出的完整标志集合（resolve_build_flags 填充后各字段非空）。
 */
struct BuildFlags {
    std::string cflags;    ///< 编译 C 标志
    std::string cxxflags;  ///< 编译 C++ 标志
    std::string ldflags;   ///< 链接标志
    std::string makeflags; ///< make 并行标志（-jN）
    std::string ltoflags;  ///< LTO 标志（lto:true 时追加到编译与链接标志）
};

}  // namespace build_defaults
