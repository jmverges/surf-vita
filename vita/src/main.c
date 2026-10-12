/* Surf for Vita: app shell (pairing UI, rendering, input mapping). */
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/system_param.h>
#include <psp2/touch.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vita2d.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "audio.h"
#include "decoder.h"
#include "session.h"

#define SCREEN_W 960
#define SCREEN_H 544
#define TOUCH_W 1920.0f
#define TOUCH_H 1088.0f

#define INPUT_INTERVAL_US 16667 /* 60 Hz */
#define STICK_DEADZONE 24
#define STICK_MAX_PX 60.0f      /* CSS px per tick at full tilt (~1800 px/s) */
#define DPAD_WHEEL_PX 120.0f
#define MAX_FINGERS 6

#define COL_WHITE RGBA8(255, 255, 255, 255)
#define COL_GREY RGBA8(170, 170, 170, 255)
#define COL_ACCENT RGBA8(110, 190, 255, 255)
#define COL_ERROR RGBA8(255, 120, 110, 255)
#define COL_BAR RGBA8(0, 0, 0, 110)
#define COL_PANEL RGBA8(20, 22, 28, 230)

/* ---------- shared state ---------- */

static vita2d_pgf *g_font;
static pthread_mutex_t g_status_lock = PTHREAD_MUTEX_INITIALIZER;
static sv_status g_status;
static char g_address[128];

static void on_status(void *user, const sv_status *st) {
  (void)user;
  pthread_mutex_lock(&g_status_lock);
  g_status = *st;
  pthread_mutex_unlock(&g_status_lock);
}

static void on_video(void *user, const uint8_t *au, size_t len, int idr,
                     uint16_t width, uint16_t height, uint32_t sequence) {
  (void)user;
  dec_submit(au, len, idr, width, height, sequence);
}

static sv_status status_snapshot(void) {
  sv_status s;
  pthread_mutex_lock(&g_status_lock);
  s = g_status;
  pthread_mutex_unlock(&g_status_lock);
  return s;
}

static const sv_callbacks g_callbacks = {on_video, on_status, NULL};

/* ---------- UTF-8 <-> UTF-16 ---------- */

static void utf8_to_utf16(const char *in, SceWChar16 *out, size_t out_len) {
  size_t o = 0;
  const unsigned char *s = (const unsigned char *)in;
  while (*s && o + 1 < out_len) {
    uint32_t cp;
    if (*s < 0x80) cp = *s++;
    else if ((*s & 0xE0) == 0xC0 && s[1]) { cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F); s += 2; }
    else if ((*s & 0xF0) == 0xE0 && s[1] && s[2]) { cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); s += 3; }
    else if ((*s & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
      cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); s += 4;
    } else { s++; continue; }
    if (cp >= 0x10000) {
      if (o + 2 >= out_len) break;
      cp -= 0x10000;
      out[o++] = (SceWChar16)(0xD800 + (cp >> 10));
      out[o++] = (SceWChar16)(0xDC00 + (cp & 0x3FF));
    } else {
      out[o++] = (SceWChar16)cp;
    }
  }
  out[o] = 0;
}

static void utf16_to_utf8(const SceWChar16 *in, char *out, size_t out_len) {
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 1 < out_len; i++) {
    uint32_t cp = in[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (in[i + 1] - 0xDC00);
      i++;
    }
    char buf[4];
    size_t n;
    if (cp < 0x80) { buf[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { buf[0] = (char)(0xC0 | (cp >> 6)); buf[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
      buf[0] = (char)(0xE0 | (cp >> 12)); buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
      buf[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
      buf[0] = (char)(0xF0 | (cp >> 18)); buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
      buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
    }
    if (o + n >= out_len) break;
    memcpy(out + o, buf, n);
    o += n;
  }
  out[o] = 0;
}

/* ---------- system keyboard (non-blocking; finished in the render loop) ---------- */

typedef enum { IME_NONE = 0, IME_ADDRESS, IME_CODE, IME_URL, IME_TEXT } ime_purpose;

#define IME_MAX 1024
static ime_purpose g_ime = IME_NONE;
static SceWChar16 g_ime_title[SCE_IME_DIALOG_MAX_TITLE_LENGTH];
static SceWChar16 g_ime_initial[IME_MAX + 1];
static SceWChar16 g_ime_buffer[IME_MAX + 1];

static int ime_open(ime_purpose purpose, const char *title, const char *initial,
                    SceUInt32 type, SceUInt32 max_len) {
  if (g_ime != IME_NONE) return -1;
  if (max_len > IME_MAX) max_len = IME_MAX;
  utf8_to_utf16(title, g_ime_title, SCE_IME_DIALOG_MAX_TITLE_LENGTH);
  utf8_to_utf16(initial ? initial : "", g_ime_initial, max_len + 1);
  memset(g_ime_buffer, 0, sizeof(g_ime_buffer));

  SceImeDialogParam p;
  sceImeDialogParamInit(&p);
  p.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH;
  p.languagesForced = SCE_FALSE;
  p.type = type;
  p.option = (type == SCE_IME_TYPE_DEFAULT) ? 0 : SCE_IME_OPTION_NO_AUTO_CAPITALIZATION;
  p.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
  p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
  p.title = g_ime_title;
  p.maxTextLength = max_len;
  p.initialText = g_ime_initial;
  p.inputTextBuffer = g_ime_buffer;
  int ret = sceImeDialogInit(&p);
  if (ret < 0) {
    printf("sceImeDialogInit 0x%08x\n", ret);
    return ret;
  }
  g_ime = purpose;
  return 0;
}

/* Returns the purpose when a dialog just finished (text in out, *accepted). */
static ime_purpose ime_poll(char *out, size_t out_len, int *accepted) {
  if (g_ime == IME_NONE) return IME_NONE;
  if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) return IME_NONE;
  SceImeDialogResult r;
  memset(&r, 0, sizeof(r));
  sceImeDialogGetResult(&r);
  *accepted = (r.button == SCE_IME_DIALOG_BUTTON_ENTER);
  utf16_to_utf8(g_ime_buffer, out, out_len);
  sceImeDialogTerm();
  ime_purpose done = g_ime;
  g_ime = IME_NONE;
  return done;
}

/* ---------- pairing worker ---------- */

typedef enum {
  APP_PAIR_ADDRESS,   /* keyboard: ip:port */
  APP_PAIR_CODE,      /* keyboard: 6 digits */
  APP_PAIRING,        /* worker running sv_pair_request (key gen + pairing) */
  APP_PAIR_CONFIRM,   /* show words, X yes / O no */
  APP_PAIR_FINISHING, /* worker running sv_pair_confirm */
  APP_PAIR_FAILED,    /* show error, X retry */
  APP_RUN,
} app_state;

static struct {
  pthread_t thread;
  char address[128];
  char code[16];
  char words[256];
  char err[256];
  volatile int done;
  int result;
  int confirming;
} g_pair;

static void *pair_worker(void *arg) {
  (void)arg;
  g_pair.words[0] = g_pair.err[0] = 0;
  if (g_pair.confirming)
    g_pair.result = sv_pair_confirm(g_pair.err, sizeof(g_pair.err));
  else
    g_pair.result = sv_pair_request(g_pair.address, g_pair.code, g_pair.words,
                                    sizeof(g_pair.words), g_pair.err, sizeof(g_pair.err));
  g_pair.done = 1;
  return NULL;
}

/* ---------- drawing helpers ---------- */

static void text(int x, int y, unsigned int color, float scale, const char *s) {
  vita2d_pgf_draw_text(g_font, x, y, color, scale, s);
}

static void text_center(int y, unsigned int color, float scale, const char *s) {
  int w = vita2d_pgf_text_width(g_font, scale, s);
  text((SCREEN_W - w) / 2, y, color, scale, s);
}

/* Truncate to fit width with an ellipsis. */
static void text_fit(int x, int y, int max_w, unsigned int color, float scale, const char *s) {
  char buf[512];
  snprintf(buf, sizeof(buf), "%s", s);
  if (vita2d_pgf_text_width(g_font, scale, buf) <= max_w) {
    text(x, y, color, scale, buf);
    return;
  }
  size_t n = strlen(buf);
  while (n > 0) {
    n--;
    while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) n--; /* UTF-8 boundary */
    memcpy(buf + n, "...", 4);
    if (vita2d_pgf_text_width(g_font, scale, buf) <= max_w) break;
  }
  text(x, y, color, scale, buf);
}

static void panel(const char *title, unsigned int title_color, const char *line1,
                  const char *line2, const char *hint) {
  vita2d_draw_rectangle(120, 160, 720, 224, COL_PANEL);
  text_center(210, title_color, 1.3f, title);
  if (line1 && *line1) text_center(260, COL_WHITE, 1.0f, line1);
  if (line2 && *line2) text_center(295, COL_WHITE, 1.0f, line2);
  if (hint && *hint) text_center(355, COL_GREY, 0.9f, hint);
}

static const char *state_name(sv_state s) {
  switch (s) {
    case SV_STATE_IDLE: return "idle";
    case SV_STATE_CONNECTING: return "connecting";
    case SV_STATE_STREAMING: return "streaming";
    case SV_STATE_PAUSED: return "paused";
    case SV_STATE_ERROR: return "error";
  }
  return "?";
}

/* Debug: once a second, send the pipeline counters as one UDP line to the
 * paired host on port 18999 (`nc -klu 18999` there), so testing needs no photos. */
static volatile unsigned long g_render_frames;
static volatile int g_report_on;

static void debug_report(void) {
  static int sock = -1;
  static uint64_t last_us;
  uint64_t now = sceKernelGetProcessTimeWide();
  if (now - last_us < 1000000) return;
  last_us = now;
  char host[128];
  snprintf(host, sizeof(host), "%s", g_address);
  char *colon = strchr(host, ':');
  if (colon) *colon = 0;
  struct sockaddr_in to;
  memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET;
  to.sin_port = htons(18999);
  if (!host[0] || inet_pton(AF_INET, host, &to.sin_addr) != 1) return;
  if (sock < 0) sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) return;
  dec_stats ds;
  dec_get_stats(&ds);
  unsigned long bin, bad, adm;
  sv_debug_counts(&bin, &bad, &adm);
  sv_status st = status_snapshot();
  char line[512];
  int n = snprintf(line, sizeof(line),
                   "t=%llu frames=%lu indec=%d pend=%d state=%d ws=%lu bad=%lu adm=%lu sub=%lu dec=%lu noout=%lu err=%lu drop=%lu idr?=%d lasterr=0x%08lx title=%.60s\n",
                   (unsigned long long)(now / 1000000), g_render_frames, dec_in_decode, dec_pending, (int)st.state, bin, bad, adm,
                   (unsigned long)ds.submitted, (unsigned long)ds.decoded, (unsigned long)ds.no_output, (unsigned long)ds.errors,
                   (unsigned long)ds.dropped, ds.waiting_idr, (unsigned long)ds.last_error, st.title);
  if (n > 0) sendto(sock, line, (size_t)n, 0, (struct sockaddr *)&to, sizeof(to));
}

static void *report_thread(void *arg) {
  (void)arg;
  for (;;) {
    if (g_report_on) debug_report();
    sceKernelDelayThread(200 * 1000);
  }
  return NULL;
}

static void draw_overlay(const sv_status *st) {
  dec_stats ds;
  dec_get_stats(&ds);
  vita2d_draw_rectangle(0, 0, SCREEN_W, 64, COL_BAR);
  text_fit(12, 24, 700, COL_WHITE, 1.0f, st->title[0] ? st->title : "(no title)");
  text_fit(12, 52, 700, COL_ACCENT, 0.85f, st->url[0] ? st->url : "");
  char right[96];
  snprintf(right, sizeof(right), "%s%s", state_name(st->state), st->loading ? " | loading" : "");
  text(730, 24, st->loading ? COL_ACCENT : COL_GREY, 0.85f, right);
  snprintf(right, sizeof(right), "dec %lu err %lu drop %lu", (unsigned long)ds.decoded,
           (unsigned long)ds.errors, (unsigned long)ds.dropped);
  text(730, 52, COL_GREY, 0.75f, right);
  {
    unsigned long bin, bad, adm;
    sv_debug_counts(&bin, &bad, &adm);
    snprintf(right, sizeof(right), "ws %lu bad %lu adm %lu sub %lu%s", bin, bad, adm,
             (unsigned long)ds.submitted, ds.waiting_idr ? " idr?" : "");
    text(730, 76, COL_GREY, 0.75f, right);
  }

  vita2d_draw_rectangle(0, SCREEN_H - 30, SCREEN_W, 30, COL_BAR);
  text(12, SCREEN_H - 9, COL_GREY, 0.75f,
       "Touch: tap/drag  Stick/Up/Down: scroll  /\\: URL  []: type  X: Enter  O: Bksp  L/R: back/fwd  START: reload");
}

/* ---------- input ---------- */

static uint64_t g_last_touch_move_us;
static uint64_t g_last_wheel_us;

/* One finger: a tap is a mouse click, a drag scrolls (wheel). Chromium's
 * touch emulation dropped Vita taps, and desktop click/wheel semantics are
 * what the backend's pointer input handles most reliably. */
static struct {
  int active, vita_id, dragging;
  float x0, y0, x, y, sent_x, sent_y;
} g_tap;

#define DRAG_THRESHOLD 0.02f /* of the screen: ~19 px horizontally */

static void touch_release_all(void) { g_tap.active = 0; }

static float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static void handle_touch(uint64_t now) {
  SceTouchData td;
  memset(&td, 0, sizeof(td));
  if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1) < 0) return;

  int found = 0;
  for (unsigned r = 0; r < td.reportNum && r < SCE_TOUCH_MAX_REPORT; r++) {
    float x = clamp01(td.report[r].x / TOUCH_W);
    float y = clamp01(td.report[r].y / TOUCH_H);
    if (!g_tap.active) {
      g_tap.active = 1;
      g_tap.vita_id = td.report[r].id;
      g_tap.dragging = 0;
      g_tap.x0 = g_tap.x = g_tap.sent_x = x;
      g_tap.y0 = g_tap.y = g_tap.sent_y = y;
      found = 1;
      break;
    }
    if (td.report[r].id == g_tap.vita_id) {
      g_tap.x = x;
      g_tap.y = y;
      found = 1;
      break;
    }
  }

  if (g_tap.active && !found) { /* finger lifted */
    if (!g_tap.dragging) sv_click(g_tap.x0, g_tap.y0);
    g_tap.active = 0;
    return;
  }
  if (!g_tap.active) return;

  float dx = g_tap.x - g_tap.x0, dy = g_tap.y - g_tap.y0;
  if (!g_tap.dragging && (dx * dx + dy * dy) > DRAG_THRESHOLD * DRAG_THRESHOLD) g_tap.dragging = 1;
  if (g_tap.dragging && now - g_last_touch_move_us >= INPUT_INTERVAL_US) {
    /* Content follows the finger: moving the finger up scrolls the page down. */
    float wx = (g_tap.sent_x - g_tap.x) * SCREEN_W, wy = (g_tap.sent_y - g_tap.y) * SCREEN_H;
    if (wx != 0 || wy != 0) {
      sv_wheel(g_tap.x, g_tap.y, wx, wy);
      g_tap.sent_x = g_tap.x;
      g_tap.sent_y = g_tap.y;
      g_last_touch_move_us = now;
    }
  }
}

static float stick_axis(int v) {
  int d = v - 128;
  int a = d < 0 ? -d : d;
  if (a <= STICK_DEADZONE) return 0.0f;
  float t = (float)(a - STICK_DEADZONE) / (float)(127 - STICK_DEADZONE);
  if (t > 1.0f) t = 1.0f;
  float speed = t * t * STICK_MAX_PX; /* fine control near the centre */
  return d < 0 ? -speed : speed;
}

static void handle_stick(const SceCtrlData *pad, uint64_t now) {
  if (now - g_last_wheel_us < INPUT_INTERVAL_US) return;
  float dx = stick_axis(pad->lx);
  float dy = stick_axis(pad->ly);
  if (dx != 0.0f || dy != 0.0f) {
    sv_wheel(0.5f, 0.5f, dx, dy);
    g_last_wheel_us = now;
  }
}

/* ---------- main ---------- */

static void init_system(void) {
  scePowerSetArmClockFrequency(444);
  scePowerSetBusClockFrequency(222);
  scePowerSetGpuClockFrequency(222);
  scePowerSetGpuXbarClockFrequency(166);

  SceAppUtilInitParam ai;
  SceAppUtilBootParam ab;
  memset(&ai, 0, sizeof(ai));
  memset(&ab, 0, sizeof(ab));
  sceAppUtilInit(&ai, &ab);

  SceCommonDialogConfigParam cfg;
  sceCommonDialogConfigParamInit(&cfg);
  sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, (int *)&cfg.language);
  sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, (int *)&cfg.enterButtonAssign);
  sceCommonDialogSetConfigParam(&cfg);

  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
  sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
  /* Network (SceNet/SceNetCtl) is initialised by the session layer. */
}

static void start_session(void) {
  memset(&g_status, 0, sizeof(g_status));
  g_status.state = SV_STATE_CONNECTING;
  dec_reset();
  int ret = sv_start(SCREEN_W, SCREEN_H, &g_callbacks);
  if (ret != 0) {
    pthread_mutex_lock(&g_status_lock);
    g_status.state = SV_STATE_ERROR;
    snprintf(g_status.message, sizeof(g_status.message), "Could not start session (%d)", ret);
    pthread_mutex_unlock(&g_status_lock);
  }
}

int main(void) {
  init_system();
  {
    pthread_t rt;
    pthread_create(&rt, NULL, report_thread, NULL);
  }

  vita2d_init();
  vita2d_set_clear_color(RGBA8(0, 0, 0, 255));
  vita2d_set_vblank_wait(1);
  g_font = vita2d_load_default_pgf();

  audio_init();
  sv_set_audio(audio_push, NULL);

  char dec_err[96] = "";
  int ret = dec_init();
  if (ret < 0) snprintf(dec_err, sizeof(dec_err), "Video decoder init failed: 0x%08X", (unsigned)ret);

  app_state app;
  if (sv_have_server(g_address, sizeof(g_address))) {
    app = APP_RUN;
    start_session();
  } else {
    app = APP_PAIR_ADDRESS;
    ime_open(IME_ADDRESS, "Surf server address (ip:port)", "", SCE_IME_TYPE_URL, 64);
  }

  int show_overlay = 0; /* SELECT toggles it */
  uint32_t prev_buttons = 0;
  char ime_text[IME_MAX * 4 + 1];

  for (;;) {
    uint64_t now = sceKernelGetProcessTimeWide();
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    uint32_t pressed = pad.buttons & ~prev_buttons;
    prev_buttons = pad.buttons;
    int ime_busy = (g_ime != IME_NONE);
    if (ime_busy) pressed = 0;

    sv_status st = status_snapshot();

    /* ----- logic ----- */
    switch (app) {
      case APP_PAIR_ADDRESS:
      case APP_PAIR_CODE:
        if (!ime_busy && (pressed & SCE_CTRL_CROSS)) {
          if (app == APP_PAIR_ADDRESS)
            ime_open(IME_ADDRESS, "Surf server address (ip:port)", g_pair.address, SCE_IME_TYPE_URL, 64);
          else
            ime_open(IME_CODE, "6-digit code from `surf pair`", "", SCE_IME_TYPE_NUMBER, 6);
        }
        break;
      case APP_PAIRING:
        if (g_pair.done) {
          pthread_join(g_pair.thread, NULL);
          app = g_pair.result == 0 ? APP_PAIR_CONFIRM : APP_PAIR_FAILED;
        }
        break;
      case APP_PAIR_CONFIRM:
        if (pressed & SCE_CTRL_CROSS) {
          g_pair.done = 0;
          g_pair.confirming = 1;
          g_pair.err[0] = 0;
          if (pthread_create(&g_pair.thread, NULL, pair_worker, NULL) == 0) {
            app = APP_PAIR_FINISHING;
          } else {
            sv_pair_cancel();
            snprintf(g_pair.err, sizeof(g_pair.err), "Could not start pairing thread");
            app = APP_PAIR_FAILED;
          }
        } else if (pressed & SCE_CTRL_CIRCLE) {
          sv_pair_cancel();
          app = APP_PAIR_ADDRESS;
          ime_open(IME_ADDRESS, "Surf server address (ip:port)", g_pair.address, SCE_IME_TYPE_URL, 64);
        }
        break;
      case APP_PAIR_FINISHING:
        if (g_pair.done) {
          pthread_join(g_pair.thread, NULL);
          if (g_pair.result == 0) {
            snprintf(g_address, sizeof(g_address), "%s", g_pair.address);
            app = APP_RUN;
            start_session();
          } else {
            app = APP_PAIR_FAILED;
          }
        }
        break;
      case APP_PAIR_FAILED:
        if (pressed & SCE_CTRL_CROSS) {
          app = APP_PAIR_ADDRESS;
          ime_open(IME_ADDRESS, "Surf server address (ip:port)", g_pair.address, SCE_IME_TYPE_URL, 64);
        }
        break;
      case APP_RUN: {
        if (pressed & SCE_CTRL_SELECT) show_overlay = !show_overlay;
        if (st.state == SV_STATE_STREAMING) {
          if (!ime_busy) {
            handle_touch(now);
            handle_stick(&pad, now);
          }
          if (pressed & SCE_CTRL_UP) sv_wheel(0.5f, 0.5f, 0, -DPAD_WHEEL_PX);
          if (pressed & SCE_CTRL_DOWN) sv_wheel(0.5f, 0.5f, 0, DPAD_WHEEL_PX);
          if (pressed & SCE_CTRL_TRIANGLE)
            ime_open(IME_URL, "Address or search", st.url, SCE_IME_TYPE_URL, IME_MAX);
          if (pressed & SCE_CTRL_SQUARE)
            ime_open(IME_TEXT, "Type text", "", SCE_IME_TYPE_DEFAULT, 512);
          if (pressed & SCE_CTRL_CROSS) sv_key(SV_KEY_ENTER);
          if (pressed & SCE_CTRL_CIRCLE) sv_key(SV_KEY_BACKSPACE);
          if (pressed & SCE_CTRL_LTRIGGER) sv_back();
          if (pressed & SCE_CTRL_RTRIGGER) sv_forward();
          if (pressed & SCE_CTRL_START) sv_reload();
        } else {
          touch_release_all();
          if ((st.state == SV_STATE_ERROR || st.state == SV_STATE_IDLE) && (pressed & SCE_CTRL_CROSS)) {
            sv_stop();
            start_session();
          }
          if (st.state == SV_STATE_ERROR && (pressed & SCE_CTRL_TRIANGLE)) {
            /* re-pair */
            sv_stop();
            app = APP_PAIR_ADDRESS;
            ime_open(IME_ADDRESS, "Surf server address (ip:port)", g_address, SCE_IME_TYPE_URL, 64);
          }
        }
        break;
      }
    }

    /* ----- draw ----- */
    g_report_on = (app == APP_RUN);
    /* The hardware decoder outputs a picture only after a few more AUs; on a
     * static page none come, so nudge the backend for keyframes until the
     * latest picture is out. */
    if (app == APP_RUN && dec_pending > 0 &&
        sceKernelGetProcessTimeWide() - dec_last_au_us > 120000)
      sv_request_keyframe();
    g_render_frames++;
    vita2d_start_drawing();
    vita2d_clear_screen();

    vita2d_texture *tex = NULL;
    int fw = 0, fh = 0, is_new = 0;
    uint32_t fseq = 0;
    int have_frame = (app == APP_RUN) && dec_latest(&tex, &fw, &fh, &fseq, &is_new);
    if (have_frame && fw > 0 && fh > 0) {
      vita2d_draw_texture_part_scale(tex, 0, 0, 0, 0, fw, fh,
                                     (float)SCREEN_W / fw, (float)SCREEN_H / fh);
    }

    char line[320];
    switch (app) {
      case APP_PAIR_ADDRESS:
        panel("Pair with a Surf server", COL_ACCENT, "Run `surf pair` on the computer running `surf serve`.",
              "Enter its address (ip:port).", "X: open keyboard");
        break;
      case APP_PAIR_CODE:
        snprintf(line, sizeof(line), "Server: %s", g_pair.address);
        panel("Pair with a Surf server", COL_ACCENT, line, "Enter the 6-digit code shown by `surf pair`.",
              "X: open keyboard");
        break;
      case APP_PAIRING:
        snprintf(line, sizeof(line), "Server: %s", g_pair.address);
        panel("Generating key / pairing...", COL_ACCENT, line,
              "The first pairing can take several seconds.", "");
        break;
      case APP_PAIR_CONFIRM:
        panel("Do these words match?", COL_ACCENT, g_pair.words,
              "Check they match the words shown on the computer.",
              "X: yes, they match    O: no, cancel");
        break;
      case APP_PAIR_FINISHING:
        panel("Finishing pairing...", COL_ACCENT, g_pair.address, "", "");
        break;
      case APP_PAIR_FAILED:
        panel("Pairing failed", COL_ERROR, g_pair.err, "", "X: try again");
        break;
      case APP_RUN:
        if (st.state == SV_STATE_CONNECTING) {
          snprintf(line, sizeof(line), "Connecting to %s ...", g_address);
          panel("Connecting", COL_ACCENT, line, "", "");
        } else if (st.state == SV_STATE_PAUSED) {
          panel("Paused", COL_ACCENT, st.message[0] ? st.message : "The browser is not streaming.",
                "", "Waiting for the server...");
        } else if (st.state == SV_STATE_ERROR) {
          panel("Connection error", COL_ERROR, st.message, g_address, "X: retry    /\\: pair again");
        } else if (st.state == SV_STATE_IDLE) {
          panel("Not connected", COL_GREY, g_address, "", "X: connect");
        } else if (!have_frame) {
          panel("Waiting for video", COL_ACCENT, "", "", "");
        }
        if (show_overlay) draw_overlay(&st);
        break;
    }
    if (dec_err[0]) text(12, SCREEN_H - 40, COL_ERROR, 0.9f, dec_err);

    vita2d_end_drawing();
    vita2d_common_dialog_update();
    /* GPU must be done reading the frame texture before the decoder may reuse
     * it (decoder.h contract), and before we acknowledge presentation. */
    vita2d_wait_rendering_done();
    vita2d_swap_buffers();

    if (have_frame && is_new) sv_presented(fseq);

    /* ----- keyboard results ----- */
    int accepted = 0;
    ime_purpose done = ime_poll(ime_text, sizeof(ime_text), &accepted);
    switch (done) {
      case IME_ADDRESS:
        if (accepted && ime_text[0]) {
          snprintf(g_pair.address, sizeof(g_pair.address), "%s", ime_text);
          app = APP_PAIR_CODE;
          ime_open(IME_CODE, "6-digit code from `surf pair`", "", SCE_IME_TYPE_NUMBER, 6);
        }
        break;
      case IME_CODE:
        if (accepted && strlen(ime_text) == 6) {
          snprintf(g_pair.code, sizeof(g_pair.code), "%s", ime_text);
          g_pair.done = 0;
          g_pair.confirming = 0;
          if (pthread_create(&g_pair.thread, NULL, pair_worker, NULL) == 0) {
            app = APP_PAIRING;
          } else {
            snprintf(g_pair.err, sizeof(g_pair.err), "Could not start pairing thread");
            app = APP_PAIR_FAILED;
          }
        } else if (accepted) {
          ime_open(IME_CODE, "The code has 6 digits", ime_text, SCE_IME_TYPE_NUMBER, 6);
        } else {
          app = APP_PAIR_ADDRESS;
        }
        break;
      case IME_URL:
        if (accepted && ime_text[0]) sv_navigate(ime_text);
        break;
      case IME_TEXT:
        if (accepted && ime_text[0]) sv_text(ime_text);
        break;
      case IME_NONE:
        break;
    }
  }

  /* not reached: the Vita closes apps from the LiveArea */
  sv_stop();
  dec_shutdown();
  vita2d_fini();
  vita2d_free_pgf(g_font);
  sceKernelExitProcess(0);
  return 0;
}
