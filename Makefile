CXX := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Werror -pedantic \
            -fno-exceptions -fno-rtti -flto=auto \
            -fno-asynchronous-unwind-tables -fno-unwind-tables \
            -ffunction-sections -fdata-sections -D_GNU_SOURCE
LDFLAGS := -Wl,--gc-sections -Wl,-z,now -Wl,-z,relro -s

TARGET := amdgpu-sentinel
SRC_DIR := src
BUILD_DIR := bin
INSTALL_BIN := $(HOME)/.local/bin
INSTALL_SYSTEMD := $(HOME)/.config/systemd/user

SRCS := $(SRC_DIR)/main.cpp
OBJS := $(BUILD_DIR)/main.o

.PHONY: all clean install uninstall

all: $(BUILD_DIR)/$(TARGET)

$(BUILD_DIR)/$(TARGET): $(SRCS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

install: $(BUILD_DIR)/$(TARGET)
	mkdir -p $(INSTALL_BIN)
	install -m 755 $(BUILD_DIR)/$(TARGET) $(INSTALL_BIN)/$(TARGET)
	mkdir -p $(INSTALL_SYSTEMD)
	@if [ -f systemd/amdgpu-sentinel.service ]; then \
		install -m 644 systemd/amdgpu-sentinel.service $(INSTALL_SYSTEMD)/amdgpu-sentinel.service; \
		systemctl --user daemon-reload; \
	fi

uninstall:
	rm -f $(INSTALL_BIN)/$(TARGET)
	rm -f $(INSTALL_SYSTEMD)/amdgpu-sentinel.service
	systemctl --user daemon-reload