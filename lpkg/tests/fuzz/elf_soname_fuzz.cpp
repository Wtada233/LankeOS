// harness #5：安装期的 SONAME 扫描 —— `get_elf_soname()`（`elf/lib_utils.cpp`）。
//
// 为什么是它：`apply_soname_links` 在**安装后的触发器**里对 `usr/lib` 下**每一个** ELF 调它，
// 以 **root** 跑，而输入来自**不可信包**（`lib_utils.hpp` 原话："上游构建产物（不可信输入）"）。
// 这条路径已经真出过事：libelf 在"DT_SONAME 的 offset 越出 `.dynstr` / `sh_link` 不是
// SHT_STRTAB / 该节不存在"时**返回 NULL**，而早先的代码直接把它赋给 `std::string`
// （libstdc++ 下等价于 `strlen(nullptr)` → SIGSEGV）⇒ **一个畸形 `.so` 就能让 root 进程在事务
// 中途段错误**。`elf_strip_fuzz` 碰不到它：那条 harness 只调 `strip_elf_data`，而这是**另一条
// 独立的 ELF 解析路径**（只读 `.dynamic`，不走 strip）。
//
// oracle：
//   ① **不崩**（历史缺陷就是崩）；
//   ② 返回非空 SONAME 时，它必须是**文件里的一个子串** —— 凭空造出来的字符串不可能来自输入
//      （这条防的是"读越界后拼出一段垃圾当 SONAME 返回"，而它会被拿去建符号链接名）。

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "elf/lib_utils.hpp"

namespace fs = std::filesystem;

namespace
{

constexpr const char* kDir = "/tmp/lpkgfz_soname";
constexpr const char* kFile = "/tmp/lpkgfz_soname/f.so";

[[noreturn]] void oracle_violation(const char* why, const std::string& detail)
{
    std::fprintf(stderr, "[fuzz] oracle 违反: %s（%s）\n", why, detail.c_str());
    __builtin_trap();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > (1u << 20)) return 0;
    std::vector<uint8_t> in(data, data + size);

    // 靠 magic 快速筛掉非 ELF：`get_elf_soname` 对它们会走 open+elf_begin 再返回空串，
    // 喂进去只是白花迭代（真正的形态空间在 ELF 内部）。
    if (size < 4 || in[0] != 0x7f || in[1] != 'E' || in[2] != 'L' || in[3] != 'F') return 0;

    std::error_code ec;
    fs::create_directories(kDir, ec);
    {
        FILE* f = std::fopen(kFile, "wb");
        if (f == nullptr) return 0;
        std::fwrite(in.data(), 1, in.size(), f);
        std::fclose(f);
    }

    const std::string soname = get_elf_soname(kFile);  // ← 被测入口（生产路径同一个）

    if (!soname.empty()) {
        // ②：SONAME 必须是输入里的字节子串。
        //
        // ⚠️ **必须按 `unsigned char` 比较**：`std::string` 的元素是 `char`（本机**有符号**），
        // 而 `in` 是 `uint8_t` —— 直接 `std::search(in.begin(), in.end(), soname.begin(), ...)`
        // 会把所有 ≥0x80 的字节判成**不相等**。这不是理论问题：**凡是 `.dynstr` 里带高位字节
        // 的 ELF 都会被误报成"SONAME 读越界"**（例如一个 16304 字节的畸变 `.so`，那 6 字节
        // 其实就在文件偏移 1167 处）。
        // 教训：**判据先拿"已知好"验一遍**（正常 `.so` 的 SONAME 全是 ASCII，所以没暴露）。
        const bool found =
            std::search(in.begin(), in.end(), soname.begin(), soname.end(),
                        [](uint8_t a, char b) { return a == static_cast<uint8_t>(b); }) != in.end();
        if (!found) {
            oracle_violation("返回的 SONAME 不是输入文件里的子串（疑似越界读出垃圾）", soname);
        }
    }
    return 0;
}
