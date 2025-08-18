# Makefile
CXX ?= g++
LIBS ?= -lcurl -larchive -lfmt -lcrypto
TARGET ?= lpkg
SRC_DIR ?= src
BUILD_DIR ?= build
CONF_SRC_DIR ?= conf
L10N_SRC_DIR ?= l10n

# --- Install Directories ---
# Use `make PREFIX=/usr/local` to change the base directory
PREFIX ?= /usr
SYSCONFDIR ?= /etc
LOCALSTATEDIR ?= /var

# These are the final destination directories
INSTALL_BIN_DIR = $(DESTDIR)$(PREFIX)/bin
INSTALL_CONF_DIR = $(DESTDIR)$(SYSCONFDIR)/lpkg
INSTALL_SHARE_DIR = $(DESTDIR)$(PREFIX)/share/lpkg
INSTALL_L10N_DIR = $(INSTALL_SHARE_DIR)/l10n
INSTALL_DOCS_DIR = $(INSTALL_SHARE_DIR)/docs
INSTALL_LOCK_DIR = $(DESTDIR)$(LOCALSTATEDIR)/lpkg

# Pass paths to C++ code
CXXFLAGS = -std=c++20 -Wall -Wextra -O2 -I./third_party/include \
           -DLPKG_CONF_DIR=\"$(INSTALL_CONF_DIR)\" \
           -DLPKG_L10N_DIR=\"$(INSTALL_L10N_DIR)\" \
           -DLPKG_DOCS_DIR=\"$(INSTALL_DOCS_DIR)\" \
           -DLPKG_LOCK_DIR=\"$(INSTALL_LOCK_DIR)\"

SRCS = $(wildcard $(SRC_DIR)/*.cpp)
OBJS = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))
DEPS = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.d,$(SRCS))

.PHONY: all clean install uninstall static

all: $(BUILD_DIR)/$(TARGET)

static: $(BUILD_DIR)/$(TARGET)-static

$(BUILD_DIR)/$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

$(BUILD_DIR)/$(TARGET)-static: $(OBJS)
	$(CXX) $(CXXFLAGS) -static -o $@ $^ $(LIBS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

-include $(DEPS)

clean:
	rm -rf $(BUILD_DIR)

install: all
	install -Dm755 $(BUILD_DIR)/$(TARGET) $(INSTALL_BIN_DIR)/$(TARGET)
	install -Dm644 $(CONF_SRC_DIR)/mirror.conf $(INSTALL_CONF_DIR)/mirror.conf
	install -d -m755 $(INSTALL_L10N_DIR)
	install -m644 $(L10N_SRC_DIR)/*.txt $(INSTALL_L10N_DIR)/
	install -d -m755 $(INSTALL_LOCK_DIR)
	install -d -m755 $(INSTALL_DOCS_DIR)

uninstall:
	rm -f $(INSTALL_BIN_DIR)/$(TARGET)
	rm -rf $(INSTALL_SHARE_DIR)
	rm -rf $(INSTALL_CONF_DIR)
	rm -rf $(INSTALL_LOCK_DIR)
