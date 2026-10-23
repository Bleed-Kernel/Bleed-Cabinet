ELF_TARGET_64 = cabinet

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifeq ($(UNAME_S)$(UNAME_M),Darwinarm64)
CC 	:= x86_64-elf-gcc
else
CC 	:= gcc
endif

ELF_BIN_DIR_64 = bin
ELF_OBJ_DIR_64 = $(ELF_BIN_DIR_64)/obj

LIB_DIR = sysroot/lib
LIBC = $(LIB_DIR)/blibc.a
CRT0 = $(LIB_DIR)/start.o


BLIBC_REPO = https://github.com/Bleed-Kernel/blibc.git
BLIBC_DIR  ?= $(if $(wildcard ../blibc/include/bleed),../blibc,external/blibc)

CRIME_REPO = https://github.com/Bleed-Kernel/Crime-Scene.git
CRIME_DIR  ?= $(if $(wildcard ../Crime-Scene/Makefile),../Crime-Scene,external/crime-scene)
CRIME_OUT  = libcrime
LIBCRIME   = $(CRIME_OUT)/libcrime.a

UIKIT_REPO = https://github.com/Bleed-Kernel/libuikit.git
UIKIT_DIR  ?= $(if $(wildcard ../libuikit/Makefile),../libuikit,external/libuikit)
UIKIT_OUT  = libuikit
LIBUIKIT   = $(UIKIT_OUT)/libuikit.a

COMMON_CFLAGS = \
	-ffreestanding \
	-fno-stack-protector \
	-fno-stack-check \
	-Wall -Wextra \
	-O2 \
	-nostdlib \
	-no-pie \
	-m64 \
	-Iinclude \
	-I$(UIKIT_OUT)/include \
	-I$(CRIME_OUT)/include \
	-Isysroot/include \
	-Isysroot/include/bleed \
	-nostdinc

LDFLAGS = \
	-static \
	-nostdlib \
	-no-pie \
	-m64 \
	-L$(LIB_DIR)

SRCS = $(shell find src -name '*.c')
ELF_OBJS_64 = $(patsubst src/%.c,$(ELF_OBJ_DIR_64)/%.o,$(SRCS))

all: $(ELF_TARGET_64)

OBJCOPY := $(shell command -v x86_64-elf-objcopy >/dev/null 2>&1 && echo x86_64-elf-objcopy || echo objcopy)
ICONS   := $(wildcard res/icon.png res/icon-16.png res/icon-32.png res/icon-48.png res/icons/*.png)

define embed_icons
	@if [ -f res/icon.png ]; then \
		$(OBJCOPY) --add-section .bleed.icon=res/icon.png \
			--set-section-flags .bleed.icon=noload,readonly $(1) || exit 1; \
	fi
	@for px in 16 32 48; do \
		if [ -f res/icon-$$px.png ]; then \
			$(OBJCOPY) --add-section .bleed.icon$$px=res/icon-$$px.png \
				--set-section-flags .bleed.icon$$px=noload,readonly $(1) || exit 1; \
		fi; \
	done
	@for f in res/icons/*.png; do \
		[ -f $$f ] || continue; \
		s=.bleed.cab.$$(basename $$f .png); \
		$(OBJCOPY) --add-section $$s=$$f --set-section-flags $$s=noload,readonly $(1) || exit 1; \
	done
	@if [ -f res/app.txt ]; then \
		$(OBJCOPY) --add-section .bleed.app=res/app.txt \
			--set-section-flags .bleed.app=noload,readonly $(1) || exit 1; \
	fi
endef

$(ELF_TARGET_64): blibc crime uikit $(ELF_OBJS_64) $(ICONS)
	@mkdir -p $(ELF_BIN_DIR_64)
	$(CC) $(LDFLAGS) \
		-o $(ELF_BIN_DIR_64)/$(ELF_TARGET_64) \
		$(CRT0) \
		$(ELF_OBJS_64) \
		$(LIBUIKIT) \
		$(LIBCRIME) \
		$(LIBC)
	$(call embed_icons,$(ELF_BIN_DIR_64)/$(ELF_TARGET_64))

$(ELF_OBJ_DIR_64)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) -MMD -MP -c $< -o $@

-include $(ELF_OBJS_64:.o=.d)

$(ELF_OBJS_64): | blibc crime uikit

# synced every build so the sysroot never goes stale against it
blibc:
	@echo "[BLIBC] Preparing blibc"
	@if [ ! -d "$(BLIBC_DIR)" ]; then \
		git clone $(BLIBC_REPO) $(BLIBC_DIR); \
	elif [ "$(BLIBC_DIR)" = "external/blibc" ]; then \
		cd $(BLIBC_DIR) && git pull --ff-only; \
	fi
	@$(MAKE) -C $(BLIBC_DIR)
	@mkdir -p sysroot
	@cp -Rp $(BLIBC_DIR)/sysroot/* sysroot/

crime: blibc
	@echo "[CRIME] Preparing crime-scene"
	@if [ ! -d "$(CRIME_DIR)" ]; then \
		git clone $(CRIME_REPO) $(CRIME_DIR); \
	elif [ "$(CRIME_DIR)" = "external/crime-scene" ]; then \
		cd $(CRIME_DIR) && git pull --ff-only; \
	fi
	@$(MAKE) -C $(CRIME_DIR) bin/libcrime.a BLIBC_DIR=$(abspath $(BLIBC_DIR))
	@echo "[CRIME] Syncing $(CRIME_OUT)"
	@rm -rf $(CRIME_OUT)/include
	@mkdir -p $(CRIME_OUT)/include/crime
	@cp -p $(CRIME_DIR)/include/crime/*.h $(CRIME_OUT)/include/crime/
	@cmp -s $(CRIME_DIR)/bin/libcrime.a $(LIBCRIME) || cp -p $(CRIME_DIR)/bin/libcrime.a $(LIBCRIME)

uikit: crime
	@echo "[UIKIT] Preparing libuikit"
	@if [ ! -d "$(UIKIT_DIR)" ]; then \
		git clone $(UIKIT_REPO) $(UIKIT_DIR); \
	elif [ "$(UIKIT_DIR)" = "external/libuikit" ]; then \
		cd $(UIKIT_DIR) && git pull --ff-only; \
	fi
	@$(MAKE) -C $(UIKIT_DIR) bin/libuikit.a BLIBC_DIR=$(abspath $(BLIBC_DIR)) CRIME_DIR=$(abspath $(CRIME_DIR))
	@echo "[UIKIT] Syncing $(UIKIT_OUT)"
	@rm -rf $(UIKIT_OUT)/include
	@mkdir -p $(UIKIT_OUT)/include
	@cp -p $(UIKIT_DIR)/include/bleed_uikit.h $(UIKIT_OUT)/include/
	@cmp -s $(UIKIT_DIR)/bin/libuikit.a $(LIBUIKIT) || cp -p $(UIKIT_DIR)/bin/libuikit.a $(LIBUIKIT)

install: all
	COPYFILE_DISABLE=1 cp $(ELF_BIN_DIR_64)/$(ELF_TARGET_64) ../Bleed-Kernel/initrd/bin/

build: all

clean:
	rm -rf bin

distclean:
	rm -rf bin sysroot external $(CRIME_OUT) $(UIKIT_OUT)

.PHONY: all build clean distclean blibc crime uikit install $(ELF_TARGET_64)
