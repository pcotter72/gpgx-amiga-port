/*
 * prof.h - frame-time profiler for test builds (make GPGX_PROFILE=1)
 *
 * Times each part of a frame with the Emu68 free-running counter
 * (control register CNTVALLO, read with MOVEC in supervisor mode).
 * Emu68 only: on a real 680x0 that control register does not exist and
 * MOVEC raises an exception. Never enable this in a release build.
 *
 * Time is exclusive: a section nested in another (FM synthesis inside a
 * 68000 write) is taken off the outer one, so all sections add up to the
 * measured frame.
 */
#ifndef GPGX_PROF_H
#define GPGX_PROF_H

enum
{
    PROF_OTHER,     /* main loop: input, cheats, bookkeeping */
    PROF_CORE,      /* system_frame_*() outside the sections below */
    PROF_68K,
    PROF_Z80,
    PROF_SVP,
    PROF_VDP,       /* render_line() / blank_line() */
    PROF_DMA,       /* vdp_dma_update() called from the frame loop */
    PROF_FM,        /* YM2612 synthesis (fm_update) */
    PROF_AUDIO,     /* audio_update(), rate control, backend submit */
    PROF_VIDEO,     /* blit / GPU output, FPS counter */
    PROF_SVP_CODE,  /* -profsvp: SVP time inside translated blocks (drc_enter) */
    PROF_SVP_INT,   /* -profsvp: SVP interpreter fallback (ssp1601_run_fast) */
    PROF_N
};

#ifdef GPGX_PROFILE
void prof_init(void);
void prof_frame_begin(void);
void prof_frame_end(void);
void prof_push(int section);
void prof_pop(void);
void prof_report(void);
void prof_interval(const char *extra);
unsigned long prof_jitctrl(unsigned long value, unsigned long write);  /* prof_asm.s */
unsigned long prof_jitctrl2(void);      /* prof_asm.s */
unsigned long prof_get_frames(void);
double prof_ticks_to_us(double ticks);
#define PROF_PUSH(s)        prof_push(s)
#define PROF_POP()          prof_pop()
#define PROF_CALL(s, call)  do { prof_push(s); call; prof_pop(); } while (0)
#else
#define PROF_PUSH(s)        ((void)0)
#define PROF_POP()          ((void)0)
#define PROF_CALL(s, call)  call
#endif

#endif /* GPGX_PROF_H */
