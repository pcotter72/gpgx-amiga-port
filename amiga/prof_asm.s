/*
 * prof_asm.s - Emu68 counter access for amiga/prof.c (test builds only)
 *
 * The trap routines run through exec.library Supervisor(): "The function
 * must end with an RTE instruction", and "Supervisor() does not modify or
 * save registers" (NDK 3.1 exec.doc, Supervisor), so a1 still holds the
 * caller's pointer inside the trap.
 *
 * MOVEC Rc,Dn = $4E7A + extension word (register in bits 14-12, control
 * register in bits 11-0), as in that autodoc's example
 * "movec.l VBR,d0 ;$4e7a,$0801". Emu68 control registers
 * (docs/internals/ControlRegisters.md in the Emu68 source):
 *   $0E0 CNTFRQ    - counter frequency in Hz
 *   $0E1 CNTVALLO  - free-running counter, lower 32 bits
 *   $0E3 INSNCNTLO - executed M68k instructions, lower 32 bits
 *   $0E5 ARMCNTLO  - executed ARM instructions, lower 32 bits
 * They exist only on Emu68.
 */

    .text
    .even

/* void prof_read3(ULONG out[3]) - out[0]=counter, [1]=m68k insns, [2]=ARM insns */
    .globl  _prof_read3
_prof_read3:
    movem.l a5-a6,-(sp)
    move.l  12(sp),a1               /* out (after 8 bytes saved + return address) */
    lea     _prof_read3_trap(pc),a5
    movea.l (4).w,a6                /* SysBase */
    jsr     -30(a6)                 /* Supervisor(a5) */
    movem.l (sp)+,a5-a6
    rts

_prof_read3_trap:
    .word   0x4e7a,0x00e1           /* movec CNTVALLO,d0 */
    .word   0x4e7a,0x10e3           /* movec INSNCNTLO,d1 */
    move.l  d0,(a1)+
    move.l  d1,(a1)+
    .word   0x4e7a,0x00e5           /* movec ARMCNTLO,d0 */
    move.l  d0,(a1)
    rte

/* ULONG prof_freq_read(void) */
    .globl  _prof_freq_read
_prof_freq_read:
    movem.l a5-a6,-(sp)
    lea     _prof_frq_trap(pc),a5
    movea.l (4).w,a6
    jsr     -30(a6)
    movem.l (sp)+,a5-a6
    rts

_prof_frq_trap:
    .word   0x4e7a,0x00e0           /* movec CNTFRQ,d0 */
    rte

/* void prof_jit4(ULONG out[4]) - out[0]=JITSIZE, [1]=JITFREE, [2]=JITCMISS,
 * [3]=JITCOUNT
 *   $0E7 JITSIZE  - total JIT cache size in bytes
 *   $0E8 JITFREE  - free bytes in the JIT cache
 *   $0EC JITCMISS - JIT cache misses (translations started), 32-bit counter
 *   $0E9 JITCOUNT - translation units in the JIT cache */
    .globl  _prof_jit4
_prof_jit4:
    movem.l a5-a6,-(sp)
    move.l  12(sp),a1
    lea     _prof_jit4_trap(pc),a5
    movea.l (4).w,a6
    jsr     -30(a6)                 /* Supervisor(a5) */
    movem.l (sp)+,a5-a6
    rts

_prof_jit4_trap:
    .word   0x4e7a,0x00e7           /* movec JITSIZE,d0 */
    .word   0x4e7a,0x10e8           /* movec JITFREE,d1 */
    move.l  d0,(a1)+
    move.l  d1,(a1)+
    .word   0x4e7a,0x00ec           /* movec JITCMISS,d0 */
    .word   0x4e7a,0x10e9           /* movec JITCOUNT,d1 */
    move.l  d0,(a1)+
    move.l  d1,(a1)
    rte

/* ULONG prof_jitctrl(ULONG value, ULONG write) - returns JITCTRL ($0EB, the
 * Emu68 JIT control register, read/write) as it was, and stores value in
 * it when write is not 0. Fields (docs/internals/ControlRegisters.md):
 * bit 0 JCC_SOFT, bits 7-4 JCC_LOOP_COUNT, 23-8 JCC_INLINE_RANGE, 31-24
 * JCC_INSN_DEPTH (instructions per translation unit, 0 = 256).
 * MOVEC Dn,Rc = $4E7B (dr = 1, M68000PRM MOVEC). */
    .globl  _prof_jitctrl
_prof_jitctrl:
    movem.l d2/a5-a6,-(sp)
    move.l  16(sp),d0               /* value (after 12 bytes saved + return address) */
    move.l  20(sp),d1               /* write */
    lea     _prof_jitctrl_trap(pc),a5
    movea.l (4).w,a6
    jsr     -30(a6)                 /* Supervisor(a5) */
    movem.l (sp)+,d2/a5-a6
    rts

_prof_jitctrl_trap:
    .word   0x4e7a,0x20eb           /* movec JITCTRL,d2 */
    tst.l   d1
    beq.s   _prof_jitctrl_ro
    .word   0x4e7b,0x00eb           /* movec d0,JITCTRL */
_prof_jitctrl_ro:
    move.l  d2,d0
    rte

/* ULONG prof_jitctrl2(void) - JITCTRL2 ($1E0, Emu68's second JIT control
 * register); bit 1 = JC2_DBF_SLOWDOWN ("slow DBF", include/M68k.h) */
    .globl  _prof_jitctrl2
_prof_jitctrl2:
    movem.l a5-a6,-(sp)
    lea     _prof_jitctrl2_trap(pc),a5
    movea.l (4).w,a6
    jsr     -30(a6)                 /* Supervisor(a5) */
    movem.l (sp)+,a5-a6
    rts

_prof_jitctrl2_trap:
    .word   0x4e7a,0x01e0           /* movec JITCTRL2,d0 */
    rte
