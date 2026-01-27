# Root Makefile for lpkg
CXX ?= g++
LIBS = -lcurl -larchive -lfmt -lcrypto
TEST_LIBS = $(LIBS) -lgtest -lgtest_main -lpthread

TARGET = lpkg
TEST_TARGET = run_tests

BUILD_DIR = build
MAIN_DIR = main
APP_SRC_DIR = $(MAIN_DIR)/src
TESTS_DIR = tests

# --- Coverage Flags ---
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage

# --- Install Directories (Matching original main/Makefile exactly) ---
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
# These macros must match the original Makefile's behavior.
CXXFLAGS = -std=c++20 -Wall -Wextra -O2 -I$(MAIN_DIR)/third_party/include -I$(APP_SRC_DIR) \
           -DLPKG_CONF_DIR=\"$(INSTALL_CONF_DIR)\" \
           -DLPKG_L10N_DIR=\"$(INSTALL_L10N_DIR)\" \
           -DLPKG_DOCS_DIR=\"$(INSTALL_DOCS_DIR)\" \
           -DLPKG_LOCK_DIR=\"$(INSTALL_LOCK_DIR)\"

TEST_CXXFLAGS = -std=c++20 -Wall -Wextra -g -I$(MAIN_DIR)/third_party/include -I$(APP_SRC_DIR) \
           -DLPKG_CONF_DIR=\"$(INSTALL_CONF_DIR)\" \
           -DLPKG_L10N_DIR=\"$(INSTALL_L10N_DIR)\" \
           -DLPKG_DOCS_DIR=\"$(INSTALL_DOCS_DIR)\" \
           -DLPKG_LOCK_DIR=\"$(INSTALL_LOCK_DIR)\"

# Source files
MAIN_SRCS = $(wildcard $(APP_SRC_DIR)/*.cpp)
TEST_SRCS = $(wildcard $(TESTS_DIR)/*.cpp)

# Object files
MAIN_OBJS = $(patsubst $(APP_SRC_DIR)/%.cpp,$(BUILD_DIR)/main/%.o,$(MAIN_SRCS))
TEST_OBJS = $(patsubst $(TESTS_DIR)/%.cpp,$(BUILD_DIR)/tests/%.o,$(TEST_SRCS))

# App objects excluding main.o for testing
LPKG_OBJS = $(filter-out $(BUILD_DIR)/main/main.o, $(MAIN_OBJS))

# Dependency files
DEPS = $(MAIN_OBJS:.o=.d) $(TEST_OBJS:.o=.d)

# Handle coverage mode flags internally to avoid shell quoting issues
ifeq ($(COVERAGE_MODE),1)
    CXXFLAGS += $(COVERAGE_FLAGS)
    TEST_CXXFLAGS += $(COVERAGE_FLAGS)
    LIBS += -lgcov
    TEST_LIBS += -lgcov
endif

.PHONY: all clean install uninstall static test coverage

all: $(BUILD_DIR)/$(TARGET)

test: $(BUILD_DIR)/$(TEST_TARGET)
	./$(BUILD_DIR)/$(TEST_TARGET)

# Coverage target using a temporary directory to avoid polluting the main build
coverage:
	@echo "Starting coverage build in temporary directory..."
	@rm -rf build_coverage coverage_report
	@mkdir -p build_coverage
	@$(MAKE) BUILD_DIR=build_coverage COVERAGE_MODE=1 build_coverage_internal
	@mkdir -p coverage_report
	@if command -v lcov > /dev/null; then \
		lcov --rc geninfo_unexecuted_blocks=1 --capture --directory build_coverage --output-file build_coverage/coverage.info --ignore-errors mismatch,inconsistent,unused; \
		lcov --ignore-errors mismatch,inconsistent,unused --remove build_coverage/coverage.info '/usr/*' '*/third_party/*' '*/tests/*' --output-file build_coverage/coverage_filtered.info; \
		genhtml --ignore-errors mismatch,inconsistent --output-directory coverage_report build_coverage/coverage_filtered.info; \
		echo "Coverage report generated at coverage_report/index.html"; \
	else \
		echo "lcov not found, using gcov summary:"; \
		gcov -n $(filter-out $(APP_SRC_DIR)/main.cpp, $(MAIN_SRCS)) --object-directory build_coverage/main; \
	fi
	@rm -rf build_coverage
	@echo "------------------------------------------------------"
	@echo "Coverage report is available in: coverage_report/index.html"
	@echo "Temporary build files have been cleaned up."

build_coverage_internal: $(BUILD_DIR)/$(TEST_TARGET)
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
	rm -rf $(BUILD_DIR) build_coverage coverage_report

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
