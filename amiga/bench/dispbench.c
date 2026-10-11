/*
 * dispbench.c - what a CPU interpreter's instruction dispatch costs on Emu68
 *
 * Build (from this directory; compile and link in separate steps - the
 * one-line form failed to find main with this toolchain):
 *   m68k-amigaos-gcc -m68040 -mhard-float -noixemul -O2 -Wall -c dispbench.c
 *   m68k-amigaos-gcc -m68040 -c dispbench_asm.s
 *   m68k-amigaos-gcc -m68040 -mhard-float -noixemul -o dispbench dispbench.o dispbench_asm.o
 * Run on the Amiga (Emu68 only: it reads Emu68 control registers):
 *   dispbench >dispbench.log
 *
 * Musashi (68000) and the Z80 core run each emulated instruction through a
 * jump-table call. In profiles they took 4.0-5.4 ns per host instruction,
 * against ~1.3 ns for straight-line code (VDP rendering). This measures the
 * dispatch alone: a direct bsr, a table jsr/rts, and a threaded jmp, each
 * with one target and with 256 targets in a pseudo-random order.
 */
#include <stdio.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <inline/macros.h>

extern ULONG bench_direct(ULONG n);
extern ULONG bench_table(ULONG n, const UBYTE *ops, ULONG *table);
extern ULONG bench_threaded(ULONG n, const UBYTE *ops, ULONG *table);
extern void bench_read3(ULONG *out);
extern ULONG bench_freq(void);
extern UBYTE bench_tiny_start[], bench_tiny_end[];
extern UBYTE bench_thr_start[], bench_thr_end[];

/* devicetree.resource (Emu68 ROM): DT_OpenKey(name)(a0) at -6,
 * DT_CloseKey(key)(a0) at -12, as in amiga/ssp16_drc.h */
#define DT_OpenKey(name) \
  LP1(0x06, APTR, DT_OpenKey, CONST_STRPTR, name, a0, , DeviceTreeBase)
#define DT_CloseKey(key) \
  LP1NR(0x0c, DT_CloseKey, APTR, key, a0, , DeviceTreeBase)

static int on_emu68(void)
{
  APTR DeviceTreeBase = OpenResource((CONST_STRPTR)"devicetree.resource");
  APTR key;
  if (!DeviceTreeBase)
    return 0;
  key = DT_OpenKey((CONST_STRPTR)"/emu68");
  if (!key)
    return 0;
  DT_CloseKey(key);
  return 1;
}

#define N 4000000UL

static UBYTE ops_one[4096];             /* all 0: one target */
static UBYTE ops_many[4096];            /* 0-255, pseudo-random */
static ULONG table_tiny[256], table_thr[256];
static ULONG freq;

typedef ULONG (*bench_fn)(ULONG n, const UBYTE *ops, ULONG *table);

static ULONG call_direct(ULONG n, const UBYTE *ops, ULONG *table)
{
  (void)ops; (void)table;
  return bench_direct(n);
}

static void run(const char *name, bench_fn fn, const UBYTE *ops, ULONG *table)
{
  ULONG a[3], b[3], got = 0;
  int pass;
  double ns = 0.0, m68k = 0.0, arm = 0.0;

  for (pass = 0; pass < 2; pass++)      /* the first pass translates the code */
  {
    Forbid();
    bench_read3(a);
    got = fn(N, ops, table);
    bench_read3(b);
    Permit();
    ns = (double)(ULONG)(b[0] - a[0]) * 1e9 / (double)freq / (double)N;
    m68k = (double)(ULONG)(b[1] - a[1]) / (double)N;
    arm = (double)(ULONG)(b[2] - a[2]) / (double)N;
  }
  printf("%-34s %6lu.%02lu ns  %5lu.%02lu m68k  %6lu.%02lu ARM insns per handler%s\n",
         name,
         (unsigned long)ns, (unsigned long)(ns * 100.0 + 0.5) % 100,
         (unsigned long)m68k, (unsigned long)(m68k * 100.0 + 0.5) % 100,
         (unsigned long)arm, (unsigned long)(arm * 100.0 + 0.5) % 100,
         got == N ? "" : "  (WRONG COUNT)");
}

int main(void)
{
  ULONG seed = 12345, tiny, thr;
  int i;

  if (!on_emu68())
  {
    printf("dispbench: needs Emu68 (it reads Emu68 control registers)\n");
    return 10;
  }
  tiny = (ULONG)(bench_tiny_end - bench_tiny_start) / 256;
  thr = (ULONG)(bench_thr_end - bench_thr_start) / 256;
  if (tiny * 256 != (ULONG)(bench_tiny_end - bench_tiny_start) ||
      thr * 256 != (ULONG)(bench_thr_end - bench_thr_start))
  {
    printf("dispbench: handler sizes are not uniform\n");
    return 10;
  }
  for (i = 0; i < 256; i++)
  {
    table_tiny[i] = (ULONG)bench_tiny_start + (ULONG)i * tiny;
    table_thr[i] = (ULONG)bench_thr_start + (ULONG)i * thr;
  }
  for (i = 0; i < 4096; i++)
  {
    seed = seed * 1103515245UL + 12345UL;
    ops_many[i] = (UBYTE)(seed >> 16);
    ops_one[i] = 0;
  }
  freq = bench_freq();

  printf("dispbench: %lu handlers per test, counter %lu Hz\n", N, (unsigned long)freq);
  run("direct bsr (baseline)", call_direct, ops_one, table_tiny);
  run("table jsr/rts, 1 target", bench_table, ops_one, table_tiny);
  run("table jsr/rts, 256 targets", bench_table, ops_many, table_tiny);
  run("threaded jmp, 1 target", bench_threaded, ops_one, table_thr);
  run("threaded jmp, 256 targets", bench_threaded, ops_many, table_thr);
  return 0;
}
