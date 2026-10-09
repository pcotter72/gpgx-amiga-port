/****************************************************************************
 *  osd.h
 *
 *  Genesis Plus GX - AmigaOS/CaffeineOS Port
 *  Operating System Dependent layer for PiStorm/Emu68
 *
 *  Target: m68k-amigaos (bebbo's GCC 6.5+)
 *  Requires: AmigaOS 3.x, CyberGraphX/P96, AHI
 *
 ****************************************************************************/

#ifndef _OSD_H_
#define _OSD_H_

/* Standard C includes */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* AmigaOS includes - bebbo's GCC provides these */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>

/* Use stdint.h if available (bebbo's GCC has it) */
#ifdef __GNUC__
#include <stdint.h>
#endif

/* Simple ctype replacements to avoid locale issues with -noixemul */
#ifdef AMIGA
#undef toupper
#undef tolower
#define toupper(c) (((c) >= 'a' && (c) <= 'z') ? ((c) - 'a' + 'A') : (c))
#define tolower(c) (((c) >= 'A' && (c) <= 'Z') ? ((c) - 'A' + 'a') : (c))
#endif

/* Basic type definitions matching core expectations */
#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Path and input limits */
#define MAX_INPUTS 8
#define MAX_KEYS 8
#define MAXPATHLEN 256

/* Disable Sega CD support initially to avoid libchdr complexity */
#ifndef NO_SCD
#define NO_SCD 1
#endif

/* Disable CHD support (requires libchdr with modern C) */
#ifndef NO_CHD
#define NO_CHD 1
#endif

/* Use simpler YM2612 core (avoid YM3438 which needs more resources) */
/* Uncomment to enable Nuked OPN2 (more accurate but slower): */
/* #define HAVE_YM3438_CORE */
/* #define HAVE_OPLL_CORE */

/* Sprite limit configuration */
#define HAVE_NO_SPRITE_LIMIT
#define MAX_SPRITES_PER_LINE 80
#define TMS_MAX_SPRITES_PER_LINE (config.no_sprite_limit ? MAX_SPRITES_PER_LINE : 4)
#define MODE4_MAX_SPRITES_PER_LINE (config.no_sprite_limit ? MAX_SPRITES_PER_LINE : 8)
#define MODE5_MAX_SPRITES_PER_LINE (config.no_sprite_limit ? MAX_SPRITES_PER_LINE : (bitmap.viewport.w >> 4))
#define MODE5_MAX_SPRITE_PIXELS (config.no_sprite_limit ? MAX_SPRITES_PER_LINE * 32 : max_sprite_pixels)

/* CRC32 function - we provide our own */
#include "crc32.h"

/* Cheat update macro */
#define CHEATS_UPDATE() do { } while(0)

/* Input configuration */
typedef struct
{
    signed char device;
    unsigned char port;
    unsigned char padtype;
} t_input_config;

/* Emulator configuration structure */
typedef struct
{
    char version[16];
    unsigned char hq_fm;
    unsigned char filter;
    unsigned char hq_psg;
    unsigned char ym2612;
    unsigned char ym2413;
    unsigned char mono;
    short psg_preamp;
    short fm_preamp;
    short cdda_volume;
    short pcm_volume;
    unsigned short lp_range;
    short low_freq;
    short high_freq;
    short lg;
    short mg;
    short hg;
    unsigned char system;
    unsigned char region_detect;
    unsigned char master_clock;
    unsigned char vdp_mode;
    unsigned char force_dtack;
    unsigned char addr_error;
    unsigned char bios;
    unsigned char lock_on;
    unsigned char add_on;
    unsigned char overscan;
    unsigned char aspect_ratio;
    unsigned char ntsc;
    unsigned char lcd;
    unsigned char gg_extra;
    unsigned char left_border;
    unsigned char render;
    t_input_config input[MAX_INPUTS];
    unsigned char invert_mouse;
    unsigned char gun_cursor;
    unsigned int overclock;
    unsigned char no_sprite_limit;
    unsigned char enhanced_vscroll;
    unsigned char enhanced_vscroll_limit;
    unsigned char cd_latency;
} t_config;

/* Global configuration */
extern t_config config;

/* BIOS paths (optional) */
extern char GG_ROM[256];
extern char AR_ROM[256];
extern char SK_ROM[256];
extern char SK_UPMEM[256];
extern char GG_BIOS[256];
extern char MD_BIOS[256];
extern char CD_BIOS_EU[256];
extern char CD_BIOS_US[256];
extern char CD_BIOS_JP[256];
extern char MS_BIOS_US[256];
extern char MS_BIOS_EU[256];
extern char MS_BIOS_JP[256];

/* OSD function prototypes */
extern void osd_input_update(void);
extern int load_archive(char *filename, unsigned char *buffer, int maxsize, char *extension);
extern void ROMCheatUpdate(void);

/* File I/O for CD support (stubbed when NO_SCD defined) */
#ifndef NO_SCD
#define cdStream            FILE
#define cdStreamOpen(fname) fopen(fname, "rb")
#define cdStreamClose       fclose
#define cdStreamRead        fread
#define cdStreamSeek        fseek
#define cdStreamTell        ftell
#define cdStreamGets        fgets
#endif

#endif /* _OSD_H_ */
