/* H.264 Annex B decoder on the Vita hardware AVC decoder (SceAvcdec).
 *
 * Threads:
 *   network thread  -> dec_submit()   copies the AU into a bounded queue
 *   decoder thread  (internal)        decodes into one of 3 RGBA textures
 *   render thread   -> dec_latest()   takes the newest decoded texture
 *
 * Render-thread contract: call vita2d_wait_rendering_done() after every frame
 * that drew a texture returned by dec_latest(), before calling dec_latest()
 * again. The texture shown in the previous frame is handed back to the decoder
 * on the next dec_latest() that returns a newer frame. */
#ifndef SURF_VITA_DECODER_H
#define SURF_VITA_DECODER_H

#include <stddef.h>
#include <stdint.h>
#include <vita2d.h>

#define DEC_MAX_WIDTH  960
#define DEC_MAX_HEIGHT 544

typedef struct {
  uint32_t submitted;
  uint32_t dropped;      /* AUs dropped by the queue policy or IDR wait */
  uint32_t decoded;      /* pictures output */
  uint32_t errors;       /* sceAvcdecDecode failures */
  uint32_t no_output;    /* decodes that returned no picture yet */
  uint32_t last_error;   /* last SCE error code (0 if none) */
  int waiting_idr;
} dec_stats;

/* Returns 0 ok, or a negative SCE error code. Must be called after vita2d_init. */
int dec_init(void);
void dec_shutdown(void);

/* Network thread. Copies the AU. */
void dec_submit(const uint8_t *au, size_t len, int idr, uint16_t width,
                uint16_t height, uint32_t sequence);

/* Drop queued work and wait for the next IDR (reconnect / new stream). */
void dec_reset(void);

/* Render thread. Returns 1 if a frame is available (fills tex, size, sequence)
 * and sets *is_new when it differs from the one returned last time. */
int dec_latest(vita2d_texture **tex, int *width, int *height,
               uint32_t *sequence, int *is_new);

void dec_get_stats(dec_stats *out);

extern volatile int dec_in_decode;
extern volatile int dec_pending;
extern volatile unsigned long long dec_last_au_us;
extern volatile int dec_lowdelay_ret;

#endif
