/****************************************************************************
 *  audio_paula14.h
 *
 *  14-Bit Paula Audio Backend for Genesis Plus GX Amiga Port
 *
 *  Uses audio.device CMD_WRITE with quad-buffering (same as 8-bit backend)
 *  but with all 4 Paula channels for the Buchner/Richter 14-bit technique:
 *
 *  Channel mapping:
 *  - AUD0 = Left HI  (volume 64)  \  combined on left DAC  -> ~12-bit left
 *  - AUD3 = Left LO  (volume  1)  /
 *  - AUD1 = Right HI (volume 64)  \  combined on right DAC -> ~12-bit right
 *  - AUD2 = Right LO (volume  1)  /
 *
 *  Formula: HI[n] = sample >> 8 (signed), LO[n] = (sample & 0xFF) >> 2
 *  DAC output = HI * 64 + LO * 1 = sample / 4 (14-bit linear, exact)
 *  Effective resolution: ~10-12 bit (hardware DAC matching, interrupt skew)
 *
 ****************************************************************************/

#ifndef _AUDIO_PAULA14_H_
#define _AUDIO_PAULA14_H_

/* Initialize 14-bit Paula audio
 * frequency: Desired sample rate (0 = default ~27710 Hz)
 * hw_pal:   1 if Amiga hardware is PAL (selects Paula crystal clock)
 * game_pal: 1 if emulated game is PAL 50Hz (selects fps for buffer sizing)
 * Returns: 1 on success, 0 on failure
 */
int paula14_audio_init(int frequency, int hw_pal, int game_pal);

/* Shutdown 14-bit Paula audio */
void paula14_audio_shutdown(void);

/* Submit audio buffer
 * buffer: 16-bit signed stereo samples (interleaved L/R)
 * samples: Number of stereo sample pairs
 */
void paula14_audio_submit(signed short *buffer, int samples);

/* Check if Paula 14-bit is available */
int paula14_audio_available(void);

/* Get current sample rate */
int paula14_audio_get_rate(void);

/* Set volume boost: 0=1x, 1=2x, 2=4x, 3=8x, 4=16x */
void paula14_set_volume_boost(int boost);

/* Set buffer margin in extra samples beyond samples_per_frame.
 * 0=WinUAE/accurate timer, 200=PiStorm/Emu68. Must be called before init. */
void paula14_set_margin(int margin);

/* Return number of buffers currently queued with Paula (0..4) */
int paula14_audio_queued(void);

/* Debug counters: frames submitted and dropped since last reset */
extern int paula14_dbg_submitted;
extern int paula14_dbg_dropped;

#endif /* _AUDIO_PAULA14_H_ */
