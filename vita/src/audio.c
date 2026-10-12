/* Plays the page's audio: PCM chunks from the session go into a small ring
 * buffer, and a thread feeds the Vita's BGM audio port from it. Latency is
 * kept near TARGET_MS by dropping the oldest samples when the buffer grows. */
#include "audio.h"

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#include <string.h>

#define GRAIN 256                 /* samples per channel per output call */
#define RING_SAMPLES (48000 * 2)  /* up to 1 s of 48 kHz stereo */
#define TARGET_MS 120             /* trim the buffer back to this */
#define MAX_MS 300                /* ... once it grows past this */

static struct {
  pthread_mutex_t lock;
  int16_t ring[RING_SAMPLES];
  size_t head, len; /* in samples (all channels interleaved) */
  int rate, channels;
  int port;
  int running;
  pthread_t thread;
} A = {.lock = PTHREAD_MUTEX_INITIALIZER, .port = -1};

static size_t ms_to_samples(int ms) { return (size_t)A.rate * A.channels * ms / 1000; }

void audio_push(void *user, const int16_t *pcm, size_t samples, int rate, int channels) {
  (void)user;
  if (channels < 1 || channels > 2 || rate < 8000 || rate > 48000) return;
  pthread_mutex_lock(&A.lock);
  if (rate != A.rate || channels != A.channels) { /* format change: start over */
    A.rate = rate;
    A.channels = channels;
    A.head = A.len = 0;
  }
  for (size_t i = 0; i < samples; i++) {
    if (A.len == RING_SAMPLES) { /* full: drop the oldest */
      A.head = (A.head + 1) % RING_SAMPLES;
      A.len--;
    }
    A.ring[(A.head + A.len) % RING_SAMPLES] = pcm[i];
    A.len++;
  }
  size_t max = ms_to_samples(MAX_MS), target = ms_to_samples(TARGET_MS);
  if (A.len > max) { /* fell behind: jump to near-live */
    size_t drop = (A.len - target) / (size_t)A.channels * (size_t)A.channels;
    A.head = (A.head + drop) % RING_SAMPLES;
    A.len -= drop;
  }
  pthread_mutex_unlock(&A.lock);
}

static void *audio_thread(void *arg) {
  (void)arg;
  static int16_t out[GRAIN * 2];
  int port_rate = 0, port_channels = 0;
  while (A.running) {
    pthread_mutex_lock(&A.lock);
    int rate = A.rate, channels = A.channels;
    pthread_mutex_unlock(&A.lock);
    if (rate == 0) { /* nothing received yet */
      sceKernelDelayThread(10 * 1000);
      continue;
    }
    if (rate != port_rate || channels != port_channels) {
      if (A.port >= 0) sceAudioOutReleasePort(A.port);
      A.port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, rate,
                                   channels == 2 ? SCE_AUDIO_OUT_MODE_STEREO : SCE_AUDIO_OUT_MODE_MONO);
      if (A.port < 0) {
        sceKernelDelayThread(500 * 1000);
        continue;
      }
      int vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
      sceAudioOutSetVolume(A.port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
      port_rate = rate;
      port_channels = channels;
    }
    size_t need = (size_t)GRAIN * (size_t)channels;
    pthread_mutex_lock(&A.lock);
    size_t take = A.len < need ? A.len : need;
    for (size_t i = 0; i < take; i++) out[i] = A.ring[(A.head + i) % RING_SAMPLES];
    A.head = (A.head + take) % RING_SAMPLES;
    A.len -= take;
    pthread_mutex_unlock(&A.lock);
    if (take < need) memset(out + take, 0, (need - take) * sizeof(int16_t)); /* underrun: silence */
    sceAudioOutOutput(A.port, out); /* blocks for one grain */
  }
  if (A.port >= 0) {
    sceAudioOutReleasePort(A.port);
    A.port = -1;
  }
  return NULL;
}

int audio_init(void) {
  A.running = 1;
  return pthread_create(&A.thread, NULL, audio_thread, NULL);
}
