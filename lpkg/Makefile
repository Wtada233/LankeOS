# Makefile
CC = g++
CFLAGS = -std=c++17 -Wall -Wextra -O2
LIBS = -lcurl -larchive
TARGET = lpkg
SRC_DIR = src
CONF_DIR = conf
BUILD_DIR = build

.PHONY: all clean install uninstall

all: $(BUILD_DIR)/$(TARGET)

$(BUILD_DIR)/$(TARGET): $(SRC_DIR)/main.cpp | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/$(TARGET) $(SRC_DIR)/main.cpp $(LIBS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

install: $(BUILD_DIR)/$(TARGET)
	install -Dm755 $(BUILD_DIR)/$(TARGET) /usr/bin/$(TARGET)
	install -Dm644 $(CONF_DIR)/mirror.conf /etc/lpkg/mirror.conf
	@echo "安装完成"

uninstall:
	rm -f /usr/bin/$(TARGET)
	rm -rf /usr/share/lpkg
	rm -rf /etc/lpkg
	
	@echo "lpkg 已卸载"
