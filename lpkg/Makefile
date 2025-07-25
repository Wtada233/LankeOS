# Makefile
CXX = g++
CXXFLAGS = -std=c++20 -Wall -Wextra -O2
LIBS = -lcurl -larchive -lfmt
TARGET = lpkg
SRC_DIR = src
CONF_DIR = conf
L10N_DIR = l10n
BUILD_DIR = build

# Find all .cpp files in the source directory
SRCS = $(wildcard $(SRC_DIR)/*.cpp)
# Generate object file names from source file names
OBJS = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))

.PHONY: all clean install uninstall

all: $(BUILD_DIR)/$(TARGET)

$(BUILD_DIR)/$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

# Rule to compile .cpp files into .o files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

install: all
	install -Dm755 $(BUILD_DIR)/$(TARGET) /usr/bin/$(TARGET)
	install -Dm644 $(CONF_DIR)/mirror.conf /etc/lpkg/mirror.conf
	install -d /usr/share/lpkg/l10n
	install -m644 $(L10N_DIR)/*.txt /usr/share/lpkg/l10n/
	@echo "安装完成"

uninstall:
	rm -f /usr/bin/$(TARGET)
	rm -rf /usr/share/lpkg
	rm -rf /etc/lpkg
	@echo "lpkg 已卸载"