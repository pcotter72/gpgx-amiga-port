/*
 * glestest.c - what does one emulator frame cost through ogles2.library?
 *
 * ogles2.library is OpenGL ES 2.0 on the Pi's GPU for a PiStorm with Emu68
 * (github.com/ladislawv/ogles2pi3: Pi 4/CM4 and, from 1.4, Pi 3/Zero 2 W).
 * This opens the emulator's kind of display (a 32-bit RTG screen of its own
 * and a borderless window of the screen's size, which ogles2 draws into
 * directly) and does what a GPU output path would do each frame: upload a
 * 320x224 frame into a 512x256 texture, draw one quad, swap. It prints
 *
 *   - the cost per frame of the upload, the draw and the swap, for a
 *     GL_RGB565 upload and for an RGBA8 upload with the 565-to-RGBA
 *     conversion (a table lookup per pixel) counted;
 *   - the same loops run flat out and ended with glFinish(): the true time
 *     per frame, because aglSwapBuffers() returns before the GPU is done;
 *   - with OGLES2_CCT_VSYNC: the time between swap returns as a histogram;
 *   - what aglSetParams2(OGLES2_CCT_VSYNC) costs per call;
 *   - four pixels read back from the picture: orientation and colours.
 *
 * Usage: glestest [WIDTH HEIGHT [modeid-hex]]   default 640 480, mode from
 *        BestCModeIDTags. Redirect the output (the screen covers the Shell).
 *
 * The shaders must not change by a byte once their binaries are shipped:
 * ogles2 finds a compiled program by a hash of the two texts. Without
 * binaries it needs its compiler (LIBS:v3dglslc) and compiles at the first
 * run.
 *
 * Build (the ogles2 SDK's include/ and lib/ in <SDK>):
 *   D:/amiga-gcc-amiga6/bin/m68k-amigaos-gcc -m68040 -mhard-float -mschulz
 *     -noixemul -O2 -fno-peephole2 -Wall -I<SDK>/include -o glestest
 *     glestest.c -L<SDK>/lib -logles2 -lm
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <graphics/displayinfo.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <utility/tagitem.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/cybergraphics.h>
#include <proto/timer.h>
#include <proto/ogles2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *CyberGfxBase;
struct Device *TimerBase;

#define SRC_W 320
#define SRC_H 224
#define TEX_W 512
#define TEX_H 256
#define FRAMES 300

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

static ULONG efreq;
static UWORD *src565;       /* the "emulator frame", RGB565 big-endian */
static ULONG *srcrgba;      /* its RGBA8 conversion */
static ULONG *lut;          /* RGB565 -> R,G,B,A bytes */

static double now_us(void)
{
    struct EClockVal ev;
    ReadEClock(&ev);
    return ((double)ev.ev_hi * 4294967296.0 + (double)ev.ev_lo) * 1000000.0 / (double)efreq;
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
        char log[512];
        log[0] = 0;
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        printf("shader: %s\n", log);
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
        char log[512];
        log[0] = 0;
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        printf("link: %s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

/* The test picture: red top left, green top right, blue bottom left, white
 * bottom right, and a bar that moves with the frame number so that every
 * upload differs from the one before. */
static void make_frame(int frame)
{
    int x, y, bar = frame % SRC_W;

    for (y = 0; y < SRC_H; y++)
    {
        UWORD *row = src565 + y * SRC_W;
        UWORD left = (y < SRC_H / 2) ? 0xF800 : 0x001F;
        UWORD right = (y < SRC_H / 2) ? 0x07E0 : 0xFFFF;
        for (x = 0; x < SRC_W / 2; x++)
            row[x] = left;
        for (; x < SRC_W; x++)
            row[x] = right;
    }
    for (y = SRC_H / 2 - 4; y < SRC_H / 2 + 4; y++)
        src565[y * SRC_W + bar] = 0x0000;
}

static void convert_rgba(void)
{
    const UWORD *s = src565;
    ULONG *d = srcrgba;
    long n = (long)SRC_W * SRC_H;

    while (n--)
        *d++ = lut[*s++];
}

struct cost { double conv, upload, draw, swap, total, finished; };

/* One measured loop. rgba: 0 = GL_RGB565 upload, 1 = convert + RGBA8.
 * hist: when not NULL, 2 ms buckets of the time between swap returns
 * (26 entries, the last for 50 ms and more). */
static void run_loop(int rgba, int frames, struct cost *c, ULONG *hist)
{
    double t0, t1, t2, t3, t4, start, last = 0.0;
    int i;

    memset(c, 0, sizeof(*c));
    start = now_us();
    for (i = 0; i < frames; i++)
    {
        make_frame(i);
        t0 = now_us();
        if (rgba)
            convert_rgba();
        t1 = now_us();
        if (rgba)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SRC_W, SRC_H, GL_RGBA, GL_UNSIGNED_BYTE, srcrgba);
        else
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SRC_W, SRC_H, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, src565);
        t2 = now_us();
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        t3 = now_us();
        aglSwapBuffers();
        t4 = now_us();
        c->conv += t1 - t0;
        c->upload += t2 - t1;
        c->draw += t3 - t2;
        c->swap += t4 - t3;
        if (hist && last > 0.0)
        {
            ULONG b = (ULONG)((t4 - last) / 2000.0);
            hist[b > 25 ? 25 : b]++;
        }
        last = t4;
    }
    c->total = now_us() - start;
    glFinish();
    c->finished = now_us() - start;
}

static void print_cost(const char *name, const struct cost *c, int frames)
{
    double n = (double)frames;

    printf("%s, %d frames:\n", name, frames);
    printf("  per frame: convert %ld us, upload %ld us, draw %ld us, swap %ld us\n",
           (long)(c->conv / n), (long)(c->upload / n), (long)(c->draw / n), (long)(c->swap / n));
    printf("  loop %ld us per frame (includes ~%ld us for the test picture and the clock);"
           " with a final glFinish %ld us per frame\n",
           (long)(c->total / n), (long)((c->total - c->conv - c->upload - c->draw - c->swap) / n),
           (long)(c->finished / n));
}

static void set_vsync(BOOL on)
{
    struct TagItem t[2];

    t[0].ti_Tag = OGLES2_CCT_VSYNC;
    t[0].ti_Data = on;
    t[1].ti_Tag = TAG_DONE;
    t[1].ti_Data = 0;
    aglSetParams2(t);
}

static void make_texture(int rgba, GLenum filter)
{
    if (rgba)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEX_W, TEX_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, TEX_W, TEX_H, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* The quad's texture coordinates for a picture that fills umax x vmax of
 * its texture; the first texture row (v = 0) is the top of the picture. */
static void set_quad(GLfloat umax, GLfloat vmax)
{
    static const GLfloat xy[8] = { -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f, -1.0f };
    static const GLfloat uv[8] = { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f };
    GLfloat quad[16];
    int i;

    for (i = 0; i < 4; i++)
    {
        quad[i * 4 + 0] = xy[i * 2];
        quad[i * 4 + 1] = xy[i * 2 + 1];
        quad[i * 4 + 2] = uv[i * 2] * umax;
        quad[i * 4 + 3] = uv[i * 2 + 1] * vmax;
    }
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
}

/* "glestest V": other ways to get the frame into the texture, 20 frames
 * each, to see whether one of them is cheaper (on the Pi 3 the 320x224
 * sub-image upload took 57 ms on 2026-11-17).
 *   sub    glTexSubImage2D of 320x224 into a 512x256 texture (as above)
 *   finish the same, with glFinish() before the upload, so that the texture
 *          is not in use by a frame still being rendered
 *   npot   a 320x224 texture, glTexSubImage2D of all of it
 *   whole  a 320x224 texture, glTexImage2D with the data every frame
 *   pot    a 512x256 texture, glTexImage2D with 512x256 of data every frame */
static void run_variants(UWORD *big565)
{
    static const char *const name[5] = { "sub", "finish", "npot", "whole", "pot" };
    int v, rgba, i;

    for (rgba = 0; rgba < 2; rgba++)
    {
        GLenum fmt = rgba ? GL_RGBA : GL_RGB;
        GLenum type = rgba ? GL_UNSIGNED_BYTE : GL_UNSIGNED_SHORT_5_6_5;
        const void *data = rgba ? (const void *)srcrgba : (const void *)src565;

        for (v = 0; v < 5; v++)
        {
            double up = 0.0, fin = 0.0, rest = 0.0, start, t0, t1, t2, t3;
            int tw = (v == 2 || v == 3) ? SRC_W : TEX_W;
            int th = (v == 2 || v == 3) ? SRC_H : TEX_H;

            if (v == 4 && rgba)
                continue;                   /* the 512x256 source is RGB565 only */
            glTexImage2D(GL_TEXTURE_2D, 0, fmt, tw, th, 0, fmt, type, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            set_quad((GLfloat)SRC_W / (GLfloat)tw, (GLfloat)SRC_H / (GLfloat)th);
            start = now_us();
            for (i = 0; i < 22; i++)
            {
                make_frame(i);
                if (rgba)
                    convert_rgba();
                if (v == 4)
                {
                    int y;
                    for (y = 0; y < SRC_H; y++)
                        memcpy(big565 + y * TEX_W, src565 + y * SRC_W, SRC_W * 2);
                }
                t0 = now_us();
                if (v == 1)
                    glFinish();
                t1 = now_us();
                if (v == 3)
                    glTexImage2D(GL_TEXTURE_2D, 0, fmt, SRC_W, SRC_H, 0, fmt, type, data);
                else if (v == 4)
                    glTexImage2D(GL_TEXTURE_2D, 0, fmt, TEX_W, TEX_H, 0, fmt, type, big565);
                else
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SRC_W, SRC_H, fmt, type, data);
                t2 = now_us();
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                aglSwapBuffers();
                t3 = now_us();
                if (i >= 2)                 /* the first two frames set things up */
                {
                    fin += t1 - t0;
                    up += t2 - t1;
                    rest += t3 - t2;
                }
            }
            glFinish();
            printf("variant %-6s %s: glFinish %ld us, upload %ld us, draw + swap %ld us; %ld us per frame in all\n",
                   name[v], rgba ? "RGBA8 " : "RGB565", (long)(fin / 20.0), (long)(up / 20.0),
                   (long)(rest / 20.0), (long)((now_us() - start) / 22.0));
        }
    }
    {
        UBYTE px[4];
        px[0] = px[1] = px[2] = px[3] = 0;
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        printf("pixel bottom left after the last variant (expect blue): R %d G %d B %d A %d\n",
               px[0], px[1], px[2], px[3]);
        aglSwapBuffers();
    }
}

int main(int argc, char **argv)
{
    /* x, y, u, v: the whole surface; the first texture row (v = 0) is the
     * top of the picture */
    int variants = 0;
    UWORD *big565 = NULL;
    ULONG modeid = INVALID_ID, err = 0, sw = 0, sh = 0;
    int width = 640, height = 480, rc = 20, i;
    struct Screen *scr = NULL;
    struct Window *win = NULL;
    struct MsgPort *tport = NULL;
    struct timerequest *treq = NULL;
    void *ctx = NULL;
    GLuint prog = 0, vbo = 0, tex = 0;
    struct TagItem ct[5];
    struct cost c;
    static ULONG hist[26];
    double t0, t1;
    GLenum glerr;

    if (argc > 1 && (argv[1][0] == 'V' || argv[1][0] == 'v'))
    {
        variants = 1;
        argc--;
        argv++;
    }
    if (argc > 2)
    {
        width = atoi(argv[1]);
        height = atoi(argv[2]);
    }
    if (argc > 3)
        modeid = strtoul(argv[3], NULL, 16);

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    CyberGfxBase = OpenLibrary("cybergraphics.library", 40);
    if (!IntuitionBase || !GfxBase || !CyberGfxBase)
    {
        printf("needs intuition/graphics V39 and cybergraphics.library\n");
        goto out;
    }
    tport = CreateMsgPort();
    treq = tport ? (struct timerequest *)CreateIORequest(tport, sizeof(*treq)) : NULL;
    if (!treq || OpenDevice("timer.device", UNIT_MICROHZ, (struct IORequest *)treq, 0))
    {
        printf("no timer.device\n");
        if (treq) { DeleteIORequest((struct IORequest *)treq); treq = NULL; }
        goto out;
    }
    TimerBase = treq->tr_node.io_Device;
    {
        struct EClockVal ev;
        efreq = ReadEClock(&ev);
    }

    src565 = (UWORD *)AllocVec((ULONG)SRC_W * SRC_H * 2, MEMF_FAST | MEMF_CLEAR);
    srcrgba = (ULONG *)AllocVec((ULONG)SRC_W * SRC_H * 4, MEMF_FAST | MEMF_CLEAR);
    lut = (ULONG *)AllocVec(65536UL * 4, MEMF_FAST);
    if (!src565 || !srcrgba || !lut)
    {
        printf("no memory\n");
        goto out;
    }
    for (i = 0; i < 65536; i++)
    {
        ULONG r = (i >> 11) & 31, g = (i >> 5) & 63, b = i & 31;
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        lut[i] = (r << 24) | (g << 16) | (b << 8) | 0xFF;   /* bytes R,G,B,A */
    }

    if (modeid == INVALID_ID)
    {
        modeid = BestCModeIDTags(CYBRBIDTG_Depth, 32,
                                 CYBRBIDTG_NominalWidth, width,
                                 CYBRBIDTG_NominalHeight, height,
                                 TAG_DONE);
        /* As the emulator does (video_cgx.c, GetBestRTGMode): VideoCore's
         * 32-bit modes come in fours, +3 is BGRA. ogles2 draws straight
         * into a BGRA or RGBA screen only; into any other it copies. */
        if (modeid != INVALID_ID && (modeid & 3) != 3)
        {
            struct DisplayInfo di;
            ULONG bgra = modeid | 3;
            if (GetDisplayInfoData(NULL, (UBYTE *)&di, sizeof(di), DTAG_DISP, bgra) > 0)
                modeid = bgra;
        }
    }
    if (modeid == INVALID_ID)
    {
        printf("no %dx%d 32-bit RTG mode\n", width, height);
        goto out;
    }

    if (!OGLES2Open())
    {
        printf("cannot open %s (LIBS:, PROGDIR:libs/ or PROGDIR:)\n", OGLES2NAME);
        goto out;
    }

    scr = OpenScreenTags(NULL,
                         SA_DisplayID, modeid,
                         SA_Width, width,
                         SA_Height, height,
                         SA_Depth, 32,
                         SA_Type, CUSTOMSCREEN,
                         SA_Quiet, TRUE,
                         SA_ShowTitle, FALSE,
                         TAG_DONE);
    if (!scr)
    {
        printf("cannot open a %dx%d screen in mode $%08lx\n", width, height, (unsigned long)modeid);
        goto out;
    }
    win = OpenWindowTags(NULL,
                         WA_CustomScreen, (ULONG)scr,
                         WA_Left, 0,
                         WA_Top, 0,
                         WA_Width, scr->Width,
                         WA_Height, scr->Height,
                         WA_Borderless, TRUE,
                         WA_Backdrop, TRUE,
                         WA_Activate, TRUE,
                         WA_RMBTrap, TRUE,
                         WA_NoCareRefresh, TRUE,
                         TAG_DONE);
    if (!win)
    {
        printf("cannot open the window\n");
        goto out;
    }

    /* What ogles2 looks at to decide whether it may draw straight into the
     * screen (its display.c, window_is_screen): the window is all of the
     * screen, has no borders, draws into the screen's bitmap, and the
     * screen is not public. Its log (T:ogles2.log) then says "application
     * window, all of its screen (no copy)". */
    {
        struct BitMap *bm = scr->RastPort.BitMap;
        struct List *pl = LockPubScreenList();
        struct Node *pn;
        int pub = 0;

        if (pl)
        {
            for (pn = pl->lh_Head; pn->ln_Succ; pn = pn->ln_Succ)
                if (((struct PubScreenNode *)pn)->psn_Screen == scr)
                    pub = 1;
            UnlockPubScreenList();
        }
        printf("window %ld,%ld %ldx%ld borders %ld %ld %ld %ld flags $%08lx; rastport bitmap %s the screen's\n",
               (long)win->LeftEdge, (long)win->TopEdge, (long)win->Width, (long)win->Height,
               (long)win->BorderLeft, (long)win->BorderTop, (long)win->BorderRight, (long)win->BorderBottom,
               (unsigned long)win->Flags, win->RPort->BitMap == bm ? "is" : "IS NOT");
        printf("screen flags $%04lx (type $%lx), in the public screen list: %s, cybergfx bitmap %ld, pixfmt %ld\n",
               (unsigned long)scr->Flags, (unsigned long)(scr->Flags & SCREENTYPE), pub ? "YES" : "no",
               (long)GetCyberMapAttr(bm, CYBRMATTR_ISCYBERGFX), (long)GetCyberMapAttr(bm, CYBRMATTR_PIXFMT));
    }

    /* A TagItem array, not the varargs stub */
    ct[0].ti_Tag = OGLES2_CCT_WINDOW;     ct[0].ti_Data = (ULONG)win;
    ct[1].ti_Tag = OGLES2_CCT_DEPTH;      ct[1].ti_Data = 0;
    ct[2].ti_Tag = OGLES2_CCT_STENCIL;    ct[2].ti_Data = 0;
    ct[3].ti_Tag = OGLES2_CCT_VSYNC;      ct[3].ti_Data = FALSE;
    ct[4].ti_Tag = TAG_DONE;              ct[4].ti_Data = 0;
    ctx = aglCreateContext2(&err, ct);
    if (!ctx)
    {
        printf("aglCreateContext2 failed (error %lu)\n", (unsigned long)err);
        goto out;
    }
    aglMakeCurrent(ctx);
    ct[0].ti_Tag = OGLES2_CCT_GET_WIDTH;  ct[0].ti_Data = (ULONG)&sw;
    ct[1].ti_Tag = OGLES2_CCT_GET_HEIGHT; ct[1].ti_Data = (ULONG)&sh;
    ct[2].ti_Tag = TAG_DONE;              ct[2].ti_Data = 0;
    aglSetParams2(ct);

    printf("glestest: screen %ldx%ld mode $%08lx, surface %lux%lu, EClock %lu Hz\n",
           (long)scr->Width, (long)scr->Height, (unsigned long)modeid,
           (unsigned long)sw, (unsigned long)sh, (unsigned long)efreq);
    printf("library %ld.%ld; %s; %s\n",
           (long)OGLES2Base->lib_Version, (long)OGLES2Base->lib_Revision,
           (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));

    t0 = now_us();
    prog = link_program();
    t1 = now_us();
    if (!prog)
        goto out;
    printf("shader compile + link: %ld us\n", (long)(t1 - t0));

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    set_quad((GLfloat)SRC_W / (GLfloat)TEX_W, (GLfloat)SRC_H / (GLfloat)TEX_H);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)(2 * sizeof(GLfloat)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
    glViewport(0, 0, (GLsizei)sw, (GLsizei)sh);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    make_texture(0, GL_NEAREST);
    glerr = glGetError();
    if (glerr != GL_NO_ERROR)
        printf("GL error $%04lx after the setup\n", (unsigned long)glerr);

    /* 1. warm up (shader variants, texture storage), then the loops */
    run_loop(0, 30, &c, NULL);

    if (variants)
    {
        big565 = (UWORD *)AllocVec((ULONG)TEX_W * TEX_H * 2, MEMF_FAST | MEMF_CLEAR);
        if (!big565)
        {
            printf("no memory\n");
            goto out;
        }
        run_variants(big565);
        rc = 0;
        goto out;
    }

    run_loop(0, FRAMES, &c, NULL);
    print_cost("RGB565 upload, nearest, no vsync", &c, FRAMES);

    make_texture(0, GL_LINEAR);
    run_loop(0, 30, &c, NULL);
    run_loop(0, FRAMES, &c, NULL);
    print_cost("RGB565 upload, linear, no vsync", &c, FRAMES);

    /* 2. orientation and colours: the centres of the four quarters.
     * glReadPixels counts rows from the bottom. */
    {
        static const char *const want[4] = { "red", "green", "blue", "white" };
        UBYTE px[4];
        int q;

        make_texture(0, GL_NEAREST);
        make_frame(0);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SRC_W, SRC_H, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, src565);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        for (q = 0; q < 4; q++)
        {
            int x = (q & 1) ? (int)sw * 3 / 4 : (int)sw / 4;
            int y = (q & 2) ? (int)sh / 4 : (int)sh * 3 / 4;
            px[0] = px[1] = px[2] = px[3] = 0;
            glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            printf("pixel %s %s (expect %s): R %d G %d B %d A %d\n",
                   (q & 2) ? "bottom" : "top", (q & 1) ? "right" : "left", want[q],
                   px[0], px[1], px[2], px[3]);
        }
        aglSwapBuffers();
    }

    make_texture(1, GL_NEAREST);
    run_loop(1, 30, &c, NULL);
    run_loop(1, FRAMES, &c, NULL);
    print_cost("RGBA8 upload with the 565 conversion, nearest, no vsync", &c, FRAMES);

    /* 3. the cost of switching vsync */
    t0 = now_us();
    for (i = 0; i < 100; i++)
        set_vsync((BOOL)(i & 1));
    t1 = now_us();
    printf("aglSetParams2(OGLES2_CCT_VSYNC): %ld us per call\n", (long)((t1 - t0) / 100.0));

    /* 4. vsync on */
    make_texture(0, GL_NEAREST);
    set_vsync(TRUE);
    run_loop(0, 10, &c, NULL);
    memset(hist, 0, sizeof(hist));
    run_loop(0, FRAMES, &c, hist);
    print_cost("RGB565 upload, nearest, VSYNC", &c, FRAMES);
    printf("  time between swap returns, 2 ms buckets from 0 (last = 50 ms and more):\n   ");
    for (i = 0; i < 26; i++)
        printf(" %lu", (unsigned long)hist[i]);
    printf("\n");
    set_vsync(FALSE);

    glerr = glGetError();
    if (glerr != GL_NO_ERROR)
        printf("GL error $%04lx at the end\n", (unsigned long)glerr);
    rc = 0;

out:
    if (ctx)
    {
        if (tex) glDeleteTextures(1, &tex);
        if (vbo) glDeleteBuffers(1, &vbo);
        if (prog) glDeleteProgram(prog);
        aglDestroyContext(ctx);
    }
    if (win) CloseWindow(win);
    if (scr) CloseScreen(scr);
    OGLES2Close();
    if (big565) FreeVec(big565);
    if (lut) FreeVec(lut);
    if (srcrgba) FreeVec(srcrgba);
    if (src565) FreeVec(src565);
    if (treq)
    {
        CloseDevice((struct IORequest *)treq);
        DeleteIORequest((struct IORequest *)treq);
    }
    if (tport) DeleteMsgPort(tport);
    if (CyberGfxBase) CloseLibrary(CyberGfxBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}
