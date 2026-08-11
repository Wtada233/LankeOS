# Root Makefile for lpkg
CXX ?= g++

# Use pkg-config to resolve dependencies
# Removed libcares from explicit list as libcurl should bring it in if needed
# libgit2 是静态库，其 openssl.c 引用 SSL_set_bio（在 libssl）。静态库依赖的共享库必须排在
# 它**之后**（--as-needed 下前置的 -lssl 无符号引用会被丢弃 → "DSO missing from command line"）。
# 故 openssl 放最后，保证 -lssl -lcrypto 紧跟 -lgit2。
PKGS = libcurl libarchive libelf libgit2 libsolv openssl
PKG_CFLAGS = $(shell pkg-config --cflags $(PKGS))
LIBS = $(shell pkg-config --libs $(PKGS))
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

TARGET = lpkg
TEST_TARGET = run_tests
VERSION ?= 6.1.0

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
           -Werror -Wall -Wextra \
           -Wl,--copy-dt-needed-entries

CXXFLAGS = $(COMMON_CXXFLAGS) -O2
TEST_CXXFLAGS = $(COMMON_CXXFLAGS) -g

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

.PHONY: all clean install uninstall static test build_test_internal docker docker-env docker-sync

all: $(BUILD_DIR)/$(TARGET)

test: docker-env
	@$(MAKE) docker-sync
	@# 装 l10n 到编译路径（LPKG_L10N_DIR=/usr/share/lpkg/l10n），否则 get_string 全返回 [MISSING_STRING]
	@docker exec $(DOCKER_ENV_CONTAINER) sh -c "mkdir -p /usr/share/lpkg && cp -r /app/main/l10n /usr/share/lpkg/"
	@docker exec -w /app $(DOCKER_ENV_CONTAINER) sh -c "make clean && make -j$$(nproc) build/run_tests && ./build/run_tests"
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

$(BUILD_DIR)/$(TARGET): $(MAIN_OBJS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

$(BUILD_DIR)/$(TEST_TARGET): $(TEST_OBJS) $(LPKG_OBJS)
	@mkdir -p $(@D)
	$(CXX) $(TEST_CXXFLAGS) -o $@ $^ $(TEST_LIBS)

static: $(BUILD_DIR)/$(TARGET)-static

$(BUILD_DIR)/$(TARGET)-static: $(MAIN_OBJS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -static -o $@ $^ $(LIBS)

$(BUILD_DIR)/main/%.o: $(APP_SRC_DIR)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/tests/%.o: $(TESTS_DIR)/%.cpp
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
# libsolv 锁定版本（tag，稳定发布版，非浮动 HEAD）：git clone --branch 拉取发布 tag。
# /usr/lib/libsolv.tag 版本戳记录已装版本，改 tag 后 docker-env 自动重编（见下），无需手动删容器。
LIBSOLV_TAG = 0.7.39
# libgit2 锁定版本（tag，用于 git clone --branch）：与 libsolv 同策略，改此变量后
# docker-env 通过 /usr/lib/libgit2.tag 版本戳自动重编（见下），无需手动删容器。
LIBGIT2_TAG = v1.7.2

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
	# 从源码构建 libgit2-static（bundled http-parser、USE_SSH=OFF）：alpine 的 libgit2-static 依赖系统
	# http-parser/ssh2，而 http-parser 无静态库，全静态链接会失败。
	# 锁定 LIBGIT2_TAG（见上）；/usr/lib/libgit2.tag 记录已装版本，与 pin 不符或 .a 缺失时重编。
	@docker exec $(DOCKER_ENV_CONTAINER) sh -c "LIBGIT2_TAG=$(LIBGIT2_TAG); if [ ! -f /usr/lib/libgit2.a ] || [ \"\$$(cat /usr/lib/libgit2.tag 2>/dev/null)\" != \"\$$LIBGIT2_TAG\" ]; then rm -rf /tmp/libgit2 && git clone -q --depth 1 --branch \$$LIBGIT2_TAG https://github.com/libgit2/libgit2.git /tmp/libgit2 && cd /tmp/libgit2 && mkdir -p build && cd build && cmake .. -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_SHARED_LIBS=OFF -DBUILD_CLI=OFF -DBUILD_TESTS=OFF -DUSE_SSH=OFF -DUSE_HTTPS=OpenSSL >/dev/null && make -j\$$(nproc) >/dev/null && make install >/dev/null && echo \$$LIBGIT2_TAG > /usr/lib/libgit2.tag; fi"
	# 从源码构建 libsolv（Alpine 无此包——apk 自家 resolver 不用 libsolv，故未打包）。
	# 锁定 LIBSOLV_TAG（见上，稳定发布 tag）；/usr/lib/libsolv.tag 记录已装版本，与 pin 不符
	# 或 lib 缺失时重编——防止现有容器静默沿用旧版 libsolv（版本漂移的来源）。
	# cmake：共享库 libsolv.so 默认无条件构建（无需 ENABLE_SHARED）；ENABLE_STATIC=ON
	# 额外构建静态库 libsolv.a（供静态链接的 lpkg 二进制）。libsolv 无 ENABLE_TESTS 选项。
	@docker exec $(DOCKER_ENV_CONTAINER) sh -c "LIBSOLV_TAG=$(LIBSOLV_TAG); if [ ! -f /usr/lib/libsolv.so ] || [ \"\$$(cat /usr/lib/libsolv.tag 2>/dev/null)\" != \"\$$LIBSOLV_TAG\" ]; then rm -rf /tmp/libsolv && git clone -q --depth 1 --branch \$$LIBSOLV_TAG https://github.com/openSUSE/libsolv.git /tmp/libsolv && cd /tmp/libsolv && mkdir -p build && cd build && cmake .. -DCMAKE_INSTALL_PREFIX=/usr -DENABLE_STATIC=ON >/dev/null && make -j\$$(nproc) >/dev/null && make install >/dev/null && echo \$$LIBSOLV_TAG > /usr/lib/libsolv.tag; fi"

docker-sync:
	@echo "Syncing source code to container..."
	@docker exec $(DOCKER_ENV_CONTAINER) sh -c "rm -rf /app/* /app/.* 2>/dev/null; true"
	@docker cp . $(DOCKER_ENV_CONTAINER):/app

$(BUILD_DIR)/$(TARGET)-docker: docker-env
	$(MAKE) docker-sync
	docker exec -w /app $(DOCKER_ENV_CONTAINER) sh -c "make clean && make static -j$$(nproc) && strip --strip-all $(BUILD_DIR)/$(TARGET)-static && upx -9 $(BUILD_DIR)/$(TARGET)-static"
	docker cp $(DOCKER_ENV_CONTAINER):/app/$(BUILD_DIR)/$(TARGET)-static $@
	@echo "--------------------------------------------------------"
	@echo "Success! Pure static binary is ready at: $@"

docker: $(BUILD_DIR)/$(TARGET)-docker

