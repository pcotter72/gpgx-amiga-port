/*
 * svp_drc_asm.s - entry into a translated SSP1601 block (amiga/ssp16_drc.h)
 *
 * void drc_enter(void *code, struct drc_regs *r)
 *   r:  0 a (rA32)   4 st (rST, upper word 0)   8 cyc   12 IRAM generation
 *      16 ssp1601_t *   20 svp->iram_rom
 * Loads the block's register set (see amiga/ssp16_drc_emit.h), calls the
 * block, and stores a, st and cyc back. The block leaves the next DSP PC
 * in drc_next_pc itself.
 */

    .text
    .even

    .globl  _drc_enter
_drc_enter:
    movem.l d2-d7/a2-a6,-(sp)       /* 11 registers = 44 bytes */
    movea.l 48(sp),a0               /* code (after 44 bytes + return address) */
    movea.l 52(sp),a1               /* regs */
    move.l  a1,-(sp)
    move.l  (a1),d7
    move.l  4(a1),d6
    move.l  8(a1),d5
    move.l  12(a1),d4
    movea.l 16(a1),a3
    movea.l 20(a1),a4
    jsr     (a0)
    movea.l (sp)+,a1
    move.l  d7,(a1)
    move.l  d6,4(a1)
    move.l  d5,8(a1)
    movem.l (sp)+,d2-d7/a2-a6
    rts
