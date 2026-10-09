/*
 * ssp1601_run_asm - Hand-optimized 68k assembly SSP1601 interpreter
 *
 * Replaces the C switch/case interpreter with register-allocated,
 * jump-table dispatched 68k code for significantly better performance.
 *
 * Register allocation:
 *   d4 = rA32 (32-bit accumulator)
 *   d5 = rST  (16-bit status, kept in low word)
 *   d6 = g_cycles counter
 *   d7 = current opcode
 *   a2 = PC pointer (into iram_rom, word-sized program memory)
 *   a3 = ssp (ssp1601_t base pointer)
 *   a4 = svp->iram_rom base
 *   a5 = jump table base
 *   d0-d3 = scratch
 *   a0-a1 = scratch
 *   a6 = saved frame pointer
 *   a7 = stack (preserved)
 *
 * SSP struct offsets (big-endian, from ssp1601_t base):
 */

/* Memory */
SSP_RAM0    = 0          /* RAM0[256] - 512 bytes */
SSP_RAM1    = 512        /* RAM1[256] - 512 bytes */

/* General registers - ssp_reg_t is 4 bytes each */
/* On big-endian m68k: .byte.h is at offset+0 (upper 16), .byte.l at offset+2 */
SSP_GR      = 1024       /* gr[16] array base */
SSP_GR0_H   = 1024       /* gr[0].byte.h - constant 0xFFFF */
SSP_X_H     = 1028       /* gr[1].byte.h - X register */
SSP_Y_H     = 1032       /* gr[2].byte.h - Y register */
SSP_A_V     = 1036       /* gr[3].v - rA32 (full 32-bit accumulator) */
SSP_A_H     = 1036       /* gr[3].byte.h - rA (upper 16 bits) */
SSP_A_L     = 1038       /* gr[3].byte.l - rAL (lower 16 bits) */
SSP_ST_H    = 1040       /* gr[4].byte.h - Status register */
SSP_STACK_H = 1044       /* gr[5].byte.h - Stack pointer */
SSP_PC_H    = 1048       /* gr[6].byte.h - Program counter */
SSP_P_V     = 1052       /* gr[7].v - P register (full 32-bit) */
SSP_P_H     = 1052       /* gr[7].byte.h - P upper */
SSP_PM0_H   = 1056       /* gr[8].byte.h */
SSP_PM1_H   = 1060
SSP_PM2_H   = 1064
SSP_XST_H   = 1068
SSP_PM4_H   = 1072
SSP_GR13_H  = 1076
SSP_PMC_V   = 1080       /* gr[14].v - PMC full 32-bit */
SSP_PMC_H   = 1080       /* gr[14].byte.h - PMC addr/mode high */
SSP_PMC_L   = 1082       /* gr[14].byte.l - PMC addr/mode low */
SSP_AL_V    = 1084       /* gr[15] - but rAL is actually gr[3].byte.l = offset 1038 */

/* Pointer registers */
SSP_PTR     = 1088       /* ptr.r[8] - 8 bytes */
SSP_PTR_R0  = 1088       /* ptr.bank.r0[4] */
SSP_PTR_R1  = 1092       /* ptr.bank.r1[4] */

/* Stack */
SSP_STACK   = 1096       /* stack[6] - 12 bytes */

/* PMAC */
SSP_PMAC    = 1108       /* pmac[2][6] - 48 bytes */

/* Emu status */
SSP_EMUST   = 1156       /* emu_status */

/* Wait mask for loop exit */
SSP_WAIT_MASK = 0xF000

/* Status flag bits */
FLAG_L = 0x1000
FLAG_Z = 0x2000
FLAG_V = 0x4000
FLAG_N = 0x8000

    .text
    .even

    .globl _ssp1601_run_asm

/*
 * Extern references - C functions we call for complex operations
 */
    .extern _ssp
    .extern _svp

/*
 * void ssp1601_run_asm(int cycles)
 *
 * Entry: 4(sp) = cycle count
 */
_ssp1601_run_asm:
    movem.l d2-d7/a2-a6,-(sp)    /* save all callee-saved regs */

    move.l  4+44(sp),d6          /* d6 = cycles (arg after 11 longs saved) */

    /* Load ssp and svp pointers */
    move.l  _ssp,a3              /* a3 = ssp struct base */
    move.l  _svp,a4              /* a4 = svp pointer = &svp->iram_rom (offset 0) */

    /* Load hot state into registers */
    move.l  SSP_A_V(a3),d4       /* d4 = rA32 */
    move.w  SSP_ST_H(a3),d5      /* d5 = rST */

    /* SET_PC: PC = (u16*)svp->iram_rom + rPC */
    moveq   #0,d0
    move.w  SSP_PC_H(a3),d0      /* d0 = rPC (16-bit offset) */
    lea     (a4,d0.l*2),a2       /* a2 = word pointer into iram_rom */

    /* Load jump table base */
    lea     .Ljumptable(pc),a5

    /* Main interpreter loop */
.Lmain_loop:
    /* Fetch opcode */
    move.w  (a2)+,d7              /* d7 = op = *PC++ */

    /* Dispatch: switch(op >> 9) - 7 bits = 0..127 */
    move.w  d7,d0
    lsr.w   #8,d0                 /* d0 = op >> 8 */
    lsr.w   #1,d0                 /* d0 = op >> 9 */
    add.w   d0,d0                 /* d0 *= 2 (word offset into table) */
    move.w  (a5,d0.w),d0         /* d0 = offset from jumptable */
    jmp     (a5,d0.w)            /* jump to handler */

/* ======================================================================
 * Opcode handlers
 * Each handler processes its opcode and jumps to .Lnext
 * ====================================================================== */

.Lnext:
    subq.l  #1,d6                /* g_cycles-- */
    ble.s   .Lexit_cycles        /* exit if cycles exhausted */
    move.l  SSP_EMUST(a3),d0
    and.l   #SSP_WAIT_MASK,d0
    bne.s   .Lexit_wait          /* exit if wait condition set */
    bra.s   .Lmain_loop

.Lexit_cycles:
    /* Check wait condition too */
    move.l  SSP_EMUST(a3),d0
    and.l   #SSP_WAIT_MASK,d0
    bne.s   .Lexit_wait
    tst.l   d6
    bgt.s   .Lmain_loop          /* continue if cycles > 0 */
.Lexit_wait:

    /* ---- Epilogue: save state back and call read_P ---- */
    move.l  d4,SSP_A_V(a3)       /* save rA32 */
    move.w  d5,SSP_ST_H(a3)      /* save rST */

    /* rPC = GET_PC() = (PC - iram_rom_base) / 2 */
    move.l  a2,d0
    sub.l   a4,d0                 /* byte offset */
    lsr.l   #1,d0                 /* word offset */
    move.w  d0,SSP_PC_H(a3)      /* save rPC */

    /* Call read_P to update P register: P = X * Y * 2 */
    move.w  SSP_X_H(a3),d0       /* signed X */
    move.w  SSP_Y_H(a3),d1       /* signed Y */
    muls.w  d1,d0                /* d0 = X * Y (signed 32-bit result) */
    add.l   d0,d0                /* d0 = X * Y * 2 */
    move.l  d0,SSP_P_V(a3)       /* rP.v = result */

    movem.l (sp)+,d2-d7/a2-a6   /* restore regs */
    rts

/* ======================================================================
 * Jump table - 128 entries, word offsets from table base
 * ====================================================================== */
    .even
.Ljumptable:
    .word .Lop_00-.Ljumptable     /* 0x00: ld d, s */
    .word .Lop_01-.Ljumptable     /* 0x01: ld d, (ri) */
    .word .Lop_02-.Ljumptable     /* 0x02: ld (ri), s */
    .word .Lop_03-.Ljumptable     /* 0x03: ld a, adr */
    .word .Lop_04-.Ljumptable     /* 0x04: ldi d, imm */
    .word .Lop_05-.Ljumptable     /* 0x05: ld d, ((ri)) */
    .word .Lop_06-.Ljumptable     /* 0x06: ldi (ri), imm */
    .word .Lop_07-.Ljumptable     /* 0x07: ld adr, a */
    .word .Lop_bad-.Ljumptable    /* 0x08: unused */
    .word .Lop_09-.Ljumptable     /* 0x09: ld d, ri */
    .word .Lop_0a-.Ljumptable     /* 0x0a: ld ri, s */
    .word .Lop_bad-.Ljumptable    /* 0x0b: unused */
    .word .Lop_0c-.Ljumptable     /* 0x0c: ldi ri, simm */
    .word .Lop_0c-.Ljumptable     /* 0x0d: ldi ri, simm */
    .word .Lop_0c-.Ljumptable     /* 0x0e: ldi ri, simm */
    .word .Lop_0c-.Ljumptable     /* 0x0f: ldi ri, simm */
    .word .Lop_10-.Ljumptable     /* 0x10: sub a, s */
    .word .Lop_11-.Ljumptable     /* 0x11: sub a, (ri) */
    .word .Lop_bad-.Ljumptable    /* 0x12: unused */
    .word .Lop_13-.Ljumptable     /* 0x13: sub a, adr */
    .word .Lop_14-.Ljumptable     /* 0x14: sub a, imm */
    .word .Lop_15-.Ljumptable     /* 0x15: sub a, ((ri)) */
    .word .Lop_bad-.Ljumptable    /* 0x16: unused */
    .word .Lop_bad-.Ljumptable    /* 0x17: unused */
    .word .Lop_bad-.Ljumptable    /* 0x18: unused */
    .word .Lop_19-.Ljumptable     /* 0x19: sub a, ri */
    .word .Lop_bad-.Ljumptable    /* 0x1a: unused */
    .word .Lop_1b-.Ljumptable     /* 0x1b: mpys */
    .word .Lop_1c-.Ljumptable     /* 0x1c: sub simm */
    .word .Lop_bad-.Ljumptable    /* 0x1d: unused */
    .word .Lop_bad-.Ljumptable    /* 0x1e: unused */
    .word .Lop_bad-.Ljumptable    /* 0x1f: unused */
    .word .Lop_bad-.Ljumptable    /* 0x20: unused */
    .word .Lop_bad-.Ljumptable    /* 0x21: unused */
    .word .Lop_bad-.Ljumptable    /* 0x22: unused */
    .word .Lop_bad-.Ljumptable    /* 0x23: unused */
    .word .Lop_24-.Ljumptable     /* 0x24: call cond, addr */
    .word .Lop_25-.Ljumptable     /* 0x25: ld d, (a) */
    .word .Lop_26-.Ljumptable     /* 0x26: bra cond, addr */
    .word .Lop_bad-.Ljumptable    /* 0x27: unused */
    .word .Lop_bad-.Ljumptable    /* 0x28: unused */
    .word .Lop_bad-.Ljumptable    /* 0x29: unused */
    .word .Lop_bad-.Ljumptable    /* 0x2a: unused */
    .word .Lop_bad-.Ljumptable    /* 0x2b: unused */
    .word .Lop_bad-.Ljumptable    /* 0x2c: unused */
    .word .Lop_bad-.Ljumptable    /* 0x2d: unused */
    .word .Lop_bad-.Ljumptable    /* 0x2e: unused */
    .word .Lop_bad-.Ljumptable    /* 0x2f: unused */
    .word .Lop_30-.Ljumptable     /* 0x30: cmp a, s */
    .word .Lop_31-.Ljumptable     /* 0x31: cmp a, (ri) */
    .word .Lop_bad-.Ljumptable    /* 0x32: unused */
    .word .Lop_33-.Ljumptable     /* 0x33: cmp a, adr */
    .word .Lop_34-.Ljumptable     /* 0x34: cmp a, imm */
    .word .Lop_35-.Ljumptable     /* 0x35: cmp a, ((ri)) */
    .word .Lop_bad-.Ljumptable    /* 0x36: unused */
    .word .Lop_bad-.Ljumptable    /* 0x37: unused */
    .word .Lop_bad-.Ljumptable    /* 0x38: unused */
    .word .Lop_39-.Ljumptable     /* 0x39: cmp a, ri */
    .word .Lop_bad-.Ljumptable    /* 0x3a: unused */
    .word .Lop_bad-.Ljumptable    /* 0x3b: unused */
    .word .Lop_3c-.Ljumptable     /* 0x3c: cmp simm */
    .word .Lop_bad-.Ljumptable    /* 0x3d: unused */
    .word .Lop_bad-.Ljumptable    /* 0x3e: unused */
    .word .Lop_bad-.Ljumptable    /* 0x3f: unused */
    .word .Lop_40-.Ljumptable     /* 0x40: add a, s */
    .word .Lop_41-.Ljumptable     /* 0x41: add a, (ri) */
    .word .Lop_bad-.Ljumptable    /* 0x42: unused */
    .word .Lop_43-.Ljumptable     /* 0x43: add a, adr */
    .word .Lop_44-.Ljumptable     /* 0x44: add a, imm */
    .word .Lop_45-.Ljumptable     /* 0x45: add a, ((ri)) */
    .word .Lop_bad-.Ljumptable    /* 0x46: unused */
    .word .Lop_bad-.Ljumptable    /* 0x47: unused */
    .word .Lop_48-.Ljumptable     /* 0x48: mod cond, op */
    .word .Lop_49-.Ljumptable     /* 0x49: add a, ri */
    .word .Lop_bad-.Ljumptable    /* 0x4a: unused */
    .word .Lop_4b-.Ljumptable     /* 0x4b: mpya */
    .word .Lop_4c-.Ljumptable     /* 0x4c: add simm */
    .word .Lop_bad-.Ljumptable    /* 0x4d: unused */
    .word .Lop_bad-.Ljumptable    /* 0x4e: unused */
    .word .Lop_bad-.Ljumptable    /* 0x4f: unused */
    .word .Lop_50-.Ljumptable     /* 0x50: and a, s */
    .word .Lop_51-.Ljumptable     /* 0x51: and a, (ri) */
    .word .Lop_bad-.Ljumptable    /* 0x52: unused */
    .word .Lop_53-.Ljumptable     /* 0x53: and a, adr */
    .word .Lop_54-.Ljumptable     /* 0x54: and a, imm */
    .word .Lop_55-.Ljumptable     /* 0x55: and a, ((ri)) */
    .word .Lop_bad-.Ljumptable    /* 0x56: unused */
    .word .Lop_bad-.Ljumptable    /* 0x57: unused */
    .word .Lop_bad-.Ljumptable    /* 0x58: unused */
    .word .Lop_59-.Ljumptable     /* 0x59: and a, ri */
    .word .Lop_bad-.Ljumptable    /* 0x5a: unused */
    .word .Lop_5b-.Ljumptable     /* 0x5b: mld */
    .word .Lop_5c-.Ljumptable     /* 0x5c: and simm */
    .word .Lop_bad-.Ljumptable    /* 0x5d: unused */
    .word .Lop_bad-.Ljumptable    /* 0x5e: unused */
    .word .Lop_bad-.Ljumptable    /* 0x5f: unused */
    .word .Lop_60-.Ljumptable     /* 0x60: or a, s */
    .word .Lop_61-.Ljumptable     /* 0x61: or a, (ri) */
    .word .Lop_bad-.Ljumptable    /* 0x62: unused */
    .word .Lop_63-.Ljumptable     /* 0x63: or a, adr */
    .word .Lop_64-.Ljumptable     /* 0x64: or a, imm */
    .word .Lop_65-.Ljumptable     /* 0x65: or a, ((ri)) */
    .word .Lop_bad-.Ljumptable    /* 0x66: unused */
    .word .Lop_bad-.Ljumptable    /* 0x67: unused */
    .word .Lop_bad-.Ljumptable    /* 0x68: unused */
    .word .Lop_69-.Ljumptable     /* 0x69: or a, ri */
    .word .Lop_bad-.Ljumptable    /* 0x6a: unused */
    .word .Lop_bad-.Ljumptable    /* 0x6b: unused */
    .word .Lop_6c-.Ljumptable     /* 0x6c: or simm */
    .word .Lop_bad-.Ljumptable    /* 0x6d: unused */
    .word .Lop_bad-.Ljumptable    /* 0x6e: unused */
    .word .Lop_bad-.Ljumptable    /* 0x6f: unused */
    .word .Lop_70-.Ljumptable     /* 0x70: eor a, s */
    .word .Lop_71-.Ljumptable     /* 0x71: eor a, (ri) */
    .word .Lop_bad-.Ljumptable    /* 0x72: unused */
    .word .Lop_73-.Ljumptable     /* 0x73: eor a, adr */
    .word .Lop_74-.Ljumptable     /* 0x74: eor a, imm */
    .word .Lop_75-.Ljumptable     /* 0x75: eor a, ((ri)) */
    .word .Lop_bad-.Ljumptable    /* 0x76: unused */
    .word .Lop_bad-.Ljumptable    /* 0x77: unused */
    .word .Lop_bad-.Ljumptable    /* 0x78: unused */
    .word .Lop_79-.Ljumptable     /* 0x79: eor a, ri */
    .word .Lop_bad-.Ljumptable    /* 0x7a: unused */
    .word .Lop_bad-.Ljumptable    /* 0x7b: unused */
    .word .Lop_7c-.Ljumptable     /* 0x7c: eor simm */
    .word .Lop_bad-.Ljumptable    /* 0x7d: unused */
    .word .Lop_bad-.Ljumptable    /* 0x7e: unused */
    .word .Lop_bad-.Ljumptable    /* 0x7f: unused */

/* ======================================================================
 * Helper macros/subroutines
 * ====================================================================== */

/*
 * UPD_ACC_ZN - update Z and N flags from d4 (rA32)
 * Clobbers: d0
 */
.macro UPD_ACC_ZN
    and.w   #0x0FFF,d5           /* clear L,Z,V,N */
    tst.l   d4
    bne.s   .Lupd_\@_notz
    or.w    #FLAG_Z,d5
    bra.s   .Lupd_\@_done
.Lupd_\@_notz:
    move.l  d4,d0
    swap    d0                   /* get high word */
    and.w   #FLAG_N,d0           /* bit 15 = N flag */
    or.w    d0,d5
.Lupd_\@_done:
.endm

/*
 * UPD_LZVN - same as UPD_ACC_ZN (L and V not used by SVP)
 */
.macro UPD_LZVN
    UPD_ACC_ZN
.endm

/*
 * READ_P - compute P = X * Y * 2 into d0, also updates SSP_P_V
 * Result in d0.l
 * Clobbers: d0, d1
 */
.macro READ_P
    move.w  SSP_X_H(a3),d0
    move.w  SSP_Y_H(a3),d1
    muls.w  d1,d0                /* d0 = X * Y (signed 32) */
    add.l   d0,d0                /* d0 = X * Y * 2 */
    move.l  d0,SSP_P_V(a3)      /* update P register */
.endm

/*
 * REG_READ_FAST - read SSP register by number (in d0.w, 0-15)
 * For regs 0-3: direct inline access (very fast)
 * For regs 4+: falls through to C handler call
 * Result in d0.w
 * Clobbers: d0, d1, a0, a1 (if calling handler)
 */
.Lreg_read:
    cmp.w   #4,d0
    bge.s   .Lreg_read_handler
    /* regs 0-3: direct access to gr[r].byte.h */
    lsl.w   #2,d0                /* offset = r * 4 */
    move.w  SSP_GR(a3,d0.w),d0  /* d0 = ssp->gr[r].byte.h */
    rts

.Lreg_read_handler:
    /* Save state to memory before calling C */
    move.l  d4,SSP_A_V(a3)
    move.w  d5,SSP_ST_H(a3)
    /* Save PC */
    move.l  a2,d1
    sub.l   a4,d1
    lsr.l   #1,d1
    move.w  d1,SSP_PC_H(a3)
    /* Call read_handlers[r]() */
    movem.l d2-d3/d6-d7/a2-a5,-(sp)
    lsl.w   #2,d0                /* offset = r * 4 */
    lea     _read_handlers,a0
    move.l  (a0,d0.w),a0
    jsr     (a0)
    movem.l (sp)+,d2-d3/d6-d7/a2-a5
    /* Reload state (handlers may modify ssp state) */
    move.l  _ssp,a3
    move.l  SSP_A_V(a3),d4
    move.w  SSP_ST_H(a3),d5
    /* Reload PC in case handler changed it */
    moveq   #0,d1
    move.w  SSP_PC_H(a3),d1
    lea     (a4,d1.l*2),a2
    /* d0 already has return value */
    rts

/*
 * REG_WRITE - write d1.w to SSP register d0.w
 * For regs 1-3: direct inline access
 * For reg 0: ignore (constant register)
 * For regs 4+: call C handler
 * Clobbers: d0, d1, a0, a1
 */
.Lreg_write:
    cmp.w   #4,d0
    bge.s   .Lreg_write_handler
    tst.w   d0
    beq.s   .Lreg_write_done     /* reg 0 = constant, ignore write */
    /* regs 1-3: direct access */
    lsl.w   #2,d0
    move.w  d1,SSP_GR(a3,d0.w)
    rts
.Lreg_write_done:
    rts

.Lreg_write_handler:
    /* Save state */
    move.l  d4,SSP_A_V(a3)
    move.w  d5,SSP_ST_H(a3)
    move.l  a2,d2
    sub.l   a4,d2
    lsr.l   #1,d2
    move.w  d2,SSP_PC_H(a3)
    /* Call write_handlers[r](d) - d1 is the value */
    movem.l d2-d3/d6-d7/a2-a5,-(sp)
    move.l  d1,-(sp)             /* push value arg */
    lsl.w   #2,d0
    lea     _write_handlers,a0
    move.l  (a0,d0.w),a0
    jsr     (a0)
    addq.l  #4,sp                /* pop arg */
    movem.l (sp)+,d2-d3/d6-d7/a2-a5
    /* Reload state */
    move.l  _ssp,a3
    move.l  SSP_A_V(a3),d4
    move.w  SSP_ST_H(a3),d5
    moveq   #0,d1
    move.w  SSP_PC_H(a3),d1
    lea     (a4,d1.l*2),a2
    rts

/*
 * PTR1_READ - read from (ri) addressing mode
 * Input: d7 = op
 * Output: d0.w = value read
 * Calls C ptr1_read_ for complex cases
 * Clobbers: d0-d3, a0-a1
 */
.Lptr1_read:
    /* t = (op&3) | ((op>>6)&4) | ((op<<1)&0x18) */
    move.w  d7,d0
    and.w   #3,d0                /* op & 3 */
    move.w  d7,d1
    lsr.w   #6,d1
    and.w   #4,d1                /* (op>>6) & 4 */
    or.w    d1,d0
    move.w  d7,d1
    add.w   d1,d1                /* op << 1 */
    and.w   #0x18,d1             /* (op<<1) & 0x18 */
    or.w    d1,d0                /* d0 = t (0..31) */

    /* Fast path: mod=0 (t < 8) - simple RAM access */
    cmp.w   #8,d0
    bge.s   .Lptr1_read_complex

    /* mod=0: RAM[ptr] */
    cmp.w   #4,d0
    bge.s   .Lptr1_r0_ram1
    /* RAM0 access */
    cmp.w   #3,d0
    beq.s   .Lptr1_r0_ram0_fixed
    /* r0[t&3] */
    lea     SSP_PTR_R0(a3),a0
    moveq   #0,d1
    move.b  (a0,d0.w),d1         /* d1 = ssp->ptr.bank.r0[t&3] */
    add.w   d1,d1                /* word offset */
    move.w  SSP_RAM0(a3,d1.w),d0
    rts
.Lptr1_r0_ram0_fixed:
    move.w  SSP_RAM0(a3),d0      /* RAM0[0] */
    rts
.Lptr1_r0_ram1:
    cmp.w   #7,d0
    beq.s   .Lptr1_r0_ram1_fixed
    /* r1[t&3] */
    move.w  d0,d1
    and.w   #3,d1
    lea     SSP_PTR_R1(a3),a0
    moveq   #0,d2
    move.b  (a0,d1.w),d2
    add.w   d2,d2
    move.w  SSP_RAM1(a3,d2.w),d0
    rts
.Lptr1_r0_ram1_fixed:
    move.w  SSP_RAM1(a3),d0      /* RAM1[0] */
    rts

.Lptr1_read_complex:
    /* mod=1 (t 8-15): post-increment (no modulo) */
    cmp.w   #0x10,d0
    bge.s   .Lptr1_read_c_call

    /* Fast path for mod=1: RAM[ptr]++ */
    move.w  d0,d2
    and.w   #7,d2                /* base index (same as mod=0) */
    cmp.w   #4,d2
    bge.s   .Lptr1_r1_ram1
    cmp.w   #3,d2
    beq.s   .Lptr1_r1_ram0_f1
    /* RAM0[r0[t&3]]++ */
    lea     SSP_PTR_R0(a3),a0
    move.w  d2,d3
    and.w   #3,d3
    moveq   #0,d1
    move.b  (a0,d3.w),d1
    add.w   d1,d1
    move.w  SSP_RAM0(a3,d1.w),d0 /* value */
    lsr.w   #1,d1                /* back to byte index */
    addq.b  #1,d1
    move.b  d1,(a0,d3.w)         /* ptr++ */
    rts
.Lptr1_r1_ram0_f1:
    move.w  SSP_RAM0+2(a3),d0    /* RAM0[1] */
    rts
.Lptr1_r1_ram1:
    cmp.w   #7,d2
    beq.s   .Lptr1_r1_ram1_f1
    /* RAM1[r1[t&3]]++ */
    move.w  d2,d3
    and.w   #3,d3
    lea     SSP_PTR_R1(a3),a0
    moveq   #0,d1
    move.b  (a0,d3.w),d1
    add.w   d1,d1
    move.w  SSP_RAM1(a3,d1.w),d0
    lsr.w   #1,d1
    addq.b  #1,d1
    move.b  d1,(a0,d3.w)
    rts
.Lptr1_r1_ram1_f1:
    move.w  SSP_RAM1+2(a3),d0    /* RAM1[1] */
    rts

.Lptr1_read_c_call:
    /* Fall through to C for mod=2 (decrement with modulo) and mod=3 (increment with modulo) */
    move.l  d4,SSP_A_V(a3)
    move.w  d5,SSP_ST_H(a3)
    move.l  a2,d1
    sub.l   a4,d1
    lsr.l   #1,d1
    move.w  d1,SSP_PC_H(a3)

    movem.l d2-d3/d6-d7/a2-a5,-(sp)
    /* Call ptr1_read_(ri, isj2, modi3) */
    move.w  d7,d0
    and.l   #3,d0
    move.l  d0,-(sp)             /* ri = op & 3 */
    move.w  d7,d0
    lsr.w   #6,d0
    and.l   #4,d0
    move.l  d0,-(sp)             /* isj2 = (op>>6)&4 */
    move.w  d7,d0
    add.w   d0,d0
    and.l   #0x18,d0
    move.l  d0,-(sp)             /* modi3 = (op<<1)&0x18 */
    jsr     _ptr1_read_
    lea     12(sp),sp
    movem.l (sp)+,d2-d3/d6-d7/a2-a5
    /* Reload */
    move.l  _ssp,a3
    move.l  SSP_A_V(a3),d4
    move.w  SSP_ST_H(a3),d5
    moveq   #0,d1
    move.w  SSP_PC_H(a3),d1
    lea     (a4,d1.l*2),a2
    rts

/*
 * PTR1_WRITE - write d1.w to (ri) addressing mode
 * Input: d7 = op, d1.w = value
 * Calls C ptr1_write for all cases (less hot than read)
 */
.Lptr1_write:
    move.l  d4,SSP_A_V(a3)
    move.w  d5,SSP_ST_H(a3)
    move.l  a2,d0
    sub.l   a4,d0
    lsr.l   #1,d0
    move.w  d0,SSP_PC_H(a3)

    movem.l d2-d3/d6-d7/a2-a5,-(sp)
    move.l  d1,-(sp)             /* d = value */
    move.l  d7,-(sp)             /* op */
    jsr     _ptr1_write
    addq.l  #8,sp
    movem.l (sp)+,d2-d3/d6-d7/a2-a5
    move.l  _ssp,a3
    move.l  SSP_A_V(a3),d4
    move.w  SSP_ST_H(a3),d5
    moveq   #0,d1
    move.w  SSP_PC_H(a3),d1
    lea     (a4,d1.l*2),a2
    rts

/*
 * PTR2_READ - read from ((ri)) addressing mode
 * Delegates to C (relatively cold path)
 */
.Lptr2_read:
    move.l  d4,SSP_A_V(a3)
    move.w  d5,SSP_ST_H(a3)
    move.l  a2,d0
    sub.l   a4,d0
    lsr.l   #1,d0
    move.w  d0,SSP_PC_H(a3)

    movem.l d2-d3/d6-d7/a2-a5,-(sp)
    move.l  d7,-(sp)
    jsr     _ptr2_read
    addq.l  #4,sp
    movem.l (sp)+,d2-d3/d6-d7/a2-a5
    move.l  _ssp,a3
    move.l  SSP_A_V(a3),d4
    move.w  SSP_ST_H(a3),d5
    moveq   #0,d1
    move.w  SSP_PC_H(a3),d1
    lea     (a4,d1.l*2),a2
    rts

/*
 * COND_CHECK - evaluate condition from op
 * Input: d7 = op
 * Output: d0 = 1 if true, 0 if false
 */
.Lcond_check:
    move.w  d7,d0
    and.w   #0xF0,d0
    beq.s   .Lcond_always         /* 0x00 = always */
    cmp.w   #0x50,d0
    beq.s   .Lcond_z              /* 0x50 = Z check */
    cmp.w   #0x70,d0
    beq.s   .Lcond_n              /* 0x70 = N check */
    moveq   #0,d0                 /* unknown = false */
    rts
.Lcond_always:
    moveq   #1,d0
    rts
.Lcond_z:
    /* cond = !((rST ^ (op<<5)) & FLAG_Z) */
    move.w  d7,d1
    lsl.w   #5,d1
    eor.w   d5,d1                 /* rST ^ (op<<5) */
    and.w   #FLAG_Z,d1
    seq     d0                    /* d0 = 0xFF if Z match, 0 otherwise */
    and.l   #1,d0
    rts
.Lcond_n:
    /* cond = !((rST ^ (op<<7)) & FLAG_N) */
    move.w  d7,d1
    lsl.w   #7,d1
    eor.w   d5,d1
    and.w   #FLAG_N,d1
    seq     d0
    and.l   #1,d0
    rts


/* ======================================================================
 * OPCODE IMPLEMENTATIONS
 * ====================================================================== */

/* --- 0x00: ld d, s --- */
.Lop_00:
    tst.w   d7
    beq     .Lnext                /* NOP */
    /* Check for A <- P special case */
    cmp.w   #0x0037,d7           /* (SSP_A<<4)|SSP_P = (3<<4)|7 = 0x37 */
    bne.s   .Lop_00_general
    /* A <- P */
    READ_P
    move.l  d0,d4                 /* rA32 = rP.v */
    bra     .Lnext
.Lop_00_general:
    move.w  d7,d0
    and.w   #0x0F,d0              /* source reg */
    bsr     .Lreg_read
    move.w  d0,d1                 /* d1 = value */
    move.w  d7,d0
    lsr.w   #4,d0
    and.w   #0x0F,d0              /* dest reg */
    bsr     .Lreg_write
    bra     .Lnext

/* --- 0x01: ld d, (ri) --- */
.Lop_01:
    bsr     .Lptr1_read
    move.w  d0,d1
    move.w  d7,d0
    lsr.w   #4,d0
    and.w   #0x0F,d0
    bsr     .Lreg_write
    bra     .Lnext

/* --- 0x02: ld (ri), s --- */
.Lop_02:
    move.w  d7,d0
    lsr.w   #4,d0
    and.w   #0x0F,d0
    bsr     .Lreg_read
    move.w  d0,d1                 /* d1 = value from source */
    bsr     .Lptr1_write
    bra     .Lnext

/* --- 0x03: ld a, adr --- (also OP a, adr for 0x13,0x33,etc) */
.Lop_03:
    move.w  d7,d0
    and.w   #0x1FF,d0
    add.w   d0,d0                 /* word offset */
    move.w  SSP_RAM0(a3,d0.w),d0 /* ssp->mem.RAM[op & 0x1ff] */
    swap    d4                    /* get rA high word */
    move.w  d0,d4                 /* rA = value (ld A doesn't affect flags) */
    swap    d4
    bra     .Lnext

/* --- 0x04: ldi d, imm --- */
.Lop_04:
    move.w  (a2)+,d1              /* imm = *PC++ */
    move.w  d7,d0
    lsr.w   #4,d0
    and.w   #0x0F,d0
    bsr     .Lreg_write
    bra     .Lnext

/* --- 0x05: ld d, ((ri)) --- */
.Lop_05:
    bsr     .Lptr2_read
    move.w  d0,d1
    move.w  d7,d0
    lsr.w   #4,d0
    and.w   #0x0F,d0
    bsr     .Lreg_write
    bra     .Lnext

/* --- 0x06: ldi (ri), imm --- */
.Lop_06:
    move.w  (a2)+,d1              /* imm = *PC++ */
    bsr     .Lptr1_write
    bra     .Lnext

/* --- 0x07: ld adr, a --- */
.Lop_07:
    move.w  d7,d0
    and.w   #0x1FF,d0
    add.w   d0,d0
    move.l  d4,d1
    swap    d1                    /* d1 = rA (high word of rA32) */
    move.w  d1,SSP_RAM0(a3,d0.w)
    bra     .Lnext

/* --- 0x09: ld d, ri --- */
.Lop_09:
    /* rIJ index = ((op>>6)&4) | (op&3) */
    move.w  d7,d0
    lsr.w   #6,d0
    and.w   #4,d0
    move.w  d7,d1
    and.w   #3,d1
    or.w    d1,d0                 /* d0 = IJind */
    lea     SSP_PTR(a3),a0
    moveq   #0,d1
    move.b  (a0,d0.w),d1         /* d1 = rIJ[index] */
    move.w  d7,d0
    lsr.w   #4,d0
    and.w   #0x0F,d0
    bsr     .Lreg_write
    bra     .Lnext

/* --- 0x0a: ld ri, s --- */
.Lop_0a:
    move.w  d7,d0
    lsr.w   #4,d0
    and.w   #0x0F,d0
    bsr     .Lreg_read
    /* rIJ[((op>>6)&4)|(op&3)] = result */
    move.w  d7,d1
    lsr.w   #6,d1
    and.w   #4,d1
    move.w  d7,d2
    and.w   #3,d2
    or.w    d2,d1
    lea     SSP_PTR(a3),a0
    move.b  d0,(a0,d1.w)
    bra     .Lnext

/* --- 0x0c-0x0f: ldi ri, simm --- */
.Lop_0c:
    /* rIJ[(op>>8)&7] = op (low byte) */
    move.w  d7,d0
    lsr.w   #8,d0
    and.w   #7,d0
    lea     SSP_PTR(a3),a0
    move.b  d7,(a0,d0.w)         /* store low byte of op */
    bra     .Lnext

/* --- ALU operations: sub/cmp/add/and/or/eor a, <source> ---
 *
 * These all follow the same pattern, differing only in the ALU op.
 * Source variants: s (reg), (ri), adr, imm, ((ri)), ri, simm
 */

/* Helper: get value from reg source into d0.w, handle P and A special cases for 32-bit ops */
/* For OP_CHECK32 pattern - check if source is P or A for 32-bit operation */

/* --- 0x10: sub a, s --- */
.Lop_10:
    /* OP_CHECK32: if src==P, sub rP.v; if src==A, sub rA32 */
    move.w  d7,d0
    and.w   #0x0F,d0
    cmp.w   #7,d0                 /* SSP_P? */
    beq.s   .Lop_10_p
    cmp.w   #3,d0                 /* SSP_A? */
    beq.s   .Lop_10_a
    bsr     .Lreg_read
    swap    d0
    clr.w   d0                    /* d0 = value << 16 */
    sub.l   d0,d4
    UPD_LZVN
    bra     .Lnext
.Lop_10_p:
    READ_P
    sub.l   d0,d4
    UPD_LZVN
    bra     .Lnext
.Lop_10_a:
    sub.l   d4,d4                 /* rA32 -= rA32 = 0 */
    UPD_LZVN
    bra     .Lnext

/* --- 0x11: sub a, (ri) --- */
.Lop_11:
    bsr     .Lptr1_read
    swap    d0
    clr.w   d0
    sub.l   d0,d4
    UPD_LZVN
    bra     .Lnext

/* --- 0x13: sub a, adr --- */
.Lop_13:
    move.w  d7,d0
    and.w   #0x1FF,d0
    add.w   d0,d0
    moveq   #0,d1
    move.w  SSP_RAM0(a3,d0.w),d1
    swap    d1
    clr.w   d1
    sub.l   d1,d4
    UPD_LZVN
    bra     .Lnext

/* --- 0x14: sub a, imm --- */
.Lop_14:
    moveq   #0,d0
    move.w  (a2)+,d0
    swap    d0
    clr.w   d0
    sub.l   d0,d4
    UPD_LZVN
    bra     .Lnext

/* --- 0x15: sub a, ((ri)) --- */
.Lop_15:
    bsr     .Lptr2_read
    swap    d0
    clr.w   d0
    sub.l   d0,d4
    UPD_LZVN
    bra     .Lnext

/* --- 0x19: sub a, ri --- */
.Lop_19:
    move.w  d7,d0
    lsr.w   #6,d0
    and.w   #4,d0
    move.w  d7,d1
    and.w   #3,d1
    or.w    d1,d0
    lea     SSP_PTR(a3),a0
    moveq   #0,d0
    move.b  (a0,d0.w),d0
    swap    d0
    clr.w   d0
    sub.l   d0,d4
    UPD_LZVN
    bra     .Lnext

/* --- 0x1b: mpys --- */
.Lop_1b:
    READ_P
    sub.l   d0,d4                 /* rA32 -= rP.v */
    UPD_ACC_ZN
    /* rX = ptr1_read_(op&3, 0, (op<<1)&0x18) */
    /* rY = ptr1_read_((op>>4)&3, 4, (op>>3)&0x18) */
    /* Fall through to C for these complex reads */
    move.l  d4,SSP_A_V(a3)
    move.w  d5,SSP_ST_H(a3)
    move.l  a2,d0
    sub.l   a4,d0
    lsr.l   #1,d0
    move.w  d0,SSP_PC_H(a3)
    movem.l d2-d3/d6-d7/a2-a5,-(sp)
    /* X = ptr1_read_(op&3, 0, (op<<1)&0x18) */
    move.w  d7,d0
    add.w   d0,d0
    and.l   #0x18,d0
    move.l  d0,-(sp)
    clr.l   -(sp)
    move.w  d7,d0
    and.l   #3,d0
    move.l  d0,-(sp)
    jsr     _ptr1_read_
    lea     12(sp),sp
    move.l  _ssp,a0
    move.w  d0,SSP_X_H(a0)
    /* Y = ptr1_read_((op>>4)&3, 4, (op>>3)&0x18) */
    move.w  d7,d0
    lsr.w   #3,d0
    and.l   #0x18,d0
    move.l  d0,-(sp)
    move.l  #4,-(sp)
    move.w  d7,d0
    lsr.w   #4,d0
    and.l   #3,d0
    move.l  d0,-(sp)
    jsr     _ptr1_read_
    lea     12(sp),sp
    move.l  _ssp,a0
    move.w  d0,SSP_Y_H(a0)
    movem.l (sp)+,d2-d3/d6-d7/a2-a5
    move.l  _ssp,a3
    move.l  SSP_A_V(a3),d4
    move.w  SSP_ST_H(a3),d5
    moveq   #0,d1
    move.w  SSP_PC_H(a3),d1
    lea     (a4,d1.l*2),a2
    bra     .Lnext

/* --- 0x1c: sub simm --- */
.Lop_1c:
    moveq   #0,d0
    move.b  d7,d0                 /* op & 0xFF */
    swap    d0
    clr.w   d0
    sub.l   d0,d4
    UPD_LZVN
    bra     .Lnext

/* --- 0x24: call cond, addr --- */
.Lop_24:
    bsr     .Lcond_check
    tst.l   d0
    beq.s   .Lop_24_skip
    /* Call: push PC, set new PC */
    move.w  (a2)+,d0              /* new_PC = *PC++ */
    /* write_STACK(GET_PC()) */
    move.l  a2,d1
    sub.l   a4,d1
    lsr.l   #1,d1                 /* current PC (after fetch) */
    /* Push to stack */
    moveq   #0,d2
    move.w  SSP_STACK_H(a3),d2
    cmp.w   #6,d2
    blt.s   .Lop_24_pushok
    moveq   #0,d2
.Lop_24_pushok:
    lea     SSP_STACK(a3),a0
    add.w   d2,d2                 /* word offset */
    move.w  d1,(a0,d2.w)
    lsr.w   #1,d2
    addq.w  #1,d2
    move.w  d2,SSP_STACK_H(a3)
    /* SET_PC(new_PC) */
    moveq   #0,d1
    move.w  d0,d1
    lea     (a4,d1.l*2),a2
    subq.l  #1,d6                /* g_cycles-- for PC write */
    bra     .Lnext
.Lop_24_skip:
    addq.l  #2,a2                /* skip immediate */
    bra     .Lnext

/* --- 0x25: ld d, (a) --- */
.Lop_25:
    /* ((u16*)svp->iram_rom)[rA] */
    move.l  d4,d0
    swap    d0                    /* d0 = rA (high word) */
    and.l   #0xFFFF,d0
    add.l   d0,d0                /* byte offset */
    move.w  (a4,d0.l),d1         /* read from iram_rom */
    move.w  d7,d0
    lsr.w   #4,d0
    and.w   #0x0F,d0
    bsr     .Lreg_write
    bra     .Lnext

/* --- 0x26: bra cond, addr --- */
.Lop_26:
    bsr     .Lcond_check
    tst.l   d0
    beq.s   .Lop_26_skip
    move.w  (a2)+,d0
    moveq   #0,d1
    move.w  d0,d1
    lea     (a4,d1.l*2),a2
    subq.l  #1,d6                /* g_cycles-- */
    bra     .Lnext
.Lop_26_skip:
    addq.l  #2,a2
    bra     .Lnext

/* --- 0x30: cmp a, s --- */
.Lop_30:
    move.w  d7,d0
    and.w   #0x0F,d0
    cmp.w   #7,d0                 /* P? */
    beq.s   .Lop_30_p
    cmp.w   #3,d0                 /* A? */
    beq.s   .Lop_30_a
    bsr     .Lreg_read
    swap    d0
    clr.w   d0
    move.l  d4,d1
    sub.l   d0,d1                 /* t = rA32 - (x<<16) */
    /* Update flags from d1 (don't modify d4!) */
    and.w   #0x0FFF,d5
    tst.l   d1
    bne.s   .Lop_30_nz
    or.w    #FLAG_Z,d5
    bra     .Lnext
.Lop_30_nz:
    move.l  d1,d0
    swap    d0
    and.w   #FLAG_N,d0
    or.w    d0,d5
    bra     .Lnext
.Lop_30_p:
    READ_P
    move.l  d4,d1
    sub.l   d0,d1
    and.w   #0x0FFF,d5
    tst.l   d1
    bne.s   .Lop_30p_nz
    or.w    #FLAG_Z,d5
    bra     .Lnext
.Lop_30p_nz:
    move.l  d1,d0
    swap    d0
    and.w   #FLAG_N,d0
    or.w    d0,d5
    bra     .Lnext
.Lop_30_a:
    /* cmp A, A = always zero */
    and.w   #0x0FFF,d5
    or.w    #FLAG_Z,d5
    bra     .Lnext

/* --- 0x31: cmp a, (ri) --- */
.Lop_31:
    bsr     .Lptr1_read
    swap    d0
    clr.w   d0
    move.l  d4,d1
    sub.l   d0,d1
    and.w   #0x0FFF,d5
    tst.l   d1
    bne.s   .Lop_31_nz
    or.w    #FLAG_Z,d5
    bra     .Lnext
.Lop_31_nz:
    move.l  d1,d0
    swap    d0
    and.w   #FLAG_N,d0
    or.w    d0,d5
    bra     .Lnext

/* --- 0x33: cmp a, adr --- */
.Lop_33:
    move.w  d7,d0
    and.w   #0x1FF,d0
    add.w   d0,d0
    moveq   #0,d1
    move.w  SSP_RAM0(a3,d0.w),d1
    swap    d1
    clr.w   d1
    move.l  d4,d0
    sub.l   d1,d0
    and.w   #0x0FFF,d5
    tst.l   d0
    bne.s   .Lop_33_nz
    or.w    #FLAG_Z,d5
    bra     .Lnext
.Lop_33_nz:
    swap    d0
    and.w   #FLAG_N,d0
    or.w    d0,d5
    bra     .Lnext

/* --- 0x34: cmp a, imm --- */
.Lop_34:
    moveq   #0,d1
    move.w  (a2)+,d1
    swap    d1
    clr.w   d1
    move.l  d4,d0
    sub.l   d1,d0
    and.w   #0x0FFF,d5
    tst.l   d0
    bne.s   .Lop_34_nz
    or.w    #FLAG_Z,d5
    bra     .Lnext
.Lop_34_nz:
    swap    d0
    and.w   #FLAG_N,d0
    or.w    d0,d5
    bra     .Lnext

/* --- 0x35: cmp a, ((ri)) --- */
.Lop_35:
    bsr     .Lptr2_read
    swap    d0
    clr.w   d0
    move.l  d4,d1
    sub.l   d0,d1
    and.w   #0x0FFF,d5
    tst.l   d1
    bne.s   .Lop_35_nz
    or.w    #FLAG_Z,d5
    bra     .Lnext
.Lop_35_nz:
    move.l  d1,d0
    swap    d0
    and.w   #FLAG_N,d0
    or.w    d0,d5
    bra     .Lnext

/* --- 0x39: cmp a, ri --- */
.Lop_39:
    move.w  d7,d0
    lsr.w   #6,d0
    and.w   #4,d0
    move.w  d7,d1
    and.w   #3,d1
    or.w    d1,d0
    lea     SSP_PTR(a3),a0
    moveq   #0,d1
    move.b  (a0,d0.w),d1
    swap    d1
    clr.w   d1
    move.l  d4,d0
    sub.l   d1,d0
    and.w   #0x0FFF,d5
    tst.l   d0
    bne.s   .Lop_39_nz
    or.w    #FLAG_Z,d5
    bra     .Lnext
.Lop_39_nz:
    swap    d0
    and.w   #FLAG_N,d0
    or.w    d0,d5
    bra     .Lnext

/* --- 0x3c: cmp simm --- */
.Lop_3c:
    moveq   #0,d1
    move.b  d7,d1
    swap    d1
    clr.w   d1
    move.l  d4,d0
    sub.l   d1,d0
    and.w   #0x0FFF,d5
    tst.l   d0
    bne.s   .Lop_3c_nz
    or.w    #FLAG_Z,d5
    bra     .Lnext
.Lop_3c_nz:
    swap    d0
    and.w   #FLAG_N,d0
    or.w    d0,d5
    bra     .Lnext

/* --- 0x40: add a, s --- */
.Lop_40:
    move.w  d7,d0
    and.w   #0x0F,d0
    cmp.w   #7,d0
    beq.s   .Lop_40_p
    cmp.w   #3,d0
    beq.s   .Lop_40_a
    bsr     .Lreg_read
    swap    d0
    clr.w   d0
    add.l   d0,d4
    UPD_LZVN
    bra     .Lnext
.Lop_40_p:
    READ_P
    add.l   d0,d4
    UPD_LZVN
    bra     .Lnext
.Lop_40_a:
    add.l   d4,d4
    UPD_LZVN
    bra     .Lnext

/* --- 0x41: add a, (ri) --- */
.Lop_41:
    bsr     .Lptr1_read
    swap    d0
    clr.w   d0
    add.l   d0,d4
    UPD_LZVN
    bra     .Lnext

/* --- 0x43: add a, adr --- */
.Lop_43:
    move.w  d7,d0
    and.w   #0x1FF,d0
    add.w   d0,d0
    moveq   #0,d1
    move.w  SSP_RAM0(a3,d0.w),d1
    swap    d1
    clr.w   d1
    add.l   d1,d4
    UPD_LZVN
    bra     .Lnext

/* --- 0x44: add a, imm --- */
.Lop_44:
    moveq   #0,d0
    move.w  (a2)+,d0
    swap    d0
    clr.w   d0
    add.l   d0,d4
    UPD_LZVN
    bra     .Lnext

/* --- 0x45: add a, ((ri)) --- */
.Lop_45:
    bsr     .Lptr2_read
    swap    d0
    clr.w   d0
    add.l   d0,d4
    UPD_LZVN
    bra     .Lnext

/* --- 0x48: mod cond, op --- */
.Lop_48:
    bsr     .Lcond_check
    tst.l   d0
    beq     .Lnext
    move.w  d7,d0
    and.w   #7,d0
    cmp.w   #2,d0
    beq.s   .Lop_48_shr
    cmp.w   #3,d0
    beq.s   .Lop_48_shl
    cmp.w   #6,d0
    beq.s   .Lop_48_neg
    cmp.w   #7,d0
    beq.s   .Lop_48_abs
    bra     .Lnext
.Lop_48_shr:
    asr.l   #1,d4                 /* arithmetic shift right */
    UPD_ACC_ZN
    bra     .Lnext
.Lop_48_shl:
    asl.l   #1,d4
    UPD_ACC_ZN
    bra     .Lnext
.Lop_48_neg:
    neg.l   d4
    UPD_ACC_ZN
    bra     .Lnext
.Lop_48_abs:
    tst.l   d4
    bpl     .Lnext
    neg.l   d4
    UPD_ACC_ZN
    bra     .Lnext

/* --- 0x49: add a, ri --- */
.Lop_49:
    move.w  d7,d0
    lsr.w   #6,d0
    and.w   #4,d0
    move.w  d7,d1
    and.w   #3,d1
    or.w    d1,d0
    lea     SSP_PTR(a3),a0
    moveq   #0,d1
    move.b  (a0,d0.w),d1
    swap    d1
    clr.w   d1
    add.l   d1,d4
    UPD_LZVN
    bra     .Lnext

/* --- 0x4b: mpya --- */
.Lop_4b:
    READ_P
    add.l   d0,d4                 /* rA32 += rP.v */
    UPD_ACC_ZN
    /* Same complex ptr reads as mpys */
    move.l  d4,SSP_A_V(a3)
    move.w  d5,SSP_ST_H(a3)
    move.l  a2,d0
    sub.l   a4,d0
    lsr.l   #1,d0
    move.w  d0,SSP_PC_H(a3)
    movem.l d2-d3/d6-d7/a2-a5,-(sp)
    move.w  d7,d0
    add.w   d0,d0
    and.l   #0x18,d0
    move.l  d0,-(sp)
    clr.l   -(sp)
    move.w  d7,d0
    and.l   #3,d0
    move.l  d0,-(sp)
    jsr     _ptr1_read_
    lea     12(sp),sp
    move.l  _ssp,a0
    move.w  d0,SSP_X_H(a0)
    move.w  d7,d0
    lsr.w   #3,d0
    and.l   #0x18,d0
    move.l  d0,-(sp)
    move.l  #4,-(sp)
    move.w  d7,d0
    lsr.w   #4,d0
    and.l   #3,d0
    move.l  d0,-(sp)
    jsr     _ptr1_read_
    lea     12(sp),sp
    move.l  _ssp,a0
    move.w  d0,SSP_Y_H(a0)
    movem.l (sp)+,d2-d3/d6-d7/a2-a5
    move.l  _ssp,a3
    move.l  SSP_A_V(a3),d4
    move.w  SSP_ST_H(a3),d5
    moveq   #0,d1
    move.w  SSP_PC_H(a3),d1
    lea     (a4,d1.l*2),a2
    bra     .Lnext

/* --- 0x4c: add simm --- */
.Lop_4c:
    moveq   #0,d0
    move.b  d7,d0
    swap    d0
    clr.w   d0
    add.l   d0,d4
    UPD_LZVN
    bra     .Lnext

/* --- AND operations --- */
.Lop_50:
    move.w  d7,d0
    and.w   #0x0F,d0
    cmp.w   #7,d0
    beq.s   .Lop_50_p
    cmp.w   #3,d0
    beq     .Lnext               /* and A,A = A (no change, but update flags) */
    bsr     .Lreg_read
    swap    d0
    or.w    #0xFFFF,d0           /* low word = 0xFFFF for AND */
    and.l   d0,d4
    UPD_ACC_ZN
    bra     .Lnext
.Lop_50_p:
    READ_P
    and.l   d0,d4
    UPD_ACC_ZN
    bra     .Lnext

.Lop_51:
    bsr     .Lptr1_read
    swap    d0
    or.w    #0xFFFF,d0
    and.l   d0,d4
    UPD_ACC_ZN
    bra     .Lnext

.Lop_53:
    move.w  d7,d0
    and.w   #0x1FF,d0
    add.w   d0,d0
    move.w  SSP_RAM0(a3,d0.w),d0
    swap    d0
    or.w    #0xFFFF,d0
    and.l   d0,d4
    UPD_ACC_ZN
    bra     .Lnext

.Lop_54:
    move.w  (a2)+,d0
    swap    d0
    or.w    #0xFFFF,d0
    and.l   d0,d4
    UPD_ACC_ZN
    bra     .Lnext

.Lop_55:
    bsr     .Lptr2_read
    swap    d0
    or.w    #0xFFFF,d0
    and.l   d0,d4
    UPD_ACC_ZN
    bra     .Lnext

.Lop_59:
    move.w  d7,d0
    lsr.w   #6,d0
    and.w   #4,d0
    move.w  d7,d1
    and.w   #3,d1
    or.w    d1,d0
    lea     SSP_PTR(a3),a0
    moveq   #0,d1
    move.b  (a0,d0.w),d1
    swap    d1
    or.w    #0xFFFF,d1
    and.l   d1,d4
    UPD_ACC_ZN
    bra     .Lnext

/* --- 0x5b: mld --- */
.Lop_5b:
    moveq   #0,d4                 /* rA32 = 0 */
    and.w   #0x0FFF,d5           /* rST &= 0x0fff */
    /* Same ptr reads as mpys/mpya */
    move.l  d4,SSP_A_V(a3)
    move.w  d5,SSP_ST_H(a3)
    move.l  a2,d0
    sub.l   a4,d0
    lsr.l   #1,d0
    move.w  d0,SSP_PC_H(a3)
    movem.l d2-d3/d6-d7/a2-a5,-(sp)
    move.w  d7,d0
    add.w   d0,d0
    and.l   #0x18,d0
    move.l  d0,-(sp)
    clr.l   -(sp)
    move.w  d7,d0
    and.l   #3,d0
    move.l  d0,-(sp)
    jsr     _ptr1_read_
    lea     12(sp),sp
    move.l  _ssp,a0
    move.w  d0,SSP_X_H(a0)
    move.w  d7,d0
    lsr.w   #3,d0
    and.l   #0x18,d0
    move.l  d0,-(sp)
    move.l  #4,-(sp)
    move.w  d7,d0
    lsr.w   #4,d0
    and.l   #3,d0
    move.l  d0,-(sp)
    jsr     _ptr1_read_
    lea     12(sp),sp
    move.l  _ssp,a0
    move.w  d0,SSP_Y_H(a0)
    movem.l (sp)+,d2-d3/d6-d7/a2-a5
    move.l  _ssp,a3
    move.l  SSP_A_V(a3),d4
    move.w  SSP_ST_H(a3),d5
    moveq   #0,d1
    move.w  SSP_PC_H(a3),d1
    lea     (a4,d1.l*2),a2
    bra     .Lnext

.Lop_5c:
    moveq   #0,d0
    move.b  d7,d0
    swap    d0
    or.w    #0xFFFF,d0
    and.l   d0,d4
    UPD_ACC_ZN
    bra     .Lnext

/* --- OR operations --- */
.Lop_60:
    move.w  d7,d0
    and.w   #0x0F,d0
    cmp.w   #7,d0
    beq.s   .Lop_60_p
    cmp.w   #3,d0
    beq     .Lnext               /* or A,A = no change */
    bsr     .Lreg_read
    swap    d0
    clr.w   d0
    or.l    d0,d4
    UPD_ACC_ZN
    bra     .Lnext
.Lop_60_p:
    READ_P
    or.l    d0,d4
    UPD_ACC_ZN
    bra     .Lnext

.Lop_61:
    bsr     .Lptr1_read
    swap    d0
    clr.w   d0
    or.l    d0,d4
    UPD_ACC_ZN
    bra     .Lnext

.Lop_63:
    move.w  d7,d0
    and.w   #0x1FF,d0
    add.w   d0,d0
    moveq   #0,d1
    move.w  SSP_RAM0(a3,d0.w),d1
    swap    d1
    clr.w   d1
    or.l    d1,d4
    UPD_ACC_ZN
    bra     .Lnext

.Lop_64:
    moveq   #0,d0
    move.w  (a2)+,d0
    swap    d0
    clr.w   d0
    or.l    d0,d4
    UPD_ACC_ZN
    bra     .Lnext

.Lop_65:
    bsr     .Lptr2_read
    swap    d0
    clr.w   d0
    or.l    d0,d4
    UPD_ACC_ZN
    bra     .Lnext

.Lop_69:
    move.w  d7,d0
    lsr.w   #6,d0
    and.w   #4,d0
    move.w  d7,d1
    and.w   #3,d1
    or.w    d1,d0
    lea     SSP_PTR(a3),a0
    moveq   #0,d1
    move.b  (a0,d0.w),d1
    swap    d1
    clr.w   d1
    or.l    d1,d4
    UPD_ACC_ZN
    bra     .Lnext

.Lop_6c:
    moveq   #0,d0
    move.b  d7,d0
    swap    d0
    clr.w   d0
    or.l    d0,d4
    UPD_ACC_ZN
    bra     .Lnext

/* --- EOR operations --- */
.Lop_70:
    move.w  d7,d0
    and.w   #0x0F,d0
    cmp.w   #7,d0
    beq.s   .Lop_70_p
    cmp.w   #3,d0
    beq.s   .Lop_70_aa
    bsr     .Lreg_read
    swap    d0
    clr.w   d0
    eor.l   d0,d4
    UPD_ACC_ZN
    bra     .Lnext
.Lop_70_p:
    READ_P
    eor.l   d0,d4
    UPD_ACC_ZN
    bra     .Lnext
.Lop_70_aa:
    moveq   #0,d4                /* eor A,A = 0 */
    UPD_ACC_ZN
    bra     .Lnext

.Lop_71:
    bsr     .Lptr1_read
    swap    d0
    clr.w   d0
    eor.l   d0,d4
    UPD_ACC_ZN
    bra     .Lnext

.Lop_73:
    move.w  d7,d0
    and.w   #0x1FF,d0
    add.w   d0,d0
    moveq   #0,d1
    move.w  SSP_RAM0(a3,d0.w),d1
    swap    d1
    clr.w   d1
    eor.l   d1,d4
    UPD_ACC_ZN
    bra     .Lnext

.Lop_74:
    moveq   #0,d0
    move.w  (a2)+,d0
    swap    d0
    clr.w   d0
    eor.l   d0,d4
    UPD_ACC_ZN
    bra     .Lnext

.Lop_75:
    bsr     .Lptr2_read
    swap    d0
    clr.w   d0
    eor.l   d0,d4
    UPD_ACC_ZN
    bra     .Lnext

.Lop_79:
    move.w  d7,d0
    lsr.w   #6,d0
    and.w   #4,d0
    move.w  d7,d1
    and.w   #3,d1
    or.w    d1,d0
    lea     SSP_PTR(a3),a0
    moveq   #0,d1
    move.b  (a0,d0.w),d1
    swap    d1
    clr.w   d1
    eor.l   d1,d4
    UPD_ACC_ZN
    bra     .Lnext

.Lop_7c:
    moveq   #0,d0
    move.b  d7,d0
    swap    d0
    clr.w   d0
    eor.l   d0,d4
    UPD_ACC_ZN
    bra     .Lnext

/* --- Unhandled opcode --- */
.Lop_bad:
    bra     .Lnext                /* silently ignore */
