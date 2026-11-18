/****************************************************************************
 *  video_gles.c - optional display path through ogles2.library
 *
 *  ogles2.library is OpenGL ES 2.0 on the Pi's GPU
 *  for a PiStorm with Emu68 (github.com/ladislawv/ogles2pi3, 1.4: Pi 4/CM4
 *  and Pi 3). The emulator keeps rendering into the normal 320x240 RGB565
 *  buffer; each drawn frame is converted to RGBA8, uploaded into a 512x256
 *  texture, and one quad is drawn into the screen.
 *
 *  Measured with amiga/bench/glestest.c (glestest.log) on the A1200 + CM4:
 *  the RGBA8 sub-image upload costs 0.7 ms when the GPU has finished the
 *  frame before (glFinish first) and 1.8 ms when it has not (the library
 *  then makes a new copy of the texture); the RGB565 upload costs 3.3 ms;
 *  draw and swap together under 0.5 ms. On a Pi 3 (A500) the same upload
 *  costs 39-57 ms, so this path is for a Pi 4/CM4 only.
 *
 *  ogles2 draws straight into the screen only when the window is all of a
 *  private screen whose pixel format is BGRA32 or RGBA32 (its display.c);
 *  otherwise it draws off screen and copies (2.7 ms more on the CM4).
 *  video_cgx.c opens such a screen and window.
 *
 *  VSync is done here, not with OGLES2_CCT_VSYNC: the frame is built first,
 *  then video_cgx_vsync_wait() sleeps until the blank (only when the frame
 *  is on time, see main.c), then aglSwapBuffers() hands it to the GPU,
 *  which writes the screen from the top faster than the display scans it.
 *
 *  The two shader texts must not change by a byte without a new binary:
 *  ogles2 finds a compiled program by a hash of them, and compiles with
 *  LIBS:v3dglslc when it has none (about 2 s once).
 *
 *  Scanlines and composite blend therefore use no shader of their own: a
 *  second quad is blended over the picture with the same program (as the
 *  minigl path did, which this one replaced after 1.11). Scanlines: a 4x4
 *  texture, two white rows and two grey ones, repeated once per source
 *  line, multiplied into the picture. Composite: the frame again, one texel
 *  to the left, at half strength (glBlendColor).
 ****************************************************************************/

#include <stdio.h>
#include <string.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <utility/tagitem.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <cybergraphx/cybergraphics.h>

#include <proto/exec.h>
#include <proto/timer.h>
#include <proto/cybergraphics.h>
#include <proto/ogles2.h>

#include "video_gles.h"
#include "video_cgx.h"

extern struct Device *TimerBase;
extern struct Library *CyberGfxBase;

#ifdef USE_ASM_VIDEO
extern void Blit565toRGBA_asm(UWORD *src, ULONG *dst, ULONG *lut, int pixels);
#endif

#define TEX_W   512             /* frame texture */
#define TEX_H   256
#define SRC_MAX_W 320           /* emulator buffer: 320 x 240 RGB565 */
#define SRC_MAX_H 240
#define SCAN_W  4               /* scanline texture: 2 bright rows, 2 dark */
#define SCAN_H  4
#define SCAN_DARK 0x8C          /* the dark rows' grey */

/* The same texts as amiga/bench/glestest.c: one compiled program serves both */
static const char quad_vs[] =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "    v_uv = a_uv;\n"
    "}\n";

static const char quad_fs[] =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_tex, v_uv);\n"
    "}\n";

static int     lib_open = 0;
static void   *ctx = NULL;
static GLuint  prog = 0, vbo = 0, tex = 0, tex_scan = 0;
static int     fx = VGLES_FX_SMOOTH;
static int     surf_w = 0, surf_h = 0;  /* the context's surface = the screen */
static ULONG  *rgba_lut = NULL;         /* 65536 RGB565 -> bytes R,G,B,A */
static ULONG  *rgba_stage = NULL;       /* SRC_MAX_W * SRC_MAX_H */
static int     q_x = -1, q_w = -1, q_h = -1;    /* what the quad was built for */
static int     rect_x0, rect_y0, rect_x1, rect_y1; /* picture, screen pixels, y down */
static int     errors_reported = 0;

/* Per-frame cost, printed at shutdown */
static ULONG   ef = 0;                  /* EClock Hz, 0 = no timing */
static double  t_fin_sum = 0.0, t_up_sum = 0.0, t_draw_sum = 0.0;
static ULONG   t_up_max = 0, t_draw_max = 0, t_frames = 0;

static ULONG ec_us(const struct EClockVal *a, const struct EClockVal *b)
{
    return (ULONG)((double)(ULONG)(b->ev_lo - a->ev_lo) * 1000000.0 / (double)ef);
}

static void report_error(const char *where)
{
    GLenum e = glGetError();
    if (e != GL_NO_ERROR && errors_reported < 8)
    {
        printf("GLES: error 0x%04lx after %s\n", (ULONG)e, where);
        errors_reported++;
    }
}

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[256];
        log[0] = 0;
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        printf("GLES: shader: %s\n", log);
    }
    return s;
}

static GLuint link_program(void)
{
    GLuint vs = compile(GL_VERTEX_SHADER, quad_vs), fs = compile(GL_FRAGMENT_SHADER, quad_fs);
    GLuint p = glCreateProgram();
    GLint ok = 0;

    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 1, "a_uv");
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[256];
        log[0] = 0;
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        printf("GLES: link: %s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

/* The quad for a picture of vp_w x up_h source pixels starting at column
 * vp_x: a 4:3 rectangle in the middle of the screen. Sharp and scanlines use
 * the largest whole multiple of the source height (even scanlines), smooth
 * and composite the screen's height.
 * Vertices 4-7 are the same rectangle for the second pass of scanlines and
 * composite, with that pass's texture coordinates. */
static void build_quad(int vp_x, int vp_w, int up_h)
{
    GLfloat v[32], x0, x1, y0, y1, u0, u1, vmax, s0, s1, t0, t1;
    int n, dw, dh;

    if (fx == VGLES_FX_SHARP || fx == VGLES_FX_SCANLINES)
    {
        n = surf_h / up_h;
        dh = (n > 0) ? n * up_h : surf_h;
    }
    else
        dh = surf_h;
    dw = dh * 4 / 3;
    if (dw > surf_w)
    {
        dw = surf_w;
        dh = surf_w * 3 / 4;
    }
    rect_x0 = (surf_w - dw) / 2;  rect_x1 = rect_x0 + dw;
    rect_y0 = (surf_h - dh) / 2;  rect_y1 = rect_y0 + dh;

    /* clip space: x right, y up, -1..1 */
    x0 = (GLfloat)rect_x0 * 2.0f / (GLfloat)surf_w - 1.0f;
    x1 = (GLfloat)rect_x1 * 2.0f / (GLfloat)surf_w - 1.0f;
    y0 = 1.0f - (GLfloat)rect_y0 * 2.0f / (GLfloat)surf_h;
    y1 = 1.0f - (GLfloat)rect_y1 * 2.0f / (GLfloat)surf_h;
    u0 = (GLfloat)vp_x / (GLfloat)TEX_W;
    u1 = (GLfloat)(vp_x + vp_w) / (GLfloat)TEX_W;
    vmax = (GLfloat)up_h / (GLfloat)TEX_H;

    /* a strip: top left, bottom left, top right, bottom right; the first
     * texture row (v = 0) is the top of the picture */
    v[0]  = x0; v[1]  = y0; v[2]  = u0; v[3]  = 0.0f;
    v[4]  = x0; v[5]  = y1; v[6]  = u0; v[7]  = vmax;
    v[8]  = x1; v[9]  = y0; v[10] = u1; v[11] = 0.0f;
    v[12] = x1; v[13] = y1; v[14] = u1; v[15] = vmax;

    if (fx == VGLES_FX_SCANLINES)
    {
        /* The scanline texture once per source line (GL_REPEAT), moved
         * down by a quarter of one of its four rows. Without that, at 3 and
         * 5 screen rows per line the middle row's centre lies exactly
         * between the white and the grey rows and rounding decides which
         * it gets. With it: 2 rows = 1 bright + 1 dark, 3 = 2 + 1,
         * 4 = 2 + 2, 5 = 3 + 2. */
        s0 = 0.0f;  s1 = 1.0f;
        t0 = -0.25f / (GLfloat)SCAN_H;
#ifdef GLES_TEST_NOQUARTER  /* test builds: without it, to see what the pixel test then reads */
        t0 = 0.0f;
#endif
        t1 = (GLfloat)up_h + t0;
    }
    else
    {
        /* composite: the frame, one texel further right in the texture */
        s0 = u0 + 1.0f / (GLfloat)TEX_W;
        s1 = u1 + 1.0f / (GLfloat)TEX_W;
        t0 = 0.0f;
        t1 = vmax;
    }
    v[16] = x0; v[17] = y0; v[18] = s0; v[19] = t0;
    v[20] = x0; v[21] = y1; v[22] = s0; v[23] = t1;
    v[24] = x1; v[25] = y0; v[26] = s1; v[27] = t0;
    v[28] = x1; v[29] = y1; v[30] = s1; v[31] = t1;
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
    q_x = vp_x;
    q_w = vp_w;
    q_h = up_h;
}

int video_gles_init(struct Window *win, int effect)
{
    struct TagItem t[5];
    struct EClockVal ev;
    struct Screen *scr;
    const GLubyte *s;
    ULONG err = 0, w = 0, h = 0, pixfmt, v, r, g, b;
    int nearest;

    if (!win || !CyberGfxBase)
        return 0;
    scr = win->WScreen;

    fx = (effect >= VGLES_FX_SHARP && effect <= VGLES_FX_COMPOSITE) ? effect : VGLES_FX_SMOOTH;
    if (fx != effect)
        printf("GLES: there is no effect %d, using smooth\n", effect);
    nearest = (fx == VGLES_FX_SHARP || fx == VGLES_FX_SCANLINES);
#ifdef GLES_TEST_PIXELS
    /* test builds: composite without the filter, so that the pixels read
     * back can be worked out from the source frame */
    if (fx == VGLES_FX_COMPOSITE)
        nearest = 1;
#endif
    errors_reported = 0;
    t_fin_sum = t_up_sum = t_draw_sum = 0.0;
    t_up_max = t_draw_max = t_frames = 0;
    q_x = q_w = q_h = -1;
    ef = TimerBase ? ReadEClock(&ev) : 0;

    /* What ogles2 needs to draw straight into the screen; anything else
     * still works, through its copy */
    pixfmt = GetCyberMapAttr(scr->RastPort.BitMap, CYBRMATTR_PIXFMT);
    if (win->LeftEdge || win->TopEdge || win->Width != scr->Width || win->Height != scr->Height ||
        win->BorderLeft || win->BorderTop || win->BorderRight || win->BorderBottom ||
        (pixfmt != PIXFMT_BGRA32 && pixfmt != PIXFMT_RGBA32))
        printf("GLES: the window is not all of a BGRA/RGBA screen (pixel format %lu):"
               " ogles2 will copy every frame\n", pixfmt);

    if (!OGLES2Open())
    {
        printf("GLES: ogles2.library not available - using the normal display\n");
        return 0;
    }
    lib_open = 1;

    /* A TagItem array, not the varargs stub */
    t[0].ti_Tag = OGLES2_CCT_WINDOW;   t[0].ti_Data = (ULONG)win;
    t[1].ti_Tag = OGLES2_CCT_DEPTH;    t[1].ti_Data = 0;
    t[2].ti_Tag = OGLES2_CCT_STENCIL;  t[2].ti_Data = 0;
    t[3].ti_Tag = OGLES2_CCT_VSYNC;    t[3].ti_Data = FALSE;
    t[4].ti_Tag = TAG_DONE;            t[4].ti_Data = 0;
    ctx = aglCreateContext2(&err, t);
    if (!ctx)
    {
        printf("GLES: cannot create a context (error %lu) - using the normal display\n", err);
        video_gles_shutdown();
        return 0;
    }
    aglMakeCurrent(ctx);
    t[0].ti_Tag = OGLES2_CCT_GET_WIDTH;   t[0].ti_Data = (ULONG)&w;
    t[1].ti_Tag = OGLES2_CCT_GET_HEIGHT;  t[1].ti_Data = (ULONG)&h;
    t[2].ti_Tag = TAG_DONE;               t[2].ti_Data = 0;
    aglSetParams2(t);
    surf_w = (int)w;
    surf_h = (int)h;
#ifdef GLES_TEST_FAIL
    /* test builds: act as if the surface were unusable, to show the way
     * back to the normal display */
    surf_w = 0;
#endif
    if (surf_w <= 0 || surf_h <= 0)
    {
        printf("GLES: no surface (%dx%d) - using the normal display\n", surf_w, surf_h);
        video_gles_shutdown();
        return 0;
    }

    s = glGetString(GL_RENDERER);
    if (s) printf("GLES: renderer %s\n", (const char *)s);
    s = glGetString(GL_VERSION);
    if (s) printf("GLES: version %s\n", (const char *)s);

    rgba_lut = (ULONG *)AllocMem(65536 * sizeof(ULONG), MEMF_ANY);
    rgba_stage = (ULONG *)AllocMem(SRC_MAX_W * SRC_MAX_H * sizeof(ULONG), MEMF_ANY);
    if (!rgba_lut || !rgba_stage)
    {
        printf("GLES: out of memory - using the normal display\n");
        video_gles_shutdown();
        return 0;
    }
    for (v = 0; v < 65536; v++)
    {
        r = (v >> 11) & 0x1F;  g = (v >> 5) & 0x3F;  b = v & 0x1F;
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        /* Big-endian longword stored as bytes R, G, B, A = GL_RGBA order */
        rgba_lut[v] = (r << 24) | (g << 16) | (b << 8) | 0xFF;
    }

    prog = link_program();
    if (!prog)
    {
        printf("GLES: no shader program (is LIBS:v3dglslc installed?) - using the normal display\n");
        video_gles_shutdown();
        return 0;
    }

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)(2 * sizeof(GLfloat)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
    glViewport(0, 0, surf_w, surf_h);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    /* all of it black first: the filter samples the texels just outside
     * the picture */
    {
        UBYTE *zero = (UBYTE *)AllocMem(TEX_W * TEX_H * 4, MEMF_ANY | MEMF_CLEAR);
        if (!zero)
        {
            printf("GLES: out of memory - using the normal display\n");
            video_gles_shutdown();
            return 0;
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEX_W, TEX_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero);
        FreeMem(zero, TEX_W * TEX_H * 4);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    report_error("the setup");

    if (fx == VGLES_FX_SCANLINES)
    {
        /* Rows 0-1 white, rows 2-3 grey; a big-endian longword is the bytes
         * R, G, B, A */
        ULONG scan[SCAN_W * SCAN_H];
        int i;

        for (i = 0; i < SCAN_W * SCAN_H; i++)
            scan[i] = (i < SCAN_W * 2) ? 0xFFFFFFFFUL
                    : (((ULONG)SCAN_DARK << 24) | ((ULONG)SCAN_DARK << 16) | ((ULONG)SCAN_DARK << 8) | 0xFF);
        glGenTextures(1, &tex_scan);
        glBindTexture(GL_TEXTURE_2D, tex_scan);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SCAN_W, SCAN_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, scan);
        glBindTexture(GL_TEXTURE_2D, tex);
        report_error("the scanline texture");
    }
    else if (fx == VGLES_FX_COMPOSITE)
    {
        glBlendColor(0.0f, 0.0f, 0.0f, 0.5f);
        report_error("glBlendColor");
    }

    printf("GLES: %dx%d, effect %d (%s), RGBA8 upload\n", surf_w, surf_h, fx,
           fx == VGLES_FX_SHARP ? "sharp" : fx == VGLES_FX_SMOOTH ? "smooth" :
           fx == VGLES_FX_SCANLINES ? "scanlines" : "composite");
    return 1;
}

int video_gles_update(const UWORD *src, int src_pitch_px,
                      int vp_x, int vp_y, int vp_w, int vp_h, double sync_limit)
{
    struct EClockVal e0, e1, e2, e3;
    int up_w, up_h, res;

    if (!ctx || !src)
        return VSYNC_OFF;

    /* Clamp the visible area to the 320x240 buffer. Rows vp_y.. are
     * uploaded whole (full pitch), columns are selected by texcoords. */
    up_w = src_pitch_px;  if (up_w > SRC_MAX_W) up_w = SRC_MAX_W;
    if (vp_x < 0) vp_x = 0;
    if (vp_y < 0) vp_y = 0;
    if (vp_x + vp_w > up_w) vp_w = up_w - vp_x;
    up_h = vp_h;
    if (vp_y + up_h > SRC_MAX_H) up_h = SRC_MAX_H - vp_y;
    if (up_w <= 0 || up_h <= 0 || vp_w <= 0)
        return VSYNC_OFF;
    src += (ULONG)vp_y * (ULONG)src_pitch_px;

    if (ef) ReadEClock(&e0);

    /* The GPU must be done with the frame before, or the upload makes the
     * library copy the texture (1.8 instead of 0.7 ms, see the top). By now
     * it normally is: this returns at once. */
    glFinish();
    if (ef) ReadEClock(&e1);

    if (vp_x != q_x || vp_w != q_w || up_h != q_h)
        build_quad(vp_x, vp_w, up_h);

    /* Whole rows: they are contiguous in the emulator buffer when its
     * pitch is the 320 pixels uploaded */
    if (src_pitch_px == up_w)
    {
#ifdef USE_ASM_VIDEO
        Blit565toRGBA_asm((UWORD *)src, rgba_stage, rgba_lut, up_w * up_h);
#else
        int n;
        for (n = 0; n < up_w * up_h; n++)
            rgba_stage[n] = rgba_lut[src[n]];
#endif
    }
    else
    {
        int y, n;
        for (y = 0; y < up_h; y++)
        {
            const UWORD *s = src + (ULONG)y * (ULONG)src_pitch_px;
            ULONG *d = rgba_stage + (ULONG)y * (ULONG)up_w;
            for (n = 0; n < up_w; n++)
                d[n] = rgba_lut[s[n]];
        }
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, up_w, up_h, GL_RGBA, GL_UNSIGNED_BYTE, rgba_stage);
    if (ef) ReadEClock(&e2);

    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

#ifndef GLES_TEST_NOPASS2   /* test builds: leave the effect out; the pixel test must then fail */
    if (fx == VGLES_FX_SCANLINES && tex_scan)
    {
        /* picture = picture * scanline texture */
        glBindTexture(GL_TEXTURE_2D, tex_scan);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ZERO, GL_SRC_COLOR);
        glDrawArrays(GL_TRIANGLE_STRIP, 4, 4);
        glDisable(GL_BLEND);
        glBindTexture(GL_TEXTURE_2D, tex);
    }
    else if (fx == VGLES_FX_COMPOSITE)
    {
        /* picture = (picture + its right-hand neighbour) / 2: dither
         * patterns that a composite TV blurred into transparency blend the
         * same way */
        glEnable(GL_BLEND);
        glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
        glDrawArrays(GL_TRIANGLE_STRIP, 4, 4);
        glDisable(GL_BLEND);
    }
#endif

#ifdef GLES_TEST_PIXELS
    /* test builds: at frame 300 compare pixels of the picture and one of
     * the border with what the source frame and the effect must give
     * (within 1 of 255 per colour). For sharp, scanlines and composite
     * (which a test build draws without the filter); not for smooth. Every
     * point is read on each screen row of its source line (at most 6):
     * with scanlines the upper ones are bright and the lower ones dark.
     * glReadPixels counts rows from the bottom. */
    if (t_frames == 300)
    {
        static const int fxp[5] = { 50, 2, 97, 2, 97 }, fyp[5] = { 50, 2, 2, 97, 97 };
        UBYTE px[4];
        int k, j, c, dw = rect_x1 - rect_x0, dh = rect_y1 - rect_y0, bad = 0, n_bright = 0, n_dark = 0;

        for (k = 0; k < 5; k++)
        {
            int sx = rect_x0 + dw * fxp[k] / 100, sy = rect_y0 + dh * fyp[k] / 100;
            /* the texel under the centre of the screen pixel */
            int tx = vp_x + ((sx - rect_x0) * 2 + 1) * vp_w / (dw * 2);
            int ty = ((sy - rect_y0) * 2 + 1) * up_h / (dh * 2);
            ULONG s0 = rgba_lut[src[(ULONG)ty * (ULONG)src_pitch_px + tx]];
            ULONG s1 = rgba_lut[src[(ULONG)ty * (ULONG)src_pitch_px + tx + 1]];
            int row0, row1, rows;

            /* first and last screen row whose centre lies in source line ty */
            row0 = rect_y0 + (ty * dh * 2 + up_h) / (up_h * 2);
            row1 = rect_y0 + ((ty + 1) * dh * 2 - up_h - 1) / (up_h * 2);
            rows = row1 - row0 + 1;
            if (rows > 6) rows = 6;
            for (j = 0; j < rows; j++)
            {
                int want[3], dark = 0, miss = 0, row = row0 + j;

                if (fx == VGLES_FX_SCANLINES)
                {
                    /* Where the centre of screen row k of the line samples
                     * the scanline texture, as a fraction of its height
                     * times 16 * per: (2k + 1) * 8, less the quarter row
                     * (1/16 of the height) of build_quad() = per. The
                     * lower half of the texture is grey. */
                    int line0 = rect_y0 + ty * dh / up_h, per = dh / up_h;
                    int pos = ((row - line0) * 2 + 1) * 8 - per;
                    if (pos < 0) pos += 16 * per;
                    dark = pos >= 8 * per;
                }
                for (c = 0; c < 3; c++)
                {
                    int a = (int)((s0 >> (24 - 8 * c)) & 255), b2 = (int)((s1 >> (24 - 8 * c)) & 255);
                    want[c] = (fx == VGLES_FX_COMPOSITE) ? (a + b2) / 2 : dark ? a * SCAN_DARK / 255 : a;
                }
                px[0] = px[1] = px[2] = px[3] = 0;
                glReadPixels(sx, surf_h - 1 - row, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                for (c = 0; c < 3; c++)
                    if ((int)px[c] - want[c] > 1 || want[c] - (int)px[c] > 1)
                        miss = 1;
                bad += miss;
                if (dark) n_dark++; else n_bright++;
                printf("GLES test: screen %d,%d = source %d,%d%s: read %d %d %d, expect %d %d %d%s\n",
                       sx, row, tx, ty, fx == VGLES_FX_SCANLINES ? (dark ? " dark" : " bright") : "",
                       px[0], px[1], px[2], want[0], want[1], want[2], miss ? "  DIFFERS" : "");
            }
        }
        if (rect_x0 > 0 || rect_y0 > 0)
        {
            px[0] = px[1] = px[2] = px[3] = 9;
            glReadPixels(0, surf_h - 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            if (px[0] || px[1] || px[2])
                bad++;
            printf("GLES test: border pixel 0,0: read %d %d %d (expect 0 0 0)\n", px[0], px[1], px[2]);
        }
        else
            printf("GLES test: the picture fills the screen, no border to read\n");
        printf("GLES test: effect %d, picture %d,%d-%d,%d on %dx%d, %d bright and %d dark rows read,"
               " %d of the pixels differ\n",
               fx, rect_x0, rect_y0, rect_x1, rect_y1, surf_w, surf_h, n_bright, n_dark, bad);
    }
#endif

    /* The frame is built: now the blank, if there is time, and out */
    res = video_cgx_vsync_wait(sync_limit);
    aglSwapBuffers();

    if (ef)
    {
        ULONG up, dr;
        ReadEClock(&e3);
        up = ec_us(&e1, &e2);
        dr = ec_us(&e2, &e3);
        if (res == VSYNC_SYNCED)
        {
            ULONG w = video_cgx_vsync_wait_us();
            dr = (dr > w) ? dr - w : 0;
        }
        t_fin_sum += (double)ec_us(&e0, &e1);
        t_up_sum += (double)up;
        t_draw_sum += (double)dr;
        if (up > t_up_max) t_up_max = up;
        if (dr > t_draw_max) t_draw_max = dr;
    }
    t_frames++;
    if (t_frames <= 2)
        report_error("the first frames");
    return res;
}

void video_gles_shutdown(void)
{
    if (t_frames && ef)
        printf("GLES: per frame glFinish %lu us, convert + upload %lu us (max %lu),"
               " draw + swap %lu us (max %lu), %lu frames\n",
               (ULONG)(t_fin_sum / (double)t_frames + 0.5),
               (ULONG)(t_up_sum / (double)t_frames + 0.5), t_up_max,
               (ULONG)(t_draw_sum / (double)t_frames + 0.5), t_draw_max, t_frames);
    t_frames = 0;

    if (ctx)
    {
        if (tex)  { glDeleteTextures(1, &tex);  tex = 0; }
        if (tex_scan) { glDeleteTextures(1, &tex_scan); tex_scan = 0; }
        if (vbo)  { glDeleteBuffers(1, &vbo);   vbo = 0; }
        if (prog) { glDeleteProgram(prog);      prog = 0; }
        aglDestroyContext(ctx);
        ctx = NULL;
    }
    if (lib_open)
    {
        OGLES2Close();
        lib_open = 0;
    }
    if (rgba_lut)   { FreeMem(rgba_lut, 65536 * sizeof(ULONG)); rgba_lut = NULL; }
    if (rgba_stage) { FreeMem(rgba_stage, SRC_MAX_W * SRC_MAX_H * sizeof(ULONG)); rgba_stage = NULL; }
}
