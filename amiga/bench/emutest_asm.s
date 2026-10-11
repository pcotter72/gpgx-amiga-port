/*
 * emutest_asm.s - instruction forms gcc started to use in Musashi when the
 * memory-access paths got branch hints (M68K_RARE_HINTS), which broke the
 * display in Sonic 3 on Emu68 (2026-10-11). Each test runs one of those
 * forms with known values and returns its result in d0; emutest.c compares
 * it with the value the 68000 PRM defines. 68020+ (scaled/indexed forms).
 */

    .text
    .even

/* ULONG t_lea_an_w(ULONG base, ULONG index)  - lea (0,a1,a2.w),a1 */
    .globl  _t_lea_an_w
_t_lea_an_w:
    move.l  a2,-(sp)
    movea.l 8(sp),a1
    movea.l 12(sp),a2
    lea     (0,a1,a2.w),a1
    move.l  a1,d0
    movea.l (sp)+,a2
    rts

/* ULONG t_bra16(ULONG *st)  - the fast path of m68k_op_bra_16 as gcc
 * emitted it with the hints: st[0] = PC, st[1] = prefetch address,
 * st[2] = prefetch data (zero-extended word). Returns the new PC, or
 * 0xdead0000 when the compare took the other way. */
    .globl  _t_bra16
_t_bra16:
    movem.l a2-a4,-(sp)
    movea.l 16(sp),a0
    movea.l (a0),a1                 /* PC */
    cmpa.l  4(a0),a1                /* == prefetch address? */
    bne.s   .Lbra_other
    move.l  a1,d0
    addq.l  #2,d0
    movea.l 8(a0),a2                /* prefetch data: the displacement */
    move.l  d0,(a0)
    move.l  d0,4(a0)
    lea     (0,a1,a2.w),a1          /* PC + sign-extended displacement */
    move.l  a1,d0
    movem.l (sp)+,a2-a4
    rts
.Lbra_other:
    move.l  #0xdead0000,d0
    movem.l (sp)+,a2-a4
    rts

/* ULONG t_pea_an_w(ULONG base, ULONG index)  - pea (0,a1,a0.w) */
    .globl  _t_pea_an_w
_t_pea_an_w:
    movea.l 4(sp),a1
    movea.l 8(sp),a0
    pea     (0,a1,a0.w)
    move.l  (sp)+,d0
    rts

/* ULONG t_cmpa(ULONG *mem, ULONG an)  - cmpa.l 4(a0),a1 ; seq: 0xff if equal */
    .globl  _t_cmpa
_t_cmpa:
    movea.l 4(sp),a0
    movea.l 8(sp),a1
    moveq   #0,d0
    cmpa.l  4(a0),a1
    seq     d0
    rts

/* Byte loads merged into a register whose upper bits a moveq set, as gcc
 * emitted m68k_op_add_8_er_ai with M68K_RARE_RD (2026-10-11):
 *   moveq #20,d0 / muls.l d0,d1 / ... / move.b (0,a0,d3.l),d0 / add.l d0,d1
 * The add uses all 32 bits of d0, so its upper 24 bits must still be 0. */

/* ULONG t_moveb_after_muls(UBYTE *buf, ULONG index) - the handler's order */
    .globl  _t_moveb_after_muls
_t_moveb_after_muls:
    movem.l d2-d4/a2,-(sp)
    movea.l 20(sp),a2
    move.l  24(sp),d3
    moveq   #3,d1
    moveq   #20,d0
    muls.l  d0,d1                   /* d1 = 60, d0 stays 20 */
    movea.l a2,a0
    move.l  a0,d4                   /* the read-handler test, in d4 */
    bne.s   .Lmb_direct
    moveq   #-1,d0
.Lmb_direct:
    andi.l  #65535,d3
    move.b  (0,a0,d3.l),d0          /* d0 = 0x000000xx */
    movem.l (sp)+,d2-d4/a2
    rts

/* ULONG t_moveb_after_moveq(UBYTE *buf) - moveq #20 then move.b (a0),d0 */
    .globl  _t_moveb_after_moveq
_t_moveb_after_moveq:
    movea.l 4(sp),a0
    moveq   #20,d0
    move.b  (a0),d0
    rts

/* ULONG t_moveb_after_moveq_neg(UBYTE *buf) - moveq #-1: upper bits must stay 1 */
    .globl  _t_moveb_after_moveq_neg
_t_moveb_after_moveq_neg:
    movea.l 4(sp),a0
    moveq   #-1,d0
    move.b  (a0),d0
    rts

/* ULONG t_add8_flow(UBYTE *buf, ULONG index, ULONG d) - load as above, then
 * add.l d0,d1 with d1 = d & 0xff, returns the 32-bit sum (bit 8 = carry) */
    .globl  _t_add8_flow
_t_add8_flow:
    movem.l d2-d4/a2,-(sp)
    movea.l 20(sp),a2
    move.l  24(sp),d3
    moveq   #3,d1
    moveq   #20,d0
    muls.l  d0,d1
    movea.l a2,a0
    move.l  a0,d4
    bne.s   .Lad_direct
    moveq   #-1,d0
.Lad_direct:
    andi.l  #65535,d3
    move.b  (0,a0,d3.l),d0
    moveq   #0,d4
    not.b   d4                      /* d4 = 0xff */
    and.l   28(sp),d4               /* destination byte */
    move.l  d4,d1
    add.l   d0,d1                   /* res: bit 8 is the carry */
    move.l  d1,d0
    movem.l (sp)+,d2-d4/a2
    rts

/* ULONG t_move_w_an(UWORD *buf, ULONG value, ULONG offset)
 *   move.w a0,(0,a1,d0.l) ; returns the word read back */
    .globl  _t_move_w_an
_t_move_w_an:
    movea.l 4(sp),a1
    movea.l 8(sp),a0
    move.l  12(sp),d0
    move.w  a0,(0,a1,d0.l)
    move.l  d0,d1
    moveq   #0,d0
    move.w  (0,a1,d1.l),d0
    rts
