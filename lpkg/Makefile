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
VERSION ?= 7.1.0

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
           $(PKG_CFLAGS) \
           -DLPKG_CONF_DIR=\"$(INSTALL_CONF_DIR)\" \
           -DLPKG_L10N_DIR=\"$(INSTALL_L10N_DIR)\" \
           -DLPKG_DOCS_DIR=\"$(INSTALL_DOCS_DIR)\" \
           -DLPKG_LOCK_DIR=\"$(INSTALL_LOCK_DIR)\" \
           -DLPKG_VERSION=\"$(VERSION)\" \
           -DFMT_HEADER_ONLY \
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
	$(wildcard $(APP_SRC_DIR)/scan/*.cpp)
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
	rm -rf $(BUILD_DIR) build_test coverage_report

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
	@docker exec $(DOCKER_ENV_CONTAINER) apk add --no-cache build-base bash pkgconfig curl-dev curl-static libarchive-dev libarchive-static openssl-dev openssl-libs-static zlib-static zstd-static lz4-static bzip2-static xz-static brotli-static nghttp2-static libpsl-static libidn2-static libunistring-static c-ares-static acl-static expat-static fmt-dev elfutils-dev upx gtest-dev tar sudo git cmake
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

