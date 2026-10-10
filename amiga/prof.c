/*
 * prof.c - frame-time profiler for test builds (make GPGX_PROFILE=1)
 *
 * See prof.h. Emu68 only. Per section it accumulates wall time and the
 * number of M68k and ARM instructions Emu68 executed, so a slow section can
 * be told apart: many M68k instructions (the C code does too much) or few
 * but expensive ones (JIT dispatch, e.g. computed jumps).
 */
#ifdef GPGX_PROFILE

#include <stdio.h>
#include <exec/types.h>

#include "prof.h"

extern void prof_read3(ULONG *out);     /* prof_asm.s: counter, m68k, ARM */
extern ULONG prof_freq_read(void);

#define PROF_DEPTH 16

static const char *const prof_names[PROF_N] = {
    "main loop other", "core frame glue", "68000", "Z80", "SVP DSP",
    "VDP render", "VDP DMA", "FM (YM2612)", "audio out", "video out"
};

static double prof_acc[PROF_N][3];      /* exclusive ticks, m68k insns, ARM insns */
static unsigned long prof_calls[PROF_N];
static int prof_stack[PROF_DEPTH];
static int prof_depth;                  /* may exceed PROF_DEPTH: then ignored */
static int prof_cur;
static ULONG prof_last[3];
static ULONG prof_freq;
static unsigned long prof_frames;
static int prof_running;

void prof_init(void)
{
    int i, k;
    prof_freq = prof_freq_read();
    for (i = 0; i < PROF_N; i++)
    {
        for (k = 0; k < 3; k++)
            prof_acc[i][k] = 0.0;
        prof_calls[i] = 0;
    }
    prof_frames = 0;
    prof_running = 0;
}

static void prof_switch(int to)
{
    ULONG now[3];
    int k;
    prof_read3(now);
    /* unsigned 32-bit differences: correct across a wrap as long as one
     * interval is shorter than the wrap period */
    for (k = 0; k < 3; k++)
    {
        prof_acc[prof_cur][k] += (double)(ULONG)(now[k] - prof_last[k]);
        prof_last[k] = now[k];
    }
    prof_cur = to;
}

void prof_frame_begin(void)
{
    prof_depth = 0;
    prof_cur = PROF_OTHER;
    prof_read3(prof_last);
    prof_running = 1;
}

void prof_frame_end(void)
{
    if (!prof_running)
        return;
    prof_switch(PROF_OTHER);
    prof_running = 0;
    prof_frames++;
}

void prof_push(int section)
{
    if (!prof_running)
        return;
    if (prof_depth < PROF_DEPTH)
    {
        prof_stack[prof_depth] = prof_cur;
        prof_calls[section]++;
        prof_switch(section);
    }
    prof_depth++;
}

void prof_pop(void)
{
    if (!prof_running || prof_depth == 0)
        return;
    prof_depth--;
    if (prof_depth < PROF_DEPTH)
        prof_switch(prof_stack[prof_depth]);
}

double prof_ticks_to_us(double ticks)
{
    return prof_freq ? ticks * 1000000.0 / (double)prof_freq : 0.0;
}

unsigned long prof_get_frames(void)
{
    return prof_frames;
}

void prof_report(void)
{
    ULONG a[3], b[3];
    double read[3], total = 0.0, us_per_tick;
    int i, k, r;

    if (!prof_frames || !prof_freq)
        return;

    /* Cost of one read (Supervisor trap + three MOVECs) in all three units */
    prof_read3(a);
    for (r = 0; r < 1000; r++)
        prof_read3(b);
    prof_read3(b);
    for (k = 0; k < 3; k++)
        read[k] = (double)(ULONG)(b[k] - a[k]) / 1001.0;
    us_per_tick = 1000000.0 / (double)prof_freq;

    for (i = 0; i < PROF_N; i++)
        total += prof_acc[i][0];

    printf("Profile: %lu frames, counter %lu Hz; one read = %lu ns, %lu m68k, %lu ARM insns\n",
           prof_frames, (unsigned long)prof_freq,
           (unsigned long)(read[0] * us_per_tick * 1000.0 + 0.5),
           (unsigned long)(read[1] + 0.5), (unsigned long)(read[2] + 0.5));
    printf("Profile: section            us/frame  share  calls/fr  m68k ins/fr  ns/m68k ARM/m68k\n");
    for (i = 0; i < PROF_N; i++)
    {
        double f = (double)prof_frames;
        ULONG us = (ULONG)(prof_acc[i][0] * us_per_tick / f + 0.5);
        ULONG pct10 = total > 0.0 ? (ULONG)(prof_acc[i][0] * 1000.0 / total + 0.5) : 0;
        ULONG cpf = (ULONG)((double)prof_calls[i] / f + 0.5);
        ULONG ins = (ULONG)(prof_acc[i][1] / f + 0.5);
        ULONG ns10 = prof_acc[i][1] > 0.0
            ? (ULONG)(prof_acc[i][0] * us_per_tick * 10000.0 / prof_acc[i][1] + 0.5) : 0;
        ULONG arm10 = prof_acc[i][1] > 0.0
            ? (ULONG)(prof_acc[i][2] * 10.0 / prof_acc[i][1] + 0.5) : 0;
        printf("Profile:   %-18s %8lu %3lu.%lu%% %8lu %12lu %5lu.%lu %5lu.%lu\n",
               prof_names[i], us, pct10 / 10, pct10 % 10, cpf, ins,
               ns10 / 10, ns10 % 10, arm10 / 10, arm10 % 10);
    }
    printf("Profile: total %lu us/frame. Each call above also carries about one read\n"
           "Profile: of overhead (calls/frame x the read cost on the first line).\n",
           (ULONG)(total * us_per_tick / (double)prof_frames + 0.5));
}

#endif /* GPGX_PROFILE */
