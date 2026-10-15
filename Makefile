BUILD_DIR := build

# Apple Clang does not support MIPS, so on macOS default to Homebrew LLVM.
# `ifeq ($(origin CC),default)` catches make's built-in CC=cc, which `?=` would not override.
ifeq ($(OS),Windows_NT)
    CC      := clang
    LD      := ld.lld
else ifneq ($(shell uname),Darwin)
    CC      := clang
    LD      := ld.lld
else
    ifeq ($(origin CC),default)
        CC  := /opt/homebrew/opt/llvm/bin/clang
    endif
    ifeq ($(origin LD),default)
        LD  := /opt/homebrew/opt/lld/bin/ld.lld
    endif
endif

TARGET  := $(BUILD_DIR)/mod.elf

LDSCRIPT := mod.ld
ARCHFLAGS := -target mips -mips2 -mabi=32 -O2 -G0 -mno-abicalls -mno-odd-spreg -mno-check-zero-division \
             -fomit-frame-pointer -ffast-math -fno-unsafe-math-optimizations -fno-builtin-memset
WARNFLAGS := -Wall -Wextra -Wno-incompatible-library-redeclaration -Wno-unused-parameter -Wno-unknown-pragmas -Wno-unused-variable \
             -Wno-missing-braces -Wno-unsupported-floating-point-opt -Werror=section
CFLAGS   := $(ARCHFLAGS) $(WARNFLAGS) -D_LANGUAGE_C -nostdinc -ffunction-sections
CPPFLAGS := -DMIPS -DF3DEX_GBI_2 -DF3DEX_GBI_PL -DGBI_DOWHILE -I include -I include/dummy_headers \
            -I mm-decomp/include -I mm-decomp/src -I mm-decomp/extracted/n64-us -idirafter include/libc -idirafter mm-decomp/include/libc
LDFLAGS  := -nostdlib -T $(LDSCRIPT) -Map $(BUILD_DIR)/mod.map --unresolved-symbols=ignore-all --emit-relocs -e 0 --no-nmagic -gc-sections

rwildcard = $(foreach d,$(wildcard $(1:=/*)),$(call rwildcard,$d,$2) $(filter $(subst *,%,$2),$d))
getdirs = $(sort $(dir $(1)))

C_SRCS := $(call rwildcard,src,*.c)
C_OBJS := $(addprefix $(BUILD_DIR)/, $(C_SRCS:.c=.o))
C_DEPS := $(addprefix $(BUILD_DIR)/, $(C_SRCS:.c=.d))

ALL_OBJS := $(C_OBJS)
ALL_DEPS := $(C_DEPS)
BUILD_DIRS := $(call getdirs,$(ALL_OBJS))

# Native library. Its name must match the `native_libraries` entry in mod.toml.
NATIVE_NAME := mm_chat_native
NATIVE_CC   := /usr/bin/clang
NATIVE_LIB  := $(BUILD_DIR)/$(NATIVE_NAME).dylib
NRM         := $(BUILD_DIR)/mm_chat.nrm
MODS_DIR    := $(HOME)/Library/Application Support/Zelda64Recompiled/mods

all: $(TARGET) $(NATIVE_LIB)

$(TARGET): $(ALL_OBJS) $(LDSCRIPT) | $(BUILD_DIR)
	$(LD) $(ALL_OBJS) $(LDFLAGS) -o $@

$(BUILD_DIR) $(BUILD_DIRS):
ifeq ($(OS),Windows_NT)
	if not exist "$(subst /,\,$@)" mkdir "$(subst /,\,$@)"
else
	mkdir -p $@
endif

$(C_OBJS): $(BUILD_DIR)/%.o : %.c | $(BUILD_DIRS)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -MMD -MF $(@:.o=.d) -c -o $@

NATIVE_SRCS := native/chat_native.c native/chat_client.c
NATIVE_HDRS := native/chat_client.h

# Universal binary, since the macOS game build ships for both arm64 and x86_64.
$(NATIVE_LIB): $(NATIVE_SRCS) $(NATIVE_HDRS) | $(BUILD_DIR)
	$(NATIVE_CC) -dynamiclib -arch arm64 -arch x86_64 -O2 -Wall -Wextra -fvisibility=hidden -o $@ $(NATIVE_SRCS)

# Windows build of the native library, cross-compiled with mingw-w64 (`brew install mingw-w64`).
WINDOWS_CC  := x86_64-w64-mingw32-gcc
WINDOWS_LIB := $(BUILD_DIR)/$(NATIVE_NAME).dll

$(WINDOWS_LIB): $(NATIVE_SRCS) $(NATIVE_HDRS) | $(BUILD_DIR)
	$(WINDOWS_CC) -shared -O2 -Wall -Wextra -static-libgcc -o $@ $(NATIVE_SRCS) -lws2_32

windows: $(WINDOWS_LIB)

# Zip to share with friends: mod files for both platforms plus the server.
DIST_DIR := $(BUILD_DIR)/dist/MMChat
DIST_ZIP := $(BUILD_DIR)/MMChat.zip

dist: $(NRM) $(NATIVE_LIB) $(WINDOWS_LIB)
	rm -rf $(BUILD_DIR)/dist $(DIST_ZIP)
	mkdir -p $(DIST_DIR)/mods $(DIST_DIR)/server
	cp $(NRM) $(NATIVE_LIB) $(WINDOWS_LIB) $(DIST_DIR)/mods/
	cp server/mmchat_server.py server/mmchat_client.py $(DIST_DIR)/server/
	cp FRIENDS_README.txt $(DIST_DIR)/README.txt
	cd $(BUILD_DIR)/dist && zip -qr ../MMChat.zip MMChat
	@echo "Created $(DIST_ZIP)"

$(NRM): $(TARGET) mod.toml
	RecompModTool mod.toml $(BUILD_DIR)

package: $(NRM) $(NATIVE_LIB)

install: package
	cp "$(NRM)" "$(NATIVE_LIB)" "$(MODS_DIR)/"

clean:
ifeq ($(OS),Windows_NT)
	if exist $(BUILD_DIR) rmdir /S /Q $(BUILD_DIR)
else
	rm -rf $(BUILD_DIR)
endif

-include $(ALL_DEPS)

.PHONY: all package install clean windows dist
