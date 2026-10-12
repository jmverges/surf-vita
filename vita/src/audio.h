/* Page audio playback (see audio.c). */
#ifndef SURF_VITA_AUDIO_H
#define SURF_VITA_AUDIO_H

#include <stddef.h>
#include <stdint.h>

/* Starts the playback thread. Returns 0 ok. */
int audio_init(void);
/* An sv_audio_fn: queue interleaved PCM for playback. */
void audio_push(void *user, const int16_t *pcm, size_t samples, int rate, int channels);

#endif
