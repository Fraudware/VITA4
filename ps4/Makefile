# VITA4 - PS4 homebrew build (OpenOrbis PS4 Toolchain).
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   export OO_PS4_TOOLCHAIN=/path/to/OpenOrbis/PS4Toolchain
#   make            -> IV0000-VITA00004_00-VITA4DOCK0000000.pkg
#   make test       -> host unit tests (no console needed)
#   make preview    -> renders every screen to build/preview/*.png

TITLE       := VITA4
VERSION := 1.00
TITLE_ID    := VITA00004
CONTENT_ID  := IV0000-VITA00004_00-VITA4DOCK0000000

TOOLCHAIN   ?= $(OO_PS4_TOOLCHAIN)
ROOT        := ..
BUILD       := build
OBJDIR      := $(BUILD)/ps4

LIBS        := -lc -lkernel -lc++ -lSceVideoOut -lScePad -lSceUserService -lSceAudioOut \
               -lSceSysmodule -lSceSystemService -lSceUsbd

INCLUDES    := -Isrc -I$(ROOT)/core/include -I$(ROOT)/vita-side/input-receiver/include \
               -Ithird_party/stb

APP_C       := $(wildcard src/*.c)
APP_CPP     := $(wildcard src/*.cpp)
SHARED_C    := $(ROOT)/core/src/uvc_protocol.c $(ROOT)/core/src/pad_passthrough.c \
               $(ROOT)/vita-side/input-receiver/src/pad_wire.c
# The PS5 build's raw-USB (FreeBSD ugen) code, used as the GoldHEN fallback.
UGEN_C      := $(ROOT)/core/src/usb_vita.c $(ROOT)/core/src/usb_transfer.c \
               $(ROOT)/core/src/uvc_stream.c $(ROOT)/core/src/uac_audio.c

OBJS        := $(patsubst src/%.c,$(OBJDIR)/%.o,$(APP_C)) \
               $(patsubst src/%.cpp,$(OBJDIR)/%.o,$(APP_CPP)) \
               $(patsubst %.c,$(OBJDIR)/shared/%.o,$(notdir $(SHARED_C) $(UGEN_C)))

CC          := clang
CXX         := clang++
LD          := ld.lld
CDIR        := linux

TARGET      := --target=x86_64-pc-freebsd12-elf
# compat/ supplies FreeBSD's USB ioctl headers (the PS4 kernel is FreeBSD-based,
# the OpenOrbis libc headers are musl). PS4 build only - never the host tests.
CFLAGS      := $(TARGET) -fPIC -funwind-tables -O2 -Wall -c -isysroot $(TOOLCHAIN) \
               -Icompat -isystem $(TOOLCHAIN)/include $(INCLUDES)
CXXFLAGS    := $(CFLAGS) -std=gnu++17 -isystem $(TOOLCHAIN)/include/c++/v1
# Every allocator call goes through src/malloc_lock.c (musl's own malloc lock
# never switches on for PS4 system threads - see that file).
HEAP_WRAP   := --wrap=malloc --wrap=free --wrap=calloc --wrap=realloc --wrap=aligned_alloc \
               --wrap=posix_memalign --wrap=memalign --wrap=valloc --wrap=malloc_usable_size
LDFLAGS     := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr $(HEAP_WRAP) \
               -L$(TOOLCHAIN)/lib $(LIBS) $(TOOLCHAIN)/lib/crt1.o

PKG_FILES   := eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png \
               sce_sys/pic0.png sce_sys/pic1.png $(wildcard sce_module/*) \
               assets/backdrop.png assets/splash.png

# The packager is a .NET app: no ICU needed for what it does.
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT := 1

.PHONY: all clean check-toolchain test preview font art

all: check-toolchain $(CONTENT_ID).pkg

check-toolchain:
	@test -n "$(TOOLCHAIN)" -a -d "$(TOOLCHAIN)/lib" || \
	  (echo "Set OO_PS4_TOOLCHAIN to your OpenOrbis PS4Toolchain folder"; exit 1)

$(CONTENT_ID).pkg: pkg.gp4
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core pkg_build $< .

pkg.gp4: $(PKG_FILES)
	$(TOOLCHAIN)/bin/$(CDIR)/create-gp4 -out $@ --content-id=$(CONTENT_ID) --files "$^"

sce_sys/param.sfo: Makefile
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_new $@
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

eboot.bin: $(OBJS)
	$(LD) $(OBJS) -o $(BUILD)/vita4.elf $(LDFLAGS)
	$(TOOLCHAIN)/bin/$(CDIR)/create-fself -in=$(BUILD)/vita4.elf -out=$(BUILD)/vita4.oelf \
	  --eboot "eboot.bin" --paid 0x3800000000000011

$(OBJDIR)/%.o: src/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -std=gnu11 -o $@ $<

# The upscaler is the hot loop: vectorized for the PS4's Jaguar cores (btver2,
# AVX), with the single-step reciprocals FSR itself uses (no Newton refinement:
# Jaguar's divider is slow). Checked against VITA5's shaders in `make test`.
UPSCALE_FLAGS := -O3 -ffast-math -mrecip=vec-divf:0,vec-sqrtf:0,divf:0,sqrtf:0
$(OBJDIR)/upscale.o: src/upscale.c src/upscale.h | $(OBJDIR)
	$(CC) $(CFLAGS) $(UPSCALE_FLAGS) -march=btver2 -std=gnu11 -o $@ $<

$(OBJDIR)/%.o: src/%.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(OBJDIR)/shared/%.o: $(ROOT)/core/src/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -std=gnu11 -o $@ $<

$(OBJDIR)/shared/%.o: $(ROOT)/vita-side/input-receiver/src/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -std=gnu11 -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)/shared

# ---- host-side checks ----------------------------------------------------------
HOST_CC     ?= gcc
HOST_CXX    ?= g++
HOST_FLAGS  := -O2 -g -Wall -Wextra -fsanitize=address,undefined $(INCLUDES)
HOST_PORTABLE_C := src/vita_desc.c src/nv12_scale.c src/pad_map.c src/xfer_layout.c src/upscale.c \
                   src/smooth.c src/pacing.c \
                   $(SHARED_C)

test:
	mkdir -p $(BUILD)/host
	for f in $(HOST_PORTABLE_C); do \
	  cc=$(HOST_CC); extra=; \
	  if [ $$f = src/upscale.c ]; then cc=clang; extra="$(UPSCALE_FLAGS) -mavx"; fi; \
	  $$cc -std=c11 $(HOST_FLAGS) $$extra -c $$f -o $(BUILD)/host/$$(basename $$f .c).o || exit 1; done
	$(HOST_CXX) -std=c++17 $(HOST_FLAGS) tests/host_tests.cpp src/canvas.cpp src/settings_io.cpp \
	  src/row_pool.cpp src/upscale_worker.cpp src/log.cpp tests/cpu_host.cpp $(addprefix $(BUILD)/host/,$(notdir $(HOST_PORTABLE_C:.c=.o))) -pthread -o $(BUILD)/host/host_tests
	$(BUILD)/host/host_tests

preview:
	mkdir -p $(BUILD)/host $(BUILD)/preview
	clang -std=c11 -O2 $(INCLUDES) -c src/nv12_scale.c -o $(BUILD)/host/nv12_preview.o
	clang -std=c11 -O3 -ffast-math $(INCLUDES) -c src/upscale.c -o $(BUILD)/host/upscale_preview.o
	clang++ -std=c++17 -O2 -w $(INCLUDES) tests/preview.cpp src/canvas.cpp src/ui.cpp \
	  $(BUILD)/host/nv12_preview.o $(BUILD)/host/upscale_preview.o -o $(BUILD)/host/preview
	$(BUILD)/host/preview $(BUILD)/preview assets/backdrop.png

font:
	python3 tools/make_font.py

art:
	python3 tools/make_art.py

clean:
	rm -rf $(BUILD) eboot.bin pkg.gp4 sce_sys/param.sfo $(CONTENT_ID).pkg
