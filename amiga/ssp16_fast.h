/*
 * ssp16_fast.h - faster SSP1601 (SVP) interpreter for the Amiga port
 *
 * Included at the end of core/cart_hw/svp/ssp16.c so it can use that
 * file's static helpers (pm_io handlers, ptr1_write, ptr2_read, read_P,
 * write_STACK). It must behave exactly like ssp1601_run_c() there; build
 * with SVP_VERIFY=1 to run both on every call and compare the whole DSP
 * state afterwards.
 *
 * Why it is faster (measured with the GPGX_PROFILE build on Emu68: the C
 * reference ran ~47 m68k instructions per DSP instruction):
 *  - PC, cycle counter, accumulator and status live in locals (registers)
 *    instead of globals and struct fields, and are written back only
 *    around calls to the rare register handlers that read them;
 *  - register reads/writes of X, Y, A, ST are inline;
 *  - the pointer-RAM read takes ST as an argument and is inlined.
 */

/* Status flags from the accumulator, as UPD_ACC_ZN / UPD_LZVN */
#define F_UPD_ACC_ZN \
  st &= ~(SSP_FLAG_Z|SSP_FLAG_N); \
  if (!a) st |= SSP_FLAG_Z; else st |= (a >> 16) & SSP_FLAG_N;

#define F_UPD_LZVN \
  st &= ~(SSP_FLAG_L|SSP_FLAG_Z|SSP_FLAG_V|SSP_FLAG_N); \
  if (!a) st |= SSP_FLAG_Z; else st |= (a >> 16) & SSP_FLAG_N;

#ifdef GPGX_PROFILE
/* Sampled cost profile: every FP_EVERY-th DSP instruction is bracketed
 * by Emu68 instruction-counter reads (amiga/prof_asm.s). Key: op >> 9,
 * or 128 + register (+16 for a write) when a register handler ran. */
extern void prof_read3(unsigned long *out);
#define FP_EVERY 32
#define FP_KEYS  160
static unsigned long fp_count[FP_KEYS];
static double fp_ins[FP_KEYS];
static int fp_ctr, fp_key_h;
unsigned long fp_iram_ops, fp_rom_ops;
extern unsigned char ssp_iram_exec[0x400];
#define FP_HANDLER(r, w) fp_key_h = 128 + (r) + ((w) ? 16 : 0)
#else
#define FP_HANDLER(r, w) ((void)0)
#endif

/* Hand the local state to the reference helpers and take it back */
#define F_OUT() \
  s->gr[SSP_A].v = a; s->gr[SSP_ST].byte.h = (unsigned short)st; \
  PC = pc; g_cycles = cyc

#define F_IN() \
  a = s->gr[SSP_A].v; st = s->gr[SSP_ST].byte.h; \
  pc = PC; cyc = g_cycles

/* REG_READ: r 0-2 from the struct, A and ST from the locals, the rest
 * through read_handlers[] (PM0-PM4, XST, PMC, STACK, PC, P, AL) */
#define F_REG_READ(r, dst) \
  switch (r) { \
    case 0: case 1: case 2: dst = s->gr[r].byte.h; break; \
    case 3: dst = a >> 16; break; \
    case 4: dst = st; break; \
    default: FP_HANDLER(r, 0); F_OUT(); dst = read_handlers[r](); F_IN(); break; \
  }

/* REG_WRITE: register 0 is constant; X, Y, A, ST inline; ST through
 * write_ST in the reference, which only stores (logging aside) */
#define F_REG_WRITE(r, d) \
  switch (r) { \
    case 0: break; \
    case 1: case 2: s->gr[r].byte.h = (unsigned short)(d); break; \
    case 3: a = (a & 0xffff) | ((u32)(d) << 16); break; \
    case 4: st = (d) & 0xffff; break; \
    default: FP_HANDLER(r, 1); F_OUT(); write_handlers[r](d); F_IN(); break; \
  }

/* COND_CHECK on the local status */
#define F_COND(c) \
  switch (op & 0xf0) { \
    case 0x00: c = 1; break; \
    case 0x50: c = !((st ^ (op << 5)) & SSP_FLAG_Z); break; \
    case 0x70: c = !((st ^ (op << 7)) & SSP_FLAG_N); break; \
    default: c = 0; break; \
  }

/* Accumulator operations, 16-bit operand (into the high word) */
#define F_SUBA(x) { a -= (x) << 16; F_UPD_LZVN }
#define F_ADDA(x) { a += (x) << 16; F_UPD_LZVN }
#define F_CMPA(x) { u32 t_ = a - ((x) << 16); \
  st &= ~(SSP_FLAG_L|SSP_FLAG_Z|SSP_FLAG_V|SSP_FLAG_N); \
  if (!t_) st |= SSP_FLAG_Z; else st |= (t_ >> 16) & SSP_FLAG_N; }
#define F_ANDA(x) { a &= (x) << 16; F_UPD_ACC_ZN }
#define F_ORA(x)  { a |= (x) << 16; F_UPD_ACC_ZN }
#define F_EORA(x) { a ^= (x) << 16; F_UPD_ACC_ZN }

/* ... and 32-bit operand (OP_CHECK32: A <- P and A <- A forms) */
#define F_SUBA32(x) { a -= (x); F_UPD_LZVN }
#define F_ADDA32(x) { a += (x); F_UPD_LZVN }
#define F_CMPA32(x) { u32 t_ = a - (x); \
  st &= ~(SSP_FLAG_L|SSP_FLAG_Z|SSP_FLAG_V|SSP_FLAG_N); \
  if (!t_) st |= SSP_FLAG_Z; else st |= (t_ >> 16) & SSP_FLAG_N; }
#define F_ANDA32(x) { a &= (x); F_UPD_ACC_ZN }
#define F_ORA32(x)  { a |= (x); F_UPD_ACC_ZN }
#define F_EORA32(x) { a ^= (x); F_UPD_ACC_ZN }

#define F_CHECK32(OP32) \
  if ((op & 0x0f) == SSP_P) { u32 p_; read_P(); p_ = s->gr[SSP_P].v; OP32(p_); break; } \
  if ((op & 0x0f) == SSP_A) { u32 a_ = a; OP32(a_); break; }

#define F_PTR1(op_) f_ptr1_read(s, ((op_)&3) | (((op_)>>6)&4) | (((op_)<<1)&0x18), st)
#define F_IJ(op_)   (((op_)&3) | (((op_)>>6)&4))

/* ptr1_read_() with ST passed in (only ST & 7, the modulo size, is used) */
static __inline__ u32 f_ptr1_read(ssp1601_t *s, int t, u32 st)
{
  u32 mask, add, v;
  unsigned char *rp;
  switch (t)
  {
    /* mod=0 (00) */
    case 0x00: case 0x01: case 0x02: return s->mem.bank.RAM0[s->ptr.bank.r0[t&3]];
    case 0x03: return s->mem.bank.RAM0[0];
    case 0x04: case 0x05: case 0x06: return s->mem.bank.RAM1[s->ptr.bank.r1[t&3]];
    case 0x07: return s->mem.bank.RAM1[0];
    /* mod=1 (01), "+!" */
    case 0x08: case 0x09: case 0x0a: return s->mem.bank.RAM0[s->ptr.bank.r0[t&3]++];
    case 0x0b: return s->mem.bank.RAM0[1];
    case 0x0c: case 0x0d: case 0x0e: return s->mem.bank.RAM1[s->ptr.bank.r1[t&3]++];
    case 0x0f: return s->mem.bank.RAM1[1];
    /* mod=2 (10), "-" */
    case 0x10: case 0x11: case 0x12:
      rp = &s->ptr.bank.r0[t&3]; v = s->mem.bank.RAM0[*rp];
      if (!(st&7)) { (*rp)--; return v; }
      add = (u32)-1; break;
    case 0x13: return s->mem.bank.RAM0[2];
    case 0x14: case 0x15: case 0x16:
      rp = &s->ptr.bank.r1[t&3]; v = s->mem.bank.RAM1[*rp];
      if (!(st&7)) { (*rp)--; return v; }
      add = (u32)-1; break;
    case 0x17: return s->mem.bank.RAM1[2];
    /* mod=3 (11), "+" */
    case 0x18: case 0x19: case 0x1a:
      rp = &s->ptr.bank.r0[t&3]; v = s->mem.bank.RAM0[*rp];
      if (!(st&7)) { (*rp)++; return v; }
      add = 1; break;
    case 0x1b: return s->mem.bank.RAM0[3];
    case 0x1c: case 0x1d: case 0x1e:
      rp = &s->ptr.bank.r1[t&3]; v = s->mem.bank.RAM1[*rp];
      if (!(st&7)) { (*rp)++; return v; }
      add = 1; break;
    case 0x1f: return s->mem.bank.RAM1[3];
    default: return 0;
  }
  /* modulo addressing (reference: label "modulo" in ptr1_read_) */
  mask = (1 << (st&7)) - 1;
  *rp = (*rp & ~mask) | ((*rp + add) & mask);
  return v;
}

static void ssp1601_run_fast(int cycles)
{
  ssp1601_t *const s = ssp;
  unsigned short *const base = (unsigned short *)svp->iram_rom;
  unsigned short *pc = base + s->gr[SSP_PC].byte.h;
  int cyc = cycles;
  u32 a = s->gr[SSP_A].v;
  u32 st = s->gr[SSP_ST].byte.h;

  do
  {
    u32 op;
    u32 v;
    int cond;
#ifdef GPGX_PROFILE
    unsigned long fp_b[3];
    int fp_on = 0;
    {
      long o_ = (long)(pc - base);
      if (o_ < 0x400)
      {
        u32 c_ = *pc >> 9;
        fp_iram_ops++;
        ssp_iram_exec[o_] = 1;
        /* the immediate / branch target word is code too */
        if ((c_ == 0x04 || c_ == 0x06 || c_ == 0x24 || c_ == 0x26 ||
             (c_ & 0x0f) == 0x04) && o_ + 1 < 0x400)
          ssp_iram_exec[o_ + 1] = 1;
      }
      else fp_rom_ops++;
    }
    if (++fp_ctr >= FP_EVERY) { fp_ctr = 0; fp_on = 1; fp_key_h = -1; prof_read3(fp_b); }
#endif
    op = *pc++;

    switch (op >> 9)
    {
      /* ld d, s */
      case 0x00:
        if (op == 0) break; /* nop */
        if (op == ((SSP_A<<4)|SSP_P)) { /* A <- P */
          read_P();
          a = s->gr[SSP_P].v;
          break;
        }
        F_REG_READ(op & 0x0f, v);
        F_REG_WRITE((op & 0xf0) >> 4, v);
        break;

      /* ld d, (ri) */
      case 0x01: v = F_PTR1(op); F_REG_WRITE((op & 0xf0) >> 4, v); break;

      /* ld (ri), s */
      case 0x02: F_REG_READ((op & 0xf0) >> 4, v); ptr1_write(op, v); break;

      /* ld a, adr */
      case 0x03: a = (a & 0xffff) | ((u32)s->mem.RAM[op & 0x1ff] << 16); break;

      /* ldi d, imm */
      case 0x04: v = *pc++; F_REG_WRITE((op & 0xf0) >> 4, v); break;

      /* ld d, ((ri)) */
      case 0x05: v = ptr2_read(op); F_REG_WRITE((op & 0xf0) >> 4, v); break;

      /* ldi (ri), imm */
      case 0x06: v = *pc++; ptr1_write(op, v); break;

      /* ld adr, a */
      case 0x07: s->mem.RAM[op & 0x1ff] = (unsigned short)(a >> 16); break;

      /* ld d, ri */
      case 0x09: v = s->ptr.r[F_IJ(op)]; F_REG_WRITE((op & 0xf0) >> 4, v); break;

      /* ld ri, s */
      case 0x0a: F_REG_READ((op & 0xf0) >> 4, v); s->ptr.r[F_IJ(op)] = (unsigned char)v; break;

      /* ldi ri, simm */
      case 0x0c: case 0x0d: case 0x0e: case 0x0f:
        s->ptr.r[(op >> 8) & 7] = (unsigned char)op; break;

      /* call cond, addr */
      case 0x24:
        F_COND(cond);
        if (cond) {
          u32 npc = *pc++;
          write_STACK((u32)(pc - base));
          pc = base + npc;
          cyc--;
        }
        else pc++;
        break;

      /* ld d, (a) */
      case 0x25: v = base[a >> 16]; F_REG_WRITE((op & 0xf0) >> 4, v); break;

      /* bra cond, addr */
      case 0x26:
        F_COND(cond);
        if (cond) { pc = base + *pc; cyc--; }
        else pc++;
        break;

      /* mod cond, op */
      case 0x48:
        F_COND(cond);
        if (cond) {
          switch (op & 7) {
            case 2: a = (u32)((int)a >> 1); break;   /* shr (arithmetic) */
            case 3: a <<= 1; break;                  /* shl */
            case 6: a = (u32)(-(int)a); break;       /* neg */
            case 7: if ((int)a < 0) a = (u32)(-(int)a); break; /* abs */
            default: break;
          }
          F_UPD_ACC_ZN
        }
        break;

      /* mpys */
      case 0x1b:
        read_P();
        a -= s->gr[SSP_P].v;
        F_UPD_ACC_ZN
        s->gr[SSP_X].byte.h = (unsigned short)f_ptr1_read(s, (op&3) | ((op<<1)&0x18), st);
        s->gr[SSP_Y].byte.h = (unsigned short)f_ptr1_read(s, ((op>>4)&3) | 4 | ((op>>3)&0x18), st);
        break;

      /* mpya (rj), (ri), b */
      case 0x4b:
        read_P();
        a += s->gr[SSP_P].v;
        F_UPD_ACC_ZN
        s->gr[SSP_X].byte.h = (unsigned short)f_ptr1_read(s, (op&3) | ((op<<1)&0x18), st);
        s->gr[SSP_Y].byte.h = (unsigned short)f_ptr1_read(s, ((op>>4)&3) | 4 | ((op>>3)&0x18), st);
        break;

      /* mld (rj), (ri), b */
      case 0x5b:
        a = 0;
        st &= 0x0fff;
        s->gr[SSP_X].byte.h = (unsigned short)f_ptr1_read(s, (op&3) | ((op<<1)&0x18), st);
        s->gr[SSP_Y].byte.h = (unsigned short)f_ptr1_read(s, ((op>>4)&3) | 4 | ((op>>3)&0x18), st);
        break;

      /* OP a, s */
      case 0x10: F_CHECK32(F_SUBA32); F_REG_READ(op & 0x0f, v); F_SUBA(v); break;
      case 0x30: F_CHECK32(F_CMPA32); F_REG_READ(op & 0x0f, v); F_CMPA(v); break;
      case 0x40: F_CHECK32(F_ADDA32); F_REG_READ(op & 0x0f, v); F_ADDA(v); break;
      case 0x50: F_CHECK32(F_ANDA32); F_REG_READ(op & 0x0f, v); F_ANDA(v); break;
      case 0x60: F_CHECK32(F_ORA32);  F_REG_READ(op & 0x0f, v); F_ORA(v);  break;
      case 0x70: F_CHECK32(F_EORA32); F_REG_READ(op & 0x0f, v); F_EORA(v); break;

      /* OP a, (ri) */
      case 0x11: v = F_PTR1(op); F_SUBA(v); break;
      case 0x31: v = F_PTR1(op); F_CMPA(v); break;
      case 0x41: v = F_PTR1(op); F_ADDA(v); break;
      case 0x51: v = F_PTR1(op); F_ANDA(v); break;
      case 0x61: v = F_PTR1(op); F_ORA(v);  break;
      case 0x71: v = F_PTR1(op); F_EORA(v); break;

      /* OP a, adr */
      case 0x13: v = s->mem.RAM[op & 0x1ff]; F_SUBA(v); break;
      case 0x33: v = s->mem.RAM[op & 0x1ff]; F_CMPA(v); break;
      case 0x43: v = s->mem.RAM[op & 0x1ff]; F_ADDA(v); break;
      case 0x53: v = s->mem.RAM[op & 0x1ff]; F_ANDA(v); break;
      case 0x63: v = s->mem.RAM[op & 0x1ff]; F_ORA(v);  break;
      case 0x73: v = s->mem.RAM[op & 0x1ff]; F_EORA(v); break;

      /* OP a, imm */
      case 0x14: v = *pc++; F_SUBA(v); break;
      case 0x34: v = *pc++; F_CMPA(v); break;
      case 0x44: v = *pc++; F_ADDA(v); break;
      case 0x54: v = *pc++; F_ANDA(v); break;
      case 0x64: v = *pc++; F_ORA(v);  break;
      case 0x74: v = *pc++; F_EORA(v); break;

      /* OP a, ((ri)) */
      case 0x15: v = ptr2_read(op); F_SUBA(v); break;
      case 0x35: v = ptr2_read(op); F_CMPA(v); break;
      case 0x45: v = ptr2_read(op); F_ADDA(v); break;
      case 0x55: v = ptr2_read(op); F_ANDA(v); break;
      case 0x65: v = ptr2_read(op); F_ORA(v);  break;
      case 0x75: v = ptr2_read(op); F_EORA(v); break;

      /* OP a, ri */
      case 0x19: v = s->ptr.r[F_IJ(op)]; F_SUBA(v); break;
      case 0x39: v = s->ptr.r[F_IJ(op)]; F_CMPA(v); break;
      case 0x49: v = s->ptr.r[F_IJ(op)]; F_ADDA(v); break;
      case 0x59: v = s->ptr.r[F_IJ(op)]; F_ANDA(v); break;
      case 0x69: v = s->ptr.r[F_IJ(op)]; F_ORA(v);  break;
      case 0x79: v = s->ptr.r[F_IJ(op)]; F_EORA(v); break;

      /* OP simm */
      case 0x1c: v = op & 0xff; F_SUBA(v); break;
      case 0x3c: v = op & 0xff; F_CMPA(v); break;
      case 0x4c: v = op & 0xff; F_ADDA(v); break;
      case 0x5c: v = op & 0xff; F_ANDA(v); break;
      case 0x6c: v = op & 0xff; F_ORA(v);  break;
      case 0x7c: v = op & 0xff; F_EORA(v); break;

      default: break;
    }
#ifdef GPGX_PROFILE
    if (fp_on)
    {
      unsigned long fp_e[3];
      int k;
      prof_read3(fp_e);
      k = fp_key_h >= 0 ? fp_key_h : (int)(op >> 9);
      fp_count[k]++;
      fp_ins[k] += (double)(unsigned long)(fp_e[1] - fp_b[1]);
    }
#endif
  }
  while (--cyc > 0 && !(s->emu_status & SSP_WAIT_MASK));

  F_OUT();
  read_P(); /* update P */
  rPC = GET_PC();
}

#ifdef GPGX_PROFILE
/* Print the sampled per-instruction costs, most expensive in total first.
 * Two back-to-back counter reads give the measurement's own m68k
 * instructions, which are taken off every sample. */
void ssp_fast_prof_report(unsigned long frames)
{
  static const char *const rn[16] = { "-", "X", "Y", "A", "ST", "STACK", "PC", "P",
    "PM0", "PM1", "PM2", "XST", "PM4", "gr13", "PMC", "AL" };
  unsigned long b[3], e[3];
  double base = 0.0, total = 0.0, n = 0.0;
  int i, k, best;
  if (!frames) return;
  for (i = 0; i < 100; i++) { prof_read3(b); prof_read3(e); base += (double)(unsigned long)(e[1] - b[1]); }
  base /= 100.0;
  for (k = 0; k < FP_KEYS; k++)
  {
    if (!fp_count[k]) continue;
    fp_ins[k] -= base * (double)fp_count[k];
    if (fp_ins[k] < 0.0) fp_ins[k] = 0.0;
    total += fp_ins[k];
    n += (double)fp_count[k];
  }
  printf("SVP cost: 1 in %d DSP instructions sampled; measurement takes %lu m68k insns, taken off\n",
         FP_EVERY, (unsigned long)(base + 0.5));
  if (n > 0.0)
    printf("SVP cost: average %lu m68k insns per DSP instruction\n", (unsigned long)(total / n + 0.5));
  printf("SVP cost: kind                DSP ins/frame  m68k each  share\n");
  for (i = 0; i < 24; i++)
  {
    best = -1;
    for (k = 0; k < FP_KEYS; k++)
      if (fp_count[k] && (best < 0 || fp_ins[k] > fp_ins[best])) best = k;
    if (best < 0) break;
    {
      char name[32];
      unsigned long per_frame = (unsigned long)((double)fp_count[best] * FP_EVERY / (double)frames + 0.5);
      unsigned long each = (unsigned long)(fp_ins[best] / (double)fp_count[best] + 0.5);
      unsigned long pct10 = total > 0.0 ? (unsigned long)(fp_ins[best] * 1000.0 / total + 0.5) : 0;
      if (best >= 128)
        sprintf(name, "%s %s handler", (best - 128) >= 16 ? "write" : "read ", rn[(best - 128) & 15]);
      else
        sprintf(name, "op %02x", best);
      printf("SVP cost:   %-20s %10lu %10lu  %3lu.%lu%%\n", name, per_frame, each, pct10 / 10, pct10 % 10);
    }
    fp_count[best] = 0;
  }
}
#endif

#ifdef SVP_VERIFY
/* ----------------------------------------------------- */
/* Run the reference and the fast interpreter from the same state and
 * compare everything the DSP can change. The reference result is kept, so
 * a difference is reported once per call and does not carry over. */

unsigned long ssp_verify_runs, ssp_verify_bad;

#define VERIFY_IRAM 0x800       /* IRAM part of iram_rom (pm_io writes [addr&0x3ff] words) */
#define VERIFY_DRAM 0x20000

static ssp1601_t v_snap, v_ref;
static unsigned char v_snap_iram[VERIFY_IRAM], v_ref_iram[VERIFY_IRAM];
static unsigned char *v_snap_dram, *v_ref_dram;

static int v_first_diff(const void *x, const void *y, int n)
{
  const unsigned char *p = (const unsigned char *)x, *q = (const unsigned char *)y;
  int i;
  for (i = 0; i < n; i++)
    if (p[i] != q[i]) return i;
  return -1;
}

static void ssp1601_run_verify(int cycles)
{
  unsigned short *base = (unsigned short *)svp->iram_rom;
  long ref_pc;
  int ref_cyc, d;
  unsigned short start_pc;

  if (!v_snap_dram)
  {
    v_snap_dram = (unsigned char *)malloc(VERIFY_DRAM);
    v_ref_dram = (unsigned char *)malloc(VERIFY_DRAM);
    if (!v_snap_dram || !v_ref_dram)
    {
      printf("SVP verify: out of memory, running the reference only\n");
      free(v_snap_dram); free(v_ref_dram);
      v_snap_dram = v_ref_dram = NULL;
      ssp1601_run_c(cycles);
      return;
    }
  }

  start_pc = ssp->gr[SSP_PC].byte.h;
  v_snap = *ssp;
  memcpy(v_snap_iram, svp->iram_rom, VERIFY_IRAM);
  memcpy(v_snap_dram, svp->dram, VERIFY_DRAM);

  ssp1601_run_c(cycles);
  v_ref = *ssp;
  memcpy(v_ref_iram, svp->iram_rom, VERIFY_IRAM);
  memcpy(v_ref_dram, svp->dram, VERIFY_DRAM);
  ref_pc = (long)(PC - base);
  ref_cyc = g_cycles;

  *ssp = v_snap;
  memcpy(svp->iram_rom, v_snap_iram, VERIFY_IRAM);
  memcpy(svp->dram, v_snap_dram, VERIFY_DRAM);

  ssp1601_run_fast(cycles);
  ssp_verify_runs++;

  {
    int bad = 0;
    if ((d = v_first_diff(ssp, &v_ref, sizeof(ssp1601_t))) >= 0)
    {
      if (ssp_verify_bad < 8)
        printf("SVP verify: run %lu from PC %04x: state differs at byte %d"
               " (fast %02x, ref %02x)\n", ssp_verify_runs, start_pc, d,
               ((unsigned char *)ssp)[d], ((unsigned char *)&v_ref)[d]);
      bad = 1;
    }
    if ((d = v_first_diff(svp->iram_rom, v_ref_iram, VERIFY_IRAM)) >= 0)
    {
      if (ssp_verify_bad < 8)
        printf("SVP verify: run %lu from PC %04x: IRAM differs at %04x\n",
               ssp_verify_runs, start_pc, d);
      bad = 1;
    }
    if ((d = v_first_diff(svp->dram, v_ref_dram, VERIFY_DRAM)) >= 0)
    {
      if (ssp_verify_bad < 8)
        printf("SVP verify: run %lu from PC %04x: DRAM differs at %05x\n",
               ssp_verify_runs, start_pc, d);
      bad = 1;
    }
    if ((long)(PC - base) != ref_pc || g_cycles != ref_cyc)
    {
      if (ssp_verify_bad < 8)
        printf("SVP verify: run %lu from PC %04x: end PC %04lx/%04lx cycles %d/%d (fast/ref)\n",
               ssp_verify_runs, start_pc, (long)(PC - base), ref_pc, g_cycles, ref_cyc);
      bad = 1;
    }
    if (bad)
      ssp_verify_bad++;
  }

  /* continue from the reference result */
  *ssp = v_ref;
  memcpy(svp->iram_rom, v_ref_iram, VERIFY_IRAM);
  memcpy(svp->dram, v_ref_dram, VERIFY_DRAM);
  PC = base + ref_pc;
  g_cycles = ref_cyc;
}
#endif /* SVP_VERIFY */
