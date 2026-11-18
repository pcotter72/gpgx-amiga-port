/****************************************************************************
 *  video_gles.h
 *
 *  Optional GPU display path through ogles2.library (OpenGL ES 2.0 for a
 *  PiStorm with Emu68, github.com/ladislawv/ogles2pi3). The emulator still
 *  renders into the normal RGB565 buffer; each drawn frame is uploaded as a
 *  texture and the Pi's GPU draws it at the screen's resolution. Built only
 *  with USE_OGLES2=1; chosen with -gl or glout=1.
 *
 ****************************************************************************/

#ifndef _VIDEO_GLES_H_
#define _VIDEO_GLES_H_

#include <exec/types.h>
#include <intuition/intuition.h>

/* Effects (-gleffect=N, gleffect=N in the config) */
#define VGLES_FX_SHARP      0   /* nearest, integer vertical scale   */
#define VGLES_FX_SMOOTH     1   /* bilinear, full height             */
#define VGLES_FX_SCANLINES  2   /* sharp, the lower half of every source line darkened */
#define VGLES_FX_COMPOSITE  3   /* smooth, each pixel averaged with its right-hand neighbour */

/* Start GPU output into the window, which must be all of a 32-bit BGRA or
 * RGBA screen of the emulator's own (video_cgx.c opens exactly that).
 * Returns 1 when it is active, 0 when the caller must keep using the normal
 * blit (the reason is printed). */
int  video_gles_init(struct Window *win, int effect);

/* Draw one frame. src = start of the RGB565 buffer, src_pitch_px = pixels
 * per row, (vp_x, vp_y, vp_w, vp_h) = the visible area within it.
 * VSync: the frame is given to the GPU at the next vertical blank if the
 * EClock (ticks) is still before sync_limit; 0.0 = never wait. Returns the
 * VSYNC_ result of video_cgx_vsync_wait() (video_cgx.h). */
int  video_gles_update(const UWORD *src, int src_pitch_px,
                       int vp_x, int vp_y, int vp_w, int vp_h, double sync_limit);

/* Release the context. Must run before the window and screen are closed. */
void video_gles_shutdown(void);

#endif /* _VIDEO_GLES_H_ */
