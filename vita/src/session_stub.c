/* Fake Surf session for building and testing the app shell without a backend
 * (compiled only with -DSV_STUB_SESSION=ON).
 *
 * - sv_have_server: true once sv_pair succeeded (marker file under
 *   ux0:data/surfvita/stub_paired).
 * - sv_pair: waits 1 s; code "000000" fails, anything else succeeds.
 * - sv_start: CONNECTING for 1 s, then STREAMING a looped 960x544 sample
 *   H.264 stream at 30 fps. Navigating to "error" or "pause" fakes those
 *   states so the retry and pause screens can be checked. */
#include "session.h"
#include "sample_h264.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define STUB_DIR "ux0:data/surfvita"
#define STUB_MARK STUB_DIR "/stub_paired"

static pthread_t g_thread;
static int g_running;
static volatile int g_quit;
static sv_callbacks g_cb;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static sv_status g_status;
static volatile int g_reload_ticks;
static volatile int g_fake; /* 0 none, 1 error, 2 pause */

static void push_status_locked(void) {
  sv_status copy = g_status;
  pthread_mutex_unlock(&g_lock);
  if (g_cb.on_status) g_cb.on_status(g_cb.user, &copy);
  pthread_mutex_lock(&g_lock);
}

static void set_state(sv_state st, const char *msg) {
  pthread_mutex_lock(&g_lock);
  g_status.state = st;
  snprintf(g_status.message, sizeof(g_status.message), "%s", msg ? msg : "");
  push_status_locked();
  pthread_mutex_unlock(&g_lock);
}

int sv_pair(const char *address, const char *code, char *words, size_t words_len,
            char *err, size_t err_len) {
  sceKernelDelayThread(1000 * 1000);
  if (!address || !*address || !code || strcmp(code, "000000") == 0) {
    snprintf(err, err_len, "stub: pairing rejected (code 000000 always fails)");
    return -1;
  }
  sceIoMkdir("ux0:data", 0777);
  sceIoMkdir(STUB_DIR, 0777);
  SceUID fd = sceIoOpen(STUB_MARK, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd >= 0) {
    sceIoWrite(fd, address, strlen(address));
    sceIoClose(fd);
  }
  snprintf(words, words_len, "amber bridge cactus dolphin ember falcon");
  return 0;
}

int sv_have_server(char *address, size_t address_len) {
  SceUID fd = sceIoOpen(STUB_MARK, SCE_O_RDONLY, 0);
  if (fd < 0) return 0;
  int n = sceIoRead(fd, address, address_len > 0 ? address_len - 1 : 0);
  sceIoClose(fd);
  if (address_len > 0) address[n > 0 ? n : 0] = 0;
  return 1;
}

static void *stub_thread(void *arg) {
  (void)arg;
  uint32_t seq = 0;
  set_state(SV_STATE_CONNECTING, "");
  for (int i = 0; i < 10 && !g_quit; i++) sceKernelDelayThread(100 * 1000);

  pthread_mutex_lock(&g_lock);
  g_status.state = SV_STATE_STREAMING;
  snprintf(g_status.title, sizeof(g_status.title), "Stub page");
  snprintf(g_status.url, sizeof(g_status.url), "https://example.com/");
  g_status.loading = 0;
  g_status.message[0] = 0;
  push_status_locked();
  pthread_mutex_unlock(&g_lock);

  int au = 0;
  while (!g_quit) {
    if (g_fake == 1) {
      g_fake = 0;
      set_state(SV_STATE_ERROR, "stub: simulated connection error");
      return NULL; /* app retries with sv_stop + sv_start */
    }
    if (g_fake == 2) {
      g_fake = 0;
      set_state(SV_STATE_PAUSED, "stub: browser is not streaming");
      for (int i = 0; i < 30 && !g_quit; i++) sceKernelDelayThread(100 * 1000);
      set_state(SV_STATE_STREAMING, "");
      au = 0; /* restart from the IDR */
    }
    if (g_reload_ticks > 0 && --g_reload_ticks == 0) {
      pthread_mutex_lock(&g_lock);
      g_status.loading = 0;
      push_status_locked();
      pthread_mutex_unlock(&g_lock);
    }
    if (g_cb.on_video)
      g_cb.on_video(g_cb.user, sv_sample_h264 + sv_sample_aus[au].off,
                    sv_sample_aus[au].len, sv_sample_aus[au].idr, 960, 544, seq++);
    au = (au + 1) % SV_SAMPLE_AU_COUNT;
    sceKernelDelayThread(33333);
  }
  return NULL;
}

int sv_start(int width, int height, const sv_callbacks *cb) {
  (void)width;
  (void)height;
  if (g_running) return 0;
  g_cb = *cb;
  g_quit = 0;
  g_fake = 0;
  if (pthread_create(&g_thread, NULL, stub_thread, NULL) != 0) return -1;
  g_running = 1;
  return 0;
}

void sv_stop(void) {
  if (!g_running) return;
  g_quit = 1;
  pthread_join(g_thread, NULL);
  g_running = 0;
}

static void loading_for(int ticks) {
  pthread_mutex_lock(&g_lock);
  g_status.loading = 1;
  push_status_locked();
  pthread_mutex_unlock(&g_lock);
  g_reload_ticks = ticks;
}

void sv_touch(int phase, int finger, float x, float y) {
  printf("stub touch phase=%d finger=%d %.3f,%.3f\n", phase, finger, x, y);
}
void sv_wheel(float x, float y, float dx, float dy) {
  printf("stub wheel %.2f,%.2f d=%.1f,%.1f\n", x, y, dx, dy);
}
void sv_text(const char *utf8) { printf("stub text '%s'\n", utf8); }
void sv_key(int key) { printf("stub key %d\n", key); }

void sv_navigate(const char *url_or_search) {
  if (strcmp(url_or_search, "error") == 0) { g_fake = 1; return; }
  if (strcmp(url_or_search, "pause") == 0) { g_fake = 2; return; }
  pthread_mutex_lock(&g_lock);
  snprintf(g_status.url, sizeof(g_status.url), "%s", url_or_search);
  snprintf(g_status.title, sizeof(g_status.title), "Stub: %s", url_or_search);
  pthread_mutex_unlock(&g_lock);
  loading_for(30);
}
void sv_back(void) { loading_for(15); }
void sv_forward(void) { loading_for(15); }
void sv_reload(void) { loading_for(30); }
void sv_presented(uint32_t sequence) { (void)sequence; }

/* Two-step pairing (stub): request = sv_pair without storing; confirm stores. */
static char g_stub_pending[128];
int sv_pair_request(const char *address, const char *code, char *words,
                    size_t words_len, char *err, size_t err_len) {
  sceKernelDelayThread(2000 * 1000); /* pretend RSA key generation */
  if (!address || !*address || !code || strcmp(code, "000000") == 0) {
    snprintf(err, err_len, "stub: pairing rejected (code 000000 always fails)");
    return -1;
  }
  snprintf(g_stub_pending, sizeof(g_stub_pending), "%s", address);
  snprintf(words, words_len, "amber bridge cactus dolphin ember falcon");
  return 0;
}
int sv_pair_confirm(char *err, size_t err_len) {
  char words[128];
  if (!g_stub_pending[0]) {
    snprintf(err, err_len, "stub: no pairing in progress");
    return -1;
  }
  int r = sv_pair(g_stub_pending, "123456", words, sizeof(words), err, err_len);
  g_stub_pending[0] = 0;
  return r;
}
void sv_pair_cancel(void) { g_stub_pending[0] = 0; }
void sv_debug_counts(unsigned long *b, unsigned long *x, unsigned long *a) { *b = *x = *a = 0; }
void sv_click(float x, float y) { (void)x; (void)y; }
void sv_request_keyframe(void) {}
void sv_set_audio(sv_audio_fn fn, void *user) { (void)fn; (void)user; }
void sv_tab_select(int64_t id) { (void)id; }
void sv_tab_close(int64_t id) { (void)id; }
void sv_tab_new(void) {}
