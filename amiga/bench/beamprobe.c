/*
 * beamprobe.c - how does the VideoCore scaler's line counter behave?
 *
 * VideoCore.card's WaitVerticalSync() (its vc4.c / vc6.c) polls the HVS
 * register SCALER_DISPSTAT1 ($f2400058 as the 68k sees it under Emu68) and
 * returns when its low 12 bits equal the display's height:
 *     do { } while ((LE32(*stat) & 0xfff) != vc4_DispSize.height);
 * rtg.library calls that once in every screen buffer swap, with or without
 * the driver's vertical blank interrupt. This tool only READS that register,
 * in a tight loop for a number of frames, and prints how the value runs:
 * the highest value, how long each value is held (one line), and how long
 * the values around the end of the picture are held - i.e. how wide the
 * window is that the driver's poll has to hit.
 *
 * Only for a PiStorm with Emu68 and VideoCore.card: the address is that
 * machine's, and nothing here checks where it runs. Do not start it
 * anywhere else.
 *
 * Usage:
 *   beamprobe [HEIGHT]        how the counter runs. HEIGHT = the display's
 *                             physical height (vc4_DispSize.height); with it
 *                             the window of that value is reported.
 *   beamprobe POLL HEIGHT [N] the driver's poll, copied: N times (default
 *                             200), each started 3 ms after the one before
 *                             returned, wait until the counter equals
 *                             HEIGHT; prints how long the waits took. One
 *                             refresh at most is the poll working; two means
 *                             it missed the value.
 *   beamprobe TICK SECONDS    only calls WaitTOF() in a loop for that long.
 *                             With the driver's vertical blank interrupt
 *                             this makes the interrupt fire in every frame;
 *                             run it in the background during a POLL to see
 *                             what the interrupt does to the poll.
 *
 * Build: D:/amiga-gcc-amiga6/bin/m68k-amigaos-gcc -m68040 -mhard-float
 *        -mschulz -noixemul -O2 -fno-peephole2 -Wall -o beamprobe
 *        beamprobe.c -lm
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <graphics/gfxbase.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct GfxBase *GfxBase;

struct Device *TimerBase;

#define STAT ((volatile ULONG *)0xf2400058UL)   /* SCALER_DISPSTAT1 */
#define MAXRUNS 60000
#define SAMPLES 4000000UL

struct run { UWORD value; UWORD pad; ULONG count; };

static ULONG efreq;

static double now_us(void)
{
    struct EClockVal ev;
    ReadEClock(&ev);
    return ((double)ev.ev_hi * 4294967296.0 + (double)ev.ev_lo) * 1000000.0 / (double)efreq;
}

/* The register is little-endian: its low 12 bits are in bytes 0 and 1 */
static UWORD line_now(void)
{
    ULONG v = *STAT;
    return (UWORD)((((v >> 24) & 0xff) | ((v >> 8) & 0xff00)) & 0xfff);
}

int main(int argc, char **argv)
{
    struct MsgPort *tport;
    struct timerequest *treq;
    struct run *r = NULL;
    ULONG n, nruns = 0, i, maxv = 0, typ_sum = 0, typ_n = 0;
    UWORD cur, v;
    double t0, t1, ns;
    long height = -1;
    /* r must be NULL for the early exits below */
    int shown = 0, mode = 0;        /* 1 = POLL, 2 = TICK */
    long count = 200, seconds = 5;

    if (argc > 1 && strcmp(argv[1], "POLL") == 0)
    {
        mode = 1;
        height = argc > 2 ? atol(argv[2]) : -1;
        if (argc > 3) count = atol(argv[3]);
        if (height < 0 || count < 1 || count > 5000)
        {
            printf("usage: beamprobe POLL HEIGHT [N]\n");
            return 5;
        }
    }
    else if (argc > 1 && strcmp(argv[1], "TICK") == 0)
    {
        mode = 2;
        if (argc > 2) seconds = atol(argv[2]);
        if (seconds < 1 || seconds > 120) seconds = 5;
    }
    else if (argc > 1)
        height = atol(argv[1]);

    tport = CreateMsgPort();
    treq = tport ? (struct timerequest *)CreateIORequest(tport, sizeof(*treq)) : NULL;
    if (!treq || OpenDevice("timer.device", UNIT_MICROHZ, (struct IORequest *)treq, 0))
    {
        printf("no timer.device\n");
        return 20;
    }
    TimerBase = treq->tr_node.io_Device;
    {
        struct EClockVal ev;
        efreq = ReadEClock(&ev);
    }
    if (mode == 2)
    {
        /* keep the vertical blank interrupt busy */
        ULONG ticks = 0;
        GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
        if (GfxBase)
        {
            t0 = now_us();
            while (now_us() - t0 < (double)seconds * 1000000.0)
            {
                WaitTOF();
                ticks++;
            }
            printf("beamprobe TICK: %lu WaitTOF calls in %ld s\n", (unsigned long)ticks, seconds);
            CloseLibrary((struct Library *)GfxBase);
        }
        r = NULL;
        goto out;
    }
    if (mode == 1)
    {
        /* The driver's poll, and how long each call of it takes */
        static ULONG hist[26];
        ULONG longest = 0, k, limit;
        double sum = 0.0;
        long c;

        for (c = 0; c < count; c++)
        {
            double t = now_us();
            while (now_us() - t < 3000.0)
                ;
            t0 = now_us();
            /* at most ~0.4 s, so that a wrong HEIGHT cannot hang this */
            for (limit = 4000000UL; limit && line_now() != (UWORD)height; limit--)
                ;
            t1 = now_us();
            if (!limit)
            {
                printf("the counter never showed %ld within 4000000 reads: wrong HEIGHT?\n", height);
                r = NULL;
                goto out;
            }
            k = (ULONG)((t1 - t0) / 2000.0);
            hist[k > 25 ? 25 : k]++;
            sum += t1 - t0;
            if ((ULONG)(t1 - t0) > longest)
                longest = (ULONG)(t1 - t0);
        }
        printf("beamprobe POLL: %ld waits for the value %ld, average %ld us, longest %lu us\n",
               count, height, (long)(sum / (double)count), (unsigned long)longest);
        printf("  2 ms buckets from 0 (last = 50 ms and more):\n   ");
        for (k = 0; k < 26; k++)
            printf(" %lu", (unsigned long)hist[k]);
        printf("\n");
        r = NULL;
        goto out;
    }

    r = (struct run *)AllocVec(sizeof(struct run) * MAXRUNS, MEMF_FAST | MEMF_CLEAR);
    if (!r)
    {
        printf("no memory\n");
        goto out;
    }

    cur = line_now();
    r[0].value = cur;
    t0 = now_us();
    for (n = 0; n < SAMPLES && nruns < MAXRUNS - 1; n++)
    {
        v = line_now();
        if (v != cur)
        {
            nruns++;
            r[nruns].value = v;
            cur = v;
        }
        r[nruns].count++;
    }
    t1 = now_us();
    ns = (t1 - t0) * 1000.0 / (double)n;
    printf("beamprobe: %lu samples in %ld us (%ld ns each), %lu runs\n",
           (unsigned long)n, (long)(t1 - t0), (long)ns, (unsigned long)(nruns + 1));

    for (i = 1; i < nruns; i++)
    {
        if (r[i].value > maxv)
            maxv = r[i].value;
        if (r[i].value >= 100 && r[i].value < 200)   /* ordinary picture lines */
        {
            typ_sum += r[i].count;
            typ_n++;
        }
    }
    printf("highest value %lu; an ordinary line (values 100-199) is held %ld us on average\n",
           (unsigned long)maxv, typ_n ? (long)((double)typ_sum / (double)typ_n * ns / 1000.0) : -1L);

    /* the runs around each end of picture: where the value stops rising */
    printf("around the end of the picture (value:us held), first frames:\n");
    for (i = 4; i + 6 < nruns && shown < 4; i++)
    {
        if (r[i + 1].value < r[i].value)        /* r[i] is the last before a fall */
        {
            ULONG k;
            printf(" ");
            for (k = i - 4; k <= i + 5; k++)
                printf(" %u:%ld", (unsigned)r[k].value, (long)((double)r[k].count * ns / 1000.0));
            printf("\n");
            if (shown == 0)
            {
                /* the first lines of the next picture, in ns: how far the
                 * counter runs ahead before it settles to one line's pace;
                 * in brackets the time since the end value began, in us */
                ULONG acc = 0;
                printf("  after it (value:ns held):\n ");
                for (k = i; k <= i + 44 && k < nruns; k++)
                {
                    if (k > i)
                        printf(" %u:%ld", (unsigned)r[k].value, (long)((double)r[k].count * ns));
                    acc += r[k].count;
                    if (k == i + 12 || k == i + 28 || k == i + 44)
                        printf(" [%ld us]\n ", (long)((double)acc * ns / 1000.0));
                }
                printf("\n");
            }
            shown++;
        }
    }

    if (height >= 0)
    {
        ULONG hits = 0, lo = 0xffffffffUL, hi = 0;
        double sum = 0.0;
        for (i = 1; i < nruns; i++)
            if (r[i].value == (UWORD)height)
            {
                hits++;
                sum += (double)r[i].count;
                if (r[i].count < lo) lo = r[i].count;
                if (r[i].count > hi) hi = r[i].count;
            }
        if (hits)
            printf("value %ld (the height given): seen %lu times, held %ld us on average"
                   " (shortest %ld, longest %ld us)\n", height, (unsigned long)hits,
                   (long)(sum / (double)hits * ns / 1000.0),
                   (long)((double)lo * ns / 1000.0), (long)((double)hi * ns / 1000.0));
        else
            printf("value %ld (the height given) was never seen\n", height);
    }

out:
    if (r) FreeVec(r);
    CloseDevice((struct IORequest *)treq);
    DeleteIORequest((struct IORequest *)treq);
    DeleteMsgPort(tport);
    return 0;
}
