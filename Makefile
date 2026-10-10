#############################################################################
#
#  Genesis Plus GX - AmigaOS/CaffeineOS Makefile
#  For bebbo's amiga-gcc cross-compiler (GCC 6.5+)
#
#  Target: m68k-amigaos (68030+ / PiStorm/Emu68)
#
#  Compiler: https://github.com/bebbo/amiga-gcc
#            Windows installer: http://franke.ms/download/setup-amiga-gcc.exe
#
#  Usage:
#    make              - Build for 68030 (Emu68 compatible)
#    make CPU=68040    - Build for 68040
#    make CPU=68060    - Build for 68060
#    make CPU=68080    - Build for Apollo/Vampire 68080
#    make clean        - Clean build files
#    make LIBC=clib2   - Use clib2 instead of libnix
#    make USE_ASM_SVP=1   - Also enable assembly-optimized SVP (Virtua Racing)
#    make pi3          - Build for Pi 3 PiStorm (adds LTO for extra whole-program gain)
#
#  Windows: If "/bin/sh not found" errors occur, the shell fix below handles it.
#
#############################################################################

# Windows shell fix - detect and use sh.exe if /bin/sh doesn't exist
ifeq ($(OS),Windows_NT)
    SHELL := sh.exe
endif

# Cross-compiler prefix
PREFIX = m68k-amigaos-

CC = $(PREFIX)gcc
AS = $(PREFIX)as
LD = $(PREFIX)gcc
STRIP = $(PREFIX)strip
OBJCOPY = $(PREFIX)objcopy

# Target executable
TARGET = GenesisPlusGX

# The USE_ZIP block below defines explicit miniz.o rules before 'all:',
# which would otherwise hijack make's default goal (make with no target
# would build only miniz.o and stop).
.DEFAULT_GOAL := all

# CPU type (default 68040)
CPU ?= 68040

# C library choice: nix20 (libnix, default), clib2, or ixemul
LIBC ?= nix20

# Optimization flags for GCC 6.5+
# -O3 is safe with bebbo's GCC
# -fomit-frame-pointer frees a register
# -fno-common required for some AmigaOS code patterns
# -fno-strict-aliasing required for GPGX type-punning through pointers
OPTFLAGS = -O3 -fomit-frame-pointer -fno-common -ffast-math -fno-strict-aliasing -funroll-loops -flto

# C standard - use gnu89 for C89 with GNU extensions (needed for inline, etc.)
STDFLAGS = -std=gnu89

# CPU-specific flags
ifeq ($(CPU),68080)
  # Apollo/Vampire 68080
  CPUFLAGS = -m68080 -mhard-float -mschulz
else ifeq ($(CPU),68060)
  CPUFLAGS = -m68060 -mhard-float -mschulz
else ifeq ($(CPU),68040)
  CPUFLAGS = -m68040 -mhard-float -mschulz
else
  # 68030 - Emu68 emulates the 68882 FPU, so use hard float
  CPUFLAGS = -m68030 -mhard-float -mschulz
endif

# C library selection
ifeq ($(LIBC),clib2)
  LIBCFLAGS = -mcrt=clib2
else ifeq ($(LIBC),ixemul)
  LIBCFLAGS = -mcrt=ixemul
else
  # Default: libnix (noixemul)
  LIBCFLAGS = -noixemul
endif

# Warning flags
WARNFLAGS = -Wall -Wno-unused-variable -Wno-unused-function \
            -Wno-unused-but-set-variable -Wno-sign-compare \
            -Wno-pointer-sign

# Include paths
INCPATHS = -I. -I./amiga -I./core -I./core/z80 -I./core/m68k \
           -I./core/sound -I./core/sound/minimp3 -I./core/input_hw -I./core/cart_hw \
           -I./core/cart_hw/svp -I./core/ntsc

# Defines
# WORDS_BIGENDIAN is the standard way to indicate big-endian
# NOTE: Do NOT define LSB_FIRST - the code uses #ifdef, not #if
# Leaving LSB_FIRST undefined means big-endian (correct for 68k)
DEFINES = -DAMIGA -DWORDS_BIGENDIAN \
          -DBYTE_ORDER=BIG_ENDIAN \
          -DALIGN_LONG -DNO_SCD -DNO_CHD -DNO_YX5200 \
          -DMAXROMSIZE=8388608 \
          -DUSE_16BPP_RENDERING

# Note: MAXROMSIZE=8MB to save memory, increase if needed
# Note: NO_YX5200 disables YX5200 chip (requires minimp3 which is not Amiga-compatible)

# All compiler flags
CFLAGS = $(STDFLAGS) $(CPUFLAGS) $(OPTFLAGS) $(WARNFLAGS) $(INCPATHS) $(DEFINES)

# Linker flags - LIBCFLAGS must come last!
LDFLAGS = $(CPUFLAGS) -flto
LIBS = -lm $(LIBCFLAGS)

#############################################################################
# Source files
#############################################################################

# Core emulation sources
CORE_SRC = \
    core/genesis.c \
    core/io_ctrl.c \
    core/loadrom.c \
    core/mem68k.c \
    core/membnk.c \
    core/memz80.c \
    core/state.c \
    core/system.c \
    core/vdp_ctrl.c \
    core/vdp_render.c

# M68K CPU core
M68K_SRC = \
    core/m68k/m68kcpu.c

# Z80 CPU core
Z80_SRC = \
    core/z80/z80.c

# Sound cores
SOUND_SRC = \
    core/sound/blip_buf.c \
    core/sound/eq.c \
    core/sound/psg.c \
    core/sound/sound.c \
    core/sound/ym2413.c \
    core/sound/ym2612.c

# Input hardware
INPUT_SRC = \
    core/input_hw/activator.c \
    core/input_hw/gamepad.c \
    core/input_hw/graphic_board.c \
    core/input_hw/input.c \
    core/input_hw/lightgun.c \
    core/input_hw/mouse.c \
    core/input_hw/paddle.c \
    core/input_hw/smash.c \
    core/input_hw/sportspad.c \
    core/input_hw/teamplayer.c \
    core/input_hw/terebi_oekaki.c \
    core/input_hw/xe_1ap.c

# Cartridge hardware
CART_SRC = \
    core/cart_hw/areplay.c \
    core/cart_hw/eeprom_93c.c \
    core/cart_hw/eeprom_i2c.c \
    core/cart_hw/eeprom_spi.c \
    core/cart_hw/flash_cfi.c \
    core/cart_hw/ggenie.c \
    core/cart_hw/md_cart.c \
    core/cart_hw/megasd.c \
    core/cart_hw/sms_cart.c \
    core/cart_hw/sram.c \
    core/cart_hw/svp/ssp16.c \
    core/cart_hw/svp/svp.c

# NTSC filter (optional, can be removed to save memory)
NTSC_SRC = \
    core/ntsc/md_ntsc.c \
    core/ntsc/sms_ntsc.c

# Amiga-specific sources
AMIGA_SRC = \
    amiga/main.c \
    amiga/crc32.c \
    amiga/video_cgx.c \
    amiga/audio_paula.c \
    amiga/audio_paula14.c \
    amiga/audio_ahi.c \
    amiga/input_amiga.c

# Assembly sources for optimized audio (enabled with USE_ASM_AUDIO=1)
AMIGA_ASM = \
    amiga/audio_asm.s

# Video assembly (enabled with USE_ASM_VIDEO=1)
VIDEO_ASM = \
    amiga/video_asm.s

# All sources
SOURCES = $(CORE_SRC) $(M68K_SRC) $(Z80_SRC) $(SOUND_SRC) \
          $(INPUT_SRC) $(CART_SRC) $(NTSC_SRC) $(AMIGA_SRC)

# Object files
OBJECTS = $(SOURCES:.c=.o)

# All ASM modules enabled by default (all CPUs in this port are 68020+)
OBJECTS += amiga/audio_asm.o amiga/video_asm.o amiga/vdp_asm.o amiga/svp_drc_asm.o
CFLAGS  += -DUSE_ASM_AUDIO -DUSE_ASM_VIDEO -DUSE_ASM_VDP

# blip_buf inner loop ASM: opt-in for diagnostic purposes.
# Enable with: make USE_ASM_BLIP=1
# This was never present in the old build - enable once confirmed working.
ifdef USE_ASM_BLIP
OBJECTS += amiga/blip_asm.o
CFLAGS  += -DUSE_ASM_BLIP
endif

# Add SVP assembly if USE_ASM_SVP is enabled (opt-in: niche, Virtua Racing only)
ifdef USE_ASM_SVP
OBJECTS += amiga/svp_asm.o
CFLAGS += -DUSE_ASM_SVP
endif

# SVP opcode / program-address histogram in the exit summary (test builds only;
# costs two counter increments per DSP instruction)
ifdef SVP_PROFILE
CFLAGS += -DSVP_PROFILE
endif

# Frame-time profiler (test builds only; Emu68 ONLY - reads Emu68's counter
# with MOVEC, which crashes a real 680x0). Prints a per-section table on exit.
# SVP interpreter: SVP_VERIFY=1 runs the fast one and the original on every
# call and compares the DSP state (test builds; much slower); SVP_REFERENCE=1
# uses only the original interpreter (for speed comparisons)
ifdef SVP_VERIFY
CFLAGS += -DSVP_VERIFY
endif
ifdef SVP_REFERENCE
CFLAGS += -DSVP_REFERENCE
endif

ifdef GPGX_PROFILE
OBJECTS += amiga/prof.o amiga/prof_asm.o
CFLAGS += -DGPGX_PROFILE
endif

# ZIP ROM support (opt-in: make USE_ZIP=1)
# Copy from miniz release: miniz.h miniz_common.h miniz.c
#                          miniz_tinfl.h miniz_tinfl.c miniz_zip.h miniz_zip.c
# Place all seven files in amiga/ then build with: make USE_ZIP=1
ifdef USE_ZIP
OBJECTS    += amiga/miniz.o amiga/miniz_tinfl.o amiga/miniz_zip.o
CFLAGS     += -DUSE_ZIP -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES -DMINIZ_NO_ZLIB_APIS
MINIZ_CFLAGS = $(CPUFLAGS) -O2 -fomit-frame-pointer -fno-strict-aliasing \
               -fno-common $(LIBCFLAGS) -std=gnu99 \
               -DMINIZ_NO_ARCHIVE_WRITING_APIS -DMINIZ_NO_TIME \
               -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES -DMINIZ_NO_ZLIB_APIS \
               -DMZ_FSEEK64=fseek -DMZ_FTELL64=ftell \
               -I./amiga
amiga/miniz.o: amiga/miniz.c amiga/miniz.h amiga/miniz_common.h
	$(CC) $(MINIZ_CFLAGS) -c amiga/miniz.c -o amiga/miniz.o
amiga/miniz_tinfl.o: amiga/miniz_tinfl.c amiga/miniz_tinfl.h amiga/miniz_common.h
	$(CC) $(MINIZ_CFLAGS:-O2=-O0) -c amiga/miniz_tinfl.c -o amiga/miniz_tinfl.o
amiga/miniz_zip.o: amiga/miniz_zip.c amiga/miniz_zip.h amiga/miniz_common.h
	$(CC) $(MINIZ_CFLAGS) -c amiga/miniz_zip.c -o amiga/miniz_zip.o
endif

# PiStorm3D GL output (opt-in: make USE_MINIGL=1)
# Needs the PiStorm3D client SDK (minigl.library 29.1). The binary still runs
# without minigl.library installed: -gl then falls back to the normal display.
# Override the SDK location with MINIGL_SDK=<path>.
ifdef USE_MINIGL
# This Windows-hosted gcc cannot follow the SDK's own "../../../backend"
# includes from a relative or /cygdrive path (verified 2026-10-09), so the
# SDK path is turned into a drive path: /cygdrive/z/x -> z:/x.
_MG_ABS    := $(patsubst /%,%,$(patsubst /cygdrive/%,/%,$(abspath ../PiStorm3D/PiStorm3D/SDK)))
_MG_DRIVE  := $(firstword $(subst /, ,$(_MG_ABS)))
MINIGL_SDK ?= $(_MG_DRIVE):/$(patsubst $(_MG_DRIVE)/%,%,$(_MG_ABS))
OBJECTS    += amiga/video_gl.o
CFLAGS     += -DUSE_MINIGL
LIBS       := -L$(MINIGL_SDK)/lib -lminigl $(LIBS)
amiga/video_gl.o: amiga/video_gl.c amiga/video_gl.h
	$(CC) $(CFLAGS) -I$(MINIGL_SDK)/minigl-shared-library/include -c amiga/video_gl.c -o amiga/video_gl.o
endif

#############################################################################
# Build rules
#############################################################################

LAUNCHER_TARGET = GenesisPlusGX-Launcher

.PHONY: all launcher clean strip

all: $(TARGET)

# MUI 3.8 game launcher + configuration GUI
launcher: $(LAUNCHER_TARGET)

LAUNCHER_OBJ = amiga/gpgx_launcher.o

$(LAUNCHER_TARGET): $(LAUNCHER_OBJ)
	@echo "Linking $(LAUNCHER_TARGET)..."
	$(LD) $(CPUFLAGS) -o $@ $(LAUNCHER_OBJ) -lmui $(LIBCFLAGS)

$(LAUNCHER_OBJ): amiga/gpgx_launcher.c
	@echo "Compiling launcher..."
	$(CC) $(CPUFLAGS) $(STDFLAGS) $(OPTFLAGS) $(WARNFLAGS) \
	      $(INCPATHS) -DAMIGA -DWORDS_BIGENDIAN \
	      -c amiga/gpgx_launcher.c -o $(LAUNCHER_OBJ)



$(TARGET): $(OBJECTS)
	@echo "Linking $(TARGET)..."
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS) $(LIBS)
	@echo "Build complete!"
	@echo "CPU target: $(CPU)"

%.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Assembly rule for .s files
%.o: %.s
	@echo "Assembling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Create stripped version for distribution
strip: $(TARGET)
	$(STRIP) -s $(TARGET) -o $(TARGET).stripped

clean:
	rm -f $(OBJECTS) amiga/svp_asm.o amiga/blip_asm.o amiga/miniz.o amiga/miniz_tinfl.o amiga/miniz_zip.o amiga/video_gl.o amiga/prof.o amiga/prof_asm.o $(TARGET) $(TARGET).stripped $(LAUNCHER_TARGET) $(LAUNCHER_OBJ)
	@echo "Clean complete!"

# Dependency generation (optional, for development)
depend:
	$(CC) $(CFLAGS) -MM $(SOURCES) > .depend

-include .depend

#############################################################################
# Additional targets
#############################################################################

# Build with debug info
debug: CFLAGS += -g -DDEBUG
debug: $(TARGET)

# Build optimized for size
small: OPTFLAGS = -Os -fomit-frame-pointer
small: $(TARGET)

# Pi 3 PiStorm build - same as default but with LTO for extra whole-program optimisation
# Note: Pi 3 is too slow for full-speed emulation with sound.
# Recommended: Pi CM4 or Pi 4 for full speed with sound.
pi3: CFLAGS  += -flto
pi3: LDFLAGS += -flto
pi3: $(TARGET)
	@echo ""
	@echo "Pi 3 build complete."
	@echo "Note: Pi 3 may not achieve full speed with sound enabled."
	@echo "Recommended: Raspberry Pi CM4 or Pi 4 for best results."

# Show configuration
info:
	@echo "Genesis Plus GX Amiga Build Configuration"
	@echo "=========================================="
	@echo "Compiler:   $(CC)"
	@echo "CPU:        $(CPU)"
	@echo "C Library:  $(LIBC)"
	@echo "Sources:    $(words $(SOURCES)) files"

# Test compiler availability
test-compiler:
	$(CC) --version
