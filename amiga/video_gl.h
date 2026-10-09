/****************************************************************************
 *  video_gl.h
 *
 *  Optional PiStorm3D / minigl.library display path (Pi 4 / CM4 only).
 *  The emulator still renders into the normal RGB565 buffer; this uploads
 *  that buffer as a texture each frame and lets the V3D GPU draw it at the
 *  screen's full resolution, with effects. Built only with USE_MINIGL=1.
 *
 ****************************************************************************/

#ifndef _VIDEO_GL_H_
#define _VIDEO_GL_H_

#include <exec/types.h>
#include <graphics/gfx.h>

/* Effects (-gleffect=N) */
#define VGL_FX_SHARP      0   /* nearest, integer vertical scale          */
#define VGL_FX_SMOOTH     1   /* bilinear, full height                    */
#define VGL_FX_SCANLINES  2   /* nearest, integer scale, dark line pass   */
#define VGL_FX_COMPOSITE  3   /* bilinear + 50% blend with the next pixel */

/* Start GL output into the screen's bitmap (4 bytes per pixel, CGX-lockable).
 * upload_rgba: 0 = upload RGB565 directly, 1 = convert to RGBA8 first.
 * Returns 1 when GL output is active, 0 when the caller must keep using the
 * normal blit (the reason is printed). */
int  video_gl_init(struct BitMap *target, int width, int height,
                   int effect, int upload_rgba);

/* Draw one frame. src = start of the RGB565 buffer, src_pitch_px = pixels
 * per row, (vp_x, vp_y, vp_w, vp_h) = the visible area within it. */
void video_gl_update(const UWORD *src, int src_pitch_px,
                     int vp_x, int vp_y, int vp_w, int vp_h);

/* Test switches, call before video_gl_init():
 * ring = 1..3 frame textures rotated per frame (-glring=N),
 * half = upload top/bottom halves on alternate frames (-glhalf). */
void video_gl_set_experiments(int ring, int half);

/* Release GL. Must run before the screen (and its bitmap) is closed. */
void video_gl_shutdown(void);

#endif /* _VIDEO_GL_H_ */
