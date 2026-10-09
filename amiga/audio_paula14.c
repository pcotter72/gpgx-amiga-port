/****************************************************************************
 *  audio_paula14.c - 14-bit Paula audio, BeginIO/audio.device backend
 *
 *  Channel mapping (Buchner/Richter):
 *  - AUD0 (ch 0) = Left  HI, vol 64  \  summed on left  DAC
 *  - AUD3 (ch 3) = Left  LO, vol  1  /
 *  - AUD1 (ch 1) = Right HI, vol 64  \  summed on right DAC
 *  - AUD2 (ch 2) = Right LO, vol  1  /
 *
 *  Uses quad-buffered CMD_WRITE via audio.device.  All four BeginIO
 *  calls are wrapped in Disable/Enable so all channels start on the
 *  same scheduler tick, preventing phase drift.
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
extern void Convert14Bit_asm(signed short *src,
                              signed char *dst_lhi, signed char *dst_llo,
                              signed char *dst_rhi, signed char *dst_rlo,
                              int samples, int boost);
#endif

/* Paula clock rates */
#define PAULA_CLOCK_PAL  3546895UL
#define PAULA_CLOCK_NTSC 3579545UL

/* Quad buffering: 4 frames headroom (~80ms at 50fps) */
#define PAULA_NUM_BUFFERS 4

/* Fixed channel assignment */
#define CHAN_LHI 0    /* AUD0: left  HI, vol 64 */
#define CHAN_RHI 1    /* AUD1: right HI, vol 64 */
#define CHAN_RLO 2    /* AUD2: right LO, vol  1 */
#define CHAN_LLO 3    /* AUD3: left  LO, vol  1 */

/* Channel allocation masks (one bit per mask array) */
static UBYTE chan_mask_lhi[] = { 1 << CHAN_LHI };  /* 0x01 */
static UBYTE chan_mask_rhi[] = { 1 << CHAN_RHI };  /* 0x02 */
static UBYTE chan_mask_rlo[] = { 1 << CHAN_RLO };  /* 0x04 */
static UBYTE chan_mask_llo[] = { 1 << CHAN_LLO };  /* 0x08 */

/* IO request arrays — one per buffer slot per channel */
static struct MsgPort *paula14_port = NULL;
static struct IOAudio *io_lhi[PAULA_NUM_BUFFERS];
static struct IOAudio *io_llo[PAULA_NUM_BUFFERS];
static struct IOAudio *io_rhi[PAULA_NUM_BUFFERS];
static struct IOAudio *io_rlo[PAULA_NUM_BUFFERS];

/* Chip RAM DMA buffers — one per slot per channel */
static BYTE *buf_lhi[PAULA_NUM_BUFFERS];
static BYTE *buf_llo[PAULA_NUM_BUFFERS];
static BYTE *buf_rhi[PAULA_NUM_BUFFERS];
static BYTE *buf_rlo[PAULA_NUM_BUFFERS];

/* Pending flags */
static BOOL req_pending_lhi[PAULA_NUM_BUFFERS];
static BOOL req_pending_llo[PAULA_NUM_BUFFERS];
static BOOL req_pending_rhi[PAULA_NUM_BUFFERS];
static BOOL req_pending_rlo[PAULA_NUM_BUFFERS];

/* Fast RAM staging */
static BYTE *fast_lhi = NULL;
static BYTE *fast_llo = NULL;
static BYTE *fast_rhi = NULL;
static BYTE *fast_rlo = NULL;
static ULONG fast_buf_size = 0;

/* State */
static BOOL paula14_open = FALSE;
static int  paula14_current = 0;

/* Debug counters */
int paula14_dbg_submitted = 0;
int paula14_dbg_dropped   = 0;

/* Audio parameters */
static ULONG paula_freq   = 0;
static ULONG paula_buflen = 0;
static UWORD paula_period = 128;

/* Volume boost shift */
static int volume_boost = 0;
static int paula14_margin = 0;

void paula14_set_margin(int margin)
{
    if (margin < 0)   margin = 0;
    if (margin > 882) margin = 882;
    paula14_margin = margin;
}

void paula14_set_volume_boost(int boost)
{
    if (boost < 0) boost = 0;
    if (boost > 4) boost = 4;
    volume_boost = boost;
}

/****************************************************************************
 * Open audio.device to claim one specific channel
 ****************************************************************************/
static struct IOAudio *alloc_chan(struct MsgPort *port,
                                  UBYTE *mask, int mlen)
{
    struct IOAudio *io;
    io = (struct IOAudio *)CreateIORequest(port, sizeof(struct IOAudio));
    if (!io) return NULL;
    io->ioa_Request.io_Message.mn_Node.ln_Pri = 127;
    io->ioa_Request.io_Command = ADCMD_ALLOCATE;
    io->ioa_Request.io_Flags   = ADIOF_NOWAIT;
    io->ioa_AllocKey           = 0;
    io->ioa_Data               = mask;
    io->ioa_Length             = mlen;
    if (OpenDevice("audio.device", 0, (struct IORequest *)io, 0))
    {
        DeleteIORequest((struct IORequest *)io);
        return NULL;
    }
    return io;
}

/****************************************************************************
 * Initialize 14-bit Paula audio
 ****************************************************************************/
int paula14_audio_init(int frequency, int hw_pal, int game_pal)
{
    ULONG paula_clock;
    UWORD period;
    double fps, spf;
    int i;

    if (paula14_open)
        paula14_audio_shutdown();

    for (i = 0; i < PAULA_NUM_BUFFERS; i++)
    {
        io_lhi[i] = io_llo[i] = io_rhi[i] = io_rlo[i] = NULL;
        buf_lhi[i] = buf_llo[i] = buf_rhi[i] = buf_rlo[i] = NULL;
        req_pending_lhi[i] = req_pending_llo[i] = FALSE;
        req_pending_rhi[i] = req_pending_rlo[i] = FALSE;
    }
    fast_lhi = fast_llo = fast_rhi = fast_rlo = NULL;
    fast_buf_size = 0;
    paula14_current = 0;

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

    /* Buffer size: samples per frame + headroom.
     * 4.5   = blip_buf rounding headroom.
     * 200.0 = PiStorm/Emu68 timer latency margin (same rationale as audio_paula.c).
     * With 60 samples (1.45ms), ~7-8% of frames produced audible gaps on PiStorm.
     * 200 samples = 4.85ms at 41242Hz reduces gap rate to < 0.03%. */
    fps = game_pal ? 50.0 : 60.0;
    spf = (double)paula_freq / fps;
    paula_buflen = (ULONG)(spf + 4.5 + (double)paula14_margin);
    paula_buflen = (paula_buflen + 1) & ~1;  /* word-align for DMA */

    /* Create message port */
    paula14_port = CreateMsgPort();
    if (!paula14_port) goto fail;

    /* Allocate all 4 channels via audio.device (slot 0 opens the device) */
    io_lhi[0] = alloc_chan(paula14_port, chan_mask_lhi, 1);
    if (!io_lhi[0]) { printf("Paula14: cannot alloc AUD%d\n", CHAN_LHI); goto fail; }
    io_llo[0] = alloc_chan(paula14_port, chan_mask_llo, 1);
    if (!io_llo[0]) { printf("Paula14: cannot alloc AUD%d\n", CHAN_LLO); goto fail; }
    io_rhi[0] = alloc_chan(paula14_port, chan_mask_rhi, 1);
    if (!io_rhi[0]) { printf("Paula14: cannot alloc AUD%d\n", CHAN_RHI); goto fail; }
    io_rlo[0] = alloc_chan(paula14_port, chan_mask_rlo, 1);
    if (!io_rlo[0]) { printf("Paula14: cannot alloc AUD%d\n", CHAN_RLO); goto fail; }

    /* Duplicate IO requests for remaining buffer slots */
    for (i = 1; i < PAULA_NUM_BUFFERS; i++)
    {
        io_lhi[i] = (struct IOAudio *)CreateIORequest(paula14_port, sizeof(struct IOAudio));
        io_llo[i] = (struct IOAudio *)CreateIORequest(paula14_port, sizeof(struct IOAudio));
        io_rhi[i] = (struct IOAudio *)CreateIORequest(paula14_port, sizeof(struct IOAudio));
        io_rlo[i] = (struct IOAudio *)CreateIORequest(paula14_port, sizeof(struct IOAudio));
        if (!io_lhi[i] || !io_llo[i] || !io_rhi[i] || !io_rlo[i])
        { printf("Paula14: cannot create IO request %d\n", i); goto fail; }
        *io_lhi[i] = *io_lhi[0];
        *io_llo[i] = *io_llo[0];
        *io_rhi[i] = *io_rhi[0];
        *io_rlo[i] = *io_rlo[0];
    }

    /* Allocate chip RAM DMA buffers */
    for (i = 0; i < PAULA_NUM_BUFFERS; i++)
    {
        buf_lhi[i] = (BYTE *)AllocMem(paula_buflen, MEMF_CHIP | MEMF_CLEAR);
        buf_llo[i] = (BYTE *)AllocMem(paula_buflen, MEMF_CHIP | MEMF_CLEAR);
        buf_rhi[i] = (BYTE *)AllocMem(paula_buflen, MEMF_CHIP | MEMF_CLEAR);
        buf_rlo[i] = (BYTE *)AllocMem(paula_buflen, MEMF_CHIP | MEMF_CLEAR);
        if (!buf_lhi[i] || !buf_llo[i] || !buf_rhi[i] || !buf_rlo[i])
        { printf("Paula14: out of chip RAM\n"); goto fail; }
    }

    /* Fast RAM staging (non-fatal) */
    fast_buf_size = paula_buflen;
    fast_lhi = (BYTE *)AllocMem(fast_buf_size, MEMF_FAST | MEMF_CLEAR);
    fast_llo = (BYTE *)AllocMem(fast_buf_size, MEMF_FAST | MEMF_CLEAR);
    fast_rhi = (BYTE *)AllocMem(fast_buf_size, MEMF_FAST | MEMF_CLEAR);
    fast_rlo = (BYTE *)AllocMem(fast_buf_size, MEMF_FAST | MEMF_CLEAR);
    if (!fast_lhi || !fast_llo || !fast_rhi || !fast_rlo)
    {
        printf("Paula14: fast staging alloc failed (non-fatal)\n");
        if (fast_lhi) { FreeMem(fast_lhi, fast_buf_size); fast_lhi = NULL; }
        if (fast_llo) { FreeMem(fast_llo, fast_buf_size); fast_llo = NULL; }
        if (fast_rhi) { FreeMem(fast_rhi, fast_buf_size); fast_rhi = NULL; }
        if (fast_rlo) { FreeMem(fast_rlo, fast_buf_size); fast_rlo = NULL; }
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
    int i;

    if (paula14_open)
    {
        /* Abort and wait for all pending requests */
        for (i = 0; i < PAULA_NUM_BUFFERS; i++)
        {
            if (req_pending_lhi[i] && io_lhi[i])
            { AbortIO((struct IORequest *)io_lhi[i]); WaitIO((struct IORequest *)io_lhi[i]); req_pending_lhi[i] = FALSE; }
            if (req_pending_llo[i] && io_llo[i])
            { AbortIO((struct IORequest *)io_llo[i]); WaitIO((struct IORequest *)io_llo[i]); req_pending_llo[i] = FALSE; }
            if (req_pending_rhi[i] && io_rhi[i])
            { AbortIO((struct IORequest *)io_rhi[i]); WaitIO((struct IORequest *)io_rhi[i]); req_pending_rhi[i] = FALSE; }
            if (req_pending_rlo[i] && io_rlo[i])
            { AbortIO((struct IORequest *)io_rlo[i]); WaitIO((struct IORequest *)io_rlo[i]); req_pending_rlo[i] = FALSE; }
        }

        paula14_open = FALSE;
    }

    /* Close devices (only slot 0 opened the device). alloc_chan() deletes
     * the request when OpenDevice() fails, so a non-NULL slot-0 request is
     * always an open channel. Done outside the paula14_open block (which
     * is only set at the very end of init) so a partial init failure -
     * e.g. AUD0 opened, AUD1 busy, or chip RAM short - releases the
     * channels it took instead of leaving them allocated until reboot. */
    if (io_lhi[0]) CloseDevice((struct IORequest *)io_lhi[0]);
    if (io_llo[0]) CloseDevice((struct IORequest *)io_llo[0]);
    if (io_rhi[0]) CloseDevice((struct IORequest *)io_rhi[0]);
    if (io_rlo[0]) CloseDevice((struct IORequest *)io_rlo[0]);

    /* Free IO requests */
    for (i = 0; i < PAULA_NUM_BUFFERS; i++)
    {
        if (io_lhi[i]) { DeleteIORequest((struct IORequest *)io_lhi[i]); io_lhi[i] = NULL; }
        if (io_llo[i]) { DeleteIORequest((struct IORequest *)io_llo[i]); io_llo[i] = NULL; }
        if (io_rhi[i]) { DeleteIORequest((struct IORequest *)io_rhi[i]); io_rhi[i] = NULL; }
        if (io_rlo[i]) { DeleteIORequest((struct IORequest *)io_rlo[i]); io_rlo[i] = NULL; }
    }

    /* Free chip RAM buffers */
    for (i = 0; i < PAULA_NUM_BUFFERS; i++)
    {
        if (buf_lhi[i]) { FreeMem(buf_lhi[i], paula_buflen); buf_lhi[i] = NULL; }
        if (buf_llo[i]) { FreeMem(buf_llo[i], paula_buflen); buf_llo[i] = NULL; }
        if (buf_rhi[i]) { FreeMem(buf_rhi[i], paula_buflen); buf_rhi[i] = NULL; }
        if (buf_rlo[i]) { FreeMem(buf_rlo[i], paula_buflen); buf_rlo[i] = NULL; }
    }

    /* Free fast RAM staging */
    if (fast_lhi) { FreeMem(fast_lhi, fast_buf_size); fast_lhi = NULL; }
    if (fast_llo) { FreeMem(fast_llo, fast_buf_size); fast_llo = NULL; }
    if (fast_rhi) { FreeMem(fast_rhi, fast_buf_size); fast_rhi = NULL; }
    if (fast_rlo) { FreeMem(fast_rlo, fast_buf_size); fast_rlo = NULL; }
    fast_buf_size = 0;

    if (paula14_port) { DeleteMsgPort(paula14_port); paula14_port = NULL; }
}

/****************************************************************************
 * Availability — non-blocking poll
 ****************************************************************************/
int paula14_audio_available(void)
{
    int i;
    if (!paula14_open) return 0;
    for (i = 0; i < PAULA_NUM_BUFFERS; i++)
    {
        int slot = (paula14_current + i) % PAULA_NUM_BUFFERS;
        if (!req_pending_lhi[slot])
            return 1;
        if (CheckIO((struct IORequest *)io_lhi[slot]))
            return 1;
    }
    return 0;
}

/****************************************************************************
 * Submit audio buffer — fully non-blocking
 ****************************************************************************/
void paula14_audio_submit(signed short *buffer, int samples)
{
    int fill_buf, slot, num, pad, s;
    BYTE *dlhi, *dllo, *drhi, *drlo;
    struct IOAudio *ilhi, *illo, *irhi, *irlo;

    if (!paula14_open)
        return;

    /* Find a free slot without blocking */
    fill_buf = -1;
    for (s = 0; s < PAULA_NUM_BUFFERS; s++)
    {
        slot = (paula14_current + s) % PAULA_NUM_BUFFERS;
        if (!req_pending_lhi[slot])
        { fill_buf = slot; break; }
        if (CheckIO((struct IORequest *)io_lhi[slot]))
        {
            WaitIO((struct IORequest *)io_lhi[slot]); req_pending_lhi[slot] = FALSE;
            WaitIO((struct IORequest *)io_llo[slot]); req_pending_llo[slot] = FALSE;
            WaitIO((struct IORequest *)io_rhi[slot]); req_pending_rhi[slot] = FALSE;
            WaitIO((struct IORequest *)io_rlo[slot]); req_pending_rlo[slot] = FALSE;
            fill_buf = slot; break;
        }
    }

    if (fill_buf < 0)
    {
        /* All slots still playing - drop this audio frame silently.
         * Paula DMA continues uninterrupted.  WaitIO would stall 0.5-2ms
         * on PiStorm (interrupt round-trip), cascading into underruns. */
        paula14_dbg_dropped++;
        paula14_current = (paula14_current + 1) % PAULA_NUM_BUFFERS;
        return;
    }

    /* Fill the chosen slot */
    num = samples;
    if (num > (int)paula_buflen) num = (int)paula_buflen;
    if (num < 2) num = 2;
    num &= ~1;

    dlhi = fast_lhi ? fast_lhi : buf_lhi[fill_buf];
    dllo = fast_llo ? fast_llo : buf_llo[fill_buf];
    drhi = fast_rhi ? fast_rhi : buf_rhi[fill_buf];
    drlo = fast_rlo ? fast_rlo : buf_rlo[fill_buf];

    if (buffer && num > 0)
    {
#ifdef USE_ASM_AUDIO
        Convert14Bit_asm(buffer, dlhi, dllo, drhi, drlo, num, volume_boost);
#else
        {
            int shift = volume_boost;
            for (s = 0; s < num; s++)
            {
                int sl = (int)buffer[s * 2];
                int sr = (int)buffer[s * 2 + 1];
                UWORD ul, ur;
                sl <<= shift;  if (sl >  32767) sl =  32767;  if (sl < -32768) sl = -32768;
                sr <<= shift;  if (sr >  32767) sr =  32767;  if (sr < -32768) sr = -32768;
                ul = (UWORD)(WORD)sl;  ur = (UWORD)(WORD)sr;
                dlhi[s] = (BYTE)((WORD)ul >> 8);
                dllo[s] = (BYTE)((ul & 0xFF) >> 2);
                drhi[s] = (BYTE)((WORD)ur >> 8);
                drlo[s] = (BYTE)((ur & 0xFF) >> 2);
            }
        }
#endif
        if (fast_lhi)
        {
            CopyMem(fast_lhi, buf_lhi[fill_buf], num);
            CopyMem(fast_llo, buf_llo[fill_buf], num);
            CopyMem(fast_rhi, buf_rhi[fill_buf], num);
            CopyMem(fast_rlo, buf_rlo[fill_buf], num);
        }
        /* Pad tail with last sample value to avoid DC step click at
         * buffer boundary (all 4 channels, ioa_Length = paula_buflen fixed). */
        if (num < (int)paula_buflen)
        {
            int pad = paula_buflen - num;
            memset(buf_lhi[fill_buf] + num, buf_lhi[fill_buf][num - 1], pad);
            memset(buf_llo[fill_buf] + num, buf_llo[fill_buf][num - 1], pad);
            memset(buf_rhi[fill_buf] + num, buf_rhi[fill_buf][num - 1], pad);
            memset(buf_rlo[fill_buf] + num, buf_rlo[fill_buf][num - 1], pad);
        }
    }
    else
    {
        memset(buf_lhi[fill_buf], 0, paula_buflen);
        memset(buf_llo[fill_buf], 0, paula_buflen);
        memset(buf_rhi[fill_buf], 0, paula_buflen);
        memset(buf_rlo[fill_buf], 0, paula_buflen);
    }

    /* Set up IO requests */
    ilhi = io_lhi[fill_buf];
    illo = io_llo[fill_buf];
    irhi = io_rhi[fill_buf];
    irlo = io_rlo[fill_buf];

    ilhi->ioa_Request.io_Command = CMD_WRITE;
    ilhi->ioa_Request.io_Flags   = ADIOF_PERVOL;
    ilhi->ioa_Data   = (UBYTE *)buf_lhi[fill_buf];
    ilhi->ioa_Length = paula_buflen;
    ilhi->ioa_Period = paula_period;
    ilhi->ioa_Volume = 64;
    ilhi->ioa_Cycles = 1;

    illo->ioa_Request.io_Command = CMD_WRITE;
    illo->ioa_Request.io_Flags   = ADIOF_PERVOL;
    illo->ioa_Data   = (UBYTE *)buf_llo[fill_buf];
    illo->ioa_Length = paula_buflen;
    illo->ioa_Period = paula_period;
    illo->ioa_Volume = 1;
    illo->ioa_Cycles = 1;

    irhi->ioa_Request.io_Command = CMD_WRITE;
    irhi->ioa_Request.io_Flags   = ADIOF_PERVOL;
    irhi->ioa_Data   = (UBYTE *)buf_rhi[fill_buf];
    irhi->ioa_Length = paula_buflen;
    irhi->ioa_Period = paula_period;
    irhi->ioa_Volume = 64;
    irhi->ioa_Cycles = 1;

    irlo->ioa_Request.io_Command = CMD_WRITE;
    irlo->ioa_Request.io_Flags   = ADIOF_PERVOL;
    irlo->ioa_Data   = (UBYTE *)buf_rlo[fill_buf];
    irlo->ioa_Length = paula_buflen;
    irlo->ioa_Period = paula_period;
    irlo->ioa_Volume = 1;
    irlo->ioa_Cycles = 1;

    /* All 4 channels start simultaneously — no phase drift */
    Disable();
    BeginIO((struct IORequest *)ilhi);
    BeginIO((struct IORequest *)illo);
    BeginIO((struct IORequest *)irhi);
    BeginIO((struct IORequest *)irlo);
    Enable();

    req_pending_lhi[fill_buf] = TRUE;
    req_pending_llo[fill_buf] = TRUE;
    req_pending_rhi[fill_buf] = TRUE;
    req_pending_rlo[fill_buf] = TRUE;

    paula14_dbg_submitted++;
    paula14_current = (paula14_current + 1) % PAULA_NUM_BUFFERS;
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
        if (req_pending_lhi[i] && !CheckIO((struct IORequest *)io_lhi[i]))
            count++;
    return count;
}
