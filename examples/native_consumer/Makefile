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
         -I. \
         -I$(BUILD_DIR)

ifneq ($(CONSUMER_CONTINUOUS),)
CFLAGS += -DCONSUMER_CONTINUOUS=1
endif

ifneq ($(CONSUMER_SHARED_STAGE_SAMPLERS),)
ifneq ($(CONSUMER_CONTINUOUS),)
$(error Shared-stage qualification requires the finite consumer)
endif
ifneq ($(CONSUMER_SINGLE_SET_SAMPLERS),)
$(error Choose one sampled-descriptor profile)
endif
ifneq ($(CONSUMER_MIXED_RESOURCES),)
$(error Choose one sampled-descriptor profile)
endif

CFLAGS += -DCONSUMER_SHARED_STAGE_SAMPLERS=1
endif

ifneq ($(CONSUMER_DXVK262_PROBE),)
CFLAGS += -DCONSUMER_DXVK262_PROBE=1
endif

ifneq ($(CONSUMER_SINGLE_SET_SAMPLERS),)
ifneq ($(CONSUMER_CONTINUOUS),)
$(error Single-set qualification requires the finite consumer)
endif
ifneq ($(CONSUMER_MIXED_RESOURCES),)
$(error Choose one sampled-descriptor profile)
endif
CFLAGS += -DCONSUMER_SINGLE_SET_SAMPLERS=1
endif

ifneq ($(CONSUMER_MIXED_RESOURCES),)
ifneq ($(CONSUMER_CONTINUOUS),)
$(error Mixed-resource qualification requires the finite consumer)
endif
CFLAGS += -DCONSUMER_MIXED_RESOURCES=1
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

.PHONY: all clean FORCE

all: $(TARGET_PIE)

# Mode switches change preprocessor flags, not source timestamps.
FORCE:

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/storage_width_shaders.h: ../../tools/prepare_consumer_storage_shaders.py \
                                     ../../tools/prepare_test_shaders.py \
                                     ../../experiments/compute/storage8.comp \
                                     ../../experiments/compute/storage16.comp | $(BUILD_DIR)
	python3 ../../tools/prepare_consumer_storage_shaders.py --out $@

$(BUILD_DIR)/sync_shaders.h: ../../tools/prepare_consumer_sync_shaders.py \
                            ../../tools/prepare_test_shaders.py \
                            ../../experiments/compute/sync_producer.comp \
                            ../../experiments/compute/sync_consumer.comp \
                            ../../experiments/compute/shared_atomic_multiwave.comp | $(BUILD_DIR)
	python3 ../../tools/prepare_consumer_sync_shaders.py --out $@

$(BUILD_DIR)/sampled_set_shaders.h: ../../tools/prepare_consumer_sampled_shaders.py \
                                 ../../tools/prepare_consumer_sync_shaders.py \
                                 ../../experiments/graphics/runtime_sampled_sets.frag \
                                 ../../experiments/graphics/runtime_single_set_samplers.frag \
                                 ../../experiments/graphics/runtime_mixed_sets.frag \
                                 ../../experiments/graphics/runtime_shared_sets.vert \
                                 ../../experiments/graphics/runtime_shared_sets.frag | $(BUILD_DIR)
	python3 ../../tools/prepare_consumer_sampled_shaders.py --out $@

$(BUILD_DIR)/main.o: main.c shaders.h resource_shader.h ps5log.h sampled_sets.h physical_device_contract.h \
                     dxvk_capability_probe.h dxvk_v262_profile.h FORCE \
                     $(BUILD_DIR)/sampled_set_shaders.h \
                     $(BUILD_DIR)/storage_width_shaders.h \
                     $(BUILD_DIR)/sync_shaders.h | $(BUILD_DIR)
	PS5_PAYLOAD_SDK=$(PS5_PAYLOAD_SDK) sh $(CLANG_WRAPPER) $(CFLAGS) -c $< -o $@

$(TARGET_PIE): $(BUILD_DIR)/main.o
	$(LLD) $(LDFLAGS) -o $@ $(LIBS)

clean:
	rm -rf $(BUILD_DIR)
