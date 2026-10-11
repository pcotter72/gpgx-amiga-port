/*
 * dispbench_asm.s - dispatch cost on Emu68 (amiga/bench/dispbench.c)
 *
 * Three ways to run n small handlers, as a CPU interpreter dispatches its
 * instruction handlers:
 *   bench_direct    bsr to one handler (no table, the baseline)
 *   bench_table     handler = table[ops[i]], called with jsr (a0) / rts
 *                   (Musashi's m68ki_instruction_jump_table[REG_IR]())
 *   bench_threaded  each handler looks up and jmp (a0)s to the next one
 * ops is a 4096-byte list of handler numbers (0-255), used in a cycle.
 * Each returns n (d0 is counted up by the handlers).
 *
 * Counter reads as in amiga/prof_asm.s (Emu68 control registers through
 * exec Supervisor(); they exist only on Emu68).
 */

    .text
    .even

/* ULONG bench_direct(ULONG n) */
    .globl  _bench_direct
_bench_direct:
    move.l  4(sp),d1
    moveq   #0,d0
.Ldir_loop:
    bsr.w   .Ldir_tiny
    subq.l  #1,d1
    bne.s   .Ldir_loop
    rts
.Ldir_tiny:
    addq.l  #1,d0
    rts

/* ULONG bench_table(ULONG n, const UBYTE *ops, ULONG *table) */
    .globl  _bench_table
_bench_table:
    movem.l d2-d4/a2-a3,-(sp)       /* 5 registers = 20 bytes */
    move.l  24(sp),d1               /* n */
    movea.l 28(sp),a2               /* ops */
    movea.l 32(sp),a3               /* table */
    moveq   #0,d0
    moveq   #0,d2                   /* index into ops, upper word stays 0 */
.Ltab_loop:
    moveq   #0,d3
    move.b  0(a2,d2.l),d3           /* handler number */
    addq.w  #1,d2
    and.w   #4095,d2
    lsl.w   #2,d3                   /* table offset [writes d3] */
    moveq   #0,d4                   /* bridge: d3 is not used as index right after the lsl */
    movea.l 0(a3,d3.w),a0
    jsr     (a0)
    subq.l  #1,d1
    bne.s   .Ltab_loop
    movem.l (sp)+,d2-d4/a2-a3
    rts

/* 256 handlers for bench_table, 4 bytes each */
    .globl  _bench_tiny_start, _bench_tiny_end
_bench_tiny_start:
    .rept   256
    addq.l  #1,d0
    rts
    .endr
_bench_tiny_end:

/* ULONG bench_threaded(ULONG n, const UBYTE *ops, ULONG *table) */
    .globl  _bench_threaded
_bench_threaded:
    movem.l d2-d4/a2-a3,-(sp)
    move.l  24(sp),d1
    movea.l 28(sp),a2
    movea.l 32(sp),a3
    moveq   #0,d0
    moveq   #0,d3
    move.b  (a2),d3                 /* first handler: ops[0] */
    moveq   #1,d2
    lsl.w   #2,d3
    moveq   #0,d4                   /* bridge */
    movea.l 0(a3,d3.w),a0
    jmp     (a0)

/* 256 handlers for bench_threaded, all the same size: each counts, stops
 * after n, else dispatches the next handler itself. The exit is placed
 * after them: Emu68 predicts a backward Bcc within 128 bytes as taken, a
 * forward one as not taken, and the exit is the rare case. */
    .globl  _bench_thr_start, _bench_thr_end
_bench_thr_start:
    .rept   256
    addq.l  #1,d0
    subq.l  #1,d1
    beq.w   .Lthr_exit
    moveq   #0,d3
    move.b  0(a2,d2.l),d3
    addq.w  #1,d2
    and.w   #4095,d2
    lsl.w   #2,d3
    moveq   #0,d4                   /* bridge */
    movea.l 0(a3,d3.w),a0
    jmp     (a0)
    .endr
_bench_thr_end:
.Lthr_exit:
    movem.l (sp)+,d2-d4/a2-a3
    rts

/* void bench_read3(ULONG out[3]) - out[0]=counter, [1]=m68k insns, [2]=ARM insns */
    .globl  _bench_read3
_bench_read3:
    movem.l a5-a6,-(sp)
    move.l  12(sp),a1
    lea     .Lread3_trap(pc),a5
    movea.l (4).w,a6
    jsr     -30(a6)                 /* Supervisor(a5) */
    movem.l (sp)+,a5-a6
    rts
.Lread3_trap:
    .word   0x4e7a,0x00e1           /* movec CNTVALLO,d0 */
    .word   0x4e7a,0x10e3           /* movec INSNCNTLO,d1 */
    move.l  d0,(a1)+
    move.l  d1,(a1)+
    .word   0x4e7a,0x00e5           /* movec ARMCNTLO,d0 */
    move.l  d0,(a1)
    rte

/* ULONG bench_freq(void) - counter frequency in Hz */
    .globl  _bench_freq
_bench_freq:
    movem.l a5-a6,-(sp)
    lea     .Lfreq_trap(pc),a5
    movea.l (4).w,a6
    jsr     -30(a6)
    movem.l (sp)+,a5-a6
    rts
.Lfreq_trap:
    .word   0x4e7a,0x00e0           /* movec CNTFRQ,d0 */
    rte
