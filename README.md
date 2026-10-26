# Genesis Plus GX - AmigaOS/CaffeineOS Port

A port of the Genesis Plus GX emulator for AmigaOS 3.x systems, specifically targeting PiStorm/Emu68 accelerated Amigas running CaffeineOS.

## Features

- Sega Genesis / Mega Drive emulation at full 60fps (NTSC) / 50fps (PAL)
- Virtua Racing (SVP DSP) at or near full speed on PiStorm/Emu68: the DSP
  program is recompiled to 68k code at run time (`-nodrc` uses the
  interpreter instead)
- Master System and Game Gear emulation
- ZIP ROM loading (archives extracted in memory via miniz)
- 32-bit BGRA CyberGraphX/P96 RTG video output (VideoCore native format)
- Optional 16-bit RTG output (`screendepth=16`) - half the blit bandwidth,
  direct RGB565 row copy on native screens; ideal for classic Zorro RTG cards
- Optional 2x scaling with configurable scanline effects
- VSync support for reduced tearing
- Three audio backends: 8-bit Paula (mono/stereo), 14-bit Paula stereo, AHI
- Paula stereo output with true left/right channel separation
- Post-blip volume boost for louder 8-bit output without resampler distortion
- 14-bit Paula calibration support (auto-loads from ENV:CyberSound/SoundDrivers/14Bit_Calibration)
- Configurable sample rates for all audio backends
- Configurable synthesis quality (`hqfm`/`hqpsg`). Measured on a Pi 3 (20.10.2026):
  `hqfm=0` saves only ~0.2 ms of a 24 ms frame and `hqpsg=0` nothing, because the
  YM2612 synthesis itself (5.9 ms) is unchanged; they are not a rescue for slow machines
- **Non-blocking Paula audio** with quad-buffer ring
- Non-blocking AHI audio with ring buffer (zero-drop at 60fps)
- Keyboard and CD32 gamepad input via lowlevel.library
- 68k assembly optimized video blitting, audio conversion, and resampling
- Save states (F5 to save, F8 to load) with on-screen feedback
- Automatic SRAM battery backup (saves on exit, loads on start)
- Cheat code support via RetroArch `.cht` files (Game Genie + GameShark/AR formats)
- Configuration file (`PROGDIR:GenesisPlusGX.cfg`, auto-created)
- FPS counter overlay

## Requirements

### Target System
- AmigaOS 3.0 or higher (Intuition and graphics.library V39)
- AmigaOS 3.1 or higher for CD32 gamepads, which need lowlevel.library V40.
  Without it the emulator still runs, with keyboard input only.
- 68030 CPU minimum (PiStorm/Emu68 recommended)
- 8MB+ Fast RAM
- CyberGraphX or Picasso96 RTG system
- Paula chip (for 8-bit/14-bit audio) or AHI (for sound card audio)
- MUI 3.8 or higher for the launcher only (muimaster.library 19)

### Minimum Hardware
- **Raspberry Pi CM4 or Pi 4** with PiStorm/PiStorm32 (recommended)
- Amiga 500/2000 with PiStorm + Pi 3 series: video only (no sound),
  the Pi 3 is too slow to emulate the Genesis sound chips at full speed.

### Tested Configurations
- Amiga 1200 + PiStorm32-Lite + CM4 (Emu68) - **full speed with sound**
  (all 40 ROMs of the test drawer, 20.10.2026)
- Amiga 500 + PiStorm + Pi 3A+ (Emu68) - about 11% too slow with sound:
  Sonic 3 needs 18.5 ms per 16.7 ms frame, 35% of frames drawn with audio
  dry-outs (90 s runs, 20.10.2026); use -nosound for video only
- Amiga 2000 + PiStorm + Emu68 (Pi 3 class, model not checked) - Sonic 3
  17.6 ms per frame, 37% of frames drawn, picture correct (20.10.2026)

### Build Requirements
- m68k-amigaos-gcc 6.5.0b. The release binaries are built with the AmigaPorts
  amiga6 build (https://github.com/AmigaPorts/amiga-gcc); bebbo's amiga-gcc
  (https://github.com/bebbo/amiga-gcc) also works. Set `TOOLCHAIN=` on the
  make line or edit the Makefile to point at the install.

## Building

### Quick Start (Windows CMD)

```cmd
build.bat              &:: Build for 68030 + all ASM optimizations (recommended)
build.bat 68040        &:: Build for 68040 with FPU
build.bat pi3          &:: Build for Pi 3 PiStorm (68030)
build.bat launcher     &:: Build MUI launcher only
build.bat clean        &:: Clean build files
```

`build.bat` always passes `USE_ASM_AUDIO=1 USE_ASM_VIDEO=1 USE_ASM_BLIP=1 USE_ZIP=1`.

### Full Build with ASM Optimizations (Recommended)

```bash
make clean
make USE_ASM_AUDIO=1 USE_ASM_VIDEO=1 USE_ASM_BLIP=1 USE_ZIP=1
```

> **Note:** Incremental builds must repeat the same flag set - `make` cannot
> detect `-D` flag changes in existing object files.

### Build Options

| Option | Description |
|--------|-------------|
| `CPU=68030` | Default, Emu68 compatible |
| `CPU=68040` | 68040 with hardware FPU |
| `CPU=68060` | 68060 with hardware FPU |
| `CPU=68080` | Apollo/Vampire 68080 |
| `USE_ASM_AUDIO=1` | Assembly-optimized Paula audio conversion |
| `USE_ASM_VIDEO=1` | Assembly-optimized RGB565-to-RGBA blitters |
| `USE_ASM_BLIP=1` | Assembly-optimized blip_buf resampling |
| `USE_ZIP=1` | ZIP ROM support (miniz, in-memory extraction) |
| `USE_MINIGL=1` | PiStorm3D GPU output (`-gl`). `build.bat` adds it automatically when `../PiStorm3D/PiStorm3D/SDK` exists; `build.bat nogl` leaves it out |
| `LIBC=nix20` | libnix runtime (default) |
| `LIBC=clib2` | clib2 runtime |
| `debug` | Build with debug symbols |
| `small` | Size-optimized build |

### Linux / macOS

```bash
git clone https://github.com/bebbo/amiga-gcc
cd amiga-gcc && make update && sudo make all PREFIX=/opt/amiga
export PATH=/opt/amiga/bin:$PATH

cd /path/to/gpgx-amiga
make USE_ASM_AUDIO=1 USE_ASM_VIDEO=1 USE_ASM_BLIP=1 USE_ZIP=1
```

## Usage

```
GenesisPlusGX <romfile> [options]
```

Supported ROM formats: `.bin`, `.gen`, `.md`, `.smd`, `.sms`, `.gg`, `.zip`
(ZIP archives are extracted in memory; the first file with a known ROM
extension inside the archive is loaded)

### Examples

```bash
# Default: 8-bit Paula, 1x scale
GenesisPlusGX rom.bin

# 14-bit stereo audio with scanlines and FPS counter
GenesisPlusGX rom.bin -14bit -scanlines -fps

# AHI audio (for systems without Paula)
GenesisPlusGX rom.bin -ahi -fps

# Maximum quality: 14-bit audio, strong scanlines, vsync
GenesisPlusGX rom.bin -14bit -scanlines=3 -vsync -fps

GenesisPlusGX rom.bin -paulafreq=22050 -fps
```

### Command-Line Options

#### Video Options
| Option | Description |
|--------|-------------|
| `-scanlines` | Enable 2x scanlines (medium intensity) |
| `-scanlines=N` | Scanline level 1-3 (1=subtle, 2=medium, 3=strong) |
| `-vsync` | Enable VSync via WaitTOF (reduces tearing, may cap at 30fps if too slow) |
| `-screendepth=N` | RTG screen depth: 32 (default) or 16 (half the blit bandwidth, for Zorro RTG cards) |

#### GPU Output (PiStorm3D, optional)

Builds made with the PiStorm3D SDK can draw the picture through the
Raspberry Pi 4 / CM4's V3D GPU using `minigl.library`: scaling to any
screen size, scanlines and a composite blend that turns dither patterns
into the transparency games expected from a TV. Off by default; enable
it in the launcher (Video tab) or with `-gl`.

| Option | Description |
|--------|-------------|
| `-gl` | Draw through the GPU (falls back to the normal display if unavailable) |
| `-nogl` | Normal display even if `glout=1` in the config |
| `-gleffect=N` | 0=sharp, 1=smooth (default), 2=scanlines, 3=composite blend |
| `-glsize=WxH` | GPU output screen size (default 640x480; the nearest RTG mode is used) |
| `-glrgba` | Upload RGBA8 instead of RGB565 (only if colours look wrong) |

Requirements (from the PiStorm3D FAQ): Pi 4 / CM4 / 4B, `minigl.library`
**29.1** in `LIBS:` (a 29.0 library refuses 29.1 clients silently), all
Picasso96 libraries from the same version, BGRA32 as the only enabled
32-bit screen modes, and PiStorm's `fast_page_zero` option off. GPU output
forces VSync off and a 32-bit screen.

**It is not faster.** The emulation is unchanged, and handing each frame
to the GPU costs about 6 ms of CPU per drawn frame (measured on a CM4 with
minigl.library 29.1: ~5.4 ms texture upload, ~1.7 ms draw at 640x480)
against ~0.8 ms for the normal display. Larger output sizes add draw time
(~4.9 ms at 1280x720 with the composite effect). Keep `autoskip=1` on.
The exit summary prints the measured `GL: upload` / `draw+present` times.

#### Audio Options
| Option | Description |
|--------|-------------|
| `-14bit` | 14-bit Paula stereo (4 channels, higher quality) |
| `-paulafreq=N` | Paula sample rate in Hz (default: 41243, up to 44100; 27710 is used automatically on a 15 kHz native display) |
| `-ahi` | Use AHI audio system (for sound cards, systems without Paula) |
| `-ahifreq=N` | AHI sample rate: 22050, 27710, 32000, 44100 (default: 44100) |
| `-paulafilter` | Force the Amiga audio (LED) filter on (~7 kHz low-pass) |
| `-nosound` | Disable audio entirely (maximum speed) |

Additional settings available in config file (and launcher GUI) only: `fmvol`, `psgvol`, `paulaboost`, `paulastereo`, `ahiboost`, `hqfm`, `hqpsg`, `glout`, `gleffect`, `glsize` (see Configuration section).

#### Performance Options
| Option | Description |
|--------|-------------|
| `-frameskip=N` | Skip N frames (1=50%, 2=33%, 3=25% render) |
| `-fps` | Show FPS counter in top-right corner |
| `-nodrc` | Virtua Racing (SVP): use the DSP interpreter instead of the recompiler |

### Audio Backend Guide

**8-bit Paula (default)** -- Hardware DMA, near-zero CPU usage for playback. Best for slow setups. Supports mono or stereo output (`paulastereo=1`). With stereo enabled, uses all 4 Paula channels (0+3 left, 1+2 right) for true left/right separation. Volume boost (`paulaboost`) amplifies the clean signal after resampling for louder output.

**14-bit Paula** (`-14bit`) -- Uses 4 DMA channels to achieve 14-bit stereo. Better audio quality, slightly more DMA bandwidth. On A500 with heavy DMA contention, consider lowering frequency with `-paulafreq=22050`.

**AHI** (`-ahi`) -- Software audio through the AHI retargetable audio system. Required for setups without Paula (e.g., some Amithlon/WinUAE configurations). Non-blocking ring buffer implementation achieves zero audio drops at 60fps. Default 44100Hz for best quality.

### Recommended Paula Frequencies

| Frequency | Period | Samples/frame | Notes |
|-----------|--------|---------------|-------|
| 44100 Hz | 80 | ~735 | CD quality, RTG only (no bitplane DMA) |
| 41243 Hz | 86 | ~687 | **Default**, near-CD, RTG only |
| 27710 Hz | 128 | ~462 | Optimal DMA:DAC ratio, all display modes |
| 22050 Hz | 162 | ~368 | Good quality, less CPU |
| 16000 Hz | ~222 | ~267 | Acceptable, for very slow systems |
| 11025 Hz | ~322 | ~184 | Low quality, minimum usable |

Frequencies above ~28600 Hz (period < 124) require RTG-only display modes where Paula has no bitplane DMA contention on the chip bus. All PiStorm/Emu68 setups with RTG screens qualify.

## Controls

### Player 1 (Keyboard)
| Key | Genesis Button | SMS/GG Button |
|-----|----------------|---------------|
| Arrow Keys | D-Pad | D-Pad |
| 1 | A | Button 1 |
| 2 | B | Button 2 |
| 3 | C | - |
| Q | X (6-button) | - |
| W | Y (6-button) | - |
| E | Z (6-button) | - |
| Space / Enter | Start | Start (GG only) |
| Escape | Quit | Quit |

> **Note:** For Master System and Game Gear ROMs, only Button 1 and Button 2 are active. The 6-button keys (Q/W/E) and C are unused.

### Player 2 (Numpad)
| Key | Genesis Button |
|-----|----------------|
| Numpad 8/2/4/6 | D-Pad |
| Numpad 0 | A |
| Numpad . | B |
| Numpad Enter | C |
| Help | Start |

A second joystick/CD32 pad in the mouse port also controls player 2.

### CD32 Gamepad (via lowlevel.library)
| CD32 Button | Genesis Button | SMS/GG Button |
|-------------|----------------|---------------|
| D-Pad | D-Pad | D-Pad |
| Blue | A | Button 1 |
| Red | B | Button 2 |
| Green | C | - |
| Yellow | Y | - |
| Play/Forward | Start | Start (GG only) |
| Reverse | Mode | - |

### Hotkeys
| Key | Action |
|-----|--------|
| F5 | Save state |
| F8 | Load state |
| Escape | Quit |

## Cheat Codes

Cheats are loaded automatically from `PROGDIR:Cheats/<romname>.cht` when a ROM is started. Create a `Cheats/` directory next to the emulator executable and copy RetroArch-compatible `.cht` files there.

### File Format

RetroArch `.cht` files are plain text with numbered entries:

```
cheat0_desc = "Infinite Lives"
cheat0_code = "009A64:0009"
cheat0_enable = false

cheat1_desc = "Max Money"
cheat1_code = "ARFA-AA6C+ARFA-AA6T"
cheat1_enable = false
```

Codes are separated by `+` when multiple patches make up one cheat.

### Enabling Cheats

All cheats are **disabled by default** (`enable = false`). To activate a cheat, open the `.cht` file in a text editor and change the desired entry to:

```
cheatN_enable = true
```

Only entries with `enable = true` are applied when the ROM loads.

### Supported Code Formats

| Format | Example | Notes |
|--------|---------|-------|
| Game Genie | `ARFA-AA6C` | 8-character GG alphabet code |
| GameShark / Action Replay | `009A64:0009` | `AAAAAA:DDDD` hex address:data |

Codes are applied as:
- **ROM patches**: even addresses within ROM range - written once at load time. The header checksum ($18E) is recomputed afterwards, so games that verify their ROM at start (red screen otherwise, e.g. Daimakaimura) start without their MASTER CODE; enable the master code too if a game still refuses
- **RAM patches**: addresses in Genesis work RAM range ($E00000+) - reapplied every frame since games reset RAM values each frame
- **Odd addresses**: skipped and reported on the Shell

### Finding Cheat Files

RetroArch cheat repositories contain thousands of `.cht` files ready to use. The filename must match the ROM filename without extension (e.g. `Sonic.cht` for `Sonic.bin`).

## Save States & SRAM

All save data is stored in the `States/` directory next to the emulator executable.

### Save States (F5/F8)
Press **F5** to save and **F8** to load. A brief on-screen message confirms the action.

Save state files use the `.gsx` extension:
- `PROGDIR:States/Sonic.gsx`

States are compatible with the GPGX 1.7.6 format and capture the complete emulator state (CPU registers, RAM, VRAM, sound chips, mapper state).

### Battery Backup (SRAM)
Games with battery-backed save RAM (RPGs, sports games with season modes, etc.) are automatically saved to a `.srm` file when you quit, and reloaded on next launch:
- `PROGDIR:States/Shining_Force.srm`

SRAM is also saved whenever you save a state (F5), providing an extra safety net. The file is only written when the SRAM contents have actually changed.

## Configuration

Settings are stored in `PROGDIR:GenesisPlusGX.cfg`, which is auto-created on first run. Edit it with any text editor. Command line arguments always override config file settings.

```
; GenesisPlusGX config
# Audio backend: paula, 14bit, ahi, off
audio=paula

# Paula sample rate in Hz (default: 41243, up to 44100 in RTG)
paulafreq=41243

# AHI sample rate: 22050, 27710, 32000, 44100 (default: 44100)
ahifreq=44100

# FM synth volume: 10-100 (default: 60)
fmvol=60

# PSG volume: 10-200 (default: 60)
psgvol=60

# Amiga audio (LED) filter, ~7 kHz low-pass: 1=force on, 0=leave as the system has it
paulafilter=0

# Paula volume boost: 0=1x, 1=2x (default), 2=4x, 3=8x
paulaboost=1

# Paula stereo: 0=mono, 1=stereo (default, uses 4 DMA channels)
paulastereo=1

# AHI volume boost: 0=1x (default), 1=2x, 2=4x, 3=8x
ahiboost=0

# paulamargin / ahimargin: obsolete and ignored. Audio buffers now hold
# exactly one frame and the queue depth is rate-controlled.

# Synthesis quality: 1=band-limited (default), 0=linear interpolation
# (hqfm=0 saves about 1% of the frame on a Pi 3; measured 20.10.2026)
hqfm=1
hqpsg=1

# Screen depth: 32 (default, PiStorm/VideoCore fast path) or 16
#   16 = RGB565 screen, half the blit bandwidth - recommended for classic
#   Amigas with Zorro RTG cards. Falls back to 32 if no 16-bit mode exists.
screendepth=32

# GPU output via PiStorm3D minigl.library 29.1 (Pi 4/CM4, GL builds only)
#   glout:    1 = on (falls back to the normal display if unavailable)
#   gleffect: 0=sharp 1=smooth 2=scanlines 3=composite blend
#   glsize:   output screen size, e.g. 640x480 1280x720 1920x1080
glout=0
gleffect=1
glsize=640x480

# Scanlines: 0=off (default), 1=subtle, 2=medium, 3=strong
scanlines=0

# VSync: 0=off (default), 1=on (may drop to 30fps)
vsync=0

# Frame skip: 0=none (default), 1-5=skip N frames
frameskip=0

# Auto frame skip: 0=off, 1=on (default)
autoskip=1

# FPS counter: 0=off, 1=on (default)
fps=1
```

## Assembly Optimizations

The port includes hand-written 68k assembly routines for the performance-critical paths. Enable them at build time with the `USE_ASM_*` flags.

### Video (`USE_ASM_VIDEO=1`)
- `Blit565toRGBA_asm` -- 1x RGB565 to RGBA32 via 64K LUT
- `Blit565toRGBA_2x_asm` -- 2x scaled blit (no scanlines)
- `Blit565toRGBA_2x_scanline_asm` -- 2x with dual-LUT scanline darkening (zero per-pixel math)

### Audio (`USE_ASM_AUDIO=1`)
- `ConvertMono8_asm` -- 16-bit stereo to 8-bit mono with volume boost, 4x unrolled
- `ConvertStereo8_asm` -- 16-bit stereo to separate 8-bit L/R with volume boost, 4x unrolled
- `Convert14Bit_Table_asm` -- 16-bit stereo to 14-bit via calibrated lookup table (Buchner/Richter method)

### Blip Buffer (`USE_ASM_BLIP=1`)
- `blip_read_samples_inner_asm` -- Stereo integrator with clamping and high-pass filter, register-optimized loop

The scanline blitter uses a pre-computed darkened LUT (built at init based on scanline level) so the ASM inner loop does two LUT lookups per pixel instead of per-pixel RGB extraction and math. This benefits the C fallback path too.

## Performance Notes

### PiStorm Speed Tiers
- **Pi Zero 2W / Pi 3 series**: Too slow for full-speed emulation with sound. Use `-nosound` for playable video-only mode.
- **Pi CM4 / Pi 4** (1.5+ GHz): Full speed with sound. Recommended.

### A500 vs A1200 Considerations
The A500's OCS/ECS chipset has a 16-bit shared chip RAM bus where CPU and DMA alternate cycles. The A1200's AGA has a 32-bit bus running at double clock speed, giving 4x chip RAM bandwidth. This means:
- On A500 PiStorm: Paula DMA and RTG framebuffer writes compete for bus time
- On A1200 PiStorm32: Much more bandwidth available, less contention

### VSync Limitation
`-vsync` uses `WaitTOF()` which may not perfectly sync with the VideoCore HDMI scanout on Emu68 RTG. Some tearing may persist. This is a known PiStorm RTG stack limitation.

## Technical Notes

### Memory Usage
- ROM buffer: Up to 8MB (configurable via MAXROMSIZE)
- RGB565 to RGBA LUT: 256KB (+ 256KB for scanline dark LUT if enabled)
- VRAM/Work RAM: ~136KB
- Sound buffers: ~32KB
- 14-bit calibration tables: 256KB (two 128KB lookup tables, Fast RAM)
- Video buffer: ~150KB (640 bytes/line x 240 lines)
- AHI ring buffers: ~12KB (4 x 3KB)

Total: ~9MB minimum recommended

### Endianness
The 68k is big-endian, matching the emulated Genesis hardware. `WORDS_BIGENDIAN` is defined at build time. Some code paths are more efficient due to this native byte order match.

### C Standard
The codebase uses C89/gnu89 (`-std=gnu89`) as required for AmigaOS idiomatic code with bebbo's amiga-gcc 6.5.0b.

### Audio Frequency Accuracy

The emulated Genesis audio runs within spec. The relevant clock chain:

- **Paula clock**: PAL Amiga 3,546,895 Hz / period 86 = **41,242.97 Hz** actual output
- **Config value** (`paulafreq=41243`): integer truncation gives 41,242 Hz -> **0.0024% error**
- **blip_buf resampling**: Genesis master clock 53,693,175 Hz resampled to 41,242 Hz; the internal fixed-point ratio is ceiling-rounded -> **+0.0013% additional pitch shift**
- **Total pitch error**: < **0.003%** - well within spec, absorbed by the audio rate control

PSG tone accuracy follows Genesis hardware: the PSG runs from the same master clock and its tone divisors are integer, so pitch errors are hardware-accurate.

### Compatibility
The port uses standard AmigaOS system calls (Intuition, CyberGraphX, AHI, timer.device, lowlevel.library) and should run on any 68k AmigaOS-compatible system with RTG, including Amithlon and WinUAE with appropriate drivers.

## TODO / Roadmap

- [x] Save states (F5/F8)
- [x] SRAM battery backup persistence (auto-save/load)
- [x] Configuration file support with version tracking
- [x] Double-buffered screens for VSync
- [x] 14-bit Paula calibration (Buchner/Richter, loads ENV:CyberSound/SoundDrivers/14Bit_Calibration)
- [x] blip_read_samples ASM (Genesis FM+PSG resampling path)
- [x] Paula stereo output (separate L/R 8-bit DMA channels)
- [x] Post-blip volume boost (paulaboost/ahiboost)
- [x] Configurable button mapping (per-player key bindings in config)
- [x] Non-blocking Paula audio with quad-buffer ring
- [x] MUI launcher GUI with ROM browser and full config editor (`build.bat launcher`)
- [x] EClock hybrid frame timer (sub-millisecond accuracy, eliminates PiStorm timer jitter)
- [x] Per-section profiling (input/emu/audio/video us breakdown, printed every 300 frames)
- [ ] LTO (removed 20.10.2026: lto-wrapper does not work on the build host; no measurable speed change on a CM4)
- [ ] blip_mix_samples ASM (for future Sega CD support, currently NO_SCD)
- [x] Cheat code support via RetroArch `.cht` files (Game Genie + GameShark/AR, ROM + RAM patches)
- [ ] Screenshot support
- [ ] Sega CD support

## Changelog

### v1.11 (25.10.2026)

Rebuild with the fixed amiga6 gcc 6.5.0b (`D:/amiga-gcc-amiga6`, toolchain
`AmigaDev eac7a11`, AmigaPorts gcc 878d2cfb8 with PRs 125 and 128). No change
to the emulation code.

**Wrong-bit tests fixed:** the gcc 6.5.0b that built v1.10 turned a C test
like `if (data & 1)` on a value kept in a stack slot into `btst #-24,(4,sp)`.
A `btst` on memory takes its bit number modulo 8, so bit 24 was tested
instead of bit 0. The data of a byte or word write never has bit 24 set, so
these tests always took the "bit clear" path. Two of them could be reached
in v1.10, each by one cartridge (`core/cart_hw/md_cart.c`):
- `mapper_seganet_w()`, used by Game no Kanzume Otokuyou: the cartridge's
  ROM write protection was never switched on.
- `tekken_regs_w()`, used by Tekken 3 Special (pirate): the protection
  register never took a 1 written to its data input.

Neither cartridge was at hand, so `amiga/test/maptest/` builds two 64 KB test
ROMs of our own code that match these database entries and check each
mapper's effect (green or red screen): both are red with v1.10 and green with
v1.11 on an A1200 with a PiStorm (Emu68, CM4). The other two sites could not be reached:
`gen_bankswitch_w()` (`core/genesis.c`, boot ROM to cartridge switch) only
runs with a Mega Drive boot ROM, which this port never loads (`config.bios`
is 0), and miniz's `mz_zip_validate_file()` is never called. The 1.11 binary
has no negative `btst` left (`objdump -d -m m68k:68040`).

**Build:** the new install's libc headers no longer include `<sys/types.h>`
from `<stdio.h>`/`<stdlib.h>`. It then arrived through `miniz.h`'s
`<time.h>` after Musashi's `#define uint unsigned int`, and its `typedef
unsigned int uint;` failed to compile. `amiga/main.c` now includes
`<sys/types.h>` before `shared.h`, the order the old headers gave.
`-fno-peephole2` stays on every compile line.

**Launcher 1.7:** rebuilt with the same compiler; About text shows v1.11.

**Tested** on an A1200 with a PiStorm (Emu68, CM4), CaffeineOS: all 40
ROMs of `amiga/test/allgames.py` start with a correct picture, 0 audio
drops, 99.9-100% of frames drawn (Virtua Racing 75%, as before;
`amiga/test/allgames_2026-10-25.txt`). Cheats: Daimakaimura with
"Untouchable" gives the same header checksum fix as 1.10 (`$7254 ->
$5ABF`) and runs; two GameShark codes on Ms. Pac-Man are both read
(`Cheats: 2 active (2 RAM)`), which needs the patched libnix `sscanf()` of
the new install: the same objects linked with the original `__srget.o` (in
`libm020/libnix.a`, the variant this build links) report `unknown format`
for both codes. Launcher 1.7 opens under MUI 3.8 and shows v1.11.

### v1.10 (21.10.2026)

**Cheats:**
- A ROM patch from a `.cht` file changes the sum that many games check at
  start-up (the standard Sega routine adds the ROM words from `$200` to the
  header's ROM end address and compares the result with the header checksum
  at `$18E`). After the cheat file is applied the sum is recomputed and
  written into the header, so such games run without their Game Genie master
  code. Daimakaimura (Japan) with only "Untouchable" enabled stopped on a red
  screen before; now it starts (the Shell reports `$7254 -> $5ABF`).
- Codes with odd addresses are reported on the Shell instead of silently
  skipped.

**Slow machines (A500 / Pi 3 class):**
- Paula cannot fetch samples below period 124 (about 28 kHz) on a 15 kHz
  native display (HRM: one audio DMA slot per scan line). The emulator counts
  the native frame's lines with `VBeamPos()` before the audio init and uses
  27710 Hz (`paulafreq=0`) below 400 lines, reported on the Shell. On an
  A500 + Pi 3 the default 41243 Hz dropped 627 audio frames in 90 s; now none.
- Autoskip skips at most 2 frames in a row (`AUTOSKIP_MAX_RUN`, was 4).
  A500 + Pi 3, Sonic 3, 90 s: 4 gave 22% of frames drawn and 2060 audio
  dry-outs, 2 gives 35% and 2746, 1 gave 51% and 3688.

**Build:**
- Compiled with the AmigaPorts amiga6 gcc 6.5.0b (`D:/amiga-gcc-amiga6`,
  named by absolute path in the Makefile), without `-flto` (`lto-wrapper`
  fails on this host) and with `-fno-peephole2` on every compile line: both
  gcc 6.5.0b builds have a wrong-code peephole2 pass (bebbo's merges a 16-bit
  and a following 32-bit zero store into one `clr.l`; the amiga6 build drops
  the `ext.l` of a `short` array index). Measured on the CM4 (Virtua Racing
  attract mode, equal 66 s runs): 14155 vs 14136 us emulation per frame, no
  change.
- Launcher 1.6: opens muimaster.library with version 19 (the amiga6 `mui.h`
  asks for 20, which MUI 3.8 refuses). `build.bat pi3` passes the same
  ZIP/ASM/GL flags as the normal build (it built without ZIP support before).

**Docs corrected:** `hqfm=0` saves about 1% of a Pi 3 frame (the FM-to-blip
flush is 0.4 ms of 24 ms) and `hqpsg=0` nothing, measured with the new profile
sections; the earlier claims were wrong. Release build on the A500 + Pi 3:
18.4-18.6 ms emulation per 16.7 ms frame (Sonic 3, 90 s runs).

**Test builds and tests:** profile sections for the audio path (blip read,
8/14-bit conversion, Chip RAM copy, device I/O), PSG synthesis and the FM
flush; `amiga/bench/beamtest` prints the native scan rate; `amiga/test/
allgames.py` runs every ROM of the Amiga's drawer through the installed binary
and keeps the exit summaries (40 ROMs, all OK on the A1200 + CM4, 20.10.2026).

### v1.9 (11.10.2026)

**Speed:**
- The video blitters and the VDP helper loops (`amiga/video_asm.s`,
  `amiga/vdp_asm.s`) count with `subq`/`bne` instead of `dbf`. With
  Emu68's "slow DBF" option on, every DBcc ends a JIT translation unit;
  those sections then took 15-35% longer per instruction (CM4). Now the
  option makes no difference.
- Virtua Racing: the DSP recompiler translates unconditional calls to
  short leaf routines into the calling block (verified against the
  interpreter, 0 differences; no measurable speed change).

**Measured (CM4, four games, no GPU output):** per frame the 68000 takes
3.2-4.0 ms, FM synthesis 2.6-3.2 ms, VDP rendering 2.0-2.7 ms and the Z80
1.9-2.1 ms. GPU output costs about 6 ms per drawn frame: the same games drew
52-63% of their frames with it, 91-100% without.

**Test builds:** the profile build (`GPGX_PROFILE=1`) also counts emulated
68000/Z80 instructions and FM samples, prints an interval line every 300
frames (including Emu68 JIT translations, units and free memory), and has
`-profsvp`, `-jitdepth=N` and `-flushtest`. `make EXTRA_DEFS=...` passes
extra defines. `amiga/bench/` holds two Emu68 test programs (dispatch cost,
instruction checks).

### v1.8 (10.10.2026)

**Virtua Racing:**
- The SVP chip's SSP1601 DSP program is recompiled into 68k code at run
  time (Emu68 only; elsewhere the interpreter below runs). Measured on a
  CM4 in a race: DSP time 13.5 -> 5.4 ms per frame, frame emulation
  22 -> 15 ms; the race now runs at 57-60 fps (about 33 before).
- The code is laid out for the Emu68 JIT: blocks chain through a
  generated dispatcher, flags are computed without branches, rare paths
  sit after each block, and all memory-port (PM/PMC) accesses are
  generated inline. IRAM overlays (reloaded ~1.6 times per frame) are
  validated by content.
- A faster DSP interpreter (state in registers) is used when the
  recompiler is off (`-nodrc`, or not on Emu68).
- Every step was checked against the original interpreter with a
  verify build that compares the full DSP state block by block
  (millions of blocks, 0 differences).

**Other:**
- The startup line names the screen pixel format (e.g. `RGB16PC`).
- Test builds: `GPGX_PROFILE=1` (frame profile from Emu68's counters),
  `SVP_VERIFY=1`; see CLAUDE.md.

### v1.7 (09.10.2026)

**Audio:**
- Frames are paced against an absolute EClock schedule instead of a
  relative delay per frame, so timer overshoot no longer accumulates
- Audio buffers hold exactly one frame (odd-sample carry keeps the
  length even) and the queue depth is rate-controlled around two
  buffers; `audio_init` runs at exactly 60/50 Hz. No more gaps from
  timer jitter. `paulamargin` / `ahimargin` are obsolete and ignored
- 14-bit Paula: all four channels start in sync (CMD_STOP / CMD_START),
  sign-symmetric HI/LO split, table-driven ASM converter
- Exit summary: queue-depth histogram, dropped / ran-dry counts, rate
  control, and a hint when Paula plays slower than its nominal rate

**Video:**
- Optional GPU output through PiStorm3D `minigl.library` 29.1 (Pi 4 /
  CM4): any screen size, sharp / smooth / scanlines / composite blend.
  CaffeineOS includes the library. Better picture, not faster (about
  6 ms of CPU per frame)
- Autoskip now skips only when more than half a frame behind
- Exit summary: frame budget, over-budget count, schedule drift,
  frames drawn

**Launcher 1.2:**
- Cheats page: tick entries of `PROGDIR:Cheats/<romname>.cht`
- GPU Output settings on the Video tab
- Obsolete buffer-margin sliders removed

**Build / docs:**
- `-screendepth=16` / `-screendepth=32` command-line switch, so the
  16-bit screen can be tried from the Shell (where the exit summary shows)
- `build.bat` judges success by the output file (the toolchain make
  exits 0 on failure), cleans automatically when options change, and
  builds GPU output in when the PiStorm3D SDK is present
- Sources and docs are plain ASCII; GenesisPlusGX.readme rewritten

**Fixed:**
- `paulafilter=1` / `-paulafilter` turned the audio (LED) filter *off*:
  CIA-A PRA bit 1 = 1 means filter off (Amiga Mail). It now turns the
  filter on, and the CIA bit is changed under `Disable()`

### v1.6 (06.07.2026)

**Fixed:**
- **Player 2 input now works** - button state was written to the wrong core
  device slot (`input.pad[1]` instead of `input.pad[4]`) since the first
  release; keyboard/numpad and mouse-port joystick both control player 2 now
- AHI buffer sized without blip_buf rounding headroom - 1-2 samples were
  silently dropped nearly every frame (small waveform skip at each buffer
  boundary)
- 8-bit audio converters (ASM and C) now round-to-nearest consistently
  instead of truncating
- Keys no longer stick when the emulator window loses focus mid-keypress
- Config parser now trims whitespace (`key = value` works)
- A `.zip` ROM given to a binary built without `USE_ZIP=1` now reports a
  clear error instead of silently loading garbage (black screen)
- `make USE_ZIP=1` without an explicit target no longer builds only miniz
- Failed audio backend init now disables sound chip emulation entirely
  instead of generating audio that is never played

**New:**
- `screendepth=16` - 16-bit RTG output: half the blit bandwidth of the
  32-bit path, direct RGB565 row copy on native-format screens; all eight
  15/16-bit CGX pixel formats supported, safe fallback to 32-bit
- `hqfm` / `hqpsg` - sound synthesis quality knobs; `hqfm=0` switches the
  YM2612 to linear resampling (later measured: about 1% of the frame on a Pi 3)
- Launcher GUI now exposes every config setting, including the previously
  file-only buffer margins, plus the new screen depth and HQ audio options

## Credits

- Genesis Plus GX by Eke-Eke: https://github.com/ekeeke/Genesis-Plus-GX
- Original Genesis Plus by Charles MacDonald
- bebbo's amiga-gcc toolchain: https://github.com/bebbo/amiga-gcc
- Emu68 by Michal Schulz
- PiStorm community
- CaffeineOS distribution

## License

Genesis Plus GX is distributed under a non-commercial license. See LICENSE.txt for details.

This port follows the same license terms as the original Genesis Plus GX project.
