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
extern void prof_jit4(ULONG *out);   /* JITSIZE, JITFREE, JITCMISS, JITCOUNT */

static ULONG prof_jit_start[4];


#define PROF_DEPTH 16

static const char *const prof_names[PROF_N] = {
    "main loop other", "core frame glue", "68000", "Z80", "SVP DSP",
    "VDP render", "VDP DMA", "FM (YM2612)", "audio out", "video out",
    "SVP in blocks", "SVP interpreter",
    "audio: blip read", "audio: convert", "audio: chip copy", "audio: device IO",
    "PSG (SN76489)", "FM -> blip"
};

int prof_svp_split;                     /* main.c -profsvp: time PROF_SVP_CODE/INT */

/* emulated work, counted in the core (m68kcpu.c, z80.c, sound.c) */
unsigned long prof_m68k_insns, prof_z80_insns, prof_fm_samples;

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
    prof_jit4(prof_jit_start);
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

/* One line for the frames since the previous call: wall time per frame of
 * the main sections, so the phases of a run (intro, demo, race) can be told
 * apart. extra is appended as it is. The printf itself lands in the next
 * frame's "main loop other". */
void prof_interval(const char *extra)
{
    static double last[PROF_N];
    static unsigned long last_frames;
    static ULONG last_miss;
    double f = (double)(prof_frames - last_frames), d[PROF_N], t = 0.0;
    static ULONG last_units;
    ULONG jit[4], made;
    int i;

    if (f <= 0.0 || !prof_freq)
        return;
    /* Emu68 translations started (JITCMISS) in this interval, units in the
     * cache and free JIT memory now. Emu68 frees units in two ways: when
     * allocating a new unit fails (64 least recently used at a time,
     * M68k_Translator.c), and after an instruction cache flush, when a
     * unit runs again and its code checksum no longer matches. Units
     * freed = translations - growth of the unit count. */
    prof_jit4(jit);
    if (!last_frames)
    {
        last_miss = prof_jit_start[2];
        last_units = prof_jit_start[3];
    }
    made = (ULONG)(jit[2] - last_miss);
    printf("Interval: Emu68 translations %lu per frame, units freed %ld per frame;"
           " now %lu units, %lu KB free\n",
           (ULONG)((double)made / f + 0.5),
           (LONG)((double)(LONG)(made - (jit[3] - last_units)) / f),
           jit[3], jit[1] / 1024);
    last_miss = jit[2];
    last_units = jit[3];
    for (i = 0; i < PROF_N; i++)
    {
        d[i] = (prof_acc[i][0] - last[i]) * 1000000.0 / (double)prof_freq / f;
        t += d[i];
        last[i] = prof_acc[i][0];
    }
    last_frames = prof_frames;
    printf("Interval to frame %lu: %lu us/frame: 68000 %lu, Z80 %lu, SVP %lu, VDP %lu,"
           " FM+PSG %lu, audio %lu, video %lu, rest %lu%s\n",
           prof_frames, (ULONG)(t + 0.5), (ULONG)(d[PROF_68K] + 0.5),
           (ULONG)(d[PROF_Z80] + 0.5),
           (ULONG)(d[PROF_SVP] + d[PROF_SVP_CODE] + d[PROF_SVP_INT] + 0.5),
           (ULONG)(d[PROF_VDP] + d[PROF_DMA] + 0.5),
           (ULONG)(d[PROF_FM] + d[PROF_FM_BLIP] + d[PROF_PSG] + 0.5),
           (ULONG)(d[PROF_AUDIO] + d[PROF_AUD_BLIP] + d[PROF_AUD_CONV]
                   + d[PROF_AUD_COPY] + d[PROF_AUD_IO] + 0.5),
           (ULONG)(d[PROF_VIDEO] + 0.5),
           (ULONG)(d[PROF_OTHER] + d[PROF_CORE] + 0.5), extra);
    {
        /* emulated work in this interval, and the time per unit (section
         * time as measured, the counter reads included) */
        static unsigned long last_c[3];
        unsigned long c[3];
        double n[3];
        c[0] = prof_m68k_insns; c[1] = prof_z80_insns; c[2] = prof_fm_samples;
        for (i = 0; i < 3; i++)
        {
            n[i] = (double)(c[i] - last_c[i]) / f;
            last_c[i] = c[i];
        }
        printf("Interval: per frame 68000 %lu insns (%lu ns each), Z80 %lu insns (%lu ns),"
               " FM %lu samples (%lu ns)\n",
               (ULONG)(n[0] + 0.5), n[0] > 0.0 ? (ULONG)(d[PROF_68K] * 1000.0 / n[0] + 0.5) : 0UL,
               (ULONG)(n[1] + 0.5), n[1] > 0.0 ? (ULONG)(d[PROF_Z80] * 1000.0 / n[1] + 0.5) : 0UL,
               (ULONG)(n[2] + 0.5), n[2] > 0.0 ? (ULONG)(d[PROF_FM] * 1000.0 / n[2] + 0.5) : 0UL);
    }
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
    {
        ULONG j[4];
        prof_jit4(j);
        printf("Profile: Emu68 JIT cache %lu KB, %lu KB free; %lu translations per frame\n",
               j[0] / 1024, j[1] / 1024,
               (ULONG)((double)(ULONG)(j[2] - prof_jit_start[2]) / (double)prof_frames + 0.5));
    }
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
    {
        /* Cost per emulated unit, with one counter read per call taken off
         * the section's time and host instructions */
        static const int sec[3] = { PROF_68K, PROF_Z80, PROF_FM };
        static const char *const what[3] = { "68000 instruction", "Z80 instruction", "FM sample" };
        unsigned long cnt[3];
        double f = (double)prof_frames;
        cnt[0] = prof_m68k_insns; cnt[1] = prof_z80_insns; cnt[2] = prof_fm_samples;
        for (k = 0; k < 3; k++)
        {
            double calls = (double)prof_calls[sec[k]];
            double us = prof_acc[sec[k]][0] * us_per_tick - calls * read[0] * us_per_tick;
            double hi = prof_acc[sec[k]][1] - calls * read[1];
            if (!cnt[k])
                continue;
            printf("Profile: %-17s %7lu per frame, %4lu ns and %3lu host m68k insns each\n",
                   what[k], (ULONG)((double)cnt[k] / f + 0.5),
                   (ULONG)(us * 1000.0 / (double)cnt[k] + 0.5),
                   (ULONG)(hi / (double)cnt[k] + 0.5));
        }
    }
}

#endif /* GPGX_PROFILE */
