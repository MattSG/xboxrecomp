/**
 * Audio output (SDL3)
 */
#ifndef APU_AUDIO_OUT_H
#define APU_AUDIO_OUT_H

#include <stdint.h>

/* Open the default playback device. Returns 1 on success, 0 on failure. */
int apu_out_init(void);

/* Close the device and release all resources. */
void apu_out_shutdown(void);

/* Returns 1 if the device is open. */
int apu_out_is_active(void);

/* Submit interleaved stereo 16-bit samples. Returns 1 if accepted. */
int apu_out_submit_samples(const int16_t *samples, int num_samples);

/* Get the preferred buffer size in samples. */
int apu_out_get_buffer_size(void);

#endif /* APU_AUDIO_OUT_H */
