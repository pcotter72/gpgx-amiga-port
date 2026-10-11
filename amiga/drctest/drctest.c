/*
 * drctest.c - host-side encoding test for amiga/ssp16_drc_emit.h
 *
 * Built and run on the development PC (not part of the Amiga build):
 *   gcc -O1 -o drctest drctest.c && ./drctest out.bin out.s
 *   python check_encoding.py out.bin out.s
 *
 * Translates every DSP opcode 0x0000-0xffff as the first instruction of a
 * block and writes two files: the bytes the translator emitted, and the
 * assembly text it meant to emit (with branch labels). check_encoding.py
 * assembles the text with m68k-amigaos-as -m68000 and compares the result
 * byte for byte, so any wrong opcode, register field, extension word or
 * branch displacement shows up with the DSP opcode and the instruction.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DRC_TEXT
#include "../ssp16_drc_emit.h"

static FILE *ftxt;

static void txt(int pos, const char *line)
{
  fprintf(ftxt, "\t%s\t| @%d\n", line, pos);
}

static void lab(int pos, int id)
{
  fprintf(ftxt, ".L%d:\t| @%d\n", id, pos);
}

int main(int argc, char **argv)
{
  static drc_u16 code[0x10001];
  struct drc_env e;
  struct drc_out o;
  FILE *fbin;
  unsigned long op;
  int i, nwords, nwords2, total = 0;
  drc_u32 start2;

  if (argc < 3) { fprintf(stderr, "usage: drctest out.bin out.s\n"); return 1; }

  /* real ssp1601_t offsets (host build of the struct, same as m68k:
   * gr 1024, ptr 1088, stack 1096, emu_status 1156) and dummy addresses */
  e.off_gr = 1024; e.off_ptr = 1088; e.off_stack = 1096; e.off_emust = 1156;
  e.base_addr = 0x08100000u;
  e.addr_PC = 0x08200000u; e.addr_g_cycles = 0x08200004u;
  e.addr_next_pc = 0x08200008u; e.addr_gen = 0x0820000cu;
  e.addr_hread = 0x08300000u; e.addr_hwrite = 0x08300010u; e.addr_ptr2 = 0x08300020u;
  e.addr_dispatch = 0x08400000u; e.addr_map = 0x08500000u; e.addr_runs = 0x08200010u;
  e.off_b_code = 4; e.off_b_gen = 8; e.off_b_nins = 12;
  e.min_cycles = 32; e.no_chain = 0; e.count_chain = 1;
  e.off_pmac = 1108; e.addr_dram = 0x08600000u; e.addr_rom = 0x08700000u;
  e.addr_inctab = 0x08800000u; e.addr_vlog = 0x08300030u;
  e.addr_slow_read = 0x08400100u; e.addr_slow_write = 0x08400180u;
  e.addr_flag = 0x08200020u; e.addr_masktab = 0x08900000u; e.addr_pmstat = 0x08200040u;
  e.addr_vlog_iram = 0x08300040u; e.addr_exitstat = 0x08200080u;

  o.cap = 64 * 1024 * 1024;
  o.buf = (unsigned char *)malloc(o.cap);
  o.pos = 0; o.overflow = 0;
  o.addr = 0x08400000u;     /* = addr_dispatch: early blocks bra.w, later jmp */
  o.text = txt; o.label = lab; o.next_label = 0;
  if (!o.buf) return 1;

  ftxt = fopen(argv[2], "w");
  if (!ftxt) return 1;
  fprintf(ftxt, "\t.text\n");

  /* every other word: "bra always" (0x4c00) so a block ends right after
   * the opcode under test; its immediate/target word is 0x4c00 too */
  for (i = 0; i <= 0x10000; i++) code[i] = 0x4c00;

  /* the dispatcher, chaining and verify (no_chain) forms */
  fprintf(ftxt, "| op dispatch\n");
  drc_emit_dispatch(&e, &o);
  e.no_chain = 1;
  fprintf(ftxt, "| op dispatch_nochain\n");
  drc_emit_dispatch(&e, &o);
  e.no_chain = 0;
  {
    int pr, pw;
    fprintf(ftxt, "| op stubs\n");
    drc_emit_stubs(&e, &o, &pr, &pw);
  }

  for (op = 0; op <= 0xffff; op++)
  {
    code[0x500] = (drc_u16)op;
    code[0x501] = (drc_u16)(op ^ 0x5a5a);     /* immediate, varies */
    fprintf(ftxt, "| op %04lx\n", op);
    if (!drc_translate(&e, &o, code, 0x500, 0x10000, &nwords, DRC_MAX_INS, &start2, &nwords2) && o.overflow)
    {
      fprintf(stderr, "buffer overflow at op %04lx\n", op);
      return 1;
    }
    total++;
  }
  /* the same in IRAM (word address < 0x400) */
  for (op = 0; op <= 0xffff; op++)
  {
    code[0x100] = (drc_u16)op;
    code[0x101] = (drc_u16)(op ^ 0x5a5a);
    fprintf(ftxt, "| op iram%04lx\n", op);
    if (!drc_translate(&e, &o, code, 0x100, 0x400, &nwords, DRC_MAX_INS, &start2, &nwords2) && o.overflow)
    {
      fprintf(stderr, "buffer overflow at IRAM op %04lx\n", op);
      return 1;
    }
    total++;
  }
  /* regions with branches into themselves: random programs of 4-20
   * instructions where every call/bra targets one of their own
   * instructions (loops backwards, skips forwards, calls), followed by an
   * unconditional bra out */
  {
    static const drc_u16 cls[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x07, 0x09, 0x0a, 0x0c,
                                   0x10, 0x11, 0x13, 0x14, 0x19, 0x1b, 0x1c, 0x25, 0x30,
                                   0x31, 0x34, 0x40, 0x48, 0x4b, 0x4c, 0x50, 0x5b, 0x5c,
                                   0x60, 0x70, 0x24, 0x26, 0x26, 0x26 };
    unsigned long seed = 12345;
    int prog;
#define RND() (seed = seed * 1103515245UL + 12345UL, (unsigned)(seed >> 16) & 0x7fff)
    for (prog = 0; prog < 3000; prog++)
    {
      drc_u32 at[24];
      int ni = 4 + (int)(RND() % 17), m;
      drc_u32 a = 0x700;
      for (i = 0; i < 0x80; i++) code[0x700 + i] = 0x4c00;
      for (m = 0; m < ni; m++)
      {
        drc_u16 c = cls[RND() % (sizeof(cls) / sizeof(cls[0]))];
        drc_u16 w = (drc_u16)((c << 9) | (RND() & 0x1ff));
        if (c == 0x24 || c == 0x26)
        {
          static const drc_u16 cc[4] = { 0x000, 0x150, 0x070, 0x0f0 };
          w = (drc_u16)((c << 9) | cc[RND() & 3]);
        }
        /* keep register moves away from PC (6) so the region continues */
        if ((c == 0x00 || c == 0x01 || c == 0x04 || c == 0x09 || c == 0x25) &&
            ((w >> 4) & 15) == 6)
          w ^= 0x0010;
        at[m] = a;
        code[a] = w;
        a += (c == 0x04 || c == 0x14 || c == 0x34 || c == 0x24 || c == 0x26) ? 2 : 1;
      }
      code[a] = 0x4c00; code[a + 1] = 0x0400;    /* bra always 0x400 (outside) */
      for (m = 0; m < ni; m++)                    /* branch targets inside */
      {
        drc_u16 c = (drc_u16)(code[at[m]] >> 9);
        if (c == 0x24 || c == 0x26)
          code[at[m] + 1] = (drc_u16)at[RND() % ni];
        else if (c == 0x04 || c == 0x14 || c == 0x34)
          code[at[m] + 1] = (drc_u16)RND();
      }
      fprintf(ftxt, "| op prog%d\n", prog);
      if (!drc_translate(&e, &o, code, 0x700, 0x10000, &nwords, DRC_MAX_INS, &start2, &nwords2) && o.overflow)
      {
        fprintf(stderr, "buffer overflow in program %d\n", prog);
        return 1;
      }
      total++;
    }
  }
  /* inlined calls: caller at 0x900, leaf routines elsewhere.
   * 0x0013 ld X,A / 0x0031 ld A,X / 0x0065 ld PC,STACK (ret) /
   * 0x4800 call always / 0x4d50 bra Z / 0x4c00 bra always / 0x9801 add #1 */
  {
    static const drc_u16 caller[] = { 0x0031, 0x4800, 0x0960, 0x0013, 0x4c00, 0x0400 };
    static const drc_u16 leaf1[] = { 0x9801, 0x0013, 0x0065 };                       /* straight */
    static const drc_u16 leaf2[] = { 0x9801, 0x4d50, 0x0985, 0x0013, 0x0065 };       /* with a skip */
    int c, w;
    for (c = 0; c < 4; c++)
    {
      for (i = 0x900; i < 0xa00; i++) code[i] = 0x4c00;
      for (w = 0; w < 6; w++) code[0x900 + w] = caller[w];
      if (c == 0) for (w = 0; w < 3; w++) code[0x960 + w] = leaf1[w];
      if (c == 1) for (w = 0; w < 5; w++) code[0x960 + w] = leaf2[w];
      if (c == 2)
      {
        /* a long leaf: the block ends inside it */
        for (w = 0; w < 37; w++) code[0x960 + w] = 0x9801;
        code[0x960 + 37] = 0x0065;
      }
      if (c == 3)
      {
        /* callee overlapping the caller: no inlining */
        code[0x902] = 0x0903;
        code[0x903] = 0x0013;
        code[0x904] = 0x0065;
      }
      fprintf(ftxt, "| op call%d\n", c);
      if (!drc_translate(&e, &o, code, 0x900, 0x10000, &nwords, DRC_MAX_INS, &start2, &nwords2) && o.overflow)
      {
        fprintf(stderr, "buffer overflow in call test %d\n", c);
        return 1;
      }
      printf("call test %d: words %d, callee range %04lx +%d\n", c, nwords,
             (unsigned long)start2, nwords2);
      total++;
    }
    /* in IRAM, with a PM4 write (0x00c3 ld PM4,A) in the caller and in
     * the callee */
    {
      static const drc_u16 icaller[] = { 0x00c3, 0x4800, 0x0260, 0x0013, 0x4c00, 0x0400 };
      static const drc_u16 ileaf[] = { 0x00c3, 0x0065 };
      for (i = 0x200; i < 0x300; i++) code[i] = 0x4c00;
      for (w = 0; w < 6; w++) code[0x200 + w] = icaller[w];
      for (w = 0; w < 2; w++) code[0x260 + w] = ileaf[w];
      fprintf(ftxt, "| op icall\n");
      if (!drc_translate(&e, &o, code, 0x200, 0x400, &nwords, DRC_MAX_INS, &start2, &nwords2) && o.overflow)
      {
        fprintf(stderr, "buffer overflow in IRAM call test\n");
        return 1;
      }
      printf("IRAM call test: words %d, callee range %04lx +%d\n", nwords,
             (unsigned long)start2, nwords2);
      total++;
    }
  }
  fclose(ftxt);

  fbin = fopen(argv[1], "wb");
  if (!fbin) return 1;
  fwrite(o.buf, 1, o.pos, fbin);
  fclose(fbin);
  printf("%d blocks, %d bytes\n", total, o.pos);
  return 0;
}
