# gd3ds build on SDL3 (macOS / Linux / Windows-MinGW)
#
# The original devkitarm/3DS build lives in Makefile.3ds (untouched).
#
# Requirements:
#   macOS:  brew install sdl3 mpg123 json-c   (curl/zlib from the macOS SDK)
#   MSYS2:  pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-SDL3 \
#                  mingw-w64-x86_64-mpg123 mingw-w64-x86_64-json-c \
#                  mingw-w64-x86_64-curl python
#   ()      build from an MSYS2 MinGW64 shell (mingw32-make)
# SDL3's pkg-config package is named `sdl3` on all platforms.

# ------------------------------------------------------------------ #
# Host detection                                                      #
# ------------------------------------------------------------------ #
ifeq ($(OS),Windows_NT)
  HOST := windows
else
  UNAME := $(shell uname 2>/dev/null || echo Unix)
  ifneq (,$(findstring MINGW,$(UNAME))$(findstring MSYS,$(UNAME)))
    HOST := windows      # running inside an MSYS/mingw shell from make
  else
    HOST := unix
  endif
endif

ifeq ($(HOST),windows)
  # MinGW-w64: pkg-config finds sdl3/mpg123/json-c/libcurl/zlib pc files
  # in /mingw64/lib/pkgconfig (MSYS2 provides it on PATH already).
  BUILDPREFIX := /mingw64
  CC ?= gcc
  EXEEXT := .exe
  # network.c includes <sys/socket.h>/<netinet/in.h>/<arpa/inet.h> which
  # MinGW does not ship; wincompat provides harmless stubs (all real
  # networking goes through curl).
  CFLAGS := -std=gnu11 -O2 -g -Wall \
	-Iplatform/sdl \
	-Iplatform/sdl/wincompat \
	-Isource \
	-Isource/utils \
	-Ilibraries \
	-I$(BUILDPREFIX)/include \
	-MMD -MP
  LIBS := -L$(BUILDPREFIX)/lib -lSDL3 -lmpg123 -ljson-c -lcurl -lz -lm
else
  # macOS default: Homebrew (zlib/curl come from the macOS SDK, no .pc)
  BREW := $(shell brew --prefix 2>/dev/null)
  ifeq ($(BREW),)
    BREW := /opt/homebrew
  endif
  CC ?= clang
  EXEEXT :=
  CFLAGS := -std=gnu11 -O2 -g -Wall \
	-Iplatform/sdl \
	-Isource \
	-Isource/utils \
	-Ilibraries \
	-I$(BREW)/include \
	-I$(BREW)/opt/json-c/include \
	-I$(BREW)/opt/mpg123/include \
	-MMD -MP
  LIBS := -L$(BREW)/lib \
	-L$(BREW)/opt/mpg123/lib \
	-L$(BREW)/opt/json-c/lib \
	-lSDL3 -lmpg123 -ljson-c -lcurl -lz -lm
endif

BUILD := build
BIN   := output/gd3ds-sdl$(EXEEXT)

ASSET_DIR   := platform/sdl/assets_build
ASSET_STAMP := $(ASSET_DIR)/.stamp
T3SFILES    := $(shell find gfx -name '*.t3s' 2>/dev/null)
GFXPNG      := $(shell find gfx -name '*.png' 2>/dev/null)

SRCS := $(shell find source libraries platform/sdl -type f -name '*.c' 2>/dev/null)
OBJS := $(patsubst %.c,$(BUILD)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# comunes (no se usan dentro por ahora pero quedan por consistencia):
# SRCS/OBJS/DEPS se arman igual en ambos hosts.

.PHONY: all clean

all: $(BIN)

# assets: pack .t3s + PNG source atlases before linking (incremental)
$(ASSET_STAMP): $(T3SFILES) $(GFXPNG) platform/sdl/tools/t3s_to_atlas.py
	@mkdir -p $(ASSET_DIR)
	python3 platform/sdl/tools/t3s_to_atlas.py
	@touch $(ASSET_STAMP)

$(BIN): $(OBJS) $(ASSET_STAMP)
	@mkdir -p $(@D)
	$(CC) $(OBJS) -o $@ $(LIBS)

$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

-include $(DEPS)

clean:
	rm -rf $(BUILD) $(ASSET_DIR)
