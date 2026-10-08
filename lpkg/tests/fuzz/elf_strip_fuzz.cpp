// harness #1：`elf/strip.cpp` 的 strip_elf_data()。
//
// 为什么是它：`strip_elf_data` 手写 ELF 节区重写 —— 用 `reinterpret_cast<Hdr*>` 就地把输入
// 头/节区表改掉（`strip.cpp`），memcpy 长度直接取 `sh_size`（:553-554）。输入是
// **不可信上游构建产物**（`builder.cpp` 对 staging 下每个 magic 命中的文件调它；
// `lib_utils.hpp` 明写"上游构建产物（不可信输入）"）。这条路径上已经真出过一个
// ASan heap-buffer-overflow（`e_ehsize` 那条守卫，`strip.cpp` 就是为它加的）。
//
// oracle 只有一条：**返回 true ⇒ 输出必须仍是 libelf 能解析的合法 ELF，且
// class/data/type/machine 逐项与输入一致**。不断言"符号变少""节区数"这类语义 —— 那是 strip
// 的功能，不是不变量，而且合法的 strip 产物本来就可以没有节区表（`strip.cpp` 正是
// "无节区 = 无事可做"，它走的是返回 false）。

#include <elf.h>
#include <gelf.h>
#include <libelf.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "fuzz_common.hpp"  // mem_trace（LPKG_FUZZ_MEM_TRACE=1 时开）

// `strip_elf_data` 是**外部链接**的（`strip.cpp`），但任何头文件里都没有声明
// （`strip.hpp` 只导出 `strip_file` / `strip_binary`），所以在这里自己声明一次。
// **签名必须与 strip.cpp 里那份逐字一致**，改那边要同步这里。
//
// ⚠️ 第 4 个参数 `source_path` 只为让 `error.strip_output_grew` 这条错误能点名文件 ——
// 两边**同步到同一个签名**，别再留第二个签名。
bool strip_elf_data(const std::vector<uint8_t>& input_data, std::vector<uint8_t>& output_data,
                    std::string& error_msg, const std::string& source_path);

namespace
{

/// 独立地把一串字节当 ELF 解析一遍（不复用 strip 内部的任何状态）。
/// 只要求拿到 ehdr —— 剥离后的合法 ELF 可以只有一个头。
bool parses_as_elf(const std::vector<uint8_t>& bytes, GElf_Ehdr& out_hdr)
{
    Elf* elf =
        elf_memory(const_cast<char*>(reinterpret_cast<const char*>(bytes.data())), bytes.size());
    if (elf == nullptr) return false;
    const bool ok = gelf_getehdr(elf, &out_hdr) != nullptr;
    elf_end(elf);
    return ok;
}

/// oracle 违反 —— **先把原因打到 stderr 再崩**。
///
/// 为什么必须先打：`__builtin_trap()` 出的是 SIGILL，而 ASan 的信号处理器打印的是**处理器
/// 自己**的栈 —— 触发点会丢（栈里只剩 `__sanitizer_print_stack_trace`/`PrintStackTrace`
/// 那几帧，一条我们自己的帧都没有）。没有这行输出，四个断言点炸出来长得一模一样。
/// stderr 是**没有**被静音的（见 fuzz_common.hpp 的说明）。
///
/// 用 `__builtin_trap` 而不是 `assert`：NDEBUG 下 assert 会消失。
[[noreturn]] void oracle_violation(const char* why)
{
    std::fprintf(stderr, "[fuzz] oracle 违反: %s\n", why);
    __builtin_trap();
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    // libelf 的版本是进程级的一次性初始化；`strip_elf_data` 内部也会调（`strip.cpp`），
    // 这里先调一次只是让 oracle 那侧不依赖调用顺序。
    elf_version(EV_CURRENT);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    mem_trace(data, size, "elf_strip");  // 定因用：跳变时把当前输入落盘（默认关闭）

    // 空输入是**有意保留**的形态：`strip.cpp` 会拿 `input_data.data()`（空 vector 时
    // 可能是 nullptr）和长度 0 去调 `elf_memory`。
    std::vector<uint8_t> input(data, data + size);
    std::vector<uint8_t> output;
    std::string error_msg;

    // 路径只影响错误消息的措辞（本 harness 不读 error_msg），给个固定名即可。
    if (!strip_elf_data(input, output, error_msg, "<fuzz-input>")) {
        // 拒绝是正常结局。注意**不能**在这里断言 error_msg 非空：`strip.cpp`
        // （`shnum == 0`）是有意的静默跳过，false 时 error_msg 就是空的。
        return 0;
    }

    // 返回 true 却给了空输出 —— 这是缺陷，不是"无事可做"（后者会返回 false）。
    if (output.empty()) oracle_violation("返回 true 但输出为空");

    GElf_Ehdr in_hdr{};
    GElf_Ehdr out_hdr{};
    // strip 返回 true 说明它自己成功解析了输入；这里独立复核一遍，顺带拿到输入的头做比对。
    if (!parses_as_elf(input, in_hdr)) {
        oracle_violation("strip 接受了输入，但输入不能被独立解析成 ELF");
    }
    if (!parses_as_elf(output, out_hdr)) {
        oracle_violation("输出不能被 libelf 重新解析成 ELF");
    }

    if (in_hdr.e_ident[EI_CLASS] != out_hdr.e_ident[EI_CLASS] ||
        in_hdr.e_ident[EI_DATA] != out_hdr.e_ident[EI_DATA] || in_hdr.e_type != out_hdr.e_type ||
        in_hdr.e_machine != out_hdr.e_machine) {
        std::fprintf(stderr,
                     "[fuzz] 头字段被改了: class %u→%u, data %u→%u, type %u→%u, machine %u→%u\n",
                     in_hdr.e_ident[EI_CLASS], out_hdr.e_ident[EI_CLASS], in_hdr.e_ident[EI_DATA],
                     out_hdr.e_ident[EI_DATA], static_cast<unsigned>(in_hdr.e_type),
                     static_cast<unsigned>(out_hdr.e_type), static_cast<unsigned>(in_hdr.e_machine),
                     static_cast<unsigned>(out_hdr.e_machine));
        oracle_violation("输出的 class/data/type/machine 与输入不一致");
    }

    return 0;
}
