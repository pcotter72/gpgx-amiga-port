/****************************************************************************
 *  video_cgx.c
 *
 *  CyberGraphX/P96 Video Backend for Genesis Plus GX Amiga Port
 *  32-bit BGRA mode with optional scanlines for best performance
 *
 ****************************************************************************/

#include <stdio.h>
#include <string.h>

#include <exec/exec.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <graphics/displayinfo.h>
#include <cybergraphx/cybergraphics.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <proto/timer.h>
#include <devices/timer.h>

/* CyberGraphX constants */
#ifndef RECTFMT_RGBA
#define RECTFMT_RGBA 3
#endif

/* CyberGraphX pixel format detection */
#ifndef CYBRMATTR_PIXFMT
#define CYBRMATTR_PIXFMT 0x80040004
#endif

/* Pixel format IDs come from cybergraphx/cybergraphics.h:
 * PIXFMT_ARGB32 = 11  (0xAARRGGBB)
 * PIXFMT_BGRA32 = 12  (0xBBGGRRAA)
 * PIXFMT_RGBA32 = 13  (0xRRGGBBAA)
 */

/* Detected screen pixel format */
static int screen_pixfmt = PIXFMT_ARGB32;  /* default to ARGB */

/* ScreenBuffer constants (OS 3.x double buffering) */
#ifndef SB_SCREEN_BITMAP
#define SB_SCREEN_BITMAP 0x01
#endif

/* LockBitMapTags defines */
#ifndef LBMI_BYTESPERROW
#define LBMI_BYTESPERROW    0x84001001
#endif
#ifndef LBMI_BASEADDRESS
#define LBMI_BASEADDRESS    0x84001006
#endif

/* BestCModeIDTags defines */
#ifndef CYBRBIDTG_Depth
#define CYBRBIDTG_Depth         0x80050009
#endif
#ifndef CYBRBIDTG_NominalWidth
#define CYBRBIDTG_NominalWidth  0x8005000a
#endif
#ifndef CYBRBIDTG_NominalHeight
#define CYBRBIDTG_NominalHeight 0x8005000b
#endif

#include "video_cgx.h"

/* Assembly optimized blitter routines */
#ifdef USE_ASM_VIDEO
extern void Blit565toRGBA_asm(UWORD *src, ULONG *dst, ULONG *lut, int pixels);
extern void Blit565toRGBA_2x_asm(UWORD *src, ULONG *dst1, ULONG *dst2, ULONG *lut, int pixels);
extern void Blit565toRGBA_2x_scanline_asm(UWORD *src, ULONG *dst1, ULONG *dst2,
                                           ULONG *lut, ULONG *dark_lut, int pixels);
#endif

/* External library bases */
extern struct Library *CyberGfxBase;
extern struct IntuitionBase *IntuitionBase;
extern struct GfxBase *GfxBase;
extern struct Device *TimerBase;

/* Video state */
static struct Screen *video_screen = NULL;
static struct Window *video_window = NULL;
static unsigned char *video_buffer = NULL;  /* Emulator's 16-bit RGB565 buffer */
static int video_width = 0;
static int video_height = 0;
static int video_pitch = 0;
static int video_depth = 32;
static int video_initialized = 0;

/* Double buffering for VSync */
static struct ScreenBuffer *sb[2] = {NULL, NULL};
static struct MsgPort *sb_safe_port = NULL;  /* dbi_SafeMessage replies (both buffers) */
static int sb_current = 0;       /* Which buffer is displayed (front) */
static int sb_safe_to_write = TRUE;  /* Last swap's SafeMessage received:
                                      * the back buffer is off-screen */
static int dbl_buf_active = 0;   /* 1 if double buffering is in use */

/* Scanline settings */
static int scanline_mode = 0;      /* 0=off, 1=subtle, 2=medium, 3=strong */
static int screen_is_2x = 0;       /* 1 if screen is 2x size for scanlines */
static int force_2x = 0;           /* Force 2x scaling (e.g. Game Gear) */
static int req_screen_w = 0;       /* video_cgx_set_screen_size(): 0 = auto */
static int req_screen_h = 0;
static int screen_width = 0;
static int screen_height = 0;

/* Blank pointer to hide mouse cursor */
static UWORD __chip blank_pointer[] = { 0, 0, 0, 0 };

/* FPS display */
static int show_fps = 0;
static int fps_frame_count = 0;
static int fps_current = 0;
static ULONG fps_last_time = 0;

/* VSync */
static int vsync_enabled = 0;

/* OSD text message */
static char osd_text[64];
static int osd_text_timer = 0;

/* 8x8 ASCII font for A-Z, a-z, 0-9, space, and common punctuation */
/* Covers ASCII 32-122 (space through 'z') */
static const unsigned char ascii_font[91][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},  /* 32 space */
    {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},  /* 33 ! */
    {0x6C,0x6C,0x00,0x00,0x00,0x00,0x00,0x00},  /* 34 " */
    {0x6C,0xFE,0x6C,0x6C,0xFE,0x6C,0x00,0x00},  /* 35 # */
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00},  /* 36 $ */
    {0x62,0x66,0x0C,0x18,0x30,0x66,0x46,0x00},  /* 37 % */
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00},  /* 38 & */
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00},  /* 39 ' */
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00},  /* 40 ( */
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},  /* 41 ) */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},  /* 42 * */
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},  /* 43 + */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30},  /* 44 , */
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},  /* 45 - */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},  /* 46 . */
    {0x02,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},  /* 47 / */
    {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00},  /* 48 0 */
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},  /* 49 1 */
    {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00},  /* 50 2 */
    {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00},  /* 51 3 */
    {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00},  /* 52 4 */
    {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00},  /* 53 5 */
    {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00},  /* 54 6 */
    {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00},  /* 55 7 */
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00},  /* 56 8 */
    {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00},  /* 57 9 */
    {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00},  /* 58 : */
    {0x00,0x18,0x18,0x00,0x18,0x18,0x30,0x00},  /* 59 ; */
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00},  /* 60 < */
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00},  /* 61 = */
    {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00},  /* 62 > */
    {0x3C,0x66,0x06,0x0C,0x18,0x00,0x18,0x00},  /* 63 ? */
    {0x3C,0x66,0x6E,0x6E,0x60,0x62,0x3C,0x00},  /* 64 @ */
    {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00},  /* 65 A */
    {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00},  /* 66 B */
    {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00},  /* 67 C */
    {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00},  /* 68 D */
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00},  /* 69 E */
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00},  /* 70 F */
    {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3E,0x00},  /* 71 G */
    {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00},  /* 72 H */
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},  /* 73 I */
    {0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0x00},  /* 74 J */
    {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00},  /* 75 K */
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00},  /* 76 L */
    {0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0xC6,0x00},  /* 77 M */
    {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00},  /* 78 N */
    {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},  /* 79 O */
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00},  /* 80 P */
    {0x3C,0x66,0x66,0x66,0x6E,0x3C,0x06,0x00},  /* 81 Q */
    {0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00},  /* 82 R */
    {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00},  /* 83 S */
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00},  /* 84 T */
    {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},  /* 85 U */
    {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00},  /* 86 V */
    {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00},  /* 87 W */
    {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00},  /* 88 X */
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00},  /* 89 Y */
    {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00},  /* 90 Z */
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},  /* 91 [ */
    {0x40,0x60,0x30,0x18,0x0C,0x06,0x02,0x00},  /* 92 \ */
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},  /* 93 ] */
    {0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00},  /* 94 ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00},  /* 95 _ */
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00},  /* 96 ` */
    {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00},  /* 97 a */
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00},  /* 98 b */
    {0x00,0x00,0x3C,0x66,0x60,0x66,0x3C,0x00},  /* 99 c */
    {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00},  /* 100 d */
    {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00},  /* 101 e */
    {0x1C,0x30,0x7C,0x30,0x30,0x30,0x30,0x00},  /* 102 f */
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x3C},  /* 103 g */
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00},  /* 104 h */
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},  /* 105 i */
    {0x0C,0x00,0x1C,0x0C,0x0C,0x0C,0x6C,0x38},  /* 106 j */
    {0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00},  /* 107 k */
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},  /* 108 l */
    {0x00,0x00,0xEC,0xFE,0xD6,0xC6,0xC6,0x00},  /* 109 m */
    {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00},  /* 110 n */
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00},  /* 111 o */
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60},  /* 112 p */
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06},  /* 113 q */
    {0x00,0x00,0x7C,0x66,0x60,0x60,0x60,0x00},  /* 114 r */
    {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00},  /* 115 s */
    {0x30,0x30,0x7C,0x30,0x30,0x30,0x1C,0x00},  /* 116 t */
    {0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00},  /* 117 u */
    {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00},  /* 118 v */
    {0x00,0x00,0xC6,0xC6,0xD6,0xFE,0x6C,0x00},  /* 119 w */
    {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00},  /* 120 x */
    {0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C},  /* 121 y */
    {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00},  /* 122 z */
};

/* Bold 8x8 digit font (0-9) - much more readable */
static const unsigned char fps_font[10][8] = {
    {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00},  /* 0 */
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},  /* 1 */
    {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00},  /* 2 */
    {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00},  /* 3 */
    {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00},  /* 4 */
    {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00},  /* 5 */
    {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00},  /* 6 */
    {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00},  /* 7 */
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00},  /* 8 */
    {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00},  /* 9 */
};

/* Forward declarations for FPS drawing */
static void draw_digit(ULONG *fb, int fb_stride, int x, int y, int digit, ULONG color);
static void draw_fps(ULONG *fb, int fb_stride, int screen_w);
static void draw_char(ULONG *fb, int fb_stride, int x, int y, int ch, ULONG color);
static void draw_text(ULONG *fb, int fb_stride, int x, int y, const char *text, ULONG color);

/* 16-bit screen variants */
static void draw_fps16(UWORD *fb, int fb_stride, int screen_w);
static void draw_fps16_at(UWORD *fb, int fb_stride, int x, int y,
                          UWORD yellow, UWORD black);
static void draw_text16_c(UWORD *fb, int fb_stride, int x, int y, const char *text,
                          UWORD color, UWORD shadow, int max_x);
static void draw_text16(UWORD *fb, int fb_stride, int x, int y, const char *text, UWORD color);

/* RGB565 to RGBA lookup table (65536 entries) */
static ULONG rgb565_to_rgba[65536];

/* Darkened version for scanlines (pre-computed per scanline_mode) */
static ULONG rgb565_to_rgba_dark[65536];

/* 16-bit screen support (screendepth=16 in the config).
 * GPGX renders RGB565 big-endian natively, so on a PIXFMT_RGB16 screen the
 * blit is a straight row copy - half the bandwidth of the 32-bit path and
 * zero per-pixel conversion. Other 15/16-bit formats go through a UWORD LUT.
 *
 * The 16-bit LUTs overlay the 32-bit tables (only one depth is ever active;
 * build is -fno-strict-aliasing), so they cost no extra memory. */
static int req_depth = 32;            /* video_cgx_set_depth(): 16 or 32 */
static int video_is_16 = 0;           /* opened screen is 15/16-bit */
static int screen_bpp = 4;            /* bytes per pixel of opened screen */
static int lut16_identity = 0;        /* screen is RGB16 BE: direct row copy */
static UWORD *rgb565_to_16 = (UWORD *)rgb565_to_rgba;
static UWORD *rgb565_to_16_dark = (UWORD *)rgb565_to_rgba_dark;

/* Convert one RGB565 value to the given 15/16-bit CGX pixel format */
static UWORD map_565_to_fmt(unsigned int v, ULONG fmt)
{
    unsigned int r = (v >> 11) & 0x1F;
    unsigned int g = (v >> 5)  & 0x3F;
    unsigned int b =  v        & 0x1F;
    unsigned int out;

    switch (fmt)
    {
        case PIXFMT_RGB16:   out = v; break;
        case PIXFMT_RGB16PC: out = v; goto swap;
        case PIXFMT_BGR16:   out = (b << 11) | (g << 5) | r; break;
        case PIXFMT_BGR16PC: out = (b << 11) | (g << 5) | r; goto swap;
        case PIXFMT_RGB15:   out = (r << 10) | ((g >> 1) << 5) | b; break;
        case PIXFMT_RGB15PC: out = (r << 10) | ((g >> 1) << 5) | b; goto swap;
        case PIXFMT_BGR15:   out = (b << 10) | ((g >> 1) << 5) | r; break;
        case PIXFMT_BGR15PC: out = (b << 10) | ((g >> 1) << 5) | r; goto swap;
        default:             out = v; break;
    }
    return (UWORD)out;
swap:
    return (UWORD)(((out & 0xFF) << 8) | ((out >> 8) & 0xFF));
}

/****************************************************************************
 * Build the RGB565 -> native 15/16-bit LUT (and darkened scanline variant)
 ****************************************************************************/
static void init_color_table16(ULONG fmt)
{
    long i;
    int dark_shift;

    switch (scanline_mode)
    {
        case 1: dark_shift = 4; break;
        case 2: dark_shift = 3; break;
        case 3: dark_shift = 2; break;
        default: dark_shift = 0; break;
    }

    lut16_identity = (fmt == PIXFMT_RGB16);

    for (i = 0; i < 65536; i++)
    {
        rgb565_to_16[i] = map_565_to_fmt((unsigned int)i, fmt);

        if (dark_shift > 0)
        {
            unsigned int r = (i >> 11) & 0x1F;
            unsigned int g = (i >> 5)  & 0x3F;
            unsigned int b =  i        & 0x1F;
            unsigned int dark565;
            r -= r >> dark_shift;
            g -= g >> dark_shift;
            b -= b >> dark_shift;
            dark565 = (r << 11) | (g << 5) | b;
            rgb565_to_16_dark[i] = map_565_to_fmt(dark565, fmt);
        }
        else
        {
            rgb565_to_16_dark[i] = rgb565_to_16[i];
        }
    }
}

/****************************************************************************
 * Initialize RGB565 to 32-bit lookup table
 * Adapts byte order to match screen's native pixel format
 ****************************************************************************/
static void init_color_table(void)
{
    int i;
    int dark_shift = 0;
    
    /* Determine darkening shift for scanlines */
    switch (scanline_mode)
    {
        case 1: dark_shift = 4; break;  /* Subtle: darken by 1/16 */
        case 2: dark_shift = 3; break;  /* Medium: darken by 1/8 */
        case 3: dark_shift = 2; break;  /* Strong: darken by 1/4 */
        default: dark_shift = 0; break;
    }
    
    for (i = 0; i < 65536; i++)
    {
        /* RGB565: RRRRRGGGGGGBBBBB */
        unsigned int r = (i >> 11) & 0x1F;
        unsigned int g = (i >> 5) & 0x3F;
        unsigned int b = i & 0x1F;
        ULONG pixel, dark_pixel;
        
        /* Expand to 8-bit */
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        
        /* Build pixel in detected screen format.
         * Note: P96 PIXFMT_ARGB32 (ID 11) on VideoCore/Emu68 has R and B
         * swapped in the actual framebuffer, so we write 0xAABBGGRR */
        switch (screen_pixfmt)
        {
            case PIXFMT_ARGB32:  /* VideoCore: actually 0xAABBGGRR in memory */
                pixel = 0xFF000000 | (b << 16) | (g << 8) | r;
                break;
            case PIXFMT_RGBA32:  /* 0xRRGGBBAA */
                pixel = (r << 24) | (g << 16) | (b << 8) | 0xFF;
                break;
            case PIXFMT_BGRA32:  /* 0xBBGGRRAA */
                pixel = (b << 24) | (g << 16) | (r << 8) | 0xFF;
                break;
            default:
                pixel = 0xFF000000 | (b << 16) | (g << 8) | r;
                break;
        }
        rgb565_to_rgba[i] = pixel;
        
        /* Darkened version for scanlines */
        if (dark_shift > 0)
        {
            unsigned int rd = r - (r >> dark_shift);
            unsigned int gd = g - (g >> dark_shift);
            unsigned int bd = b - (b >> dark_shift);
            
            switch (screen_pixfmt)
            {
                case PIXFMT_ARGB32:
                    dark_pixel = 0xFF000000 | (bd << 16) | (gd << 8) | rd;
                    break;
                case PIXFMT_RGBA32:
                    dark_pixel = (rd << 24) | (gd << 16) | (bd << 8) | 0xFF;
                    break;
                case PIXFMT_BGRA32:
                    dark_pixel = (bd << 24) | (gd << 16) | (rd << 8) | 0xFF;
                    break;
                default:
                    dark_pixel = 0xFF000000 | (bd << 16) | (gd << 8) | rd;
                    break;
            }
            rgb565_to_rgba_dark[i] = dark_pixel;
        }
        else
        {
            rgb565_to_rgba_dark[i] = rgb565_to_rgba[i];
        }
    }
}

/****************************************************************************
 * Get best RTG display mode for 32-bit
 ****************************************************************************/
static ULONG GetBestRTGMode(ULONG width, ULONG height)
{
    ULONG displayID;
    ULONG bgra_id;
    struct DisplayInfo dispinfo;
    
    if (!CyberGfxBase)
        return INVALID_ID;
    
    displayID = BestCModeIDTags(
        CYBRBIDTG_Depth, 32,
        CYBRBIDTG_NominalWidth, width,
        CYBRBIDTG_NominalHeight, height,
        TAG_DONE);
    
    if (displayID == INVALID_ID)
        return INVALID_ID;
    
    /* Try BGRA variant: on VideoCore, 32-bit modes come in groups of 4:
     * +0=ARGB, +1=ABGR, +2=RGBA, +3=BGRA
     * BGRA matches VideoCore native format */
    bgra_id = (displayID & ~0x03) | 0x03;
    if (bgra_id != displayID)
    {
        if (GetDisplayInfoData(NULL, (UBYTE *)&dispinfo, sizeof(dispinfo),
                               DTAG_DISP, bgra_id) > 0)
        {
            printf("Video: Switching to BGRA mode $%08lx (was $%08lx)\n",
                   bgra_id, displayID);
            displayID = bgra_id;
        }
    }
    
    return displayID;
}

/****************************************************************************
 * Initialize video subsystem
 ****************************************************************************/
int video_cgx_init(int width, int height, int mode)
{
    ULONG displayID;
    int open_depth = 32;

    (void)mode;  /* Unused - depth comes from video_cgx_set_depth() */
    
    if (video_initialized)
        video_cgx_shutdown();
    
    video_width = width;
    video_height = height;
    
    /* Determine screen size: an explicit request (GL output draws its own
     * scaled picture), else scanline mode or forced 2x, else 1x. */
    if (req_screen_w > 0 && req_screen_h > 0)
    {
        screen_width = req_screen_w;
        screen_height = req_screen_h;
        screen_is_2x = 0;
    }
    else if (scanline_mode > 0 || force_2x)
    {
        screen_width = width * 2;
        screen_height = height * 2;
        screen_is_2x = 1;
    }
    else
    {
        screen_width = width;
        screen_height = height;
        screen_is_2x = 0;
    }
    
    /* Initialize color lookup table */
    init_color_table();
    
    /* Allocate emulator buffer (16-bit RGB565) */
    video_pitch = width * 2;  /* 2 bytes per pixel for RGB565 */
    video_buffer = (unsigned char *)AllocMem(video_pitch * height, MEMF_PUBLIC | MEMF_CLEAR);
    if (!video_buffer)
    {
        printf("Video: Cannot allocate buffer\n");
        return 0;
    }
    
    /* Pick a display mode. screendepth=16 tries a 16-bit (then 15-bit)
     * mode first: half the blit bandwidth vs 32-bit, and on PIXFMT_RGB16
     * screens the blit becomes a straight row copy. */
    displayID = INVALID_ID;
    if (req_depth == 16 && CyberGfxBase)
    {
        displayID = BestCModeIDTags(
            CYBRBIDTG_Depth, 16,
            CYBRBIDTG_NominalWidth, screen_width,
            CYBRBIDTG_NominalHeight, screen_height,
            TAG_DONE);
        if (displayID != INVALID_ID)
        {
            open_depth = 16;
        }
        else
        {
            displayID = BestCModeIDTags(
                CYBRBIDTG_Depth, 15,
                CYBRBIDTG_NominalWidth, screen_width,
                CYBRBIDTG_NominalHeight, screen_height,
                TAG_DONE);
            if (displayID != INVALID_ID)
                open_depth = 15;
            else
                printf("Video: No 16-bit RTG mode, falling back to 32-bit\n");
        }
    }

    if (displayID == INVALID_ID)
    {
        open_depth = 32;
        displayID = GetBestRTGMode(screen_width, screen_height);
    }
    if (displayID == INVALID_ID)
    {
        printf("Video: No suitable RTG mode found\n");
        FreeMem(video_buffer, video_pitch * height);
        video_buffer = NULL;
        return 0;
    }

    /* Open RTG screen */
    video_screen = OpenScreenTags(NULL,
        SA_Width, screen_width,
        SA_Height, screen_height,
        SA_Depth, open_depth,
        SA_DisplayID, displayID,
        SA_Title, (ULONG)"Genesis Plus GX",
        SA_ShowTitle, FALSE,
        SA_Quiet, TRUE,
        SA_Type, CUSTOMSCREEN,
        TAG_DONE);
    
    if (!video_screen)
    {
        printf("Video: Cannot open %d-bit screen\n", open_depth);
        FreeMem(video_buffer, video_pitch * height);
        video_buffer = NULL;
        return 0;
    }

    /* Detect actual pixel format of the opened screen and build the right
     * LUT. A 15/16-bit format switches to the 16-bit blit path (direct row
     * copy when the screen is PIXFMT_RGB16 = GPGX's native RGB565 BE).
     * P96 RGBFB_ names describe component order MSB->LSB in a longword.
     * PIXFMT_ARGB32=11 (0xAARRGGBB), PIXFMT_BGRA32=12, PIXFMT_RGBA32=13 */
    {
        /* PIXFMT_LUT8 (0) .. PIXFMT_RGBA32 (13), cybergraphics.h */
        static const char *const pixfmt_names[] = {
            "LUT8", "RGB15", "BGR15", "RGB15PC", "BGR15PC", "RGB16", "BGR16",
            "RGB16PC", "BGR16PC", "RGB24", "BGR24", "ARGB32", "BGRA32", "RGBA32"
        };
        ULONG fmt = GetCyberMapAttr(video_screen->RastPort.BitMap, CYBRMATTR_PIXFMT);
        int old_fmt = screen_pixfmt;

        printf("Video: pixel format %lu (%s)\n", fmt,
               fmt < sizeof(pixfmt_names) / sizeof(pixfmt_names[0])
                   ? pixfmt_names[fmt] : "unknown");

        if (fmt >= PIXFMT_RGB15 && fmt <= PIXFMT_BGR16PC)
        {
            video_is_16 = 1;
            screen_bpp = 2;
            init_color_table16(fmt);
            printf("Video: 16-bit screen (%s)\n",
                   lut16_identity ? "RGB16 native, direct copy" : "via LUT");
        }
        else
        {
            video_is_16 = 0;
            screen_bpp = 4;

            if (fmt == PIXFMT_ARGB32)
            {
                screen_pixfmt = PIXFMT_ARGB32;
                printf("Video: Screen format ARGB32\n");
            }
            else if (fmt == PIXFMT_BGRA32)
            {
                screen_pixfmt = PIXFMT_BGRA32;
                printf("Video: Screen format BGRA32\n");
            }
            else if (fmt == PIXFMT_RGBA32)
            {
                screen_pixfmt = PIXFMT_RGBA32;
                printf("Video: Screen format RGBA32\n");
            }
            else
            {
                printf("Video: Unknown format (%lu), assuming ARGB32\n", fmt);
                screen_pixfmt = PIXFMT_ARGB32;
            }

            /* Rebuild LUT if format changed from initial assumption */
            if (screen_pixfmt != old_fmt)
            {
                printf("Video: Rebuilding color LUT for detected format\n");
                init_color_table();
            }
        }
    }
    
    /* Set up double buffering if VSync is enabled */
    dbl_buf_active = 0;
    if (vsync_enabled)
    {
        /* One reply port shared by both buffers' dbi_SafeMessage.
         * graphics.doc (AllocDBufInfo): the SafeMessage "is replied to when
         * it is safe to write to the old BitMap (the one which was installed
         * when you called ChangeVPBitMap)". Every successful swap replies
         * exactly one, and with two buffers the old BitMap is always the
         * next back buffer - so one port + one flag tracks it, whichever
         * ScreenBuffer's DBufInfo Intuition hands to ChangeVPBitMap. Same
         * pattern as the SafeToWrite fragment in that autodoc. */
        sb_safe_port = CreateMsgPort();

        if (sb_safe_port)
        {
            /* Allocate two screen buffers */
            sb[0] = AllocScreenBuffer(video_screen, NULL, SB_SCREEN_BITMAP);
            sb[1] = AllocScreenBuffer(video_screen, NULL, 0);

            if (sb[0] && sb[1])
            {
                sb[0]->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = sb_safe_port;
                sb[1]->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = sb_safe_port;
                sb_current = 0;
                sb_safe_to_write = TRUE;
                dbl_buf_active = 1;
                printf("Video: Double-buffered VSync enabled\n");
            }
            else
            {
                printf("Video: AllocScreenBuffer failed, falling back to single buffer\n");
                if (sb[0]) { FreeScreenBuffer(video_screen, sb[0]); sb[0] = NULL; }
                if (sb[1]) { FreeScreenBuffer(video_screen, sb[1]); sb[1] = NULL; }
                DeleteMsgPort(sb_safe_port); sb_safe_port = NULL;
            }
        }
        else
        {
            printf("Video: Cannot create MsgPort for double buffer\n");
        }
    }
    
    /* Open borderless window */
    video_window = OpenWindowTags(NULL,
        WA_CustomScreen, (ULONG)video_screen,
        WA_Left, 0,
        WA_Top, 0,
        WA_Width, screen_width,
        WA_Height, screen_height,
        WA_Borderless, TRUE,
        WA_Backdrop, TRUE,
        WA_Activate, TRUE,
        WA_RMBTrap, TRUE,
        WA_NoCareRefresh, TRUE,
        WA_IDCMP, IDCMP_RAWKEY | IDCMP_CLOSEWINDOW | IDCMP_ACTIVEWINDOW | IDCMP_INACTIVEWINDOW,
        TAG_DONE);
    
    if (!video_window)
    {
        printf("Video: Cannot open window\n");
        /* Clean up double buffer resources */
        if (sb[1]) { FreeScreenBuffer(video_screen, sb[1]); sb[1] = NULL; }
        if (sb[0]) { FreeScreenBuffer(video_screen, sb[0]); sb[0] = NULL; }
        if (sb_safe_port) { DeleteMsgPort(sb_safe_port); sb_safe_port = NULL; }
        dbl_buf_active = 0;
        CloseScreen(video_screen);
        video_screen = NULL;
        FreeMem(video_buffer, video_pitch * height);
        video_buffer = NULL;
        return 0;
    }
    
    /* Hide mouse pointer */
    SetPointer(video_window, blank_pointer, 1, 1, 0, 0);
    
    /* Make sure window is active */
    ActivateWindow(video_window);
    WindowToFront(video_window);
    
    /* Read back actual screen dimensions - RTG may open a different size
     * than requested if the exact mode isn't available. Using the wrong
     * size causes dest_y to be miscalculated, clipping the bottom. */
    screen_width  = video_screen->Width;
    screen_height = video_screen->Height;

    video_initialized = 1;
    video_depth = video_is_16 ? 16 : 32;

    return 1;
}

/****************************************************************************
 * Shutdown video subsystem
 ****************************************************************************/
void video_cgx_shutdown(void)
{
    if (video_window)
    {
        ClearPointer(video_window);
        CloseWindow(video_window);
        video_window = NULL;
    }
    
    /* Clean up double-buffer resources before closing screen */
    if (dbl_buf_active)
    {
        /* A swap still in flight replies its SafeMessage to sb_safe_port:
         * collect it before the port is deleted. */
        if (!sb_safe_to_write && sb_safe_port)
        {
            while (!GetMsg(sb_safe_port))
                WaitPort(sb_safe_port);
            sb_safe_to_write = TRUE;
        }

        /* No ChangeScreenBuffer() back to sb[0] here: intuition.doc says
         * restoring the original buffer before closing is not required
         * ("Simply FreeScreenBuffer() each buffer ... then close the
         * screen"), and that swap would reply a SafeMessage to the port
         * deleted just below. */
        if (sb[1]) { FreeScreenBuffer(video_screen, sb[1]); sb[1] = NULL; }
        if (sb[0]) { FreeScreenBuffer(video_screen, sb[0]); sb[0] = NULL; }
        if (sb_safe_port) { DeleteMsgPort(sb_safe_port); sb_safe_port = NULL; }
        dbl_buf_active = 0;
    }
    
    if (video_screen)
    {
        CloseScreen(video_screen);
        video_screen = NULL;
    }
    
    if (video_buffer)
    {
        FreeMem(video_buffer, video_pitch * video_height);
        video_buffer = NULL;
    }
    
    video_initialized = 0;
}

/****************************************************************************
 * Get video buffer pointer
 ****************************************************************************/
unsigned char *video_cgx_get_buffer(void)
{
    return video_buffer;
}

/****************************************************************************
 * Get video buffer pitch
 ****************************************************************************/
int video_cgx_get_pitch(void)
{
    return video_pitch;
}

/****************************************************************************
 * Clear the entire screen to black
 ****************************************************************************/
void video_cgx_clear(void)
{
    APTR handle;
    ULONG fb_bpr;
    UBYTE *fb_base;
    int buf_idx;
    
    if (!video_initialized || !video_screen)
        return;
    
    if (!CyberGfxBase)
        return;
    
    if (dbl_buf_active)
    {
        /* Clear both screen buffers */
        for (buf_idx = 0; buf_idx < 2; buf_idx++)
        {
            handle = LockBitMapTags(sb[buf_idx]->sb_BitMap,
                                   LBMI_BYTESPERROW, (ULONG)&fb_bpr,
                                   LBMI_BASEADDRESS, (ULONG)&fb_base,
                                   TAG_DONE);
            if (handle)
            {
                int line;
                for (line = 0; line < screen_height; line++)
                {
                    memset(fb_base + line * fb_bpr, 0, screen_width * screen_bpp);
                }
                UnLockBitMap(handle);
            }
        }
    }
    else
    {
        handle = LockBitMapTags(video_screen->RastPort.BitMap,
                               LBMI_BYTESPERROW, (ULONG)&fb_bpr,
                               LBMI_BASEADDRESS, (ULONG)&fb_base,
                               TAG_DONE);
        
        if (handle)
        {
            int line;
            for (line = 0; line < screen_height; line++)
            {
                memset(fb_base + line * fb_bpr, 0, screen_width * screen_bpp);
            }
            UnLockBitMap(handle);
        }
    }
}

/****************************************************************************
 * Update display from buffer - convert RGB565 to RGBA32 with optional scanlines
 ****************************************************************************/
void video_cgx_update(int x, int y, int w, int h)
{
    APTR handle;
    ULONG fb_bpr;
    UBYTE *fb_base;
    int dest_x, dest_y;
    struct BitMap *target_bm;
    int back_buf = 0;
    
    if (!video_initialized || !video_window || !video_buffer)
        return;
    
    /* Calculate destination - center on screen */
    if (screen_is_2x)
    {
        dest_x = (screen_width - w * 2) / 2;
        dest_y = (screen_height - h * 2) / 2;
    }
    else
    {
        dest_x = (screen_width - w) / 2;
        dest_y = (screen_height - h) / 2;
    }
    if (dest_x < 0) dest_x = 0;
    if (dest_y < 0) dest_y = 0;
    
    if (dbl_buf_active)
    {
        /* Double-buffered path:
         * Render to back buffer, swap at vblank for tear-free display.
         * NON-BLOCKING: we never wait for vblank. The timer in main()
         * controls frame pacing so audio stays at correct speed.
         * If back buffer isn't safe yet, render to front (minor tear risk). */
        /* Non-blocking check: has the last swap's SafeMessage come back,
         * i.e. is the buffer it replaced (= the back buffer) off-screen? */
        if (!sb_safe_to_write && GetMsg(sb_safe_port))
            sb_safe_to_write = TRUE;

        if (sb_safe_to_write)
        {
            back_buf = 1 - sb_current;
        }
        else
        {
            /* Back buffer still being displayed - render to front instead.
             * This avoids stalling on vblank at cost of possible tear. */
            back_buf = sb_current;
        }
        
        /* Lock the target buffer's bitmap */
        target_bm = sb[back_buf]->sb_BitMap;
    }
    else
    {
        /* Single-buffer path: render directly to screen */
        target_bm = video_screen->RastPort.BitMap;
    }
    
    /* Lock framebuffer for direct access */
    handle = LockBitMapTags(target_bm,
                           LBMI_BYTESPERROW, (ULONG)&fb_bpr,
                           LBMI_BASEADDRESS, (ULONG)&fb_base,
                           TAG_DONE);
    
    if (handle)
    {
      if (video_is_16)
      {
        /* ---- 15/16-bit screen: half the bandwidth of the 32-bit path ---- */
        UWORD *fb16 = (UWORD *)(fb_base + dest_y * fb_bpr) + dest_x;
        ULONG fb_wstride = fb_bpr / 2;
        UWORD *src16 = (UWORD *)(video_buffer + y * video_pitch + x * 2);
        int stride16 = video_pitch / 2;
        int ln, col;

        if (screen_is_2x && scanline_mode > 0)
        {
            /* 2x with scanlines - dual UWORD LUT */
            for (ln = 0; ln < h; ln++)
            {
                UWORD *srcp = src16 + ln * stride16;
                UWORD *dst1 = fb16 + (ln * 2) * fb_wstride;
                UWORD *dst2 = fb16 + (ln * 2 + 1) * fb_wstride;
                for (col = 0; col < w; col++)
                {
                    UWORD px = rgb565_to_16[*srcp];
                    UWORD dk = rgb565_to_16_dark[*srcp];
                    srcp++;
                    *dst1++ = px;
                    *dst1++ = px;
                    *dst2++ = dk;
                    *dst2++ = dk;
                }
            }
        }
        else if (screen_is_2x)
        {
            /* 2x without scanlines */
            for (ln = 0; ln < h; ln++)
            {
                UWORD *srcp = src16 + ln * stride16;
                UWORD *dst1 = fb16 + (ln * 2) * fb_wstride;
                UWORD *dst2 = fb16 + (ln * 2 + 1) * fb_wstride;
                for (col = 0; col < w; col++)
                {
                    UWORD px = rgb565_to_16[*srcp++];
                    *dst1++ = px;
                    *dst1++ = px;
                    *dst2++ = px;
                    *dst2++ = px;
                }
            }
        }
        else if (lut16_identity)
        {
            /* 1:1 on a native RGB16 screen: GPGX output IS the screen
             * format - straight row copy, zero per-pixel work */
            for (ln = 0; ln < h; ln++)
            {
                CopyMem(src16 + ln * stride16, fb16 + ln * fb_wstride, w * 2);
            }
        }
        else
        {
            /* 1:1 via UWORD LUT (RGB16PC / BGR16 / 15-bit screens) */
            for (ln = 0; ln < h; ln++)
            {
                UWORD *srcp = src16 + ln * stride16;
                UWORD *dstp = fb16 + ln * fb_wstride;
                for (col = 0; col < w; col++)
                {
                    *dstp++ = rgb565_to_16[*srcp++];
                }
            }
        }

        /* Draw FPS counter if enabled */
        if (show_fps)
        {
            draw_fps16((UWORD *)fb_base, fb_wstride, screen_width);
        }

        /* Draw OSD message if active */
        if (osd_text_timer > 0 && osd_text[0] != 0)
        {
            draw_text16(fb16, fb_wstride, 4, 4, osd_text, rgb565_to_16[0xFFFF]);
            osd_text_timer--;
            if (osd_text_timer == 0)
                osd_text[0] = 0;
        }
      }
      else
      {
        ULONG *fb = (ULONG *)((UBYTE *)fb_base + dest_y * fb_bpr) + dest_x;
        ULONG fb_stride = fb_bpr / sizeof(ULONG);
        UWORD *src_base = (UWORD *)(video_buffer + y * video_pitch + x * 2);
        int src_stride = video_pitch / 2;
        int line;

        if (screen_is_2x && scanline_mode > 0)
        {
            /* 2x with scanlines - uses pre-computed dark LUT */
            for (line = 0; line < h; line++)
            {
                UWORD *srcp = src_base + line * src_stride;
                ULONG *dst1 = fb + (line * 2) * fb_stride;
                ULONG *dst2 = fb + (line * 2 + 1) * fb_stride;
                
#ifdef USE_ASM_VIDEO
                Blit565toRGBA_2x_scanline_asm(srcp, dst1, dst2,
                                               rgb565_to_rgba, rgb565_to_rgba_dark, w);
#else
                int col;
                for (col = 0; col < w; col++)
                {
                    ULONG pixel = rgb565_to_rgba[*srcp];
                    ULONG dark_pixel = rgb565_to_rgba_dark[*srcp];
                    srcp++;
                    
                    /* Main line - double pixels horizontally */
                    *dst1++ = pixel;
                    *dst1++ = pixel;
                    
                    /* Scanline - darkened */
                    *dst2++ = dark_pixel;
                    *dst2++ = dark_pixel;
                }
#endif
            }
        }
        else if (screen_is_2x)
        {
            /* 2x without scanlines (scale2x) */
            for (line = 0; line < h; line++)
            {
                UWORD *srcp = src_base + line * src_stride;
                ULONG *dst1 = fb + (line * 2) * fb_stride;
                ULONG *dst2 = fb + (line * 2 + 1) * fb_stride;
#ifdef USE_ASM_VIDEO
                Blit565toRGBA_2x_asm(srcp, dst1, dst2, rgb565_to_rgba, w);
#else
                int col;
                for (col = 0; col < w; col++)
                {
                    ULONG pixel = rgb565_to_rgba[*srcp++];
                    *dst1++ = pixel;
                    *dst1++ = pixel;
                    *dst2++ = pixel;
                    *dst2++ = pixel;
                }
#endif
            }
        }
        else
        {
            /* 1:1 direct rendering */
            for (line = 0; line < h; line++)
            {
                UWORD *srcp = src_base + line * src_stride;
                ULONG *dstp = fb + line * fb_stride;
#ifdef USE_ASM_VIDEO
                Blit565toRGBA_asm(srcp, dstp, rgb565_to_rgba, w);
#else
                int col;
                for (col = 0; col < w; col++)
                {
                    *dstp++ = rgb565_to_rgba[*srcp++];
                }
#endif
            }
        }
        
        /* Draw FPS counter if enabled */
        if (show_fps)
        {
            draw_fps(fb - dest_y * fb_stride - dest_x, fb_stride, screen_width);
        }

        /* Draw OSD message if active (drawn within viewport so next frame clears it) */
        if (osd_text_timer > 0 && osd_text[0] != 0)
        {
            ULONG white = 0xFFFFFFFF;  /* White is same in all 32-bit formats */
            draw_text(fb, fb_stride, 4, 4, osd_text, white);
            osd_text_timer--;
            if (osd_text_timer == 0)
                osd_text[0] = 0;
        }
      }

        UnLockBitMap(handle);
        
        if (dbl_buf_active && back_buf != sb_current)
        {
            /* We rendered to the back buffer - swap at next vblank. The
             * buffer being replaced is unsafe until its SafeMessage returns.
             * A failed swap (menus etc.) sends no message and changes
             * nothing, so the back buffer simply stays writable. */
            if (ChangeScreenBuffer(video_screen, sb[back_buf]))
            {
                sb_current = back_buf;
                sb_safe_to_write = FALSE;
            }
        }
        /* If back_buf == sb_current, we rendered to the front buffer
         * (back wasn't safe yet). No swap needed - it's already visible,
         * though there may be minor tearing on this frame. */
    }
}

/****************************************************************************
 * Get window pointer for input handling
 ****************************************************************************/
struct Window *video_cgx_get_window(void)
{
    return video_window;
}

/****************************************************************************
 * Check if CyberGraphX is available
 ****************************************************************************/
int video_cgx_available(void)
{
    return CyberGfxBase != NULL;
}

/****************************************************************************
 * Set scanline mode (0=off, 1=subtle, 2=medium, 3=strong)
 * Must be called BEFORE video_cgx_init
 ****************************************************************************/
void video_cgx_set_scanlines(int level)
{
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    scanline_mode = level;
}

/****************************************************************************
 * Get current scanline mode
 ****************************************************************************/
int video_cgx_get_scanlines(void)
{
    return scanline_mode;
}

/****************************************************************************
 * Set preferred screen depth (16 or 32) - call before video_cgx_init.
 * 16 tries a 16-bit (then 15-bit) RTG mode; falls back to 32-bit.
 ****************************************************************************/
void video_cgx_set_depth(int depth)
{
    req_depth = (depth == 16) ? 16 : 32;
}

/****************************************************************************
 * Enable/disable VSync
 ****************************************************************************/
void video_cgx_set_vsync(int enable)
{
    vsync_enabled = enable ? 1 : 0;
}

int video_cgx_get_vsync(void)
{
    return vsync_enabled;
}

/****************************************************************************
 * Explicit screen size (e.g. for GL output). 0,0 = automatic.
 * Must be called BEFORE video_cgx_init.
 ****************************************************************************/
void video_cgx_set_screen_size(int w, int h)
{
    req_screen_w = (w > 0 && h > 0) ? w : 0;
    req_screen_h = (w > 0 && h > 0) ? h : 0;
}

/****************************************************************************
 * The screen and its bitmap (valid between video_cgx_init and shutdown).
 ****************************************************************************/
struct BitMap *video_cgx_get_screen_bitmap(void)
{
    return video_screen ? video_screen->RastPort.BitMap : NULL;
}

int video_cgx_get_screen_width(void)  { return screen_width; }
int video_cgx_get_screen_height(void) { return screen_height; }
int video_cgx_get_screen_bpp(void)    { return screen_bpp; }

/****************************************************************************
 * Force 2x scaling (for small resolutions like Game Gear 160x144)
 * Must be called before video_cgx_init()
 ****************************************************************************/
void video_cgx_set_force_2x(int enable)
{
    force_2x = enable ? 1 : 0;
}

/****************************************************************************
 * Toggle fullscreen (placeholder)
 ****************************************************************************/
void video_cgx_toggle_fullscreen(void)
{
    /* Already fullscreen */
}

/****************************************************************************
 * Set window title
 ****************************************************************************/
void video_cgx_set_title(const char *title)
{
    if (video_screen)
    {
        SetWindowTitles(video_window, (STRPTR)title, (STRPTR)title);
    }
}

/****************************************************************************
 * Set palette (not used in 32-bit mode, but kept for API compatibility)
 ****************************************************************************/
void video_cgx_set_palette(unsigned short *palette)
{
    /* Not needed for 32-bit mode */
    (void)palette;
}

/****************************************************************************
 * Enable/disable FPS display
 ****************************************************************************/
void video_cgx_show_fps(int enable)
{
    show_fps = enable;
    fps_frame_count = 0;
    fps_current = 0;
    fps_last_time = 0;
}

/****************************************************************************
 * Draw a digit at position using 8x8 font
 * Draws directly to framebuffer
 ****************************************************************************/
static void draw_digit(ULONG *fb, int fb_stride, int x, int y, int digit, ULONG color)
{
    int row, col;
    const unsigned char *glyph;
    
    if (digit < 0 || digit > 9)
        return;
    
    glyph = fps_font[digit];
    
    for (row = 0; row < 8; row++)
    {
        ULONG *dst = fb + (y + row) * fb_stride + x;
        unsigned char bits = glyph[row];
        
        for (col = 0; col < 8; col++)
        {
            if (bits & (0x80 >> col))
            {
                dst[col] = color;
            }
        }
    }
}

/****************************************************************************
 * Draw FPS counter on screen
 ****************************************************************************/
static void draw_fps(ULONG *fb, int fb_stride, int screen_w)
{
    int fps = fps_current;
    int x = screen_w - 24;  /* Position in top-right */
    int y = 4;
    ULONG yellow, black;
    int d1, d2;
    
    /* Build colors in detected screen format */
    if (screen_pixfmt == PIXFMT_ARGB32)
    {
        /* VideoCore ARGB: actually 0xAABBGGRR in memory */
        yellow = 0xFF00FFFF;  /* A=FF B=00 G=FF R=FF */
        black  = 0xFF000000;  /* A=FF B=00 G=00 R=00 */
    }
    else if (screen_pixfmt == PIXFMT_BGRA32)
    {
        yellow = 0x00FFFFFF;  /* B=00 G=FF R=FF A=FF */
        black  = 0x000000FF;  /* B=00 G=00 R=00 A=FF */
    }
    else  /* RGBA32 or fallback */
    {
        yellow = 0xFFFF00FF;  /* R=FF G=FF B=00 A=FF */
        black  = 0x000000FF;  /* R=00 G=00 B=00 A=FF */
    }
    
    if (fps > 99) fps = 99;
    if (fps < 0) fps = 0;
    
    d1 = fps / 10;
    d2 = fps % 10;
    
    /* Clear FPS area to prevent ghosting from previous frame's digits.
     * Area covers: x-2 to x+20, y-2 to y+10 (digits are 8x8 + shadow) */
    {
        int cx, cy;
        int x0 = x - 2;
        int y0 = y - 2;
        int x1 = x + 21;
        int y1 = y + 10;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        for (cy = y0; cy < y1; cy++)
        {
            ULONG *row = fb + cy * fb_stride + x0;
            for (cx = x0; cx < x1; cx++)
                *row++ = black;
        }
    }
    
    /* Draw black outline/shadow (draw in all 4 directions + diagonals) */
    if (d1 > 0)
    {
        draw_digit(fb, fb_stride, x - 1, y, d1, black);
        draw_digit(fb, fb_stride, x + 1, y, d1, black);
        draw_digit(fb, fb_stride, x, y - 1, d1, black);
        draw_digit(fb, fb_stride, x, y + 1, d1, black);
    }
    draw_digit(fb, fb_stride, x + 9, y, d2, black);
    draw_digit(fb, fb_stride, x + 11, y, d2, black);
    draw_digit(fb, fb_stride, x + 10, y - 1, d2, black);
    draw_digit(fb, fb_stride, x + 10, y + 1, d2, black);
    
    /* Draw yellow digits */
    if (d1 > 0)
    {
        draw_digit(fb, fb_stride, x, y, d1, yellow);
    }
    draw_digit(fb, fb_stride, x + 10, y, d2, yellow);
}

/****************************************************************************
 * Draw a single ASCII character at (x,y) in framebuffer
 ****************************************************************************/
static void draw_char(ULONG *fb, int fb_stride, int x, int y, int ch, ULONG color)
{
    int row, col;
    const unsigned char *glyph;

    /* Map ASCII 32-122 to font table */
    if (ch < 32 || ch > 122)
        return;

    glyph = ascii_font[ch - 32];

    for (row = 0; row < 8; row++)
    {
        ULONG *dst = fb + (y + row) * fb_stride + x;
        unsigned char bits = glyph[row];

        for (col = 0; col < 8; col++)
        {
            if (bits & (0x80 >> col))
            {
                dst[col] = color;
            }
        }
    }
}

/****************************************************************************
 * Draw text string at (x,y) in framebuffer with shadow
 ****************************************************************************/
static void draw_text(ULONG *fb, int fb_stride, int x, int y,
                      const char *text, ULONG color)
{
    /* Opaque black in the detected screen format. 0x000000FF is black only
     * where the alpha byte comes last (BGRA32/RGBA32); on the ARGB32 path
     * memory holds 0xAABBGGRR, where that value is transparent red instead.
     * Same mapping as draw_fps(). */
    ULONG shadow = (screen_pixfmt == PIXFMT_BGRA32 ||
                    screen_pixfmt == PIXFMT_RGBA32) ? 0x000000FF : 0xFF000000;
    int i;

    for (i = 0; text[i] != 0; i++)
    {
        int cx = x + i * 8;
        /* Draw shadow */
        draw_char(fb, fb_stride, cx + 1, y + 1, text[i], shadow);
        /* Draw foreground */
        draw_char(fb, fb_stride, cx, y, text[i], color);
    }
}

/****************************************************************************
 * 16-bit screen variants of the FPS/OSD drawing helpers.
 * Same glyphs; UWORD framebuffer writes and LUT-mapped colors.
 ****************************************************************************/
static void draw_glyph16(UWORD *fb, int fb_stride, int x, int y,
                         const unsigned char *glyph, UWORD color)
{
    int row, col;

    for (row = 0; row < 8; row++)
    {
        UWORD *dst = fb + (y + row) * fb_stride + x;
        unsigned char bits = glyph[row];

        for (col = 0; col < 8; col++)
        {
            if (bits & (0x80 >> col))
            {
                dst[col] = color;
            }
        }
    }
}

static void draw_digit16(UWORD *fb, int fb_stride, int x, int y, int digit, UWORD color)
{
    if (digit < 0 || digit > 9)
        return;
    draw_glyph16(fb, fb_stride, x, y, fps_font[digit], color);
}

static void draw_fps16(UWORD *fb, int fb_stride, int screen_w)
{
    /* Colours in the 16-bit screen format, via the screen LUT */
    draw_fps16_at(fb, fb_stride, screen_w - 24, 4,
                  rgb565_to_16[0xFFE0], rgb565_to_16[0x0000]);
}

/* FPS digits with their left edge at x, in caller-supplied colours */
static void draw_fps16_at(UWORD *fb, int fb_stride, int x, int y,
                          UWORD yellow, UWORD black)
{
    int fps = fps_current;
    int d1, d2;

    if (fps > 99) fps = 99;
    if (fps < 0) fps = 0;

    d1 = fps / 10;
    d2 = fps % 10;

    /* Clear FPS area to prevent ghosting */
    {
        int cx, cy;
        int x0 = x - 2;
        int y0 = y - 2;
        int x1 = x + 21;
        int y1 = y + 10;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        for (cy = y0; cy < y1; cy++)
        {
            UWORD *row = fb + cy * fb_stride + x0;
            for (cx = x0; cx < x1; cx++)
                *row++ = black;
        }
    }

    /* Black outline */
    if (d1 > 0)
    {
        draw_digit16(fb, fb_stride, x - 1, y, d1, black);
        draw_digit16(fb, fb_stride, x + 1, y, d1, black);
        draw_digit16(fb, fb_stride, x, y - 1, d1, black);
        draw_digit16(fb, fb_stride, x, y + 1, d1, black);
    }
    draw_digit16(fb, fb_stride, x + 9, y, d2, black);
    draw_digit16(fb, fb_stride, x + 11, y, d2, black);
    draw_digit16(fb, fb_stride, x + 10, y - 1, d2, black);
    draw_digit16(fb, fb_stride, x + 10, y + 1, d2, black);

    /* Yellow digits */
    if (d1 > 0)
    {
        draw_digit16(fb, fb_stride, x, y, d1, yellow);
    }
    draw_digit16(fb, fb_stride, x + 10, y, d2, yellow);
}

static void draw_char16(UWORD *fb, int fb_stride, int x, int y, int ch, UWORD color)
{
    if (ch < 32 || ch > 122)
        return;
    draw_glyph16(fb, fb_stride, x, y, ascii_font[ch - 32], color);
}

static void draw_text16(UWORD *fb, int fb_stride, int x, int y,
                        const char *text, UWORD color)
{
    draw_text16_c(fb, fb_stride, x, y, text, color, rgb565_to_16[0x0000], 0x7FFF);
}

/* Text with a 1-pixel shadow. Characters that would reach max_x (the
 * shadow is one pixel wider) are not drawn, so a long message cannot
 * spill past the right edge of the row into the next one. */
static void draw_text16_c(UWORD *fb, int fb_stride, int x, int y, const char *text,
                          UWORD color, UWORD shadow, int max_x)
{
    int i;

    for (i = 0; text[i] != 0; i++)
    {
        int cx = x + i * 8;
        if (cx + 9 > max_x)
            break;
        draw_char16(fb, fb_stride, cx + 1, y + 1, text[i], shadow);
        draw_char16(fb, fb_stride, cx, y, text[i], color);
    }
}

/****************************************************************************
 * Draw the FPS counter and OSD message into the emulator's own RGB565
 * frame (used when GL output draws the picture). Colours are plain RGB565
 * here: rgb565_to_16[] is the SCREEN format, and on a 32-bit screen it
 * overlays the 32-bit LUT, so it must not be used for this buffer.
 * The OSD timer counts down exactly as in video_cgx_update().
 ****************************************************************************/
void video_cgx_overlay_rgb565(UWORD *buf, int pitch_px,
                              int vp_x, int vp_y, int vp_w, int vp_h)
{
    UWORD *origin;

    if (!buf || vp_x < 0 || vp_y < 0 || vp_w < 32 || vp_h < 16)
        return;
    if (vp_x + vp_w > pitch_px)
        return;
    origin = buf + (ULONG)vp_y * (ULONG)pitch_px + (ULONG)vp_x;

    if (show_fps)
        draw_fps16_at(origin, pitch_px, vp_w - 24, 4, 0xFFE0, 0x0000);

    if (osd_text_timer > 0 && osd_text[0] != 0)
    {
        draw_text16_c(origin, pitch_px, 4, 4, osd_text, 0xFFFF, 0x0000, vp_w);
        osd_text_timer--;
        if (osd_text_timer == 0)
            osd_text[0] = 0;
    }
}

/****************************************************************************
 * Set OSD message (called from main.c)
 ****************************************************************************/
void video_cgx_draw_text(const char *msg, int x, int y)
{
    /* Store the message - it will be rendered during next video update */
    (void)x;
    (void)y;
    strncpy(osd_text, msg, sizeof(osd_text) - 1);
    osd_text[sizeof(osd_text) - 1] = 0;
    osd_text_timer = 90;  /* ~1.5 seconds at 60fps */
}

/****************************************************************************
 * Update FPS counter - call once per frame from main loop
 ****************************************************************************/
void video_cgx_update_fps(void)
{
    struct timeval tv;
    ULONG current_time;
    
    if (!show_fps)
        return;
    
    fps_frame_count++;
    
    /* Get current time in milliseconds */
    if (!TimerBase)
        return;
    
    GetSysTime(&tv);
    current_time = tv.tv_secs * 1000 + tv.tv_micro / 1000;
    
    /* Update FPS every second */
    if (fps_last_time == 0)
    {
        fps_last_time = current_time;
    }
    else if (current_time - fps_last_time >= 1000)
    {
        fps_current = (fps_frame_count * 1000 + (current_time - fps_last_time) / 2) / (current_time - fps_last_time);
        fps_frame_count = 0;
        fps_last_time = current_time;
    }
}