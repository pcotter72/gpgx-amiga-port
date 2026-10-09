/****************************************************************************
 *  audio_paula.c
 *
 *  Paula Audio Backend for Genesis Plus GX Amiga Port
 *  Direct hardware DMA playback - zero CPU overhead
 *  Based on Atari800 implementation
 *
 *  Advantages over AHI:
 *  - Hardware DMA: CPU free during playback
 *  - Lower latency: Direct hardware access
 *  - Triple-buffered: Smooth playback
 *  - Fixed-point timing: No audio drift
 *
 ****************************************************************************/

#include <stdio.h>
#include <string.h>

#include <exec/exec.h>
#include <exec/memory.h>
#include <devices/audio.h>

#include <proto/exec.h>
#include <clib/alib_protos.h>

#include "audio_paula.h"

/* Assembly conversion routines */
#ifdef USE_ASM_AUDIO
extern void ConvertAudio_asm(signed short *src, signed char *dst, int samples);
extern void ConvertMono8_asm(signed short *src, signed char *dst, int samples);
extern void ConvertStereo8_asm(signed short *src, signed char *dst_left, signed char *dst_right, int samples);
#endif

/* Paula clock rates */
#define PAULA_CLOCK_PAL  3546895UL
#define PAULA_CLOCK_NTSC 3579545UL

/* Optimal periods for minimal distortion (from Richter's analysis):
 * Period 64:  55420 Hz PAL / 55930 Hz NTSC (requires double-scan screen)
 * Period 128: 27710 Hz PAL / 27965 Hz NTSC (works in all modes)
 * Period 86:  41243 Hz PAL / 41623 Hz NTSC (RTG only, no bitplane DMA)
 * Period 80:  44336 Hz PAL / 44744 Hz NTSC (RTG only, absolute max safe)
 *
 * Using period 128 avoids DMA starvation issues and maps 1:2 to DAC rate,
 * minimizing aliasing artifacts.
 */
#define OPTIMAL_PERIOD 128

/* Quad buffering for smooth playback on Pi 3.
 * 4 buffers = up to 4 frames of audio queued ahead (~67ms at 60fps).
 * Extra headroom absorbs occasional heavy frames without a stall.
 * Chip RAM cost: 4 x ~462 bytes x 2 channels = ~3.7 KB (negligible).
 */
#define PAULA_NUM_BUFFERS 4

/* Paula resources */
static struct MsgPort *paula_port = NULL;
static struct IOAudio *paula_io_left[PAULA_NUM_BUFFERS];
static struct IOAudio *paula_io_right[PAULA_NUM_BUFFERS];
static BYTE *paula_buf[PAULA_NUM_BUFFERS];       /* Left (or mono) buffers */
static BYTE *paula_buf_right[PAULA_NUM_BUFFERS];  /* Right channel buffers (stereo only) */

/* Channel allocation masks */
static UBYTE paula_left_channels[] = {1, 8};    /* Channel 0 or 3 */
static UBYTE paula_right_channels[] = {2, 4};   /* Channel 1 or 2 */

/* State */
static BOOL paula_open = FALSE;
/* Per-channel OpenDevice() success. paula_open is only set once BOTH are
 * open, so shutdown must not use it to decide CloseDevice(): a left
 * channel opened before the right one failed would stay allocated. */
static BOOL paula_left_dev_open = FALSE;
static BOOL paula_right_dev_open = FALSE;
static int paula_current = 0;
static BOOL req_pending_left[PAULA_NUM_BUFFERS];
static BOOL req_pending_right[PAULA_NUM_BUFFERS];

/* Debug counters - reset by paula_audio_get_stats() */
int paula_dbg_submitted = 0;
int paula_dbg_dropped   = 0;

/* Audio parameters */
static ULONG paula_freq = 22050;
static ULONG paula_buflen = 0;
static UWORD paula_period = 0;
static int paula_is_pal = 1;
static int paula_margin = 0;  /* extra samples beyond frame; 0=WinUAE, 200=PiStorm */

/* Conversion buffer for 16-bit stereo to 8-bit mono */
static BYTE *convert_buffer = NULL;
static int convert_buffer_size = 0;

/* Stereo mode: 0 = mono (same data both channels), 1 = true stereo */

/* Fast RAM staging buffers - avoid per-byte chip bus writes on PiStorm */
static BYTE *fast_buf_left = NULL;
static BYTE *fast_buf_right = NULL;
static int fast_buf_size = 0;
static int paula_stereo = 0;

/****************************************************************************
 * Initialize Paula audio
 * frequency: Desired sample rate in Hz (0 = use optimal 27710Hz)
 *            Paula will snap to nearest achievable rate based on period.
 *            Minimum period is 80 (~44kHz, RTG only) or 124 (~28.6kHz native).
 *            In RTG modes without bitplane DMA, Paula can safely go much higher.
 * is_pal: 1 for PAL (50Hz), 0 for NTSC (60Hz)
 ****************************************************************************/
int paula_audio_init(int frequency, int hw_pal, int game_pal)
{
    ULONG paula_clock;
    double fps_exact;
    double samples_per_frame;
    UWORD period;
    int i;

    /* Clean up if already initialized */
    if (paula_open)
        paula_audio_shutdown();

    /* Initialize arrays */
    for (i = 0; i < PAULA_NUM_BUFFERS; i++)
    {
        paula_io_left[i] = NULL;
        paula_io_right[i] = NULL;
        paula_buf[i] = NULL;
        paula_buf_right[i] = NULL;
        req_pending_left[i] = FALSE;
        req_pending_right[i] = FALSE;
    }

    /* Paula clock = real Amiga hardware crystal (hw_pal).
     * Frame rate = emulated game's video mode (game_pal).
     * These differ when running e.g. NTSC Sonic on a PAL A1200. */
    paula_is_pal = hw_pal;
    paula_clock = hw_pal ? PAULA_CLOCK_PAL : PAULA_CLOCK_NTSC;

    if (frequency <= 0)
    {
        /* Default: optimal period 128 */
        period = OPTIMAL_PERIOD;
    }
    else
    {
        /* Calculate period from desired frequency */
        period = (UWORD)(paula_clock / (ULONG)frequency);

        /* Clamp: minimum period 80 for RTG (no bitplane DMA contention),
         * maximum 1024 for reasonable audio quality */
        if (period < 80) period = 80;
        if (period > 1024) period = 1024;

        /* Make even for clean DMA timing */
        period = (period + 1) & ~1;
    }

    paula_period = period;
    paula_freq = paula_clock / period;  /* Actual achieved frequency */

    /* Use nominal fps (50/60) for buffer sizing.
     * Genesis actual fps (49.701/59.922) would oversize the buffer by ~4 samples,
     * causing more frequent silent drops (~every 16s vs ~every 6min).
     * With drop-on-full and last-sample tail padding neither causes audible
     * artifacts, but nominal fps keeps the buffer tight and drops rare. */
    fps_exact = game_pal ? 50.0 : 60.0;

    /* Buffer size: samples per frame + margin.
     *
     *   4.5          = blip_buf rounding (occasionally produces 1-4 extra samples)
     *   paula_margin = timer latency margin (see paula_set_margin())
     *
     * Default (margin=0): buffer ≈ samples_per_frame + 4.5 ≈ 692 samples (NTSC,
     * 41242Hz) = 16.78ms. Minimal excess (0.11ms/frame) → queue fills in ~610 frames
     * (~10s) causing one silent drop, then resets. Inaudible.
     *
     * margin=200: 892 samples (NTSC) = 21.62ms. For PiStorm/Emu68 where timer.device
     * spikes to >5ms late. WARNING: excess of 4.96ms/frame fills 4-buffer queue in
     * ~17 frames, then drops ~22% of audio frames → audible 15Hz trembling.
     * Only use on PiStorm/Emu68. */
    samples_per_frame = (double)paula_freq / fps_exact;
    paula_buflen = (ULONG)(samples_per_frame + 4.5 + (double)paula_margin);
    paula_buflen = (paula_buflen + 1) & ~1;  /* Word-align for DMA */

    /* Create message port */
    paula_port = CreateMsgPort();
    if (!paula_port)
    {
        printf("Paula: Cannot create message port\n");
        return 0;
    }

    /* Create and open LEFT channel */
    paula_io_left[0] = (struct IOAudio *)CreateIORequest(paula_port, sizeof(struct IOAudio));
    if (!paula_io_left[0])
    {
        printf("Paula: Cannot create left IO request\n");
        paula_audio_shutdown();
        return 0;
    }

    paula_io_left[0]->ioa_Request.io_Message.mn_Node.ln_Pri = 0;
    paula_io_left[0]->ioa_Request.io_Command = ADCMD_ALLOCATE;
    paula_io_left[0]->ioa_Request.io_Flags = ADIOF_NOWAIT;
    paula_io_left[0]->ioa_AllocKey = 0;
    paula_io_left[0]->ioa_Data = paula_left_channels;
    paula_io_left[0]->ioa_Length = sizeof(paula_left_channels);

    if (OpenDevice("audio.device", 0, (struct IORequest *)paula_io_left[0], 0))
    {
        printf("Paula: Cannot open audio.device for left channel\n");
        paula_audio_shutdown();
        return 0;
    }
    paula_left_dev_open = TRUE;

    /* Create and open RIGHT channel */
    paula_io_right[0] = (struct IOAudio *)CreateIORequest(paula_port, sizeof(struct IOAudio));
    if (!paula_io_right[0])
    {
        printf("Paula: Cannot create right IO request\n");
        paula_audio_shutdown();
        return 0;
    }

    paula_io_right[0]->ioa_Request.io_Message.mn_Node.ln_Pri = 0;
    paula_io_right[0]->ioa_Request.io_Command = ADCMD_ALLOCATE;
    paula_io_right[0]->ioa_Request.io_Flags = ADIOF_NOWAIT;
    paula_io_right[0]->ioa_AllocKey = 0;
    paula_io_right[0]->ioa_Data = paula_right_channels;
    paula_io_right[0]->ioa_Length = sizeof(paula_right_channels);

    if (OpenDevice("audio.device", 0, (struct IORequest *)paula_io_right[0], 0))
    {
        printf("Paula: Cannot open audio.device for right channel\n");
        paula_audio_shutdown();
        return 0;
    }
    paula_right_dev_open = TRUE;

    paula_open = TRUE;

    /* Create additional IO requests for triple-buffering */
    for (i = 1; i < PAULA_NUM_BUFFERS; i++)
    {
        paula_io_left[i] = (struct IOAudio *)CreateIORequest(paula_port, sizeof(struct IOAudio));
        paula_io_right[i] = (struct IOAudio *)CreateIORequest(paula_port, sizeof(struct IOAudio));

        if (!paula_io_left[i] || !paula_io_right[i])
        {
            printf("Paula: Cannot create IO request %d\n", i);
            paula_audio_shutdown();
            return 0;
        }

        /* Copy allocated channel info */
        *paula_io_left[i] = *paula_io_left[0];
        *paula_io_right[i] = *paula_io_right[0];
    }

    /* Allocate audio buffers in CHIP memory - REQUIRED for Paula DMA! */
    for (i = 0; i < PAULA_NUM_BUFFERS; i++)
    {
        paula_buf[i] = (BYTE *)AllocMem(paula_buflen, MEMF_CHIP | MEMF_CLEAR);
        if (!paula_buf[i])
        {
            printf("Paula: Cannot allocate CHIP RAM buffer %d\n", i);
            paula_audio_shutdown();
            return 0;
        }
        /* Always allocate right buffers - small cost, enables runtime stereo */
        paula_buf_right[i] = (BYTE *)AllocMem(paula_buflen, MEMF_CHIP | MEMF_CLEAR);
        if (!paula_buf_right[i])
        {
            printf("Paula: Cannot allocate CHIP RAM right buffer %d\n", i);
            paula_audio_shutdown();
            return 0;
        }
    }

    /* Allocate conversion buffer in any RAM */
    convert_buffer_size = paula_buflen;
    convert_buffer = (BYTE *)AllocMem(convert_buffer_size, MEMF_PUBLIC);
    if (!convert_buffer)
    {
        printf("Paula: Cannot allocate conversion buffer\n");
        paula_audio_shutdown();
        return 0;
    }

    /* Allocate Fast RAM staging buffers to avoid per-byte chip bus writes.
     * On PiStorm, chip RAM writes go over the slow 68000 bus.
     * By converting to Fast RAM first, then bulk CopyMem to chip RAM,
     * we reduce bus contention significantly. */
    fast_buf_size = paula_buflen;
    fast_buf_left = (BYTE *)AllocMem(fast_buf_size, MEMF_FAST | MEMF_CLEAR);
    fast_buf_right = (BYTE *)AllocMem(fast_buf_size, MEMF_FAST | MEMF_CLEAR);
    if (!fast_buf_left || !fast_buf_right)
    {
        printf("Paula: Fast staging alloc failed (non-fatal, using chip direct)\n");
        if (fast_buf_left) { FreeMem(fast_buf_left, fast_buf_size); fast_buf_left = NULL; }
        if (fast_buf_right) { FreeMem(fast_buf_right, fast_buf_size); fast_buf_right = NULL; }
        fast_buf_size = 0;
    }

    paula_current = 0;

    return 1;
}

/****************************************************************************
 * Shutdown Paula audio
 ****************************************************************************/
void paula_audio_shutdown(void)
{
    int i;

    if (paula_open)
    {
        /* Abort and wait for all pending requests */
        for (i = 0; i < PAULA_NUM_BUFFERS; i++)
        {
            if (req_pending_left[i] && paula_io_left[i])
            {
                AbortIO((struct IORequest *)paula_io_left[i]);
                WaitIO((struct IORequest *)paula_io_left[i]);
                req_pending_left[i] = FALSE;
            }
            if (req_pending_right[i] && paula_io_right[i])
            {
                AbortIO((struct IORequest *)paula_io_right[i]);
                WaitIO((struct IORequest *)paula_io_right[i]);
                req_pending_right[i] = FALSE;
            }
        }

        paula_open = FALSE;
    }

    /* Close devices (frees the allocated channels). Outside the paula_open
     * block so a partial init failure also releases what it opened. */
    if (paula_left_dev_open)
    {
        CloseDevice((struct IORequest *)paula_io_left[0]);
        paula_left_dev_open = FALSE;
    }
    if (paula_right_dev_open)
    {
        CloseDevice((struct IORequest *)paula_io_right[0]);
        paula_right_dev_open = FALSE;
    }

    /* Free IO requests */
    for (i = 0; i < PAULA_NUM_BUFFERS; i++)
    {
        if (paula_io_left[i])
        {
            DeleteIORequest((struct IORequest *)paula_io_left[i]);
            paula_io_left[i] = NULL;
        }
        if (paula_io_right[i])
        {
            DeleteIORequest((struct IORequest *)paula_io_right[i]);
            paula_io_right[i] = NULL;
        }
    }

    /* Free CHIP RAM buffers */
    for (i = 0; i < PAULA_NUM_BUFFERS; i++)
    {
        if (paula_buf[i])
        {
            FreeMem(paula_buf[i], paula_buflen);
            paula_buf[i] = NULL;
        }
        if (paula_buf_right[i])
        {
            FreeMem(paula_buf_right[i], paula_buflen);
            paula_buf_right[i] = NULL;
        }
    }

    /* Free conversion buffer */
    if (convert_buffer)
    {
        FreeMem(convert_buffer, convert_buffer_size);
        convert_buffer = NULL;
    }

    /* Free fast RAM staging buffers */
    if (fast_buf_left)
    {
        FreeMem(fast_buf_left, fast_buf_size);
        fast_buf_left = NULL;
    }
    if (fast_buf_right)
    {
        FreeMem(fast_buf_right, fast_buf_size);
        fast_buf_right = NULL;
    }
    fast_buf_size = 0;

    /* Free message port */
    if (paula_port)
    {
        DeleteMsgPort(paula_port);
        paula_port = NULL;
    }

    paula_open = FALSE;
}

/****************************************************************************
 * Convert 16-bit signed stereo to 8-bit signed mono
 * Genesis outputs 16-bit stereo, Paula needs 8-bit signed
 ****************************************************************************/

/* Volume boost is always compiled - used by both C and ASM paths */
int paula_volume_boost = 0;  /* shift left by N = multiply by 2^N */
                              /* 0=1x, 1=2x, 2=4x, 3=8x */

void paula_set_margin(int margin)
{
    if (margin < 0)   margin = 0;
    if (margin > 882) margin = 882;
    paula_margin = margin;
}

void paula_set_volume_boost(int boost)
{
    if (boost < 0) boost = 0;
    if (boost > 4) boost = 4;
    paula_volume_boost = boost;
}

void paula_set_stereo(int stereo)
{
    paula_stereo = stereo ? 1 : 0;
}

/* C conversion functions - always compiled (asm versions not used for 8-bit) */

static void convert_16bit_stereo_to_8bit_mono(signed short *src, BYTE *dst, int samples)
{
    int i;
    int shift = paula_volume_boost;
    BYTE *fast = fast_buf_left ? fast_buf_left : dst;

    for (i = 0; i < samples; i++)
    {
        int sum = (int)src[i * 2] + (int)src[i * 2 + 1];
        sum <<= shift;
        sum = (sum + 256) >> 9;
        if (sum > 127) sum = 127;
        if (sum < -128) sum = -128;
        fast[i] = (BYTE)sum;
    }

    /* Bulk copy from Fast RAM to Chip RAM */
    if (fast != dst)
        CopyMem(fast, dst, samples);
}

static void convert_16bit_stereo_to_8bit_stereo(signed short *src, BYTE *dst_left, BYTE *dst_right, int samples)
{
    int i;
    int shift = paula_volume_boost;
    BYTE *fast_l = fast_buf_left ? fast_buf_left : dst_left;
    BYTE *fast_r = fast_buf_right ? fast_buf_right : dst_right;

    for (i = 0; i < samples; i++)
    {
        int l = (int)src[i * 2];
        int r = (int)src[i * 2 + 1];
        l <<= shift;
        r <<= shift;
        l = (l + 128) >> 8;  /* round to nearest (matches mono path) */
        r = (r + 128) >> 8;
        if (l > 127) l = 127;
        if (l < -128) l = -128;
        if (r > 127) r = 127;
        if (r < -128) r = -128;
        fast_l[i] = (BYTE)l;
        fast_r[i] = (BYTE)r;
    }

    /* Bulk copy from Fast RAM to Chip RAM */
    if (fast_l != dst_left)
        CopyMem(fast_l, dst_left, samples);
    if (fast_r != dst_right)
        CopyMem(fast_r, dst_right, samples);
}



/****************************************************************************
 * Check if Paula can accept a new buffer - NON-BLOCKING poll
 *
 * Scans all slots for one that is free or already completed via CheckIO.
 * Never calls WaitIO - returns 0 immediately if all slots are busy.
 ****************************************************************************/
int paula_audio_available(void)
{
    int i;
    if (!paula_open)
        return 0;
    for (i = 0; i < PAULA_NUM_BUFFERS; i++)
    {
        int slot = (paula_current + i) % PAULA_NUM_BUFFERS;
        if (!req_pending_left[slot])
            return 1;
        if (CheckIO((struct IORequest *)paula_io_left[slot]))
            return 1;
    }
    return 0;
}

/****************************************************************************
 * Submit audio buffer to Paula - FULLY NON-BLOCKING
 ****************************************************************************/
void paula_audio_submit(signed short *buffer, int samples)
{
    struct IOAudio *io_left, *io_right;
    BYTE *buf;
    int samples_to_output;
    int fill_buf;
    int i;

    if (!paula_open)
        return;

    /* --- Find a free slot without blocking --- */
    fill_buf = -1;
    for (i = 0; i < PAULA_NUM_BUFFERS; i++)
    {
        int slot = (paula_current + i) % PAULA_NUM_BUFFERS;

        if (!req_pending_left[slot])
        {
            /* Slot was never queued or was already harvested */
            fill_buf = slot;
            break;
        }

        /* Non-blocking completion check */
        if (CheckIO((struct IORequest *)paula_io_left[slot]))
        {
            /* Request completed - WaitIO is now instant (no blocking) */
            WaitIO((struct IORequest *)paula_io_left[slot]);
            req_pending_left[slot] = FALSE;
            if (req_pending_right[slot])
            {
                WaitIO((struct IORequest *)paula_io_right[slot]);
                req_pending_right[slot] = FALSE;
            }
            fill_buf = slot;
            break;
        }
    }

    if (fill_buf < 0)
    {
        /* All slots still playing - drop this audio frame. */
        paula_dbg_dropped++;
        paula_current = (paula_current + 1) % PAULA_NUM_BUFFERS;
        return;
    }
    paula_dbg_submitted++;

    /* --- Fill the chosen slot --- */
    io_left  = paula_io_left[fill_buf];
    io_right = paula_io_right[fill_buf];
    buf      = paula_buf[fill_buf];

    samples_to_output = samples;
    if (samples_to_output > (int)paula_buflen)
        samples_to_output = (int)paula_buflen;
    if (samples_to_output < 2)
        samples_to_output = 2;
    samples_to_output = samples_to_output & ~1;  /* Keep even for DMA */

    if (buffer && samples_to_output > 0)
    {
        if (paula_stereo)
        {
            BYTE *buf_r = paula_buf_right[fill_buf];
#ifdef USE_ASM_AUDIO
            {
                BYTE *dl = fast_buf_left  ? fast_buf_left  : buf;
                BYTE *dr = fast_buf_right ? fast_buf_right : buf_r;
                ConvertStereo8_asm(buffer, dl, dr, samples_to_output);
                if (fast_buf_left)
                {
                    CopyMem(fast_buf_left,  buf,   samples_to_output);
                    CopyMem(fast_buf_right, buf_r, samples_to_output);
                }
            }
#else
            convert_16bit_stereo_to_8bit_stereo(buffer, buf, buf_r, samples_to_output);
#endif
            /* Pad tail with last sample value to avoid DC step click at
             * buffer boundary (ioa_Length = paula_buflen, fixed). */
            if (samples_to_output < (int)paula_buflen)
            {
                int pad = paula_buflen - samples_to_output;
                memset(buf   + samples_to_output, buf[samples_to_output - 1],   pad);
                memset(buf_r + samples_to_output, buf_r[samples_to_output - 1], pad);
            }
        }
        else
        {
#ifdef USE_ASM_AUDIO
            {
                BYTE *dl = fast_buf_left ? fast_buf_left : buf;
                ConvertMono8_asm(buffer, dl, samples_to_output);
                if (fast_buf_left)
                    CopyMem(fast_buf_left, buf, samples_to_output);
            }
#else
            convert_16bit_stereo_to_8bit_mono(buffer, buf, samples_to_output);
#endif
            /* Pad tail with last sample value to avoid DC step click */
            if (samples_to_output < (int)paula_buflen)
            {
                int pad = paula_buflen - samples_to_output;
                memset(buf + samples_to_output, buf[samples_to_output - 1], pad);
            }
        }
    }
    else
    {
        memset(buf, 0, paula_buflen);
    }

    /* Fixed-length buffers: Paula plays exactly paula_buflen samples each time.
     * Variable ioa_Length causes timing jitter at buffer transitions (audible
     * as clicks).  The tail is padded with the last sample value above to
     * prevent a DC step at the boundary. */
    io_left->ioa_Request.io_Command = CMD_WRITE;
    io_left->ioa_Request.io_Flags   = ADIOF_PERVOL;
    io_left->ioa_Data    = (UBYTE *)buf;
    io_left->ioa_Length  = paula_buflen;
    io_left->ioa_Period  = paula_period;
    io_left->ioa_Volume  = 64;
    io_left->ioa_Cycles  = 1;

    io_right->ioa_Request.io_Command = CMD_WRITE;
    io_right->ioa_Request.io_Flags   = ADIOF_PERVOL;
    io_right->ioa_Data    = paula_stereo ? (UBYTE *)paula_buf_right[fill_buf] : (UBYTE *)buf;
    io_right->ioa_Length  = paula_buflen;
    io_right->ioa_Period  = paula_period;
    io_right->ioa_Volume  = 64;
    io_right->ioa_Cycles  = 1;

    /* Disable/Enable ensures L and R start on the same scheduler tick.
     * Without this, a task switch between the two BeginIO calls causes
     * L/R phase drift that accumulates to audible crackling. */
    Disable();
    BeginIO((struct IORequest *)io_left);
    BeginIO((struct IORequest *)io_right);
    Enable();
    req_pending_left[fill_buf]  = TRUE;
    req_pending_right[fill_buf] = TRUE;

    /* Advance to next slot for the following call */
    paula_current = (paula_current + 1) % PAULA_NUM_BUFFERS;
}

/****************************************************************************
 * Get recommended sample rate for Paula
 ****************************************************************************/
int paula_audio_get_rate(void)
{
    return paula_freq;
}

/****************************************************************************
 * Return number of buffers currently queued (pending) with Paula.
 ****************************************************************************/
int paula_audio_queued(void)
{
    int i, count = 0;
    if (!paula_open)
        return 0;
    for (i = 0; i < PAULA_NUM_BUFFERS; i++)
    {
        if (req_pending_left[i])
        {
            if (!CheckIO((struct IORequest *)paula_io_left[i]))
                count++;
        }
    }
    return count;
}
