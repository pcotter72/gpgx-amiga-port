/*
 * bovpprobe.c - is the emulator's start-up measurement of WaitBOVP() steady?
 *
 * video_cgx.c (vsync_setup) opens its screen and at once times 40 WaitBOVP()
 * calls in a row. It takes WaitBOVP as the VSync mechanism when the median
 * interval lies between 5 and 50 ms and at least 30 of the 40 intervals are
 * within 10% of it. This tool repeats exactly that: ROUNDS times it opens a
 * 320x240 32-bit screen, runs BLOCKS such measurements one after another,
 * prints each one's numbers and verdict, and closes the screen. A block
 * which fails is printed interval by interval.
 *
 * Nothing in here can hang unless WaitBOVP() itself does.
 * Output goes to stdout; redirect it to a file (the screen covers the Shell).
 *
 * Usage: bovpprobe [modeid-hex]     default: BestCModeIDTags 320x240x32
 *
 * Build: D:/amiga-gcc-amiga6/bin/m68k-amigaos-gcc -m68040 -mhard-float -mschulz
 *        -noixemul -O2 -fno-peephole2 -Wall -o bovpprobe bovpprobe.c -lm
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
#include <proto/timer.h>
#include <proto/cybergraphics.h>
#include <stdio.h>
#include <stdlib.h>

#define CAL_N  40
#define ROUNDS 6
#define BLOCKS 6

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *CyberGfxBase;
struct Device *TimerBase;

static ULONG efreq;

static double ticks(void)
{
    struct EClockVal ev;
    ReadEClock(&ev);
    return (double)ev.ev_hi * 4294967296.0 + (double)ev.ev_lo;
}

static long us(double t)
{
    return (long)(t * 1000000.0 / (double)efreq + 0.5);
}

int main(int argc, char **argv)
{
    ULONG modeid = INVALID_ID;
    struct MsgPort *tport = NULL;
    struct timerequest *treq = NULL;
    struct Screen *scr;
    static double t[CAL_N + 1], iv[CAL_N], raw[CAL_N];
    int rc = 20, r, b, i, j, failed = 0, total = 0;

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
    printf("bovpprobe: mode $%08lx, EClock %lu Hz, %d rounds of %d blocks of %d WaitBOVP\n",
           (unsigned long)modeid, (unsigned long)efreq, ROUNDS, BLOCKS, CAL_N);

    for (r = 0; r < ROUNDS; r++)
    {
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
        for (b = 0; b < BLOCKS; b++)
        {
            double med, lo, hi;
            int good = 0, ok;

            WaitBOVP(&scr->ViewPort);
            t[0] = ticks();
            for (i = 1; i <= CAL_N; i++)
            {
                WaitBOVP(&scr->ViewPort);
                t[i] = ticks();
            }
            for (i = 0; i < CAL_N; i++)
            {
                double d = t[i + 1] - t[i];
                raw[i] = d;
                for (j = i; j > 0 && iv[j - 1] > d; j--)
                    iv[j] = iv[j - 1];
                iv[j] = d;
            }
            med = iv[CAL_N / 2];
            lo = iv[0];
            hi = iv[CAL_N - 1];
            for (i = 0; i < CAL_N; i++)
                if (iv[i] > med * 0.9 && iv[i] < med * 1.1)
                    good++;
            ok = !(med < (double)efreq / 200.0 || med > (double)efreq / 20.0) && good * 4 >= CAL_N * 3;
            total++;
            if (!ok) failed++;
            printf("round %d block %d: median %ld us, shortest %ld, longest %ld, %d of %d within 10%%: %s\n",
                   r, b, us(med), us(lo), us(hi), good, CAL_N, ok ? "accepted" : "REFUSED");
            if (!ok)
            {
                printf("   ");
                for (i = 0; i < CAL_N; i++)
                    printf(" %ld", us(raw[i]));
                printf("\n");
            }
        }
        CloseScreen(scr);
    }
    printf("bovpprobe: %d of %d measurements refused\n", failed, total);
    rc = 0;

out:
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
