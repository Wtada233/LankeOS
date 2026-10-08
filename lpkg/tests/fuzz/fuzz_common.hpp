#pragma once

// 两个 fuzz harness 共用的东西。**只放都要用的**，别把这里变成杂物间。

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/**
 * 把 **stdout** 永久接到 /dev/null。
 *
 * 只静音 stdout、**必须保留 stderr**：
 *   · 进度行全走 stdout（`ui/term.cpp` 都是 `std::cout`），而
 *     `extract_tar_zst` 每轮都构造一个 `ui::Line`（`archive.cpp`），非 TTY 下析构也至少
 *     打一行 —— 不静音的话 libFuzzer 每秒几百次的输出会被进度行冲垮，还白搭 I/O；
 *   · 而 ASan/UBSan 的崩溃报告、libFuzzer 自己的统计、以及 `log_warning`/`log_error`
 *     （`base/utils.cpp` 里都走 `std::cerr`）**全在 stderr 上** —— 把 stderr 也关掉
 *     就等于把"为什么崩"一起关掉。
 *
 * 幂等；在 `LLVMFuzzerInitialize` 里调一次即可。
 */
inline void silence_stdout()
{
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull < 0) return;
    ::dup2(devnull, STDOUT_FILENO);
    ::close(devnull);
}

// ============================ 内存追踪（定因用） ============================
//
// **存在的理由**：曾有一次 `elf_strip_fuzz` 运行把宿主内存吃到 60 GB 触发 OOM，
// 事后**查不出是谁**：OOM 转储里所有任务的 rss_anon 加起来只有 1.1 GB、内核却声称
// `active_anon` 59.85 GB；fuzzer 自己的 fd 恒为 5、RssShmem=0。也就是说单看"跑完之后的
// 快照"根本定不了因 —— 必须**在跑的过程中**记录"第几次迭代开始涨、当时的输入是什么"。
//
// 默认**关闭**（走环境变量），因为每 500 次读一遍 /proc/meminfo 对吞吐有影响。
// 用法：LPKG_FUZZ_MEM_TRACE=1 LPKG_FUZZ_MEM_DUMP=<目录> ./<harness> ...
//   · 每 5000 次打一行常规采样（iter / 输入长度 / shmem / anon / 可用）；
//   · **采样窗口内 shmem 或 anon 涨超过 64 MB** → 判定为跳变，把**当前输入**落盘到
//     `<dump 目录>/memjump-<iter>-<len>.bin` 并打一行醒目的日志。
//     那个 .bin 就是复现用的 case，可直接喂 `-runs=1`。

inline bool mem_trace_enabled()
{
    static const bool on = std::getenv("LPKG_FUZZ_MEM_TRACE") != nullptr;
    return on;
}

/// 从 /proc/meminfo 读一个字段（单位 kB）；读不到返回 -1。
inline long meminfo_kb(const char* key)
{
    FILE* f = std::fopen("/proc/meminfo", "r");
    if (f == nullptr) return -1;
    char line[256];
    long value = -1;
    const std::size_t klen = std::strlen(key);
    while (std::fgets(line, sizeof line, f) != nullptr) {
        if (std::strncmp(line, key, klen) == 0) {
            value = std::atol(line + klen);
            break;
        }
    }
    std::fclose(f);
    return value;
}

/**
 * 读**本 cgroup** 的计数（`/sys/fs/cgroup/memory.stat`），单位 kB。
 *
 * 为什么需要它、而且**必须以它为准**：曾有一次运行里容器 cgroup 的 `shmem` 涨到 18 GB、
 * 把容器自己的内存上限撞穿，而**宿主的 `meminfo` `Shmem` 平在 150 MB、进程 RSS 也毫无变化**
 * （`anon=0`）。也就是说：**用 `/proc/meminfo` 或进程 RSS 做判据根本看不到这个泄漏**。
 * 只有 cgroup 的记账反映它。（容器内 `/sys/fs/cgroup` 就是它自己的 cgroup，直接读即可。）
 */
inline long memcg_kb(const char* key)
{
    FILE* f = std::fopen("/sys/fs/cgroup/memory.stat", "r");
    if (f == nullptr) return -1;
    char line[256];
    long value = -1;
    const std::size_t klen = std::strlen(key);
    while (std::fgets(line, sizeof line, f) != nullptr) {
        // 注意要带上分隔空格，否则 "anon" 会误匹配 "anon_thp"
        if (std::strncmp(line, key, klen) == 0 && line[klen] == ' ') {
            value = std::atol(line + klen) / 1024;  // memory.stat 是字节
            break;
        }
    }
    std::fclose(f);
    return value;
}

/// 优先取 cgroup 的计数；没有 cgroup（比如宿主上直接跑）就退回 /proc/meminfo。
inline long stat_kb(const char* memcg_key, const char* meminfo_key)
{
    const long v = memcg_kb(memcg_key);
    return v >= 0 ? v : meminfo_kb(meminfo_key);
}

/// 每次迭代开头调一次（未开启时只是一次静态 bool 判断，几乎零开销）。
inline void mem_trace(const uint8_t* data, std::size_t size, const char* tag)
{
    if (!mem_trace_enabled()) return;
    static unsigned long long iter = 0;
    static long base_shmem = -1;  // 本轮第一次采样（算"累计增长"的基线）
    static long base_anon = -1;
    static long base_file = -1;
    static const char* dump_dir = std::getenv("LPKG_FUZZ_MEM_DUMP");
    ++iter;

    // **滚动记录"最后见过的输入"**：那个巨额分配是**某一次迭代一口气**吃掉的，
    // 进程会在分配中途被 cgroup 杀掉 —— 按窗口采样永远追不上（`shmem` 每 50 次看都是 0，
    // 可容器却涨到 19 GB）。这里每 10 次把当前输入覆写到 `<dump>/last.bin`，
    // 于是**卡死/崩溃后这个文件里就是触发者本身**（每次都写，不偏）。
    // 代价：约 18 KB/10 次迭代的写盘，可忽略。
    {
        char last_path[512];
        std::snprintf(last_path, sizeof last_path, "%s/last.bin", dump_dir ? dump_dir : "/tmp");
        if (FILE* f = std::fopen(last_path, "wb")) {
            std::fwrite(data, 1, size, f);
            std::fclose(f);
        }
    }

    if (iter % 50 != 0) return;

    // **必须用 cgroup 的计数**：宿主 `meminfo` 的 `Shmem` 与进程 RSS 都看不到这次泄漏
    // （容器 cgroup 记到 18 GB 时，宿主 Shmem 只有 150 MB、进程 anon≈0）。
    const long shmem = stat_kb("shmem", "Shmem:");
    const long anon = stat_kb("anon", "AnonPages:");
    const long file = stat_kb("file", "Cached:");  // 页缓存（cgroup 的 file 含 shmem）
    const long avail = meminfo_kb("MemAvailable:");
    // 用"**自本轮起的累计增长**"而不是"单窗口增量"：fuzzer 往往在**语料初始化阶段**
    // 就吃满上限（13 条 [dbg] 之后就被 cgroup 杀了），单窗口采样根本来不及。累计量只要越过
    // 门槛就落盘，跳变再陡也能抓到。
    if (base_shmem < 0) {
        base_shmem = shmem;
        base_anon = anon;
        base_file = file;
    }
    const long dshmem = shmem - base_shmem;
    const long danon = anon - base_anon;
    const long dfile = file - base_file;
    // 64 MB 累计增量 = 明显不对劲（正常跑这条路径根本不该涨）
    if (dshmem > 65536 || danon > 65536 || dfile > 65536) {
        char path[512];
        std::snprintf(path, sizeof path, "%s/memjump-%llu-%zu.bin", dump_dir ? dump_dir : "/tmp",
                      iter, size);
        if (FILE* out = std::fopen(path, "wb")) {
            std::fwrite(data, 1, size, out);
            std::fclose(out);
        }
        std::fprintf(stderr,
                     "[memtrace] **跳变** %s iter=%llu 输入=%zu 字节 累计Δshmem=%+ldkB "
                     "累计Δanon=%+ldkB 累计Δfile=%+ldkB → 已存 %s\n",
                     tag, iter, size, dshmem, danon, dfile, path);
    }
    if (iter % 2000 == 0) {
        std::fprintf(
            stderr,
            "[memtrace] %s iter=%llu 输入=%zu shmem=%ldkB anon=%ldkB file=%ldkB 可用=%ldkB\n", tag,
            iter, size, shmem, anon, file, avail);
    }
}
