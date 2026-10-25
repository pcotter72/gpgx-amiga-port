/****************************************************************************
 *  main.c
 *
 *  Genesis Plus GX - AmigaOS/CaffeineOS Main Frontend
 *  68k conversion by Pedro Cotter
 *
 *  Author: Pedro Cotter
 *  Requires: AmigaOS 3.x, CyberGraphX/P96, AHI
 *
 ****************************************************************************/

/* Version info */
#define GPGX_VERSION  "1.11"
#define GPGX_DATE     "25.10.2026"
#define GPGX_AUTHOR   "Pedro Cotter"

/* Standard AmigaOS version string - readable by C:Version command */
static const char amiga_version[] = "$VER: GenesisPlusGX " GPGX_VERSION " (" GPGX_DATE ") by " GPGX_AUTHOR;

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Before shared.h: its typedef of uint must come before m68k.h's
   "#define uint unsigned int". The 2026-10-24 libc headers no longer pull
   it in through stdio.h/stdlib.h, so miniz.h's <time.h> brought it in
   after the macro ("duplicate 'unsigned'"). */
#include <sys/types.h>

/* AmigaOS includes */
#include <exec/exec.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <devices/timer.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/timer.h>

/* Genesis Plus GX core includes */
#include "shared.h"
#include "md_ntsc.h"
#include "sms_ntsc.h"

/* Amiga subsystem includes */
#include "video_cgx.h"
#ifdef USE_MINIGL
#include "video_gl.h"
#endif
#include "audio_paula.h"
#include "audio_paula14.h"
#include "prof.h"
#ifdef GPGX_PROFILE
extern void prof_read3(ULONG *out);
static int prof_flushtest = 0;
static ULONG prof_flushes = 0;
static double prof_flush_ticks = 0.0;   /* Emu68 counter ticks */
#define PROF_INTERVAL 300               /* frames per "Interval" line */
static int prof_jitdepth = 0;           /* -jitdepth=N: Emu68 instructions per unit */
extern unsigned int ssp_iram_gen;       /* core/cart_hw/svp/ssp16.c */
#endif
#include "audio_ahi.h"
#include "input_amiga.h"
#include "crc32.h"

/* Audio backend selection */
static int audio_use_ahi = 0;
static int audio_use_14bit = 0;
static int audio_disabled = 0;
static int frame_skip = 0;

/* Real Amiga hardware PAL/NTSC flag (for Paula crystal clock).
 * This is a HARDWARE property of the motherboard, independent of
 * what game the Genesis emulator is running. Detected at startup
 * via GfxBase->DisplayFlags. */
static int amiga_hw_pal = 1;  /* default PAL, detected at runtime */

/* NTSC blitter objects (disabled for now to save memory) */
md_ntsc_t *md_ntsc = NULL;
sms_ntsc_t *sms_ntsc = NULL;

/* Libraries */
struct Library *CyberGfxBase = NULL;
struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;
struct Device *TimerBase = NULL;

/* Video configuration */
#define VIDEO_WIDTH  320
#define VIDEO_HEIGHT 240

/* Audio configuration */
#define AUDIO_FREQUENCY 27710  /* Optimal Paula rate (period 128) */
/* Autoskip never skips more than this many frames in a row, so a machine
 * that is permanently a little behind still draws every (N+1)th frame.
 * Was 4 (every 5th frame): an A500 with a Pi 3 that is 11% too slow then
 * drew 22% of its frames (measured 2026-10-20). */
#define AUTOSKIP_MAX_RUN 2
#define AUDIO_SAMPLES   1024  /* Must cover worst case: period 80 PAL = 892 spf.
                               * 800 caused buffer overflow at period 86 (825 spf)
                               * and period 80 (892 spf). 1024 covers all rates. */

static short audio_buffer[AUDIO_SAMPLES * 2];

/* Global configuration */
t_config config;

/* BIOS path variables (optional) */
char GG_ROM[256] = "";
char AR_ROM[256] = "";
char SK_ROM[256] = "";
char SK_UPMEM[256] = "";
char GG_BIOS[256] = "";
char MD_BIOS[256] = "";
char CD_BIOS_EU[256] = "";
char CD_BIOS_US[256] = "";
char CD_BIOS_JP[256] = "";
char MS_BIOS_US[256] = "";
char MS_BIOS_EU[256] = "";
char MS_BIOS_JP[256] = "";

/* Running flag */
static volatile int running = 1;

/* Save state / SRAM support */
static char rom_path[512];         /* Full path to loaded ROM */
static char sram_path[512];        /* SRAM battery backup path (.srm) */
static char state_path[512];       /* Save state path (.gsx) */
static unsigned char *state_buf = NULL;  /* Save state buffer */
static int sram_load_failed = 0;   /* .srm read error: autosave would clobber it */

/* On-screen message for save/load feedback - handled by video_cgx */

/* Frame timing */
static struct MsgPort *timer_port = NULL;
static struct timerequest *timer_req = NULL;
static int timer_open = 0;
static ULONG eclock_freq = 0;  /* EClock Hz, set by timer_init (PAL ~709379, NTSC ~715909) */

/****************************************************************************
 * Default configuration
 ****************************************************************************/
static void config_default(void)
{
    memset(&config, 0, sizeof(config));
    
    strcpy(config.version, "GPGX-AMIGA");
    
    /* Sound */
    config.hq_fm = 1;       /* Band-limited synthesis for clean resampling */
    config.hq_psg = 1;      /* High quality PSG resampling */
    config.filter = 0;      /* LP filter: inaudible on 8-bit Paula (quantisation noise dominates) */
    config.psg_preamp = 60;
    config.fm_preamp = 50;    /* Conservative: prevents blip_buf clipping */
                               /* realistic peak ~28000 with typical channel usage */
    config.cdda_volume = 100;
    config.pcm_volume = 100;
    config.lp_range = 0x9999;
    config.low_freq = 880;
    config.high_freq = 5000;
    config.lg = 100;
    config.mg = 100;
    config.hg = 100;
    config.mono = 0;
    config.ym2612 = 0;   /* MAME core */
    config.ym2413 = 2;   /* EMU2413 */
    
    /* System */
    config.system = 0;          /* AUTO */
    config.region_detect = 0;   /* AUTO */
    config.master_clock = 0;    /* AUTO */
    config.vdp_mode = 0;        /* AUTO */
    config.force_dtack = 0;
    config.addr_error = 1;
    config.bios = 0;
    config.lock_on = 0;
    config.add_on = 0;          /* AUTO */
    
    /* Display */
    config.overscan = 0;
    config.aspect_ratio = 0;
    config.ntsc = 0;
    config.lcd = 0;
    config.gg_extra = 0;  /* Crop to authentic 160x144 GG LCD area */
    config.left_border = 0;
    config.render = 0;
    
    /* Controllers - auto-detect 3 or 6 button from ROM header */
    config.input[0].device = 0;  /* Gamepad */
    config.input[0].port = 0;
    config.input[0].padtype = DEVICE_PAD2B | DEVICE_PAD3B | DEVICE_PAD6B; /* Auto */
    config.input[1].device = 0;
    config.input[1].port = 0;
    config.input[1].padtype = DEVICE_PAD2B | DEVICE_PAD3B | DEVICE_PAD6B; /* Auto */
    
    /* Misc */
    config.invert_mouse = 0;
    config.gun_cursor = 1;
    config.overclock = 0;
    config.no_sprite_limit = 0;
    config.enhanced_vscroll = 0;
    config.enhanced_vscroll_limit = 8;
    config.cd_latency = 1;
}

/****************************************************************************
 * OSD Input Update - Read Amiga input devices
 ****************************************************************************/
void osd_input_update(void)
{
    int i;
    unsigned short state;
    
    /* Clear previous input */
    for (i = 0; i < MAX_INPUTS; i++)
    {
        input.pad[i] = 0;
    }
    
    /* Read player 1 */
    state = input_amiga_read(0);
    
    /* Map to Genesis format - note: Genesis core defines are in input.h:
     * INPUT_UP=0x0001, INPUT_DOWN=0x0002, INPUT_LEFT=0x0004, INPUT_RIGHT=0x0008
     * INPUT_B=0x0010, INPUT_C=0x0020, INPUT_A=0x0040, INPUT_START=0x0080
     * INPUT_Z=0x0100, INPUT_Y=0x0200, INPUT_X=0x0400, INPUT_MODE=0x0800
     */
    if (state & INPUT_BTN_UP)    input.pad[0] |= INPUT_UP;     /* 0x0001 */
    if (state & INPUT_BTN_DOWN)  input.pad[0] |= INPUT_DOWN;   /* 0x0002 */
    if (state & INPUT_BTN_LEFT)  input.pad[0] |= INPUT_LEFT;   /* 0x0004 */
    if (state & INPUT_BTN_RIGHT) input.pad[0] |= INPUT_RIGHT;  /* 0x0008 */
    if (system_hw < SYSTEM_MD)
    {
        /* SMS/GG/SG-1000: 2-button pad - A=Button1, B=Button2, C=unused */
        if (state & INPUT_BTN_A) input.pad[0] |= INPUT_BUTTON1;
        if (state & INPUT_BTN_B) input.pad[0] |= INPUT_BUTTON2;
    }
    else
    {
        if (state & INPUT_BTN_B) input.pad[0] |= INPUT_B;
        if (state & INPUT_BTN_C) input.pad[0] |= INPUT_C;
        if (state & INPUT_BTN_A) input.pad[0] |= INPUT_A;
        if (state & INPUT_BTN_Z) input.pad[0] |= INPUT_Z;
        if (state & INPUT_BTN_Y) input.pad[0] |= INPUT_Y;
        if (state & INPUT_BTN_X) input.pad[0] |= INPUT_X;
        if (state & INPUT_BTN_MODE) input.pad[0] |= INPUT_MODE;
    }
    if (state & INPUT_BTN_START) input.pad[0] |= INPUT_START;

    /* Read player 2.
     * Control port 2 is device slot 4 in the core (gamepad_2_read() reads
     * input.pad[4]); pad[1] is only used by multitap slot 2. */
    state = input_amiga_read(1);

    if (state & INPUT_BTN_UP)    input.pad[4] |= INPUT_UP;
    if (state & INPUT_BTN_DOWN)  input.pad[4] |= INPUT_DOWN;
    if (state & INPUT_BTN_LEFT)  input.pad[4] |= INPUT_LEFT;
    if (state & INPUT_BTN_RIGHT) input.pad[4] |= INPUT_RIGHT;
    if (system_hw < SYSTEM_MD)
    {
        /* SMS/GG/SG-1000: 2-button pad - A=Button1, B=Button2, C=unused */
        if (state & INPUT_BTN_A) input.pad[4] |= INPUT_BUTTON1;
        if (state & INPUT_BTN_B) input.pad[4] |= INPUT_BUTTON2;
    }
    else
    {
        if (state & INPUT_BTN_B) input.pad[4] |= INPUT_B;
        if (state & INPUT_BTN_C) input.pad[4] |= INPUT_C;
        if (state & INPUT_BTN_A) input.pad[4] |= INPUT_A;
        if (state & INPUT_BTN_Z) input.pad[4] |= INPUT_Z;
        if (state & INPUT_BTN_Y) input.pad[4] |= INPUT_Y;
        if (state & INPUT_BTN_X) input.pad[4] |= INPUT_X;
        if (state & INPUT_BTN_MODE) input.pad[4] |= INPUT_MODE;
    }
    if (state & INPUT_BTN_START) input.pad[4] |= INPUT_START;
}

/****************************************************************************
 * ROM Archive Loader
 ****************************************************************************/
/****************************************************************************
 * ZIP ROM support (compiled in when USE_ZIP is defined)
 * Requires amiga/miniz.h + amiga/miniz.c from github.com/richgel999/miniz
 *
 * load_archive() detects a .zip extension and delegates here.
 * The first file with a known ROM extension is extracted; its inner
 * extension is returned so the core identifies the system correctly.
 ****************************************************************************/
#ifdef USE_ZIP
#include "miniz.h"

static const char *zip_rom_exts[] = {
    ".bin", ".gen", ".md", ".smd", ".rom",
    ".sms", ".gg", ".sg", NULL
};

/* Case-insensitive full-extension match: ci_ext_match("Sonic.BIN", ".bin") = 1 */
static int ci_ext_match(const char *filename, const char *ext)
{
    const char *dot = NULL;
    const char *p = filename;
    int a, b;
    while (*p) { if (*p == '.') dot = p; p++; }
    if (!dot) return 0;
    p = dot;
    while (*ext && *p)
    {
        a = (unsigned char)*ext; b = (unsigned char)*p;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
        ext++; p++;
    }
    return !*ext && !*p;
}

/* Fill the 4-byte extension buffer the same way the plain path does */
static void fill_ext_from_name(const char *filename, char *extension)
{
    const char *dot = NULL;
    const char *p = filename;
    int i, len;
    while (*p) { if (*p == '.') dot = p; p++; }
    if (dot)
    {
        const char *src = dot + 1;
        len = (int)strlen(src);
        if      (len >= 3) { extension[0]=src[0]; extension[1]=src[1]; extension[2]=src[2]; extension[3]=0; }
        else if (len == 2) { extension[0]=' ';    extension[1]=src[0]; extension[2]=src[1]; extension[3]=0; }
        else if (len == 1) { extension[0]=' ';    extension[1]=' ';    extension[2]=src[0]; extension[3]=0; }
        else               { extension[0]=0; return; }
        for (i = 0; i < 3; i++)
            if (extension[i] >= 'A' && extension[i] <= 'Z') extension[i] += 32;
    }
    else extension[0] = 0;
}

static int load_archive_zip(char *filename, unsigned char *buffer, int maxsize, char *extension)
{
    mz_zip_archive zip;
    mz_zip_archive_file_stat fstat;
    mz_uint i, num_files;
    int found, j, result;
    FILE *f;
    long zip_size;
    unsigned char *zip_data;

    /* Load the entire ZIP file into memory so miniz uses the in-memory
       extraction path: all compressed data is available at once and
       tinfl_decompress runs in a single call with no coroutine resume. */
    f = fopen(filename, "rb");
    if (!f)
    {
        printf("ZIP: Cannot open %s\n", filename);
        return 0;
    }
    fseek(f, 0L, SEEK_END);
    zip_size = ftell(f);
    fseek(f, 0L, SEEK_SET);
    if (zip_size <= 0)
    {
        fclose(f);
        return 0;
    }
    zip_data = (unsigned char *)malloc((size_t)zip_size);
    if (!zip_data)
    {
        printf("ZIP: Out of memory for ZIP buffer (%ld bytes)\n", zip_size);
        fclose(f);
        return 0;
    }
    if (fread(zip_data, 1, (size_t)zip_size, f) != (size_t)zip_size)
    {
        printf("ZIP: Read error\n");
        free(zip_data);
        fclose(f);
        return 0;
    }
    fclose(f);

    mz_zip_zero_struct(&zip);
    if (!mz_zip_reader_init_mem(&zip, zip_data, (size_t)zip_size, 0))
    {
        printf("ZIP: Not a valid ZIP: %s\n", filename);
        free(zip_data);
        return 0;
    }

    num_files = mz_zip_reader_get_num_files(&zip);
    found = -1;

    /* Pass 1: prefer a file with a known ROM extension */
    for (i = 0; i < num_files && found < 0; i++)
    {
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        if (!mz_zip_reader_file_stat(&zip, i, &fstat)) continue;
        for (j = 0; zip_rom_exts[j]; j++)
        {
            if (ci_ext_match(fstat.m_filename, zip_rom_exts[j]))
            {
                found = (int)i;
                break;
            }
        }
    }

    /* Pass 2: fall back to first non-directory file */
    if (found < 0)
    {
        for (i = 0; i < num_files; i++)
        {
            if (!mz_zip_reader_is_file_a_directory(&zip, i))
            {
                found = (int)i;
                break;
            }
        }
    }

    if (found < 0)
    {
        printf("ZIP: No files found in %s\n", filename);
        mz_zip_reader_end(&zip);
        free(zip_data);
        return 0;
    }

    if (!mz_zip_reader_file_stat(&zip, (mz_uint)found, &fstat))
    {
        mz_zip_reader_end(&zip);
        free(zip_data);
        return 0;
    }

    if ((int)fstat.m_uncomp_size > maxsize)
    {
        printf("ZIP: ROM too large (%d bytes, max %d)\n",
               (int)fstat.m_uncomp_size, maxsize);
        mz_zip_reader_end(&zip);
        free(zip_data);
        return 0;
    }

    printf("ZIP: Extracting %s -> %s (%d bytes)\n",
           filename, fstat.m_filename, (int)fstat.m_uncomp_size);

    result = 0;
    if (!mz_zip_reader_extract_to_mem(&zip, (mz_uint)found,
                                       buffer, (size_t)fstat.m_uncomp_size, 0))
    {
        printf("ZIP: Extraction failed (%s)\n",
               mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
    }
    else
    {
        if (extension)
            fill_ext_from_name(fstat.m_filename, extension);
        result = (int)fstat.m_uncomp_size;
    }

    mz_zip_reader_end(&zip);
    free(zip_data);
    return result;
}
#endif /* USE_ZIP */

int load_archive(char *filename, unsigned char *buffer, int maxsize, char *extension)
{
    BPTR file;
    int size = 0;

    /* Detect .zip extension (case-insensitive) */
    {
        const char *p = filename;
        const char *dot = NULL;
        while (*p) { if (*p == '.') dot = p; p++; }
        if (dot)
        {
            const char *e = dot + 1;
            if ((e[0]=='z'||e[0]=='Z') && (e[1]=='i'||e[1]=='I') &&
                (e[2]=='p'||e[2]=='P') && e[3]==0)
            {
#ifdef USE_ZIP
                return load_archive_zip(filename, buffer, maxsize, extension);
#else
                /* Never load raw ZIP bytes as a ROM - the core would execute
                 * garbage and show a black screen with no explanation. */
                printf("Error: %s is a ZIP archive but ZIP support is not\n"
                       "compiled into this binary (build with USE_ZIP=1).\n",
                       filename);
                return 0;
#endif
            }
        }
    }

    file = Open(filename, MODE_OLDFILE);
    if (!file)
    {
        printf("Error: Cannot open %s\n", filename);
        return 0;
    }
    
    /* Get file size */
    Seek(file, 0, OFFSET_END);
    size = Seek(file, 0, OFFSET_BEGINNING);
    
    if (size > maxsize)
    {
        printf("Error: ROM too large (%d bytes, max %d)\n", size, maxsize);
        Close(file);
        return 0;
    }
    
    /* Read file */
    if (Read(file, buffer, size) != size)
    {
        printf("Error: Failed to read ROM\n");
        Close(file);
        return 0;
    }
    
    Close(file);
    
    /* Extract extension including dot (4 chars: ".ext" + null terminator) */
    if (extension)
    {
        char *p = strrchr(filename, '.');
        if (p)
        {
            int len;
            p++;  /* skip the dot */
            len = strlen(p);
            if (len >= 3)
            {
                /* 3+ char extension: "sms", "bin", "gen" etc */
                extension[0] = p[0];
                extension[1] = p[1];
                extension[2] = p[2];
                extension[3] = 0;
            }
            else if (len == 2)
            {
                /* 2 char extension: "gg", "sg" - pad at front so ext[1..2] has it */
                extension[0] = ' ';
                extension[1] = p[0];
                extension[2] = p[1];
                extension[3] = 0;
            }
            else if (len == 1)
            {
                extension[0] = ' ';
                extension[1] = ' ';
                extension[2] = p[0];
                extension[3] = 0;
            }
            else
            {
                extension[0] = 0;
            }
        }
        else
        {
            extension[0] = 0;
        }
    }
    
    return size;
}

/****************************************************************************
 * Cheat system - loads PROGDIR:Cheats/<romname>.cht (RetroArch format)
 *
 * Supports:
 *   - Game Genie (XXXX-XXXX)  -> ROM patch (once) or RAM patch (per-frame)
 *   - GameShark/AR (AAAAAA:DDDD hex) -> RAM patch (per-frame)
 *   - '+'-joined multi-code entries
 *
 * Only entries with enable = true are applied. Disabled by default.
 * RAM patches (addresses >= $E00000) are reapplied every frame because
 * the game resets values like health/lives on each frame.
 ****************************************************************************/

/* Per-frame RAM patches (work_ram[offset] = data each frame) */
#define MAX_RAM_PATCHES 64
static struct { uint32 offset; uint16 data; uint8 is_word; } ram_patches[MAX_RAM_PATCHES];
static int ram_patch_count = 0;
static int rom_patch_count = 0;   /* ROM words changed (header checksum fix) */

/* Genesis Game Genie alphabet (32 chars, no I O Q U) */
static const char gg_alpha[32] = "ABCDEFGHJKLMNPRSTVWXYZ0123456789";

static int gg_char_val(char c)
{
    int i;
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (i = 0; i < 32; i++)
        if (gg_alpha[i] == c) return i;
    return -1;
}

/* Decode one Genesis Game Genie token (XXXX-XXXX, 8 code chars).
 * The 40 code bits are scrambled, not linear. Per character (5 bits each,
 * MSB first; A = address bits 23..0, D = data bits 15..0):
 *   n[0] = D7..D3          n[1] = D2..D0, A15..A14
 *   n[2] = A13..A9         n[3] = A8, A23..A20
 *   n[4] = A19..A16, D12   n[5] = D11..D8, D15
 *   n[6] = D14..D13, A7..A5   n[7] = A4..A0
 * Matches decode_cheat() in upstream Genesis Plus GX gx/gui/cheats.c. */
static int gg_decode(const char *code, uint32 *addr, uint16 *data)
{
    int n[8], i, j;
    char c;
    j = 0;
    for (i = 0; code[i] && j < 8; i++)
    {
        c = code[i];
        if (c == '-') continue;
        n[j] = gg_char_val(c);
        if (n[j] < 0) return 0;
        j++;
    }
    if (j != 8) return 0;
    *addr = ((uint32)(n[1] & 3)   << 14) | ((uint32)n[2]        <<  9) |
            ((uint32)(n[3] & 0xF) << 20) | ((uint32)(n[3] >> 4) <<  8) |
            ((uint32)(n[4] >> 1)  << 16) | ((uint32)(n[6] & 7)  <<  5) |
            (uint32)n[7];
    *data = (uint16)((n[0] << 3)        | (n[1] >> 2)        |
                     ((n[4] & 1) << 12) | ((n[5] & 1) << 15) |
                     ((n[5] >> 1) << 8) | ((n[6] >> 3) << 13));
    return 1;
}

/* Decode GameShark/AR hex format "AAAAAA:DDDD". */
static int gs_decode(const char *code, uint32 *addr, uint16 *data)
{
    unsigned long a, d;
    if (sscanf(code, "%lx:%lx", &a, &d) == 2)
    {
        *addr = (uint32)a;
        *data = (uint16)d;
        return 1;
    }
    return 0;
}

/* Route a decoded (addr, data) pair.
 * Even addr < cart.romsize -> ROM patch (once).
 * addr >= $E00000          -> RAM patch (per-frame via work_ram).
 * Odd addr                 -> silently skip (master codes).
 * Returns 1 if handled, 0 if skipped. */
static int route_patch(uint32 addr, uint16 data)
{
    if (!(addr & 1) && addr + 1 < (uint32)cart.romsize)
    {
        *(uint16 *)(cart.rom + addr) = data;
        rom_patch_count++;
        return 1;
    }
    if (addr >= 0xE00000U)
    {
        if (ram_patch_count < MAX_RAM_PATCHES)
        {
            /* Same rule as upstream RAMCheatUpdate(): a value with any bit
             * set in the high byte is a word patch at the even address,
             * otherwise a byte patch at the exact address (e.g. the common
             * FFxxxx:00YY form writes YY to FFxxxx, not 00 to it). Upstream
             * selects the byte by OR-ing 1 into the address mask, so its word
             * mask is taken to have bit 0 clear. */
            uint8 is_word = (data & 0xFF00) ? 1 : 0;
            ram_patches[ram_patch_count].offset  = addr & (is_word ? 0xFFFE : 0xFFFF);
            ram_patches[ram_patch_count].data    = data;
            ram_patches[ram_patch_count].is_word = is_word;
            ram_patch_count++;
            return 1;
        }
        return 0;
    }
    /* Odd address (master codes) or unmapped region - skip silently */
    return 0;
}

/* Apply one code string (may be '+'-joined). Returns count of routed patches. */
static int apply_cht_code(const char *code_str, const char *desc)
{
    char buf[256];
    char *tok, *plus;
    uint32 addr;
    uint16 data;
    int any = 0;
    int l;

    strncpy(buf, code_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    tok = buf;
    while (tok && *tok)
    {
        while (*tok == ' ') tok++;
        plus = strchr(tok, '+');
        if (plus) *plus = 0;
        l = strlen(tok);
        while (l > 0 && tok[l-1] == ' ') tok[--l] = 0;

        if (*tok)
        {
            if (gg_decode(tok, &addr, &data) || gs_decode(tok, &addr, &data))
            {
                if (route_patch(addr, data))
                {
                    any++;
                }
                /* Decoded but not applied. Say why - a silent skip here is
                 * indistinguishable from a cheat that simply has no effect. */
                else if (addr >= 0xE00000U)
                    printf("Cheat: '%s' dropped, patch table full (max %d)\n",
                           tok, MAX_RAM_PATCHES);
                else if (addr & 1)
                    printf("Cheat: '%s' not applied, odd address %06lX (master code?)\n",
                           tok, (unsigned long)addr);
                else
                    printf("Cheat: '%s' not applied, address %06lX is past the end of this ROM\n",
                           tok, (unsigned long)addr);
            }
            else
                printf("Cheat: unknown format '%s'\n", tok);
        }
        tok = plus ? plus + 1 : NULL;
    }
    if (any)
        printf("Cheat ON: %s\n", desc);
    return any;
}

/* Reapply RAM patches every frame (game resets counters each frame). */
static void apply_ram_patches(void)
{
    int i;
    /* work_ram is stored big-endian here (no LSB_FIRST), so 68K byte
     * address N is work_ram[N] - no ^1 as on little-endian hosts. */
    for (i = 0; i < ram_patch_count; i++)
    {
        if (ram_patches[i].is_word)
            *(uint16 *)(work_ram + ram_patches[i].offset) = ram_patches[i].data;
        else
            work_ram[ram_patches[i].offset] = (uint8)ram_patches[i].data;
    }
}

/* Load and apply PROGDIR:Cheats/<romname>.cht.
 * Mega Drive only: gg_decode() implements the Genesis Game Genie layout and
 * route_patch() uses the 68K memory map, so running it for an SMS, Game Gear
 * or SG-1000 ROM would patch meaningless ROM offsets. SMS Game Genie codes
 * use a different format entirely and cannot be decoded here. */
static void load_cheats_cht(const char *romfile)
{
    BPTR f;
    char cht_path[512];
    char line[512];
    char key[64], value[256];
    char pending_code[256];
    char pending_desc[256];
    int  pending_idx = -1;
    int  applied = 0;
    int  pos, ch_val;
    const char *p;
    const char *base;
    const char *dot;
    char *eq, *v, *vend;
    int  len;

    base = romfile;
    for (p = romfile; *p; p++)
        if (*p == '/' || *p == ':') base = p + 1;
    dot = NULL;
    for (p = base; *p; p++)
        if (*p == '.') dot = p;
    len = dot ? (int)(dot - base) : (int)strlen(base);
    if (len > 255) len = 255;
    snprintf(cht_path, sizeof(cht_path), "PROGDIR:Cheats/%.*s.cht", len, base);

    f = Open(cht_path, MODE_OLDFILE);
    if (!f)
        return;   /* no cheat file for this ROM - normal, stay quiet */

    if (system_hw != SYSTEM_MD)
    {
        printf("Cheats: %s ignored - codes are Mega Drive only\n", cht_path);
        Close(f);
        return;
    }

    pending_code[0] = 0;
    pending_desc[0] = 0;

    for (;;)
    {
        pos = 0;
        for (;;)
        {
            if (Read(f, &line[pos], 1) != 1) { if (!pos) goto done; break; }
            ch_val = (unsigned char)line[pos];
            if (ch_val == '\n' || ch_val == '\r') break;
            if (pos < (int)sizeof(line) - 1) pos++;
        }
        line[pos] = 0;
        if (!line[0] || line[0] == ';' || line[0] == '#') continue;

        eq = strchr(line, '=');
        if (!eq) continue;

        *eq = 0;
        strncpy(key, line, sizeof(key) - 1);
        key[sizeof(key) - 1] = 0;
        {
            int kl = strlen(key);
            while (kl > 0 && (key[kl-1] == ' ' || key[kl-1] == '\t')) key[--kl] = 0;
        }

        v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (*v == '"') v++;
        strncpy(value, v, sizeof(value) - 1);
        value[sizeof(value) - 1] = 0;
        vend = value + strlen(value);
        while (vend > value && (*(vend-1) == ' ' || *(vend-1) == '\t' ||
                                *(vend-1) == '"' || *(vend-1) == '\r')) vend--;
        *vend = 0;

        if (strncmp(key, "cheat", 5) != 0) continue;
        {
            char *under = strchr(key + 5, '_');
            int   idx;
            const char *field;
            if (!under) continue;
            idx   = atoi(key + 5);
            field = under + 1;

            if (strcmp(field, "desc") == 0)
            {
                if (idx != pending_idx) { pending_idx = idx; pending_code[0] = 0; }
                strncpy(pending_desc, value, sizeof(pending_desc) - 1);
                pending_desc[sizeof(pending_desc) - 1] = 0;
            }
            else if (strcmp(field, "code") == 0)
            {
                if (idx != pending_idx) { pending_idx = idx; pending_desc[0] = 0; }
                strncpy(pending_code, value, sizeof(pending_code) - 1);
                pending_code[sizeof(pending_code) - 1] = 0;
            }
            else if (strcmp(field, "enable") == 0 && strcmp(value, "true") == 0)
            {
                if (idx == pending_idx && pending_code[0])
                    if (apply_cht_code(pending_code, pending_desc) > 0)
                        applied++;
            }
        }
    }
done:
    Close(f);
    /* A ROM patch changes the sum the game's start-up code checks: the
     * standard Sega routine (at $234 in Daimakaimura) adds the words from
     * $200 to the header's ROM end address ($1A4, inclusive) and compares
     * the result with the header checksum at $18E; on a mismatch the game
     * stops (red screen). A real Game Genie needs the game's MASTER CODE to
     * skip that test. Writing the new sum into the header passes the test
     * without it (verified 2026-10-20: Daimakaimura (Japan) with only
     * "Untouchable" RYDA-C60G enabled showed the red screen; with the
     * master code EABA-AA3Y, a bra.s over the compare, it started). Games
     * with a different test still need their master code. The core's
     * header-based ROM database lookup ran at load time, before this. */
    if (rom_patch_count > 0 && cart.romsize > 0x200)
    {
        uint32 end = rominfo.romend + 1;   /* header value is inclusive */
        uint32 i;
        uint16 old, sum = 0;
        if (end < 0x202 || end > cart.romsize) end = cart.romsize;
        for (i = 0x200; i + 1 < end; i += 2)
            sum += (uint16)((cart.rom[i] << 8) | cart.rom[i + 1]);
        old = (uint16)((cart.rom[0x18E] << 8) | cart.rom[0x18F]);
        if (sum != old)
        {
            cart.rom[0x18E] = (uint8)(sum >> 8);
            cart.rom[0x18F] = (uint8)sum;
            printf("Cheats: ROM patched, header checksum $%04X -> $%04X so the game's\n"
                   "        own checksum test passes without a master code\n",
                   old, sum);
        }
    }
    /* Printed whenever a .cht exists, including the 0 case: confirms the file
     * was found and parsed, so "nothing happened" can be told apart from
     * "the file was never located". */
    printf("Cheats: %d active (%d RAM) from %s\n",
           applied, ram_patch_count, cht_path);
}

void ROMCheatUpdate(void) { /* cheats handled by apply_ram_patches() each frame */ }

/****************************************************************************
 * Timer initialization
 ****************************************************************************/
static int timer_init(void)
{
    timer_port = CreateMsgPort();
    if (!timer_port)
        return 0;
    
    timer_req = (struct timerequest *)CreateIORequest(timer_port, sizeof(struct timerequest));
    if (!timer_req)
    {
        DeleteMsgPort(timer_port);
        timer_port = NULL;
        return 0;
    }
    
    if (OpenDevice(TIMERNAME, UNIT_MICROHZ, (struct IORequest *)timer_req, 0))
    {
        DeleteIORequest((struct IORequest *)timer_req);
        DeleteMsgPort(timer_port);
        timer_req = NULL;
        timer_port = NULL;
        return 0;
    }
    
    TimerBase = timer_req->tr_node.io_Device;
    timer_open = 1;

    /* Cache EClock frequency - used by eclock_us_since() / timer_wait_until() */
    {
        struct EClockVal ev;
        eclock_freq = ReadEClock(&ev);
    }
    return 1;
}

/****************************************************************************
 * Timer shutdown
 ****************************************************************************/
static void timer_shutdown(void)
{
    if (timer_open)
    {
        CloseDevice((struct IORequest *)timer_req);
        timer_open = 0;
    }
    
    if (timer_req)
    {
        DeleteIORequest((struct IORequest *)timer_req);
        timer_req = NULL;
    }
    
    if (timer_port)
    {
        DeleteMsgPort(timer_port);
        timer_port = NULL;
    }
    
    TimerBase = NULL;
}

/****************************************************************************
 * Delay in microseconds (used for short pauses outside the main loop)
 ****************************************************************************/
static void timer_delay_us(unsigned long microseconds)
{
    if (!timer_open || microseconds < 200)
        return;

    timer_req->tr_node.io_Command = TR_ADDREQUEST;
    timer_req->tr_time.tv_secs = microseconds / 1000000;
    timer_req->tr_time.tv_micro = microseconds % 1000000;
    DoIO((struct IORequest *)timer_req);
}

/****************************************************************************
 * EClock elapsed time since a recorded start point, in microseconds.
 * Safe for durations up to ~6053 s (ULONG wrap at ~709379 Hz).
 ****************************************************************************/
static ULONG eclock_us_since(struct EClockVal *start)
{
    struct EClockVal now;
    ULONG ticks;
    if (!eclock_freq) return 0;
    ReadEClock(&now);
    ticks = now.ev_lo - start->ev_lo;
    return (ULONG)((double)ticks * 1000000.0 / (double)eclock_freq);
}

/****************************************************************************
 * EClock elapsed time between two recorded EClockVal snapshots, in us.
 ****************************************************************************/
static ULONG eclock_diff_us(struct EClockVal *a, struct EClockVal *b)
{
    ULONG ticks;
    if (!eclock_freq) return 0;
    ticks = b->ev_lo - a->ev_lo;
    return (ULONG)((double)ticks * 1000000.0 / (double)eclock_freq);
}

/****************************************************************************
 * Full 64-bit EClock value as a double tick count. Exact for any realistic
 * uptime (2^53 ticks at ~709 kHz is centuries), and lets the frame schedule
 * carry fractional ticks so its average period is exact.
 ****************************************************************************/
static double eclock_ticks(const struct EClockVal *v)
{
    return (double)v->ev_hi * 4294967296.0 + (double)v->ev_lo;
}

/* SVP (Virtua Racing) counters, defined in ssp16.c */
extern unsigned long ssp_dbg_calls, ssp_dbg_idle, ssp_dbg_cycles;
#ifdef SVP_VERIFY
extern unsigned long ssp_verify_runs, ssp_verify_bad;   /* amiga/ssp16_fast.h */
extern unsigned long ssp_drc_vblocks, ssp_drc_vbad;     /* amiga/ssp16_drc.h */
#endif
/* SVP recompiler (amiga/ssp16_drc.h) */
extern unsigned long ssp_drc_blocks, ssp_drc_resets, ssp_drc_fallbacks;
extern unsigned long ssp_drc_runs, ssp_drc_waitstart;
extern unsigned long ssp_drc_disp[7], ssp_drc_hcnt[32];
extern unsigned long ssp_drc_tr_iram, ssp_drc_rs_code, ssp_drc_rs_meta, ssp_drc_rs_fail;
extern double ssp_drc_tr_words, ssp_drc_tr_bytes;
extern double ssp_drc_run_cycles;
extern int ssp_drc_disable;
extern const char *ssp_drc_why;
extern void ssp1601_drc_shutdown(void);
#ifdef SVP_PROFILE
extern unsigned long ssp_prof_op[128];
extern unsigned long ssp_prof_pc[0x10000];

/* Print the n largest entries of a counter array, largest first.
 * Destroys the array (each printed entry is zeroed). */
static void print_top(const char *title, unsigned long *a, int size, int n,
                      double total)
{
    int k, i, best;
    printf("%s\n", title);
    for (k = 0; k < n; k++)
    {
        best = -1;
        for (i = 0; i < size; i++)
            if (a[i] && (best < 0 || a[i] > a[best]))
                best = i;
        if (best < 0)
            break;
        {
            ULONG pct10 = total > 0.0
                ? (ULONG)((double)a[best] * 1000.0 / total + 0.5) : 0;
            printf("  %04x  %10lu  %3lu.%lu%%\n", (unsigned)best, a[best],
                   pct10 / 10, pct10 % 10);
        }
        a[best] = 0;
    }
}
#endif

/****************************************************************************
 * Frame limiter: wait until an absolute EClock deadline.
 *
 * Coarse timer.device sleep until BUSYSPIN_US before the deadline, then an
 * EClock busy-spin for the rest. timer.device DoIO can fire several ms late
 * on PiStorm/Emu68; the busy-spin margin absorbs that.
 *
 * The caller keeps an absolute schedule (deadline += period every frame),
 * so whatever a wait overshoots is taken off the next wait instead of being
 * lost. The previous limiter waited a fixed time from each frame's own
 * start: measured on target (2026-10-09) every frame then ran 110-172 us
 * long, varying from run to run, and the audio queue drifted with it.
 ****************************************************************************/
#define BUSYSPIN_US 3000

static void timer_wait_until(double deadline)
{
    struct EClockVal now;
    double remain_us;

    if (!timer_open || !eclock_freq)
        return;

    /* Phase 1: coarse sleep via timer.device until within BUSYSPIN_US */
    ReadEClock(&now);
    remain_us = (deadline - eclock_ticks(&now)) * 1000000.0 / (double)eclock_freq;
    if (remain_us > (double)(BUSYSPIN_US + 500))
    {
        ULONG sleep_us = (ULONG)(remain_us - (double)BUSYSPIN_US);
        timer_req->tr_node.io_Command = TR_ADDREQUEST;
        timer_req->tr_time.tv_secs    = sleep_us / 1000000;
        timer_req->tr_time.tv_micro   = sleep_us % 1000000;
        DoIO((struct IORequest *)timer_req);
    }

    /* Phase 2: busy-spin to the deadline */
    do {
        ReadEClock(&now);
    } while (eclock_ticks(&now) < deadline);
}

/****************************************************************************
 * Audio rate control helpers (used by the main loop's audio submit).
 *
 * The audio backends play exactly the samples they are given; nothing pads
 * a buffer any more. Two things are done here, once, for every backend:
 *
 *  - Odd-sample carry. Paula's CMD_WRITE needs an even length (RKRM Devices,
 *    CMD_WRITE: "must be an even number"), so when a frame leaves an odd
 *    total, its last stereo sample is held back and played first next frame.
 *    Nothing is dropped and nothing is repeated.
 *
 *  - Rate control. Production and consumption should balance exactly, but a
 *    stall (disk access, a slow frame) or clock tolerance moves the queue
 *    depth, and nothing would ever move it back. So the depth is measured
 *    before each submit: below the target the frame is stretched by ~0.6%,
 *    above it shrunk by the same, and at the target it is left untouched.
 *    In steady state it never acts, so there is no pitch change at all.
 ****************************************************************************/
#define AUDIO_TARGET_DEPTH 2

static short audio_out[(AUDIO_SAMPLES + 8) * 2];  /* carry + one frame */
static short audio_carry[2];
static int   audio_carry_valid = 0;

/* Linear-interpolation resample of interleaved 16-bit stereo, n_in -> n_out
 * sample pairs. First and last samples map exactly, so consecutive frames
 * still join without a step. Only used for the small corrections above. */
static void audio_resample(const short *in, int n_in, short *out, int n_out)
{
    ULONG step, pos;
    LONG a, b, frac;
    int i, idx;

    if (n_in < 2 || n_out < 2)
    {
        for (i = 0; i < n_out; i++)
        {
            idx = (i < n_in) ? i : n_in - 1;
            out[i * 2]     = in[idx * 2];
            out[i * 2 + 1] = in[idx * 2 + 1];
        }
        return;
    }

    step = ((ULONG)(n_in - 1) << 16) / (ULONG)(n_out - 1);
    pos  = 0;
    for (i = 0; i < n_out - 1; i++, pos += step)
    {
        idx  = (int)(pos >> 16);
        if (idx > n_in - 2) idx = n_in - 2;
        /* 15-bit fraction: |b - a| <= 65535, so the product stays below 2^31 */
        frac = (LONG)((pos >> 1) & 0x7FFF);
        a = in[idx * 2];     b = in[idx * 2 + 2];
        out[i * 2]     = (short)(a + (((b - a) * frac) >> 15));
        a = in[idx * 2 + 1]; b = in[idx * 2 + 3];
        out[i * 2 + 1] = (short)(a + (((b - a) * frac) >> 15));
    }
    out[(n_out - 1) * 2]     = in[(n_in - 1) * 2];
    out[(n_out - 1) * 2 + 1] = in[(n_in - 1) * 2 + 1];
}

/* Build this frame's output in audio_out: carry first, then the frame
 * (resampled to n_frame pairs if that differs from n_in). Returns the even
 * number of pairs to play and leaves any odd last pair as the new carry. */
static int audio_prepare(const short *in, int n_in, int n_frame)
{
    int pos = 0, total, play;

    if (n_in < 0) n_in = 0;
    if (n_frame > AUDIO_SAMPLES) n_frame = AUDIO_SAMPLES;
    if (n_in == 0) n_frame = 0;

    if (audio_carry_valid)
    {
        audio_out[0] = audio_carry[0];
        audio_out[1] = audio_carry[1];
        pos = 1;
    }
    if (n_frame == n_in)
        memcpy(&audio_out[pos * 2], in, (size_t)n_in * 2 * sizeof(short));
    else if (n_frame > 0)
        audio_resample(in, n_in, &audio_out[pos * 2], n_frame);

    total = pos + n_frame;
    play  = total & ~1;
    if (total & 1)
    {
        audio_carry[0] = audio_out[(total - 1) * 2];
        audio_carry[1] = audio_out[(total - 1) * 2 + 1];
        audio_carry_valid = 1;
    }
    else
        audio_carry_valid = 0;
    return play;
}

/****************************************************************************
 * Open libraries
 ****************************************************************************/
static int open_libraries(void)
{
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    if (!IntuitionBase)
    {
        printf("Error: Cannot open intuition.library v39+\n");
        return 0;
    }
    
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    if (!GfxBase)
    {
        printf("Error: Cannot open graphics.library v39+\n");
        return 0;
    }
    
    /* Detect real Amiga hardware PAL/NTSC.
     * Paula's DMA clock is tied to the motherboard crystal, NOT the
     * emulated game's video mode.  A PAL A1200 always has a 3,546,895 Hz
     * crystal; an NTSC machine has 3,579,545 Hz.  Using the wrong clock
     * causes blip_buf to generate samples at the wrong rate, leading to
     * systematic sample surplus/deficit and audible trembling. */
    amiga_hw_pal = (GfxBase->DisplayFlags & PAL) ? 1 : 0;
    printf("Amiga hardware: %s\n", amiga_hw_pal ? "PAL" : "NTSC");

    /* CyberGraphX is optional but highly recommended */
    CyberGfxBase = OpenLibrary("cybergraphics.library", 40);
    if (!CyberGfxBase)
    {
        printf("Warning: CyberGraphX not available, using fallback rendering\n");
    }
    
    return 1;
}

/****************************************************************************
 * Close libraries
 ****************************************************************************/
static void close_libraries(void)
{
    if (CyberGfxBase)
    {
        CloseLibrary(CyberGfxBase);
        CyberGfxBase = NULL;
    }
    
    if (GfxBase)
    {
        CloseLibrary((struct Library *)GfxBase);
        GfxBase = NULL;
    }
    
    if (IntuitionBase)
    {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
    }
}

/****************************************************************************
 * Paula hardware low-pass filter control via CIA-A
 * Bit 1 of CIA-A PRA ($BFE001) is /LED: "led light (0=bright)/audio filter
 * control" (HRM Appendix F). Amiga Mail "Switching the audio filter":
 * "ciaa.ciapra |= CIAF_LED; Force the filter off", and "When ON, the filter
 * is about 3db at 7kHz". So:
 * Bit 1 = 0: Power LED bright, filter ON
 * Bit 1 = 1: Power LED dim, filter OFF
 * Read-modify-write under Disable(), as in that example.
 ****************************************************************************/
static int paula_filter_was_set = 0;
static unsigned char paula_filter_original = 0;

static void set_paula_filter(int enable)
{
    volatile unsigned char *ciaa_pra = (volatile unsigned char *)0xBFE001;
    
    Disable();
    /* Save original state on first call so we can restore on exit */
    if (!paula_filter_was_set)
    {
        paula_filter_original = *ciaa_pra & 0x02;
        paula_filter_was_set = 1;
    }

    if (enable)
    {
        *ciaa_pra &= ~0x02;  /* Clear bit 1: filter ON, LED bright */
    }
    else
    {
        *ciaa_pra |= 0x02;   /* Set bit 1: filter OFF, LED dim */
    }
    Enable();
}

static void restore_paula_filter(void)
{
    if (paula_filter_was_set)
    {
        volatile unsigned char *ciaa_pra = (volatile unsigned char *)0xBFE001;
        Disable();
        if (paula_filter_original)
            *ciaa_pra |= 0x02;
        else
            *ciaa_pra &= ~0x02;
        Enable();
        paula_filter_was_set = 0;
    }
}

/****************************************************************************
 * Build save file paths from ROM path
 * ROM: "Games:Roms/Sonic.bin" -> SRAM: "Games:Roms/Sonic.srm"
 *                              -> State: "Games:Roms/Sonic.gsx"
 ****************************************************************************/
static void build_save_paths(const char *romfile)
{
    const char *basename;
    char name_buf[256];
    char *dot;

    /* Extract just the ROM filename (strip directory/volume) */
    basename = strrchr(romfile, '/');
    if (!basename)
        basename = strrchr(romfile, ':');
    if (basename)
        basename++;  /* skip the separator */
    else
        basename = romfile;

    /* Copy basename and strip extension */
    strncpy(name_buf, basename, sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf) - 1] = 0;
    dot = strrchr(name_buf, '.');
    if (dot)
        *dot = 0;

    /* Copy full ROM path for reference */
    strncpy(rom_path, romfile, sizeof(rom_path) - 1);
    rom_path[sizeof(rom_path) - 1] = 0;

    /* Ensure PROGDIR:States/ exists */
    {
        BPTR lock = Lock("PROGDIR:States", ACCESS_READ);
        if (lock)
            UnLock(lock);
        else
        {
            BPTR newdir = CreateDir("PROGDIR:States");
            if (newdir) UnLock(newdir);
        }
    }

    /* Build paths: PROGDIR:States/<romname>.srm / .gsx */
    sprintf(sram_path, "PROGDIR:States/%s.srm", name_buf);
    sprintf(state_path, "PROGDIR:States/%s.gsx", name_buf);
}

/****************************************************************************
 * Set on-screen message
 ****************************************************************************/
static void osd_set_message(const char *msg)
{
    video_cgx_draw_text(msg, 8, 8);
}

/****************************************************************************
 * SRAM battery backup - load from disk
 ****************************************************************************/
static void sram_autoload(void)
{
    BPTR file;
    int size;

    if (!sram.on)
        return;

    file = Open(sram_path, MODE_OLDFILE);
    if (!file)
        return;

    /* Get file size */
    Seek(file, 0, OFFSET_END);
    size = Seek(file, 0, OFFSET_BEGINNING);

    if (size > 0 && size <= (int)sizeof(sram.sram))
    {
        if (Read(file, sram.sram, size) != size)
        {
            /* Short read: sram.sram[] now holds a mix of file data and the
             * 0xFF fill from sram_init(). Block autosave so the damaged
             * contents are not written back over the player's save. */
            sram_load_failed = 1;
            printf("SRAM: Read error on %s - saving disabled this session\n",
                   sram_path);
        }
        else
        {
            printf("SRAM: Loaded %d bytes from %s\n", size, sram_path);
        }
    }
    Close(file);
}

/****************************************************************************
 * SRAM battery backup - save to disk
 ****************************************************************************/
static void sram_autosave(void)
{
    BPTR file;
    int size;
    unsigned long crc;

    if (!sram.on)
        return;

    /* A failed load left the buffer partly filled - writing it back would
     * destroy whatever is still intact in the file. */
    if (sram_load_failed)
        return;

    /* Compare CRC to detect if SRAM was modified since init/last save */
    crc = crc32(0, sram.sram, 0x10000);
    if (crc == sram.crc)
    {
        /* SRAM hasn't changed, skip write */
        return;
    }

    /* Save size = up to the last byte that is not 0xFF (the fill used by
     * sram_init()); sram_autoload() leaves the rest of sram.sram[] at 0xFF.
     * Same rule as the upstream libretro core.
     *
     * The mapped range (sram.end - sram.start + 1) can NOT be used:
     *   - SMS carts: sms_cart.c only sets sram.on, start = end = 0 -> 1 byte
     *   - EEPROM carts declare a 1-2 byte range, but their data lives
     *     anywhere in sram.sram[] (eeprom_i2c.c indexes up to 0xFFFF)
     *   - odd-start SRAM (0x200001-0x203FFF) holds data up to offset
     *     end & 0xFFFF, one byte past end - start + 1
     * All-0xFF contents: write the full 64K so the restore is exact even
     * for the 0x00-filled Sonic 1 Remastered case. */
    for (size = 0x10000; size > 0; size--)
    {
        if (sram.sram[size - 1] != 0xFF)
            break;
    }
    if (size == 0)
        size = 0x10000;

    file = Open(sram_path, MODE_NEWFILE);
    if (!file)
    {
        printf("SRAM: Error saving to %s\n", sram_path);
        return;
    }

    if (Write(file, sram.sram, size) != size)
    {
        Close(file);
        printf("SRAM: Write error on %s\n", sram_path);
        return;  /* keep old CRC so the next save retries */
    }
    Close(file);

    /* Update CRC to avoid re-saving unchanged data */
    sram.crc = crc;

    printf("SRAM: Saved %d bytes to %s\n", size, sram_path);
}

/****************************************************************************
 * Save state to disk (F5)
 ****************************************************************************/
static void do_save_state(void)
{
    BPTR file;
    int size;

    if (!state_buf)
    {
        osd_set_message("Save failed: no buffer");
        return;
    }

    /* Save emulator state to buffer */
    size = state_save(state_buf);
    if (size <= 0)
    {
        osd_set_message("Save failed: state_save error");
        return;
    }

    /* Write to disk */
    file = Open(state_path, MODE_NEWFILE);
    if (!file)
    {
        printf("State: Error creating %s\n", state_path);
        osd_set_message("Save failed: disk error");
        return;
    }

    if (Write(file, state_buf, size) != size)
    {
        Close(file);
        printf("State: Write error on %s\n", state_path);
        osd_set_message("Save failed: write error");
        return;
    }
    Close(file);

    printf("State: Saved %d bytes to %s\n", size, state_path);
    osd_set_message("State saved");
}

/****************************************************************************
 * Load state from disk (F8)
 ****************************************************************************/
static void do_load_state(void)
{
    BPTR file;
    int size;
    int result;

    if (!state_buf)
    {
        osd_set_message("Load failed: no buffer");
        return;
    }

    file = Open(state_path, MODE_OLDFILE);
    if (!file)
    {
        osd_set_message("No save state found");
        return;
    }

    /* Get file size */
    Seek(file, 0, OFFSET_END);
    size = Seek(file, 0, OFFSET_BEGINNING);

    if (size <= 0 || size > STATE_SIZE)
    {
        Close(file);
        osd_set_message("Load failed: invalid file");
        return;
    }

    /* Read state data. On a short read state_load() is not called, so the
     * running emulation is left untouched. */
    memset(state_buf, 0, STATE_SIZE);
    if (Read(file, state_buf, size) != size)
    {
        Close(file);
        printf("State: Read error on %s\n", state_path);
        osd_set_message("Load failed: read error");
        return;
    }
    Close(file);

    /* Restore emulator state */
    result = state_load(state_buf);
    if (result <= 0)
    {
        osd_set_message("Load failed: incompatible state");
        printf("State: Load failed (version mismatch?)\n");
        return;
    }

    printf("State: Loaded %d bytes from %s\n", size, state_path);
    osd_set_message("State loaded");
}

/****************************************************************************
 * Configuration file support
 * Loads PROGDIR:GenesisPlusGX.cfg (key=value format)
 * Command line arguments override config file settings
 ****************************************************************************/
typedef struct
{
    int scanlines;
    int vsync;
    int audio_mode;     /* 0=paula, 1=14bit, 2=ahi, 3=off */
    int ahi_freq;
    int paula_freq;
    int frameskip;
    int fps;
    int fmvol;          /* FM preamp percentage (10-100, default 60, clip point ~65) */
    int psgvol;         /* PSG preamp percentage (10-200, default 60) */
    int paulafilter;    /* Hardware Paula low-pass filter: 0=off, 1=on */
    int paulaboost;     /* Paula 8-bit volume boost: 0-4 (0=1x, 1=2x, 2=4x, 3=8x, 4=16x) */
    int paulastereo;    /* Paula stereo output: 0=mono, 1=stereo */
    int ahiboost;       /* AHI 16-bit volume boost: 0-4 (0=1x, 1=2x, 2=4x, 3=8x, 4=16x) */
    int ahimargin;      /* Obsolete, ignored: buffers are exact-length and the */
    int paulamargin;    /* queue depth is rate-controlled (2026-10). Still parsed. */
    int autoskip;       /* Dynamic frame skip: 1=auto (skip render when behind), 0=off */
    int hqfm;           /* FM -> blip flush: 1=band-limited (default), 0=linear (~1% of a Pi 3 frame) */
    int hqpsg;          /* PSG: 1=band-limited (default), 0=linear (no measurable gain on a Pi 3) */
    int screendepth;    /* RTG screen depth: 32 (default) or 16 (RGB565, half the blit bandwidth) */
    int glout;          /* 1 = GPU output via PiStorm3D minigl.library (GL builds only) */
    int gleffect;       /* GPU output effect: 0=sharp 1=smooth 2=scanlines 3=composite */
    int glwidth;        /* GPU output screen size */
    int glheight;
    int cheats;         /* Apply PROGDIR:Cheats/<rom>.cht: 1=yes (default), 0=ignore the file */
    /* Player 1 button key mappings (Amiga rawkey codes, -1 = use default) */
    int p1_a;
    int p1_b;
    int p1_c;
    int p1_x;
    int p1_y;
    int p1_z;
    int p1_start;
    int p1_mode;
    /* Player 2 button key mappings */
    int p2_a;
    int p2_b;
    int p2_c;
    int p2_x;
    int p2_y;
    int p2_z;
    int p2_start;
    int p2_mode;
} config_file_t;

#define AUDIO_MODE_PAULA  0
#define AUDIO_MODE_14BIT  1
#define AUDIO_MODE_AHI    2
#define AUDIO_MODE_OFF    3

static void config_set_defaults(config_file_t *cfg)
{
    cfg->scanlines = 0;
    cfg->vsync = 0;
    cfg->audio_mode = AUDIO_MODE_PAULA;
    cfg->ahi_freq = 44100;
    cfg->paula_freq = 41243;  /* period 86, ~41243 Hz: recommended for CM4+RTG (Nyquist 20621 Hz) */
                               /* Use 0 (period 128, ~27710 Hz) only on Pi 3 / slow hardware */
    cfg->frameskip = 0;       /* 0 = off (use autoskip instead) */
    cfg->fps = 1;
    cfg->fmvol = 60;  /* FM clips blip_buf above ~65; keep below that */
    cfg->psgvol = 60; /* Combined FM+PSG: 60+60=120% peak. Rarely both at max simultaneously.
                       * Lower from 75 to 60 to reduce combined clipping on heavy scenes. */
    cfg->paulafilter = 0;
    cfg->paulaboost = 1;  /* 1=2x boost: Sonic 1 peaks ~34% of 16-bit range -> ~68% of 8-bit */
                          /* 0=no boost leaves audio at only ~34% of 8-bit range (poor SNR) */
    cfg->paulastereo = 1;
    cfg->ahiboost = 0;    /* 0=no boost: with fmvol=60 16-bit signal is already at good level */
    cfg->ahimargin = 0;
    cfg->paulamargin = 0;
    cfg->autoskip = 1;        /* Dynamic frame skip on by default */
    cfg->hqfm = 1;            /* Band-limited FM resampling (0 saves ~0.2 ms/frame on a Pi 3) */
    cfg->hqpsg = 1;           /* Band-limited PSG */
    cfg->screendepth = 32;    /* 32-bit BGRA (VideoCore fast path); 16 halves blit bandwidth */
    cfg->glout = 0;           /* GPU output off by default */
    cfg->gleffect = 1;        /* smooth */
    cfg->glwidth = 640;
    cfg->glheight = 480;
    cfg->cheats = 1;          /* Apply the .cht file when one exists */
    /* -1 means use built-in default key */
    cfg->p1_a = -1; cfg->p1_b = -1; cfg->p1_c = -1;
    cfg->p1_x = -1; cfg->p1_y = -1; cfg->p1_z = -1;
    cfg->p1_start = -1; cfg->p1_mode = -1;
    cfg->p2_a = -1; cfg->p2_b = -1; cfg->p2_c = -1;
    cfg->p2_x = -1; cfg->p2_y = -1; cfg->p2_z = -1;
    cfg->p2_start = -1; cfg->p2_mode = -1;
}

/****************************************************************************
 * Key name to Amiga rawkey code lookup
 * Supports common key names for config file button mapping
 ****************************************************************************/
typedef struct {
    const char *name;
    int rawkey;
} keyname_entry_t;

static const keyname_entry_t keyname_table[] = {
    /* Letters */
    {"a", 0x20}, {"b", 0x35}, {"c", 0x33}, {"d", 0x22}, {"e", 0x12},
    {"f", 0x23}, {"g", 0x24}, {"h", 0x25}, {"i", 0x17}, {"j", 0x26},
    {"k", 0x27}, {"l", 0x28}, {"m", 0x37}, {"n", 0x36}, {"o", 0x18},
    {"p", 0x19}, {"q", 0x10}, {"r", 0x13}, {"s", 0x21}, {"t", 0x14},
    {"u", 0x16}, {"v", 0x34}, {"w", 0x11}, {"x", 0x32}, {"y", 0x15},
    {"z", 0x31},
    /* Number row */
    {"1", 0x01}, {"2", 0x02}, {"3", 0x03}, {"4", 0x04}, {"5", 0x05},
    {"6", 0x06}, {"7", 0x07}, {"8", 0x08}, {"9", 0x09}, {"0", 0x0A},
    /* Special keys */
    {"space", 0x40}, {"enter", 0x44}, {"return", 0x44},
    {"backspace", 0x41}, {"tab", 0x42}, {"del", 0x46},
    {"lshift", 0x60}, {"rshift", 0x61}, {"lalt", 0x64}, {"ralt", 0x65},
    {"lctrl", 0x63}, {"ctrl", 0x63}, {"lamiga", 0x66}, {"ramiga", 0x67},
    {"capslock", 0x62}, {"help", 0x5F},
    /* Punctuation */
    {"comma", 0x38}, {",", 0x38},
    {"period", 0x39}, {".", 0x39},
    {"slash", 0x3A}, {"/", 0x3A},
    {"semicolon", 0x29}, {";", 0x29},
    {"quote", 0x2A}, {"'", 0x2A},
    {"backslash", 0x0D}, {"\\", 0x0D},
    {"lbracket", 0x1A}, {"[", 0x1A},
    {"rbracket", 0x1B}, {"]", 0x1B},
    {"minus", 0x0B}, {"-", 0x0B},
    {"equals", 0x0C}, {"=", 0x0C},
    {"backtick", 0x00}, {"`", 0x00},
    /* Function keys */
    {"f1", 0x50}, {"f2", 0x51}, {"f3", 0x52}, {"f4", 0x53}, {"f5", 0x54},
    {"f6", 0x55}, {"f7", 0x56}, {"f8", 0x57}, {"f9", 0x58}, {"f10", 0x59},
    /* Numpad */
    {"np0", 0x0F}, {"np1", 0x1D}, {"np2", 0x1E}, {"np3", 0x1F},
    {"np4", 0x2D}, {"np5", 0x2E}, {"np6", 0x2F},
    {"np7", 0x3D}, {"np8", 0x3E}, {"np9", 0x3F},
    {"npdot", 0x3C}, {"npenter", 0x43},
    {"npminus", 0x4A}, {"npplus", 0x5E},
    {"npmul", 0x5D}, {"npdiv", 0x5C},
    /* Arrow keys (for reference, not remappable as buttons) */
    {"up", 0x4C}, {"down", 0x4D}, {"left", 0x4F}, {"right", 0x4E},
    {NULL, -1}
};

static int keyname_to_rawkey(const char *name)
{
    int i;
    char lower[32];
    int len;

    /* Convert to lowercase for case-insensitive matching */
    len = strlen(name);
    if (len >= 32) len = 31;
    for (i = 0; i < len; i++)
        lower[i] = (name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i];
    lower[len] = '\0';

    /* Check for hex rawkey code (e.g., "0x20") */
    if (lower[0] == '0' && lower[1] == 'x')
    {
        return (int)strtol(lower, NULL, 16);
    }

    /* Look up by name */
    for (i = 0; keyname_table[i].name != NULL; i++)
    {
        if (strcmp(lower, keyname_table[i].name) == 0)
            return keyname_table[i].rawkey;
    }

    return -1;  /* Not found */
}

static void config_load(config_file_t *cfg)
{
    BPTR file;
    char line[256];
    char key[64];
    char value[64];
    int pos, ch;

    file = Open("PROGDIR:GenesisPlusGX.cfg", MODE_OLDFILE);
    if (!file)
        return;

    /* Read line by line */
    while (1)
    {
        /* Read one line */
        pos = 0;
        while (pos < (int)sizeof(line) - 1)
        {
            if (Read(file, &line[pos], 1) != 1)
            {
                if (pos == 0)
                    goto done;
                break;
            }
            ch = line[pos];
            if (ch == '\n' || ch == '\r')
                break;
            pos++;
        }
        line[pos] = 0;

        /* Skip empty lines and comments */
        if (line[0] == 0 || line[0] == '#' || line[0] == ';')
            continue;

        /* Parse key=value (whitespace around key and value is trimmed) */
        key[0] = 0;
        value[0] = 0;
        {
            char *eq = strchr(line, '=');
            char *src;
            int len;
            if (!eq)
                continue;
            *eq = 0;

            src = line;
            while (*src == ' ' || *src == '\t') src++;
            strncpy(key, src, sizeof(key) - 1);
            key[sizeof(key) - 1] = 0;
            len = strlen(key);
            while (len > 0 && (key[len-1] == ' ' || key[len-1] == '\t'))
                key[--len] = 0;

            src = eq + 1;
            while (*src == ' ' || *src == '\t') src++;
            strncpy(value, src, sizeof(value) - 1);
            value[sizeof(value) - 1] = 0;
            len = strlen(value);
            while (len > 0 && (value[len-1] == ' ' || value[len-1] == '\t'))
                value[--len] = 0;
        }

        /* Match keys */
        if (strcmp(key, "scanlines") == 0)
        {
            cfg->scanlines = atoi(value);
            if (cfg->scanlines < 0) cfg->scanlines = 0;
            if (cfg->scanlines > 3) cfg->scanlines = 3;
        }
        else if (strcmp(key, "vsync") == 0)
        {
            cfg->vsync = atoi(value);
        }
        else if (strcmp(key, "audio") == 0)
        {
            if (strcmp(value, "paula") == 0)
                cfg->audio_mode = AUDIO_MODE_PAULA;
            else if (strcmp(value, "14bit") == 0)
                cfg->audio_mode = AUDIO_MODE_14BIT;
            else if (strcmp(value, "ahi") == 0)
                cfg->audio_mode = AUDIO_MODE_AHI;
            else if (strcmp(value, "off") == 0)
                cfg->audio_mode = AUDIO_MODE_OFF;
        }
        else if (strcmp(key, "ahifreq") == 0)
        {
            /* audio_update() writes one frame of samples into the static
             * audio_buffer (AUDIO_SAMPLES = 1024 stereo samples). Slowest
             * frame rate is PAL 49.70 Hz: 48000 Hz -> 966 samples/frame,
             * which fits; 96000 would be ~1931 and overrun the buffer.
             * The launcher writes this key from a free-form string. */
            cfg->ahi_freq = atoi(value);
            if (cfg->ahi_freq < 8000 || cfg->ahi_freq > 48000)
            {
                printf("Warning: ahifreq=%d out of range (8000-48000), using 44100\n",
                       cfg->ahi_freq);
                cfg->ahi_freq = 44100;
            }
        }
        else if (strcmp(key, "paulafreq") == 0)
        {
            cfg->paula_freq = atoi(value);
        }
        else if (strcmp(key, "frameskip") == 0)
        {
            cfg->frameskip = atoi(value);
            if (cfg->frameskip < 0) cfg->frameskip = 0;
            if (cfg->frameskip > 5) cfg->frameskip = 5;
        }
        else if (strcmp(key, "autoskip") == 0)
        {
            cfg->autoskip = atoi(value) ? 1 : 0;
        }
        else if (strcmp(key, "hqfm") == 0)
        {
            cfg->hqfm = atoi(value) ? 1 : 0;
        }
        else if (strcmp(key, "hqpsg") == 0)
        {
            cfg->hqpsg = atoi(value) ? 1 : 0;
        }
        else if (strcmp(key, "screendepth") == 0)
        {
            cfg->screendepth = atoi(value);
            if (cfg->screendepth != 16 && cfg->screendepth != 32)
                cfg->screendepth = 32;
        }
        else if (strcmp(key, "glout") == 0)
        {
            cfg->glout = atoi(value) ? 1 : 0;
        }
        else if (strcmp(key, "gleffect") == 0)
        {
            cfg->gleffect = atoi(value);
            if (cfg->gleffect < 0 || cfg->gleffect > 3)
                cfg->gleffect = 1;
        }
        else if (strcmp(key, "glsize") == 0)
        {
            int w = 0, h = 0;
            if (sscanf(value, "%dx%d", &w, &h) == 2 &&
                w >= 320 && h >= 224 && w <= 4096 && h <= 4096)
            {
                cfg->glwidth = w;
                cfg->glheight = h;
            }
        }
        else if (strcmp(key, "fps") == 0)
        {
            cfg->fps = atoi(value);
        }
        else if (strcmp(key, "fmvol") == 0)
        {
            cfg->fmvol = atoi(value);
            if (cfg->fmvol < 10) cfg->fmvol = 10;
            if (cfg->fmvol > 100) cfg->fmvol = 100;
        }
        else if (strcmp(key, "psgvol") == 0)
        {
            cfg->psgvol = atoi(value);
            if (cfg->psgvol < 10) cfg->psgvol = 10;
            if (cfg->psgvol > 200) cfg->psgvol = 200;
        }
        else if (strcmp(key, "paulafilter") == 0)
        {
            cfg->paulafilter = atoi(value) ? 1 : 0;
        }
        else if (strcmp(key, "cheats") == 0)
        {
            cfg->cheats = atoi(value) ? 1 : 0;
        }
        else if (strcmp(key, "paulaboost") == 0)
        {
            cfg->paulaboost = atoi(value);
            if (cfg->paulaboost < 0) cfg->paulaboost = 0;
            if (cfg->paulaboost > 4) cfg->paulaboost = 4;
        }
        else if (strcmp(key, "paulastereo") == 0)
        {
            cfg->paulastereo = atoi(value) ? 1 : 0;
        }
        else if (strcmp(key, "ahiboost") == 0)
        {
            cfg->ahiboost = atoi(value);
            if (cfg->ahiboost < 0) cfg->ahiboost = 0;
            if (cfg->ahiboost > 4) cfg->ahiboost = 4;
        }
        else if (strcmp(key, "ahimargin") == 0)
        {
            cfg->ahimargin = atoi(value);
            if (cfg->ahimargin < 0)   cfg->ahimargin = 0;
            if (cfg->ahimargin > 882) cfg->ahimargin = 882;
        }
        else if (strcmp(key, "paulamargin") == 0)
        {
            cfg->paulamargin = atoi(value);
            if (cfg->paulamargin < 0)   cfg->paulamargin = 0;
            if (cfg->paulamargin > 882) cfg->paulamargin = 882;
        }
        /* Player 1 button mappings */
        else if (strcmp(key, "p1_a") == 0)     { cfg->p1_a = keyname_to_rawkey(value); }
        else if (strcmp(key, "p1_b") == 0)     { cfg->p1_b = keyname_to_rawkey(value); }
        else if (strcmp(key, "p1_c") == 0)     { cfg->p1_c = keyname_to_rawkey(value); }
        else if (strcmp(key, "p1_x") == 0)     { cfg->p1_x = keyname_to_rawkey(value); }
        else if (strcmp(key, "p1_y") == 0)     { cfg->p1_y = keyname_to_rawkey(value); }
        else if (strcmp(key, "p1_z") == 0)     { cfg->p1_z = keyname_to_rawkey(value); }
        else if (strcmp(key, "p1_start") == 0) { cfg->p1_start = keyname_to_rawkey(value); }
        else if (strcmp(key, "p1_mode") == 0)  { cfg->p1_mode = keyname_to_rawkey(value); }
        /* Player 2 button mappings */
        else if (strcmp(key, "p2_a") == 0)     { cfg->p2_a = keyname_to_rawkey(value); }
        else if (strcmp(key, "p2_b") == 0)     { cfg->p2_b = keyname_to_rawkey(value); }
        else if (strcmp(key, "p2_c") == 0)     { cfg->p2_c = keyname_to_rawkey(value); }
        else if (strcmp(key, "p2_x") == 0)     { cfg->p2_x = keyname_to_rawkey(value); }
        else if (strcmp(key, "p2_y") == 0)     { cfg->p2_y = keyname_to_rawkey(value); }
        else if (strcmp(key, "p2_z") == 0)     { cfg->p2_z = keyname_to_rawkey(value); }
        else if (strcmp(key, "p2_start") == 0) { cfg->p2_start = keyname_to_rawkey(value); }
        else if (strcmp(key, "p2_mode") == 0)  { cfg->p2_mode = keyname_to_rawkey(value); }
        else
        {
            /* Unknown key - ignore */
        }
    }

done:
    Close(file);
}

static void config_save_default(void)
{
    BPTR file;

    /* Only create if it doesn't exist */
    file = Open("PROGDIR:GenesisPlusGX.cfg", MODE_OLDFILE);
    if (file)
    {
        Close(file);
        return;
    }

    file = Open("PROGDIR:GenesisPlusGX.cfg", MODE_NEWFILE);
    if (!file)
        return;

    {
        const char *cfg =
            "; CaffeineOS GenesisPlusGX v" GPGX_VERSION " (" GPGX_DATE ")\n"
            ";\n"
            "; audio: paula (8bit stereo), 14bit (14bit stereo), ahi, off\n"
            "; 14bit gives better dynamic range (~13 bit effective) on the same Paula hardware\n"
            "audio=paula\n"
            "; paulafreq: paula/14bit sample rate. 0=auto (period 128, ~27710 Hz PAL / ~27965 Hz NTSC)\n"
            ";   Rates above ~28 kHz need a 31 kHz native display; on a 15 kHz one\n"
            ";   (e.g. A500) the emulator falls back to 27710 by itself.\n"
            "; IMPORTANT: 27710 Hz is low quality - FM harmonics above 13855 Hz alias as noise.\n"
            "; On A1200+PiStorm CM4 with RTG (no bitplane DMA), always use 41243 Hz:\n"
            ";   Pi 3 (slower):  0      (period 128, ~27710 Hz - avoids CPU overload)\n"
            ";   Pi CM4 (fast):  41243  (period 86,  ~41243 Hz - RECOMMENDED, Nyquist 20621 Hz)\n"
            "paulafreq=41243\n"
            "ahifreq=44100\n"
            "; hqfm/hqpsg: 1=band-limited synthesis (best quality),\n"
            ";   0=linear interpolation (hqfm=0 saves about 1% of the frame on a Pi 3)\n"
            "hqfm=1\n"
            "hqpsg=1\n"
            "; screendepth: 32 (default, fastest on PiStorm/VideoCore) or 16\n"
            ";   16 = RGB565 screen: half the blit bandwidth. Recommended for\n"
            ";   classic Amigas with Zorro RTG cards (Picasso II etc).\n"
            ";   Falls back to 32 if no 16-bit RTG mode exists.\n"
            "screendepth=32\n"
            "; glout: 1 = GPU output through PiStorm3D minigl.library 29.1\n"
            ";   (Pi 4/CM4 only). Better scaling and effects, but costs ~6 ms\n"
            ";   of CPU per drawn frame. Forces vsync off. Falls back to the\n"
            ";   normal display when the library is missing.\n"
            "glout=0\n"
            "; gleffect: 0=sharp 1=smooth 2=scanlines 3=composite blend\n"
            "gleffect=1\n"
            "; glsize: GPU output screen size, e.g. 640x480 1280x720 1920x1080\n"
            "glsize=640x480\n"
            "scanlines=0\n"
            "; scanlines: 0=off (fastest), 1=subtle, 2=medium, 3=strong\n"
            ";   NOTE: scanlines>0 opens a 2x screen - 4x the pixel writes per frame\n"
            "vsync=0\n"
            "frameskip=0\n"
            "; frameskip: 0=off/auto, 1=skip 1 of 2, 2=skip 2 of 3, 3=skip 3 of 4\n"
            ";   Use autoskip=1 instead for dynamic skip (better quality)\n"
            "autoskip=1\n"
            "; autoskip: 1=dynamic frame skip (skip drawing when over half a frame behind)\n"
            ";   Overrides fixed frameskip when frameskip=0. Best for Pi 3/slow hardware.\n"
            "fps=1\n"
            "; fmvol/psgvol: FM and PSG preamp (10-100/200).\n"
            "; FM clips blip_buf above ~65. PSG safe up to 200 alone.\n"
            "; Combined peak = fmvol+psgvol but FM and PSG rarely both at max simultaneously.\n"
            "; Lower fmvol if you hear distortion on loud games.\n"
            "fmvol=60\n"
            "psgvol=60\n"
            "; paulafilter: 1=force the Amiga audio (LED) filter on (~7 kHz low-pass),\n"
            ";   0=leave it as the system has it.\n"
            "paulafilter=0\n"
            "paulaboost=1\n"
            "paulastereo=1\n"
            "ahiboost=0\n"
            "; paulamargin/ahimargin: obsolete and ignored. Audio buffers now hold\n"
            ";   exactly one frame and the queue depth is rate-controlled.\n"
            "\n"
            "; cheats: 1=apply PROGDIR:Cheats/<romname>.cht when one exists, 0=ignore it.\n"
            ";   Mega Drive / Genesis only. Individual codes are switched with the\n"
            ";   cheatN_enable lines inside the .cht file itself.\n"
            "cheats=1\n"
            "\n"
            "; Button mapping (uncomment and edit to customize)\n"
            "; Key names: a-z, 0-9, space, enter, lshift, rshift,\n"
            ";   lalt, ralt, lctrl, comma, period, slash, semicolon,\n"
            ";   lbracket, rbracket, minus, equals, backslash,\n"
            ";   f1-f10, np0-np9, npdot, npenter, npminus, npplus\n"
            ";   Or use hex rawkey: 0x20\n"
            ";\n"
            "; Player 1 defaults: A=1, B=2, C=3, X=q, Y=w, Z=e, Start=space\n"
            ";p1_a=1\n"
            ";p1_b=2\n"
            ";p1_c=3\n"
            ";p1_x=q\n"
            ";p1_y=w\n"
            ";p1_z=e\n"
            ";p1_start=space\n"
            ";p1_mode=tab\n"
            ";\n"
            "; Player 2 defaults: A=np0, B=npdot, C=npenter, Start=help\n"
            ";p2_a=np0\n"
            ";p2_b=npdot\n"
            ";p2_c=npenter\n"
            ";p2_start=help\n";
        Write(file, (APTR)cfg, strlen(cfg));
    }
    Close(file);
}

/****************************************************************************
 * Main entry point
 ****************************************************************************/
int main(int argc, char *argv[])
{
    char *romfile = NULL;
    int frame_time_us;
    int audio_samples;
    struct Window *win;
    int i;
    int scanlines = 0;
    int use_vsync = 0;
#ifdef USE_MINIGL
    int use_gl = 0;           /* -gl: draw through PiStorm3D / minigl.library */
    int gl_effect = VGL_FX_SMOOTH;
    int gl_rgba = 0;          /* -glrgba: upload RGBA8 instead of RGB565 */
    int gl_w = 640, gl_h = 480;
    int gl_ring = 1;          /* -glring=N test switch: frame textures rotated */
    int gl_half = 0;          /* -glhalf test switch: upload halves alternately */
    int gl_active = 0;        /* GL output really running */
#endif
    int use_ahi = 0;
    int ahi_freq = 22050;     /* AHI core frequency - keep low for speed */
    int use_14bit = 0;
    int paula_freq = 0;      /* 0 = use optimal (27710Hz period 128) */
    int frameskip = 0;      /* 0=none, 1=skip every 2nd, 2=skip 2 of 3, etc */
    int use_autoskip = 0;   /* Dynamic frame skip: skip render when behind schedule */
    int nosound = 0;        /* Disable audio for max speed */
    int show_fps_counter = 0;  /* Show FPS in top-right */
    int use_paula_filter = 0;  /* Paula hardware low-pass filter */
    int use_cheats = 1;        /* Apply the .cht file for this ROM */
    config_file_t cfg;
    
    printf("\n");
    printf("Genesis Plus GX v%s for AmigaOS/CaffeineOS\n", GPGX_VERSION);
    printf("M68k conversion by %s (%s)\n\n", GPGX_AUTHOR, GPGX_DATE);
    
    /* Load config file (creates default if missing) */
    config_set_defaults(&cfg);
    config_save_default();
    config_load(&cfg);

    /* Apply config as initial values (command line will override) */
    scanlines = cfg.scanlines;
    use_vsync = cfg.vsync;
    ahi_freq = cfg.ahi_freq;
    paula_freq = cfg.paula_freq;
    frameskip = cfg.frameskip;
    use_autoskip = cfg.autoskip;
    /* vsync: ChangeScreenBuffer blocks on VBL - that IS the frame clock.
     * autoskip would see elapsed>=budget on every frame and skip half of them. */
    if (use_vsync) use_autoskip = 0;
    show_fps_counter = cfg.fps;
#ifdef USE_MINIGL
    use_gl = cfg.glout;
    gl_effect = cfg.gleffect;
    gl_w = cfg.glwidth;
    gl_h = cfg.glheight;
#endif
    switch (cfg.audio_mode)
    {
        case AUDIO_MODE_14BIT: use_14bit = 1; break;
        case AUDIO_MODE_AHI:   use_ahi = 1; break;
        case AUDIO_MODE_OFF:   nosound = 1; break;
        default: break;
    }
    use_paula_filter = cfg.paulafilter;
    use_cheats = cfg.cheats;
    /* Note: fm_preamp and psg_preamp are applied after config_default()
     * to avoid being overwritten by the global config reset */

    /* Check arguments */
    if (argc < 2)
    {
        printf("Usage: %s <romfile> [options]\n", argv[0]);
#ifdef USE_ZIP
        printf("\nSupported formats: .bin .gen .md .smd .rom .sms .gg .sg, or a .zip holding one\n");
#else
        printf("\nSupported formats: .bin .gen .md .smd .rom .sms .gg .sg\n");
#endif
        printf("\nVideo options:\n");
        printf("  -scanlines    Enable scanlines (medium, 2x mode)\n");
        printf("  -scanlines=N  Scanline level 1-3 (1=subtle, 2=medium, 3=strong)\n");
        printf("  -vsync        Enable VSync (reduces tearing, may drop to 30fps)\n");
        printf("  -screendepth=N  RTG screen depth: 32 (default) or 16 (Zorro RTG cards)\n");
        printf("\nAudio options:\n");
        printf("  -14bit        Use 14-bit Paula stereo (higher quality, 4 channels)\n");
        printf("  -paulafreq=N  Paula sample rate in Hz (default 41243; 0 = 27710)\n");
        printf("                Lower = less CPU. E.g. 22050, 16000, 13000\n");
        printf("  -ahi          Use AHI audio (for sound cards)\n");
        printf("  -ahifreq=N    AHI sample rate: 22050, 27710, 32000, 44100 (config default: 44100)\n");
        printf("  -nosound      Disable audio (faster on slow machines)\n");
        printf("  -paulafilter  Force the Amiga audio (LED) filter on (~7 kHz low-pass)\n");
        printf("\nCheat options (Mega Drive only, PROGDIR:Cheats/<romname>.cht):\n");
        printf("  -cheats       Apply the cheat file (default)\n");
        printf("  -nocheats     Ignore the cheat file for this run\n");
        printf("\nPerformance options:\n");
        printf("  -frameskip=N  Skip N frames (1=50%%, 2=33%%, 3=25%% render)\n");
        printf("  -autoskip     Skip drawing when behind schedule (default)\n");
        printf("  -noautoskip   Never skip frames automatically\n");
        printf("  -fps          Show FPS counter in top-right corner\n");
        printf("  -nodrc        Virtua Racing (SVP): interpreter, not the recompiler\n");
#ifdef USE_MINIGL
        printf("\nGL output (PiStorm3D / minigl.library 29, Pi 4/CM4 only):\n");
        printf("  -gl           Draw through the V3D GPU (falls back if unavailable)\n");
        printf("  -nogl         Normal display even if glout=1 in the config\n");
        printf("  -gleffect=N   0=sharp 1=smooth 2=scanlines 3=composite blend\n");
        printf("  -glsize=WxH   GL screen size (default 640x480)\n");
        printf("  -glrgba       Upload RGBA8 instead of RGB565 (if colours look wrong)\n");
#endif
        printf("\nDefault audio: 8-bit Paula stereo, 41243 Hz (see the config file)\n");
        printf("\nConfig file: PROGDIR:GenesisPlusGX.cfg (auto-created on first run)\n");
        printf("Command line options override config file settings.\n");
        return 5;
    }
    
    romfile = argv[1];
    
    /* Parse optional arguments */
    for (i = 2; i < argc; i++)
    {
        if (strcmp(argv[i], "-scanlines") == 0)
        {
            scanlines = 2;  /* Medium by default */
        }
        else if (strncmp(argv[i], "-scanlines=", 11) == 0)
        {
            scanlines = atoi(argv[i] + 11);
            if (scanlines < 1) scanlines = 1;
            if (scanlines > 3) scanlines = 3;
        }
        else if (strcmp(argv[i], "-vsync") == 0)
        {
            use_vsync = 1;
            use_autoskip = 0;  /* VBL is frame clock; autoskip would falsely skip half frames */
        }
        else if (strncmp(argv[i], "-screendepth=", 13) == 0)
        {
            int depth = atoi(argv[i] + 13);
            if (depth == 16 || depth == 32)
                cfg.screendepth = depth;
            else
                printf("Ignoring %s (use -screendepth=16 or -screendepth=32)\n", argv[i]);
        }
        else if (strcmp(argv[i], "-ahi") == 0)
        {
            use_ahi = 1;
        }
        else if (strncmp(argv[i], "-ahifreq=", 9) == 0)
        {
            ahi_freq = atoi(argv[i] + 9);
            /* Validate frequency */
            if (ahi_freq != 22050 && ahi_freq != 27710 && 
                ahi_freq != 32000 && ahi_freq != 44100)
            {
                printf("Warning: Invalid AHI frequency %d, using 22050\n", ahi_freq);
                ahi_freq = 22050;
            }
        }
        else if (strcmp(argv[i], "-14bit") == 0)
        {
            use_14bit = 1;
        }
        else if (strncmp(argv[i], "-paulafreq=", 11) == 0)
        {
            paula_freq = atoi(argv[i] + 11);
            /* Validate: Paula can do roughly 3500 - 44100 Hz.
             * Higher rates (above ~28600) require RTG mode (no bitplane DMA).
             * Period 80 = ~44336 Hz PAL, period 128 = 27710 Hz PAL. */
            if (paula_freq < 3000 || paula_freq > 44100)
            {
                printf("Warning: Paula frequency %d out of range (3000-44100), using default\n",
                       paula_freq);
                paula_freq = 0;  /* 0 = let backend use optimal */
            }
        }
        else if (strcmp(argv[i], "-nosound") == 0)
        {
            nosound = 1;
        }
        else if (strcmp(argv[i], "-paulafilter") == 0)
        {
            use_paula_filter = 1;
        }
        else if (strcmp(argv[i], "-nocheats") == 0)
        {
            use_cheats = 0;
        }
        else if (strcmp(argv[i], "-cheats") == 0)
        {
            use_cheats = 1;
        }
        else if (strncmp(argv[i], "-frameskip=", 11) == 0)
        {
            frameskip = atoi(argv[i] + 11);
            if (frameskip < 0) frameskip = 0;
            if (frameskip > 5) frameskip = 5;
        }
        else if (strcmp(argv[i], "-autoskip") == 0)
        {
            use_autoskip = 1;
        }
        else if (strcmp(argv[i], "-noautoskip") == 0)
        {
            use_autoskip = 0;
        }
#ifdef GPGX_PROFILE
        else if (strcmp(argv[i], "-flushtest") == 0)
        {
            prof_flushtest = 1;   /* CacheClearU() once per frame */
        }
        else if (strcmp(argv[i], "-profsvp") == 0)
        {
            /* split the SVP time: translated blocks / interpreter / C */
            extern int prof_svp_split;
            prof_svp_split = 1;
        }
        else if (strncmp(argv[i], "-jitdepth=", 10) == 0)
        {
            /* Emu68 JIT test: at most N m68k instructions per translation
             * unit (1-256) while the emulation runs, then the old value */
            prof_jitdepth = atoi(argv[i] + 10);
            if (prof_jitdepth < 1 || prof_jitdepth > 256)
                prof_jitdepth = 0;
        }
#endif
        else if (strcmp(argv[i], "-nodrc") == 0)
        {
            ssp_drc_disable = 1;  /* SVP: interpreter instead of the recompiler */
        }
        else if (strcmp(argv[i], "-fps") == 0)
        {
            show_fps_counter = 1;
        }
#ifdef USE_MINIGL
        else if (strcmp(argv[i], "-gl") == 0)
        {
            use_gl = 1;
        }
        else if (strcmp(argv[i], "-nogl") == 0)
        {
            use_gl = 0;
        }
        else if (strncmp(argv[i], "-gleffect=", 10) == 0)
        {
            use_gl = 1;
            gl_effect = atoi(argv[i] + 10);
        }
        else if (strcmp(argv[i], "-glrgba") == 0)
        {
            use_gl = 1;
            gl_rgba = 1;
        }
        else if (strcmp(argv[i], "-glhalf") == 0)
        {
            use_gl = 1;
            gl_half = 1;
        }
        else if (strncmp(argv[i], "-glring=", 8) == 0)
        {
            use_gl = 1;
            gl_ring = atoi(argv[i] + 8);
            if (gl_ring < 1 || gl_ring > 3)
            {
                printf("GL: -glring takes 1 to 3, using 1\n");
                gl_ring = 1;
            }
        }
        else if (strncmp(argv[i], "-glsize=", 8) == 0)
        {
            int w = 0, h = 0;
            use_gl = 1;
            if (sscanf(argv[i] + 8, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 224)
            {
                gl_w = w;
                gl_h = h;
            }
            else
                printf("GL: ignoring %s (use e.g. -glsize=1280x720)\n", argv[i]);
        }
#endif
    }

#ifdef USE_MINIGL
    /* GL output draws its own picture into a single 32-bit screen bitmap:
     * no double-buffered VSync (the swap would change the bitmap under the
     * GL context) and no 16-bit screen. */
    if (use_gl && use_vsync)
    {
        printf("GL: VSync is not supported with GL output, disabled\n");
        use_vsync = 0;
    }
#endif
    
    /* Set scanline mode before video init */
    
    /* Set vsync mode */
    video_cgx_set_vsync(use_vsync);
    
    /* Open AmigaOS libraries */
    if (!open_libraries())
    {
        return 10;
    }
    
    /* Initialize timer */
    if (!timer_init())
    {
        printf("Warning: Timer not available, frame sync disabled\n");
    }
#ifdef GPGX_PROFILE
    prof_init();   /* Emu68 only: see amiga/prof.h */
#endif

    /* Initialize configuration */
    config_default();
    
    /* Apply user volume settings after config_default() which overwrites them.
     * FM+PSG share blip_buf; peaks add. FM peak at preamp=65 ~= 32767 (clip point).
     * PSG alone is safe up to preamp=200. User is responsible for avoiding clipping. */
    config.fm_preamp  = cfg.fmvol;
    config.psg_preamp = cfg.psgvol;

    /* Sound chip quality (applied after config_default() for the same reason).
     * hq_fm=0 switches the YM2612 -> blip flush from band-limited to linear
     * interpolation: 0.4 -> 0.2 ms of a 24 ms frame on a Pi 3 (2026-10-20);
     * the synthesis itself is unchanged. */
    config.hq_fm  = cfg.hqfm;
    config.hq_psg = cfg.hqpsg;

    /* Set scanline mode and screen depth before video init */
    video_cgx_set_scanlines(scanlines);
    video_cgx_set_depth(cfg.screendepth);
    
    /* Set up bitmap structure FIRST - before any emulator init */
    /* Use static buffer initially, will update pointer after video init */
    static uint8 temp_bitmap_buffer[320 * 240 * 2];  /* 16-bit, 320x240 */
    memset(&bitmap, 0, sizeof(bitmap));
    bitmap.width = VIDEO_WIDTH;
    bitmap.height = VIDEO_HEIGHT;
    bitmap.pitch = VIDEO_WIDTH * 2;  /* 16-bit = 2 bytes per pixel */
    bitmap.data = temp_bitmap_buffer;
    bitmap.viewport.w = 256;
    bitmap.viewport.h = 224;
    bitmap.viewport.x = 0;
    bitmap.viewport.y = 0;
    
    /* Load ROM */
    if (!load_rom(romfile))
    {
        printf("Error: Failed to load ROM: %s\n", romfile);
        timer_shutdown();
        close_libraries();
        return 10;
    }
    
    /* Get ROM info */
    {
        const char *sys_name;
        if (system_hw == SYSTEM_MD || system_hw == SYSTEM_PBC)
            sys_name = "Mega Drive/Genesis";
        else if (system_hw == SYSTEM_SMS || system_hw == SYSTEM_SMS2)
            sys_name = "Master System";
        else if (system_hw == SYSTEM_GG || system_hw == SYSTEM_GGMS)
            sys_name = "Game Gear";
        else if (system_hw == SYSTEM_SG || system_hw == SYSTEM_SGII || system_hw == SYSTEM_SGII_RAM_EXT)
            sys_name = "SG-1000";
        else
            sys_name = "Unknown";
        printf("System: %s (%lu bytes)\n", sys_name, (unsigned long)cart.romsize);
    }

    /* Auto-enable 2x scaling for Game Gear (160x144 is too small at 1x) */
    if (system_hw == SYSTEM_GG || system_hw == SYSTEM_GGMS)
    {
        video_cgx_set_force_2x(1);
    }
    
    /* Configure input system - must be done BEFORE system_init() */
    /* because system_init() -> io_init() -> input_init() reads these */
    input.system[0] = SYSTEM_GAMEPAD;  /* Port 1: gamepad */
    input.system[1] = SYSTEM_GAMEPAD;  /* Port 2: gamepad */
    
    /* Initialize Genesis Plus GX core AFTER loading ROM */
    system_init();
    
    /* Paula cannot fetch samples below period 124 on a 15 kHz native
     * display (HRM: one audio DMA slot per scan line and channel); only
     * the doubled 31 kHz modes allow it. The scan rate is read off the
     * vertical beam counter: a 15 kHz frame has 262/312 lines, a doubled
     * one about twice that (A500 + Pi 3: 262, A1200 + CM4: 525, measured
     * 2026-10-20 with amiga/bench/beamtest). Without this an A500 dropped
     * 627 audio frames in 90 s at 41243 Hz. */
    if (!use_ahi && paula_freq > 0 && GfxBase)
    {
        ULONG pclk = amiga_hw_pal ? 3546895UL : 3579545UL;
        if (pclk / (ULONG)paula_freq < 124)
        {
            LONG beam_max = 0, beam_last = -1, wraps = 0, i;
            for (i = 0; i < 60000 && wraps < 3; i++)
            {
                LONG p = VBeamPos();
                if (p > beam_max) beam_max = p;
                if (p < beam_last) wraps++;
                beam_last = p;
            }
            if (beam_max < 400)
            {
                printf("Audio: native display is 15 kHz (%ld lines per frame): Paula\n"
                       "       cannot fetch %d Hz, using 27710 Hz (paulafreq=0) instead.\n",
                       (long)beam_max, paula_freq);
                paula_freq = 0;
            }
        }
    }

    /* Initialize emulator audio */
    {
        /* Determine the actual core sample rate.
         * For Paula, pre-compute the actual rate the backend will achieve
         * (snapped to nearest valid period) so blip_buf resamples correctly. */
        int core_freq;
        if (use_ahi)
        {
            core_freq = ahi_freq;
        }
        else if (paula_freq > 0)
        {
            /* Pre-compute what the backend will snap to:
             * period = clock / freq, clamped 80..1024, made even
             * actual_freq = clock / period
             * IMPORTANT: Use the REAL Amiga hardware clock, not the
             * game's PAL/NTSC mode.  Paula's crystal is on the
             * motherboard - it doesn't change with the game. */
            ULONG pclk = amiga_hw_pal ? 3546895UL : 3579545UL;
            UWORD pp = (UWORD)(pclk / (ULONG)paula_freq);
            if (pp < 80) pp = 80;
            if (pp > 1024) pp = 1024;
            pp = (pp + 1) & ~1;
            core_freq = (int)(pclk / pp);
        }
        else
        {
            /* Default optimal: period 128 */
            ULONG pclk = amiga_hw_pal ? 3546895UL : 3579545UL;
            core_freq = (int)(pclk / 128);
        }
        /* Produce samples for 60/50 Hz frames - the rate the frame limiter
         * runs at. 0.0 would mean the exact console rate (59.92/49.70 Hz,
         * core/system.c audio_set_rate()), i.e. 0.13%/0.6% more audio than
         * real time and a queue that overflows. Pitch rises by the same
         * 0.13%/0.6% (~2/10 cents); game speed is unchanged, it already ran
         * at 60/50 Hz. */
        audio_init(core_freq, vdp_pal ? 50.0 : 60.0);
    }
    
    /* Reset system */
    system_reset();

    /* Load and apply cheats from PROGDIR:Cheats/<romname>.cht (enable = true only) */
    if (use_cheats)
        load_cheats_cht(romfile);
    else
        printf("Cheats: disabled\n");

    /* Calculate frame time */
    frame_time_us = vdp_pal ? 20000 : 16667;  /* 50Hz or 60Hz */
    
#ifdef USE_MINIGL
    if (use_gl)
    {
        video_cgx_set_screen_size(gl_w, gl_h);
        video_cgx_set_depth(32);
    }
#endif
    /* Initialize video (opens screen) */
    if (!video_cgx_init(VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_MODE_RTG16))
    {
        timer_shutdown();
        close_libraries();
        return 10;
    }
    
    /* Update bitmap to use video buffer */
    bitmap.pitch = video_cgx_get_pitch();
    bitmap.data = video_cgx_get_buffer();

#ifdef USE_MINIGL
    if (use_gl)
    {
        if (video_cgx_get_screen_bpp() != 4)
            printf("GL: needs a 32-bit screen - using the normal display\n");
        else
        {
            video_gl_set_experiments(gl_ring, gl_half);
            gl_active = video_gl_init(video_cgx_get_screen_bitmap(),
                                      video_cgx_get_screen_width(),
                                      video_cgx_get_screen_height(),
                                      gl_effect, gl_rgba);
        }
    }
#endif
    
    /* Clear the screen to black before starting */
    video_cgx_clear();
    
    /* Store settings */
    audio_use_ahi = use_ahi;
    audio_use_14bit = use_14bit;
    audio_disabled = nosound;
    frame_skip = frameskip;
    
    /* Enable FPS display if requested */
    if (show_fps_counter)
    {
        video_cgx_show_fps(1);
    }
    
    /* Paula hardware filter (CIA-A PRA bit 1). Set here, after the last
     * early return in main(), so every run that changes it also reaches
     * restore_paula_filter() at shutdown. */
    if (use_paula_filter && !nosound)
    {
        set_paula_filter(1);
        printf("Paula hardware filter: ON\n");
    }

    /* Initialize audio output (unless disabled) */
    if (nosound)
    {
        printf("Audio: disabled\n");
    }
    else if (use_ahi)
    {
        /* AHI: configurable frequency stereo */
        if (!ahi_audio_init(ahi_freq, 1, vdp_pal))
        {
            printf("Warning: AHI audio not available\n");
            audio_disabled = 1;  /* skip sound chip emulation entirely */
        }
        else
        {
            ahi_set_volume_boost(cfg.ahiboost);
            printf("Audio: AHI %dHz (boost=%dx)\n", ahi_freq, 1 << cfg.ahiboost);
        }
    }
    else if (use_14bit)
    {
        /* Paula 14-bit: True stereo, ~10-12 bit effective resolution */
        if (!paula14_audio_init(paula_freq, amiga_hw_pal, vdp_pal))
        {
            printf("Warning: Paula 14-bit audio not available\n");
            /* Fall back to 8-bit */
            audio_use_14bit = 0;
            if (!paula_audio_init(paula_freq, amiga_hw_pal, vdp_pal))
            {
                printf("Warning: Paula audio not available\n");
                audio_disabled = 1;  /* skip sound chip emulation entirely */
            }
        }
        else
        {
            paula14_set_volume_boost(cfg.paulaboost);
            printf("Audio: Paula 14-bit %dHz (boost=%dx)\n", 
                   paula14_audio_get_rate(),
                   1 << cfg.paulaboost);
        }
    }
    else
    {
        /* Paula 8-bit mono: Direct hardware DMA (lowest CPU overhead) */
        if (!paula_audio_init(paula_freq, amiga_hw_pal, vdp_pal))
        {
            printf("Warning: Paula audio not available\n");
            audio_disabled = 1;  /* skip sound chip emulation entirely */
        }
        else
        {
            paula_set_volume_boost(cfg.paulaboost);
            paula_set_stereo(cfg.paulastereo);
            printf("Audio: Paula 8-bit %dHz %s (boost=%dx)\n", 
                   paula_audio_get_rate(),
                   cfg.paulastereo ? "stereo" : "mono",
                   1 << cfg.paulaboost);
            printf("  fm_preamp=%d psg_preamp=%d\n",
                   config.fm_preamp, config.psg_preamp);
        }
    }
    
    /* Initialize input */
    input_amiga_init();

    /* Apply custom key mappings from config file.
     * remap_button replaces the default key for each button. */
    if (cfg.p1_a >= 0)     input_amiga_remap_button(0, INPUT_BTN_A, cfg.p1_a);
    if (cfg.p1_b >= 0)     input_amiga_remap_button(0, INPUT_BTN_B, cfg.p1_b);
    if (cfg.p1_c >= 0)     input_amiga_remap_button(0, INPUT_BTN_C, cfg.p1_c);
    if (cfg.p1_x >= 0)     input_amiga_remap_button(0, INPUT_BTN_X, cfg.p1_x);
    if (cfg.p1_y >= 0)     input_amiga_remap_button(0, INPUT_BTN_Y, cfg.p1_y);
    if (cfg.p1_z >= 0)     input_amiga_remap_button(0, INPUT_BTN_Z, cfg.p1_z);
    if (cfg.p1_start >= 0) input_amiga_remap_button(0, INPUT_BTN_START, cfg.p1_start);
    if (cfg.p1_mode >= 0)  input_amiga_remap_button(0, INPUT_BTN_MODE, cfg.p1_mode);
    if (cfg.p2_a >= 0)     input_amiga_remap_button(1, INPUT_BTN_A, cfg.p2_a);
    if (cfg.p2_b >= 0)     input_amiga_remap_button(1, INPUT_BTN_B, cfg.p2_b);
    if (cfg.p2_c >= 0)     input_amiga_remap_button(1, INPUT_BTN_C, cfg.p2_c);
    if (cfg.p2_x >= 0)     input_amiga_remap_button(1, INPUT_BTN_X, cfg.p2_x);
    if (cfg.p2_y >= 0)     input_amiga_remap_button(1, INPUT_BTN_Y, cfg.p2_y);
    if (cfg.p2_z >= 0)     input_amiga_remap_button(1, INPUT_BTN_Z, cfg.p2_z);
    if (cfg.p2_start >= 0) input_amiga_remap_button(1, INPUT_BTN_START, cfg.p2_start);
    if (cfg.p2_mode >= 0)  input_amiga_remap_button(1, INPUT_BTN_MODE, cfg.p2_mode);

    /* Set up save file paths and load SRAM */
    build_save_paths(romfile);

    /* Allocate save state buffer */
    state_buf = (unsigned char *)malloc(STATE_SIZE);
    if (!state_buf)
    {
        printf("Warning: Could not allocate save state buffer\n");
    }

    /* Load SRAM battery backup if it exists */
    sram_autoload();

    printf("Starting emulation (%s)...\n", vdp_pal ? "PAL 50Hz" : "NTSC 60Hz");
    
    /* Get window for input processing */
    win = video_cgx_get_window();
    
    /* Clear the entire video buffer to black */
    memset(bitmap.data, 0, bitmap.pitch * bitmap.height);

    /* Prime audio pipeline: queue 2 silent buffers immediately before the
     * main loop so they are still playing when frame 1 arrives.
     *
     * CRITICAL: must be done HERE (not before init/SRAM load) because init
     * code takes ~100ms and the pre-fill buffers (~33ms each) would drain
     * completely before the first real frame, leaving 0 queued -> gap click.
     *
     * With 2 pre-fills queued at T=0 and frame 1 submitting at T~8ms:
     *   - pre-fill 1 still has 8ms to play -> audio.device pre-loads shadow
     *     registers for pre-fill 2 AND for frame 1 immediately -> gapless. */
    if (!nosound)
    {
        int prime_samples;
        memset(audio_buffer, 0, sizeof(audio_buffer));
        if (audio_use_ahi && ahi_audio_available())
        {
            prime_samples = ahi_freq / (vdp_pal ? 50 : 60);
            ahi_audio_submit(audio_buffer, prime_samples);
            ahi_audio_submit(audio_buffer, prime_samples);
        }
        else if (audio_use_14bit && paula14_audio_available())
        {
            /* Even: Paula buffers are exact-length and CMD_WRITE needs it */
            prime_samples = (paula14_audio_get_rate() / (vdp_pal ? 50 : 60)) & ~1;
            paula14_audio_submit(audio_buffer, prime_samples);
            paula14_audio_submit(audio_buffer, prime_samples);
        }
        else if (paula_audio_available())
        {
            prime_samples = (paula_audio_get_rate() / (vdp_pal ? 50 : 60)) & ~1;
            paula_audio_submit(audio_buffer, prime_samples);
            paula_audio_submit(audio_buffer, prime_samples);
        }
    }

    /* Main emulation loop */
    {
        int frame_count = 0;
        int skip_counter = 0;
        int do_render = 1;  /* Always render first frame */
        ULONG frame_time_us = vdp_pal ? 20000 : 16667;  /* 50Hz=20ms, 60Hz=16.67ms */
        int skip_next_render = 0;  /* Auto-skip: 1 = skip next frame's render */
        int auto_skip_count = 0;   /* Consecutive skips so far */
        /* EClock frame timing: an absolute schedule (see timer_wait_until()).
         * The period is exactly 1/60 s or 1/50 s, matching audio_init(). */
        struct EClockVal ev_frame_start;
        double frame_period_ticks = 0.0;
        double next_deadline = 0.0;
        ULONG sched_resyncs = 0;            /* long stalls the schedule skipped */
        /* Audio queue statistics, printed when the emulator exits. Depth is
         * the number of buffers still playing just before a frame's audio is
         * submitted: 0 means the queue ran dry (an audible gap), and a full
         * queue means that frame's audio was thrown away. Counted here, not in
         * the backends, because this loop checks *_available() first, so a
         * backend never sees the frames it is not given. */
        ULONG aud_frames = 0, aud_dropped = 0, aud_dry = 0;
        ULONG aud_first_drop = 0;           /* 1-based frame number, 0 = none */
        ULONG aud_depth[5];                 /* Paula: 0..4, AHI: 0..3 */
        ULONG aud_stretch = 0, aud_shrink = 0;  /* rate-control corrections */
        struct EClockVal ev_run_start;
        /* Frame timing statistics, printed with the audio ones. "Emulation"
         * is input + core + audio, "video" is the blit and any wait inside
         * ChangeScreenBuffer(). Sums are doubles: ULONG microsecond sums
         * overflow in ~1 h. */
        struct EClockVal ev_mid;
        double ft_emu_sum = 0.0, ft_vid_sum = 0.0;
        ULONG ft_emu_max = 0, ft_vid_max = 0, ft_over = 0, ft_frames = 0;
        ULONG ft_emu_us = 0;
        /* Start-to-start frame period: shows wait overshoot and stalls that
         * the work time alone cannot see. */
        struct EClockVal ev_prev_start;
        int ft_have_prev = 0;
        ULONG ft_period_max = 0, ft_late = 0;
        ULONG ft_drawn = 0;                 /* frames actually rendered */

        memset(aud_depth, 0, sizeof(aud_depth));
        if (eclock_freq)
        {
            ReadEClock(&ev_run_start);
            frame_period_ticks = (vdp_pal ? 0.020 : 1.0 / 60.0) * (double)eclock_freq;
            next_deadline = eclock_ticks(&ev_run_start) + frame_period_ticks;
        }

#ifdef GPGX_PROFILE
        {
            /* Emu68 JIT control register: logged, and with -jitdepth=N its
             * JCC_INSN_DEPTH field (bits 31-24, 0 = 256) set for the run */
            ULONG jc = prof_jitctrl(0, 0);
            printf("Profile: Emu68 JITCTRL $%08lx (%lu instructions per unit)\n",
                   jc, (jc >> 24) ? (jc >> 24) : 256UL);
            {
                /* bit 1 = slow DBF: Emu68 then ends a translation unit
                 * after every DBcc (M68k_LINE5.c) */
                ULONG jc2 = prof_jitctrl2();
                printf("Profile: Emu68 JITCTRL2 $%08lx (slow DBF %s)\n",
                       jc2, (jc2 & 2) ? "on" : "off");
            }
            if (prof_jitdepth)
            {
                prof_jitctrl((jc & 0x00ffffffUL) | ((ULONG)(prof_jitdepth & 0xff) << 24), 1);
                printf("Profile: Emu68 JITCTRL set to $%08lx (-jitdepth=%d)\n",
                       prof_jitctrl(0, 0), prof_jitdepth);
                prof_jitdepth = (int)(jc >> 24);   /* old field, restored after the loop */
            }
            else
                prof_jitdepth = -1;                /* nothing to restore */
        }
#endif
        while (running)
        {
            /* Record frame start for EClock timing */
            ReadEClock(&ev_frame_start);
#ifdef GPGX_PROFILE
            prof_frame_begin();
#endif

            if (eclock_freq)
            {
                if (ft_have_prev)
                {
                    ULONG period_us = eclock_diff_us(&ev_prev_start, &ev_frame_start);
                    if (period_us > ft_period_max)
                        ft_period_max = period_us;
                    if (period_us > frame_time_us + 1000)
                        ft_late++;
                }
                ev_prev_start = ev_frame_start;
                ft_have_prev = 1;
            }

            /* Process Amiga input events */
            if (input_amiga_process(win))
            {
                running = 0;
                break;
            }
            
            /* Update emulated input */
            osd_input_update();

            /* Process save state hotkeys */
            if (input_amiga_hotkey == INPUT_HOTKEY_SAVE)
            {
                input_amiga_hotkey = INPUT_HOTKEY_NONE;
                do_save_state();
                sram_autosave();
            }
            else if (input_amiga_hotkey == INPUT_HOTKEY_LOAD)
            {
                input_amiga_hotkey = INPUT_HOTKEY_NONE;
                do_load_state();
            }
            
            /* Frame skipping - always render first frame.
             *
             * Fixed (frameskip>0):
             *   frameskip=1: render every 2nd  (50% video)
             *   frameskip=2: render every 3rd  (33% video)
             *   frameskip=3: render every 4th  (25% video)
             *
             * Auto (frameskip=0, autoskip=1):
             *   If the schedule is over half a frame behind, skip this
             *   render, but never more than AUTOSKIP_MAX_RUN in a row.
             *   On fast hardware: full 60fps. On slow: degrades gracefully. */
            if (frame_skip > 0 && frame_count > 0)
            {
                if (skip_counter < frame_skip)
                {
                    skip_counter++;
                    do_render = 0;
                }
                else
                {
                    skip_counter = 0;
                    do_render = 1;
                }
            }
            else if (use_autoskip && frame_count > 0)
            {
                /* Skip render if the schedule is behind, but never more than
                 * AUTOSKIP_MAX_RUN consecutive skips (a skipped frame only
                 * saves the VDP render and the blit, ~2 ms on a Pi 3). */
                if (skip_next_render && auto_skip_count < AUTOSKIP_MAX_RUN)
                {
                    do_render = 0;
                    auto_skip_count++;
                }
                else
                {
                    do_render = 1;
                    auto_skip_count = 0;
                }
                skip_next_render = 0;
            }
            else
            {
                do_render = 1;
            }
            
            /* Run one frame - pass skip flag to avoid VDP rendering when skipping */
#ifndef NO_SCD
            if (system_hw == SYSTEM_MCD)
            {
                system_frame_scd(do_render ? 0 : 1);
            }
            else
#endif
            if ((system_hw & SYSTEM_PBC) == SYSTEM_MD)
            {
                PROF_CALL(PROF_CORE, system_frame_gen(do_render ? 0 : 1));
            }
            else
            {
                PROF_CALL(PROF_CORE, system_frame_sms(do_render ? 0 : 1));
            }

            /* Reapply RAM cheat patches (game may reset values each frame) */
            if (ram_patch_count > 0)
                apply_ram_patches();
            
            /* Generate and play audio FIRST (before display update).
             * This ensures the next audio buffer is queued with Paula
             * before the potentially slow screen blit, preventing
             * buffer underrun (trembling/stuttering). */
            PROF_PUSH(PROF_AUDIO);
            if (!audio_disabled)
            {
                int depth, sent = 0, n_frame, play;

                PROF_PUSH(PROF_AUD_BLIP);
                audio_samples = audio_update(audio_buffer);
                PROF_POP();

                if (audio_use_ahi)
                    depth = ahi_audio_queued();
                else if (audio_use_14bit)
                    depth = paula14_audio_queued();
                else
                    depth = paula_audio_queued();

                /* Rate control: nudge the frame length while the queue is
                 * off target, ~0.6% per frame (twice that once it has run
                 * dry). At the target nothing is resampled. */
                n_frame = audio_samples;
                if (depth != AUDIO_TARGET_DEPTH && audio_samples > 1)
                {
                    int adj = (audio_samples + 199) / 200;
                    if (depth == 0)
                        adj *= 2;
                    if (depth < AUDIO_TARGET_DEPTH)
                    {
                        n_frame = audio_samples + adj;
                        aud_stretch++;
                    }
                    else
                    {
                        n_frame = audio_samples - adj;
                        aud_shrink++;
                    }
                }
                play = audio_prepare(audio_buffer, audio_samples, n_frame);

                if (play > 0)
                {
                    if (audio_use_ahi)
                    {
                        if (ahi_audio_available())
                        {
                            ahi_audio_submit(audio_out, play);
                            sent = 1;
                        }
                    }
                    else if (audio_use_14bit)
                    {
                        if (paula14_audio_available())
                        {
                            paula14_audio_submit(audio_out, play);
                            sent = 1;
                        }
                    }
                    else
                    {
                        if (paula_audio_available())
                        {
                            paula_audio_submit(audio_out, play);
                            sent = 1;
                        }
                    }
                }
                else
                    sent = 1;   /* nothing to play yet (carry only) */

                aud_frames++;
                if (depth < 0) depth = 0;
                if (depth > 4) depth = 4;
                aud_depth[depth]++;
                if (depth == 0)
                    aud_dry++;
                if (!sent)
                {
                    aud_dropped++;
                    if (!aud_first_drop)
                        aud_first_drop = (ULONG)frame_count + 1;
                }
            }
            PROF_POP();

            if (eclock_freq)
            {
                ReadEClock(&ev_mid);
                ft_emu_us = eclock_diff_us(&ev_frame_start, &ev_mid);
            }

            /* Update display - only when not skipping */
            PROF_PUSH(PROF_VIDEO);
            if (do_render)
            {
                int vp_x, vp_y, vp_w, vp_h;

                ft_drawn++;

                /* Clear screen when viewport dimensions change to avoid pixel
                 * bleeding: old pixels outside the new (smaller) viewport stay
                 * visible as border garbage if not erased. bit 0 = size changed. */
                if (bitmap.viewport.changed & 1)
                {
                    video_cgx_clear();
                    bitmap.viewport.changed &= ~1;  /* preserve bit 1 (VDP pending flag) */
                }

                /* Handle viewport offsets for display:
                 *
                 * remap_line() in vdp_render.c already applies viewport.x/y
                 * when writing to video_buffer:
                 *   - negative viewport.x: source starts at linebuf[0x20+|x|],
                 *     writes the cropped content to column 0 of video_buffer
                 *   - negative viewport.y: v_counter is shifted so cropped
                 *     lines land at row 0 of video_buffer
                 *
                 * So for negative offsets we only shrink w/h; the source
                 * pointer stays at (0, 0) - remap_line already did the crop.
                 *
                 * For Genesis: x/y are 0, w/h = 320x224 or 256x224.
                 * For GG (gg_extra=0): x=-48, y=-24 -> blit 160x144 from (0,0).
                 */
                vp_x = bitmap.viewport.x;
                vp_y = bitmap.viewport.y;
                vp_w = bitmap.viewport.w;
                vp_h = bitmap.viewport.h;

                if (vp_x < 0)
                {
                    vp_w += vp_x * 2;  /* reduce width by 2 * border */
                    vp_x = 0;           /* remap_line already applied the crop */
                }
                if (vp_y < 0)
                {
                    vp_h += vp_y * 2;  /* reduce height by 2 * border */
                    vp_y = 0;           /* remap_line already applied the crop */
                }

#ifdef USE_MINIGL
                if (gl_active)
                {
                    /* FPS / OSD text go into the frame itself: the GPU
                     * draws the whole screen, so nothing drawn on it
                     * afterwards would survive. */
                    video_cgx_overlay_rgb565((UWORD *)bitmap.data, bitmap.pitch / 2,
                                             vp_x, vp_y, vp_w, vp_h);
                    video_gl_update((const UWORD *)bitmap.data, bitmap.pitch / 2,
                                    vp_x, vp_y, vp_w, vp_h);
                }
                else
#endif
                video_cgx_update(vp_x, vp_y, vp_w, vp_h);
            }
            
            /* Update FPS counter */
            video_cgx_update_fps();
            PROF_POP();
#ifdef GPGX_PROFILE
            prof_frame_end();
            if (prof_flushtest)
            {
                /* What a full JIT cache flush costs on Emu68: the call
                 * itself here, the re-translation in the next frame's
                 * sections. */
                ULONG t[3], u[3];
                prof_read3(t);
                CacheClearU();
                prof_read3(u);
                prof_flush_ticks += (double)(ULONG)(u[0] - t[0]);
                prof_flushes++;
            }
            if (prof_get_frames() % PROF_INTERVAL == 0)
            {
                /* Per interval: frames drawn, and for the SVP the DSP
                 * cycles, IRAM words written, new translations and runs
                 * entered from C, all per frame */
                static ULONG iv_drawn, iv_cyc, iv_gen, iv_blocks, iv_runs;
                char extra[160];
                extra[0] = 0;
                if (svp)
                    sprintf(extra, "; DSP cycles %lu, IRAM writes %lu, translations %lu,"
                            " runs from C %lu",
                            (ULONG)(ssp_dbg_cycles - iv_cyc) / PROF_INTERVAL,
                            (ULONG)(ssp_iram_gen - iv_gen) / PROF_INTERVAL,
                            (ULONG)(ssp_drc_blocks - iv_blocks),
                            (ULONG)(ssp_drc_runs - iv_runs) / PROF_INTERVAL);
                printf("Interval: %lu of %d frames drawn%s\n",
                       ft_drawn - iv_drawn, PROF_INTERVAL, extra);
                prof_interval("");
                iv_drawn = ft_drawn; iv_cyc = ssp_dbg_cycles; iv_gen = ssp_iram_gen;
                iv_blocks = ssp_drc_blocks; iv_runs = ssp_drc_runs;
            }
#endif

            /* Frame timing: absolute EClock schedule. A frame that runs past
             * its deadline does not wait, and the next deadline stays where
             * it was, so short overruns and wait overshoot are won back. */
            {
                ULONG elapsed_us = eclock_us_since(&ev_frame_start);

                if (eclock_freq)
                {
                    struct EClockVal ev_now;
                    double now_t;
                    ULONG vid_us = (elapsed_us > ft_emu_us) ? elapsed_us - ft_emu_us : 0;

                    ft_frames++;
                    ft_emu_sum += (double)ft_emu_us;
                    ft_vid_sum += (double)vid_us;
                    if (ft_emu_us > ft_emu_max) ft_emu_max = ft_emu_us;
                    if (vid_us > ft_vid_max)    ft_vid_max = vid_us;
                    if (elapsed_us >= frame_time_us)
                        ft_over++;

                    ReadEClock(&ev_now);
                    now_t = eclock_ticks(&ev_now);
                    if (now_t >= next_deadline)
                    {
                        /* Autoskip only once the schedule is more than half
                         * a frame behind. Skipping on ANY lateness skipped
                         * every other frame when frames overran slightly
                         * (GPU output overruns by ~0.5 ms): 59% of frames
                         * drawn, measured on target, vs ~94% modelled with
                         * this threshold. Smaller lags are won back by the
                         * schedule on the following frames. */
                        if (use_autoskip &&
                            now_t - next_deadline > 0.5 * frame_period_ticks)
                            skip_next_render = 1;
                        /* A long stall (disk access, requester, state save)
                         * is skipped rather than caught up at full speed. */
                        if (now_t - next_deadline > 4.0 * frame_period_ticks)
                        {
                            next_deadline = now_t;
                            sched_resyncs++;
                        }
                    }
                    else
                        timer_wait_until(next_deadline);
                    next_deadline += frame_period_ticks;
                }
            }
            
            frame_count++;
        }
#ifdef GPGX_PROFILE
        if (prof_jitdepth >= 0)
        {
            ULONG jc = prof_jitctrl(0, 0);
            prof_jitctrl((jc & 0x00ffffffUL) | ((ULONG)prof_jitdepth << 24), 1);
            printf("Profile: Emu68 JITCTRL restored to $%08lx\n", prof_jitctrl(0, 0));
        }
#endif

        /* Exit summary. Integer-only output: nothing else in this program
         * prints floating point through libnix's printf. */
        {
            double secs = 0.0;

            if (eclock_freq)
            {
                struct EClockVal ev_run_end;

                ReadEClock(&ev_run_end);
                /* Full 64-bit difference: the 32-bit eclock_* helpers wrap
                 * after ~6053 s, and a play session can run longer. */
                secs = (eclock_ticks(&ev_run_end) - eclock_ticks(&ev_run_start))
                       / (double)eclock_freq;
            }

            if (aud_frames)
            {
                printf("\nAudio: %lu frames", aud_frames);
                if (secs >= 1.0)
                {
                    ULONG dpm10 = (ULONG)((double)aud_dropped * 600.0 / secs + 0.5);
                    printf(" in %lu s, %lu dropped (%lu.%lu/min)",
                           (ULONG)(secs + 0.5), aud_dropped, dpm10 / 10, dpm10 % 10);
                }
                else
                    printf(", %lu dropped", aud_dropped);
                if (aud_first_drop)
                    printf(", first at frame %lu", aud_first_drop);
                printf(", %lu ran dry\n", aud_dry);
                printf("Audio: buffers queued before submit  0:%lu 1:%lu 2:%lu 3:%lu 4:%lu\n",
                       aud_depth[0], aud_depth[1], aud_depth[2],
                       aud_depth[3], aud_depth[4]);
                printf("Audio: rate control stretched %lu frames, shrank %lu\n",
                       aud_stretch, aud_shrink);
                if (audio_use_14bit)
                    printf("Audio: 14-bit channels started in sync %d time(s)\n",
                           paula14_dbg_syncs);
                /* Paula playing slower than its nominal rate shows up as the
                 * queue piling up: shrinking on most frames, or dropping.
                 * HRM 5-2-5-1: below period 124 (123 PAL) the audio DMA
                 * cannot fetch in time on a 15 kHz display and "the
                 * previous sample will be reused". */
                if (!audio_use_ahi && aud_frames >= 600 &&
                    (aud_shrink * 2 > aud_frames || aud_dropped * 20 > aud_frames))
                {
                    printf("Audio: Paula played slower than its nominal rate. A sample\n");
                    printf("       rate above ~28 kHz needs a 31 kHz native display mode;\n");
                    printf("       try paulafreq=27710.\n");
                }
            }
            if (ft_frames)
            {
                ULONG emu_avg = (ULONG)(ft_emu_sum / (double)ft_frames + 0.5);
                ULONG vid_avg = (ULONG)(ft_vid_sum / (double)ft_frames + 0.5);
                ULONG over_pct10 = (ULONG)((double)ft_over * 1000.0 / (double)ft_frames + 0.5);
                /* Schedule drift: real time minus frames * exact period.
                 * ~0 means the average frame rate is exactly 60/50 Hz. */
                LONG drift_ms = (LONG)((secs - (double)ft_frames * frame_period_ticks
                                        / (double)eclock_freq) * 1000.0);

                printf("Frames: budget %lu us, average emulation %lu us + video %lu us\n",
                       frame_time_us, emu_avg, vid_avg);
                printf("Frames: max emulation %lu us, max video %lu us, %lu over budget (%lu.%lu%%)\n",
                       ft_emu_max, ft_vid_max, ft_over, over_pct10 / 10, over_pct10 % 10);
                printf("Frames: start-to-start max %lu us, %lu frames >1 ms late, schedule drift %ld ms, %lu resyncs\n",
                       ft_period_max, ft_late, drift_ms, sched_resyncs);
                {
                    ULONG dr10 = (ULONG)((double)ft_drawn * 1000.0 / (double)ft_frames + 0.5);
                    printf("Frames: %lu of %lu drawn (%lu.%lu%%)\n",
                           ft_drawn, ft_frames, dr10 / 10, dr10 % 10);
                }
                if (svp && ssp_dbg_calls)
                {
                    ULONG idle10 = (ULONG)((double)ssp_dbg_idle * 1000.0
                                           / (double)ssp_dbg_calls + 0.5);
                    printf("SVP: %lu runs, %lu ended in an idle wait (%lu.%lu%%)\n",
                           ssp_dbg_calls, ssp_dbg_idle, idle10 / 10, idle10 % 10);
                    if (ssp_dbg_cycles)
                        printf("SVP: %lu DSP cycles per frame (limit %d per line)\n",
                               (ULONG)((double)ssp_dbg_cycles / (double)ft_frames + 0.5),
                               (int)SVP_cycles);
                    printf("SVP recompiler: %s; %lu blocks translated, %lu resets,"
                           " %lu runs finished by the interpreter\n",
                           ssp_drc_why, ssp_drc_blocks, ssp_drc_resets, ssp_drc_fallbacks);
                    if (ssp_drc_blocks)
                        printf("SVP recompiler: translations %lu%% IRAM, %lu words and %lu bytes"
                               " each; resets: code full %lu, index full %lu, failed %lu\n",
                               (ULONG)((double)ssp_drc_tr_iram * 100.0 / (double)ssp_drc_blocks + 0.5),
                               (ULONG)(ssp_drc_tr_words / (double)ssp_drc_blocks + 0.5),
                               (ULONG)(ssp_drc_tr_bytes / (double)ssp_drc_blocks + 0.5),
                               ssp_drc_rs_code, ssp_drc_rs_meta, ssp_drc_rs_fail);
                    if (ssp_drc_runs)
                    {
                        static const char *const rn[16] = { "-", "X", "Y", "A", "ST",
                            "STACK", "PC", "P", "PM0", "PM1", "PM2", "XST", "PM4",
                            "gr13", "PMC", "AL" };
                        double fr = (double)ft_frames;
                        int h;
                        printf("SVP recompiler: per frame %lu blocks entered from C, %lu chained"
                               " (profile builds only); back to C: few cycles %lu,"
                               " untranslated %lu, IRAM check %lu, segment %lu\n",
                               (ULONG)(ssp_drc_runs / fr + 0.5), (ULONG)(ssp_drc_disp[0] / fr + 0.5),
                               (ULONG)(ssp_drc_disp[3] / fr + 0.5), (ULONG)(ssp_drc_disp[4] / fr + 0.5),
                               (ULONG)(ssp_drc_disp[5] / fr + 0.5), (ULONG)(ssp_drc_disp[6] / fr + 0.5));
                        printf("SVP recompiler: handler calls per frame:");
                        for (h = 0; h < 32; h++)
                            if (ssp_drc_hcnt[h])
                                printf(" %s %s %lu", h < 16 ? "read" : "write", rn[h & 15],
                                       (ULONG)(ssp_drc_hcnt[h] / fr + 0.5));
                        printf("\n");
                    }
                    if (ssp_drc_runs)
                    {
                        ULONG cpb10 = (ULONG)(ssp_drc_run_cycles * 10.0 / (double)ssp_drc_runs + 0.5);
                        printf("SVP recompiler: %lu runs from C per frame, %lu.%lu DSP cycles each;"
                               " %lu runs began in an idle wait\n",
                               (ULONG)((double)ssp_drc_runs / (double)ft_frames + 0.5),
                               cpb10 / 10, cpb10 % 10, ssp_drc_waitstart);
                    }
#ifdef SVP_VERIFY
                    if (ssp_drc_vblocks)
                        printf("SVP DRC verify: %lu blocks compared, %lu differed\n",
                               ssp_drc_vblocks, ssp_drc_vbad);
                    else
                        printf("SVP verify: %lu runs compared, %lu differed\n",
                               ssp_verify_runs, ssp_verify_bad);
#endif
#ifdef SVP_PROFILE
                    {
                        double ops = 0.0;
                        int o;
                        for (o = 0; o < 128; o++)
                            ops += (double)ssp_prof_op[o];
                        print_top("SVP profile: busiest opcodes (op >> 9)",
                                  ssp_prof_op, 128, 16, ops);
                        print_top("SVP profile: busiest program addresses (word)",
                                  ssp_prof_pc, 0x10000, 40, ops);
                    }
#endif
                }
            }
        }
    }

#ifdef GPGX_PROFILE
    if (prof_flushes)
        printf("Profile: -flushtest: %lu CacheClearU() calls, %lu us each (call only)\n",
               prof_flushes, (ULONG)(prof_ticks_to_us(prof_flush_ticks) / (double)prof_flushes + 0.5));
    prof_report();
    if (svp)
    {
        extern void ssp_fast_prof_report(unsigned long frames);
        ssp_fast_prof_report(prof_get_frames());
        {
            extern unsigned long ssp_drc_pmstat[10], ssp_drc_exitstat[20];
            double f = (double)prof_get_frames();
            if (f > 0.0)
                printf("SVP PM paths per frame: read ROM %lu DRAM %lu raw %lu C %lu;"
                       " write DRAM %lu cell %lu raw %lu C %lu IRAM %lu ignored %lu\n",
                       (ULONG)(ssp_drc_pmstat[0] / f + 0.5), (ULONG)(ssp_drc_pmstat[1] / f + 0.5),
                       (ULONG)(ssp_drc_pmstat[2] / f + 0.5), (ULONG)(ssp_drc_pmstat[3] / f + 0.5),
                       (ULONG)(ssp_drc_pmstat[4] / f + 0.5), (ULONG)(ssp_drc_pmstat[5] / f + 0.5),
                       (ULONG)(ssp_drc_pmstat[6] / f + 0.5), (ULONG)(ssp_drc_pmstat[7] / f + 0.5),
                       (ULONG)(ssp_drc_pmstat[8] / f + 0.5), (ULONG)(ssp_drc_pmstat[9] / f + 0.5));
            if (f > 0.0)
                printf("SVP block exits per frame: end %lu, bra forward %lu, bra back %lu,"
                       " call %lu, not taken %lu, PC write %lu, few cycles %lu, flag %lu;"
                       " loops inside blocks %lu\n",
                       (ULONG)(ssp_drc_exitstat[0] / f + 0.5), (ULONG)(ssp_drc_exitstat[1] / f + 0.5),
                       (ULONG)(ssp_drc_exitstat[2] / f + 0.5), (ULONG)(ssp_drc_exitstat[3] / f + 0.5),
                       (ULONG)(ssp_drc_exitstat[5] / f + 0.5), (ULONG)(ssp_drc_exitstat[6] / f + 0.5),
                       (ULONG)(ssp_drc_exitstat[7] / f + 0.5), (ULONG)(ssp_drc_exitstat[8] / f + 0.5),
                       (ULONG)(ssp_drc_exitstat[4] / f + 0.5));
            if (f > 0.0)
                printf("SVP calls per frame: callee in ROM: leaf %lu, leaf with branches %lu,"
                       " other %lu; in IRAM: leaf %lu, leaf with branches %lu, other %lu;"
                       " ret in IRAM blocks %lu, in ROM blocks %lu, other PC writes %lu\n",
                       (ULONG)(ssp_drc_exitstat[10] / f + 0.5), (ULONG)(ssp_drc_exitstat[11] / f + 0.5),
                       (ULONG)(ssp_drc_exitstat[12] / f + 0.5), (ULONG)(ssp_drc_exitstat[13] / f + 0.5),
                       (ULONG)(ssp_drc_exitstat[14] / f + 0.5), (ULONG)(ssp_drc_exitstat[15] / f + 0.5),
                       (ULONG)(ssp_drc_exitstat[16] / f + 0.5), (ULONG)(ssp_drc_exitstat[17] / f + 0.5),
                       (ULONG)(ssp_drc_exitstat[18] / f + 0.5));
            if (f > 0.0)
            {
                extern unsigned long ssp_drc_tr_inlined;
                printf("SVP inlined calls per frame %lu; translations with an inlined call %lu\n",
                       (ULONG)(ssp_drc_exitstat[19] / f + 0.5), ssp_drc_tr_inlined);
            }
        }
    }
#endif
    printf("\n");

    /* Save SRAM battery backup */
    sram_autosave();

    /* SVP recompiler memory (allocated on first use) */
    ssp1601_drc_shutdown();

    /* Free save state buffer */
    if (state_buf)
    {
        free(state_buf);
        state_buf = NULL;
    }
    
    /* Cleanup */
    audio_shutdown();
    input_amiga_shutdown();
    if (audio_use_ahi)
    {
        ahi_audio_shutdown();
    }
    else if (audio_use_14bit)
    {
        paula14_audio_shutdown();
    }
    else
    {
        paula_audio_shutdown();
    }
#ifdef USE_MINIGL
    /* Before the screen closes: the GL context renders into its bitmap */
    if (gl_active)
        video_gl_shutdown();
#endif
    video_cgx_shutdown();
    restore_paula_filter();
    timer_shutdown();
    close_libraries();
    
    return 0;
}
