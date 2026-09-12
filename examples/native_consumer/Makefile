# Standalone Makefile for PS5 Vulkan Public SDK Consumer
# Consumes strictly staged public SDK: dist-sdk/include and dist-sdk/lib

SDK_DIR ?= $(abspath ../../dist-sdk)
PS5_BOILERPLATE ?= $(abspath ../../../../third_party/ps5-native-app-boilerplate)
PS5_PAYLOAD_SDK ?= $(PS5_BOILERPLATE)/.deps/native/ps5-payload-sdk
CLANG_WRAPPER ?= $(PS5_BOILERPLATE)/tooling/prospero-clang18
LLD ?= $(PS5_PAYLOAD_SDK)/bin/prospero-lld
BUILDER ?= $(PS5_BOILERPLATE)/build/host/ps5-native-tool

BUILD_DIR ?= build
TARGET_ELF = $(BUILD_DIR)/consumer.elf
TARGET_PIE = $(BUILD_DIR)/consumer_pie.elf
MAP_FILE = $(BUILD_DIR)/consumer.map
DEP_FILE = $(BUILD_DIR)/main.d

CFLAGS = -std=c11 -O2 -g -Wall -Wextra -Werror \
         -ffunction-sections -fdata-sections \
         -MD -MP -MF $(DEP_FILE) \
         -I$(SDK_DIR)/include \
         -I.

ifneq ($(CONSUMER_CONTINUOUS),)
CFLAGS += -DCONSUMER_CONTINUOUS=1
endif

LDFLAGS = -L$(PS5_PAYLOAD_SDK)/target/lib \
          -T $(SDK_DIR)/lib/ps5-pie.ld \
          --eh-frame-hdr \
          --version-script $(SDK_DIR)/lib/app-symbols.map \
          -Map=$(MAP_FILE) \
          -e _start

LIBS = $(SDK_DIR)/lib/crt.o \
       $(BUILD_DIR)/main.o \
       $(SDK_DIR)/lib/libps5vk.a \
       $(SDK_DIR)/lib/libpsbc.a \
       $(PS5_PAYLOAD_SDK)/target/lib/libc++.a \
       $(PS5_PAYLOAD_SDK)/target/lib/libc++abi.a \
       $(PS5_PAYLOAD_SDK)/target/lib/libunwind.a \
       $(PS5_PAYLOAD_SDK)/target/lib/libpthread.a \
       $(PS5_PAYLOAD_SDK)/target/lib/libc.a \
       --as-needed \
       $(wildcard $(PS5_PAYLOAD_SDK)/target/lib/*.so) \
       $(SDK_DIR)/lib/libSceAgc.so \
       $(SDK_DIR)/lib/libSceAgcDriver.so

.PHONY: all clean

all: $(TARGET_PIE)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/main.o: main.c shaders.h ps5log.h | $(BUILD_DIR)
	PS5_PAYLOAD_SDK=$(PS5_PAYLOAD_SDK) sh $(CLANG_WRAPPER) $(CFLAGS) -c $< -o $@

$(TARGET_PIE): $(BUILD_DIR)/main.o
	$(LLD) $(LDFLAGS) -o $@ $(LIBS)

clean:
	rm -rf $(BUILD_DIR)
