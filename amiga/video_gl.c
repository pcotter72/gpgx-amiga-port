/****************************************************************************
 *  video_gl.c - optional PiStorm3D / minigl.library display path
 *
 *  PROTOTYPE. The emulator keeps rendering into the normal 320x240 RGB565
 *  buffer. Each frame that buffer is uploaded into a texture and the V3D
 *  GPU draws it into the screen's own bitmap at full screen resolution,
 *  with an optional effect. Everything after the upload costs the GPU, not
 *  the 68k; the upload itself still runs as 68k code (minigl converts to
 *  RGBA8 and tiles it for V3D on our side - SDK v3d_texture.h).
 *
 *  Uses the shared minigl.library (29.x) through the SDK's dispatch import
 *  library; if the library is missing, video_gl_init() fails and the caller
 *  keeps the normal CyberGraphX blit.
 *
 *  The minigl sources are not public yet, so three things are discovered at
 *  run time instead of assumed: whether an RGB565 upload is accepted (else
 *  fall back to RGBA8 converted here), every GL error (printed once), and
 *  the per-frame cost (printed at shutdown). The texture is 512x256, a power
 *  of two, so non-power-of-two support never matters, and whole buffer rows
 *  are uploaded, so GL_UNPACK_ROW_LENGTH is never needed.
 ****************************************************************************/

#include <stdio.h>
#include <string.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <devices/timer.h>

#include <proto/exec.h>
#include <proto/timer.h>
#include <proto/minigl.h>

#include "video_gl.h"

#ifdef USE_ASM_VIDEO
extern void Blit565toRGBA_asm(UWORD *src, ULONG *dst, ULONG *lut, int pixels);
#endif

#define TEX_W   512             /* power-of-two frame texture */
#define TEX_H   256
#define SRC_MAX_W 320           /* emulator buffer: 320 x 240 RGB565 */
#define SRC_MAX_H 240
#define SCAN_W  4               /* scanline texture: 2 bright rows, 2 dark */
#define SCAN_H  4

static int     gl_lib_open = 0;
static void   *gl_ctx = NULL;
#define RING_MAX 3
static GLuint  tex_ring[RING_MAX];     /* frame textures, rotated per frame */
static int     tex_count = 0;          /* how many were generated */
static int     ring_n = 1;             /* -glring=N experiment, 1 = off */
static int     ring_i = 0;
static int     half_mode = 0;          /* -glhalf experiment: halves alternate */
static int     half_phase = 0;
static GLuint  tex_scan = 0;
static int     gl_fx = VGL_FX_SMOOTH;
static int     gl_rgba = 0;             /* 1 = RGBA8 upload via rgba_lut */
static int     gl_w = 0, gl_h = 0;      /* screen (= context) size */
static ULONG  *rgba_lut = NULL;         /* 65536 RGB565 -> 0xRRGGBBAA */
static ULONG  *rgba_stage = NULL;       /* SRC_MAX_W * SRC_MAX_H */
static int     gl_errors_reported = 0;

/* Per-frame cost, printed at shutdown. Sums are doubles (see main.c). */
static ULONG   ef = 0;                  /* EClock Hz, 0 = no timing */
static double  t_up_sum = 0.0, t_draw_sum = 0.0;
static ULONG   t_up_max = 0, t_draw_max = 0, t_frames = 0;
static double  t_rows_sum = 0.0;       /* rows per upload, for the report */

static ULONG ec_us(const struct EClockVal *a, const struct EClockVal *b)
{
    return (ULONG)((double)(b->ev_lo - a->ev_lo) * 1000000.0 / (double)ef);
}

static void report_gl_error(const char *where)
{
    GLenum e = glGetError();
    if (e != GL_NO_ERROR && gl_errors_reported < 8)
    {
        printf("GL: error 0x%04lx after %s\n", (ULONG)e, where);
        gl_errors_reported++;
    }
}

static int build_rgba_lut(void)
{
    ULONG v, r, g, b;

    rgba_lut = (ULONG *)AllocMem(65536 * sizeof(ULONG), MEMF_ANY);
    rgba_stage = (ULONG *)AllocMem(SRC_MAX_W * SRC_MAX_H * sizeof(ULONG), MEMF_ANY);
    if (!rgba_lut || !rgba_stage)
        return 0;
    for (v = 0; v < 65536; v++)
    {
        r = (v >> 11) & 0x1F;  g = (v >> 5) & 0x3F;  b = v & 0x1F;
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        /* Big-endian longword stored as bytes R, G, B, A = GL_RGBA order */
        rgba_lut[v] = (r << 24) | (g << 16) | (b << 8) | 0xFF;
    }
    return 1;
}

/* Create one frame texture in the chosen format. Returns 1 if accepted. */
static int create_frame_texture(GLuint tex, const void *zero, int rgba)
{
    GLint filt = (gl_fx == VGL_FX_SHARP || gl_fx == VGL_FX_SCANLINES)
                 ? GL_NEAREST : GL_LINEAR;
    int n;

    /* Start clean. Bounded: a driver that never clears its error flag
     * must not hang the emulator here. */
    for (n = 0; n < 16 && glGetError() != GL_NO_ERROR; n++)
        ;
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    if (rgba)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEX_W, TEX_H, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, zero);
    else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, TEX_W, TEX_H, 0,
                     GL_RGB, MGL_UNSIGNED_SHORT_5_6_5, zero);
    return glGetError() == GL_NO_ERROR;
}

int video_gl_init(struct BitMap *target, int width, int height,
                  int effect, int upload_rgba)
{
    const GLubyte *s;
    UBYTE *zero;
    ULONG scan[SCAN_W * SCAN_H];
    int i, ok;
    struct EClockVal ev;

    if (!target || width <= 0 || height <= 0)
        return 0;

    gl_fx   = (effect >= VGL_FX_SHARP && effect <= VGL_FX_COMPOSITE) ? effect : VGL_FX_SMOOTH;
    gl_rgba = upload_rgba ? 1 : 0;
    gl_w    = width;
    gl_h    = height;
    gl_errors_reported = 0;
    t_up_sum = t_draw_sum = 0.0;
    t_up_max = t_draw_max = t_frames = 0;
    ef = TimerBase ? ReadEClock(&ev) : 0;

    if (!MiniGLOpen())
    {
        /* MiniGLOpen() also refuses, silently, a library from another
         * release than the SDK this was built with (SDK README). */
        printf("GL: minigl.library %d.1 not available (missing, or another version)"
               " - using the normal display\n", MINIGL_VERSION);
        return 0;
    }
    gl_lib_open = 1;

    gl_ctx = mglCreateContextFromBitMap(target);
    if (!gl_ctx)
    {
        printf("GL: cannot create a context on the screen bitmap - using the normal display\n");
        video_gl_shutdown();
        return 0;
    }

    s = glGetString(GL_RENDERER);
    if (s) printf("GL: renderer %s\n", (const char *)s);
    s = glGetString(GL_VERSION);
    if (s) printf("GL: version %s\n", (const char *)s);

    glViewport(0, 0, gl_w, gl_h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, (GLdouble)gl_w, (GLdouble)gl_h, 0.0, -1.0, 1.0);  /* y down */
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    report_gl_error("state setup");

    /* Zeroed initial image, big enough for either format (4 bytes/texel) */
    zero = (UBYTE *)AllocMem(TEX_W * TEX_H * 4, MEMF_ANY | MEMF_CLEAR);
    if (!zero)
    {
        printf("GL: out of memory - using the normal display\n");
        video_gl_shutdown();
        return 0;
    }

    if (half_mode && ring_n > 1)
    {
        printf("GL: -glhalf and -glring cannot be combined, using -glhalf only\n");
        ring_n = 1;
    }
    ring_i = 0;
    half_phase = 0;
    t_rows_sum = 0.0;

    glGenTextures(ring_n, tex_ring);
    tex_count = ring_n;
    ok = create_frame_texture(tex_ring[0], zero, gl_rgba);
    if (!ok && !gl_rgba)
    {
        printf("GL: RGB565 texture refused, falling back to RGBA8 upload\n");
        gl_rgba = 1;
        ok = create_frame_texture(tex_ring[0], zero, 1);
    }
    for (i = 1; ok && i < ring_n; i++)
        ok = create_frame_texture(tex_ring[i], zero, gl_rgba);
    FreeMem(zero, TEX_W * TEX_H * 4);
    if (!ok)
    {
        printf("GL: frame texture refused - using the normal display\n");
        video_gl_shutdown();
        return 0;
    }
    if (gl_rgba && !build_rgba_lut())
    {
        printf("GL: out of memory for the RGBA8 path - using the normal display\n");
        video_gl_shutdown();
        return 0;
    }

    if (gl_fx == VGL_FX_SCANLINES)
    {
        /* Rows 0-1 white, rows 2-3 dark grey, as RGBA bytes */
        for (i = 0; i < SCAN_W * SCAN_H; i++)
            scan[i] = (i < SCAN_W * 2) ? 0xFFFFFFFFUL : 0x8C8C8CFFUL;
        glGenTextures(1, &tex_scan);
        glBindTexture(GL_TEXTURE_2D, tex_scan);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SCAN_W, SCAN_H, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, scan);
        report_gl_error("scanline texture");
    }

    printf("GL: %dx%d, effect %d (%s), upload %s\n", gl_w, gl_h, gl_fx,
           gl_fx == VGL_FX_SHARP ? "sharp" : gl_fx == VGL_FX_SMOOTH ? "smooth" :
           gl_fx == VGL_FX_SCANLINES ? "scanlines" : "composite",
           gl_rgba ? "RGBA8 (converted here)" : "RGB565 (direct)");
    if (half_mode)
        printf("GL: experiment -glhalf: top and bottom halves uploaded on alternate frames\n");
    if (ring_n > 1)
        printf("GL: experiment -glring: %d frame textures rotated\n", ring_n);
    return 1;
}

/* One textured rectangle: screen rect (x0,y0)-(x1,y1), texture (0,0)-(u,v) */
static void quad(GLfloat x0, GLfloat y0, GLfloat x1, GLfloat y1,
                 GLfloat u0, GLfloat v0, GLfloat u1, GLfloat v1)
{
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(x0, y0);
    glTexCoord2f(u1, v0); glVertex2f(x1, y0);
    glTexCoord2f(u1, v1); glVertex2f(x1, y1);
    glTexCoord2f(u0, v1); glVertex2f(x0, y1);
    glEnd();
}

void video_gl_update(const UWORD *src, int src_pitch_px,
                     int vp_x, int vp_y, int vp_w, int vp_h)
{
    struct EClockVal e0, e1, e2;
    int up_w, up_h, dh, dw, n, row0, rows;
    GLfloat x0, y0, x1, y1, u0, u1, v;
    GLuint cur;

    if (!gl_ctx || !src)
        return;

    /* Clamp the visible area to the 320x240 buffer. Rows vp_y.. are
     * uploaded whole (full pitch), columns are selected by texcoords. */
    up_w = src_pitch_px;  if (up_w > SRC_MAX_W) up_w = SRC_MAX_W;
    if (vp_x < 0) vp_x = 0;
    if (vp_y < 0) vp_y = 0;
    if (vp_x + vp_w > up_w) vp_w = up_w - vp_x;
    up_h = vp_h;
    if (vp_y + up_h > SRC_MAX_H) up_h = SRC_MAX_H - vp_y;
    if (up_w <= 0 || up_h <= 0 || vp_w <= 0)
        return;
    src += (ULONG)vp_y * (ULONG)src_pitch_px;

    /* Which rows to upload. Normally all of them; -glhalf alternates the
     * top and bottom half (cut on a 4-row boundary, the RGBA8 utile) so
     * each upload covers half the area while the picture stays whole. */
    row0 = 0;
    rows = up_h;
    if (half_mode)
    {
        int h1 = (up_h / 2) & ~3;
        if (h1 <= 0)
            h1 = up_h;
        if (half_phase == 0) { row0 = 0;  rows = h1; }
        else                 { row0 = h1; rows = up_h - h1; }
        half_phase ^= 1;
    }
    cur = tex_ring[ring_i];

    if (ef) ReadEClock(&e0);

    /* Upload whole rows (rows are contiguous in the emulator buffer) */
    glBindTexture(GL_TEXTURE_2D, cur);
    if (rows > 0)
    {
        const UWORD *s = src + (ULONG)row0 * (ULONG)src_pitch_px;
        if (gl_rgba)
        {
#ifdef USE_ASM_VIDEO
            Blit565toRGBA_asm((UWORD *)s, rgba_stage, rgba_lut, up_w * rows);
#else
            for (n = 0; n < up_w * rows; n++)
                rgba_stage[n] = rgba_lut[s[n]];
#endif
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row0, up_w, rows,
                            GL_RGBA, GL_UNSIGNED_BYTE, rgba_stage);
        }
        else
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row0, up_w, rows,
                            GL_RGB, MGL_UNSIGNED_SHORT_5_6_5, s);
    }

    if (ef) ReadEClock(&e1);

    /* Destination: 4:3 picture. Sharp and scanline modes use the largest
     * whole multiple of the source height (even scanlines, no uneven rows);
     * smooth and composite fill the screen height. */
    if (gl_fx == VGL_FX_SHARP || gl_fx == VGL_FX_SCANLINES)
    {
        n  = gl_h / up_h;
        dh = (n > 0) ? n * up_h : gl_h;
    }
    else
        dh = gl_h;
    dw = dh * 4 / 3;
    if (dw > gl_w)
    {
        dw = gl_w;
        dh = gl_w * 3 / 4;
    }
    x0 = (GLfloat)((gl_w - dw) / 2);  x1 = x0 + (GLfloat)dw;
    y0 = (GLfloat)((gl_h - dh) / 2);  y1 = y0 + (GLfloat)dh;
    u0 = (GLfloat)vp_x / (GLfloat)TEX_W;
    u1 = (GLfloat)(vp_x + vp_w) / (GLfloat)TEX_W;
    v  = (GLfloat)up_h / (GLfloat)TEX_H;

    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_BLEND);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    quad(x0, y0, x1, y1, u0, 0.0f, u1, v);

    if (gl_fx == VGL_FX_SCANLINES && tex_scan)
    {
        /* dst = dst * scan colour; one 4-row repeat per source line */
        glBindTexture(GL_TEXTURE_2D, tex_scan);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ZERO, GL_SRC_COLOR);
        quad(x0, y0, x1, y1, 0.0f, 0.0f, 1.0f, (GLfloat)up_h);
        glDisable(GL_BLEND);
    }
    else if (gl_fx == VGL_FX_COMPOSITE)
    {
        /* Average each pixel with its right-hand neighbour: dither patterns
         * that a composite TV blurred into transparency blend the same way. */
        GLfloat du = 1.0f / (GLfloat)TEX_W;
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glColor4f(1.0f, 1.0f, 1.0f, 0.5f);
        quad(x0, y0, x1, y1, u0 + du, 0.0f, u1 + du, v);
        glDisable(GL_BLEND);
    }

    mglSwitchDisplay();

    if (ef)
    {
        ULONG up, dr;
        ReadEClock(&e2);
        up = ec_us(&e0, &e1);
        dr = ec_us(&e1, &e2);
        t_up_sum += (double)up;
        t_draw_sum += (double)dr;
        if (up > t_up_max) t_up_max = up;
        if (dr > t_draw_max) t_draw_max = dr;
        t_rows_sum += (double)rows;
        t_frames++;
    }
    ring_i = (ring_i + 1) % ring_n;
    if (t_frames <= 2)
        report_gl_error("first frames");
}

void video_gl_set_experiments(int ring, int half)
{
    ring_n = (ring >= 1 && ring <= RING_MAX) ? ring : 1;
    half_mode = half ? 1 : 0;
}

void video_gl_shutdown(void)
{
    if (t_frames)
    {
        printf("GL: upload avg %lu us (max %lu), draw+present avg %lu us (max %lu), %lu frames\n",
               (ULONG)(t_up_sum / (double)t_frames + 0.5), t_up_max,
               (ULONG)(t_draw_sum / (double)t_frames + 0.5), t_draw_max, t_frames);
        printf("GL: avg %lu rows x 320 per upload, %d texture(s)%s\n",
               (ULONG)(t_rows_sum / (double)t_frames + 0.5), ring_n,
               half_mode ? ", halves alternating" : "");
    }
    t_frames = 0;

    if (gl_ctx)
    {
        if (tex_scan)  { glDeleteTextures(1, &tex_scan);  tex_scan = 0; }
        if (tex_count) { glDeleteTextures(tex_count, tex_ring); tex_count = 0; }
        mglDeleteContext();
        gl_ctx = NULL;
    }
    if (gl_lib_open)
    {
        MiniGLClose();
        gl_lib_open = 0;
    }
    if (rgba_lut)   { FreeMem(rgba_lut, 65536 * sizeof(ULONG)); rgba_lut = NULL; }
    if (rgba_stage) { FreeMem(rgba_stage, SRC_MAX_W * SRC_MAX_H * sizeof(ULONG)); rgba_stage = NULL; }
}
