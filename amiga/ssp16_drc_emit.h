/*
 * ssp16_drc_emit.h - SSP1601 (SVP) to 68000 block translator
 *
 * Pure C with no Amiga or core dependencies, so it also compiles on the
 * development PC (amiga/drctest/) where every emitted instruction is
 * checked byte for byte against m68k-amigaos-as. Everything the generated
 * code needs to know about the running program (struct offsets, global
 * addresses, helper entry points) comes in through struct drc_env.
 *
 * The semantics follow ssp1601_run_c() in core/cart_hw/svp/ssp16.c and
 * the fast interpreter in amiga/ssp16_fast.h. Only 68000 instructions are
 * emitted.
 *
 * Register use inside a translated block (set up by drc_enter in
 * amiga/svp_drc_asm.s):
 *   d7 = accumulator rA32        d6 = status rST (upper word always 0)
 *   d5 = cycles left at block entry (only changed at exits / helper calls)
 *   d4 = IRAM generation the block was validated for
 *   a3 = ssp1601_t *             a4 = svp->iram_rom (program words)
 *   d0-d3, a0-a1 scratch; C helpers clobber d0-d1/a0-a1 only.
 * A block exits by storing the next DSP PC (word address) at
 * env->addr_next_pc, taking the executed cycles off d5 and returning (rts).
 */

#ifndef SSP16_DRC_EMIT_H
#define SSP16_DRC_EMIT_H

typedef unsigned int   drc_u32;
typedef unsigned short drc_u16;

struct drc_env
{
  /* ssp1601_t field offsets */
  int off_gr;           /* gr[0] (each entry 4 bytes, .byte.h at +0, .l at +2) */
  int off_ptr;          /* ptr.r[0..7] */
  int off_stack;        /* stack[0..5] */
  int off_emust;        /* emu_status (32-bit; low word at +2) */
  /* absolute addresses */
  drc_u32 base_addr;    /* svp->iram_rom */
  drc_u32 addr_PC;      /* static unsigned short *PC in ssp16.c */
  drc_u32 addr_g_cycles;
  drc_u32 addr_next_pc; /* unsigned short written at every exit */
  drc_u32 addr_gen;     /* IRAM generation counter (32-bit) */
  drc_u32 addr_hread;   /* u32 drc_hread(int r) */
  drc_u32 addr_hwrite;  /* void drc_hwrite(int r, u32 d) */
  drc_u32 addr_ptr2;    /* u32 ptr2_read(int op) */
  /* block chaining (drc_emit_dispatch) */
  drc_u32 addr_dispatch; /* every block exit jumps here, next PC in d0.w */
  drc_u32 addr_map;      /* struct drc_block *map[0x10000] */
  drc_u32 addr_runs;     /* unsigned long[7]: [0] chained block runs, [1..6]
                          * returns to C: cycles used up, idle wait, few cycles
                          * left, not translated, IRAM recheck, first segment */
  int off_b_code, off_b_gen, off_b_nins;  /* struct drc_block fields */
  int min_cycles;        /* below this many cycles C finishes the run */
  int no_chain;          /* verify builds: every exit returns to C */
  /* inline PM handlers */
  int off_pmac;          /* ssp1601_t pmac[2][6] */
  drc_u32 addr_dram;     /* svp->dram */
  drc_u32 addr_rom;      /* cart.rom (array in the cart struct) */
  drc_u32 addr_inctab;   /* long[32]: get_inc() for (mode >> 11) & 31 */
  drc_u32 addr_vlog;     /* verify builds: void (u32 off, u32 old, u32 val), else 0 */
  /* shared slow paths (drc_emit_stubs) */
  drc_u32 addr_slow_read, addr_slow_write;
  drc_u32 addr_flag;     /* byte: leave the block after this instruction */
  drc_u32 addr_masktab;  /* word[65536]: overwrite mask (non-zero nibbles of d) */
  drc_u32 addr_pmstat;   /* profile builds: unsigned long[10] PM path counts, else 0:
                          * read ROM, read DRAM, read raw, read C, write DRAM,
                          * write cell, write raw, write C, write IRAM, write
                          * ignored */
  drc_u32 addr_vlog_iram; /* verify builds: void (u32 off, u32 old, u32 val), else 0 */
  drc_u32 addr_exitstat; /* profile builds: unsigned long[10] block exit counts, else 0:
                          * end, bra fwd, bra back, call, loop inside, not taken,
                          * PC write, too few cycles, flag exit, - */
};

struct drc_out
{
  unsigned char *buf;
  drc_u32 addr;          /* absolute address of buf[0] when the code runs */
  int pos, cap;
  int overflow;
#ifdef DRC_TEXT
  /* host test: the intended assembly, one line per instruction, with the
   * byte offset it starts at; branch targets as labels */
  void (*text)(int pos, const char *line);
  void (*label)(int pos, int id);
  int next_label;
#endif
};

#define DRC_MAX_INS 24          /* DSP instructions per block */
#define DRC_SKIP_MAX 32         /* words: a forward conditional bra within this stays inside */

/* ------------------------------------------------------------------ */
/* raw emission */

static void drc_w(struct drc_out *o, drc_u32 w)
{
  if (o->pos + 2 > o->cap) { o->overflow = 1; return; }
  o->buf[o->pos] = (unsigned char)(w >> 8);
  o->buf[o->pos + 1] = (unsigned char)w;
  o->pos += 2;
}

static void drc_l(struct drc_out *o, drc_u32 l)
{
  drc_w(o, l >> 16);
  drc_w(o, l & 0xffff);
}

#ifdef DRC_TEXT
#include <stdio.h>
#define T(...) do { char t_[96]; sprintf(t_, __VA_ARGS__); o->text(o->pos, t_); } while (0)
#else
#define T(...) ((void)0)
#endif

/* (d16,An) effective address: mode 5, or (An) (mode 2, no extension word)
 * when the displacement is 0, as the assembler encodes it */
#define EA_D16(d16, an) ((d16) ? (0x28 | (an)) : (0x10 | (an)))
#define EXT_D16(o, d16) do { if (d16) drc_w((o), (d16) & 0xffff); } while (0)
/* same as a destination field (register in bits 11-9, mode in 8-6) */
#define EAD_D16(d16, an) ((d16) ? (0x140 | ((an) << 9)) : (0x080 | ((an) << 9)))

/* ------------------------------------------------------------------ */
/* instructions (68000; encodings from the M68000 PRM, checked by the
 * host test against m68k-amigaos-as) */

static void i_move_w_d16_dn(struct drc_out *o, int d16, int an, int dn)
{ T("move.w %d(%%a%d),%%d%d", d16, an, dn); drc_w(o, 0x3000 | (dn << 9) | EA_D16(d16, an)); EXT_D16(o, d16); }
static void i_move_w_dn_d16(struct drc_out *o, int dn, int d16, int an)
{ T("move.w %%d%d,%d(%%a%d)", dn, d16, an); drc_w(o, 0x3000 | EAD_D16(d16, an) | dn); EXT_D16(o, d16); }
static void i_move_l_d16_dn(struct drc_out *o, int d16, int an, int dn)
{ T("move.l %d(%%a%d),%%d%d", d16, an, dn); drc_w(o, 0x2000 | (dn << 9) | EA_D16(d16, an)); EXT_D16(o, d16); }
static void i_move_l_dn_d16(struct drc_out *o, int dn, int d16, int an)
{ T("move.l %%d%d,%d(%%a%d)", dn, d16, an); drc_w(o, 0x2000 | EAD_D16(d16, an) | dn); EXT_D16(o, d16); }
static void i_move_b_d16_dn(struct drc_out *o, int d16, int an, int dn)
{ T("move.b %d(%%a%d),%%d%d", d16, an, dn); drc_w(o, 0x1000 | (dn << 9) | EA_D16(d16, an)); EXT_D16(o, d16); }
static void i_move_b_dn_d16(struct drc_out *o, int dn, int d16, int an)
{ T("move.b %%d%d,%d(%%a%d)", dn, d16, an); drc_w(o, 0x1000 | EAD_D16(d16, an) | dn); EXT_D16(o, d16); }
static void i_move_w_imm_d16(struct drc_out *o, int imm, int d16, int an)
{ T("move.w #%d,%d(%%a%d)", imm & 0xffff, d16, an); drc_w(o, 0x303c | EAD_D16(d16, an)); drc_w(o, imm & 0xffff); EXT_D16(o, d16); }
static void i_move_b_imm_d16(struct drc_out *o, int imm, int d16, int an)
{ T("move.b #%d,%d(%%a%d)", imm & 0xff, d16, an); drc_w(o, 0x103c | EAD_D16(d16, an)); drc_w(o, imm & 0xff); EXT_D16(o, d16); }
/* (0,An,Xn.W) and (0,An,Xn.L) brief extension: register in bits 14-12, bit 11 = .L */
static void i_move_w_idx_dn(struct drc_out *o, int an, int xn, int dn)
{ T("move.w 0(%%a%d,%%d%d.w),%%d%d", an, xn, dn); drc_w(o, 0x3030 | (dn << 9) | an); drc_w(o, xn << 12); }
static void i_move_w_idxl_dn(struct drc_out *o, int an, int xn, int dn)
{ T("move.w 0(%%a%d,%%d%d.l),%%d%d", an, xn, dn); drc_w(o, 0x3030 | (dn << 9) | an); drc_w(o, (xn << 12) | 0x0800); }
static void i_move_w_dn_idx(struct drc_out *o, int dn, int an, int xn)
{ T("move.w %%d%d,0(%%a%d,%%d%d.w)", dn, an, xn); drc_w(o, 0x3180 | (an << 9) | dn); drc_w(o, xn << 12); }
static void i_move_l_dd(struct drc_out *o, int s, int d)
{ T("move.l %%d%d,%%d%d", s, d); drc_w(o, 0x2000 | (d << 9) | s); }
static void i_move_w_dd(struct drc_out *o, int s, int d)
{ T("move.w %%d%d,%%d%d", s, d); drc_w(o, 0x3000 | (d << 9) | s); }
static void i_move_w_imm_dn(struct drc_out *o, int imm, int dn)
{ T("move.w #%d,%%d%d", imm & 0xffff, dn); drc_w(o, 0x303c | (dn << 9)); drc_w(o, imm & 0xffff); }
static void i_moveq(struct drc_out *o, int v, int dn)
{ T("moveq #%d,%%d%d", v, dn); drc_w(o, 0x7000 | (dn << 9) | (v & 0xff)); }
static void i_swap(struct drc_out *o, int dn)
{ T("swap %%d%d", dn); drc_w(o, 0x4840 | dn); }
static void i_clr_w(struct drc_out *o, int dn)
{ T("clr.w %%d%d", dn); drc_w(o, 0x4240 | dn); }
static void i_tst_l(struct drc_out *o, int dn)
{ T("tst.l %%d%d", dn); drc_w(o, 0x4a80 | dn); }
static void i_not_w(struct drc_out *o, int dn)
{ T("not.w %%d%d", dn); drc_w(o, 0x4640 | dn); }
static void i_neg_l(struct drc_out *o, int dn)
{ T("neg.l %%d%d", dn); drc_w(o, 0x4480 | dn); }
static void i_asr_l_1(struct drc_out *o, int dn)
{ T("asr.l #1,%%d%d", dn); drc_w(o, 0xe280 | dn); }
static void i_lsl_w_reg(struct drc_out *o, int cnt, int dn)
{ T("lsl.w %%d%d,%%d%d", cnt, dn); drc_w(o, 0xe168 | (cnt << 9) | dn); }
static void i_muls_w_d16(struct drc_out *o, int d16, int an, int dn)
{ T("muls.w %d(%%a%d),%%d%d", d16, an, dn); drc_w(o, 0xc1c0 | (dn << 9) | EA_D16(d16, an)); EXT_D16(o, d16); }
/* immediate ALU ops (ANDI/ORI/EORI/SUBI/ADDI/CMPI) */
static void i_andi_w(struct drc_out *o, int imm, int dn)
{ T("andi.w #%d,%%d%d", imm & 0xffff, dn); drc_w(o, 0x0240 | dn); drc_w(o, imm & 0xffff); }
static void i_ori_w(struct drc_out *o, int imm, int dn)
{ T("ori.w #%d,%%d%d", imm & 0xffff, dn); drc_w(o, 0x0040 | dn); drc_w(o, imm & 0xffff); }
static void i_addi_w(struct drc_out *o, int imm, int dn)
{ T("addi.w #%d,%%d%d", imm & 0xffff, dn); drc_w(o, 0x0640 | dn); drc_w(o, imm & 0xffff); }
static void i_cmpi_w(struct drc_out *o, int imm, int dn)
{ T("cmpi.w #%d,%%d%d", imm & 0xffff, dn); drc_w(o, 0x0c40 | dn); drc_w(o, imm & 0xffff); }
static void i_andi_l(struct drc_out *o, drc_u32 imm, int dn)
{ T("andi.l #%u,%%d%d", imm, dn); drc_w(o, 0x0280 | dn); drc_l(o, imm); }
static void i_ori_l(struct drc_out *o, drc_u32 imm, int dn)
{ T("ori.l #%u,%%d%d", imm, dn); drc_w(o, 0x0080 | dn); drc_l(o, imm); }
static void i_eori_l(struct drc_out *o, drc_u32 imm, int dn)
{ T("eori.l #%u,%%d%d", imm, dn); drc_w(o, 0x0a80 | dn); drc_l(o, imm); }
static void i_subi_l(struct drc_out *o, drc_u32 imm, int dn)
{ T("subi.l #%u,%%d%d", imm, dn); drc_w(o, 0x0480 | dn); drc_l(o, imm); }
static void i_addi_l(struct drc_out *o, drc_u32 imm, int dn)
{ T("addi.l #%u,%%d%d", imm, dn); drc_w(o, 0x0680 | dn); drc_l(o, imm); }
static void i_andi_l_d16(struct drc_out *o, drc_u32 imm, int d16, int an)
{ T("andi.l #%u,%d(%%a%d)", imm, d16, an); drc_w(o, 0x0280 | EA_D16(d16, an)); drc_l(o, imm); EXT_D16(o, d16); }
/* register ALU ops */
static void i_sub_l_dd(struct drc_out *o, int s, int d)
{ T("sub.l %%d%d,%%d%d", s, d); drc_w(o, 0x9080 | (d << 9) | s); }
static void i_add_l_dd(struct drc_out *o, int s, int d)
{ T("add.l %%d%d,%%d%d", s, d); drc_w(o, 0xd080 | (d << 9) | s); }
static void i_and_l_dd(struct drc_out *o, int s, int d)
{ T("and.l %%d%d,%%d%d", s, d); drc_w(o, 0xc080 | (d << 9) | s); }
static void i_or_l_dd(struct drc_out *o, int s, int d)
{ T("or.l %%d%d,%%d%d", s, d); drc_w(o, 0x8080 | (d << 9) | s); }
static void i_eor_l_dd(struct drc_out *o, int s, int d)
{ T("eor.l %%d%d,%%d%d", s, d); drc_w(o, 0xb180 | (s << 9) | d); }
static void i_add_w_dd(struct drc_out *o, int s, int d)
{ T("add.w %%d%d,%%d%d", s, d); drc_w(o, 0xd040 | (d << 9) | s); }
static void i_and_w_dd(struct drc_out *o, int s, int d)
{ T("and.w %%d%d,%%d%d", s, d); drc_w(o, 0xc040 | (d << 9) | s); }
static void i_or_w_dd(struct drc_out *o, int s, int d)
{ T("or.w %%d%d,%%d%d", s, d); drc_w(o, 0x8040 | (d << 9) | s); }
/* quick */
static void i_addq_l(struct drc_out *o, int k, int dn)
{ T("addq.l #%d,%%d%d", k, dn); drc_w(o, 0x5080 | ((k & 7) << 9) | dn); }
static void i_subq_l(struct drc_out *o, int k, int dn)
{ T("subq.l #%d,%%d%d", k, dn); drc_w(o, 0x5180 | ((k & 7) << 9) | dn); }
static void i_addq_w(struct drc_out *o, int k, int dn)
{ T("addq.w #%d,%%d%d", k, dn); drc_w(o, 0x5040 | ((k & 7) << 9) | dn); }
static void i_subq_w(struct drc_out *o, int k, int dn)
{ T("subq.w #%d,%%d%d", k, dn); drc_w(o, 0x5140 | ((k & 7) << 9) | dn); }
static void i_addq_b_d16(struct drc_out *o, int k, int d16, int an)
{ T("addq.b #%d,%d(%%a%d)", k, d16, an); drc_w(o, 0x5000 | ((k & 7) << 9) | EA_D16(d16, an)); EXT_D16(o, d16); }
static void i_subq_b_d16(struct drc_out *o, int k, int d16, int an)
{ T("subq.b #%d,%d(%%a%d)", k, d16, an); drc_w(o, 0x5100 | ((k & 7) << 9) | EA_D16(d16, an)); EXT_D16(o, d16); }
static void i_btst_imm(struct drc_out *o, int bit, int dn)
{ T("btst #%d,%%d%d", bit, dn); drc_w(o, 0x0800 | dn); drc_w(o, bit); }
/* absolute long */
static void i_move_l_imm_abs(struct drc_out *o, drc_u32 imm, drc_u32 abs)
{ T("move.l #%u,(%u).l", imm, abs); drc_w(o, 0x23fc); drc_l(o, imm); drc_l(o, abs); }
static void i_move_w_imm_abs(struct drc_out *o, int imm, drc_u32 abs)
{ T("move.w #%d,(%u).l", imm & 0xffff, abs); drc_w(o, 0x33fc); drc_w(o, imm & 0xffff); drc_l(o, abs); }
static void i_move_w_dn_abs(struct drc_out *o, int dn, drc_u32 abs)
{ T("move.w %%d%d,(%u).l", dn, abs); drc_w(o, 0x33c0 | dn); drc_l(o, abs); }
static void i_move_l_dn_abs(struct drc_out *o, int dn, drc_u32 abs)
{ T("move.l %%d%d,(%u).l", dn, abs); drc_w(o, 0x23c0 | dn); drc_l(o, abs); }
static void i_move_l_abs_dn(struct drc_out *o, drc_u32 abs, int dn)
{ T("move.l (%u).l,%%d%d", abs, dn); drc_w(o, 0x2039 | (dn << 9)); drc_l(o, abs); }
static void i_cmp_l_abs_dn(struct drc_out *o, drc_u32 abs, int dn)
{ T("cmp.l (%u).l,%%d%d", abs, dn); drc_w(o, 0xb0b9 | (dn << 9)); drc_l(o, abs); }
/* stack / control */
static void i_push_dn(struct drc_out *o, int dn)
{ T("move.l %%d%d,-(%%sp)", dn); drc_w(o, 0x2f00 | dn); }
static void i_push_imm(struct drc_out *o, drc_u32 imm)
{ T("move.l #%u,-(%%sp)", imm); drc_w(o, 0x2f3c); drc_l(o, imm); }
static void i_addq_l_sp(struct drc_out *o, int k)
{ T("addq.l #%d,%%sp", k); drc_w(o, 0x508f | ((k & 7) << 9)); }
static void i_jsr_abs(struct drc_out *o, drc_u32 abs)
{ T("jsr (%u).l", abs); drc_w(o, 0x4eb9); drc_l(o, abs); }
static void i_rts(struct drc_out *o)
{ T("rts"); drc_w(o, 0x4e75); }
static void i_jmp_abs(struct drc_out *o, drc_u32 abs)
{ T("jmp (%u).l", abs); drc_w(o, 0x4ef9); drc_l(o, abs); }
static void i_jmp_a0(struct drc_out *o)
{ T("jmp (%%a0)"); drc_w(o, 0x4ed0); }
static void i_movea_l_imm_a0(struct drc_out *o, drc_u32 imm)
{ T("movea.l #%u,%%a0", imm); drc_w(o, 0x207c); drc_l(o, imm); }
/* movea.l 0(a0,d1.l),a0 */
static void i_movea_l_idx_a0(struct drc_out *o)
{ T("movea.l 0(%%a0,%%d1.l),%%a0"); drc_w(o, 0x2070); drc_w(o, (1 << 12) | 0x0800); }
static void i_movea_l_d16a0_a0(struct drc_out *o, int d16)
{ T("movea.l %d(%%a0),%%a0", d16); drc_w(o, 0x2068); drc_w(o, d16 & 0xffff); }
static void i_move_l_a0_dn(struct drc_out *o, int dn)
{ T("move.l %%a0,%%d%d", dn); drc_w(o, 0x2008 | (dn << 9)); }
static void i_move_w_d16a0_dn(struct drc_out *o, int d16, int dn)
{ T("move.w %d(%%a0),%%d%d", d16, dn); drc_w(o, 0x3028 | (dn << 9)); drc_w(o, d16 & 0xffff); }
static void i_cmp_l_d16a0_dn(struct drc_out *o, int d16, int dn)
{ T("cmp.l %d(%%a0),%%d%d", d16, dn); drc_w(o, 0xb0a8 | (dn << 9)); drc_w(o, d16 & 0xffff); }
static void i_cmp_l_dd(struct drc_out *o, int s, int d)
{ T("cmp.l %%d%d,%%d%d", s, d); drc_w(o, 0xb080 | (d << 9) | s); }
static void i_addq_l_abs(struct drc_out *o, int k, drc_u32 abs)
{ T("addq.l #%d,(%u).l", k, abs); drc_w(o, 0x50b9 | ((k & 7) << 9)); drc_l(o, abs); }

/* used by the inline PM handlers */
static void i_lea_d16_a1(struct drc_out *o, int d16, int an)
{ T("lea %d(%%a%d),%%a1", d16, an); drc_w(o, 0x43c0 | EA_D16(d16, an)); EXT_D16(o, d16); }
static void i_lea_d16_sp_sp(struct drc_out *o, int d16)
{ T("lea %d(%%sp),%%sp", d16); drc_w(o, 0x4fef); drc_w(o, d16 & 0xffff); }
static void i_move_l_ind_a1_dn(struct drc_out *o, int dn)
{ T("move.l (%%a1),%%d%d", dn); drc_w(o, 0x2011 | (dn << 9)); }
static void i_addq_l_ind_a1(struct drc_out *o, int k)
{ T("addq.l #%d,(%%a1)", k); drc_w(o, 0x5091 | ((k & 7) << 9)); }
static void i_add_l_dn_ind_a1(struct drc_out *o, int dn)
{ T("add.l %%d%d,(%%a1)", dn); drc_w(o, 0xd191 | (dn << 9)); }
static void i_move_l_ind_a1_d16(struct drc_out *o, int d16, int an)
{ T("move.l (%%a1),%d(%%a%d)", d16, an); drc_w(o, 0x2000 | EAD_D16(d16, an) | 0x11); EXT_D16(o, d16); }
static void i_movea_l_abs_a0(struct drc_out *o, drc_u32 abs)
{ T("movea.l (%u).l,%%a0", abs); drc_w(o, 0x2079); drc_l(o, abs); }
static void i_move_w_dn_idxl(struct drc_out *o, int dn, int an, int xn)
{ T("move.w %%d%d,0(%%a%d,%%d%d.l)", dn, an, xn); drc_w(o, 0x3180 | (an << 9) | dn); drc_w(o, (xn << 12) | 0x0800); }
static void i_move_l_idxw_dn(struct drc_out *o, int an, int xn, int dn)
{ T("move.l 0(%%a%d,%%d%d.w),%%d%d", an, xn, dn); drc_w(o, 0x2030 | (dn << 9) | an); drc_w(o, xn << 12); }
static void i_btst_imm_d16(struct drc_out *o, int bit, int d16, int an)
{ T("btst #%d,%d(%%a%d)", bit, d16, an); drc_w(o, 0x0800 | EA_D16(d16, an)); drc_w(o, bit); EXT_D16(o, d16); }
static void i_ori_b_d16(struct drc_out *o, int imm, int d16, int an)
{ T("ori.b #%d,%d(%%a%d)", imm & 0xff, d16, an); drc_w(o, 0x0000 | EA_D16(d16, an)); drc_w(o, imm & 0xff); EXT_D16(o, d16); }
static void i_andi_b_d16(struct drc_out *o, int imm, int d16, int an)
{ T("andi.b #%d,%d(%%a%d)", imm & 0xff, d16, an); drc_w(o, 0x0200 | EA_D16(d16, an)); drc_w(o, imm & 0xff); EXT_D16(o, d16); }
static void i_ori_w_d16(struct drc_out *o, int imm, int d16, int an)
{ T("ori.w #%d,%d(%%a%d)", imm & 0xffff, d16, an); drc_w(o, 0x0040 | EA_D16(d16, an)); drc_w(o, imm & 0xffff); EXT_D16(o, d16); }
static void i_andi_w_d16(struct drc_out *o, int imm, int d16, int an)
{ T("andi.w #%d,%d(%%a%d)", imm & 0xffff, d16, an); drc_w(o, 0x0240 | EA_D16(d16, an)); drc_w(o, imm & 0xffff); EXT_D16(o, d16); }
static void i_lsl_w_imm(struct drc_out *o, int cnt, int dn)
{ T("lsl.w #%d,%%d%d", cnt, dn); drc_w(o, 0xe148 | ((cnt & 7) << 9) | dn); }
static void i_lsr_w_imm(struct drc_out *o, int cnt, int dn)
{ T("lsr.w #%d,%%d%d", cnt, dn); drc_w(o, 0xe048 | ((cnt & 7) << 9) | dn); }
static void i_tst_w(struct drc_out *o, int dn)
{ T("tst.w %%d%d", dn); drc_w(o, 0x4a40 | dn); }
static void i_adda_l_dn_a0(struct drc_out *o, int dn)
{ T("adda.l %%d%d,%%a0", dn); drc_w(o, 0xd1c0 | dn); }
static void i_movea_l_imm_a2(struct drc_out *o, drc_u32 imm)
{ T("movea.l #%u,%%a2", imm); drc_w(o, 0x247c); drc_l(o, imm); }
static void i_move_w_ind_a0_dn(struct drc_out *o, int dn)
{ T("move.w (%%a0),%%d%d", dn); drc_w(o, 0x3010 | (dn << 9)); }
static void i_move_w_dn_ind_a0(struct drc_out *o, int dn)
{ T("move.w %%d%d,(%%a0)", dn); drc_w(o, 0x3080 | dn); }
static void i_eor_w_dd(struct drc_out *o, int s, int d)
{ T("eor.w %%d%d,%%d%d", s, d); drc_w(o, 0xb140 | (s << 9) | d); }
/* continue at an absolute address: bra.w when it is within reach (Emu68
 * inlines a BRA of up to 8191 bytes instead of ending its translation
 * unit), else jmp abs.l */
static void i_goto_abs(struct drc_out *o, drc_u32 target)
{
  long disp = (long)target - (long)(o->addr + (drc_u32)o->pos + 2);
  if (disp >= -32768 && disp <= 32767)
  {
    T(".word 24576,%ld", disp & 0xffff);
    drc_w(o, 0x6000);
    drc_w(o, (drc_u32)disp & 0xffff);
  }
  else
  {
    T("jmp (%u).l", target);
    drc_w(o, 0x4ef9);
    drc_l(o, target);
  }
}
static void i_seq(struct drc_out *o, int dn)
{ T("seq %%d%d", dn); drc_w(o, 0x57c0 | dn); }
static void i_smi(struct drc_out *o, int dn)
{ T("smi %%d%d", dn); drc_w(o, 0x5bc0 | dn); }
static void i_ext_w(struct drc_out *o, int dn)
{ T("ext.w %%d%d", dn); drc_w(o, 0x4880 | dn); }
static void i_subx_l_dd(struct drc_out *o, int s, int d)
{ T("subx.l %%d%d,%%d%d", s, d); drc_w(o, 0x9180 | (d << 9) | s); }
static void i_or_w_a2idxl_dn(struct drc_out *o, int xn, int dn)
{ T("or.w 0(%%a2,%%d%d.l),%%d%d", xn, dn); drc_w(o, 0x8072 | (dn << 9)); drc_w(o, (xn << 12) | 0x0800); }
static void i_move_l_d16_d16(struct drc_out *o, int s16, int d16, int an)
{ T("move.l %d(%%a%d),%d(%%a%d)", s16, an, d16, an); drc_w(o, 0x2000 | EAD_D16(d16, an) | EA_D16(s16, an)); EXT_D16(o, s16); EXT_D16(o, d16); }
static void i_ext_l(struct drc_out *o, int dn)
{ T("ext.l %%d%d", dn); drc_w(o, 0x48c0 | dn); }
static void i_or_b_dd(struct drc_out *o, int s, int d)
{ T("or.b %%d%d,%%d%d", s, d); drc_w(o, 0x8000 | (d << 9) | s); }
static void i_tst_b_abs(struct drc_out *o, drc_u32 abs)
{ T("tst.b (%u).l", abs); drc_w(o, 0x4a39); drc_l(o, abs); }
static void i_st_abs(struct drc_out *o, drc_u32 abs)
{ T("st (%u).l", abs); drc_w(o, 0x50f9); drc_l(o, abs); }
static void i_sf_abs(struct drc_out *o, drc_u32 abs)
{ T("sf (%u).l", abs); drc_w(o, 0x51f9); drc_l(o, abs); }
static void i_movea_l_imm_a1(struct drc_out *o, drc_u32 imm)
{ T("movea.l #%u,%%a1", imm); drc_w(o, 0x227c); drc_l(o, imm); }
static void i_move_l_a1_abs(struct drc_out *o, drc_u32 abs)
{ T("move.l %%a1,(%u).l", abs); drc_w(o, 0x23c9); drc_l(o, abs); }
static void i_move_l_sp_postinc_dn(struct drc_out *o, int dn)
{ T("move.l (%%sp)+,%%d%d", dn); drc_w(o, 0x201f | (dn << 9)); }

/* Bcc.W with a 16-bit displacement patched later. Condition numbers from
 * the PRM: 0 = BRA, 6 = NE, 7 = EQ, 10 = PL, 11 = MI, 5 = CS. */
#define CC_T  0
#define CC_CS 5
#define CC_NE 6
#define CC_EQ 7
#define CC_PL 10

struct drc_fix { int pos; int label; };

static struct drc_fix i_bcc_w(struct drc_out *o, int cc)
{
  static const char *const nm[16] = { "bra", "bsr", "bhi", "bls", "bcc", "bcs", "bne", "beq",
                                      "bvc", "bvs", "bpl", "bmi", "bge", "blt", "bgt", "ble" };
  struct drc_fix f;
  f.pos = o->pos;
#ifdef DRC_TEXT
  f.label = o->next_label++;
  T("%s.w .L%d", nm[cc], f.label);
#else
  (void)nm;
  f.label = 0;
#endif
  drc_w(o, 0x6000 | (cc << 8));
  drc_w(o, 0);
  return f;
}

/* a branch target in already emitted code */
static struct drc_fix drc_mark(struct drc_out *o)
{
  struct drc_fix f;
  f.pos = o->pos;
#ifdef DRC_TEXT
  f.label = o->next_label++;
  o->label(o->pos, f.label);
#else
  f.label = 0;
#endif
  return f;
}

/* bra.w back to a mark */
static void i_bra_mark(struct drc_out *o, struct drc_fix m)
{
  int disp = m.pos - (o->pos + 2);
  T("bra.w .L%d", m.label);
  drc_w(o, 0x6000);
  drc_w(o, (drc_u32)disp & 0xffff);
}

static void drc_here(struct drc_out *o, struct drc_fix f)
{
  int disp = o->pos - (f.pos + 2);
  if (!o->overflow && f.pos + 4 <= o->cap)
  {
    o->buf[f.pos + 2] = (unsigned char)(disp >> 8);
    o->buf[f.pos + 3] = (unsigned char)disp;
  }
#ifdef DRC_TEXT
  o->label(o->pos, f.label);
#endif
}

/* ------------------------------------------------------------------ */
/* translator state */

#define R_A    7   /* d7 */
#define R_ST   6   /* d6 */
#define R_CYC  5   /* d5 */
#define R_GEN  4   /* d4 */
#define R_S    3   /* a3 */
#define R_BASE 4   /* a4 */

#define SSPR_X 1
#define SSPR_Y 2
#define SSPR_A 3
#define SSPR_ST 4
#define SSPR_STACK 5
#define SSPR_PC 6
#define SSPR_P 7

#define FLAG_LZVN_CLR 0x0fff
#define FLAG_ZN_CLR   0x5fff

/* A rare path, emitted after the block's hot code so that the branch to
 * it is a forward branch that is normally not taken (Emu68 keeps such a
 * branch inside its translation unit; a taken forward branch leaves it). */
#define COLD_EXIT        1   /* [sf flag]; d0 = u; d5 -= a; to the dispatcher */
#define COLD_MOVEQ_BACK  2   /* moveq #a,d<b>; back */
#define COLD_SLOW_READ   3   /* C read handler a, k = b, PC = u; back */
#define COLD_SLOW_WRITE  4   /* C write handler a (value d0), k = b, PC = u; back */
#define COLD_RAW_READ    5   /* raw PMx register a (PM0 idle check if b); back */
#define COLD_RAW_WRITE   6   /* raw PMx register a = d0; back */
#define COLD_DRAM_READ   7   /* PMx (reg c) read, not ROM: DRAM or C; back / back2 */
#define COLD_WRITE_OTHER 8   /* PMx (reg c) write, not plain/overwrite DRAM; back */
#define COLD_IDLE4       9   /* read_PM4 idle loop: set WAIT bit a, flag; back */
#define COLD_PMC_PENDING 10  /* PMx access with PMC_SET / HAVE_ADDR pending (pm_io):
                              * c = reg, b = write, a = blind access, u = idle WAIT
                              * bit for a PM4 read that returns 0 here (or 0);
                              * back = continue after the guard, back2 = done */
#define DRC_MAX_COLD 320   /* up to ~9 per instruction (PM to PM moves) x 24 + checks */

struct drc_cold
{
  int kind;
  struct drc_fix from;      /* the branch into the cold code */
  struct drc_fix back;      /* where it continues in the hot code */
  struct drc_fix back2;
  int a, b, c, clear;
  drc_u32 u;
};

static struct drc_cold drc_cold_buf[DRC_MAX_COLD];

struct drc_tr
{
  const struct drc_env *e;
  struct drc_out *o;
  const drc_u16 *code;  /* the program words (svp->iram_rom as words) */
  int k;                /* DSP instructions before the current one */
  drc_u32 pc_after;     /* DSP word address PC points to while it runs */
  int handler;          /* a C register handler ran in this instruction */
  drc_u32 pcw;          /* DSP word address of the current instruction */
  int ends;             /* this instruction ends the block */
  struct drc_cold *cold;  /* rare paths, emitted after the block (drc_cold_emit) */
  int ncold;
};

#define GR_H(t, r) ((t)->e->off_gr + 4 * (r))       /* gr[r].byte.h */
#define GR_V(t, r) ((t)->e->off_gr + 4 * (r))       /* gr[r].v */
#define GR_L(t, r) ((t)->e->off_gr + 4 * (r) + 2)   /* gr[r].byte.l */

/* d5 -= n */
static void g_sub_cyc(struct drc_tr *t, int n)
{
  if (n >= 1 && n <= 8) i_subq_l(t->o, n, R_CYC);
  else if (n > 8) i_subi_l(t->o, (drc_u32)n, R_CYC);
}

/* profile builds: count a block exit of this kind */
static void g_stat(struct drc_tr *t, int kind)
{
  if (t->e->addr_exitstat)
    i_addq_l_abs(t->o, 1, t->e->addr_exitstat + 4 * kind);
}

/* leave the block: continue at DSP word address next, n cycles used */
static void g_exit(struct drc_tr *t, drc_u32 next, int n)
{
  i_move_w_imm_dn(t->o, (int)next, 0);
  g_sub_cyc(t, n);
  i_goto_abs(t->o, t->e->addr_dispatch);
}

/* queue a rare path; the forward branch to it must already be emitted */
static struct drc_cold *g_cold(struct drc_tr *t, int kind, struct drc_fix from)
{
  struct drc_cold *c;
  if (t->ncold >= DRC_MAX_COLD)
  {
    t->o->overflow = 1;           /* the block is translated again, shorter */
    c = &t->cold[DRC_MAX_COLD - 1];
  }
  else
    c = &t->cold[t->ncold++];
  c->kind = kind;
  c->from = from;
  c->back.pos = c->back2.pos = -1;
  c->a = c->b = c->c = c->clear = 0;
  c->u = 0;
  return c;
}

/* forward branch to a rare exit: next PC, n cycles used */
static void g_cold_exit(struct drc_tr *t, int cc, drc_u32 next, int n, int clear_flag)
{
  struct drc_cold *c = g_cold(t, COLD_EXIT, i_bcc_w(t->o, cc));
  c->u = next;
  c->a = n;
  c->clear = clear_flag;
}

/* Z/N (and L/V cleared) from data register r, as UPD_LZVN / UPD_ACC_ZN */
static void g_flags(struct drc_tr *t, int r, int clr)
{
  /* branchless: a taken forward branch costs Emu68 a trip through its
   * main loop. Z -> bit 13, N -> bit 15 (N only when not zero). */
  i_andi_w(t->o, clr, R_ST);
  i_tst_l(t->o, r);
  i_seq(t->o, 1);
  i_smi(t->o, 2);
  i_andi_w(t->o, 0x0020, 1);
  i_andi_w(t->o, 0x0080, 2);
  i_or_w_dd(t->o, 2, 1);
  i_lsl_w_imm(t->o, 8, 1);
  i_or_w_dd(t->o, 1, R_ST);
}

/* rP = X * Y * 2 (read_P); result also in d0 */
static void g_read_p(struct drc_tr *t)
{
  i_move_w_d16_dn(t->o, GR_H(t, SSPR_X), R_S, 0);
  i_muls_w_d16(t->o, GR_H(t, SSPR_Y), R_S, 0);
  i_add_l_dd(t->o, 0, 0);
  i_move_l_dn_d16(t->o, 0, GR_V(t, SSPR_P), R_S);
}

/* Hand the state to a C handler (F_OUT): A, ST, the global PC pointer and
 * g_cycles as the interpreter would have them. Keeps d0. */
static void g_sync_out(struct drc_tr *t)
{
  i_move_l_dn_d16(t->o, R_A, GR_V(t, SSPR_A), R_S);
  i_move_w_dn_d16(t->o, R_ST, GR_H(t, SSPR_ST), R_S);
  i_move_l_imm_abs(t->o, t->e->base_addr + 2 * t->pc_after, t->e->addr_PC);
  i_move_l_dd(t->o, R_CYC, 1);
  if (t->k >= 1 && t->k <= 8) i_subq_l(t->o, t->k, 1);
  else if (t->k > 8) i_subi_l(t->o, (drc_u32)t->k, 1);
  i_move_l_dn_abs(t->o, 1, t->e->addr_g_cycles);
}

/* ... and take it back (F_IN). Keeps d0. */
static void g_sync_in(struct drc_tr *t)
{
  i_move_l_d16_dn(t->o, GR_V(t, SSPR_A), R_S, R_A);
  i_moveq(t->o, 0, R_ST);
  i_move_w_d16_dn(t->o, GR_H(t, SSPR_ST), R_S, R_ST);
  i_move_l_abs_dn(t->o, t->e->addr_g_cycles, R_CYC);
  if (t->k >= 1 && t->k <= 8) i_addq_l(t->o, t->k, R_CYC);
  else if (t->k > 8) i_addi_l(t->o, (drc_u32)t->k, R_CYC);
  t->handler = 1;
}

/* C handler call for REG_READ(r) -> d0, through the shared slow_read */
static void g_reg_read_slow(struct drc_tr *t, int r)
{
  i_moveq(t->o, r, 2);
  i_moveq(t->o, t->k, 1);
  i_movea_l_imm_a1(t->o, t->e->base_addr + 2 * t->pc_after);
  i_jsr_abs(t->o, t->e->addr_slow_read);
  t->handler = 1;
}

/* C handler call for REG_WRITE(r, d0), through the shared slow_write */
static void g_reg_write_slow(struct drc_tr *t, int r)
{
  i_moveq(t->o, r, 2);
  i_moveq(t->o, t->k, 1);
  i_movea_l_imm_a1(t->o, t->e->base_addr + 2 * t->pc_after);
  i_jsr_abs(t->o, t->e->addr_slow_write);
  t->handler = 1;
}

static void g_pm_read(struct drc_tr *t, int r);
static void g_pm_write(struct drc_tr *t, int r);
static void g_pmc_read(struct drc_tr *t);
static void g_pmc_write(struct drc_tr *t);
static void g_stack_read(struct drc_tr *t);

/* REG_READ(r) -> d0 (low word; upper word undefined) */
static void g_reg_read(struct drc_tr *t, int r, drc_u32 op)
{
  switch (r)
  {
    case 0: case SSPR_X: case SSPR_Y:
      i_move_w_d16_dn(t->o, GR_H(t, r), R_S, 0); break;
    case SSPR_A:
      i_move_l_dd(t->o, R_A, 0); i_swap(t->o, 0); break;
    case SSPR_ST:
      i_move_w_dd(t->o, R_ST, 0); break;
    case SSPR_PC:   /* read_PC: GET_PC(), the word after this instruction */
      i_move_w_imm_dn(t->o, (int)t->pc_after, 0); break;
    case SSPR_P:    /* read_P: updates rP, returns its high word */
      g_read_p(t); i_swap(t->o, 0); break;
    case 13:        /* read_unknown */
      i_moveq(t->o, 0, 0); break;
    case 15:        /* read_AL: "ld -, AL" (op 0x000f) also drops a pending PMC set */
      if (op == 0x000f) i_andi_l_d16(t->o, ~(drc_u32)3, t->e->off_emust, R_S);
      i_move_w_dd(t->o, R_A, 0); break;
    case SSPR_STACK:
      g_stack_read(t); break;
    case 8: case 9: case 10: case 12:   /* PM0-PM2, PM4 */
      g_pm_read(t, r); break;
    case 14:                            /* PMC */
      g_pmc_read(t); break;
    default:        /* XST */
      g_reg_read_slow(t, r);
      break;
  }
}

/* write_STACK(d0): if (rSTACK >= 6) rSTACK = 0; stack[rSTACK++] = d */
static void g_push_stack(struct drc_tr *t)
{
  struct drc_cold *c;
  i_move_w_d16_dn(t->o, GR_H(t, SSPR_STACK), R_S, 1);
  i_cmpi_w(t->o, 6, 1);
  c = g_cold(t, COLD_MOVEQ_BACK, i_bcc_w(t->o, 4));   /* bcc: >= 6, wrap to 0 */
  c->a = 0; c->b = 1;
  c->back = drc_mark(t->o);
  i_move_w_dd(t->o, 1, 2);
  i_add_w_dd(t->o, 2, 2);
  i_addi_w(t->o, t->e->off_stack, 2);
  i_move_w_dn_idx(t->o, 0, R_S, 2);
  i_addq_w(t->o, 1, 1);
  i_move_w_dn_d16(t->o, 1, GR_H(t, SSPR_STACK), R_S);
}

/* REG_WRITE(r, d0) */
static void g_reg_write(struct drc_tr *t, int r)
{
  switch (r)
  {
    case 0: case SSPR_P: case 13: break;              /* constant / write_unknown */
    case SSPR_X: case SSPR_Y:
      i_move_w_dn_d16(t->o, 0, GR_H(t, r), R_S); break;
    case SSPR_A:
      i_swap(t->o, R_A); i_move_w_dd(t->o, 0, R_A); i_swap(t->o, R_A); break;
    case SSPR_ST:   /* write_ST: rST = d */
      i_move_w_dd(t->o, 0, R_ST); break;
    case SSPR_STACK:
      g_push_stack(t); break;
    case SSPR_PC:   /* write_PC: SET_PC(d); g_cycles-- ; ends the block */
      g_stat(t, 6);
      g_sub_cyc(t, t->k + 2);   /* next PC is in d0 already */
      i_goto_abs(t->o, t->e->addr_dispatch);
      t->ends = 1;
      break;
    case 15:        /* write_AL */
      i_move_w_dd(t->o, 0, R_A); break;
    case 8: case 9: case 10: case 12:   /* PM0-PM2, PM4 */
      g_pm_write(t, r); break;
    case 14:                            /* PMC */
      g_pmc_write(t); break;
    default:        /* XST */
      g_reg_write_slow(t, r);
      break;
  }
}

/* ptr1_read_(t) -> d0. Uses d1-d3. */
static void g_ptr1_read(struct drc_tr *t, int ti)
{
  int bank = (ti & 4) ? 512 : 0;
  int ri = ti & 3, mod = ti >> 3;
  int preg = t->e->off_ptr + (ti & 4) + ri;
  struct drc_fix modulo, done;

  if (ri == 3)  /* fixed RAMx[mod] */
  {
    i_move_w_d16_dn(t->o, bank + 2 * mod, R_S, 0);
    return;
  }
  i_moveq(t->o, 0, 1);
  i_move_b_d16_dn(t->o, preg, R_S, 1);
  if (mod <= 1)
  {
    i_add_w_dd(t->o, 1, 1);
    if (bank) i_addi_w(t->o, bank, 1);
    i_move_w_idx_dn(t->o, R_S, 1, 0);
    if (mod == 1) i_addq_b_d16(t->o, 1, preg, R_S);
    return;
  }
  /* mod 2 "-" / mod 3 "+", modulo by ST & 7 */
  i_move_w_dd(t->o, 1, 2);
  i_add_w_dd(t->o, 1, 1);
  if (bank) i_addi_w(t->o, bank, 1);
  i_move_w_idx_dn(t->o, R_S, 1, 0);
  i_move_w_dd(t->o, R_ST, 3);
  i_andi_w(t->o, 7, 3);
  modulo = i_bcc_w(t->o, CC_NE);
  if (mod == 3) i_addq_b_d16(t->o, 1, preg, R_S); else i_subq_b_d16(t->o, 1, preg, R_S);
  done = i_bcc_w(t->o, CC_T);
  drc_here(t->o, modulo);
  /* mask = (1 << (st & 7)) - 1; r = (r & ~mask) | ((r + add) & mask) */
  i_moveq(t->o, 1, 1);
  i_lsl_w_reg(t->o, 3, 1);
  i_subq_w(t->o, 1, 1);
  i_move_w_dd(t->o, 2, 3);
  if (mod == 3) i_addq_w(t->o, 1, 3); else i_subq_w(t->o, 1, 3);
  i_and_w_dd(t->o, 1, 3);
  i_not_w(t->o, 1);
  i_and_w_dd(t->o, 1, 2);
  i_or_w_dd(t->o, 3, 2);
  i_move_b_dn_d16(t->o, 2, preg, R_S);
  drc_here(t->o, done);
}

/* ptr1_write(op, d0). Uses d1. */
static void g_ptr1_write(struct drc_tr *t, drc_u32 op)
{
  int ti = (int)((op & 3) | ((op >> 6) & 4) | ((op << 1) & 0x18));
  int bank = (ti & 4) ? 512 : 0;
  int ri = ti & 3, mod = ti >> 3;
  int preg = t->e->off_ptr + (ti & 4) + ri;

  if (ri == 3)
  {
    i_move_w_dn_d16(t->o, 0, bank + 2 * mod, R_S);
    return;
  }
  i_moveq(t->o, 0, 1);
  i_move_b_d16_dn(t->o, preg, R_S, 1);
  i_add_w_dd(t->o, 1, 1);
  if (bank) i_addi_w(t->o, bank, 1);
  i_move_w_dn_idx(t->o, 0, R_S, 1);
  if (mod == 1 || mod == 3) i_addq_b_d16(t->o, 1, preg, R_S);
  else if (mod == 2) i_subq_b_d16(t->o, 1, preg, R_S);
}

/* ptr2_read(op) -> d0 (C, touches only the struct and program memory) */
static void g_ptr2_read(struct drc_tr *t, drc_u32 op)
{
  i_push_imm(t->o, op);
  i_jsr_abs(t->o, t->e->addr_ptr2);
  i_addq_l_sp(t->o, 4);
}

#define ALU_SUB 1
#define ALU_CMP 3
#define ALU_ADD 4
#define ALU_AND 5
#define ALU_OR  6
#define ALU_EOR 7

/* accumulator op with the 16-bit operand in d0 (low word) */
static void g_alu16_d0(struct drc_tr *t, int alu)
{
  i_swap(t->o, 0);
  i_clr_w(t->o, 0);
  switch (alu)
  {
    case ALU_SUB: i_sub_l_dd(t->o, 0, R_A); g_flags(t, R_A, FLAG_LZVN_CLR); break;
    case ALU_ADD: i_add_l_dd(t->o, 0, R_A); g_flags(t, R_A, FLAG_LZVN_CLR); break;
    case ALU_CMP: i_move_l_dd(t->o, R_A, 1); i_sub_l_dd(t->o, 0, 1); g_flags(t, 1, FLAG_LZVN_CLR); break;
    case ALU_AND: i_and_l_dd(t->o, 0, R_A); g_flags(t, R_A, FLAG_ZN_CLR); break;
    case ALU_OR:  i_or_l_dd(t->o, 0, R_A);  g_flags(t, R_A, FLAG_ZN_CLR); break;
    case ALU_EOR: i_eor_l_dd(t->o, 0, R_A); g_flags(t, R_A, FLAG_ZN_CLR); break;
  }
}

/* ... with a constant 16-bit operand */
static void g_alu16_imm(struct drc_tr *t, int alu, drc_u32 v)
{
  drc_u32 x = (v & 0xffff) << 16;
  switch (alu)
  {
    case ALU_SUB: i_subi_l(t->o, x, R_A); g_flags(t, R_A, FLAG_LZVN_CLR); break;
    case ALU_ADD: i_addi_l(t->o, x, R_A); g_flags(t, R_A, FLAG_LZVN_CLR); break;
    case ALU_CMP: i_move_l_dd(t->o, R_A, 1); i_subi_l(t->o, x, 1); g_flags(t, 1, FLAG_LZVN_CLR); break;
    case ALU_AND: i_andi_l(t->o, x, R_A); g_flags(t, R_A, FLAG_ZN_CLR); break;
    case ALU_OR:  i_ori_l(t->o, x, R_A);  g_flags(t, R_A, FLAG_ZN_CLR); break;
    case ALU_EOR: i_eori_l(t->o, x, R_A); g_flags(t, R_A, FLAG_ZN_CLR); break;
  }
}

/* ... 32-bit operand in d0 (OP_CHECK32 'A <- P' forms) */
static void g_alu32_d0(struct drc_tr *t, int alu)
{
  switch (alu)
  {
    case ALU_SUB: i_sub_l_dd(t->o, 0, R_A); g_flags(t, R_A, FLAG_LZVN_CLR); break;
    case ALU_ADD: i_add_l_dd(t->o, 0, R_A); g_flags(t, R_A, FLAG_LZVN_CLR); break;
    case ALU_CMP: i_move_l_dd(t->o, R_A, 1); i_sub_l_dd(t->o, 0, 1); g_flags(t, 1, FLAG_LZVN_CLR); break;
    case ALU_AND: i_and_l_dd(t->o, 0, R_A); g_flags(t, R_A, FLAG_ZN_CLR); break;
    case ALU_OR:  i_or_l_dd(t->o, 0, R_A);  g_flags(t, R_A, FLAG_ZN_CLR); break;
    case ALU_EOR: i_eor_l_dd(t->o, 0, R_A); g_flags(t, R_A, FLAG_ZN_CLR); break;
  }
}

/* OP_CHECK32 'A <- A' forms: operand is the accumulator itself */
static void g_alu_aa(struct drc_tr *t, int alu)
{
  switch (alu)
  {
    case ALU_SUB: i_moveq(t->o, 0, R_A); g_flags(t, R_A, FLAG_LZVN_CLR); break;
    case ALU_ADD: i_add_l_dd(t->o, R_A, R_A); g_flags(t, R_A, FLAG_LZVN_CLR); break;
    case ALU_CMP: i_andi_w(t->o, FLAG_LZVN_CLR, R_ST); i_ori_w(t->o, 0x2000, R_ST); break;
    case ALU_AND: case ALU_OR: g_flags(t, R_A, FLAG_ZN_CLR); break;
    case ALU_EOR: i_moveq(t->o, 0, R_A); g_flags(t, R_A, FLAG_ZN_CLR); break;
  }
}

/* COND_CHECK: returns 1 always, 0 never, else 2 and leaves a Bcc.W that is
 * taken when the condition is FALSE in *skip */
static int g_cond(struct drc_tr *t, drc_u32 op, struct drc_fix *skip)
{
  int bit, want;
  switch (op & 0xf0)
  {
    case 0x00: return 1;
    case 0x50: bit = 13; break;   /* Z */
    case 0x70: bit = 15; break;   /* N */
    default:   return 0;
  }
  want = (op >> 8) & 1;           /* flag must equal this bit of op */
  i_btst_imm(t->o, bit, R_ST);
  /* btst sets Z when the flag is 0: false if (flag != want) */
  *skip = i_bcc_w(t->o, want ? CC_EQ : CC_NE);
  return 2;
}

/* exit checks after an instruction that called a C handler: the
 * interpreter loop stops on an idle wait, and an IRAM write may have
 * changed code this block was translated from */
static void g_after_handler(struct drc_tr *t, drc_u32 next)
{
  i_tst_b_abs(t->o, t->e->addr_flag);
  g_cold_exit(t, CC_NE, next, t->k + 1, 1);
}

#define IJ(op) (int)(((op) & 3) | (((op) >> 6) & 4))
#define PTR1T(op) (int)(((op) & 3) | (((op) >> 6) & 4) | (((op) << 1) & 0x18))

/* ------------------------------------------------------------------ */
/* inline register handlers (the common paths of pm_io, read_PM0/PM4,
 * read/write_PMC and read_STACK in ssp16.c); anything else calls the C
 * handler through the slow path above */

#define PMAC(t, w, reg) ((t)->e->off_pmac + 4 * (6 * (w) + (reg)))

/* pmac += get_inc(mode): d2.w = mode, a1 = &pmac[w][reg]. The table has
 * get_inc() for (mode >> 11) & 31 (bit 14 is 0 on these paths). */
static void g_pm_inc(struct drc_tr *t)
{
  i_lsr_w_imm(t->o, 8, 2);
  i_lsr_w_imm(t->o, 3, 2);
  i_add_w_dd(t->o, 2, 2);
  i_add_w_dd(t->o, 2, 2);
  i_movea_l_imm_a0(t->o, t->e->addr_inctab);
  i_move_l_idxw_dn(t->o, 0, 2, 2);
  i_add_l_dn_ind_a1(t->o, 2);
}

/* pm_io with PMC_SET pending: "this MUST be blind r or w" - the word
 * before PC, *(PC-1), is the instruction (or its immediate). Known when
 * translating. 1 = blind, so the PMC value is stored. */
static int g_blind(struct drc_tr *t)
{
  drc_u32 w = t->code[t->pc_after - 1];
  return !((w & 0xff0f) && (w & 0xfff0));
}

/* d3 = get_inc(mode), d2.w = mode; uses a2 */
static void g_pm_inc_d3(struct drc_tr *t)
{
  i_move_w_dd(t->o, 2, 3);
  i_lsr_w_imm(t->o, 8, 3);
  i_lsr_w_imm(t->o, 3, 3);
  i_add_w_dd(t->o, 3, 3);
  i_add_w_dd(t->o, 3, 3);
  i_movea_l_imm_a2(t->o, t->e->addr_inctab);
  i_move_l_idxw_dn(t->o, 2, 3, 3);
}

/* PM0-PM2 / PM4 read -> d0 */
static void g_pm_read(struct drc_tr *t, int r)
{
  struct drc_out *o = t->o;
  const struct drc_env *e = t->e;
  int reg = (r == 12) ? 4 : r - 8;
  drc_u32 ppc = 2 * t->pcw;               /* GET_PPC_OFFS() */
  struct drc_cold *c_slow, *c_raw = NULL, *c_other, *c_idle = NULL;
  struct drc_fix got, done;

  /* pm_io: a PMC set or half-written PMC address takes the C path */
  i_move_w_d16_dn(o, e->off_emust + 2, R_S, 1);
  i_andi_w(o, 3, 1);
  c_slow = g_cold(t, COLD_PMC_PENDING, i_bcc_w(o, CC_NE));
  c_slow->c = reg; c_slow->b = 0; c_slow->a = g_blind(t);
  c_slow->u = (reg == 4 && ppc == 0x0854) ? 0x8000 : (reg == 4 && ppc == 0x4f12) ? 0x4000 : 0;
  if (c_slow->u) t->handler = 1;
  c_slow->back = drc_mark(o);
  if (reg != 4)
  {
    /* external memory only when (rST & 0x60); else the raw register */
    i_move_w_dd(o, R_ST, 1);
    i_andi_w(o, 0x60, 1);
    c_raw = g_cold(t, COLD_RAW_READ, i_bcc_w(o, CC_EQ));
    c_raw->a = r;
    c_raw->b = (reg == 0 && (ppc == 0x800 || ppc == 0x1851e));
    if (c_raw->b) t->handler = 1;
  }
  i_lea_d16_a1(o, PMAC(t, 0, reg), R_S);
  i_move_l_ind_a1_dn(o, 1);               /* d1 = mode:addr */
  i_move_l_dd(o, 1, 2);
  i_swap(o, 2);                           /* d2.w = mode */
  /* ROM ((mode & 0xfff0) == 0x0800, pmac++) and DRAM ((mode & 0x47ff) ==
   * 0x0018, pmac += get_inc(mode)) without a taken branch: get_inc() of a
   * ROM mode is 1, so the increment comes from the table either way; both
   * words are read and d3 selects one. Any other mode goes to C. */
  i_move_w_dd(o, 2, 3);
  i_andi_w(o, 0xfff0, 3);
  i_cmpi_w(o, 0x0800, 3);
  i_seq(o, 3);                            /* d3.b = $ff: ROM */
  i_move_w_dd(o, 2, 0);
  i_andi_w(o, 0x47ff, 0);
  i_cmpi_w(o, 0x0018, 0);
  i_seq(o, 0);                            /* d0.b = $ff: DRAM */
  i_or_b_dd(o, 3, 0);
  c_other = g_cold(t, COLD_SLOW_READ, i_bcc_w(o, CC_EQ));
  c_other->a = r; c_other->b = t->k; c_other->u = e->base_addr + 2 * t->pc_after;
  if (e->addr_pmstat) i_addq_l_abs(o, 1, e->addr_pmstat + 0);
  i_ext_w(o, 3);
  i_ext_l(o, 3);                          /* d3 = -1 ROM, 0 DRAM */
  i_move_l_dd(o, 2, 0);                   /* ROM word index: addr | (mode & 0xf) << 16 */
  i_andi_l(o, 0xf, 0);
  i_swap(o, 0);
  i_move_w_dd(o, 1, 0);
  i_add_l_dd(o, 0, 0);
  g_pm_inc(t);                            /* pmac += increment (old addr kept in d1) */
  i_movea_l_imm_a0(o, e->addr_rom);
  i_move_w_idxl_dn(o, 0, 0, 0);           /* d0 = ROM word */
  i_moveq(o, 0, 2);
  i_move_w_dd(o, 1, 2);
  i_add_l_dd(o, 2, 2);
  i_movea_l_imm_a0(o, e->addr_dram);
  i_move_w_idxl_dn(o, 0, 2, 2);           /* d2 = DRAM word */
  i_eor_w_dd(o, 2, 0);
  i_and_w_dd(o, 3, 0);
  i_eor_w_dd(o, 2, 0);                    /* ROM ? rom : dram */
  got = drc_mark(o);
  i_move_l_ind_a1_d16(o, GR_V(t, 14), R_S);              /* rPMC.v = pmac */
  if (reg == 4 && (ppc == 0x0854 || ppc == 0x4f12))
  {
    /* read_PM4: idle loop on [30fe08] / [30fe06] reading 0 */
    i_tst_w(o, 0);
    c_idle = g_cold(t, COLD_IDLE4, i_bcc_w(o, CC_EQ));
    c_idle->a = (ppc == 0x0854) ? 0x8000 : 0x4000;
    t->handler = 1;
  }
  done = drc_mark(o);
  c_slow->back2 = done;
  if (c_raw) c_raw->back = done;
  c_other->back = done;
  (void)got;
  if (c_idle) c_idle->back = done;
  t->handler = 1;   /* the slow path may call a C handler */
}

/* PM0-PM2 / PM4 write of d0 */
/* store d0 into DRAM at a0 as plain (mode bit 10 clear) or overwrite
 * write: one branchless read-modify-write, a plain write being an
 * overwrite with mask $ffff. d2.w = mode on entry; d1, d2, a2 used. In
 * verify builds the write is logged; a1 is reloaded with &pmac[1][reg]. */
static void g_pm_store(struct drc_tr *t, int reg)
{
  struct drc_out *o = t->o;
  const struct drc_env *e = t->e;
  i_move_w_dd(o, 2, 1);
  i_andi_w(o, 0x0400, 1);
  i_seq(o, 1);
  i_ext_w(o, 1);                          /* $ffff plain, 0 overwrite */
  i_movea_l_imm_a2(o, e->addr_masktab);
  i_moveq(o, 0, 2);
  i_move_w_dd(o, 0, 2);
  i_add_l_dd(o, 2, 2);
  i_or_w_a2idxl_dn(o, 2, 1);              /* | non-zero nibbles of d */
  i_move_w_ind_a0_dn(o, 2);               /* old */
  i_eor_w_dd(o, 2, 0);
  i_and_w_dd(o, 1, 0);
  i_eor_w_dd(o, 2, 0);                    /* old ^ ((old ^ d) & mask) */
  i_move_w_dn_ind_a0(o, 0);
  if (e->addr_vlog)
  {
    i_push_dn(o, 0);
    i_push_dn(o, 2);
    i_move_l_a0_dn(o, 1);
    i_subi_l(o, e->addr_dram, 1);
    i_push_dn(o, 1);
    i_jsr_abs(o, e->addr_vlog);
    i_lea_d16_sp_sp(o, 12);
    i_lea_d16_a1(o, PMAC(t, 1, reg), R_S);
  }
}

static void g_pm_write(struct drc_tr *t, int r)
{
  struct drc_out *o = t->o;
  const struct drc_env *e = t->e;
  int reg = (r == 12) ? 4 : r - 8;
  struct drc_cold *c_slow, *c_raw = NULL, *c_other;
  struct drc_fix done;

  i_move_w_d16_dn(o, e->off_emust + 2, R_S, 1);
  i_andi_w(o, 3, 1);
  c_slow = g_cold(t, COLD_PMC_PENDING, i_bcc_w(o, CC_NE));
  c_slow->c = reg; c_slow->b = 1; c_slow->a = g_blind(t); c_slow->u = 0;
  c_slow->back = drc_mark(o);
  if (reg != 4)
  {
    i_move_w_dd(o, R_ST, 1);
    i_andi_w(o, 0x60, 1);
    c_raw = g_cold(t, COLD_RAW_WRITE, i_bcc_w(o, CC_EQ));
    c_raw->a = r;
  }
  i_lea_d16_a1(o, PMAC(t, 1, reg), R_S);
  i_move_l_ind_a1_dn(o, 1);               /* d1 = mode:addr */
  i_move_l_dd(o, 1, 2);
  i_swap(o, 2);                           /* d2.w = mode */
  i_moveq(o, 0, 3);
  i_move_w_dd(o, 1, 3);
  i_add_l_dd(o, 3, 3);
  i_movea_l_imm_a0(o, e->addr_dram);
  i_adda_l_dn_a0(o, 3);                   /* a0 = &dram[addr] */
  /* DRAM, cell increment: (mode & 0xfbff) == 0x4018. The most common
   * write in Virtua Racing (measured: ~2,100 per frame against ~560
   * plain/overwrite), so it is the path without a taken branch. */
  i_move_w_dd(o, 2, 3);
  i_andi_w(o, 0xfbff, 3);
  i_cmpi_w(o, 0x4018, 3);
  c_other = g_cold(t, COLD_WRITE_OTHER, i_bcc_w(o, CC_NE));
  c_other->a = r; c_other->b = t->k; c_other->c = reg; c_other->u = e->base_addr + 2 * t->pc_after;
  if (e->addr_pmstat) i_addq_l_abs(o, 1, e->addr_pmstat + 20);
  /* d3 = (addr & 1) ? 31 : 1 without a branch */
  i_move_w_dd(o, 1, 3);
  i_andi_l(o, 1, 3);
  i_neg_l(o, 3);
  i_andi_l(o, 30, 3);
  i_addq_l(o, 1, 3);
  g_pm_store(t, reg);
  i_add_l_dn_ind_a1(o, 3);                /* pmac += increment */
  i_move_l_ind_a1_d16(o, GR_V(t, 14), R_S);
  done = drc_mark(o);
  c_slow->back2 = done;
  if (c_raw) c_raw->back = done;
  c_other->back = done;
  t->handler = 1;
}

/* read_PMC -> d0 */
static void g_pmc_read(struct drc_tr *t)
{
  struct drc_out *o = t->o;
  int st = t->e->off_emust + 3;           /* low byte: 1 HAVE_ADDR, 2 SET */
  struct drc_fix noaddr, done;
  i_move_w_d16_dn(o, GR_L(t, 14), R_S, 0);
  i_btst_imm_d16(o, 0, st, R_S);
  noaddr = i_bcc_w(o, CC_EQ);
  i_andi_b_d16(o, 0xfe, st, R_S);
  i_ori_b_d16(o, 0x02, st, R_S);
  /* ((l << 4) & 0xfff0) | ((l >> 4) & 0xf) */
  i_move_w_dd(o, 0, 1);
  i_lsl_w_imm(o, 4, 0);
  i_lsr_w_imm(o, 4, 1);
  i_andi_w(o, 0xf, 1);
  i_or_w_dd(o, 1, 0);
  done = i_bcc_w(o, CC_T);
  drc_here(o, noaddr);
  i_ori_b_d16(o, 0x01, st, R_S);
  drc_here(o, done);
}

/* write_PMC(d0) */
static void g_pmc_write(struct drc_tr *t)
{
  struct drc_out *o = t->o;
  int st = t->e->off_emust + 3;
  struct drc_fix noaddr, done;
  i_btst_imm_d16(o, 0, st, R_S);
  noaddr = i_bcc_w(o, CC_EQ);
  i_andi_b_d16(o, 0xfe, st, R_S);
  i_ori_b_d16(o, 0x02, st, R_S);
  i_move_w_dn_d16(o, 0, GR_H(t, 14), R_S);
  done = i_bcc_w(o, CC_T);
  drc_here(o, noaddr);
  i_ori_b_d16(o, 0x01, st, R_S);
  i_move_w_dn_d16(o, 0, GR_L(t, 14), R_S);
  drc_here(o, done);
}

/* read_STACK -> d0: --rSTACK; if ((short)rSTACK < 0) rSTACK = 5 */
static void g_stack_read(struct drc_tr *t)
{
  struct drc_out *o = t->o;
  struct drc_cold *c;
  i_move_w_d16_dn(o, GR_H(t, SSPR_STACK), R_S, 1);
  i_subq_w(o, 1, 1);
  c = g_cold(t, COLD_MOVEQ_BACK, i_bcc_w(o, 11));     /* bmi: below 0, wrap to 5 */
  c->a = 5; c->b = 1;
  c->back = drc_mark(o);
  i_move_w_dn_d16(o, 1, GR_H(t, SSPR_STACK), R_S);
  i_add_w_dd(o, 1, 1);
  i_addi_w(o, t->e->off_stack, 1);
  i_move_w_idx_dn(o, R_S, 1, 0);
}

/* Emit the queued rare paths after the block. Each starts where its
 * forward branch points and ends with an exit or a bra.w back. */
static void drc_cold_emit(struct drc_tr *t)
{
  struct drc_out *o = t->o;
  const struct drc_env *e = t->e;
  int i;
  for (i = 0; i < t->ncold; i++)
  {
    struct drc_cold *c = &t->cold[i];
    drc_here(o, c->from);
    t->k = c->b;
    switch (c->kind)
    {
      case COLD_EXIT:
        g_stat(t, c->clear ? 8 : 7);
        if (c->clear) i_sf_abs(o, e->addr_flag);
        i_move_w_imm_dn(o, (int)c->u, 0);
        g_sub_cyc(t, c->a);
        i_goto_abs(o, e->addr_dispatch);
        break;
      case COLD_MOVEQ_BACK:
        i_moveq(o, c->a, c->b);
        i_bra_mark(o, c->back);
        break;
      case COLD_SLOW_READ:
      case COLD_SLOW_WRITE:
        if (e->addr_pmstat) i_addq_l_abs(o, 1, e->addr_pmstat + (c->kind == COLD_SLOW_READ ? 12 : 28));
        i_moveq(o, c->a, 2);
        i_moveq(o, c->b, 1);
        i_movea_l_imm_a1(o, c->u);
        i_jsr_abs(o, c->kind == COLD_SLOW_READ ? e->addr_slow_read : e->addr_slow_write);
        i_bra_mark(o, c->back);
        break;
      case COLD_RAW_READ:
        if (e->addr_pmstat) i_addq_l_abs(o, 1, e->addr_pmstat + 8);
        i_move_w_d16_dn(o, e->off_gr + 4 * c->a, R_S, 0);
        if (c->a == 8)
        {
          if (c->b)
          {
            /* read_PM0: idle loop on PM0 bit 1 */
            struct drc_fix nz;
            i_btst_imm(o, 1, 0);
            nz = i_bcc_w(o, CC_NE);
            i_ori_w_d16(o, 0x2000, e->off_emust + 2, R_S);   /* SSP_WAIT_PM0 */
            i_st_abs(o, e->addr_flag);
            drc_here(o, nz);
          }
          i_andi_w_d16(o, 0xfffd, e->off_gr + 4 * c->a, R_S);  /* rPM0 &= ~2 */
        }
        i_bra_mark(o, c->back);
        break;
      case COLD_RAW_WRITE:
        if (e->addr_pmstat) i_addq_l_abs(o, 1, e->addr_pmstat + 24);
        i_move_w_dn_d16(o, 0, e->off_gr + 4 * c->a, R_S);
        i_bra_mark(o, c->back);
        break;
      case COLD_DRAM_READ:
      {
        /* a1 = &pmac[0][reg], d1 = mode:addr, d2.w = mode */
        struct drc_fix slow;
        i_move_w_dd(o, 2, 3);
        i_andi_w(o, 0x47ff, 3);
        i_cmpi_w(o, 0x0018, 3);
        slow = i_bcc_w(o, CC_NE);
        if (e->addr_pmstat) i_addq_l_abs(o, 1, e->addr_pmstat + 4);
        i_moveq(o, 0, 3);
        i_move_w_dd(o, 1, 3);
        i_add_l_dd(o, 3, 3);
        i_movea_l_imm_a0(o, e->addr_dram);
        i_move_w_idxl_dn(o, 0, 3, 0);
        g_pm_inc(t);
        i_bra_mark(o, c->back);               /* to rPMC update */
        drc_here(o, slow);
        if (e->addr_pmstat) i_addq_l_abs(o, 1, e->addr_pmstat + 12);
        i_moveq(o, c->a, 2);
        i_moveq(o, c->b, 1);
        i_movea_l_imm_a1(o, c->u);
        i_jsr_abs(o, e->addr_slow_read);
        i_bra_mark(o, c->back2);
        break;
      }
      case COLD_WRITE_OTHER:
      {
        /* a0 = &dram[addr], a1 = &pmac[1][reg], d0 = value,
         * d1 = mode:addr, d2.w = mode */
        struct drc_fix notdram, notiram;
        /* DRAM with get_inc() increment, plain or overwrite:
         * (mode & 0x43ff) == 0x0018 */
        i_move_w_dd(o, 2, 3);
        i_andi_w(o, 0x43ff, 3);
        i_cmpi_w(o, 0x0018, 3);
        notdram = i_bcc_w(o, CC_NE);
        if (e->addr_pmstat) i_addq_l_abs(o, 1, e->addr_pmstat + 16);
        g_pm_inc_d3(t);
        g_pm_store(t, c->c);
        i_add_l_dn_ind_a1(o, 3);
        i_move_l_ind_a1_d16(o, e->off_gr + 4 * 14, R_S);    /* rPMC.v */
        i_bra_mark(o, c->back);
        drc_here(o, notdram);
        /* IRAM: (mode & 0x47ff) == 0x001c */
        i_move_w_dd(o, 2, 3);
        i_andi_w(o, 0x47ff, 3);
        i_cmpi_w(o, 0x001c, 3);
        notiram = i_bcc_w(o, CC_NE);
        if (e->addr_pmstat) i_addq_l_abs(o, 1, e->addr_pmstat + 32);
        i_moveq(o, 0, 3);                     /* whole register: the verify log gets all of it */
        i_move_w_dd(o, 1, 3);
        i_andi_w(o, 0x3ff, 3);
        i_add_w_dd(o, 3, 3);                  /* byte offset in IRAM */
        if (e->addr_vlog_iram)
          i_move_w_idx_dn(o, R_BASE, 3, 1);   /* old value, for the log */
        i_move_w_dn_idx(o, 0, R_BASE, 3);     /* iram_rom[addr & 0x3ff] = d */
        if (e->addr_vlog_iram)
        {
          i_push_dn(o, 0);
          i_push_dn(o, 1);
          i_push_dn(o, 3);
          i_jsr_abs(o, e->addr_vlog_iram);
          i_lea_d16_sp_sp(o, 12);
          i_lea_d16_a1(o, e->off_pmac + 4 * (6 + c->c), R_S);
        }
        i_addq_l_abs(o, 1, e->addr_gen);      /* ssp_iram_gen++ */
        i_st_abs(o, e->addr_flag);            /* code may have changed: leave after it */
        g_pm_inc_d3(t);
        i_add_l_dn_ind_a1(o, 3);
        i_move_l_ind_a1_d16(o, e->off_gr + 4 * 14, R_S);
        i_bra_mark(o, c->back);
        drc_here(o, notiram);
        /* any other mode: pm_io writes nothing, only rPMC.v = pmac */
        if (e->addr_pmstat) i_addq_l_abs(o, 1, e->addr_pmstat + 36);
        i_move_l_ind_a1_d16(o, e->off_gr + 4 * 14, R_S);
        i_bra_mark(o, c->back);
        break;
      }
      case COLD_PMC_PENDING:
      {
        /* d1.w = emu_status & 3, not 0. SSP_PMC_SET (2): if the access is
         * blind, pmac[write][reg] = rPMC.v; clear SET; the access returns
         * 0 (a read gives 0, a write does nothing). Else only
         * SSP_PMC_HAVE_ADDR (1): clear it and continue normally. */
        struct drc_fix addr_only;
        i_btst_imm(o, 1, 1);
        addr_only = i_bcc_w(o, CC_EQ);
        if (e->addr_pmstat) i_addq_l_abs(o, 1, e->addr_pmstat + (c->b ? 28 : 12));
        if (c->a)
          i_move_l_d16_d16(o, e->off_gr + 4 * 14, e->off_pmac + 4 * (6 * c->b + c->c), R_S);
        i_andi_w_d16(o, 0xfffd, e->off_emust + 2, R_S);
        if (!c->b)
        {
          i_moveq(o, 0, 0);
          if (c->u)                           /* read_PM4: 0 at an idle loop */
          {
            i_ori_w_d16(o, (int)c->u, e->off_emust + 2, R_S);
            i_st_abs(o, e->addr_flag);
          }
        }
        i_bra_mark(o, c->back2);
        drc_here(o, addr_only);
        i_andi_w_d16(o, 0xfffe, e->off_emust + 2, R_S);
        i_bra_mark(o, c->back);
        break;
      }
      case COLD_IDLE4:
        i_ori_w_d16(o, c->a, e->off_emust + 2, R_S);
        i_st_abs(o, e->addr_flag);
        i_bra_mark(o, c->back);
        break;
    }
  }
  t->ncold = 0;
}

/* ------------------------------------------------------------------ */
/* region translation */

/* program words an instruction takes */
static int drc_ins_len(drc_u32 op)
{
  switch (op >> 9)
  {
    case 0x04: case 0x06: case 0x24: case 0x26:
    case 0x14: case 0x34: case 0x44: case 0x54: case 0x64: case 0x74:
      return 2;
  }
  return 1;
}

/* register an instruction writes through REG_WRITE, or -1 */
static int drc_dst_reg(drc_u32 op)
{
  switch (op >> 9)
  {
    case 0x00:
      if (op == 0 || op == ((SSPR_A << 4) | SSPR_P)) return -1;
      return (int)((op >> 4) & 15);
    case 0x01: case 0x04: case 0x05: case 0x09: case 0x25:
      return (int)((op >> 4) & 15);
  }
  return -1;
}

static int drc_is_branch(drc_u32 op)       /* call / bra */
{
  return (op >> 9) == 0x24 || (op >> 9) == 0x26;
}

static int drc_branch_cond(drc_u32 op)     /* 1 always, 2 on a flag, 0 never */
{
  switch (op & 0xf0) { case 0x00: return 1; case 0x50: case 0x70: return 2; }
  return 0;
}

/* control never continues after it */
static int drc_is_uncond(drc_u32 op)
{
  if (drc_is_branch(op) && drc_branch_cond(op) == 1) return 1;
  return drc_dst_reg(op) == SSPR_PC;
}

static void i_cmpi_l(struct drc_out *o, drc_u32 imm, int dn)
{ T("cmpi.l #%u,%%d%d", imm, dn); drc_w(o, 0x0c80 | dn); drc_l(o, imm); }

/* Bcc.W to a region label (text label id given), patched in drc_translate */
static struct drc_fix i_bcc_lab(struct drc_out *o, int cc, int label)
{
  static const char *const nm[16] = { "bra", "bsr", "bhi", "bls", "bcc", "bcs", "bne", "beq",
                                      "bvc", "bvs", "bpl", "bmi", "bge", "blt", "bgt", "ble" };
  struct drc_fix f;
  f.pos = o->pos;
  f.label = label;
  T("%s.w .L%d", nm[cc], label);
  (void)nm;
  drc_w(o, 0x6000 | (cc << 8));
  drc_w(o, 0);
  return f;
}

static void drc_patch(struct drc_out *o, struct drc_fix f, int target)
{
  int disp = target - (f.pos + 2);
  if (!o->overflow && f.pos + 4 <= o->cap)
  {
    o->buf[f.pos + 2] = (unsigned char)(disp >> 8);
    o->buf[f.pos + 3] = (unsigned char)disp;
  }
}

/* One DSP instruction that is not a call/bra (those are handled by the
 * region loop). t->k = instructions since the last cycle accounting. */
static drc_u32 g_insn(struct drc_tr *t, drc_u32 pcw)
{
  const struct drc_env *e = t->e;
  struct drc_out *o = t->o;
  const drc_u16 *code = t->code;
  drc_u32 op = code[pcw];
  drc_u32 next = pcw + 1;
  struct drc_fix skip = { 0, 0 };   /* set by g_cond() when it returns 2 */
  int c;

  t->handler = 0;
  t->ends = 0;
  t->pc_after = next;
  t->pcw = pcw;

  switch (op >> 9)
  {
    case 0x00:  /* ld d, s */
      if (op == 0) break;
      if (op == ((SSPR_A << 4) | SSPR_P)) { g_read_p(t); i_move_l_dd(o, 0, R_A); break; }
      g_reg_read(t, (int)(op & 0x0f), op);
      g_reg_write(t, (int)((op & 0xf0) >> 4));
      break;
    case 0x01:  /* ld d, (ri) */
      g_ptr1_read(t, PTR1T(op));
      g_reg_write(t, (int)((op & 0xf0) >> 4));
      break;
    case 0x02:  /* ld (ri), s */
      g_reg_read(t, (int)((op & 0xf0) >> 4), op);
      g_ptr1_write(t, op);
      break;
    case 0x03:  /* ld a, adr */
      i_swap(o, R_A); i_move_w_d16_dn(o, 2 * (int)(op & 0x1ff), R_S, R_A); i_swap(o, R_A);
      break;
    case 0x04:  /* ldi d, imm */
      next = pcw + 2; t->pc_after = next;
      i_move_w_imm_dn(o, code[pcw + 1], 0);
      g_reg_write(t, (int)((op & 0xf0) >> 4));
      break;
    case 0x05:  /* ld d, ((ri)) */
      g_ptr2_read(t, op);
      g_reg_write(t, (int)((op & 0xf0) >> 4));
      break;
    case 0x06:  /* ldi (ri), imm */
      next = pcw + 2; t->pc_after = next;
      i_move_w_imm_dn(o, code[pcw + 1], 0);
      g_ptr1_write(t, op);
      break;
    case 0x07:  /* ld adr, a */
      i_move_l_dd(o, R_A, 0); i_swap(o, 0);
      i_move_w_dn_d16(o, 0, 2 * (int)(op & 0x1ff), R_S);
      break;
    case 0x09:  /* ld d, ri */
      i_moveq(o, 0, 0);
      i_move_b_d16_dn(o, e->off_ptr + IJ(op), R_S, 0);
      g_reg_write(t, (int)((op & 0xf0) >> 4));
      break;
    case 0x0a:  /* ld ri, s */
      g_reg_read(t, (int)((op & 0xf0) >> 4), op);
      i_move_b_dn_d16(o, 0, e->off_ptr + IJ(op), R_S);
      break;
    case 0x0c: case 0x0d: case 0x0e: case 0x0f:  /* ldi ri, simm */
      i_move_b_imm_d16(o, (int)(op & 0xff), e->off_ptr + (int)((op >> 8) & 7), R_S);
      break;

    case 0x25:  /* ld d, (a): program word at A >> 16 */
      i_move_l_dd(o, R_A, 0); i_clr_w(o, 0); i_swap(o, 0); i_add_l_dd(o, 0, 0);
      i_move_w_idxl_dn(o, R_BASE, 0, 0);
      g_reg_write(t, (int)((op & 0xf0) >> 4));
      break;

    case 0x48:  /* mod cond, op */
      c = g_cond(t, op, &skip);
      if (c)
      {
        struct drc_fix pos;
        switch (op & 7)
        {
          case 2: i_asr_l_1(o, R_A); break;
          case 3: i_add_l_dd(o, R_A, R_A); break;
          case 6: i_neg_l(o, R_A); break;
          case 7:   /* abs: (a ^ s) - s with s = a >> 31, no branch */
            i_move_l_dd(o, R_A, 1); i_add_l_dd(o, 1, 1); i_subx_l_dd(o, 1, 1);
            i_eor_l_dd(o, 1, R_A); i_sub_l_dd(o, 1, R_A);
            (void)pos; break;
          default: break;
        }
        g_flags(t, R_A, FLAG_ZN_CLR);
        if (c == 2) drc_here(o, skip);
      }
      break;

    case 0x1b:  /* mpys */
    case 0x4b:  /* mpya */
    case 0x5b:  /* mld */
      if ((op >> 9) == 0x5b)
      {
        i_moveq(o, 0, R_A);
        i_andi_w(o, 0x0fff, R_ST);
      }
      else
      {
        g_read_p(t);
        if ((op >> 9) == 0x1b) i_sub_l_dd(o, 0, R_A); else i_add_l_dd(o, 0, R_A);
        g_flags(t, R_A, FLAG_ZN_CLR);
      }
      g_ptr1_read(t, (int)((op & 3) | ((op << 1) & 0x18)));
      i_move_w_dn_d16(o, 0, GR_H(t, SSPR_X), R_S);
      g_ptr1_read(t, (int)(((op >> 4) & 3) | 4 | ((op >> 3) & 0x18)));
      i_move_w_dn_d16(o, 0, GR_H(t, SSPR_Y), R_S);
      break;

    /* OP a, s */
    case 0x10: case 0x30: case 0x40: case 0x50: case 0x60: case 0x70:
    {
      int alu = (int)(op >> 13);
      if ((op & 0x0f) == SSPR_P) { g_read_p(t); g_alu32_d0(t, alu); break; }
      if ((op & 0x0f) == SSPR_A) { g_alu_aa(t, alu); break; }
      g_reg_read(t, (int)(op & 0x0f), op);
      g_alu16_d0(t, alu);
      break;
    }
    /* OP a, (ri) */
    case 0x11: case 0x31: case 0x41: case 0x51: case 0x61: case 0x71:
      g_ptr1_read(t, PTR1T(op));
      g_alu16_d0(t, (int)(op >> 13));
      break;
    /* OP a, adr */
    case 0x13: case 0x33: case 0x43: case 0x53: case 0x63: case 0x73:
      i_move_w_d16_dn(o, 2 * (int)(op & 0x1ff), R_S, 0);
      g_alu16_d0(t, (int)(op >> 13));
      break;
    /* OP a, imm */
    case 0x14: case 0x34: case 0x44: case 0x54: case 0x64: case 0x74:
      next = pcw + 2; t->pc_after = next;
      g_alu16_imm(t, (int)(op >> 13), code[pcw + 1]);
      break;
    /* OP a, ((ri)) */
    case 0x15: case 0x35: case 0x45: case 0x55: case 0x65: case 0x75:
      g_ptr2_read(t, op);
      g_alu16_d0(t, (int)(op >> 13));
      break;
    /* OP a, ri */
    case 0x19: case 0x39: case 0x49: case 0x59: case 0x69: case 0x79:
      i_moveq(o, 0, 0);
      i_move_b_d16_dn(o, e->off_ptr + IJ(op), R_S, 0);
      g_alu16_d0(t, (int)(op >> 13));
      break;
    /* OP simm */
    case 0x1c: case 0x3c: case 0x4c: case 0x5c: case 0x6c: case 0x7c:
      g_alu16_imm(t, (int)(op >> 13), op & 0xff);
      break;

    default:    /* unhandled: no effect in the reference */
      break;
  }

  if (!t->ends && t->handler)
    g_after_handler(t, next);
  return next;
}

/* Shared slow paths for the C register handlers, generated once after the
 * dispatcher. Call: d2 = register, d1 = k (instructions since the last
 * cycle accounting), a1 = what the global PC must hold; slow_write also
 * d0 = value. They do what g_sync_out/g_sync_in did inline, and set the
 * exit flag when the instruction must be the block's last (the DSP now
 * waits, or IRAM was written). slow_read returns the value in d0. */
static void drc_emit_stubs(const struct drc_env *e, struct drc_out *o,
                           int *pos_read, int *pos_write)
{
  struct drc_fix to_tail, set, done, tail_fix;
  int tail;
  int a_v = e->off_gr + 4 * SSPR_A, st_h = e->off_gr + 4 * SSPR_ST;

  /* slow_write */
  *pos_write = o->pos;
  i_andi_l(o, 0xffff, 0);
  i_move_l_dn_d16(o, R_A, a_v, R_S);
  i_move_w_dn_d16(o, R_ST, st_h, R_S);
  i_move_l_a1_abs(o, e->addr_PC);
  i_move_l_dd(o, R_CYC, 3);
  i_sub_l_dd(o, 1, 3);
  i_move_l_dn_abs(o, 3, e->addr_g_cycles);
  i_push_dn(o, 1);
  i_push_dn(o, 0);
  i_push_dn(o, 2);
  i_jsr_abs(o, e->addr_hwrite);
  i_addq_l_sp(o, 8);
  i_move_l_sp_postinc_dn(o, 1);
  to_tail = i_bcc_w(o, CC_T);

  /* slow_read */
  *pos_read = o->pos;
  i_move_l_dn_d16(o, R_A, a_v, R_S);
  i_move_w_dn_d16(o, R_ST, st_h, R_S);
  i_move_l_a1_abs(o, e->addr_PC);
  i_move_l_dd(o, R_CYC, 3);
  i_sub_l_dd(o, 1, 3);
  i_move_l_dn_abs(o, 3, e->addr_g_cycles);
  i_push_dn(o, 1);
  i_push_dn(o, 2);
  i_jsr_abs(o, e->addr_hread);
  i_addq_l_sp(o, 4);
  i_move_l_sp_postinc_dn(o, 1);

  /* common tail: take the state back, then the exit checks */
  tail = o->pos;
  drc_here(o, to_tail);
  (void)tail; (void)tail_fix;
  i_move_l_d16_dn(o, a_v, R_S, R_A);
  i_moveq(o, 0, R_ST);
  i_move_w_d16_dn(o, st_h, R_S, R_ST);
  i_move_l_abs_dn(o, e->addr_g_cycles, R_CYC);
  i_add_l_dd(o, 1, R_CYC);
  i_move_w_d16_dn(o, e->off_emust + 2, R_S, 1);
  i_andi_w(o, 0xf000, 1);
  set = i_bcc_w(o, CC_NE);
  i_cmp_l_abs_dn(o, e->addr_gen, R_GEN);
  done = i_bcc_w(o, CC_EQ);
  drc_here(o, set);
  i_st_abs(o, e->addr_flag);
  drc_here(o, done);
  i_rts(o);
}

/* The block dispatcher, generated once at the start of the code buffer.
 * Every block exit jumps here with the next DSP PC in d0.w and the cycles
 * left in d5. It goes straight on to the next translated block when the
 * reference interpreter would continue (cycles left, no idle wait), the
 * block exists, an IRAM block was checked at the current IRAM generation,
 * and enough cycles remain for its first segment; otherwise it returns to
 * the C dispatcher (drc_enter's caller) with the PC in addr_next_pc. */
static void drc_emit_dispatch(const struct drc_env *e, struct drc_out *o)
{
  struct drc_fix f_cyc, f_wait, f_none, f_rom, f_gen, f_short;
  i_andi_l(o, 0xffff, 0);
  if (!e->no_chain)
  {
    i_tst_l(o, R_CYC);
    f_cyc = i_bcc_w(o, 15);                 /* ble: cycles used up */
    i_move_w_d16_dn(o, e->off_emust + 2, R_S, 1);
    i_andi_w(o, 0xf000, 1);
    f_wait = i_bcc_w(o, CC_NE);             /* the DSP waits */
    i_cmpi_l(o, (drc_u32)e->min_cycles, R_CYC);
    f_short = i_bcc_w(o, 13);               /* blt: C finishes the run */
    i_move_l_dd(o, 0, 1);
    i_add_l_dd(o, 1, 1);
    i_add_l_dd(o, 1, 1);                    /* d1 = pc * 4 */
    i_movea_l_imm_a0(o, e->addr_map);
    i_movea_l_idx_a0(o);                    /* first translation of pc */
    i_move_l_a0_dn(o, 1);
    f_none = i_bcc_w(o, CC_EQ);             /* not translated yet */
    i_move_l_abs_dn(o, e->addr_gen, R_GEN); /* d4 = current IRAM generation */
    i_cmpi_w(o, 0x400, 0);
    f_rom = i_bcc_w(o, 4);                  /* bcc: program ROM, no check */
    i_cmp_l_d16a0_dn(o, e->off_b_gen, R_GEN);
    f_gen = i_bcc_w(o, CC_NE);              /* IRAM may have changed: C checks */
    drc_here(o, f_rom);
    i_moveq(o, 0, 1);
    i_move_w_d16a0_dn(o, e->off_b_nins, 1);
    i_cmp_l_dd(o, 1, R_CYC);
    {
      struct drc_fix f_few = i_bcc_w(o, 13);  /* blt: first segment does not fit */
      struct drc_fix *why[6], to_out[6];
      int r;
      i_addq_l_abs(o, 1, e->addr_runs);
      i_movea_l_d16a0_a0(o, e->off_b_code);
      i_jmp_a0(o);
      why[0] = &f_cyc; why[1] = &f_wait; why[2] = &f_short;
      why[3] = &f_none; why[4] = &f_gen; why[5] = &f_few;
      for (r = 0; r < 6; r++)       /* count why the chain stopped */
      {
        drc_here(o, *why[r]);
        i_addq_l_abs(o, 1, e->addr_runs + 4 * (r + 1));
        to_out[r] = i_bcc_w(o, CC_T);
      }
      for (r = 0; r < 6; r++)
        drc_here(o, to_out[r]);
    }
  }
  i_move_w_dn_abs(o, 0, e->addr_next_pc);
  i_rts(o);
}

/* Translate the region starting at DSP word address start: up to
 * DRC_MAX_INS instructions in program order, ending after an
 * unconditional transfer, at limit (0x400 for IRAM code) or at the
 * instruction cap. A call/bra whose target is an instruction of the region
 * becomes a jump inside the generated code.
 *
 * Cycles: the interpreter stops as soon as its counter reaches 0 after an
 * instruction. The region is cut into segments that start at the region
 * start, at every branch target inside it and after every conditional
 * branch; a branch is always the last instruction of its segment. d5 is
 * exact at each segment start, which checks that the whole segment fits
 * (d5 >= its length) and otherwise leaves for the dispatcher, which then
 * finishes the run with the interpreter.
 *
 * Returns the length of the first segment (what the dispatcher must have
 * cycles for), 0 on buffer overflow; *nwords = program words used. */
static int drc_translate(const struct drc_env *e, struct drc_out *o,
                         const drc_u16 *code, drc_u32 start, drc_u32 limit,
                         int *nwords)
{
  struct drc_tr tr, *t = &tr;
  drc_u32 addr[DRC_MAX_INS + 1];
  unsigned char seg[DRC_MAX_INS + 1];
  int seglen[DRC_MAX_INS];
  int label_pos[DRC_MAX_INS];
  struct drc_fix fix[DRC_MAX_INS];
  int fix_to[DRC_MAX_INS];
  int nfix = 0, n = 0, i, j, k, done = 0, lab0 = 0;
  drc_u32 a = start;

  t->e = e; t->o = o; t->code = code;
  t->cold = drc_cold_buf; t->ncold = 0;

  /* 1. the instructions of the region */
  for (;;)
  {
    drc_u32 op = code[a];
    addr[n++] = a;
    a += (drc_u32)drc_ins_len(op);
    if (drc_is_uncond(op)) break;
    /* A block ends at its first call/bra, even one whose condition is
     * never true here (only Z and N are emulated). Virtua Racing loads
     * IRAM with ever-new combinations of overlay routines, and blocks
     * reaching past a branch into the next routine were rarely found
     * again (measured: 64-instruction regions 450,000 translations,
     * blocks continuing past never-taken branches 268,000, blocks
     * ending at every branch 3,000). Branches back into the block
     * still stay inside the generated code.
     * Exception: a conditional bra jumping at most DRC_SKIP_MAX words
     * forward (an if-then skip) - the skipped words and the target are
     * code of the same routine, and both outcomes stay inside the block
     * (measured: not-taken and forward-taken exits were over half of all
     * block exits). Unconditional jumps, calls and returns still end it:
     * what follows them may be data or another routine. */
    if (drc_is_branch(op))
    {
      drc_u32 at = addr[n - 1], tgt = code[at + 1];
      if (!((op >> 9) == 0x26 && drc_branch_cond(op) == 2 &&
            tgt > at && tgt - at <= DRC_SKIP_MAX))
        break;
    }
    if (n >= DRC_MAX_INS || a >= limit || a > 0xffff) break;
  }
  addr[n] = a;

  /* 2. segment starts */
  for (i = 0; i <= n; i++) seg[i] = 0;
  seg[0] = 1;
  for (i = 0; i < n; i++)
  {
    drc_u32 op = code[addr[i]];
    if (!drc_is_branch(op) || drc_branch_cond(op) == 0) continue;
    if (drc_branch_cond(op) == 2 && i + 1 < n) seg[i + 1] = 1;
    for (j = 0; j < n; j++)
      if (addr[j] == code[addr[i] + 1]) { seg[j] = 1; break; }
  }
  for (i = 0; i < n; i++)
  {
    if (!seg[i]) continue;
    for (j = i + 1; j < n && !seg[j]; j++) ;
    seglen[i] = j - i;
  }

#ifdef DRC_TEXT
  lab0 = o->next_label;
  o->next_label += n;
#endif

  /* 3. code */
  k = 0;
  for (i = 0; i < n && !done; i++)
  {
    drc_u32 pcw = addr[i];
    drc_u32 op = code[pcw];

    if (seg[i])
    {
      struct drc_fix ok;
      g_sub_cyc(t, k);          /* fall-through from the previous segment */
      k = 0;
      label_pos[i] = o->pos;
#ifdef DRC_TEXT
      o->label(o->pos, lab0 + i);
#endif
      i_cmpi_l(o, (drc_u32)seglen[i], R_CYC);
      g_cold_exit(t, 13, pcw, 0, 0);    /* blt: the segment does not fit */
      (void)ok;
    }
    t->k = k;

    if (drc_is_branch(op) && drc_branch_cond(op) != 0)
    {
      drc_u32 next = pcw + 2, target = code[pcw + 1];
      struct drc_fix skip = { 0, 0 };   /* set by g_cond() when it returns 2 */
      int c, tj = -1;
      t->pc_after = next;
      for (j = 0; j < n; j++)
        if (addr[j] == target) { tj = j; break; }
      c = g_cond(t, op, &skip);
      /* taken */
      if ((op >> 9) == 0x24)
      {
        i_move_w_imm_dn(o, (int)next, 0);
        g_push_stack(t);
      }
      if (tj >= 0)
      {
        g_stat(t, 4);
        g_sub_cyc(t, k + 2);    /* write_PC takes one more cycle */
        fix[nfix] = i_bcc_lab(o, CC_T, lab0 + tj);
        fix_to[nfix++] = tj;
      }
      else
      {
        g_stat(t, (op >> 9) == 0x24 ? 3 : target < pcw ? 2 : 1);
        g_exit(t, target, k + 2);
      }
      if (c == 1)
      {
        done = 1;               /* unconditional: the region ends here */
        break;
      }
      /* not taken */
      drc_here(o, skip);
      if (i == n - 1)
      {
        g_stat(t, 5);
        g_exit(t, next, k + 1);
        done = 1;
        break;
      }
      g_sub_cyc(t, k + 1);
      k = 0;                    /* next instruction starts a segment */
      continue;
    }

    /* everything else, including a never-taken call/bra (a 2-word no-op) */
    if (drc_is_branch(op))
      ;                         /* g_insn would treat 0x24/0x26 as unknown */
    else
      g_insn(t, pcw);
    if (t->ends && !drc_is_branch(op))
    {
      done = 1;                 /* PC written: exit emitted */
      break;
    }
    k++;
  }
  if (!done)
  {
    g_stat(t, 0);
    g_exit(t, a, k);
  }
  drc_cold_emit(t);

  for (i = 0; i < nfix; i++)
    drc_patch(o, fix[i], label_pos[fix_to[i]]);

  *nwords = (int)(a - start);
  return o->overflow ? 0 : seglen[0];
}

#undef T
#endif /* SSP16_DRC_EMIT_H */
