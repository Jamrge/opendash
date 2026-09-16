# gd3ds build on SDL3
#
#
# The original devkitarm/3DS build lives in Makefile.3ds (untouched).
#
# Requirements:
# sdl3 mpg123 json-c
# curl and zlib are linked from the macOS SDK.
#
# SDL3: pkg-config package is named `sdl3` (verify:
#   PKG_CONFIG_PATH=$(BREW)/lib/pkgconfig pkg-config --modversion sdl3
# -> 3.4.x; include via $(BREW)/include (<SDL3/SDL.h>), link -lSDL3).

BREW := $(shell brew --prefix 2>/dev/null)
ifeq ($(BREW),)
BREW := /opt/homebrew
endif

CC ?= clang

BUILD := build
BIN   := output/gd3ds-sdl

ASSET_DIR   := platform/sdl/assets_build
ASSET_STAMP := $(ASSET_DIR)/.stamp
T3SFILES    := $(shell find gfx -name '*.t3s' 2>/dev/null)
GFXPNG      := $(shell find gfx -name '*.png' 2>/dev/null)

SRCS := $(shell find source libraries platform/sdl -type f -name '*.c')
OBJS := $(patsubst %.c,$(BUILD)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

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
