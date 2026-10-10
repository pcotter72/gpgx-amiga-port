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
