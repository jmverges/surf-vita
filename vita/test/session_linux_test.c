/* Native Linux smoke test for the Vita session layer (session.c +
 * session_net.c built without __vita__). Usage:
 *   SURFVITA_HOME=/tmp/x session_linux_test 127.0.0.1:18080 [CODE]
 * With CODE it pairs first; then it streams for ~15 s and reports. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "session.h"

static unsigned long g_aus, g_idr, g_bytes;
static uint32_t g_last_seq;
static unsigned g_w, g_h;

static void on_video(void *user, const uint8_t *au, size_t len, int idr, uint16_t w, uint16_t h,
                     uint32_t seq) {
  (void)user;
  if (len < 4 || !(au[0] == 0 && au[1] == 0 && (au[2] == 1 || (au[2] == 0 && au[3] == 1))))
    fprintf(stderr, "AU %u is not Annex B\n", seq);
  if (getenv("SV_DUMP")) {
    FILE *f = fopen(getenv("SV_DUMP"), "ab");
    if (f) { fwrite(au, 1, len, f); fclose(f); }
  }
  g_aus++;
  g_idr += idr ? 1 : 0;
  g_bytes += len;
  g_last_seq = seq;
  g_w = w;
  g_h = h;
}

static void on_status(void *user, const sv_status *s) {
  (void)user;
  printf("status: state=%d loading=%d title=\"%s\" url=\"%s\" msg=\"%s\"\n", s->state, s->loading,
         s->title, s->url, s->message);
  fflush(stdout);
}

int main(int argc, char **argv) {
  char words[128], err[256], addr[96];
  sv_callbacks cb;
  int i;
  if (argc < 2) return 2;
  if (argc >= 3) {
    if (sv_pair_request(argv[1], argv[2], words, sizeof(words), err, sizeof(err)) != 0) {
      printf("pair request failed: %s\n", err);
      return 1;
    }
    printf("words: %s\n", words);
    if (sv_pair_confirm(err, sizeof(err)) != 0) {
      printf("pair confirm failed: %s\n", err);
      return 1;
    }
    printf("paired\n");
  }
  if (!sv_have_server(addr, sizeof(addr))) {
    printf("no server record\n");
    return 1;
  }
  printf("server: %s\n", addr);
  memset(&cb, 0, sizeof(cb));
  cb.on_video = on_video;
  cb.on_status = on_status;
  if (sv_start(960, 544, &cb) != 0) {
    printf("sv_start failed\n");
    return 1;
  }
  int ticks = getenv("SV_TEST_SECONDS") ? atoi(getenv("SV_TEST_SECONDS")) * 10 : 150;
  for (i = 0; i < ticks; i++) {
    usleep(100000);
    if (g_aus) sv_presented(g_last_seq);
    if (i == 30 && getenv("SV_TEST_NAVIGATE")) sv_navigate(getenv("SV_TEST_NAVIGATE"));
    if (i == 40 && getenv("SV_TEST_CLICK")) {
      float cx = 0, cy = 0;
      if (sscanf(getenv("SV_TEST_CLICK"), "%f,%f", &cx, &cy) == 2) { sv_click(cx, cy); fprintf(stderr, "clicked %.3f,%.3f\n", cx, cy); }
    }
    if (i == 80) {
      sv_touch(0, 0, 0.5f, 0.5f);
      sv_touch(1, 0, 0.5f, 0.45f);
      sv_touch(2, 0, 0.5f, 0.4f);
    }
  }
  sv_stop();
  printf("video: %lu AUs (%lu IDR), %lu bytes, %ux%u\n", g_aus, g_idr, g_bytes, g_w, g_h);
  return g_aus > 0 ? 0 : 1;
}
