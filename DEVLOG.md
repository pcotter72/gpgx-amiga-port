# Development Log

---

## 2026-04-28

### Bug fix: SetPointer sprite data in Fast RAM (Enforcer Severity 3)

**Hit:**
```
intuition.library SetPointer(0x08F7A1FC,0x0A2D94E0,1,1,0,0)
Severity 3: sprite data must be in CHIP ram
----> 098B0EC4 - "genesisplusgx" Hunk 0000, Offset 000FA17C
```

**Root cause:** `blank_pointer[]` in `amiga/video_cgx.c:111` was declared as a plain
`static UWORD[]`. bebbo's GCC places ordinary statics in BSS/data, which the linker
maps to Fast RAM. The Amiga sprite DMA is custom chip DMA — it can only address Chip
RAM. `SetPointer()` hands the pointer directly to the hardware sprite registers;
passing a Fast RAM address causes it to DMA garbage or nothing.

**Fix:** `amiga/video_cgx.c:111`
```c
/* before */
static UWORD blank_pointer[] = { 0, 0, 0, 0 };
/* after */
static UWORD __chip blank_pointer[] = { 0, 0, 0, 0 };
```
`__chip` is a bebbo GCC extension that places the variable in the `.chip` section,
which the linker maps into Chip RAM.

**Build note (Windows):** `make` with LTO (`-flto`) fails from Git Bash / MSYS2 because
`COLLECT_LTO_WRAPPER` is not propagated into the GCC linker invocation. Use PowerShell
to call `make` — it inherits the Windows environment correctly and LTO links cleanly.

---

## 2026-07-06

### v1.6 - Full code audit: bug fixes + speed options for slow Amigas

**Player 2 never worked (since first release).** `osd_input_update()` wrote
player 2 buttons to `input.pad[1]`, but the GPGX core reads control port 2
from device slot 4 (`gamepad_2_read()` -> `gamepad_read(4)`, `input.dev[4]`).
`pad[1]` is a multitap-only slot that plain gamepads never read. Fix: write
player 2 state to `input.pad[4]` (amiga/main.c).

**AHI dropped 1-2 samples nearly every frame.** `audio_init(freq, 0.0)` makes
the core run at the exact Genesis frame rate (59.9224 Hz NTSC), so blip_buf
delivers ~736 samples/frame at 44100 Hz - but the AHI buffer was sized
`ceil(freq/60)` = 735 with no headroom, silently clamping the excess. The
Paula backends already had +4.5 headroom. Fix: `+4` blip rounding headroom in
`ahi_audio_init()`.

**Converter rounding.** ASM 8-bit converters truncated (`asr`) where the C
mono path rounded; C stereo didn't round at all. All four paths now
round-to-nearest (add 256 before >>9 mono, 128 before >>8 stereo).

**Stuck keys on focus loss.** Key-up events go to the newly activated window,
latching whatever was held. `key_state[]` is now cleared on
IDCMP_ACTIVEWINDOW/INACTIVEWINDOW.

**ZIP black-screen foot-gun.** A binary built without USE_ZIP=1 silently read
raw compressed ZIP bytes into the ROM buffer -> emulated CPU executes garbage
-> black screen with only the FPS counter. `load_archive()` now refuses .zip
files with a clear error when ZIP support isn't compiled in. Related Makefile
fix: the miniz rules preceded `all:`, so `make USE_ZIP=1` without a target
built only miniz.o (`.DEFAULT_GOAL := all`).

**Misc:** config parser trims whitespace around key/value; failed audio init
now sets audio_disabled (skips FM/PSG/blip work entirely).

### New: screendepth=16 - 16-bit RTG output path

The port always opened a 32-bit screen and converted RGB565 -> 32bpp through
a 64K LUT: 4 bytes/pixel over the bus. `screendepth=16` opens a 16-bit (or
15-bit) mode instead: half the bandwidth, and on PIXFMT_RGB16 screens (GPGX's
native RGB565 BE) the 1x blit is a straight CopyMem row copy with zero
per-pixel work. All eight 15/16-bit CGX formats handled via a UWORD LUT that
overlays the 32-bit LUT storage (no extra memory). Aimed at classic Amigas
with Zorro RTG cards (Picasso II ~3.5 MB/s: 32bpp@60fps needs ~18 MB/s -
impossible; 16bpp direct copy makes ~25-30 fps realistic). Falls back to
32-bit when no 16-bit mode exists.

### New: hqfm / hqpsg config keys

`config.hq_fm`/`hq_psg` were hardcoded to 1 (band-limited synthesis). Now
exposed in the config; `hqfm=0` switches the YM2612 to linear interpolation -
one of the largest CPU knobs in the core for Pi 3 / real 68030.

### Launcher: complete settings coverage

Added GUI widgets for screendepth (Video tab), hqfm/hqpsg and the
paulamargin/ahimargin sliders (Audio tab - the margins were config-file-only
before). Every cfg key now has a widget; launcher Config struct/load/save
extended so saves don't drop the new keys.
