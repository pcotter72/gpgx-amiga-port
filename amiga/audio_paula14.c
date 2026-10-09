/****************************************************************************
 *  audio_paula14.c - 14-bit Paula audio, BeginIO/audio.device backend
 *
 *  Channel mapping (Buchner/Richter):
 *  - AUD0 (ch 0) = Left  HI, vol 64  \  summed on left  DAC
 *  - AUD3 (ch 3) = Left  LO, vol  1  /
 *  - AUD1 (ch 1) = Right HI, vol 64  \  summed on right DAC
 *  - AUD2 (ch 2) = Right LO, vol  1  /
 *
 *  Left = channels 0 and 3. NOTE: the Hardware Reference Manual (5-2-1)
 *  and RKRM Devices say the opposite (1/2 left, 0/3 right). Paula's AUDL
 *  pin carries channels 0 and 3 (8364 pinout), Eagleplayer's 14-bit
 *  output (Ampout_Paula14.s) uses 0/3 as left, and so does the tracker
 *  LRRL convention - so this mapping is kept. Don't "fix" it from the
 *  manuals without a listening test on real hardware.
 *
 *  All four channels are allocated with ONE allocation key, so the four
 *  CMD_WRITEs of a buffer can be started in exact sync: whenever every
 *  channel is idle (first buffer, or after the queue ran dry) the writes
 *  are queued behind CMD_STOP and released with one CMD_START, which
 *  "exactly synchronizes them" (RKRM Devices, CMD_START). While playing,
 *  same-length buffers switch at the same sample on all four channels.
 *
 *  Samples go through two 64 KB tables (HI, LO) built for the current
 *  volume boost: clamping is baked in, and the split is sign-symmetric -
 *  a negative sample gets a negative LO - so a quiet signal near zero
 *  moves only the volume-1 channel instead of flipping the HI channel
 *  between 0 and -1 on every zero crossing (same as Eagleplayer's
 *  PAULA14_NEG14RANGE).
 *
 ****************************************************************************/

#include <stdio.h>
#include <string.h>

#include <exec/exec.h>
#include <exec/memory.h>
#include <devices/audio.h>

#include <proto/exec.h>
#include <clib/alib_protos.h>

#include "audio_paula14.h"

#ifdef USE_ASM_AUDIO
extern void Convert14Tab_asm(signed short *src,
                             signed char *dst_lhi, signed char *dst_llo,
                             signed char *dst_rhi, signed char *dst_rlo,
                             int samples,
                             const signed char *hi_mid, const signed char *lo_mid);
#endif

/* Paula clock rates */
#define PAULA_CLOCK_PAL  3546895UL
#define PAULA_CLOCK_NTSC 3579545UL

/* Quad buffering: 4 frames headroom */
#define PAULA_NUM_BUFFERS 4

/* Channel roles. Each slot sends one CMD_WRITE per role. */
#define R_LHI 0
#define R_LLO 1
#define R_RHI 2
#define R_RLO 3
#define NUM_ROLES 4
static const UBYTE role_chan[NUM_ROLES] = { 0, 3, 1, 2 };    /* AUDx */
static const UWORD role_vol[NUM_ROLES]  = { 64, 1, 64, 1 };

/* One allocation of all four channels: a single combination, 0x0F */
static UBYTE chan_all[] = { 0x0F };

static struct MsgPort *paula14_port = NULL;
static struct IOAudio *io_ctl = NULL;      /* the OpenDevice() request, unit 0x0F */
static BOOL ctl_open = FALSE;

/* Per slot, per role: request and Chip RAM DMA buffer */
static struct IOAudio *io_req[PAULA_NUM_BUFFERS][NUM_ROLES];
static BYTE *buf[PAULA_NUM_BUFFERS][NUM_ROLES];
static BOOL pending[PAULA_NUM_BUFFERS];    /* all four requests of the slot sent */

/* Fast RAM staging, one per role */
static BYTE *fast[NUM_ROLES];
static ULONG fast_buf_size = 0;

/* HI/LO tables: 2 x 65536 bytes, indexed by the signed sample from the middle */
#define TAB_BYTES 131072UL
static BYTE *tab14 = NULL;
static int  tab_boost = -1;                /* boost the tables were built for */

/* State */
static BOOL paula14_open = FALSE;
static int  paula14_current = 0;

/* Debug counters */
int paula14_dbg_submitted = 0;
int paula14_dbg_dropped   = 0;
int paula14_dbg_syncs     = 0;             /* synchronised (CMD_STOP/START) starts */

/* Audio parameters */
static ULONG paula_freq   = 0;
static ULONG paula_buflen = 0;             /* buffer CAPACITY in bytes/samples */
static UWORD paula_period = 128;

/* Volume boost shift */
static int volume_boost = 0;

void paula14_set_volume_boost(int boost)
{
    if (boost < 0) boost = 0;
    if (boost > 4) boost = 4;
    volume_boost = boost;
}

/****************************************************************************
 * HI/LO tables for one boost value.
 *
 * v   = sample << boost, clamped to 16 bits
 * v14 = floor(v / 4)                 (-8192..8191, as before)
 * |v14| = 64 * |HI| + |LO|, HI and LO taking the sign of v14:
 *   v14 >= 0:  HI = v14 / 64,  LO = v14 % 64          (0..127, 0..63)
 *   v14 <  0:  HI = -(m / 64), LO = -(m % 64), m=-v14  (-128..0, -63..0)
 * HI*64 + LO == v14 exactly in both cases.
 ****************************************************************************/
static void build_tables(int boost)
{
    BYTE *hi = tab14 + 32768;              /* hi[s] for s = -32768..32767 */
    BYTE *lo = tab14 + 65536 + 32768;
    LONG s, v, v14, m;

    for (s = -32768; s <= 32767; s++)
    {
        v = s * (1L << boost);     /* not s << boost: s may be negative */
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        /* floor division by 4 without relying on >> of a negative value */
        v14 = (v >= 0) ? (v >> 2) : -(((-v) + 3) >> 2);
        if (v14 >= 0)
        {
            hi[s] = (BYTE)(v14 >> 6);
            lo[s] = (BYTE)(v14 & 63);
        }
        else
        {
            m = -v14;
            hi[s] = (BYTE)(-(m >> 6));
            lo[s] = (BYTE)(-(m & 63));
        }
    }
    tab_boost = boost;
}

/****************************************************************************
 * Initialize 14-bit Paula audio
 ****************************************************************************/
int paula14_audio_init(int frequency, int hw_pal, int game_pal)
{
    ULONG paula_clock;
    UWORD period;
    double fps, spf;
    int i, r;

    if (paula14_open || ctl_open)
        paula14_audio_shutdown();

    for (i = 0; i < PAULA_NUM_BUFFERS; i++)
    {
        for (r = 0; r < NUM_ROLES; r++)
        {
            io_req[i][r] = NULL;
            buf[i][r] = NULL;
        }
        pending[i] = FALSE;
    }
    for (r = 0; r < NUM_ROLES; r++)
        fast[r] = NULL;
    fast_buf_size = 0;
    paula14_current = 0;
    paula14_dbg_syncs = 0;

    paula_clock = hw_pal ? PAULA_CLOCK_PAL : PAULA_CLOCK_NTSC;
    if (frequency <= 0)
        period = 128;
    else
    {
        period = (UWORD)(paula_clock / (ULONG)frequency);
        if (period < 80)   period = 80;
        if (period > 1024) period = 1024;
        period = (period + 1) & ~1;
    }
    paula_period = period;
    paula_freq   = paula_clock / period;

    /* Buffer CAPACITY, not playback length: each CMD_WRITE plays exactly
     * the samples submitted (main.c passes one frame, always an even count).
     * +3% +16 covers blip_buf rounding (a few samples over freq/fps) and
     * main.c's rate control, which stretches a frame by up to ~1.2%. */
    fps = game_pal ? 50.0 : 60.0;
    spf = (double)paula_freq / fps;
    paula_buflen = (ULONG)(spf * 1.03 + 16.0);
    paula_buflen = (paula_buflen + 1) & ~1;  /* word-align for DMA */

    paula14_port = CreateMsgPort();
    if (!paula14_port) goto fail;

    /* Allocate all four channels at once (one key). OpenDevice() allocates
     * as ADCMD_ALLOCATE would when ioa_Length is non-zero (RKRM Devices,
     * Opening The Audio Device) and copies the combination into io_Unit. */
    io_ctl = (struct IOAudio *)CreateIORequest(paula14_port, sizeof(struct IOAudio));
    if (!io_ctl) goto fail;
    io_ctl->ioa_Request.io_Message.mn_Node.ln_Pri = 127;
    io_ctl->ioa_Request.io_Command = ADCMD_ALLOCATE;
    io_ctl->ioa_Request.io_Flags   = ADIOF_NOWAIT;
    io_ctl->ioa_AllocKey           = 0;
    io_ctl->ioa_Data               = chan_all;
    io_ctl->ioa_Length             = sizeof(chan_all);
    if (OpenDevice("audio.device", 0, (struct IORequest *)io_ctl, 0))
    {
        printf("Paula14: cannot allocate all four audio channels (in use?)\n");
        DeleteIORequest((struct IORequest *)io_ctl);
        io_ctl = NULL;
        goto fail;
    }
    ctl_open = TRUE;
    if ((ULONG)io_ctl->ioa_Request.io_Unit != 0x0F)
    {
        printf("Paula14: unexpected channel set $%lx\n",
               (ULONG)io_ctl->ioa_Request.io_Unit);
        goto fail;
    }

    /* One request per slot per channel: same key, single-channel unit */
    for (i = 0; i < PAULA_NUM_BUFFERS; i++)
    {
        for (r = 0; r < NUM_ROLES; r++)
        {
            io_req[i][r] = (struct IOAudio *)CreateIORequest(paula14_port,
                                                             sizeof(struct IOAudio));
            if (!io_req[i][r])
            {
                printf("Paula14: cannot create IO request %d\n", i);
                goto fail;
            }
            *io_req[i][r] = *io_ctl;
            io_req[i][r]->ioa_Request.io_Unit =
                (struct Unit *)(ULONG)(1UL << role_chan[r]);
        }
    }

    /* Chip RAM DMA buffers */
    for (i = 0; i < PAULA_NUM_BUFFERS; i++)
    {
        for (r = 0; r < NUM_ROLES; r++)
        {
            buf[i][r] = (BYTE *)AllocMem(paula_buflen, MEMF_CHIP | MEMF_CLEAR);
            if (!buf[i][r])
            {
                printf("Paula14: out of chip RAM\n");
                goto fail;
            }
        }
    }

    /* HI/LO tables in Fast RAM if there is any */
    tab14 = (BYTE *)AllocMem(TAB_BYTES, MEMF_FAST);
    if (!tab14)
        tab14 = (BYTE *)AllocMem(TAB_BYTES, MEMF_ANY);
    if (!tab14)
    {
        printf("Paula14: out of memory for the conversion tables\n");
        goto fail;
    }
    tab_boost = -1;          /* built on the first submit, for the boost then */

    /* Fast RAM staging (non-fatal) */
    fast_buf_size = paula_buflen;
    for (r = 0; r < NUM_ROLES; r++)
        fast[r] = (BYTE *)AllocMem(fast_buf_size, MEMF_FAST | MEMF_CLEAR);
    if (!fast[0] || !fast[1] || !fast[2] || !fast[3])
    {
        printf("Paula14: fast staging alloc failed (non-fatal)\n");
        for (r = 0; r < NUM_ROLES; r++)
            if (fast[r]) { FreeMem(fast[r], fast_buf_size); fast[r] = NULL; }
        fast_buf_size = 0;
    }

    paula14_open = TRUE;
    printf("Audio: Paula 14-bit %luHz stereo (period=%u, quad-buffered BeginIO)\n",
           paula_freq, paula_period);
    return 1;

fail:
    paula14_audio_shutdown();
    return 0;
}

/****************************************************************************
 * Shutdown
 ****************************************************************************/
void paula14_audio_shutdown(void)
{
    int i, r;

    if (paula14_open)
    {
        /* Abort and wait for all pending requests */
        for (i = 0; i < PAULA_NUM_BUFFERS; i++)
        {
            if (!pending[i])
                continue;
            for (r = 0; r < NUM_ROLES; r++)
            {
                if (io_req[i][r])
                {
                    AbortIO((struct IORequest *)io_req[i][r]);
                    WaitIO((struct IORequest *)io_req[i][r]);
                }
            }
            pending[i] = FALSE;
        }
        paula14_open = FALSE;
    }

    /* Release the channels. Outside the block above (paula14_open is set
     * only at the very end of init) so a partial init failure still frees
     * what it allocated. */
    if (ctl_open)
    {
        CloseDevice((struct IORequest *)io_ctl);
        ctl_open = FALSE;
    }
    if (io_ctl)
    {
        DeleteIORequest((struct IORequest *)io_ctl);
        io_ctl = NULL;
    }

    for (i = 0; i < PAULA_NUM_BUFFERS; i++)
    {
        for (r = 0; r < NUM_ROLES; r++)
        {
            if (io_req[i][r])
            {
                DeleteIORequest((struct IORequest *)io_req[i][r]);
                io_req[i][r] = NULL;
            }
            if (buf[i][r])
            {
                FreeMem(buf[i][r], paula_buflen);
                buf[i][r] = NULL;
            }
        }
    }

    for (r = 0; r < NUM_ROLES; r++)
        if (fast[r]) { FreeMem(fast[r], fast_buf_size); fast[r] = NULL; }
    fast_buf_size = 0;

    if (tab14) { FreeMem(tab14, TAB_BYTES); tab14 = NULL; }
    tab_boost = -1;

    if (paula14_port) { DeleteMsgPort(paula14_port); paula14_port = NULL; }
}

/* Have all four requests of a sent slot finished? (CheckIO never blocks) */
static int slot_done(int slot)
{
    int r;
    for (r = 0; r < NUM_ROLES; r++)
        if (!CheckIO((struct IORequest *)io_req[slot][r]))
            return 0;
    return 1;
}

/* No request still playing or queued on any channel: all four are idle. */
static int channels_idle(void)
{
    int i;
    for (i = 0; i < PAULA_NUM_BUFFERS; i++)
        if (pending[i] && !slot_done(i))
            return 0;
    return 1;
}

/****************************************************************************
 * Availability - non-blocking poll
 ****************************************************************************/
int paula14_audio_available(void)
{
    int i, slot;
    if (!paula14_open) return 0;
    for (i = 0; i < PAULA_NUM_BUFFERS; i++)
    {
        slot = (paula14_current + i) % PAULA_NUM_BUFFERS;
        if (!pending[slot] || slot_done(slot))
            return 1;
    }
    return 0;
}

/****************************************************************************
 * Submit audio buffer - non-blocking
 ****************************************************************************/
void paula14_audio_submit(signed short *buffer, int samples)
{
    int fill_buf, slot, num, s, r, sync;
    BYTE *d[NUM_ROLES];
    struct IOAudio *io;

    if (!paula14_open)
        return;

    /* Find a free slot. A sent slot is reused only when all four of its
     * requests have finished, so WaitIO() below never waits. */
    fill_buf = -1;
    for (s = 0; s < PAULA_NUM_BUFFERS; s++)
    {
        slot = (paula14_current + s) % PAULA_NUM_BUFFERS;
        if (!pending[slot])
        {
            fill_buf = slot;
            break;
        }
        if (slot_done(slot))
        {
            for (r = 0; r < NUM_ROLES; r++)
                WaitIO((struct IORequest *)io_req[slot][r]);
            pending[slot] = FALSE;
            fill_buf = slot;
            break;
        }
    }
    if (fill_buf < 0)
    {
        /* All slots still playing - drop this audio frame. main.c checks
         * paula14_audio_available() first, so this is a rare race. */
        paula14_dbg_dropped++;
        return;
    }

    /* Exactly the samples given. main.c always passes an even count (it
     * carries an odd last sample into the next frame); the checks below
     * only guard against a caller that does not. */
    num = samples;
    if (num > (int)paula_buflen) num = (int)paula_buflen;   /* capacity */
    num &= ~1;                       /* CMD_WRITE length must be even */
    if (!buffer || num < 2)
        return;                      /* slot stays free */

    if (tab_boost != volume_boost)
        build_tables(volume_boost);

    for (r = 0; r < NUM_ROLES; r++)
        d[r] = fast[r] ? fast[r] : buf[fill_buf][r];

#ifdef USE_ASM_AUDIO
    Convert14Tab_asm(buffer, d[R_LHI], d[R_LLO], d[R_RHI], d[R_RLO], num,
                     tab14 + 32768, tab14 + 65536 + 32768);
#else
    {
        const BYTE *hi = tab14 + 32768;
        const BYTE *lo = tab14 + 65536 + 32768;
        for (s = 0; s < num; s++)
        {
            int sl = buffer[s * 2];
            int sr = buffer[s * 2 + 1];
            d[R_LHI][s] = hi[sl];
            d[R_LLO][s] = lo[sl];
            d[R_RHI][s] = hi[sr];
            d[R_RLO][s] = lo[sr];
        }
    }
#endif
    if (fast[0])
        for (r = 0; r < NUM_ROLES; r++)
            CopyMem(fast[r], buf[fill_buf][r], num);

    /* Each request plays exactly num samples (no tail padding) */
    for (r = 0; r < NUM_ROLES; r++)
    {
        io = io_req[fill_buf][r];
        io->ioa_Request.io_Command = CMD_WRITE;
        io->ioa_Request.io_Flags   = ADIOF_PERVOL;
        io->ioa_Data   = (UBYTE *)buf[fill_buf][r];
        io->ioa_Length = num;
        io->ioa_Period = paula_period;
        io->ioa_Volume = role_vol[r];
        io->ioa_Cycles = 1;
    }

    /* All four channels idle: start them together. CMD_STOP makes the
     * writes queue instead of playing, CMD_START releases all four at once.
     * Otherwise every channel is still playing the previous buffer and the
     * new ones follow it at the same sample on each channel. */
    sync = channels_idle();
    if (sync)
    {
        io_ctl->ioa_Request.io_Command = CMD_STOP;
        io_ctl->ioa_Request.io_Flags   = 0;
        DoIO((struct IORequest *)io_ctl);
    }
    for (r = 0; r < NUM_ROLES; r++)
        BeginIO((struct IORequest *)io_req[fill_buf][r]);
    if (sync)
    {
        io_ctl->ioa_Request.io_Command = CMD_START;
        io_ctl->ioa_Request.io_Flags   = 0;
        DoIO((struct IORequest *)io_ctl);
        paula14_dbg_syncs++;
    }

    pending[fill_buf] = TRUE;
    paula14_dbg_submitted++;
    paula14_current = (fill_buf + 1) % PAULA_NUM_BUFFERS;
}

/****************************************************************************
 * Utility
 ****************************************************************************/
int paula14_audio_get_rate(void) { return (int)paula_freq; }

int paula14_audio_queued(void)
{
    int i, count = 0;
    if (!paula14_open) return 0;
    for (i = 0; i < PAULA_NUM_BUFFERS; i++)
        if (pending[i] && !slot_done(i))
            count++;
    return count;
}
