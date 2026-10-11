/*
 * ssp16_drc.h - SSP1601 (SVP) recompiler runtime for the Amiga port
 *
 * Included at the end of core/cart_hw/svp/ssp16.c, after ssp16_fast.h,
 * so it can use that file's statics (PC, g_cycles, register handlers,
 * ptr2_read, read_P) and fall back to ssp1601_run_fast().
 *
 * Emu68 only. Translated code goes to fresh Fast RAM addresses: Emu68
 * reads code above 16 MB straight from memory (src/cache.c) and keys its
 * own translations by address, so new code needs no cache flush there,
 * while any CacheClearU() on Emu68 discards its whole JIT cache (~0.8 ms
 * measured). A real 68040 would need a ranged cache flush per block, so
 * on anything that is not Emu68 the recompiler stays off and the fast
 * interpreter runs.
 *
 * Virtua Racing runs 98.8% of its DSP code from IRAM and reloads IRAM
 * with different routines ~1.6 times per frame (measured), so each IRAM
 * address keeps a list of translations, each with a copy of the program
 * words it was made from. ssp_iram_gen counts IRAM writes; a block that
 * was checked at the current count is used directly, otherwise its words
 * are compared again.
 */

#include <exec/memory.h>
#include <proto/exec.h>
#include <inline/macros.h>

#include "ssp16_drc_emit.h"

extern void drc_enter(void *code, void *regs);  /* amiga/svp_drc_asm.s */

struct drc_regs                 /* layout used by drc_enter */
{
  u32 a;
  u32 st;
  int cyc;
  u32 gen;
  ssp1601_t *s;
  unsigned short *base;
};

struct drc_block
{
  struct drc_block *next;       /* other translations at this address */
  void *code;
  u32 gen;                      /* IRAM generation it was last checked at */
  unsigned short nins, nwords;
  unsigned short start2, nwords2; /* inlined callee: second program range */
  unsigned short src[1];        /* nwords + nwords2 program words */
};

/* code buffer: up to 32 MB (3,000 blocks with inline PM handlers were
 * ~13 MB; a 4 MB buffer reset every few frames), less if not available */
#define DRC_CODE_MAX   (32 * 1024 * 1024)
#define DRC_CODE_MIN   (4 * 1024 * 1024)
static u32 drc_code_size;
#define DRC_META_SIZE  (1024 * 1024)
#define DRC_BLOCK_MAX  32768    /* bytes one block may need at most */
#define DRC_DISP_MAX   1024     /* dispatcher and slow paths at the buffer start */
/* With fewer cycles left than the longest block, C finishes the run with
 * the interpreter: a block may not run past the cycle limit, and the
 * entry points such an exit creates are not worth translating. */
#define DRC_MIN_CYCLES 8

static struct drc_block **drc_map;          /* 0x10000 lists */
static unsigned char *drc_code, *drc_meta;
static u32 drc_code_pos, drc_meta_pos;
static int drc_state;                       /* 0 = not set up, 1 = on, -1 = off */
static unsigned short drc_next_pc;          /* written by every block exit */
static unsigned char drc_exit_flag;         /* set by slow paths / idle detection */
static struct drc_env drc_e;
static struct drc_regs drc_r;
static unsigned char *drc_base_seen;
static u32 drc_disp_size;                  /* bytes the dispatcher takes */
/* A copy of the dispatcher every DRC_DISP_EVERY bytes of code, so block
 * exits reach one with a bra.w that Emu68 inlines (it does so up to
 * 8191 bytes, src/M68k_LINE6.c) */
#define DRC_DISP_EVERY 4096
static u32 drc_disp_pos;                   /* offset of the nearest copy */
static unsigned short *drc_masktab;        /* overwrite masks, 65536 words */

/* exit-summary counters (amiga/main.c) */
unsigned long ssp_drc_blocks, ssp_drc_resets, ssp_drc_fallbacks;
unsigned long ssp_drc_runs;                /* blocks entered from C */
unsigned long ssp_drc_disp[7];             /* dispatcher: chained, and why it returned */
unsigned long ssp_drc_hcnt[32];            /* handler calls: [r] reads, [16 + r] writes */
unsigned long ssp_drc_pmstat[10];          /* profile builds: PM paths taken (drc_env) */
unsigned long ssp_drc_exitstat[20];        /* profile builds: block exits by kind */
unsigned long ssp_drc_tr_inlined;          /* translations with an inlined call */
double ssp_drc_run_cycles;                 /* DSP cycles they used */
unsigned long ssp_drc_waitstart;           /* runs that began in an idle wait */
/* translation statistics */
unsigned long ssp_drc_tr_iram;             /* translations of IRAM code */
double ssp_drc_tr_words, ssp_drc_tr_bytes; /* program words / code bytes */
unsigned long ssp_drc_rs_code, ssp_drc_rs_meta, ssp_drc_rs_fail;  /* reset reasons */
int ssp_drc_disable;                        /* main.c: -nodrc */
const char *ssp_drc_why = "not started";

/* get_inc() (ssp16.c) for (mode >> 11) & 31, used by the inline PM code */
static long drc_inctab[32];

#ifdef SVP_VERIFY
/* inline DRAM / IRAM writes log themselves like pm_io does */
void drc_vlog_dram(u32 off, u32 old, u32 val)
{
  ssp_vlog_add(1, (int)(off >> 1), (unsigned short)old, (unsigned short)val);
}

void drc_vlog_iram(u32 off, u32 old, u32 val)
{
  ssp_vlog_add(2, (int)(off >> 1), (unsigned short)old, (unsigned short)val);
}
#endif

/* C entry points for translated code (address taken: standard ABI) */
u32 drc_hread(int r)
{
  ssp_drc_hcnt[r & 15]++;
  return read_handlers[r]();
}

void drc_hwrite(int r, u32 d)
{
  ssp_drc_hcnt[16 + (r & 15)]++;
  write_handlers[r](d);
}

/* devicetree.resource (Emu68 ROM): DT_OpenKey(name)(a0) at -6,
 * DT_CloseKey(key)(a0) at -12 (devicetree_lib.sfd: ==bias 6) */
#define DT_OpenKey(name) \
  LP1(0x06, APTR, DT_OpenKey, CONST_STRPTR, name, a0, , DeviceTreeBase)
#define DT_CloseKey(key) \
  LP1NR(0x0c, DT_CloseKey, APTR, key, a0, , DeviceTreeBase)

static int drc_on_emu68(void)
{
  APTR DeviceTreeBase = OpenResource((CONST_STRPTR)"devicetree.resource");
  APTR key;
  if (!DeviceTreeBase)
    return 0;
  key = DT_OpenKey((CONST_STRPTR)"/emu68");  /* Emu68 always creates it */
  if (!key)
    return 0;
  DT_CloseKey(key);
  return 1;
}

static void drc_free(void)
{
  if (drc_code) FreeVec(drc_code);
  if (drc_meta) FreeVec(drc_meta);
  if (drc_map)  FreeVec(drc_map);
  if (drc_masktab) FreeVec(drc_masktab);
  drc_code = drc_meta = NULL;
  drc_map = NULL;
  drc_masktab = NULL;
}

void ssp1601_drc_shutdown(void)
{
  drc_free();
  drc_state = 0;
}

/* Forget every translation. The code addresses get reused afterwards, so
 * the JIT must forget them too (one full flush on Emu68). */
static void drc_reset(void)
{
  memset(drc_map, 0, 0x10000 * sizeof(struct drc_block *));
  if (drc_code_pos > drc_disp_size)
    CacheClearU();
  drc_code_pos = drc_disp_size;   /* the dispatcher stays */
  drc_disp_pos = 0;
  drc_e.addr_dispatch = (u32)drc_code;
  drc_meta_pos = 0;
  ssp_drc_resets++;
}

static int drc_init(void)
{
  if (ssp_drc_disable) { ssp_drc_why = "off (-nodrc)"; return 0; }
  if (!drc_on_emu68()) { ssp_drc_why = "off (not Emu68)"; return 0; }

  for (drc_code_size = DRC_CODE_MAX; drc_code_size >= DRC_CODE_MIN; drc_code_size >>= 1)
    if ((drc_code = (unsigned char *)AllocVec(drc_code_size, MEMF_FAST)) != NULL)
      break;
  drc_meta = (unsigned char *)AllocVec(DRC_META_SIZE, MEMF_FAST);
  drc_map  = (struct drc_block **)AllocVec(0x10000 * sizeof(struct drc_block *), MEMF_FAST | MEMF_CLEAR);
  drc_masktab = (unsigned short *)AllocVec(0x10000 * sizeof(unsigned short), MEMF_FAST);
  if (!drc_code || !drc_meta || !drc_map || !drc_masktab)
  {
    drc_free();
    ssp_drc_why = "off (no memory)";
    return 0;
  }
  /* Emu68 reads code below 16 MB through its cache model; only code
   * above it is safe without a flush */
  if ((unsigned long)drc_code < 0x01000000UL)
  {
    drc_free();
    ssp_drc_why = "off (code memory below 16 MB)";
    return 0;
  }

  drc_e.off_gr    = (int)offsetof(ssp1601_t, gr);
  drc_e.off_ptr   = (int)offsetof(ssp1601_t, ptr);
  drc_e.off_stack = (int)offsetof(ssp1601_t, stack);
  drc_e.off_emust = (int)offsetof(ssp1601_t, emu_status);
  drc_e.base_addr = (u32)svp->iram_rom;
  drc_e.addr_PC = (u32)&PC;
  drc_e.addr_g_cycles = (u32)&g_cycles;
  drc_e.addr_next_pc = (u32)&drc_next_pc;
  drc_e.addr_gen = (u32)&ssp_iram_gen;
  drc_e.addr_hread = (u32)drc_hread;
  drc_e.addr_hwrite = (u32)drc_hwrite;
  drc_e.addr_ptr2 = (u32)ptr2_read;
  drc_e.addr_map = (u32)drc_map;
  drc_e.addr_runs = (u32)ssp_drc_disp;
  drc_e.off_b_code = (int)offsetof(struct drc_block, code);
  drc_e.off_b_gen  = (int)offsetof(struct drc_block, gen);
  drc_e.off_b_nins = (int)offsetof(struct drc_block, nins);
  drc_e.min_cycles = DRC_MIN_CYCLES;
  drc_e.off_pmac = (int)offsetof(ssp1601_t, pmac);
  drc_e.addr_dram = (u32)svp->dram;
  drc_e.addr_rom = (u32)cart.rom;
  drc_e.addr_inctab = (u32)drc_inctab;
  drc_e.addr_masktab = (u32)drc_masktab;
#ifdef GPGX_PROFILE
  drc_e.addr_pmstat = (u32)ssp_drc_pmstat;
  drc_e.addr_exitstat = (u32)ssp_drc_exitstat;
#else
  drc_e.addr_pmstat = 0;
  drc_e.addr_exitstat = 0;
#endif
  {
    /* overwite_write() in ssp16.c: a nibble of d replaces the old one
     * only if it is not zero */
    u32 v;
    for (v = 0; v < 0x10000; v++)
      drc_masktab[v] = (unsigned short)(((v & 0xf000) ? 0xf000 : 0) | ((v & 0x0f00) ? 0x0f00 : 0) |
                                        ((v & 0x00f0) ? 0x00f0 : 0) | ((v & 0x000f) ? 0x000f : 0));
  }
#ifdef SVP_VERIFY
  drc_e.addr_vlog = (u32)drc_vlog_dram;
  drc_e.addr_vlog_iram = (u32)drc_vlog_iram;
#else
  drc_e.addr_vlog = 0;
  drc_e.addr_vlog_iram = 0;
#endif
  {
    int i;
    for (i = 0; i < 32; i++)
    {
      long inc = i & 7;
      if (inc)
      {
        if (inc != 7) inc--;
        inc = 1L << inc;
        if (i & 0x10) inc = -inc;      /* mode bit 15: decrement */
      }
      drc_inctab[i] = inc;
    }
  }
#ifdef SVP_VERIFY
  drc_e.no_chain = 1;     /* every block returns, so it can be checked */
#else
  drc_e.no_chain = 0;
#endif
#ifdef GPGX_PROFILE
  drc_e.count_chain = 1;
#else
  drc_e.count_chain = 0;
#endif
  {
    struct drc_out o;
    int pr, pw;
    drc_e.addr_flag = (u32)&drc_exit_flag;
    drc_exit_flag = 0;
    o.buf = drc_code; o.addr = (u32)drc_code; o.pos = 0; o.cap = DRC_DISP_MAX; o.overflow = 0;
    drc_emit_dispatch(&drc_e, &o);
    drc_e.addr_dispatch = (u32)drc_code;
    drc_emit_stubs(&drc_e, &o, &pr, &pw);
    drc_e.addr_slow_read = (u32)(drc_code + pr);
    drc_e.addr_slow_write = (u32)(drc_code + pw);
    drc_disp_size = ((u32)o.pos + 3) & ~3u;
    drc_disp_pos = 0;
  }
  drc_base_seen = svp->iram_rom;
  drc_code_pos = drc_disp_size;
  drc_meta_pos = 0;
  /* Emu68 keys its translations by address: if code of another program
   * ran in this memory before it was freed and handed to us, a stale
   * translation could still exist. One flush now makes all of them
   * invalid; after this only our own code runs here, and an address is
   * never reused without drc_reset() flushing again. */
  CacheClearU();
  ssp_drc_why = "on";
  return 1;
}

/* Translate the block at pcw and put it first in its list */
static struct drc_block *drc_new(u32 pcw)
{
  const unsigned short *base = (const unsigned short *)svp->iram_rom;
  struct drc_out o;
  struct drc_block *b;
  int nins = 0, nwords, nwords2 = 0, max_ins = DRC_MAX_INS;
  drc_u32 start2 = 0;
  u32 need;

  for (;;)
  {
    if (drc_code_pos + DRC_BLOCK_MAX > drc_code_size)
    {
      ssp_drc_rs_code++;
      drc_reset();
    }
    else if (drc_meta_pos + sizeof(struct drc_block) + 2 * 2 * DRC_MAX_INS > DRC_META_SIZE)
    {
      ssp_drc_rs_meta++;
      drc_reset();
    }
    if (drc_code_pos - drc_disp_pos > DRC_DISP_EVERY)
    {
      /* a new dispatcher copy near the coming blocks */
      o.buf = drc_code + drc_code_pos;
      o.addr = (u32)o.buf;
      o.pos = 0; o.cap = DRC_DISP_MAX; o.overflow = 0;
      drc_emit_dispatch(&drc_e, &o);
      drc_disp_pos = drc_code_pos;
      drc_e.addr_dispatch = (u32)(drc_code + drc_code_pos);
      drc_code_pos += ((u32)o.pos + 3) & ~3u;
    }
    o.buf = drc_code + drc_code_pos;
    o.addr = (u32)o.buf;
    o.pos = 0;
    o.cap = DRC_BLOCK_MAX;
    o.overflow = 0;
    nins = drc_translate(&drc_e, &o, base, pcw, pcw < 0x400 ? 0x400 : 0x10000, &nwords, max_ins,
                         &start2, &nwords2);
    if (nins)
      break;
    /* did not fit in DRC_BLOCK_MAX bytes: try a shorter block */
    ssp_drc_rs_fail++;
    if (max_ins == 1)
      break;
    max_ins /= 2;
  }
  if (!nins)
    return NULL;

  need = (u32)(sizeof(struct drc_block) + 2 * (nwords + nwords2) + 3) & ~3u;
  b = (struct drc_block *)(drc_meta + drc_meta_pos);
  drc_meta_pos += need;
  b->code = drc_code + drc_code_pos;
  drc_code_pos += ((u32)o.pos + 3) & ~3u;
  b->nins = (unsigned short)nins;
  b->nwords = (unsigned short)nwords;
  b->start2 = (unsigned short)start2;
  b->nwords2 = (unsigned short)nwords2;
  memcpy(b->src, base + pcw, 2 * nwords);
  if (nwords2)
    memcpy(b->src + nwords, base + start2, 2 * nwords2);
  b->gen = ssp_iram_gen;
  b->next = drc_map[pcw];
  drc_map[pcw] = b;
  ssp_drc_blocks++;
  if (pcw < 0x400) ssp_drc_tr_iram++;
  if (nwords2) ssp_drc_tr_inlined++;
  ssp_drc_tr_words += (double)nwords;
  ssp_drc_tr_bytes += (double)o.pos;
  return b;
}

/* A translation of the code now at pcw */
static struct drc_block *drc_get(u32 pcw)
{
  const unsigned short *base = (const unsigned short *)svp->iram_rom;
  struct drc_block *b, **pp;

  for (pp = &drc_map[pcw]; (b = *pp) != NULL; pp = &b->next)
  {
    if (pcw >= 0x400)               /* program ROM never changes */
      return b;
    if (b->gen == ssp_iram_gen)
      return b;
    if (!memcmp(b->src, base + pcw, 2 * b->nwords) &&
        (!b->nwords2 || !memcmp(b->src + b->nwords, base + b->start2, 2 * b->nwords2)))
    {
      b->gen = ssp_iram_gen;
      if (pp != &drc_map[pcw])      /* most recent first */
      {
        *pp = b->next;
        b->next = drc_map[pcw];
        drc_map[pcw] = b;
      }
      return b;
    }
  }
  return drc_new(pcw);
}

static int drc_ready(void)
{
  if (drc_state == 0)
    drc_state = drc_init() ? 1 : -1;
  if (drc_state == 1 && drc_base_seen != svp->iram_rom)
  {
    /* a new cartridge: program memory moved */
    drc_e.base_addr = (u32)svp->iram_rom;
    drc_base_seen = svp->iram_rom;
    drc_reset();
  }
  return drc_state == 1;
}

/* Same contract as ssp1601_run_c(): run until cycles are used up or the
 * DSP waits; at least one instruction. */
static void ssp1601_run_drc(int cycles)
{
  ssp1601_t *s = ssp;
  unsigned short *base = (unsigned short *)svp->iram_rom;
  u32 pcw = s->gr[SSP_PC].byte.h;
  int first = 1;

  /* The interpreter loop checks the wait flags after every instruction,
   * so a run that starts while the DSP waits executes one instruction.
   * Blocks only check after their own handler calls. */
  if (s->emu_status & SSP_WAIT_MASK)
  {
    ssp_drc_waitstart++;
    ssp1601_run_fast(cycles);
    return;
  }

  drc_r.a = s->gr[SSP_A].v;
  drc_r.st = s->gr[SSP_ST].byte.h;
  drc_r.cyc = cycles;
  drc_r.s = s;
  drc_r.base = base;

  for (;;)
  {
    struct drc_block *b;
    if (!first && (drc_r.cyc <= 0 || (s->emu_status & SSP_WAIT_MASK)))
      break;
    first = 0;
    b = drc_r.cyc < DRC_MIN_CYCLES ? NULL : drc_get(pcw);
    if (!b || drc_r.cyc < (int)b->nins)
    {
      /* few cycles left (a block could run past the limit): finish
       * this call instruction by instruction */
      s->gr[SSP_A].v = drc_r.a;
      s->gr[SSP_ST].byte.h = (unsigned short)drc_r.st;
      rPC = (unsigned short)pcw;
      ssp_drc_fallbacks++;
      ssp1601_run_fast(drc_r.cyc);
      return;
    }
    drc_r.gen = ssp_iram_gen;
    {
      int before = drc_r.cyc;
      drc_enter(b->code, &drc_r);
      ssp_drc_runs++;
      ssp_drc_run_cycles += (double)(before - drc_r.cyc);
    }
    pcw = drc_next_pc;
  }

  s->gr[SSP_A].v = drc_r.a;
  s->gr[SSP_ST].byte.h = (unsigned short)drc_r.st;
  PC = base + pcw;
  g_cycles = drc_r.cyc;
  read_P(); /* update P */
  rPC = (unsigned short)pcw;
}

#ifdef SVP_VERIFY
/* ----------------------------------------------------- */
/* Per-block check: run the translated block, undo it, run the reference
 * interpreter for the same cycles, compare the DSP state, the next PC and
 * every DRAM/IRAM write (logged in pm_io), then continue from the
 * reference result. */

unsigned long ssp_drc_vblocks, ssp_drc_vbad;
static unsigned long drc_vctr;

static void drc_vlog_undo(void)
{
  int i;
  for (i = ssp_vlog_n - 1; i >= 0; i--)
  {
    struct ssp_vlog_e *l = &ssp_vlog[i];
    if (l->kind == 1) ((unsigned short *)svp->dram)[l->addr] = l->old;
    else ((unsigned short *)svp->iram_rom)[l->addr] = l->old;
  }
}

static void drc_vreport(u32 pcw, const struct drc_block *b, const ssp1601_t *d,
                        const ssp1601_t *r, u32 dpc, u32 rpc, int rcyc,
                        const struct ssp_vlog_e *dlog, int dn)
{
  static const char *const rn[16] = { "-", "X", "Y", "A", "ST", "STACK", "PC", "P",
    "PM0", "PM1", "PM2", "XST", "PM4", "gr13", "PMC", "AL" };
  const unsigned short *base = (const unsigned short *)svp->iram_rom;
  int i;
  printf("SVP DRC verify: block %04lx (%d ins):", (unsigned long)pcw, b->nins);
  for (i = 0; i < b->nwords && i < 12; i++) printf(" %04x", b->src[i]);
  printf("\n");
  (void)base;
  if (dpc != rpc || rcyc != 0)
    printf("  next PC drc %04lx ref %04lx, ref cycles left %d\n",
           (unsigned long)dpc, (unsigned long)rpc, rcyc);
  for (i = 0; i < 16; i++)
    if (d->gr[i].v != r->gr[i].v)
      printf("  %s: drc %08x ref %08x\n", rn[i], d->gr[i].v, r->gr[i].v);
  for (i = 0; i < 8; i++)
    if (d->ptr.r[i] != r->ptr.r[i])
      printf("  r%d: drc %02x ref %02x\n", i, d->ptr.r[i], r->ptr.r[i]);
  for (i = 0; i < 6; i++)
    if (d->stack[i] != r->stack[i])
      printf("  stack[%d]: drc %04x ref %04x\n", i, d->stack[i], r->stack[i]);
  for (i = 0; i < 12; i++)
    if (((const unsigned int *)d->pmac)[i] != ((const unsigned int *)r->pmac)[i])
      printf("  pmac[%d]: drc %08x ref %08x\n", i,
             ((const unsigned int *)d->pmac)[i], ((const unsigned int *)r->pmac)[i]);
  if (d->emu_status != r->emu_status)
    printf("  emu_status: drc %08x ref %08x\n", d->emu_status, r->emu_status);
  for (i = 0; i < 512; i++)
    if (d->mem.RAM[i] != r->mem.RAM[i])
    {
      printf("  RAM[%03x]: drc %04x ref %04x\n", i, d->mem.RAM[i], r->mem.RAM[i]);
      break;
    }
  if (dn != ssp_vlog_n || memcmp(dlog, ssp_vlog, dn * sizeof(struct ssp_vlog_e)))
    printf("  memory writes differ: drc %d ref %d\n", dn, ssp_vlog_n);
}

static void ssp1601_run_drc_verify(int cycles)
{
  static ssp1601_t snap, dres;
  static struct ssp_vlog_e dlog[SSP_VLOG_MAX];
  ssp1601_t *s = ssp;
  unsigned short *base = (unsigned short *)svp->iram_rom;
  u32 pcw = s->gr[SSP_PC].byte.h;
  int cyc = cycles, first = 1;

  if (s->emu_status & SSP_WAIT_MASK)  /* as in ssp1601_run_drc() */
  {
    ssp_drc_waitstart++;
    ssp1601_run_fast(cycles);
    return;
  }

  for (;;)
  {
    struct drc_block *b;
    int used, dn, bad;
    u32 dpc;

    if (!first && (cyc <= 0 || (s->emu_status & SSP_WAIT_MASK)))
      break;
    first = 0;
    b = cyc < DRC_MIN_CYCLES ? NULL : drc_get(pcw);
    if (!b || cyc < (int)b->nins)
    {
      rPC = (unsigned short)pcw;
      ssp1601_run_fast(cyc);
      return;
    }

    /* check every 8th block run; the others just run (no chaining
     * in verify builds, so each returns here) */
    if (++drc_vctr & 7)
    {
      drc_r.a = s->gr[SSP_A].v; drc_r.st = s->gr[SSP_ST].byte.h;
      drc_r.cyc = cyc; drc_r.s = s; drc_r.base = base; drc_r.gen = ssp_iram_gen;
      drc_enter(b->code, &drc_r);
      s->gr[SSP_A].v = drc_r.a;
      s->gr[SSP_ST].byte.h = (unsigned short)drc_r.st;
      cyc = drc_r.cyc;
      pcw = drc_next_pc;
      continue;
    }

    /* translated block */
    rPC = (unsigned short)pcw;
    snap = *s;
    ssp_vlog_n = 0; ssp_vlog_on = 1;
    drc_r.a = s->gr[SSP_A].v; drc_r.st = s->gr[SSP_ST].byte.h;
    drc_r.cyc = cyc; drc_r.s = s; drc_r.base = base; drc_r.gen = ssp_iram_gen;
    drc_enter(b->code, &drc_r);
    s->gr[SSP_A].v = drc_r.a;
    s->gr[SSP_ST].byte.h = (unsigned short)drc_r.st;
    read_P();                       /* the reference ends with read_P() too */
    dpc = drc_next_pc;
    rPC = (unsigned short)dpc;      /* ... and with rPC = GET_PC() */
    used = cyc - drc_r.cyc;
    dres = *s;
    dn = ssp_vlog_n;
    memcpy(dlog, ssp_vlog, dn * sizeof(struct ssp_vlog_e));

    /* undo, then the reference for the same cycles */
    drc_vlog_undo();
    *s = snap;
    ssp_vlog_n = 0;
    ssp1601_run_c(used);
    ssp_vlog_on = 0;
    ssp_drc_vblocks++;

    bad = memcmp(&dres, s, sizeof(ssp1601_t)) != 0 || dpc != rPC || g_cycles != 0 ||
          dn != ssp_vlog_n || memcmp(dlog, ssp_vlog, dn * sizeof(struct ssp_vlog_e)) != 0 ||
          ssp_vlog_overflow;
    if (bad)
    {
      if (ssp_drc_vbad < 12)
        drc_vreport(pcw, b, &dres, s, dpc, rPC, g_cycles, dlog, dn);
      ssp_drc_vbad++;
    }
    ssp_vlog_overflow = 0;

    /* continue from the reference */
    pcw = rPC;
    cyc -= used - g_cycles;
  }

  PC = base + pcw;
  g_cycles = cyc;
  read_P();
  rPC = (unsigned short)pcw;
}
#endif /* SVP_VERIFY */
