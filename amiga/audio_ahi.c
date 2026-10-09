/****************************************************************************
 *  audio_ahi.c
 *
 *  AHI Audio Backend for Genesis Plus GX Amiga Port
 *
 *  APPROACH: Fully non-blocking ring buffer with ahir_Link.
 *
 *  The previous attempts all had the same fundamental problem: WaitIO
 *  blocks the main loop. Even with double buffering and ahir_Link,
 *  if AHI has any internal latency in signaling completion, the WaitIO
 *  stalls the emulation. On Emu68/Amithlon this consistently costs
 *  ~3ms per frame, dropping from 60 to 50 FPS.
 *
 *  Solution: NEVER block. Use a ring of buffers. Collect completed
 *  requests via GetMsg (non-blocking). If no buffer is free, drop the
 *  audio frame - the emulation keeps running at full speed. Use
 *  ahir_Link to chain consecutive requests for gapless playback.
 *
 *  The frame timer in main.c controls emulation speed, not audio.
 *
 ****************************************************************************/

#include <stdio.h>
#include <string.h>

#include <exec/exec.h>
#include <devices/ahi.h>

#include <proto/exec.h>
#include <clib/alib_protos.h>

#include "audio_ahi.h"

/* Ring buffer size - enough to absorb timing jitter.
 * 4 buffers = up to 4 frames of audio queued ahead.
 * More buffers = more latency but less chance of drops. */
#define AHI_NUM_BUFFERS 3

/* AHI resources */
static struct MsgPort *ahi_port = NULL;
static struct AHIRequest *ahi_req[AHI_NUM_BUFFERS];
static BYTE ahi_device = -1;

/* Audio buffers */
static BYTE *ahi_buffer[AHI_NUM_BUFFERS];
static int ahi_buf_max_samples = 0;
static int ahi_buf_max_bytes = 0;

/* Ring buffer state */
static int ahi_write_idx = 0;        /* Next buffer to fill */
static int ahi_pending[AHI_NUM_BUFFERS]; /* 1 = request in flight */
static int ahi_last_sent = -1;       /* Index of most recently sent request (for linking) */

/* Stats for debugging */
static int ahi_drops = 0;            /* Frames dropped due to full ring */
static int ahi_total = 0;            /* Total frames submitted */

/* State */
static int ahi_initialized = 0;
static int ahi_frequency = 44100;

/* Buffer margin - extra samples appended after each frame's audio.
 * 0   = exact frame size: no queue build-up on WinUAE / accurate timers.
 * 220 = ~4.99ms at 44100Hz: prevents ahir_Link chain breaks on PiStorm/Emu68
 *       where timer.device can fire up to 5ms late.
 * Set via ahi_set_margin() before ahi_audio_init(). */
static int ahi_margin = 0;

/****************************************************************************
 * Collect completed requests (non-blocking)
 * Drains all completed messages from the port without blocking.
 ****************************************************************************/
static void ahi_collect_finished(void)
{
    struct Message *msg;
    int i;
    
    while ((msg = GetMsg(ahi_port)) != NULL)
    {
        /* Find which request completed */
        for (i = 0; i < AHI_NUM_BUFFERS; i++)
        {
            if (msg == (struct Message *)ahi_req[i])
            {
                ahi_pending[i] = 0;
                break;
            }
        }
    }
}

/****************************************************************************
 * Find a free buffer (non-blocking)
 * Returns buffer index or -1 if all busy
 ****************************************************************************/
static int ahi_find_free(void)
{
    int i;
    int idx;
    
    /* Start from write_idx and scan forward */
    for (i = 0; i < AHI_NUM_BUFFERS; i++)
    {
        idx = (ahi_write_idx + i) % AHI_NUM_BUFFERS;
        if (!ahi_pending[idx])
            return idx;
    }
    
    return -1;  /* All buffers busy */
}

/****************************************************************************
 * Set buffer margin (call before ahi_audio_init)
 ****************************************************************************/
void ahi_set_margin(int margin)
{
    if (margin < 0)   margin = 0;
    if (margin > 882) margin = 882;  /* cap at one PAL frame worth */
    ahi_margin = margin;
}

/****************************************************************************
 * Initialize AHI audio
 ****************************************************************************/
int ahi_audio_init(int frequency, int stereo, int is_pal)
{
    int i;
    int fps;
    int samples_per_frame;
    (void)stereo;

    ahi_frequency = frequency;

    /* Buffer size: 1 frame of audio + optional margin.
     * Must use correct fps for PAL (50Hz) vs NTSC (60Hz).
     * PAL frames generate 882 samples at 44100Hz; NTSC generates 735.
     * Hardcoding /60 on a PAL ROM truncates 131 samples per frame -> crackling.
     *
     * ahi_margin controls the extra samples:
     *   0   = exact frame size. Each buffer lasts exactly one frame (16.67ms NTSC
     *         / 20ms PAL). No queue build-up; no frame drops on WinUAE or any
     *         platform with an accurate timer. Default.
     *   220 = 4.99ms at 44100Hz. For PiStorm/Emu68 where timer.device fires up
     *         to 5ms late: without this margin the ahir_Link chain breaks ~7% of
     *         frames causing audible crackling. Set ahimargin=220 in the config.
     *
     * WARNING: margin > 0 causes each buffer to outlast its game frame by margin
     * samples. The audio queue grows by (margin/frequency) seconds per frame and
     * fills all AHI_NUM_BUFFERS slots in ~(AHI_NUM_BUFFERS * frequency / margin)
     * frames, after which ~1 in every (frequency/margin + 1) frames is silently
     * dropped. With margin=220 and 3 buffers this is ~25% drops - perceived as
     * a 15Hz amplitude trembling. Use margin=0 unless on PiStorm/Emu68. */
    fps = is_pal ? 50 : 60;
    samples_per_frame = (frequency + fps - 1) / fps;

    /* +4 blip_buf rounding headroom (same as the Paula backends): the core
     * runs at the exact Genesis frame rate (59.9224 Hz NTSC), so blip_buf
     * delivers slightly MORE samples per frame than frequency/60 - e.g. 736
     * at 44100 Hz vs the 735 computed above. Without this headroom the last
     * 1-2 samples were clamped off nearly every frame (waveform skip at
     * each buffer boundary). */
    ahi_buf_max_samples = samples_per_frame + 4 + ahi_margin;
    ahi_buf_max_bytes = ahi_buf_max_samples * 4; /* 16-bit stereo */

    for (i = 0; i < AHI_NUM_BUFFERS; i++)
    {
        ahi_buffer[i] = NULL;
        ahi_req[i] = NULL;
        ahi_pending[i] = 0;
    }
    ahi_write_idx = 0;
    ahi_last_sent = -1;
    ahi_drops = 0;
    ahi_total = 0;
    ahi_device = -1;

    /* Create message port */
    ahi_port = CreateMsgPort();
    if (!ahi_port)
    {
        printf("AHI: Cannot create message port\n");
        return 0;
    }

    /* Create first IO request and open device */
    ahi_req[0] = (struct AHIRequest *)CreateIORequest(ahi_port,
                     sizeof(struct AHIRequest));
    if (!ahi_req[0])
    {
        printf("AHI: Cannot create IO request\n");
        ahi_audio_shutdown();
        return 0;
    }

    ahi_req[0]->ahir_Version = 4;
    ahi_device = OpenDevice(AHINAME, AHI_DEFAULT_UNIT,
                            (struct IORequest *)ahi_req[0], 0);
    if (ahi_device)
    {
        printf("AHI: Cannot open ahi.device\n");
        ahi_audio_shutdown();
        return 0;
    }

    /* Create remaining IO requests as copies */
    for (i = 1; i < AHI_NUM_BUFFERS; i++)
    {
        ahi_req[i] = (struct AHIRequest *)AllocMem(sizeof(struct AHIRequest),
                                                    MEMF_PUBLIC);
        if (!ahi_req[i])
        {
            printf("AHI: Cannot allocate request %d\n", i);
            ahi_audio_shutdown();
            return 0;
        }
        CopyMem(ahi_req[0], ahi_req[i], sizeof(struct AHIRequest));
    }

    /* Allocate audio buffers */
    for (i = 0; i < AHI_NUM_BUFFERS; i++)
    {
        ahi_buffer[i] = (BYTE *)AllocMem(ahi_buf_max_bytes,
                                          MEMF_PUBLIC | MEMF_CLEAR);
        if (!ahi_buffer[i])
        {
            printf("AHI: Cannot allocate buffer %d\n", i);
            ahi_audio_shutdown();
            return 0;
        }
    }

    ahi_initialized = 1;

    /* No priming here. main() queues two silent frames immediately before
     * entering the main loop, which is what establishes the ahir_Link chain.
     * Priming here as well would put 4 buffers into this 3-slot ring: the
     * ring is full when main() primes, so one of its frames is dropped, and
     * these two would have been queued ~100ms earlier (the rest of init) and
     * drained before frame 1 arrives anyway. */

    return 1;
}

/****************************************************************************
 * Shutdown AHI audio
 ****************************************************************************/
void ahi_audio_shutdown(void)
{
    int i;

    ahi_initialized = 0;

    /* Abort and wait for all pending requests */
    for (i = 0; i < AHI_NUM_BUFFERS; i++)
    {
        if (ahi_req[i] && ahi_pending[i] && !ahi_device)
        {
            AbortIO((struct IORequest *)ahi_req[i]);
            WaitIO((struct IORequest *)ahi_req[i]);
            ahi_pending[i] = 0;
        }
    }

    /* Close device */
    if (!ahi_device && ahi_req[0])
    {
        CloseDevice((struct IORequest *)ahi_req[0]);
        ahi_device = -1;
    }

    /* Free buffers */
    for (i = 0; i < AHI_NUM_BUFFERS; i++)
    {
        if (ahi_buffer[i])
        {
            FreeMem(ahi_buffer[i], ahi_buf_max_bytes);
            ahi_buffer[i] = NULL;
        }
    }

    /* Free IO requests: copies with FreeMem, original with DeleteIORequest */
    for (i = AHI_NUM_BUFFERS - 1; i >= 1; i--)
    {
        if (ahi_req[i])
        {
            FreeMem(ahi_req[i], sizeof(struct AHIRequest));
            ahi_req[i] = NULL;
        }
    }
    if (ahi_req[0])
    {
        DeleteIORequest((struct IORequest *)ahi_req[0]);
        ahi_req[0] = NULL;
    }

    /* Delete message port */
    if (ahi_port)
    {
        DeleteMsgPort(ahi_port);
        ahi_port = NULL;
    }
}

/****************************************************************************
 * Submit audio samples for playback - NEVER BLOCKS
 *
 * 1. Collect any finished requests (non-blocking GetMsg)
 * 2. Find a free buffer in the ring
 * 3. If none free: drop this frame's audio (emulation continues)
 * 4. Copy samples, link to previous request, SendIO
 *
 * The ahir_Link ensures gapless playback between consecutive buffers.
 * Dropping occasional frames causes minor audio glitches but keeps
 * the emulation running at full 60 FPS.
 ****************************************************************************/
static int ahi_volume_boost = 0;  /* shift left by N: 0=1x, 1=2x, 2=4x, 3=8x */

void ahi_set_volume_boost(int boost)
{
    if (boost < 0) boost = 0;
    if (boost > 4) boost = 4;
    ahi_volume_boost = boost;
}

void ahi_audio_submit(short *samples, int count)
{
    struct AHIRequest *io;
    int buf;
    int bytes;

    if (!ahi_initialized)
        return;

    ahi_total++;

    /* Collect completed requests - never blocks */
    ahi_collect_finished();

    /* Find a free buffer */
    buf = ahi_find_free();
    if (buf < 0)
    {
        /* All buffers busy - drop this audio frame.
         * Emulation continues at full speed. */
        ahi_drops++;
        return;
    }

    /* Limit to buffer capacity */
    if (count > ahi_buf_max_samples)
        count = ahi_buf_max_samples;

    bytes = count * 4;

    /* Copy and amplify samples to buffer.
     * Post-blip boost: amplify clean 16-bit signal from blip_buf
     * before sending to AHI. Clamp to int16 range. */
    if (ahi_volume_boost > 0)
    {
        short *src = samples;
        short *dst = (short *)ahi_buffer[buf];
        int i;
        int shift = ahi_volume_boost;
        int total = count * 2;  /* stereo: 2 values per sample */
        for (i = 0; i < total; i++)
        {
            int v = (int)src[i] << shift;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            dst[i] = (short)v;
        }
    }
    else
    {
        CopyMem(samples, ahi_buffer[buf], bytes);
    }

    /* Pad tail with last stereo sample pair to fill buffer to ahi_buf_max_samples.
     * Only runs when ahi_margin > 0 (count < ahi_buf_max_samples).
     * Holds the last sample so the margin region is smooth audio, not silence,
     * preventing a DC-step click at buffer boundaries on PiStorm/Emu68. */
    if (count < ahi_buf_max_samples)
    {
        short *dst = (short *)ahi_buffer[buf];
        short last_l = dst[count * 2 - 2];
        short last_r = dst[count * 2 - 1];
        int i;
        for (i = count; i < ahi_buf_max_samples; i++)
        {
            dst[i * 2]     = last_l;
            dst[i * 2 + 1] = last_r;
        }
    }

    /* Setup IO request */
    io = ahi_req[buf];
    io->ahir_Std.io_Message.mn_Node.ln_Pri = 0;
    io->ahir_Std.io_Command = CMD_WRITE;
    io->ahir_Std.io_Data = ahi_buffer[buf];
    io->ahir_Std.io_Length = ahi_buf_max_bytes;
    io->ahir_Std.io_Offset = 0;
    io->ahir_Type = AHIST_S16S;
    io->ahir_Frequency = ahi_frequency;
    io->ahir_Volume = 0x10000;
    io->ahir_Position = 0x8000;

    /* Link to the previously sent request for gapless playback.
     * ahir_Link tells AHI: "start this buffer immediately when
     * the linked request finishes, with no gap." */
    if (ahi_last_sent >= 0 && ahi_pending[ahi_last_sent])
    {
        io->ahir_Link = ahi_req[ahi_last_sent];
    }
    else
    {
        io->ahir_Link = NULL;
    }

    /* Send non-blocking */
    SendIO((struct IORequest *)io);
    ahi_pending[buf] = 1;
    ahi_last_sent = buf;

    /* Advance write index */
    ahi_write_idx = (buf + 1) % AHI_NUM_BUFFERS;
}

/****************************************************************************
 * Wait for all audio to finish (used during shutdown/pause)
 ****************************************************************************/
void ahi_audio_wait(void)
{
    int i;

    if (!ahi_initialized)
        return;

    for (i = 0; i < AHI_NUM_BUFFERS; i++)
    {
        if (ahi_pending[i])
        {
            WaitIO((struct IORequest *)ahi_req[i]);
            ahi_pending[i] = 0;
        }
    }
    ahi_last_sent = -1;
}

/****************************************************************************
 * Check if audio is available
 ****************************************************************************/
int ahi_audio_available(void)
{
    return ahi_initialized;
}

/****************************************************************************
 * Return number of buffers currently queued with AHI.
 * Used by the main loop for adaptive frame timing.
 ****************************************************************************/
int ahi_audio_queued(void)
{
    int i, count = 0;
    if (!ahi_initialized)
        return 0;
    ahi_collect_finished();
    for (i = 0; i < AHI_NUM_BUFFERS; i++)
    {
        if (ahi_pending[i])
            count++;
    }
    return count;
}
