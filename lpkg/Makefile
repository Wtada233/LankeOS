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

.PHONY: all clean install uninstall static test

all: $(BUILD_DIR)/$(TARGET)

test: $(BUILD_DIR)/$(TEST_TARGET)
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
	rm -rf $(BUILD_DIR)

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