# Root Makefile for lpkg
CXX ?= g++

# Use pkg-config to resolve dependencies
# Removed libcares from explicit list as libcurl should bring it in if needed
# libgit2 是静态库，其 openssl.c 引用 SSL_set_bio（在 libssl）。静态库依赖的共享库必须排在
# 它**之后**（--as-needed 下前置的 -lssl 无符号引用会被丢弃 → "DSO missing from command line"）。
# 故 openssl 放最后，保证 -lssl -lcrypto 紧跟 -lgit2。
PKGS = libcurl libarchive libelf libgit2 libsolv openssl
# 吞掉 pkg-config 的 stderr：模块缺失时它直接往终端喷 "Package 'x' not found / Perhaps you
# should add …"，而这两行是**递归变量**（只在真正在本机编译/链接时才展开），那份噪音对
# `make docker`（编译发生在容器里，主机 lib 根本用不到）纯属垃圾。缺依赖真正该看的诊断是
# deps-check —— 它带版本窗口、逐模块明确报缺哪一个。
PKG_CFLAGS = $(shell pkg-config --cflags $(PKGS) 2>/dev/null)
LIBS = $(shell pkg-config --libs $(PKGS) 2>/dev/null)
# 动态链接（测试二进制必须动态——全静态链接会改变运行时行为，导致 Tar/pack 等异常）。
# Alpine 默认 --no-copy-dt-needed-entries：.so 的传递依赖（libarchive→libz 等）不自动继承，
# 用 --copy-dt-needed-entries 恢复（链接器从共享库的 DT_NEEDED 复制传递依赖）。
TEST_LIBS = $(LIBS) -lgtest -lgtest_main -lpthread

# Override LIBS for static build
ifeq ($(findstring static,$(MAKECMDGOALS)),static)
    # Using --start-group/--end-group to handle complex static dependency chains
    # We use pkg-config --static to get all transitive dependencies
    LIBS = -Wl,--start-group $(shell pkg-config --libs --static $(PKGS)) -Wl,--end-group -static
endif

PKGS_LIST = $(PKGS)

# ---- 依赖库探测（构建必要条件）----
# 每个 pkg-config 模块必须 >= min；可选第三段 cap = "下一个大版本边界"（排除性上界，
# 同大版本线内的 patch/minor 放行，如 libcurl 8.x 里 8.21.0 通过；>= cap 才拦）。
# 冒到 cap 及以后 = 大版本可能 API 破坏、未经确认 → 构建**立即**报错，而不是
# "pkg-config 探测通过了，却在编译/链接期才因 API 破坏/缺静态库炸"。
# cap 写 nolimit = 该库从不 breaking、无上限（如 libelf）。跑全量测试确认 API 兼容后，
# 把对应 cap 抬到下一边界（min 只在需要新特性时抬）。
# static 构建额外校验：各模块**自身** Libs（--libs-only-l，即 lpkg 真正链接的，
# 不含传递私有依赖如 -lz）都能在其 libdir 找到 .a。
LPKG_DEPS = libcurl:8.14.1:9 libarchive:3.8.3:4 libelf:0.190:nolimit \
            libgit2:1.7.2:2 libsolv:0.7.39:0.8 openssl:3.1.8:4

.PHONY: deps-check
deps-check:
	@# 版本窗口 +（static 时）自身库 .a，单循环按模块探测；任一失败 → 立即终止。
	@command -v pkg-config >/dev/null 2>&1 || { echo "deps-check: error: 未找到 pkg-config（无法探测依赖库）"; exit 1; }; \
	dirs=""; for spec in $(LPKG_DEPS); do mod="$${spec%%:*}"; dirs="$$dirs $$(pkg-config --variable=libdir "$$mod" 2>/dev/null)"; done; \
	need_static=0; echo "$(MAKECMDGOALS)" | grep -qw static && need_static=1; \
	for spec in $(LPKG_DEPS); do \
	  mod="$${spec%%:*}"; rest="$${spec#*:}"; min="$${rest%%:*}"; cap="$${rest#*:}"; \
	  [ "$$cap" = nolimit ] && cap=""; \
	  if pkg-config --atleast-version="$$min" "$$mod" >/dev/null 2>&1; then \
	    if [ -z "$$cap" ] || ! pkg-config --atleast-version="$$cap" "$$mod" >/dev/null 2>&1; then \
	      if [ -n "$$cap" ]; then \
	        echo "  dep  $$mod $$(pkg-config --modversion $$mod)  [$$min, $$cap)"; \
	      else \
	        echo "  dep  $$mod $$(pkg-config --modversion $$mod)  >= $$min (nolimit)"; \
	      fi; \
	      if [ "$$need_static" = 1 ]; then \
	        for l in $$(pkg-config --libs-only-l "$$mod" 2>/dev/null); do \
	          name="$${l#-l}"; [ -z "$$name" ] && continue; \
	          found=0; for d in $$dirs; do [ -n "$$d" ] && [ -f "$$d/lib$$name.a" ] && { found=1; break; }; done; \
	          if [ "$$found" != 1 ]; then echo "deps-check: error: 静态库 lib$$name.a 缺失（$$mod 自身链接需要，静态构建必需）"; exit 1; fi; \
	        done; \
	      fi; \
	    else \
	      echo "deps-check: error: $$mod $$(pkg-config --modversion $$mod) 已达下一个大版本边界 $$cap（API 是否破坏未确认；跑全量测试确认后把 LPKG_DEPS 的 cap 抬到下一边界）"; \
	      exit 1; \
	    fi; \
	  elif pkg-config --exists "$$mod" >/dev/null 2>&1; then \
	    echo "deps-check: error: $$mod $$(pkg-config --modversion $$mod) 过旧，需 >= $$min（可能缺 API）"; \
	    exit 1; \
	  else \
	    echo "deps-check: error: 缺依赖库 pkg-config 模块 $$mod（需 >= $$min）"; \
	    exit 1; \
	  fi; \
	done
	@echo "dependency checks passed"

TARGET = lpkg
TEST_TARGET = run_tests
VERSION ?= 8.0.0

BUILD_DIR = build
MAIN_DIR = main
APP_SRC_DIR = $(MAIN_DIR)/src
TESTS_DIR = tests

# --- Coverage Flags ---
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage

# --- Install Directories ---
PREFIX ?= /usr
SYSCONFDIR ?= /etc
LOCALSTATEDIR ?= /var

INSTALL_BIN_DIR = $(DESTDIR)$(PREFIX)/bin
INSTALL_CONF_DIR = $(DESTDIR)$(SYSCONFDIR)/lpkg
INSTALL_SHARE_DIR = $(DESTDIR)$(PREFIX)/share/lpkg
INSTALL_L10N_DIR = $(INSTALL_SHARE_DIR)/l10n
INSTALL_DOCS_DIR = $(INSTALL_SHARE_DIR)/docs
INSTALL_LOCK_DIR = $(DESTDIR)$(LOCALSTATEDIR)/lpkg

# Common flags shared between production and test builds
COMMON_CXXFLAGS = -std=c++20 -fPIC -Wall -Wextra -I$(MAIN_DIR)/third_party/include -I$(APP_SRC_DIR) \
	-I$(APP_SRC_DIR)/base -I$(APP_SRC_DIR)/vercmp -I$(APP_SRC_DIR)/crypto \
	-I$(APP_SRC_DIR)/config -I$(APP_SRC_DIR)/i18n -I$(APP_SRC_DIR)/db \
	-I$(APP_SRC_DIR)/repo -I$(APP_SRC_DIR)/trigger \
	-I$(APP_SRC_DIR)/pkg -I$(APP_SRC_DIR)/archive -I$(APP_SRC_DIR)/build \
	-I$(APP_SRC_DIR)/elf -I$(APP_SRC_DIR)/scan \
	-I$(APP_SRC_DIR)/ui \
           $(PKG_CFLAGS) \
           -DLPKG_CONF_DIR=\"$(INSTALL_CONF_DIR)\" \
           -DLPKG_L10N_DIR=\"$(INSTALL_L10N_DIR)\" \
           -DLPKG_DOCS_DIR=\"$(INSTALL_DOCS_DIR)\" \
           -DLPKG_LOCK_DIR=\"$(INSTALL_LOCK_DIR)\" \
           -DLPKG_VERSION=\"$(VERSION)\" \
           -Werror
# 转义引号吃掉，宏展开成 /etc/lpkg 被当成除法（实测炸过构建）；用户显式覆盖的项目同名
# flag（如 -O3 覆盖 -O2）不在项目列表里 → 保留，仍然后者胜。

# --- 主机环境传入的 flags（"系统 flags"）---
# 必须在下面与项目 flags 合成**之前**捕获：这是要注入容器的那一份原始值
# （合成后的 CXXFLAGS 含 COMMON_CXXFLAGS，注入会重复）。
HOST_CFLAGS := $(CFLAGS)
HOST_CXXFLAGS := $(CXXFLAGS)
HOST_LDFLAGS := $(LDFLAGS)
HOST_TEST_CXXFLAGS := $(TEST_CXXFLAGS)

# 系统 flags 里的 `-Wp,-D_FORTIFY_SOURCE=3` 会和编译器**内建**的那份打架：`-Wp,` 把宏直接塞给
# cpp，driver 的 specs 守卫（`%{!D_FORTIFY_SOURCE:…}`）看不见它，于是 gcc 在 -O2 下照样按自己
# 的默认再定义一次 `_FORTIFY_SOURCE`（`<built-in>`，Alpine 13.2 是 2）→ cpp 报
# `"_FORTIFY_SOURCE" redefined`，被本项目的 -Werror 升成**致命错误**（实测整包编译全灭，
# 而该诊断没有 -W 选项名：-Wno-error=builtin-macro-redefined / -Wno-builtin-macro-redefined
# 都关不掉）。干净解法是先 `-U` 掉内建那份，让调用方显式给的值生效（fortify 等级以系统
# flags 为准）。**只在其确实带了这个宏时才 -U** —— 否则会平白关掉编译器默认的 fortify。
SYSTEM_FORTIFY_UNDEF = $(if $(findstring _FORTIFY_SOURCE,$(HOST_CFLAGS) $(HOST_CXXFLAGS)),-U_FORTIFY_SOURCE)

# 链接期行为 flag 必须在**库之前**出现（ld 的 --copy-dt-needed-entries 决定其后 DSO 的
# DT_NEEDED 是否被继承；放到 -larchive 之后 = 不生效，Alpine 上直接报
# "libarchive.so: undefined reference to deflateInit_ / DSO missing from command line"）。
# 放进 LDFLAGS：语义正确（它就是链接选项）且位置天然在 $^ 与 $(LIBS) 之前。
LDFLAGS := $(HOST_LDFLAGS) -Wl,--copy-dt-needed-entries

# 系统 flags 追加在**后面**（gcc 后者胜）；-U 必须排在它们**前面**才压得住内建定义。
# 这里必须是 `=`（递归展开），**不能**用 `:=`：COMMON_CXXFLAGS 里含 $(shell pkg-config …)，
# 立即展开会让**每一次** make（包括主机上根本不编译的 `make docker`）都去主机跑 pkg-config
# —— 主机没装 libsolv 就满屏 "Package 'libsolv' not found"，而那份输出主机上根本用不到
# （真正编译发生在容器里，容器内 pkg-config 齐全）。同理引用 HOST_CXXFLAGS 而不是
# $(CXXFLAGS)：递归变量引用自身会死循环。
CXXFLAGS = $(COMMON_CXXFLAGS) -O2 $(SYSTEM_FORTIFY_UNDEF) $(HOST_CXXFLAGS)
# 测试编译同样吃主机 CXXFLAGS（"不忽略系统 flags"对测试构建一样成立）
TEST_CXXFLAGS = $(COMMON_CXXFLAGS) -g $(SYSTEM_FORTIFY_UNDEF) $(HOST_TEST_CXXFLAGS) $(HOST_CXXFLAGS)

MAIN_SRCS = $(wildcard $(APP_SRC_DIR)/*.cpp) \
	$(wildcard $(APP_SRC_DIR)/base/*.cpp) $(wildcard $(APP_SRC_DIR)/vercmp/*.cpp) \
	$(wildcard $(APP_SRC_DIR)/crypto/*.cpp) $(wildcard $(APP_SRC_DIR)/config/*.cpp) \
	$(wildcard $(APP_SRC_DIR)/i18n/*.cpp) $(wildcard $(APP_SRC_DIR)/db/*.cpp) \
	$(wildcard $(APP_SRC_DIR)/repo/*.cpp) $(wildcard $(APP_SRC_DIR)/trigger/*.cpp) \
	$(wildcard $(APP_SRC_DIR)/pkg/*.cpp) $(wildcard $(APP_SRC_DIR)/archive/*.cpp) \
	$(wildcard $(APP_SRC_DIR)/build/*.cpp) $(wildcard $(APP_SRC_DIR)/elf/*.cpp) \
	$(wildcard $(APP_SRC_DIR)/scan/*.cpp) $(wildcard $(APP_SRC_DIR)/ui/*.cpp)
TEST_SRCS = $(TESTS_DIR)/main.cpp $(wildcard $(TESTS_DIR)/unit/*.cpp) $(wildcard $(TESTS_DIR)/integration/*.cpp)
MAIN_OBJS = $(patsubst $(APP_SRC_DIR)/%.cpp,$(BUILD_DIR)/main/%.o,$(MAIN_SRCS))
TEST_OBJS = $(patsubst $(TESTS_DIR)/%.cpp,$(BUILD_DIR)/tests/%.o,$(TEST_SRCS))
LPKG_OBJS = $(filter-out $(BUILD_DIR)/main/main.o, $(MAIN_OBJS))
DEPS = $(MAIN_OBJS:.o=.d) $(TEST_OBJS:.o=.d)

# Handle coverage mode flags internally
ifeq ($(COVERAGE_MODE),1)
    CXXFLAGS += $(COVERAGE_FLAGS)
    TEST_CXXFLAGS += $(COVERAGE_FLAGS)
    LIBS += -lgcov
    TEST_LIBS += -lgcov
endif

.PHONY: all clean install uninstall static test build_test_internal docker docker-env docker-sync deps-check

all: deps-check $(BUILD_DIR)/$(TARGET)

test: docker-env
	@$(MAKE) docker-sync
	@# l10n 兜底副本。init_localization 的查找顺序是 <exe>/../l10n → <exe>/../main/l10n →
	@# <exe>/../src/l10n → 安装路径，容器里测试命中 /app/main/l10n（同步进去的源码树），
	@# 这一份是"源码树布局不可用"时的后备。必须拷进 l10n/ 目录**本身**：
	@# `cp -r /app/main/l10n /usr/share/lpkg/` 在目标已存在时会嵌套成 l10n/l10n（踩过）。
	@docker exec $(DOCKER_ENV_CONTAINER) sh -c "mkdir -p /usr/share/lpkg/l10n && rm -rf /usr/share/lpkg/l10n/* && cp -r /app/main/l10n/. /usr/share/lpkg/l10n/"
	@docker exec $(DOCKER_ENV_FLAGS) -w /app $(DOCKER_ENV_CONTAINER) sh -c "make clean && make -j$$(nproc) build/run_tests && ./build/run_tests"
	@echo "------------------------------------------"
	@echo "Tests completed inside Docker container."

coverage:
	@echo "Starting comprehensive test suite with coverage..."
	@rm -rf build_test coverage_report
	@mkdir -p build_test
	@$(MAKE) BUILD_DIR=build_test COVERAGE_MODE=1 build_test_internal
	@mkdir -p coverage_report
	@lcov --rc geninfo_unexecuted_blocks=1 --capture --directory build_test --output-file build_test/coverage.info --ignore-errors mismatch,inconsistent,unused || true
	@lcov --ignore-errors mismatch,inconsistent,unused --remove build_test/coverage.info '/usr/*' '*/third_party/*' '*/tests/*' --output-file build_test/coverage_filtered.info || true
	@genhtml --ignore-errors mismatch,inconsistent --output-directory coverage_report build_test/coverage_filtered.info || true
	@rm -rf build_test
	@echo "------------------------------------------------------"
	@echo "Coverage report is available in: coverage_report/index.html"

# ---- 格式检查 / 就地格式化（2026-09-26 新增）----------------------------------
# clang-format 在**主机**跑、不进容器：它只读文本，不解析头文件、不链接依赖库，所以没有任何
# 理由为它把 llvm 拉进 lpkg-builder（容器是 alpine，clang-format 在 clang-extra 里，会带整条
# llvm 依赖）。全仓当前 **0 残留 / 197 个文件**（clang-format 23.1.2 实测），所以这条是
# **可执行的不变量**，而不是"大概干净"。换个大版本可能因换行决策不同而误报 →
# 可用 `make format-check CLANG_FORMAT=clang-format-18` 指定。
# 覆盖范围 = lpkg 自己拥有的源码树（main/src + tests）；**不含** main/third_party（别人的
# 代码，改了下次同步就丢）。
FMT_DIRS = $(APP_SRC_DIR) $(TESTS_DIR)
FMT_FIND = find $(FMT_DIRS) -type f \( -name '*.cpp' -o -name '*.hpp' \)
CLANG_FORMAT ?= clang-format

.PHONY: format-check format
format-check:
	@command -v $(CLANG_FORMAT) >/dev/null 2>&1 || \
	  { echo "format-check: error: 未找到 $(CLANG_FORMAT)（装 clang，或指定 CLANG_FORMAT=/path/to/clang-format）"; exit 1; }
	@# 一次调用喂**全部**文件：clang-format 逐条打印违规位置（文件:行:列 + 原行 + 插入符），
	@# 任一文件不合规即退出码非 0 —— "红"是那个退出码说的，不是我们 grep 输出凑出来的。
	@n=$$($(FMT_FIND) | wc -l); \
	 if $(CLANG_FORMAT) --dry-run --Werror $$($(FMT_FIND)); then \
	   echo "format-check: $$n 个文件全部符合 .clang-format"; \
	 else \
	   echo ""; \
	   echo "format-check: 上面这些位置不符合 .clang-format —— 跑 make format 就地修"; \
	   exit 1; \
	 fi

# -i 就地改写。这是唯一会改文件内容的形态目标（不编译、不碰容器、不依赖 deps-check）。
format:
	@$(FMT_FIND) -print0 | xargs -0 $(CLANG_FORMAT) -i
	@echo "format: 已按 .clang-format 就地格式化 ($(FMT_DIRS))"

# ---- 索引格式契约（C++ ↔ Python）-----------------------------------------------
# 索引格式有两份读者：C++ 的 `parse_repo_index_line()`（base/utils.cpp）与
# `main/scripts/lrepo-mgr.py` 的 `parse_aggregated_index()` —— 后者的结果会被 `push`
# 用来**读-改-写整份索引**、被 `cleanup` 用来决定删哪些包文件。两侧必须同语义。
#
# 契约的**全部**机制是「同一份 fixture + 同一份期望」，两侧各自断言、谁也不调谁：
#   · C++ 侧   ：tests/unit/test_repo_index_conformance.cpp（随 `make test` 跑）
#   · Python 侧：本目标（**在主机跑**，用主机的 python3 —— 不为它给 alpine 容器装 24 MiB
#                的 python3，那是一处当前不可达的潜在分叉不值的机器成本）
# **两处的 fixture 与期望逐字相同，改一处必须同时改另一处**（两边注释里都写着）。
.PHONY: check-index-format
check-index-format:
	@command -v python3 >/dev/null 2>&1 || \
	  { echo "check-index-format: error: 未找到 python3"; exit 1; }
	@python3 $(MAIN_DIR)/scripts/check_index_conformance.py

# ---- clang-tidy 的编译数据库（`make tidy` 的前置）-------------------------------
# 本仓库是**纯 Make**（不是 CMake），所以 `compile_commands.json` 得自己生成。上
# `bear` / `compiledb` 是没必要的额外工具：Makefile 本来就知道每一条编译命令
# （就是下面那两条模式规则，用的还是同一组 `$(CXXFLAGS)` / `$(TEST_CXXFLAGS)`）。
#
# **不依赖 `deps-check`**：生成阶段不编译，只要求 `pkg-config --cflags` 能解析 ——
# 所以本目标跟 `tidy` 一样跑在容器里（依赖装齐的那一侧）。
# 落在 `build/` 下：那个目录本来就不入版本库（本目录也没有 .gitignore，
# 别为了它去根目录拉一个新文件）。
#
# flags 里的 `-DLPKG_CONF_DIR=\"/etc/lpkg\"` 带引号，JSON 里必须转义 —— 见下面的 sed。
.PHONY: compile-commands
compile-commands:
	@mkdir -p $(BUILD_DIR)
	@printf '[\n' > $(BUILD_DIR)/compile_commands.json
	@first=1; \
	for src in $(MAIN_SRCS) $(TEST_SRCS); do \
	  case "$$src" in $(TESTS_DIR)/*) flags='$(TEST_CXXFLAGS)' ;; *) flags='$(CXXFLAGS)' ;; esac; \
	  cmd="$(CXX) $$flags -MMD -MP -c $$src"; \
	  esc=$$(printf '%s' "$$cmd" | sed 's/\\/\\\\/g; s/"/\\"/g'); \
	  if [ $$first -eq 0 ]; then printf ',\n'; fi; first=0; \
	  printf '  {"directory": "%s", "command": "%s", "file": "%s"}' '$(CURDIR)' "$$esc" "$$src"; \
	done >> $(BUILD_DIR)/compile_commands.json
	@printf '\n]\n' >> $(BUILD_DIR)/compile_commands.json
	@echo "compile-commands: 已写出 $(BUILD_DIR)/compile_commands.json（$$(grep -c file $(BUILD_DIR)/compile_commands.json) 条）"

# ---- clang-tidy 静态检查（**只报告，不拦**）-----------------------------------
# 跑在**容器**里（本项目的验证环境就是它，`docker-env` 把依赖装齐）。
# 配置见 `.clang-tidy`。
#
# **本目标刻意不看退出码**（下面的 `|| true`）：现在的用途是"把实际告警摊开给人看"，
# 挑出高价值项、逐条改完之后再把它们升级成拦截。**别把它接到 CI 上** ——
# 一个红度未知的闸门只会挡住所有提交。
# 基线注意：tidy 用的是容器里的 clang-tidy 17，与 `format-check` 的 clang-format 23.1.2
# **不是同一个版本**（两者跑在不同的地方：format 在能跑的那一侧跑，tidy 在容器里）。
.PHONY: tidy
tidy:
	@mkdir -p $(BUILD_DIR)
	@# `docker-env` 的输出（`apk` 的 fetch 行、"OK: N MiB in M packages"）对使用者没用，
	@# 但**失败时必须看得见** —— 所以落日志、只在失败时回放。
	@$(MAKE) docker-env > $(BUILD_DIR)/docker-env.log 2>&1 || \
	  { echo "tidy: docker-env 失败，日志如下："; cat $(BUILD_DIR)/docker-env.log; exit 1; }
	@# **必须 sync**（2026-10-03 加）：clang-tidy 在容器里跑，分析的是**容器那份源码** ——
	@# 不 sync 就会拿上一次 sync 的旧源码出报告（实测：改完再跑，仍报旧行号/旧告警，看着像
	@# "没修掉"，实际是根本没看到新代码）。这与 `test` 目标同一条纪律。
	@$(MAKE) docker-sync >/dev/null
	@# **必须并行**，不能逐个 `clang-tidy` 串行跑：143 个 TU，每个都要把
	@# `nlohmann/json.hpp`（920 KB）之类的三方头解析一遍 —— 这是 clang-tidy 的**固有成本，
	@# 配置绕不开**（tidy 必须建完整 AST 才能分析；`HeaderFilterRegex` 只管**报不报**，
	@# 管不了**解不解析**）。串行实测 4 分半都跑不完；下面按核数并行。
	@#
	@# 用 `xargs -P` 而**不是** `run-clang-tidy`：后者是个 **Python 脚本**，容器里没有 python3
	@#（实测 `env: can't execute 'python3'` —— 当初换成它时**整个目标一行都没跑**，
	@# 却因为日志文件在、报出"0 告警"，看着像通过）。`xargs` 是 busybox 自带的，没有这个坑。
	@echo "tidy: 并行静态检查（跑在容器里）..."
	@start=$$(date +%s); \
	docker exec -w /app $(DOCKER_ENV_CONTAINER) sh -c \
	  'make -s compile-commands >/dev/null && \
	   printf "%s\n" $(MAIN_SRCS) $(TEST_SRCS) | \
	     xargs -P $$(nproc) -I{} clang-tidy -p $(BUILD_DIR) \
	       --extra-arg=-Wno-missing-field-initializers {}' \
	  > $(BUILD_DIR)/tidy.log 2>&1 || true; \
	echo "tidy: 耗时 $$(( $$(date +%s) - start ))s，结果在 $(BUILD_DIR)/tidy.log"
	@echo -n "tidy: 告警总数 = "
	@grep -c 'warning:' $(BUILD_DIR)/tidy.log || echo 0
	@echo "tidy: 按 check 分类 ——"
	@sed -n 's/.*\[\([a-z0-9.,-]*\)\]$$/\1/p' $(BUILD_DIR)/tidy.log | sort | uniq -c | sort -rn | head -20 || true

# ---- ASan + UBSan 下的测试 -----------------------------------------------------
# **独立的一棵构建树**（`build-san/`），不污染正常的 `build/`。
#
# 为什么用 **clang** 而不是容器自带的 gcc：**alpine 的 gcc 不带 sanitizer 运行时**
# （实测 `g++ -fsanitize=address` 报 `cannot find -lasan` —— 它只装了头文件
# `plugin/include/asan.h`）。`clang` + `compiler-rt` 由 `docker-env` 装进容器。
#
# ⚠️ **本目标现在的用途是"跑一遍看掉出来什么"，不是闸门**：一个 1000+ 用例、大量依赖
# root 与文件系统副作用的套件在 sanitizer 下**大概率会暴露一批既有问题**（含夹具自身的
# 泄漏）。先把它们**分类**（真缺陷 / 夹具问题 / 第三方库噪音），分完之后才谈接不接 CI ——
# 在那之前接上去，等于用一个红度未知的闸门挡住所有提交。
#
# 不加 `-fno-sanitize-recover`：先要**列全**，不想第一个 UBSan 发现问题就中止。
#
# `-Wno-error`：**clang 的告警集与 gcc 不同，本项目目前只在 gcc 下过了 `-Werror`**。
# 实测 clang 17 会额外报（都是 gcc 不报的）：11 处 `-Wmissing-field-initializers`
# （`git_clone_options{}` 这类 libgit2 结构体的花括号初始化）、2 处
# `-Wunused-lambda-capture`、1 处 `-Wunused-const-variable`。
# **让项目在 clang 下也过 `-Werror` 是一件独立的事（跨编译器可移植性）**，不该混进
# "跑一遍 sanitizer"这件事里 —— 所以这里先放宽，先拿数据。清完之后再把 `-Wno-error` 去掉。
SANITIZE_FLAGS ?= -fsanitize=address,undefined -fno-omit-frame-pointer -g -Wno-error
SAN_BUILD_DIR = build-san

.PHONY: test-sanitize
test-sanitize: docker-env
	@echo "test-sanitize: 用 clang + ASan/UBSan 重建测试二进制并跑（容器内，独立构建树 $(SAN_BUILD_DIR)/）..."
	@$(MAKE) docker-sync >/dev/null
	@# flags 走 **`HOST_CXXFLAGS`**（不是 `HOST_TEST_CXXFLAGS`）：后者只进 `TEST_CXXFLAGS`，
	@# 而主源码用的是 `CXXFLAGS` —— 实测把 `-Wno-error` 放错地方时，测试 TU 放行了、
	@# `builder_executor.cpp` 照样被 `-Werror` 卡住（clang 独有的 missing-field-initializers）。
	@# `HOST_CXXFLAGS` 同时进 `CXXFLAGS` 与 `TEST_CXXFLAGS`，链接行也吃 `TEST_CXXFLAGS`，
	@# 所以 sanitizer 运行时能链上。
	@docker exec -w /app $(DOCKER_ENV_CONTAINER) sh -c \
	  'make -j$$(nproc) BUILD_DIR=$(SAN_BUILD_DIR) CXX=clang++ \
	        HOST_CXXFLAGS="$(SANITIZE_FLAGS)" \
	        $(SAN_BUILD_DIR)/$(TEST_TARGET) && ./$(SAN_BUILD_DIR)/$(TEST_TARGET)'

# ---- libFuzzer 模糊测试（手动跑：`make fuzz`；**不接 CI、不做闸门**）--------------------
# 与 test-sanitize 同构：容器内、clang、独立构建树、flags 走 HOST_CXXFLAGS。三处不同：
#   1) `-fsanitize=` 组合里多一个 `fuzzer`（clang driver 自动链 libclang_rt.fuzzer-x86_64.a，
#      **它提供 main**）⇒ 绝不能链 `TEST_LIBS`（:19 含 `-lgtest_main`，会撞 main）；
#   2) 链接只吃 `$(LIBS)`；
#   3) 语料与产物路径要考虑 `docker-sync` 会清空 `/app`（见 `tests/fuzz/run.sh`）。
# 为什么不接 CI：fuzz 的产出依赖时间与种子，天生不确定，与"全量绿才算过"冲突 ——
# 同 test-sanitize 注释里那条"别用红度未知的闸门挡住所有提交"。
#
# **harness 放 `tests/fuzz/` 而不是顶层新目录**：那里既有的 `$(BUILD_DIR)/tests/%.o`
# 规则（见下方模式规则）能直接编它们，又被 format-check 的 `FMT_FIND`（:213，递归 find
# main/src tests）自动覆盖 —— FMT_DIRS 一行都不用改。而 `TEST_SRCS`（:162）只收
# `tests/main.cpp` + `unit/` + `integration/`，所以它们**不会**被编进 run_tests。
FUZZ_BUILD_DIR = build-fuzz
FUZZ_DIR = $(TESTS_DIR)/fuzz
FUZZ_ARTIFACTS_DIR = fuzz-artifacts
FUZZ_FLAGS ?= -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g -O1 -Wno-error
# 默认限时，保证 `make fuzz` 会结束；要长跑：`make fuzz FUZZ_TIME_ELF=600`
FUZZ_TIME_ELF ?= 60
FUZZ_TIME_ARCHIVE ?= 60
# 文本形态的解析器（WAL 行 / 依赖串 / 索引行 / DB 行 / 求解器差分 / soname 目录）：
# 单轮成本低但 harness 数量多，给它们各自的时限，免得默认 `make fuzz` 被拉长。
FUZZ_TIME_TEXT ?= 30
FUZZ_ONLY ?=
# ⚠️ 这里**没有**"harness 清单"这种变量，加 harness 也**不需要**改 Makefile：
# 构建目标由 `$(wildcard $(FUZZ_DIR)/*.cpp)`（见下面的 FUZZ_OBJS）与 run.sh 里
# 从 `tests/fuzz/*_fuzz.cpp` 推导的那行共同决定。曾经这里有个 `FUZZ_NAMES = ...`
# 常量，**全文件零引用**、而且只列了当时那 5 个 —— 名字看起来像"清单"，加 harness 的人
# 会以为必须改它（2026-10-04 删，与 `constants::TAB` 同类的死代码）。

# ⚠️ harness 的 `.o` **必须**也纳入头依赖追踪。它们走的是通用的
# `$(BUILD_DIR)/tests/%.o` 规则，而 `TEST_SRCS`（:162）只收 `tests/main.cpp` + `unit/` +
# `integration/`，**不含 `tests/fuzz/`** ⇒ 它们的 `.d` 不在 `DEPS` 里 ⇒ **改了
# `tests/fuzz/*.hpp` 不会触发重编**。实测踩过：改了追踪逻辑跑出来的还是旧二进制，
# 白折腾一轮（症状是"日志行数对不上、采样一次都没打"）。
FUZZ_OBJS = $(patsubst $(FUZZ_DIR)/%.cpp,$(BUILD_DIR)/tests/fuzz/%.o,$(wildcard $(FUZZ_DIR)/*.cpp))
DEPS += $(FUZZ_OBJS:.o=.d)

# $^ = harness.o + $(LPKG_OBJS)（:165 已剔掉 build/main/main.o ⇒ 不会有第二个 main）
$(BUILD_DIR)/%_fuzz: $(BUILD_DIR)/tests/fuzz/%_fuzz.o $(LPKG_OBJS) | deps-check
	@mkdir -p $(@D)
	$(CXX) $(TEST_CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

.PHONY: fuzz fuzz-collect

fuzz: docker-env
	@echo "fuzz: clang + libFuzzer/ASan/UBSan（容器内，构建树 $(FUZZ_BUILD_DIR)/）..."
	@$(MAKE) docker-sync >/dev/null
	@# 环境变量用 docker exec -e 传，不在命令行里拼嵌套引号（make → sh → docker exec → sh
	@# 四层引号，错法全是静默的：少跑一个 harness 看起来也是绿的）。
	@# `rc=` 捕获 + 无条件跑 fuzz-collect：harness 崩了也**必须**先把样本取回来，
	@# 否则下一轮 docker-sync 的 `rm -rf /app/*` 会把它清掉。
	@mkdir -p $(FUZZ_BUILD_DIR)
	@docker exec -w /app \
	   -e FUZZ_BUILD='$(FUZZ_BUILD_DIR)' \
	   -e FUZZ_FLAGS='$(FUZZ_FLAGS)' \
	   -e FUZZ_TIME_ELF='$(FUZZ_TIME_ELF)' \
	   -e FUZZ_TIME_ARCHIVE='$(FUZZ_TIME_ARCHIVE)' \
	   -e FUZZ_TIME_TEXT='$(FUZZ_TIME_TEXT)' \
	   -e FUZZ_ONLY='$(FUZZ_ONLY)' \
	   $(DOCKER_ENV_CONTAINER) sh $(FUZZ_DIR)/run.sh; rc=$$?; \
	 $(MAKE) fuzz-collect; exit $$rc

fuzz-collect:
	@# **先清空宿主的产物目录**（2026-10-04 加）：`docker cp` 是整目录覆盖式拷贝，不清的话
	@# 上一轮留在宿主的旧样本会混在本轮里 —— 下面那句"有崩溃样本"就会**被旧文件误报**，
	@# 而且没人分得清哪个 reproducer 属于哪一轮。容器侧另有一份清理（见 tests/fuzz/run.sh）。
	@rm -rf $(FUZZ_ARTIFACTS_DIR)
	@mkdir -p $(FUZZ_ARTIFACTS_DIR)
	@docker cp $(DOCKER_ENV_CONTAINER):/app/$(FUZZ_ARTIFACTS_DIR)/. $(FUZZ_ARTIFACTS_DIR)/ 2>/dev/null || true
	@if ls $(FUZZ_ARTIFACTS_DIR)/* >/dev/null 2>&1; then \
	   echo "fuzz: 有崩溃样本 —— $(FUZZ_ARTIFACTS_DIR)/（逐个转成 gtest 回归用例后删掉）"; \
	   ls -l $(FUZZ_ARTIFACTS_DIR)/; \
	 else \
	   echo "fuzz: 本轮无崩溃样本"; \
	 fi

build_test_internal: $(BUILD_DIR)/$(TEST_TARGET)
	@FILTER="$(GTEST_FILTER)"; if [ -n "$$FILTER" ]; then ./$(BUILD_DIR)/$(TEST_TARGET) --gtest_filter="$$FILTER"; else ./$(BUILD_DIR)/$(TEST_TARGET); fi

# 注意：deps-check 是 phony 前置，必须用 order-only（`|`）——普通前置会进 $^，
# 链接 recipe 的 $^ 会把字面量 "deps-check" 当输入文件塞给 ld。
$(BUILD_DIR)/$(TARGET): $(MAIN_OBJS) | deps-check
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD_DIR)/$(TEST_TARGET): $(TEST_OBJS) $(LPKG_OBJS) | deps-check
	@mkdir -p $(@D)
	$(CXX) $(TEST_CXXFLAGS) $(LDFLAGS) -o $@ $^ $(TEST_LIBS)

static: deps-check $(BUILD_DIR)/$(TARGET)-static

# $(LDFLAGS) 放在 -static **之前**：主机 LDFLAGS 若带 -pie 之类的链接模式 flag，
# 也必须让 -static（本目标的语义）最后胜出。
$(BUILD_DIR)/$(TARGET)-static: $(MAIN_OBJS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -static -o $@ $^ $(LIBS)

$(BUILD_DIR)/main/%.o: $(APP_SRC_DIR)/%.cpp | deps-check
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/tests/%.o: $(TESTS_DIR)/%.cpp | deps-check
	@mkdir -p $(@D)
	$(CXX) $(TEST_CXXFLAGS) -MMD -MP -c -o $@ $<

-include $(DEPS)

clean:
	rm -rf $(BUILD_DIR) build_test coverage_report $(FUZZ_BUILD_DIR)
	@# 注意**不删** $(FUZZ_ARTIFACTS_DIR)：崩溃样本是**证据**不是产物（转成 gtest 回归用例
	@# 之后手工删），跟 §8.3 那条"验证手段不入库、但也不能随手清掉"同一个道理。

define do_install
	install -Dm755 $(1) $(INSTALL_BIN_DIR)/$(TARGET)
	install -Dm644 $(MAIN_DIR)/conf/mirror.conf $(INSTALL_CONF_DIR)/mirror.conf
	install -Dm644 $(MAIN_DIR)/conf/triggers.conf $(INSTALL_CONF_DIR)/triggers.conf
	install -Dm644 $(MAIN_DIR)/conf/build.conf $(INSTALL_CONF_DIR)/build.conf
	install -d -m755 $(INSTALL_L10N_DIR)
	install -m644 $(MAIN_DIR)/l10n/*.txt $(INSTALL_L10N_DIR)/
	install -d -m755 $(INSTALL_LOCK_DIR)
	install -d -m755 $(INSTALL_DOCS_DIR)
endef

install: all
	$(call do_install,$(BUILD_DIR)/$(TARGET))

static-install: static
	$(call do_install,$(BUILD_DIR)/$(TARGET)-static)

docker-install: $(BUILD_DIR)/$(TARGET)-docker
	$(call do_install,$<)

uninstall:
	rm -f $(INSTALL_BIN_DIR)/$(TARGET)
	rm -rf $(INSTALL_SHARE_DIR)
	rm -rf $(INSTALL_CONF_DIR)
	rm -rf $(INSTALL_LOCK_DIR)

DOCKER_ENV_CONTAINER = lpkg-builder

# 把主机 flags 注入容器：docker exec **不继承主机环境**（只有 -e 明确指定的变量才进容器），
# 于是默认的 `make docker` 路径此前把主机的 CFLAGS/CXXFLAGS/LDFLAGS 全丢了 —— 容器里 `make`
# 看到的这三个变量是空的，只剩 Makefile 自带的 COMMON_CXXFLAGS/-O2 生效（实测主机
# `CXXFLAGS=-march=x86-64-v3` 根本到不了容器内的编译行）。
# 用上面捕获的 HOST_*（与项目 flags 合成**之前**的原始值）逐个 -e 注入；容器内
# `CXXFLAGS := $(COMMON_CXXFLAGS) -O2 $(CXXFLAGS)` 再把它们拼到**末尾**（gcc 后者胜，
# 与主机直编路径同一语义）。flag 值含空格/反斜杠 → 整体单引号包成单个 shell 词，
# docker 只取 '=' 之后的字面值，不再过一层 shell 展开。
# （docker-env 里 apk/cmake 构建的依赖库不注入：它们由 tag 戳缓存，注入既不触发重编，
#   又会把主机的 glibc 工具链假设带进 musl 容器。）
DOCKER_ENV_FLAGS = -e 'CFLAGS=$(HOST_CFLAGS)' -e 'CXXFLAGS=$(HOST_CXXFLAGS)' -e 'LDFLAGS=$(HOST_LDFLAGS)'

# libsolv 锁定版本（tag，稳定发布版，非浮动 HEAD）：git clone --branch 拉取发布 tag。
# /usr/lib/libsolv.tag 版本戳记录已装版本，改 tag 后 docker-env 自动重编（见下），无需手动删容器。
LIBSOLV_TAG = 0.7.39
# libgit2 锁定版本（tag，用于 git clone --branch）：与 libsolv 同策略，改此变量后
# docker-env 通过 /usr/lib/libgit2.tag 版本戳自动重编（见下），无需手动删容器。
LIBGIT2_TAG = v1.7.2

# 容器内两个 Alpine 没有（或静态库不可用）的依赖，从源码构建：
# libgit2 —— alpine 的 libgit2-static 依赖系统 http-parser/ssh2，而 http-parser 无静态库，
# 全静态链接会失败；故自建（bundled http-parser、USE_SSH=OFF）。锁定 LIBGIT2_TAG（见上），
# /usr/lib/libgit2.tag 记录已装版本，与 pin 不符或 .a 缺失时重编。
# libsolv —— Alpine 无此包（apk 自家 resolver 不用 libsolv）。锁定 LIBSOLV_TAG（见上，稳定
# 发布 tag），/usr/lib/libsolv.tag 记录已装版本，与 pin 不符或 lib 缺失时重编，防止现有容器
# 静默沿用旧版 libsolv（版本漂移的来源）。cmake：共享库 libsolv.so 默认无条件构建（无需
# ENABLE_SHARED）；ENABLE_STATIC=ON 额外构建静态库 libsolv.a（供静态链接的 lpkg 二进制）。
# libsolv 无 ENABLE_TESTS 选项。
# （这些注释写在 recipe **外面**：recipe 里的 `#` 行会被 make 原样回显到构建日志里刷屏。）
docker-env:
	@mkdir -p $(BUILD_DIR)
	@if ! docker inspect $(DOCKER_ENV_CONTAINER) >/dev/null 2>&1; then \
		echo "Initializing pure build environment..."; \
		docker run --name $(DOCKER_ENV_CONTAINER) -d alpine:3.19 sleep infinity; \
	elif [ "$$(docker inspect -f '{{.State.Running}}' $(DOCKER_ENV_CONTAINER))" = "false" ]; then \
		echo "Container exists but is stopped. Starting $(DOCKER_ENV_CONTAINER)..."; \
		docker start $(DOCKER_ENV_CONTAINER) >/dev/null; \
	fi
	@# `clang` + `compiler-rt`：`make test-sanitize` 用（**容器自带的 gcc 不带 sanitizer
	@# 运行时** —— 实测 `g++ -fsanitize=address` 报 `cannot find -lasan`；alpine 的 gcc 只装了
	@# 头文件 `plugin/include/asan.h`）。`clang-extra-tools`：`make tidy` 用的 `clang-tidy`
	@# （**不在 `clang` 包里**，实测 `apk info -L clang | grep -c clang-tidy` = 0）。
	@docker exec $(DOCKER_ENV_CONTAINER) apk add --no-cache build-base bash pkgconfig curl-dev curl-static libarchive-dev libarchive-static openssl-dev openssl-libs-static zlib-static zstd-static lz4-static bzip2-static xz-static brotli-static nghttp2-static libpsl-static libidn2-static libunistring-static c-ares-static acl-static expat-static elfutils-dev upx gtest-dev tar sudo git cmake clang compiler-rt clang-extra-tools
	@docker exec $(DOCKER_ENV_CONTAINER) sh -c "LIBGIT2_TAG=$(LIBGIT2_TAG); if [ ! -f /usr/lib/libgit2.a ] || [ \"\$$(cat /usr/lib/libgit2.tag 2>/dev/null)\" != \"\$$LIBGIT2_TAG\" ]; then rm -rf /tmp/libgit2 && git clone -q --depth 1 --branch \$$LIBGIT2_TAG https://github.com/libgit2/libgit2.git /tmp/libgit2 && cd /tmp/libgit2 && mkdir -p build && cd build && cmake .. -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_SHARED_LIBS=OFF -DBUILD_CLI=OFF -DBUILD_TESTS=OFF -DUSE_SSH=OFF -DUSE_HTTPS=OpenSSL >/dev/null && make -j\$$(nproc) >/dev/null && make install >/dev/null && echo \$$LIBGIT2_TAG > /usr/lib/libgit2.tag; fi"
	@docker exec $(DOCKER_ENV_CONTAINER) sh -c "LIBSOLV_TAG=$(LIBSOLV_TAG); if [ ! -f /usr/lib/libsolv.so ] || [ \"\$$(cat /usr/lib/libsolv.tag 2>/dev/null)\" != \"\$$LIBSOLV_TAG\" ]; then rm -rf /tmp/libsolv && git clone -q --depth 1 --branch \$$LIBSOLV_TAG https://github.com/openSUSE/libsolv.git /tmp/libsolv && cd /tmp/libsolv && mkdir -p build && cd build && cmake .. -DCMAKE_INSTALL_PREFIX=/usr -DENABLE_STATIC=ON >/dev/null && make -j\$$(nproc) >/dev/null && make install >/dev/null && echo \$$LIBSOLV_TAG > /usr/lib/libsolv.tag; fi"

docker-sync:
	@echo "Syncing source code to container..."
	@docker exec $(DOCKER_ENV_CONTAINER) sh -c "rm -rf /app/* /app/.* 2>/dev/null; true"
	@docker cp . $(DOCKER_ENV_CONTAINER):/app

$(BUILD_DIR)/$(TARGET)-docker: docker-env
	$(MAKE) docker-sync
	docker exec $(DOCKER_ENV_FLAGS) -w /app $(DOCKER_ENV_CONTAINER) sh -c "make clean && make static -j$$(nproc) && strip --strip-all $(BUILD_DIR)/$(TARGET)-static && upx -9 $(BUILD_DIR)/$(TARGET)-static"
	docker cp $(DOCKER_ENV_CONTAINER):/app/$(BUILD_DIR)/$(TARGET)-static $@
	@echo "--------------------------------------------------------"
	@echo "Success! Pure static binary is ready at: $@"

docker: $(BUILD_DIR)/$(TARGET)-docker

