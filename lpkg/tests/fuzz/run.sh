#!/bin/sh
# 在**容器内**跑：构建两个 harness、生成富语料、逐个 fuzz，最后把结局计数打出来。
#
# 由 `make fuzz` 经 `docker exec` 调用 —— 那层只负责 docker-sync、传环境变量、以及把产物
# 拷回宿主。逻辑放这里而不是塞进 Makefile recipe：那几条嵌套引号（make → sh → docker exec
# → sh）极易写错，而这里的错法都是静默的（少跑一个 harness 也"绿"）。
#
# 为什么必须在容器里编：alpine 的 gcc 不带 sanitizer 运行时（只装了头文件），
# 只有容器里的 clang + compiler-rt 能编出 libFuzzer 目标（见 Makefile 的 test-sanitize 说明）。

set -u

BUILD="${FUZZ_BUILD:-build-fuzz}"
FLAGS="${FUZZ_FLAGS:--fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g -O1 -Wno-error}"
TIME_ELF="${FUZZ_TIME_ELF:-60}"
TIME_ARCHIVE="${FUZZ_TIME_ARCHIVE:-60}"
# 文本形态的解析器（WAL 行 / 依赖串 / 索引行 / DB 行 / 求解器差分 / soname 目录）：
# 单轮成本低，但 harness 数量多 —— 给它们一组自己的时限，免得默认 `make fuzz` 被拉长。
TIME_TEXT="${FUZZ_TIME_TEXT:-30}"
ONLY="${FUZZ_ONLY:-}"
SCRATCH=/tmp/lpkg-fuzz-corpus
ARTIFACTS=/app/fuzz-artifacts
CORPUS=tests/fuzz/corpus

# 每次全量重编。为什么值得：`HOST_CXXFLAGS` 变了 make 不感知，复用上一轮的 .o 会让
# sanitizer/插桩**静默失效** —— 那是最坏的一种失败（看起来在 fuzz，其实没插桩）。
rm -rf "$BUILD"
# 暂存语料目录**按 harness 名派生**（与下面的构建目标同一套推导）：加 harness 不必回来改这里。
# （libFuzzer 要求"第一个语料目录"必须存在，否则直接报 `required directory ... does not exist`
# 并退出。）
# **每次跑先清空容器内的产物目录**。理由：`docker cp` 是整目录拷贝，
# 而 `lpkg-builder` 容器是长期存活的 —— 不清的话上一轮的崩溃样本会被留在那儿，
# 于是 ① `fuzz-collect` 的"有崩溃样本"**被旧文件误触发**（看起来本轮又红了，其实没有），
# ② 旧 reproducer 会一直堆着、没人知道它对应哪一轮。产物本来就是**一次性**的
# （"逐个转成 gtest 回归用例后删掉"），所以每次重来一遍是对的。
rm -rf "$ARTIFACTS"
mkdir -p "$ARTIFACTS"
for src in tests/fuzz/*_fuzz.cpp; do
    mkdir -p "$SCRATCH/$(basename "$src" _fuzz.cpp)"
done

echo "fuzz: 容器内构建（clang + libFuzzer/ASan/UBSan，构建树 $BUILD/）..."
# 构建目标**从源文件自动推导**：加一个 harness 只要往这里的 `*_fuzz.cpp` 放一个文件，
# 不必回来改这行（Makefile 里加了名字、这行还写着旧的两个 ⇒ 新 harness 压根
# 没编出来，要到"跑"那一步才报"没有那个文件"）。
targets=""
for src in tests/fuzz/*_fuzz.cpp; do
    targets="$targets $BUILD/$(basename "$src" .cpp)"
done
# shellcheck disable=SC2086  # 故意不加引号：$targets 是"多个目标"的列表
make -j"$(nproc)" BUILD_DIR="$BUILD" CXX=clang++ HOST_CXXFLAGS="$FLAGS" $targets || exit 1

want() { [ -z "$ONLY" ] || [ "$ONLY" = "$1" ]; }

# 注意**不用 `set -e`**：harness 崩了也要继续把另一个跑完、并且退出去让 `make fuzz`
# 收集产物。红度靠末尾的退出码传出去。
rc=0
run_one() {
    name="$1"
    want "$name" || return 0
    short="${name%_fuzz}"  # 语料与暂存目录用短名：tests/fuzz/corpus/elf_strip、…/archive_name
    seconds="$2"
    shift 2
    echo "===== $name（${seconds}s）====="
    # ASAN_OPTIONS=detect_leaks=0：libFuzzer 在 musl 上**启动就漏 56 字节**（8 直接 + 48 间接），
    # 一个**空的** libFuzzer harness 也照样漏（对照实验：`clang++ -fsanitize=fuzzer,address` 编一个
    # 只 `return 0` 的 harness，跑 `-runs=1` 即复现）—— 那是 compiler-rt 的启动分配，不是 lpkg、
    # 也不是 harness。留着它会让每次跑都"报一个崩溃"，把真信号淹掉。lpkg 自身的泄漏另由
    # `make test-sanitize` 覆盖（那条基线是干净的）。
    #
    # libFuzzer 语义：**第一个**语料目录是"新单元写出处"，其余是只读种子源。
    # scratch 放 /tmp：既不污染入库种子，也不受 docker-sync 清空 /app 的影响。
    ASAN_OPTIONS='detect_leaks=0' ./"$BUILD/$name" "$SCRATCH/$short" "$CORPUS/$short" "$@" \
        -artifact_prefix="$ARTIFACTS/" -print_final_stats=1 -max_total_time="$seconds" || rc=1
}

# 逐个跑（`run_one` 自己看 `$ONLY` 决定跑不跑）。`-max_len` 按**输入形态**给：
# 版本串是小文本（1024 足够表达所有形态，也让每次迭代更快），ELF/归档可以很大。
run_one elf_strip_fuzz "$TIME_ELF" -max_len=262144
run_one strip_archive_fuzz "$TIME_ELF" -max_len=262144
run_one vercmp_fuzz "$TIME_ELF" -max_len=1024
run_one elf_soname_fuzz "$TIME_ELF" -max_len=262144
run_one archive_name_fuzz "$TIME_ARCHIVE" -max_len=4096
# 文本形态（`TIME_TEXT`）：输入都是小文本/小脚本，`-max_len` 给足形态即可
run_one wal_line_fuzz "$TIME_TEXT" -max_len=4096
run_one dep_string_fuzz "$TIME_TEXT" -max_len=1024
run_one index_line_fuzz "$TIME_TEXT" -max_len=1024
run_one db_line_fuzz "$TIME_TEXT" -max_len=4096
run_one solver_diff_fuzz "$TIME_TEXT" -max_len=1024
run_one soname_dir_fuzz "$TIME_TEXT" -max_len=4096

echo "fuzz: 跑完（rc=$rc）"
exit $rc
