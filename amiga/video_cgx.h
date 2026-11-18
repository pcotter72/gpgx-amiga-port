/****************************************************************************
 *  video_cgx.h
 *
 *  CyberGraphX/P96 Video Backend for Genesis Plus GX Amiga Port
 *
 ****************************************************************************/

#ifndef _VIDEO_CGX_H_
#define _VIDEO_CGX_H_

#include <intuition/intuition.h>

/* Video modes */
#define VIDEO_MODE_RTG16    0   /* 16-bit RTG (CyberGraphX/P96) */
#define VIDEO_MODE_RTG8     1   /* 8-bit RTG with palette */
#define VIDEO_MODE_AGA      2   /* Native AGA (HAM8/chunky) */

/* Initialize video subsystem */
/* Returns: 1 on success, 0 on failure */
int video_cgx_init(int width, int height, int mode);

/* Shutdown video subsystem */
void video_cgx_shutdown(void);

/* Get the video buffer pointer */
/* The emulator will render directly to this buffer */
unsigned char *video_cgx_get_buffer(void);

/* Get the video buffer pitch (bytes per line) */
int video_cgx_get_pitch(void);

/* Update display from buffer. With VSync the picture waits for the next
 * vertical blank if the EClock (in ticks) is still before sync_limit;
 * pass 0.0 to never wait. Returns: */
#define VSYNC_OFF    0  /* no VSync: drawn at once */
#define VSYNC_SYNCED 1  /* put on screen at the vertical blank */
#define VSYNC_LATE   2  /* called after sync_limit: drawn at once */
#define VSYNC_BUSY   3  /* on time, but the buffer swap was not possible */
int video_cgx_update(int x, int y, int w, int h, double sync_limit);

/* Clear entire screen to black */
void video_cgx_clear(void);

/* Set scanline mode (0=off, 1=subtle, 2=medium, 3=strong) */
/* Must be called BEFORE video_cgx_init */
void video_cgx_set_scanlines(int level);

/* Set preferred RTG screen depth: 32 (default) or 16.
 * 16 = RGB565 screen, half the blit bandwidth - recommended for classic
 * Amigas with Zorro RTG cards. Falls back to 32 if no 16/15-bit mode
 * exists. Must be called BEFORE video_cgx_init. */
void video_cgx_set_depth(int depth);

/* Get current scanline mode */
int video_cgx_get_scanlines(void);

/* Enable/disable VSync - call before video_cgx_init(), which measures how
 * the display reports its vertical blank. 2 = only by waiting for the blank
 * (WaitBOVP), never with two screen buffers: for the GPU path. */
void video_cgx_set_vsync(int enable);

/* The wait for the blank alone (video_cgx_update() does it itself): sleeps
 * until the next vertical blank if the EClock is before sync_limit and the
 * display is reached by waiting. Returns a VSYNC_ result. */
int video_cgx_vsync_wait(double sync_limit);
int video_cgx_get_vsync(void);

/* After video_cgx_init(): the display's refresh period in EClock ticks
 * (0.0: VSync off or no usable vertical blank), the EClock at the last
 * blank a frame waited for, how long that frame waited (us), and the name
 * of the method in use. */
double video_cgx_vsync_period(void);
double video_cgx_vsync_blank(void);
unsigned long video_cgx_vsync_wait_us(void);
const char *video_cgx_vsync_method(void);

/* Force 2x scaling (for Game Gear etc.) - call before video_cgx_init() */
void video_cgx_set_force_2x(int enable);

/* Explicit screen size, e.g. for GL output (0,0 = automatic).
 * Call before video_cgx_init(). */
void video_cgx_set_screen_size(int w, int h);

/* The opened screen: its bitmap, size and bytes per pixel. */
struct BitMap *video_cgx_get_screen_bitmap(void);
int video_cgx_get_screen_width(void);
int video_cgx_get_screen_height(void);
int video_cgx_get_screen_bpp(void);

/* Update the color palette (for 8-bit modes) */
/* palette: 256 entries of RGB565 */
void video_cgx_set_palette(unsigned short *palette);

/* Get the window pointer for input handling */
struct Window *video_cgx_get_window(void);

/* Check if CyberGraphX is available */
int video_cgx_available(void);

/* Toggle fullscreen (if supported) */
void video_cgx_toggle_fullscreen(void);

/* Set window title */
void video_cgx_set_title(const char *title);

/* Enable/disable FPS display */
void video_cgx_show_fps(int enable);

/* Update FPS counter (call once per frame) */
void video_cgx_update_fps(void);

/* Draw OSD text message (shown for ~1.5 seconds) */
void video_cgx_draw_text(const char *msg, int x, int y);

/* Draw the FPS counter / OSD message into an RGB565 frame (GL output).
 * (vp_x, vp_y, vp_w, vp_h) = visible area, pitch_px = pixels per row. */
void video_cgx_overlay_rgb565(UWORD *buf, int pitch_px,
                              int vp_x, int vp_y, int vp_w, int vp_h);

#endif /* _VIDEO_CGX_H_ */
