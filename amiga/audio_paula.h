/****************************************************************************
 *  audio_paula.h
 *
 *  Paula Audio Backend for Genesis Plus GX Amiga Port
 *  Direct hardware DMA playback
 *
 ****************************************************************************/

#ifndef _AUDIO_PAULA_H_
#define _AUDIO_PAULA_H_

/* Initialize Paula audio
 * frequency: Sample rate (recommended: 22050 or 28000)
 * hw_pal:   1 if Amiga hardware is PAL (selects Paula crystal clock)
 * game_pal: 1 if emulated game is PAL 50Hz (selects fps for buffer sizing)
 * Returns: 1 on success, 0 on failure
 */
int paula_audio_init(int frequency, int hw_pal, int game_pal);

/* Shutdown Paula audio */
void paula_audio_shutdown(void);

/* Submit audio buffer
 * buffer: 16-bit signed stereo samples (interleaved L/R)
 * samples: Number of stereo sample pairs
 */
void paula_audio_submit(signed short *buffer, int samples);

/* Check if Paula is available */
int paula_audio_available(void);

/* Get current sample rate */
int paula_audio_get_rate(void);

/* Set output volume boost for 8-bit conversion (0=1x, 1=2x, 2=4x, 3=8x, 4=16x) */
void paula_set_volume_boost(int boost);

/* Set buffer margin in extra samples beyond samples_per_frame.
 * 0   = exact frame size (WinUAE / any accurate timer — no queue buildup).
 * 200 = 4.85ms margin (PiStorm/Emu68 — prevents buffer underrun from timer jitter).
 * Must be called before paula_audio_init(). */
void paula_set_margin(int margin);

/* Enable true stereo output (0=mono, 1=stereo with separate L/R channels) */
void paula_set_stereo(int stereo);

/* Return number of buffers currently queued with Paula (0..4) */
int paula_audio_queued(void);

/* Debug counters: frames submitted and dropped since last reset */
extern int paula_dbg_submitted;
extern int paula_dbg_dropped;

#endif /* _AUDIO_PAULA_H_ */
