/****************************************************************************
 *  audio_ahi.h
 *
 *  AHI Audio Backend for Genesis Plus GX Amiga Port
 *
 ****************************************************************************/

#ifndef _AUDIO_AHI_H_
#define _AUDIO_AHI_H_

/* Initialize AHI audio subsystem */
int ahi_audio_init(int frequency, int stereo, int is_pal);

/* Shutdown AHI audio */
void ahi_audio_shutdown(void);

/* Submit audio samples for playback */
/* samples: interleaved stereo 16-bit samples */
/* count: number of sample frames (not bytes) */
void ahi_audio_submit(short *samples, int count);

/* Wait for audio buffer space */
void ahi_audio_wait(void);

/* Check if audio is available */
int ahi_audio_available(void);

/* Set output volume boost (0=1x, 1=2x, 2=4x, 3=8x, 4=16x) */
void ahi_set_volume_boost(int boost);

/* Set buffer margin in extra samples beyond samples_per_frame.
 * 0  = exact frame size (WinUAE / any accurate timer - no drops).
 * 220 = 4.99ms margin (PiStorm/Emu68 with timer jitter - prevents chain breaks).
 * Must be called before ahi_audio_init(). */
void ahi_set_margin(int margin);

/* Return number of buffers currently queued with AHI (0..3) */
int ahi_audio_queued(void);

#endif /* _AUDIO_AHI_H_ */
