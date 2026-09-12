LAB_ROOT ?= ../..
PS5_PAYLOAD_SDK ?= $(abspath $(LAB_ROOT)/../ps5debug-NG/ps5-payload-sdk/install)
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
SOURCE := $(LAB_ROOT)/tools/bigapp-control/main.c
all: build/control/status.elf build/control/launch.elf build/control/close.elf
build/control:
	mkdir -p $@
build/control/status.elf: $(SOURCE) | build/control
	$(CC) -O2 -Wall -Wextra -Werror -DACTION_STATUS=1 -DTARGET_TITLE='"PPSA99994"' -o $@ $< -lSceSystemService -lSceUserService
build/control/launch.elf: $(SOURCE) | build/control
	$(CC) -O2 -Wall -Wextra -Werror -DACTION_LAUNCH=1 -DTARGET_TITLE='"PPSA99994"' -o $@ $< -lSceSystemService -lSceUserService
build/control/close.elf: $(SOURCE) | build/control
	$(CC) -O2 -Wall -Wextra -Werror -DTARGET_TITLE='"PPSA99994"' -o $@ $< -lSceSystemService -lSceUserService
