/* Contract between the Vita app shell (main.c, decoder.c) and the Surf session
 * (session.c: pinned TLS WebSocket, pairing, identity, client/core). */
#ifndef SURF_VITA_SESSION_H
#define SURF_VITA_SESSION_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
  SV_STATE_IDLE = 0,
  SV_STATE_CONNECTING,
  SV_STATE_STREAMING,
  SV_STATE_PAUSED,      /* backend browser-mode not "streaming" */
  SV_STATE_ERROR,       /* see sv_status.message */
} sv_state;

typedef struct {
  sv_state state;
  char title[256];
  char url[1024];
  int loading;
  char message[256];    /* human readable error or pause reason */
} sv_status;

typedef struct {
  /* Called on the network thread for every admitted H.264 Annex B access unit.
   * The buffer is valid only during the call; copy it. */
  void (*on_video)(void *user, const uint8_t *au, size_t len, int idr,
                   uint16_t width, uint16_t height, uint32_t sequence);
  /* Called on the network thread when status changes. */
  void (*on_status)(void *user, const sv_status *status);
  void *user;
} sv_callbacks;

/* Pairing (blocking; call from a worker thread or before streaming).
 * address "ip:port", code = the 6 digits shown by `surf pair`.
 * On success fills words (the 6 confirmation words, space separated) and
 * stores the server record + identity under ux0:data/surfvita/. Returns 0 ok. */
int sv_pair(const char *address, const char *code, char *words, size_t words_len,
            char *err, size_t err_len);

/* Two-step pairing (additive; preferred over sv_pair). docs/security.md: the
 * user must compare the six words with the ones the server shows and cancel
 * when they differ. sv_pair_request does steps up to the words (blocking,
 * key generation may take seconds on first pairing); then call
 * sv_pair_confirm when the user accepts, or sv_pair_cancel when not.
 * sv_pair == sv_pair_request + sv_pair_confirm without the human check. */
int sv_pair_request(const char *address, const char *code, char *words,
                    size_t words_len, char *err, size_t err_len);
int sv_pair_confirm(char *err, size_t err_len);
void sv_pair_cancel(void);

/* Whether a paired server record exists; fills address. */
int sv_have_server(char *address, size_t address_len);

/* Starts the network thread: connect, authenticate, stream. Viewport in
 * pixels (960x544). Returns 0 ok. */
int sv_start(int width, int height, const sv_callbacks *cb);
void sv_stop(void);

/* Input. Coordinates are normalized to the rendered page surface, 0..1. */
enum { SV_TOUCH_DOWN = 0, SV_TOUCH_MOVE = 1, SV_TOUCH_UP = 2 };
void sv_touch(int phase, int finger, float x, float y);
void sv_wheel(float x, float y, float dx, float dy);   /* dx/dy in CSS px */
void sv_text(const char *utf8);
enum { SV_KEY_ENTER = 1, SV_KEY_BACKSPACE, SV_KEY_TAB, SV_KEY_ESCAPE,
       SV_KEY_UP, SV_KEY_DOWN, SV_KEY_LEFT, SV_KEY_RIGHT };
void sv_key(int key);
void sv_navigate(const char *url_or_search);
void sv_back(void);
void sv_forward(void);
void sv_reload(void);

/* The app shell reports when a decoded frame was shown (media pipeline
 * acknowledgement, see docs/porting-clients.md "Media path" step 5). */
void sv_presented(uint32_t sequence);

#endif
