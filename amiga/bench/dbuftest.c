/*
 * dbuftest.c - how does Intuition double buffering behave on this display?
 *
 * Opens a 320x240 32-bit RTG screen with two ScreenBuffers (the emulator's
 * VSync setup) and times, with the EClock:
 *
 *   refresh   60 x WaitBOVP(): the refresh period a waiting task sees
 *   single    one ChangeScreenBuffer() at a time, started d ms after a
 *             vertical blank: how long the call takes, and when the
 *             dbi_SafeMessage (and, in the first pass, the dbi_DispMessage)
 *             comes back, counted from the call
 *   loop      the emulator's frame loop: w ms of "work", then a swap if the
 *             last swap's SafeMessage is back, else a frame without a swap
 *             ("front"); prints what every frame did
 *   loopwait  the same, but a frame waits for the SafeMessage instead of
 *             going without a swap
 *
 * The tests line up with a blank through WaitBOVP(). Where calls of it in a
 * row return at once (the refresh line then shows a few us instead of a
 * second: seen with a polling driver and with uaegfx), the refresh line says
 * nothing, and the first test of each table starts at an arbitrary point of
 * the refresh. The later ones start where the swap before them returned.
 *
 * No wait for a message in here can hang: each is a poll with a 300 ms limit.
 * Output goes to stdout; redirect it to a file (the screen covers the Shell).
 *
 * Usage: dbuftest [modeid-hex]     default: BestCModeIDTags 320x240x32
 *
 * Build: D:/amiga-gcc-amiga6/bin/m68k-amigaos-gcc -m68040 -mhard-float -mschulz
 *        -noixemul -O2 -fno-peephole2 -Wall -o dbuftest dbuftest.c -lm
 */
#include <exec/types.h>
#include <exec/ports.h>
#include <devices/timer.h>
#include <graphics/gfx.h>
#include <graphics/view.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/cybergraphics.h>
#include <proto/timer.h>
#include <stdio.h>
#include <stdlib.h>

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *CyberGfxBase;
struct Device *TimerBase;

static ULONG efreq;
static struct Screen *scr;
static struct ScreenBuffer *sb[2];
static struct MsgPort *safe_port, *disp_port;
static int cur;                 /* buffer shown (or queued to be shown) */
static int safe_out, disp_out;  /* messages not collected yet */

#define LIMIT_US 300000UL

static double now_us(void)
{
    struct EClockVal ev;
    ReadEClock(&ev);
    return ((double)ev.ev_hi * 4294967296.0 + (double)ev.ev_lo) * 1000000.0 / (double)efreq;
}

static void spin_until(double t)
{
    while (now_us() < t)
        ;
}

/* Polls a port until a message is there or LIMIT_US has passed since t0.
 * Returns the time since t0 in us, or -1 when nothing came. */
static long poll_port(struct MsgPort *port, double t0, int *out)
{
    for (;;)
    {
        double t = now_us();
        if (GetMsg(port))
        {
            (*out)--;
            return (long)(t - t0);
        }
        if (t - t0 > (double)LIMIT_US)
            return -1;
    }
}

static void drain(void)
{
    double t0 = now_us();
    while (safe_out > 0 && poll_port(safe_port, t0, &safe_out) >= 0)
        ;
    t0 = now_us();
    while (disp_out > 0 && poll_port(disp_port, t0, &disp_out) >= 0)
        ;
}

static void set_disp(int on)
{
    sb[0]->sb_DBufInfo->dbi_DispMessage.mn_ReplyPort = on ? disp_port : NULL;
    sb[1]->sb_DBufInfo->dbi_DispMessage.mn_ReplyPort = on ? disp_port : NULL;
}

/* One swap; returns the call's duration in us, or -1 when it was refused */
static long swap(int with_disp, double *t_call)
{
    double t0 = now_us(), t1;
    int ok = ChangeScreenBuffer(scr, sb[1 - cur]) ? 1 : 0;
    t1 = now_us();
    if (t_call)
        *t_call = t0;
    if (!ok)
        return -1;
    cur = 1 - cur;
    safe_out++;
    if (with_disp)
        disp_out++;
    return (long)(t1 - t0);
}

static void test_single(int with_disp)
{
    int d;

    set_disp(with_disp);
    printf("\nsingle swaps, dbi_DispMessage %s (all times in us from the call)\n",
           with_disp ? "used" : "not used (the emulator's setup)");
    printf("  ms after blank   call returns   SafeMessage   DispMessage\n");
    for (d = 0; d <= 17; d++)
    {
        double t0;
        long ret, safe, disp = -2;

        drain();
        WaitBOVP(&scr->ViewPort);
        spin_until(now_us() + (double)d * 1000.0);
        ret = swap(with_disp, &t0);
        if (ret < 0)
        {
            printf("  %2d               refused\n", d);
            continue;
        }
        safe = poll_port(safe_port, t0, &safe_out);
        if (with_disp)
            disp = poll_port(disp_port, t0, &disp_out);
        printf("  %2d               %8ld       %8ld      %8ld\n", d, ret, safe, disp);
    }
}

/* Results are kept and printed after the loop: a printf in it would put
 * file I/O into the timing */
#define MAX_FRAMES 32
static struct { long start, waited, ret, since; int action; } rec[MAX_FRAMES];

static void test_loop(long work_us, int wait_safe, int frames)
{
    int i, safe_ok = 1;
    double t_prev_ret = 0.0, t_start;

    if (frames > MAX_FRAMES)
        frames = MAX_FRAMES;
    set_disp(0);
    drain();
    WaitBOVP(&scr->ViewPort);
    t_start = now_us();
    for (i = 0; i < frames; i++)
    {
        double t_frame = now_us(), t_call, t_ret;
        long waited = 0, ret;

        spin_until(t_frame + (double)work_us);
        if (!safe_ok)
        {
            if (wait_safe)
            {
                double t0 = now_us();
                waited = poll_port(safe_port, t0, &safe_out);
                safe_ok = (waited >= 0);
            }
            else if (GetMsg(safe_port))
            {
                safe_out--;
                safe_ok = 1;
            }
        }
        rec[i].start = (long)(t_frame - t_start);
        rec[i].waited = waited;
        rec[i].ret = 0;
        rec[i].since = 0;
        if (!safe_ok)
        {
            rec[i].action = 0;
            continue;
        }
        ret = swap(0, &t_call);
        t_ret = now_us();
        if (ret < 0)
        {
            rec[i].action = 2;
            continue;
        }
        safe_ok = 0;
        rec[i].action = 1;
        rec[i].ret = ret;
        rec[i].since = t_prev_ret > 0.0 ? (long)(t_ret - t_prev_ret) : 0L;
        t_prev_ret = t_ret;
    }

    printf("\n%s, %ld us of work per frame (us)\n",
           wait_safe ? "loopwait" : "loop", work_us);
    printf("  frame    start  action  waited-for-safe   in-call  since-last-swap-return\n");
    for (i = 0; i < frames; i++)
    {
        if (rec[i].action == 1)
            printf("  %3d  %8ld  swap       %8ld     %8ld  %8ld\n", i, rec[i].start,
                   rec[i].waited, rec[i].ret, rec[i].since);
        else
            printf("  %3d  %8ld  %s\n", i, rec[i].start,
                   rec[i].action ? "refused" : "front");
    }
}

int main(int argc, char **argv)
{
    ULONG modeid = INVALID_ID;
    struct MsgPort *tport = NULL;
    struct timerequest *treq = NULL;
    int rc = 20, i;
    double t0, t1;

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

    if (argc > 1)
        modeid = strtoul(argv[1], NULL, 16);
    else
        modeid = BestCModeIDTags(CYBRBIDTG_Depth, 32,
                                 CYBRBIDTG_NominalWidth, 320,
                                 CYBRBIDTG_NominalHeight, 240,
                                 TAG_DONE);
    if (modeid == INVALID_ID)
    {
        printf("no 320x240 32-bit RTG mode\n");
        goto out;
    }

    safe_port = CreateMsgPort();
    disp_port = CreateMsgPort();
    if (!safe_port || !disp_port)
        goto out;

    scr = OpenScreenTags(NULL,
                         SA_DisplayID, modeid,
                         SA_Width, 320,
                         SA_Height, 240,
                         SA_Depth, 32,
                         SA_Type, CUSTOMSCREEN,
                         SA_Quiet, TRUE,
                         SA_ShowTitle, FALSE,
                         TAG_DONE);
    if (!scr)
    {
        printf("cannot open a screen in mode $%08lx\n", (unsigned long)modeid);
        goto out;
    }
    sb[0] = AllocScreenBuffer(scr, NULL, SB_SCREEN_BITMAP);
    sb[1] = AllocScreenBuffer(scr, NULL, 0);
    if (!sb[0] || !sb[1])
    {
        printf("AllocScreenBuffer failed\n");
        goto out;
    }
    sb[0]->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = safe_port;
    sb[1]->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = safe_port;

    printf("dbuftest: mode $%08lx, EClock %lu Hz, intuition %ld.%ld, graphics %ld.%ld\n",
           (unsigned long)modeid, (unsigned long)efreq,
           (long)IntuitionBase->LibNode.lib_Version, (long)IntuitionBase->LibNode.lib_Revision,
           (long)GfxBase->LibNode.lib_Version, (long)GfxBase->LibNode.lib_Revision);

    WaitBOVP(&scr->ViewPort);
    t0 = now_us();
    for (i = 0; i < 60; i++)
        WaitBOVP(&scr->ViewPort);
    t1 = now_us();
    printf("refresh: 60 x WaitBOVP took %ld us\n", (long)(t1 - t0));

    test_single(1);
    test_single(0);
    test_loop(2000, 0, 16);
    test_loop(8000, 0, 16);
    test_loop(14700, 0, 24);
    test_loop(18000, 0, 16);
    test_loop(2000, 1, 12);
    test_loop(8000, 1, 12);
    test_loop(14700, 1, 12);
    rc = 0;

out:
    if (scr)
    {
        drain();
        if (safe_out > 0 || disp_out > 0)
            printf("warning: %d Safe and %d Disp messages never came back\n", safe_out, disp_out);
        if (sb[1]) FreeScreenBuffer(scr, sb[1]);
        if (sb[0]) FreeScreenBuffer(scr, sb[0]);
        CloseScreen(scr);
    }
    /* Ports are only deleted when no message can still arrive at them */
    if (safe_port && safe_out <= 0) DeleteMsgPort(safe_port);
    if (disp_port && disp_out <= 0) DeleteMsgPort(disp_port);
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
