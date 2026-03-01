# Root Makefile for lpkg
CXX ?= g++

# Use pkg-config to resolve dependencies
# Removed libcares from explicit list as libcurl should bring it in if needed
PKGS = libcurl libarchive libcrypto
LIBS = $(shell pkg-config --libs $(PKGS))
TEST_LIBS = $(LIBS) -lgtest -lgtest_main -lpthread

# Override LIBS for static build
ifeq ($(findstring static,$(MAKECMDGOALS)),static)
    # Using --start-group/--end-group to handle complex static dependency chains
    # We use pkg-config --static to get all transitive dependencies
    LIBS = -Wl,--start-group $(shell pkg-config --libs --static $(PKGS)) -Wl,--end-group -static
endif

TARGET = lpkg
TEST_TARGET = run_tests
VERSION ?= 1.0.3

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

# Pass paths to C++ code
CXXFLAGS = -std=c++20 -Wall -Wextra -O2 -I$(MAIN_DIR)/third_party/include -I$(APP_SRC_DIR) \
           -DLPKG_CONF_DIR=\"$(INSTALL_CONF_DIR)\" \
           -DLPKG_L10N_DIR=\"$(INSTALL_L10N_DIR)\" \
           -DLPKG_DOCS_DIR=\"$(INSTALL_DOCS_DIR)\" \
           -DLPKG_LOCK_DIR=\"$(INSTALL_LOCK_DIR)\" \
           -DLPKG_VERSION=\"$(VERSION)\" \
           -DFMT_HEADER_ONLY

TEST_CXXFLAGS = -std=c++20 -Wall -Wextra -g -I$(MAIN_DIR)/third_party/include -I$(APP_SRC_DIR) \
           -DLPKG_CONF_DIR=\"$(INSTALL_CONF_DIR)\" \
           -DLPKG_L10N_DIR=\"$(INSTALL_L10N_DIR)\" \
           -DLPKG_DOCS_DIR=\"$(INSTALL_DOCS_DIR)\" \
           -DLPKG_LOCK_DIR=\"$(INSTALL_LOCK_DIR)\" \
           -DFMT_HEADER_ONLY

MAIN_SRCS = $(wildcard $(APP_SRC_DIR)/*.cpp)
TEST_SRCS = $(wildcard $(TESTS_DIR)/*.cpp)
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

.PHONY: all clean install uninstall static test

all: $(BUILD_DIR)/$(TARGET)

test:
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
	./$(BUILD_DIR)/$(TEST_TARGET)

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

install: all
	install -Dm755 $(BUILD_DIR)/$(TARGET) $(INSTALL_BIN_DIR)/$(TARGET)
	install -Dm644 $(MAIN_DIR)/conf/mirror.conf $(INSTALL_CONF_DIR)/mirror.conf
	install -d -m755 $(INSTALL_L10N_DIR)
	install -m644 $(MAIN_DIR)/l10n/*.txt $(INSTALL_L10N_DIR)/
	install -d -m755 $(INSTALL_LOCK_DIR)
	install -d -m755 $(INSTALL_DOCS_DIR)

uninstall:
	rm -f $(INSTALL_BIN_DIR)/$(TARGET)
	rm -rf $(INSTALL_SHARE_DIR)
	rm -rf $(INSTALL_CONF_DIR)
	rm -rf $(INSTALL_LOCK_DIR)
