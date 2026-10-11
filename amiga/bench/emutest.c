/*
 * emutest.c - checks instruction forms that appeared in Musashi when the
 * M68K_RARE_HINTS test switch was on (see core/m68k/m68kcpu.h); that build
 * lost the display in Sonic 3 on Emu68. Each case runs the form with known
 * values and compares with the result the 68000 PRM defines.
 *
 * Build (from this directory):
 *   m68k-amigaos-gcc -m68040 -noixemul -O2 -Wall -c emutest.c
 *   m68k-amigaos-gcc -m68040 -c emutest_asm.s
 *   m68k-amigaos-gcc -m68040 -noixemul -o emutest emutest.o emutest_asm.o
 * Run:  emutest >emutest.log
 */
#include <stdio.h>
#include <exec/types.h>

extern ULONG t_lea_an_w(ULONG base, ULONG index);
extern ULONG t_bra16(ULONG *st);
extern ULONG t_pea_an_w(ULONG base, ULONG index);
extern ULONG t_cmpa(ULONG *mem, ULONG an);
extern ULONG t_move_w_an(UWORD *buf, ULONG value, ULONG offset);
extern ULONG t_moveb_after_muls(UBYTE *buf, ULONG index);
extern ULONG t_moveb_after_moveq(UBYTE *buf);
extern ULONG t_moveb_after_moveq_neg(UBYTE *buf);
extern ULONG t_add8_flow(UBYTE *buf, ULONG index, ULONG d);

static int fails;

static void check(const char *what, ULONG got, ULONG want)
{
  printf("%-46s got %08lx want %08lx  %s\n", what, (unsigned long)got,
         (unsigned long)want, got == want ? "ok" : "WRONG");
  if (got != want)
    fails++;
}

int main(void)
{
  static UWORD buf[64];
  ULONG st[3], mem[2];
  int i;

  /* lea (0,a1,a2.w),a1: index = sign-extended low word of a2 */
  check("lea (0,a1,a2.w) index +0x0010", t_lea_an_w(0x00200000, 0x00000010), 0x00200010);
  check("lea (0,a1,a2.w) index -0x0010 (0000fff0)", t_lea_an_w(0x00200000, 0x0000fff0), 0x001ffff0);
  check("lea (0,a1,a2.w) index -2 (1234fffe)", t_lea_an_w(0x00200000, 0x1234fffe), 0x001ffffe);

  /* the bra.w fast path: new PC = PC + displacement (PC = extension word) */
  for (i = 0; i < 2; i++)
  {
    st[0] = 0x00001000; st[1] = 0x00001000; st[2] = i ? 0x0000fff0 : 0x00000020;
    check(i ? "bra_16 fast path, displacement -16" : "bra_16 fast path, displacement +32",
          t_bra16(st), i ? 0x00000ff0 : 0x00001020);
    check("  PC and prefetch address advanced", st[0] == 0x1002 && st[1] == 0x1002, 1);
  }

  /* pea (0,a1,a0.w) */
  check("pea (0,a1,a0.w) index +0x0010", t_pea_an_w(0x00200000, 0x00000010), 0x00200010);
  check("pea (0,a1,a0.w) index -0x0010 (0000fff0)", t_pea_an_w(0x00200000, 0x0000fff0), 0x001ffff0);

  /* cmpa.l 4(a0),a1 */
  mem[0] = 0; mem[1] = 0x00123456;
  check("cmpa.l 4(a0),a1 equal -> seq ff", t_cmpa(mem, 0x00123456), 0xff);
  check("cmpa.l 4(a0),a1 not equal -> seq 00", t_cmpa(mem, 0x00123458), 0x00);

  /* move.w a0,(0,a1,d0.l) */
  check("move.w a0,(0,a1,d0.l)", t_move_w_an(buf, 0x12345678, 6), 0x5678);
  check("move.w a0,(0,a1,d0.l) other value", t_move_w_an(buf, 0xabcd8001, 10), 0x8001);

  /* byte loads into a register set by moveq (M68K_RARE_RD code shape) */
  {
    static UBYTE bytes[16] = { 0x00, 0x7f, 0x80, 0xff, 0x01, 0xc3, 0x3c, 0xaa,
                               0x55, 0xfe, 0x10, 0x20, 0x40, 0x08, 0x04, 0x02 };
    for (i = 0; i < 4; i++)
    {
      char name[64];
      sprintf(name, "moveq #20/muls/move.b, byte %02x", bytes[i + 1]);
      check(name, t_moveb_after_muls(bytes, (ULONG)(i + 1)), bytes[i + 1]);
    }
    check("moveq #20,d0 / move.b (a0),d0, byte c3", t_moveb_after_moveq(&bytes[5]), 0xc3);
    check("moveq #-1,d0 / move.b (a0),d0, byte 3c", t_moveb_after_moveq_neg(&bytes[6]), 0xffffff3c);
    check("add.b flow: ff + 01 -> 0x100 (carry)", t_add8_flow(bytes, 3, 0x01), 0x100);
    check("add.b flow: 80 + 80 -> 0x100 (carry)", t_add8_flow(bytes, 2, 0x80), 0x100);
    check("add.b flow: 7f + 01 -> 0x080", t_add8_flow(bytes, 1, 0x01), 0x080);
  }

  printf("emutest: %d wrong\n", fails);
  return fails ? 5 : 0;
}
