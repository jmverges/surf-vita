/* Surf session for PS Vita: pairing, per-server RSA identity, pinned TLS
 * authentication, WebSocket stream, client/core control state and media
 * admission. Flow mirrors client/desktop/crates/surf-session (api.rs, lib.rs). */
#include "session.h"
#include "session_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/asn1write.h>
#include <mbedtls/bignum.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>
#include <pthread.h>

#include "surf/core.h"
#include "surf/frame.h"
#include "surf/input.h"
#include "surf/media.h"
#include "surf/protocol.h"
#include "surf/session.h"

#define SV_DEFAULT_PORT 18080
#define SV_DEVICE_NAME "PS Vita"
#define SV_DEAD_PEER_NS (90ull * 1000000000ull)
#define SV_INPUT_QUEUE 256
#define SV_TEXT_MAX 1024
#define SV_MAX_TOUCH 6
#define SV_PRESENT_RING 32

/* ================================================================ records */

typedef struct sv_record {
  char server_id[65];
  char fingerprint[65];
  char name[128];
  char host[64];
  uint16_t port;
} sv_record;

static sv_audio_fn g_audio_fn;
static void *g_audio_user;
void sv_set_audio(sv_audio_fn fn, void *user) {
  g_audio_fn = fn;
  g_audio_user = user;
}

/* Debug counters for the on-screen overlay. */
static volatile unsigned long g_dbg_binary, g_dbg_badframe, g_dbg_admitted;
void sv_debug_counts(unsigned long *binary, unsigned long *bad, unsigned long *admitted) {
  *binary = g_dbg_binary;
  *bad = g_dbg_badframe;
  *admitted = g_dbg_admitted;
}

static void errf(char *err, size_t len, const char *msg) {
  if (err && len) snprintf(err, len, "%s", msg);
}

static int is_hex64(const char *s) {
  size_t i;
  for (i = 0; i < 64; i++)
    if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
  return s[64] == 0;
}

static int hex_decode32(const char *s, uint8_t out[32]) {
  size_t i;
  if (!is_hex64(s)) return -1;
  for (i = 0; i < 32; i++) {
    int hi = s[2 * i] <= '9' ? s[2 * i] - '0' : s[2 * i] - 'a' + 10;
    int lo = s[2 * i + 1] <= '9' ? s[2 * i + 1] - '0' : s[2 * i + 1] - 'a' + 10;
    out[i] = (uint8_t)(hi * 16 + lo);
  }
  return 0;
}

static int parse_address(const char *address, char *host, size_t host_cap, uint16_t *port) {
  const char *p = address, *colon;
  size_t n;
  long v = SV_DEFAULT_PORT;
  if (!address) return -1;
  while (*p == ' ') p++;
  if (strncmp(p, "https://", 8) == 0) p += 8;
  colon = strchr(p, ':');
  n = colon ? (size_t)(colon - p) : strcspn(p, "/ ");
  if (n == 0 || n >= host_cap) return -1;
  memcpy(host, p, n);
  host[n] = 0;
  if (colon) {
    char *end;
    v = strtol(colon + 1, &end, 10);
    if (end == colon + 1 || (*end && *end != '/' && *end != ' ')) return -1;
  }
  if (v <= 0 || v > 65535) return -1;
  *port = (uint16_t)v;
  return 0;
}

static void record_path(char *out, size_t cap) { snprintf(out, cap, "%s/server.json", sv_data_dir()); }

static int record_load(sv_record *r) {
  char path[256], *data = NULL, endpoint[96];
  long long version = 0;
  int ok = -1;
  memset(r, 0, sizeof(*r));
  record_path(path, sizeof(path));
  if (sv_read_file(path, &data, NULL, 8192) != 0) {
    char tmp[264];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path); /* interrupted replace */
    if (sv_read_file(tmp, &data, NULL, 8192) != 0) return -1;
  }
  if (sv_json_int(data, "version", &version) == 0 && version == 1 &&
      sv_json_string(data, "serverID", r->server_id, sizeof(r->server_id)) == 0 &&
      sv_json_string(data, "fingerprint", r->fingerprint, sizeof(r->fingerprint)) == 0 &&
      sv_json_string(data, "endpoint", endpoint, sizeof(endpoint)) == 0 &&
      is_hex64(r->server_id) && is_hex64(r->fingerprint) &&
      parse_address(endpoint, r->host, sizeof(r->host), &r->port) == 0) {
    if (sv_json_string(data, "name", r->name, sizeof(r->name)) != 0) r->name[0] = 0;
    ok = 0;
  }
  free(data);
  return ok;
}

static int record_save(const sv_record *r) {
  char path[256], json[1024], name[300];
  int n;
  if (sv_mkdir(sv_data_dir()) != 0) return -1;
  if (sv_json_escape(r->name, name, sizeof(name)) != 0) name[0] = 0;
  n = snprintf(json, sizeof(json),
               "{\"version\":1,\"serverID\":\"%s\",\"fingerprint\":\"%s\",\"name\":\"%s\","
               "\"endpoint\":\"%s:%u\"}\n",
               r->server_id, r->fingerprint, name, r->host, (unsigned)r->port);
  if (n < 0 || (size_t)n >= sizeof(json)) return -1;
  record_path(path, sizeof(path));
  return sv_write_file_atomic(path, json, (size_t)n);
}

/* =============================================================== identity */

typedef struct sv_identity {
  mbedtls_pk_context pk;
  uint8_t public_der[600]; /* PKCS#1 RSAPublicKey */
  size_t public_len;
  char device_id[65];
} sv_identity;

static void identity_path(const char *server_id, char *out, size_t cap) {
  snprintf(out, cap, "%s/identities/%s.pem", sv_data_dir(), server_id);
}

static int identity_finish(sv_identity *id) {
  mbedtls_mpi n, e;
  unsigned char *p;
  uint8_t digest[32];
  int len = 0, r;
  if (!mbedtls_pk_can_do(&id->pk, MBEDTLS_PK_RSA) ||
      mbedtls_rsa_get_len(mbedtls_pk_rsa(id->pk)) != 256)
    return -1;
  mbedtls_mpi_init(&n);
  mbedtls_mpi_init(&e);
  if (mbedtls_rsa_export(mbedtls_pk_rsa(id->pk), &n, NULL, NULL, NULL, &e) != 0) goto fail;
  /* Same bytes as the Rust client's to_pkcs1_der(): SEQUENCE { n, e }. */
  p = id->public_der + sizeof(id->public_der);
  if ((r = mbedtls_asn1_write_mpi(&p, id->public_der, &e)) < 0) goto fail;
  len += r;
  if ((r = mbedtls_asn1_write_mpi(&p, id->public_der, &n)) < 0) goto fail;
  len += r;
  if ((r = mbedtls_asn1_write_len(&p, id->public_der, (size_t)len)) < 0) goto fail;
  len += r;
  if ((r = mbedtls_asn1_write_tag(&p, id->public_der,
                                  MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)) < 0)
    goto fail;
  len += r;
  memmove(id->public_der, p, (size_t)len);
  id->public_len = (size_t)len;
  mbedtls_sha256(id->public_der, id->public_len, digest, 0);
  sv_hex(digest, 32, id->device_id);
  mbedtls_mpi_free(&n);
  mbedtls_mpi_free(&e);
  return 0;
fail:
  mbedtls_mpi_free(&n);
  mbedtls_mpi_free(&e);
  return -1;
}

static void identity_free(sv_identity *id) { mbedtls_pk_free(&id->pk); }

static int identity_load(const char *server_id, sv_identity *id) {
  char path[256], *pem = NULL;
  size_t len = 0;
  int r;
  memset(id, 0, sizeof(*id));
  mbedtls_pk_init(&id->pk);
  if (!is_hex64(server_id)) return -1;
  identity_path(server_id, path, sizeof(path));
  if (sv_read_file(path, &pem, &len, 16384) != 0) {
    char tmp[264];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (sv_read_file(tmp, &pem, &len, 16384) != 0) return -1;
  }
  r = mbedtls_pk_parse_key(&id->pk, (const unsigned char *)pem, len + 1, NULL, 0,
                           mbedtls_ctr_drbg_random, sv_drbg());
  memset(pem, 0, len);
  free(pem);
  if (r != 0 || identity_finish(id) != 0) {
    identity_free(id);
    return -1;
  }
  return 0;
}

static int identity_load_or_create(const char *server_id, sv_identity *id, char *err, size_t err_len) {
  char path[256], dir[256];
  unsigned char *pem;
  int r;
  if (identity_load(server_id, id) == 0) return 0;
  memset(id, 0, sizeof(*id));
  mbedtls_pk_init(&id->pk);
  /* RSA-2048 key generation is slow on the Vita CPU (seconds). */
  if (mbedtls_pk_setup(&id->pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0 ||
      mbedtls_rsa_gen_key(mbedtls_pk_rsa(id->pk), mbedtls_ctr_drbg_random, sv_drbg(), 2048,
                          65537) != 0 ||
      identity_finish(id) != 0) {
    errf(err, err_len, "Could not create the device key");
    identity_free(id);
    return -1;
  }
  pem = malloc(8192);
  if (!pem) goto fail;
  r = mbedtls_pk_write_key_pem(&id->pk, pem, 8192);
  snprintf(dir, sizeof(dir), "%s/identities", sv_data_dir());
  identity_path(server_id, path, sizeof(path));
  if (r != 0 || sv_mkdir(sv_data_dir()) != 0 || sv_mkdir(dir) != 0 ||
      sv_write_file_atomic(path, pem, strlen((char *)pem)) != 0) {
    memset(pem, 0, 8192);
    free(pem);
    goto fail;
  }
  memset(pem, 0, 8192);
  free(pem);
  return 0;
fail:
  errf(err, err_len, "Could not save the device key");
  identity_free(id);
  return -1;
}

static const char *const k_words[64] = {
    "amber", "anchor", "apple", "april", "arrow", "atlas", "bamboo", "beacon",
    "birch", "bloom", "blue", "bridge", "brook", "canyon", "cedar", "cloud",
    "coral", "crane", "dawn", "delta", "drift", "eagle", "ember", "fern",
    "field", "finch", "forest", "frost", "garden", "glade", "gold", "harbor",
    "hazel", "heron", "island", "jade", "lake", "leaf", "lilac", "luna",
    "maple", "meadow", "mist", "moon", "north", "ocean", "olive", "orchid",
    "pearl", "pine", "quartz", "rain", "reed", "river", "robin", "sage",
    "shore", "silver", "sky", "stone", "sun", "tide", "willow", "wren",
};

static void pairing_phrase(const sv_identity *id, const char *server_id, char *out, size_t cap) {
  mbedtls_sha256_context ctx;
  uint8_t d[32], v[6];
  size_t i, o = 0;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, (const unsigned char *)"SURF-PAIR-V1\0", 13);
  mbedtls_sha256_update(&ctx, (const unsigned char *)server_id, strlen(server_id));
  mbedtls_sha256_update(&ctx, id->public_der, id->public_len);
  mbedtls_sha256_finish(&ctx, d);
  mbedtls_sha256_free(&ctx);
  v[0] = d[0] >> 2;
  v[1] = (uint8_t)(((d[0] & 3) << 4) | (d[1] >> 4));
  v[2] = (uint8_t)(((d[1] & 15) << 2) | (d[2] >> 6));
  v[3] = d[2] & 63;
  v[4] = d[3] >> 2;
  v[5] = (uint8_t)(((d[3] & 3) << 4) | (d[4] >> 4));
  if (cap) out[0] = 0;
  for (i = 0; i < 6; i++) {
    int n = snprintf(out + o, cap - o, "%s%s", i ? " " : "", k_words[v[i]]);
    if (n < 0 || (size_t)n >= cap - o) return;
    o += (size_t)n;
  }
}

static int sign_auth(sv_identity *id, const char *server_id, const char *challenge_id,
                     const char *nonce, char *out, size_t cap) {
  mbedtls_sha256_context ctx;
  uint8_t digest[32], sig[256];
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, (const unsigned char *)"SURF-AUTH-V1\0", 13);
  mbedtls_sha256_update(&ctx, (const unsigned char *)server_id, strlen(server_id));
  mbedtls_sha256_update(&ctx, (const unsigned char *)"\0", 1);
  mbedtls_sha256_update(&ctx, (const unsigned char *)id->device_id, strlen(id->device_id));
  mbedtls_sha256_update(&ctx, (const unsigned char *)"\0", 1);
  mbedtls_sha256_update(&ctx, (const unsigned char *)challenge_id, strlen(challenge_id));
  mbedtls_sha256_update(&ctx, (const unsigned char *)"\0", 1);
  mbedtls_sha256_update(&ctx, (const unsigned char *)nonce, strlen(nonce));
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  mbedtls_rsa_set_padding(mbedtls_pk_rsa(id->pk), MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE);
  if (mbedtls_rsa_pkcs1_sign(mbedtls_pk_rsa(id->pk), mbedtls_ctr_drbg_random, sv_drbg(),
                             MBEDTLS_MD_SHA256, 32, digest, sig) != 0)
    return -1;
  return sv_b64url(sig, sizeof(sig), out, cap) > 0 ? 0 : -1;
}

/* ================================================================ pairing */

static pthread_mutex_t g_pair_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
  int active;
  sv_record record;
  uint8_t pin[32];
  char status_id[65];
} g_pair;

static int safe_id(const char *s) {
  size_t i;
  for (i = 0; s[i]; i++)
    if (i >= 64 || !((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
  return i > 0;
}

static void http_error(const sv_http_response *resp, const char *fallback, char *err, size_t len) {
  if (resp->body && resp->body_len > 0 && resp->body_len < 200) {
    char msg[256];
    size_t n = resp->body_len;
    memcpy(msg, resp->body, n);
    while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r' || msg[n - 1] == ' ')) n--;
    msg[n] = 0;
    if (err && len) snprintf(err, len, "%s (%d: %s)", fallback, resp->status, msg);
  } else if (err && len) {
    snprintf(err, len, "%s (HTTP %d)", fallback, resp->status);
  }
}

int sv_pair_request(const char *address, const char *code, char *words, size_t words_len,
                    char *err, size_t err_len) {
  sv_record rec;
  sv_http_response resp;
  sv_identity id;
  uint8_t observed[32], advertised[32];
  char api[16], transport[32], observed_hex[65], pubkey[512], body[900];
  char status_id[65], device_id[65], phrase[128], expected[128];
  int i;

  if (words && words_len) words[0] = 0;
  if (!code || strlen(code) != 6) goto bad_code;
  for (i = 0; i < 6; i++)
    if (code[i] < '0' || code[i] > '9') goto bad_code;
  memset(&rec, 0, sizeof(rec));
  if (parse_address(address, rec.host, sizeof(rec.host), &rec.port) != 0) {
    errf(err, err_len, "Enter the server address as IP:port");
    return -1;
  }
  if (sv_platform_net_init(err, err_len) != 0 || sv_crypto_init(err, err_len) != 0) return -1;

  /* 1. Probe: learn the presented certificate and bind it to /server. */
  if (sv_http_request(rec.host, rec.port, NULL, "GET", "/api/v1/server", NULL, NULL, &resp,
                      observed, err, err_len) != 0)
    return -1;
  if (resp.status != 200 || sv_json_string(resp.body, "api", api, sizeof(api)) != 0 ||
      strcmp(api, "v1") != 0 ||
      sv_json_string(resp.body, "serverID", rec.server_id, sizeof(rec.server_id)) != 0 ||
      sv_json_string(resp.body, "fingerprint", rec.fingerprint, sizeof(rec.fingerprint)) != 0) {
    sv_http_free(&resp);
    errf(err, err_len, "This is not a Surf API v1 server");
    return -1;
  }
  if (sv_json_string(resp.body, "name", rec.name, sizeof(rec.name)) != 0) rec.name[0] = 0;
  if (sv_json_string(resp.body, "transport", transport, sizeof(transport)) != 0) transport[0] = 0;
  sv_http_free(&resp);
  if (strcmp(transport, "tunnel") == 0) {
    errf(err, err_len, "Tunneled Surf endpoints are not supported");
    return -1;
  }
  sv_hex(observed, 32, observed_hex);
  if (hex_decode32(rec.fingerprint, advertised) != 0 || memcmp(advertised, observed, 32) != 0 ||
      strcmp(rec.server_id, rec.fingerprint) != 0) {
    errf(err, err_len, "Advertised server identity does not match its TLS certificate");
    return -1;
  }

  /* 2. Per-server device key. */
  if (identity_load_or_create(rec.server_id, &id, err, err_len) != 0) return -1;
  if (sv_b64url(id.public_der, id.public_len, pubkey, sizeof(pubkey)) < 0) {
    identity_free(&id);
    errf(err, err_len, "Could not encode the device key");
    return -1;
  }
  snprintf(body, sizeof(body), "{\"deviceName\":\"%s\",\"publicKey\":\"%s\",\"code\":\"%s\"}",
           SV_DEVICE_NAME, pubkey, code);

  /* 3. Pairing request over the pinned connection. */
  if (sv_http_request(rec.host, rec.port, observed, "POST", "/api/v1/pairing/request", NULL, body,
                      &resp, NULL, err, err_len) != 0) {
    identity_free(&id);
    return -1;
  }
  if (resp.status < 200 || resp.status > 299 ||
      sv_json_string(resp.body, "id", status_id, sizeof(status_id)) != 0 ||
      sv_json_string(resp.body, "deviceID", device_id, sizeof(device_id)) != 0 ||
      sv_json_string(resp.body, "phrase", phrase, sizeof(phrase)) != 0) {
    http_error(&resp, "Pairing was refused", err, err_len);
    sv_http_free(&resp);
    identity_free(&id);
    return -1;
  }
  sv_http_free(&resp);
  pairing_phrase(&id, rec.server_id, expected, sizeof(expected));
  if (strcmp(phrase, expected) != 0 || strcmp(device_id, id.device_id) != 0 || !safe_id(status_id)) {
    identity_free(&id);
    errf(err, err_len, "Pairing response is not bound to this device and server");
    return -1;
  }
  identity_free(&id);
  if (words && words_len) snprintf(words, words_len, "%s", phrase);

  pthread_mutex_lock(&g_pair_lock);
  g_pair.active = 1;
  g_pair.record = rec;
  memcpy(g_pair.pin, observed, 32);
  snprintf(g_pair.status_id, sizeof(g_pair.status_id), "%s", status_id);
  pthread_mutex_unlock(&g_pair_lock);
  return 0;
bad_code:
  errf(err, err_len, "Enter the six-digit code shown by the Surf server");
  return -1;
}

int sv_pair_confirm(char *err, size_t err_len) {
  sv_record rec;
  uint8_t pin[32];
  char path[128], id[65];
  sv_http_response resp;
  int paired = 0, confirmed = 0, approved = 0;

  pthread_mutex_lock(&g_pair_lock);
  if (!g_pair.active) {
    pthread_mutex_unlock(&g_pair_lock);
    errf(err, err_len, "No pairing in progress");
    return -1;
  }
  rec = g_pair.record;
  memcpy(pin, g_pair.pin, 32);
  snprintf(id, sizeof(id), "%s", g_pair.status_id);
  g_pair.active = 0;
  pthread_mutex_unlock(&g_pair_lock);

  snprintf(path, sizeof(path), "/api/v1/pairing/confirm/%s", id);
  if (sv_http_request(rec.host, rec.port, pin, "POST", path, NULL, NULL, &resp, NULL, err,
                      err_len) != 0)
    return -1;
  if (resp.status != 200 || sv_json_bool(resp.body, "paired", &paired) != 0 ||
      sv_json_bool(resp.body, "clientConfirmed", &confirmed) != 0 ||
      sv_json_bool(resp.body, "serverApproved", &approved) != 0 || !paired || !confirmed ||
      !approved) {
    http_error(&resp, "The server did not finish pairing", err, err_len);
    sv_http_free(&resp);
    return -1;
  }
  sv_http_free(&resp);
  if (record_save(&rec) != 0) {
    errf(err, err_len, "Could not save the server record");
    return -1;
  }
  /* Acknowledge so the server drops the completed candidate. Not fatal: the
   * device is paired either way and the candidate expires on its own. */
  snprintf(path, sizeof(path), "/api/v1/pairing/ack/%s", id);
  if (sv_http_request(rec.host, rec.port, pin, "POST", path, NULL, NULL, &resp, NULL, NULL, 0) == 0)
    sv_http_free(&resp);
  return 0;
}

void sv_pair_cancel(void) {
  sv_record rec;
  uint8_t pin[32];
  char path[128];
  sv_http_response resp;
  pthread_mutex_lock(&g_pair_lock);
  if (!g_pair.active) {
    pthread_mutex_unlock(&g_pair_lock);
    return;
  }
  rec = g_pair.record;
  memcpy(pin, g_pair.pin, 32);
  snprintf(path, sizeof(path), "/api/v1/pairing/cancel/%s", g_pair.status_id);
  g_pair.active = 0;
  pthread_mutex_unlock(&g_pair_lock);
  if (sv_http_request(rec.host, rec.port, pin, "POST", path, NULL, NULL, &resp, NULL, NULL, 0) == 0)
    sv_http_free(&resp);
}

int sv_pair(const char *address, const char *code, char *words, size_t words_len, char *err,
            size_t err_len) {
  if (sv_pair_request(address, code, words, words_len, err, err_len) != 0) return -1;
  return sv_pair_confirm(err, err_len);
}

int sv_have_server(char *address, size_t address_len) {
  sv_record rec;
  if (record_load(&rec) != 0) return 0;
  if (address && address_len) snprintf(address, address_len, "%s:%u", rec.host, (unsigned)rec.port);
  return 1;
}

/* ================================================================== input */

enum {
  SV_IN_TOUCH = 1, SV_IN_WHEEL, SV_IN_TEXT, SV_IN_KEY, SV_IN_NAVIGATE,
  SV_IN_BACK, SV_IN_FORWARD, SV_IN_RELOAD, SV_IN_PRESENTED, SV_IN_CLICK, SV_IN_KEYFRAME, SV_IN_TAB
};

typedef struct sv_input {
  int kind;
  int a, b;           /* touch: phase, finger. key: key */
  float x, y, dx, dy;
  uint32_t u;         /* presented sequence */
  char *text;         /* malloc'd */
} sv_input;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static sv_input g_queue[SV_INPUT_QUEUE];
static size_t g_q_head, g_q_len;
static volatile int g_running;
static pthread_t g_thread;
static int g_thread_started;
static sv_callbacks g_cb;
static int g_view_w = 960, g_view_h = 544;

static void queue_push(const sv_input *in) {
  pthread_mutex_lock(&g_lock);
  if (!g_running) {
    pthread_mutex_unlock(&g_lock);
    free(in->text);
    return;
  }
  /* Coalesce: a move replaces a queued move of the same finger at the tail. */
  if (g_q_len > 0 && in->kind == SV_IN_TOUCH && in->a == SV_TOUCH_MOVE) {
    sv_input *last = &g_queue[(g_q_head + g_q_len - 1) % SV_INPUT_QUEUE];
    if (last->kind == SV_IN_TOUCH && last->a == SV_TOUCH_MOVE && last->b == in->b) {
      *last = *in;
      pthread_mutex_unlock(&g_lock);
      return;
    }
  }
  if (g_q_len == SV_INPUT_QUEUE) { /* bounded: drop the newest */
    pthread_mutex_unlock(&g_lock);
    free(in->text);
    return;
  }
  g_queue[(g_q_head + g_q_len) % SV_INPUT_QUEUE] = *in;
  g_q_len++;
  pthread_mutex_unlock(&g_lock);
}

static int queue_pop(sv_input *out) {
  int ok = 0;
  pthread_mutex_lock(&g_lock);
  if (g_q_len > 0) {
    *out = g_queue[g_q_head];
    g_q_head = (g_q_head + 1) % SV_INPUT_QUEUE;
    g_q_len--;
    ok = 1;
  }
  pthread_mutex_unlock(&g_lock);
  return ok;
}

static void queue_clear(void) {
  sv_input in;
  while (queue_pop(&in)) free(in.text);
}

static int finite01(float v) { return v == v && v >= -1.0f && v <= 2.0f; }

void sv_touch(int phase, int finger, float x, float y) {
  sv_input in;
  if (phase < SV_TOUCH_DOWN || phase > SV_TOUCH_UP || finger < 0 || finger > 63 || !finite01(x) ||
      !finite01(y))
    return;
  memset(&in, 0, sizeof(in));
  in.kind = SV_IN_TOUCH;
  in.a = phase;
  in.b = finger;
  in.x = x < 0 ? 0 : x > 1 ? 1 : x;
  in.y = y < 0 ? 0 : y > 1 ? 1 : y;
  queue_push(&in);
}

void sv_wheel(float x, float y, float dx, float dy) {
  sv_input in;
  if (!finite01(x) || !finite01(y) || dx != dx || dy != dy) return;
  memset(&in, 0, sizeof(in));
  in.kind = SV_IN_WHEEL;
  in.x = x; in.y = y; in.dx = dx; in.dy = dy;
  queue_push(&in);
}

static void push_text(int kind, const char *s) {
  sv_input in;
  size_t n;
  if (!s) return;
  n = strlen(s);
  if (n == 0 || n > SV_TEXT_MAX) return;
  memset(&in, 0, sizeof(in));
  in.kind = kind;
  in.text = malloc(n + 1);
  if (!in.text) return;
  memcpy(in.text, s, n + 1);
  queue_push(&in);
}

void sv_text(const char *utf8) { push_text(SV_IN_TEXT, utf8); }
void sv_navigate(const char *url_or_search) { push_text(SV_IN_NAVIGATE, url_or_search); }

static void push_simple(int kind, int a, uint32_t u) {
  sv_input in;
  memset(&in, 0, sizeof(in));
  in.kind = kind;
  in.a = a;
  in.u = u;
  queue_push(&in);
}

void sv_key(int key) {
  if (key >= SV_KEY_ENTER && key <= SV_KEY_RIGHT) push_simple(SV_IN_KEY, key, 0);
}
void sv_back(void) { push_simple(SV_IN_BACK, 0, 0); }
void sv_forward(void) { push_simple(SV_IN_FORWARD, 0, 0); }
void sv_reload(void) { push_simple(SV_IN_RELOAD, 0, 0); }
void sv_presented(uint32_t sequence) { push_simple(SV_IN_PRESENTED, 0, sequence); }

void sv_request_keyframe(void) { push_simple(SV_IN_KEYFRAME, 0, 0); }

enum { SV_TAB_SELECT = 0, SV_TAB_CLOSE, SV_TAB_NEW };
void sv_tab_select(int64_t id) { push_simple(SV_IN_TAB, SV_TAB_SELECT, (uint32_t)id); }
void sv_tab_close(int64_t id) { push_simple(SV_IN_TAB, SV_TAB_CLOSE, (uint32_t)id); }
void sv_tab_new(void) { push_simple(SV_IN_TAB, SV_TAB_NEW, 0); }

void sv_click(float x, float y) {
  sv_input in;
  if (!finite01(x) || !finite01(y)) return;
  memset(&in, 0, sizeof(in));
  in.kind = SV_IN_CLICK;
  in.x = x;
  in.y = y;
  queue_push(&in);
}

/* ======================================================== network thread */

typedef struct sv_conn_state {
  sv_ws ws;
  surf_core_t *core;
  surf_protocol_workspace_t *workspace;
  void *workspace_mem;
  uint64_t generation;
  surf_media_policy_t media;
  surf_input_state_t input;
  uint32_t admitted_generation, presented_generation;
  struct { uint32_t seq, gen, source; int used; } ring[SV_PRESENT_RING];
  size_t ring_next;
  struct { int used; int finger; double x, y; } touch[SV_MAX_TOUCH];
  int cap_pointer, cap_browser_setup, cap_reqkeyframe;
  int paused;
  uint64_t last_keyframe_request_ns;
  sv_status status;
  char *cmd;
  size_t cmd_cap;
} sv_conn_state;

static void emit_status(sv_conn_state *s, sv_state state, const char *message) {
  sv_status next = s->status;
  next.state = state;
  if (message) snprintf(next.message, sizeof(next.message), "%s", message);
  else next.message[0] = 0;
  if (memcmp(&next, &s->status, sizeof(next)) != 0) {
    s->status = next;
    if (g_cb.on_status) g_cb.on_status(g_cb.user, &s->status);
  }
}

static int send_command(sv_conn_state *s, surf_protocol_command_t *cmd) {
  size_t len = 0;
  surf_protocol_result_t r = surf_protocol_encode_command(cmd, s->cmd, s->cmd_cap, &len);
  if (r == SURF_PROTOCOL_ERROR_BUFFER) {
    char *grown = realloc(s->cmd, len + 1);
    if (!grown) return -1;
    s->cmd = grown;
    s->cmd_cap = len + 1;
    r = surf_protocol_encode_command(cmd, s->cmd, s->cmd_cap, &len);
  }
  if (r != SURF_PROTOCOL_OK) return 0; /* invalid command: drop it, keep the session */
  return sv_ws_send(&s->ws, SV_WS_TEXT, s->cmd, len);
}

static int send_simple(sv_conn_state *s, surf_protocol_command_kind_t kind) {
  surf_protocol_command_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.kind = kind;
  return send_command(s, &cmd);
}

static int send_size(sv_conn_state *s) {
  surf_protocol_command_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.kind = SURF_PROTOCOL_COMMAND_SIZE;
  cmd.data.size.width = g_view_w & ~1;
  cmd.data.size.height = g_view_h & ~1;
  return send_command(s, &cmd);
}

static int request_keyframe(sv_conn_state *s) {
  uint64_t now = sv_now_ns();
  if (!s->cap_reqkeyframe) return 0;
  if (s->last_keyframe_request_ns && now - s->last_keyframe_request_ns < 500000000ull) return 0;
  s->last_keyframe_request_ns = now;
  return send_simple(s, SURF_PROTOCOL_COMMAND_REQUEST_KEYFRAME);
}

static uint32_t input_surface(const sv_conn_state *s) {
  return s->presented_generation ? s->presented_generation : s->admitted_generation;
}

static int send_touch(sv_conn_state *s, const char *phase, int finger_index_only) {
  surf_protocol_command_t cmd;
  surf_protocol_touch_point_t points[SV_MAX_TOUCH];
  surf_input_sample_t sample;
  size_t n = 0, i;
  uint32_t surface = input_surface(s);
  if (surface == 0) return 0; /* nothing presented yet: no surface to touch */
  surf_input_set_surface(&s->input, surface);
  for (i = 0; i < SV_MAX_TOUCH; i++) {
    if (!s->touch[i].used) continue;
    if (finger_index_only >= 0 && (int)i != finger_index_only) continue;
    memset(&points[n], 0, sizeof(points[n]));
    points[n].id = s->touch[i].finger;
    points[n].x = s->touch[i].x;
    points[n].y = s->touch[i].y;
    n++;
  }
  if (n == 0) return 0;
  if (surf_input_pointer_sample(&s->input, points[0].x, points[0].y, 1.0, 1.0, sv_now_ns(),
                                &sample) != SURF_INPUT_OK)
    return 0;
  memset(&cmd, 0, sizeof(cmd));
  cmd.kind = SURF_PROTOCOL_COMMAND_TOUCH;
  cmd.causal.interaction_id = sample.interaction_id;
  cmd.causal.client_ns = sample.client_ns;
  cmd.data.touch.phase = surf_string_from_cstr(phase);
  cmd.data.touch.sequence = sample.sequence;
  cmd.data.touch.timestamp_ns = sample.event_ns ? sample.event_ns : sample.client_ns;
  cmd.data.touch.surface = surface;
  cmd.data.touch.points = points;
  cmd.data.touch.point_count = n;
  return send_command(s, &cmd);
}

/* A left click at a surface fraction: pointer move, down, up (desktop mouse
 * semantics, backend pointer_input.go). Needs the `pointer-input` capability. */
static int send_pointer(sv_conn_state *s, const char *phase, double x, double y, int buttons,
                        int clicks) {
  surf_protocol_command_t cmd;
  surf_input_sample_t sample;
  uint32_t surface = input_surface(s);
  if (!s->cap_pointer || surface == 0 || s->paused) return 0;
  surf_input_set_surface(&s->input, surface);
  if (surf_input_pointer_sample(&s->input, x, y, 1.0, 1.0, sv_now_ns(), &sample) != SURF_INPUT_OK)
    return 0;
  memset(&cmd, 0, sizeof(cmd));
  cmd.kind = SURF_PROTOCOL_COMMAND_POINTER;
  cmd.causal.interaction_id = sample.interaction_id;
  cmd.causal.client_ns = sample.client_ns;
  cmd.data.pointer.phase = surf_string_from_cstr(phase);
  cmd.data.pointer.button = surf_string_from_cstr(clicks ? "left" : "none");
  cmd.data.pointer.sequence = sample.sequence;
  cmd.data.pointer.timestamp_ns = sample.event_ns ? sample.event_ns : sample.client_ns;
  cmd.data.pointer.surface = surface;
  cmd.data.pointer.x = x;
  cmd.data.pointer.y = y;
  cmd.data.pointer.buttons = buttons;
  cmd.data.pointer.clicks = clicks;
  return send_command(s, &cmd);
}

static int send_click(sv_conn_state *s, double x, double y) {
  if (send_pointer(s, "move", x, y, 0, 0) != 0) return -1;
  if (send_pointer(s, "down", x, y, 1, 1) != 0) return -1;
  return send_pointer(s, "up", x, y, 0, 1);
}

static int handle_touch(sv_conn_state *s, const sv_input *in) {
  int i, slot = -1, r = 0;
  for (i = 0; i < SV_MAX_TOUCH; i++)
    if (s->touch[i].used && s->touch[i].finger == in->b) slot = i;
  if (s->paused) return 0;
  if (in->a == SV_TOUCH_DOWN) {
    if (slot < 0)
      for (i = 0; i < SV_MAX_TOUCH && slot < 0; i++)
        if (!s->touch[i].used) slot = i;
    if (slot < 0) return 0;
    s->touch[slot].used = 1;
    s->touch[slot].finger = in->b;
    s->touch[slot].x = in->x;
    s->touch[slot].y = in->y;
    return send_touch(s, "start", slot);
  }
  if (slot < 0) return 0;
  s->touch[slot].x = in->x;
  s->touch[slot].y = in->y;
  if (in->a == SV_TOUCH_MOVE) return send_touch(s, "move", -1); /* full active snapshot */
  r = send_touch(s, "end", slot);
  s->touch[slot].used = 0;
  return r;
}

static int handle_key(sv_conn_state *s, int key) {
  static const struct { const char *name; int code; } keys[] = {
      {"Enter", 13}, {"Backspace", 8}, {"Tab", 9}, {"Escape", 27},
      {"ArrowUp", 38}, {"ArrowDown", 40}, {"ArrowLeft", 37}, {"ArrowRight", 39}};
  surf_protocol_command_t cmd;
  int down;
  if (key < 1 || key > 8 || s->paused) return 0;
  for (down = 1; down >= 0; down--) { /* iOS sendKeyName: down then up */
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = SURF_PROTOCOL_COMMAND_KEY;
    cmd.data.key.down = down;
    cmd.data.key.key = surf_string_from_cstr(keys[key - 1].name);
    cmd.data.key.code = surf_string_from_cstr(keys[key - 1].name);
    cmd.data.key.text = surf_string_from_cstr("");
    cmd.data.key.key_code = keys[key - 1].code;
    if (send_command(s, &cmd) != 0) return -1;
  }
  return 0;
}

static int handle_input(sv_conn_state *s, sv_input *in) {
  surf_protocol_command_t cmd;
  int r = 0;
  memset(&cmd, 0, sizeof(cmd));
  switch (in->kind) {
  case SV_IN_TOUCH: r = handle_touch(s, in); break;
  case SV_IN_CLICK: r = send_click(s, in->x, in->y); break;
  case SV_IN_TAB: {
    static const char *actions[] = {"select", "close", "new"};
    if (in->a < SV_TAB_SELECT || in->a > SV_TAB_NEW) break;
    cmd.kind = SURF_PROTOCOL_COMMAND_TAB;
    cmd.data.tab.action = surf_string_from_cstr(actions[in->a]);
    cmd.data.tab.id = (int32_t)in->u;
    r = send_command(s, &cmd);
    break;
  }
  case SV_IN_KEYFRAME: { /* the app's decoder is holding pictures back */
    uint64_t now = sv_now_ns();
    if (!s->cap_reqkeyframe || s->paused) break;
    if (s->last_keyframe_request_ns && now - s->last_keyframe_request_ns < 100000000ull) break;
    s->last_keyframe_request_ns = now;
    r = send_simple(s, SURF_PROTOCOL_COMMAND_REQUEST_KEYFRAME);
    break;
  }
  case SV_IN_WHEEL: {
    surf_input_sample_t sample;
    uint32_t surface = input_surface(s);
    if (!s->cap_pointer || surface == 0 || s->paused) break;
    surf_input_set_surface(&s->input, surface);
    if (surf_input_wheel_sample(&s->input, (double)in->x * g_view_w, (double)in->y * g_view_h,
                                in->dx, in->dy, g_view_w, g_view_h, sv_now_ns(),
                                &sample) != SURF_INPUT_OK)
      break;
    cmd.kind = SURF_PROTOCOL_COMMAND_WHEEL;
    cmd.causal.interaction_id = sample.interaction_id;
    cmd.causal.client_ns = sample.client_ns;
    cmd.data.wheel.sequence = sample.sequence;
    cmd.data.wheel.timestamp_ns = sample.event_ns ? sample.event_ns : sample.client_ns;
    cmd.data.wheel.surface = surface;
    cmd.data.wheel.x = sample.x;
    cmd.data.wheel.y = sample.y;
    cmd.data.wheel.delta_x = sample.delta_x;
    cmd.data.wheel.delta_y = sample.delta_y;
    r = send_command(s, &cmd);
    break;
  }
  case SV_IN_TEXT: /* iOS: {"t":"key","text":...} */
    if (s->paused) break;
    cmd.kind = SURF_PROTOCOL_COMMAND_KEY;
    cmd.data.key.key = surf_string_from_cstr("");
    cmd.data.key.code = surf_string_from_cstr("");
    cmd.data.key.text = surf_string_from_cstr(in->text);
    r = send_command(s, &cmd);
    break;
  case SV_IN_KEY: r = handle_key(s, in->a); break;
  case SV_IN_NAVIGATE:
    cmd.kind = SURF_PROTOCOL_COMMAND_NAVIGATE;
    cmd.data.url.url = surf_string_from_cstr(in->text);
    r = send_command(s, &cmd);
    break;
  case SV_IN_BACK: r = send_simple(s, SURF_PROTOCOL_COMMAND_BACK); break;
  case SV_IN_FORWARD: r = send_simple(s, SURF_PROTOCOL_COMMAND_FORWARD); break;
  case SV_IN_RELOAD: r = send_simple(s, SURF_PROTOCOL_COMMAND_RELOAD); break;
  case SV_IN_PRESENTED: {
    size_t i;
    for (i = 0; i < SV_PRESENT_RING; i++) {
      if (s->ring[i].used && s->ring[i].seq == in->u) {
        s->presented_generation = s->ring[i].gen;
        surf_core_present_frame(s->core, s->generation, s->ring[i].gen, s->ring[i].source);
        break;
      }
    }
    break;
  }
  default: break;
  }
  free(in->text);
  in->text = NULL;
  return r;
}

static void refresh_status(sv_conn_state *s) {
  surf_snapshot_t snap;
  surf_browser_mode_snapshot_t mode;
  surf_effect_t effect;
  sv_status next = s->status;
  int was_paused = s->paused;
  while (surf_core_next_effect(s->core, &effect)) {
  }
  if (surf_core_snapshot(s->core, &snap) == SURF_CORE_OK) {
    size_t n = snap.active_title.length < sizeof(next.title) - 1 ? snap.active_title.length
                                                                  : sizeof(next.title) - 1;
    if (snap.active_title.data) memcpy(next.title, snap.active_title.data, n);
    next.title[n] = 0;
    n = snap.current_url.length < sizeof(next.url) - 1 ? snap.current_url.length
                                                         : sizeof(next.url) - 1;
    if (snap.current_url.data) memcpy(next.url, snap.current_url.data, n);
    next.url[n] = 0;
    next.loading = snap.loading ? 1 : 0;
    next.tab_count = 0;
    for (size_t t = 0; t < snap.tab_count && next.tab_count < SV_MAX_TABS; t++) {
      sv_tab *dst = &next.tabs[next.tab_count++];
      size_t tn = snap.tabs[t].title.length < sizeof(dst->title) - 1 ? snap.tabs[t].title.length
                                                                      : sizeof(dst->title) - 1;
      dst->id = snap.tabs[t].id;
      dst->active = snap.tabs[t].active || (snap.has_active_tab && snap.tabs[t].id == snap.active_tab_id);
      if (snap.tabs[t].title.data && tn) memcpy(dst->title, snap.tabs[t].title.data, tn);
      dst->title[tn] = 0;
    }
  }
  s->paused = 0;
  next.state = SV_STATE_STREAMING;
  next.message[0] = 0;
  if (surf_core_browser_mode(s->core, &mode) == SURF_CORE_OK && mode.paused) {
    size_t n = mode.message.length < sizeof(next.message) - 1 ? mode.message.length
                                                               : sizeof(next.message) - 1;
    s->paused = 1;
    next.state = SV_STATE_PAUSED;
    if (mode.message.data && n) memcpy(next.message, mode.message.data, n);
    else snprintf(next.message, sizeof(next.message), "Browser %.*s", (int)(mode.state.length > 32 ? 32 : mode.state.length), mode.state.data ? mode.state.data : "");
    next.message[n ? n : strlen(next.message)] = 0;
  }
  if (was_paused && !s->paused) {
    /* docs/protocol.md: re-send the viewport and start fresh media. */
    surf_media_policy_reset(&s->media);
    send_size(s);
  }
  if (!was_paused && s->paused) memset(s->touch, 0, sizeof(s->touch));
  if (memcmp(&next, &s->status, sizeof(next)) != 0) {
    s->status = next;
    if (g_cb.on_status) g_cb.on_status(g_cb.user, &s->status);
  }
}

static int handle_text(sv_conn_state *s, const uint8_t *data, size_t len) {
  surf_protocol_event_t event;
  if (surf_protocol_decode_event(s->workspace, (const char *)data, len, &event) != SURF_PROTOCOL_OK)
    return 0; /* unknown or oversized event: ignore, keep streaming */
  if (event.kind == SURF_PROTOCOL_EVENT_VIDEO_CONFIG && event.data.video.generation != 0 &&
      event.data.video.generation != s->admitted_generation) {
    /* A new encoder generation: wait for its IDR. */
    surf_media_policy_reset(&s->media);
  }
  surf_core_dispatch_protocol(s->core, s->generation, &event);
  refresh_status(s);
  return 0;
}

static int handle_binary(sv_conn_state *s, const uint8_t *data, size_t len) {
  surf_frame_view_t frame;
  surf_media_admission_t adm;
  int idr;
  g_dbg_binary++;
  if (surf_frame_parse(data, len, &frame) != SURF_FRAME_OK) { g_dbg_badframe++; return 0; } /* drop bad frames */
  if (frame.type == SURF_FRAME_TYPE_AUDIO) {
    /* width = sample rate, height = channels (docs/protocol.md media envelope) */
    if (g_audio_fn && !s->paused && frame.payload_length >= 2 && frame.width > 0 &&
        frame.height > 0)
      g_audio_fn(g_audio_user, (const int16_t *)frame.payload, frame.payload_length / 2,
                 frame.width, frame.height);
    return 0;
  }
  if (frame.type != SURF_FRAME_TYPE_VIDEO || s->paused) return 0;
  idr = (frame.flags & SURF_FRAME_FLAG_IDR) != 0;
  if (surf_media_policy_admit(&s->media, frame.encoder_generation, frame.sequence, idr, &adm) !=
      SURF_MEDIA_OK)
    return 0;
  if (adm.action == SURF_MEDIA_ACTION_DROP_REQUEST_KEYFRAME) return request_keyframe(s);
  s->admitted_generation = frame.encoder_generation;
  s->ring[s->ring_next].used = 1;
  s->ring[s->ring_next].seq = frame.sequence;
  s->ring[s->ring_next].gen = frame.encoder_generation;
  s->ring[s->ring_next].source = frame.source_sequence;
  s->ring_next = (s->ring_next + 1) % SV_PRESENT_RING;
  g_dbg_admitted++;
  if (g_cb.on_video && frame.payload_length > 0)
    g_cb.on_video(g_cb.user, frame.payload, frame.payload_length, idr, frame.width, frame.height,
                  frame.sequence);
  return 0;
}

/* Outcome of one connection attempt. */
enum { SV_FATAL = 0, SV_RETRY = 1, SV_STOPPED = 2 };

static int authenticate(const sv_record *rec, const uint8_t pin[32], char *ticket, size_t ticket_cap,
                        sv_conn_state *s, char *err, size_t err_len, int *fatal) {
  sv_identity id;
  sv_http_response resp;
  char body[1024], cid[128], dev[65], nonce[128], sig[400], cookie[512], path[160];
  char compat[64], caps_buf[1];
  const char *caps;
  size_t caps_len;
  long long cv = 0;
  (void)caps_buf;
  *fatal = 0;
  if (identity_load(rec->server_id, &id) != 0) {
    errf(err, err_len, "This server's device key is missing; pair again");
    *fatal = 1;
    return -1;
  }
  snprintf(body, sizeof(body), "{\"deviceID\":\"%s\"}", id.device_id);
  if (sv_http_request(rec->host, rec->port, pin, "POST", "/api/v1/auth/challenge", NULL, body,
                      &resp, NULL, err, err_len) != 0)
    goto fail_id;
  if (resp.status == 401) {
    sv_http_free(&resp);
    errf(err, err_len, "This device is no longer paired with the server");
    *fatal = 1;
    goto fail_id;
  }
  if (resp.status != 200 || sv_json_string(resp.body, "id", cid, sizeof(cid)) != 0 ||
      sv_json_string(resp.body, "deviceID", dev, sizeof(dev)) != 0 ||
      sv_json_string(resp.body, "nonce", nonce, sizeof(nonce)) != 0 || strcmp(dev, id.device_id) != 0 ||
      cid[0] == 0 || nonce[0] == 0) {
    http_error(&resp, "Authentication challenge failed", err, err_len);
    sv_http_free(&resp);
    goto fail_id;
  }
  sv_http_free(&resp);
  if (sign_auth(&id, rec->server_id, cid, nonce, sig, sizeof(sig)) != 0) {
    errf(err, err_len, "Could not sign the authentication challenge");
    goto fail_id;
  }
  identity_free(&id);
  snprintf(body, sizeof(body), "{\"challengeID\":\"%s\",\"signature\":\"%s\"}", cid, sig);
  if (sv_http_request(rec->host, rec->port, pin, "POST", "/api/v1/auth/complete", NULL, body, &resp,
                      NULL, err, err_len) != 0)
    return -1;
  if (resp.status < 200 || resp.status > 299 || resp.cookie[0] == 0) {
    if (resp.status == 401) *fatal = 1;
    http_error(&resp, "Authentication failed", err, err_len);
    sv_http_free(&resp);
    return -1;
  }
  snprintf(cookie, sizeof(cookie), "%s", resp.cookie);
  sv_http_free(&resp);

  snprintf(path, sizeof(path), "/api/v1/config?av=%s&cv=%s&nv=%s", SV_APP_VERSION, SV_COMPAT_VERSION,
           SV_WIRE_VERSION);
  if (sv_http_request(rec->host, rec->port, pin, "GET", path, cookie, NULL, &resp, NULL, err,
                      err_len) != 0)
    return -1;
  if (resp.status != 200 || sv_json_string(resp.body, "ticket", ticket, ticket_cap) != 0 ||
      sv_json_string(resp.body, "compatibility", compat, sizeof(compat)) != 0 ||
      sv_json_int(resp.body, "compatibilityVersion", &cv) != 0) {
    http_error(&resp, "Server configuration failed", err, err_len);
    sv_http_free(&resp);
    return -1;
  }
  if (strcmp(compat, "compatible") != 0 || cv != atoi(SV_COMPAT_VERSION)) {
    if (err && err_len)
      snprintf(err, err_len, "%s",
               strcmp(compat, "client-update-required") == 0 ? "This Surf Vita client is too old for the server"
                                                             : "The Surf server needs an update");
    sv_http_free(&resp);
    *fatal = 1;
    return -1;
  }
  if (sv_json_raw(resp.body, "caps", &caps, &caps_len) == 0) {
    char buf[2048];
    size_t n = caps_len < sizeof(buf) - 1 ? caps_len : sizeof(buf) - 1;
    memcpy(buf, caps, n);
    buf[n] = 0;
    s->cap_pointer = strstr(buf, "\"pointer-input\"") != NULL;
    s->cap_browser_setup = strstr(buf, "\"browser-setup\"") != NULL;
    s->cap_reqkeyframe = strstr(buf, "\"reqkeyframe\"") != NULL;
  }
  sv_http_free(&resp);
  if (ticket[0] == 0 || !safe_id(ticket)) {
    errf(err, err_len, "Server configuration omitted the stream ticket");
    return -1;
  }
  return 0;
fail_id:
  identity_free(&id);
  return -1;
}

static int run_connection(sv_conn_state *s, uint64_t *connected_ms, char *err, size_t err_len) {
  sv_record rec;
  uint8_t pin[32];
  char ticket[160], path[320];
  int fatal = 0, r;
  uint64_t start_ns, last_rx;

  *connected_ms = 0;
  if (record_load(&rec) != 0 || hex_decode32(rec.fingerprint, pin) != 0) {
    errf(err, err_len, "No paired Surf server");
    return SV_FATAL;
  }
  if (authenticate(&rec, pin, ticket, sizeof(ticket), s, err, err_len, &fatal) != 0) {
    if (err && strstr(err, "Server Identity Changed")) fatal = 1;
    return fatal ? SV_FATAL : SV_RETRY;
  }
  snprintf(path, sizeof(path), "/api/v1/ws?ticket=%s&cv=%s&nv=%s", ticket, SV_COMPAT_VERSION,
           SV_WIRE_VERSION);
  if (sv_ws_connect(&s->ws, rec.host, rec.port, pin, path, err, err_len) != 0)
    return (err && strstr(err, "Server Identity Changed")) ? SV_FATAL : SV_RETRY;

  /* New authenticated control epoch. */
  s->generation++;
  surf_core_begin_connection(s->core, s->generation);
  surf_media_policy_init(&s->media);
  surf_input_state_init(&s->input);
  s->admitted_generation = s->presented_generation = 0;
  memset(s->ring, 0, sizeof(s->ring));
  memset(s->touch, 0, sizeof(s->touch));
  s->paused = 0;
  s->last_keyframe_request_ns = 0;
  queue_clear(); /* inputs aimed at an older surface are stale */

  if (send_size(s) != 0 ||
      (s->cap_browser_setup && send_simple(s, SURF_PROTOCOL_COMMAND_BROWSER_WATCH) != 0)) {
    sv_ws_close(&s->ws);
    return SV_RETRY;
  }
  {
    surf_protocol_command_t audio; /* audio on only when the app plays it */
    memset(&audio, 0, sizeof(audio));
    audio.kind = SURF_PROTOCOL_COMMAND_AUDIO;
    audio.data.toggle.on = g_audio_fn != NULL;
    send_command(s, &audio);
  }
  emit_status(s, SV_STATE_STREAMING, NULL);
  refresh_status(s);

  start_ns = last_rx = sv_now_ns();
  for (;;) {
    int opcode, handled = 0;
    const uint8_t *data;
    size_t len;
    sv_input in;
    if (!g_running) {
      sv_ws_close(&s->ws);
      return SV_STOPPED;
    }
    /* Drain a bounded burst of messages, then service input. */
    while (handled < 16 && (r = sv_ws_poll(&s->ws, &opcode, &data, &len)) == 1) {
      handled++;
      last_rx = sv_now_ns();
      if (opcode == SV_WS_TEXT) r = handle_text(s, data, len);
      else r = handle_binary(s, data, len);
      if (r != 0) break;
    }
    if (r < 0) break;
    while (queue_pop(&in)) {
      if (handle_input(s, &in) != 0) {
        r = -1;
        break;
      }
    }
    if (r < 0) break;
    /* Server pings (every 30 s) count as liveness too: an idle page sends no events. */
    if (s->ws.last_rx_ns > last_rx) last_rx = s->ws.last_rx_ns;
    if (sv_now_ns() - last_rx > SV_DEAD_PEER_NS) break;
  }
  *connected_ms = (sv_now_ns() - start_ns) / 1000000u;
  sv_ws_close(&s->ws);
  errf(err, err_len, "The secure session ended");
  return SV_RETRY;
}

static void *network_thread(void *arg) {
  sv_conn_state *s = arg;
  surf_reconnect_policy_t policy;
  char err[256];
  (void)arg;
  surf_reconnect_policy_init(&policy);
  emit_status(s, SV_STATE_CONNECTING, NULL);
  while (g_running) {
    uint64_t connected_ms = 0;
    uint8_t attempt = 0;
    uint32_t delay = 0;
    int outcome;
    err[0] = 0;
    outcome = run_connection(s, &connected_ms, err, sizeof(err));
    if (outcome == SV_STOPPED || !g_running) break;
    if (surf_reconnect_policy_failure(&policy, outcome == SV_RETRY, connected_ms, &attempt, &delay) !=
        SURF_RECONNECT_RETRY) {
      emit_status(s, SV_STATE_ERROR, err[0] ? err : "Connection failed");
      while (g_running) sv_sleep_ms(50);
      break;
    }
    {
      char msg[300];
      snprintf(msg, sizeof(msg), "%s; reconnecting (%u)", err[0] ? err : "Disconnected",
               (unsigned)attempt);
      emit_status(s, SV_STATE_CONNECTING, msg);
    }
    while (g_running && delay > 0) {
      unsigned step = delay > 50 ? 50 : delay;
      sv_sleep_ms(step);
      delay -= step;
    }
  }
  return NULL;
}

static sv_conn_state *g_state;

static void state_free(sv_conn_state *s) {
  if (!s) return;
  if (s->core) surf_core_destroy(s->core);
  free(s->workspace_mem);
  free(s->cmd);
  free(s);
}

int sv_start(int width, int height, const sv_callbacks *cb) {
  sv_conn_state *s;
  surf_core_config_t config;
  surf_protocol_workspace_config_t wcfg;
  size_t wsize;
  pthread_attr_t attr;
  char err[128];

  if (g_thread_started || cb == NULL) return -1;
  if (!sv_have_server(NULL, 0)) return -1;
  if (sv_platform_net_init(err, sizeof(err)) != 0 || sv_crypto_init(err, sizeof(err)) != 0) return -1;
  s = calloc(1, sizeof(*s));
  if (!s) return -1;
  surf_core_config_init(&config);
  if (surf_core_create(&config, &s->core) != SURF_CORE_OK) goto fail;
  /* Smaller than the desktop defaults: bounded memory on the Vita. Larger
   * events fail to decode and are ignored. */
  surf_protocol_workspace_config_init(&wcfg);
  wcfg.token_capacity = 8192;
  wcfg.item_capacity = 1024;
  wcfg.string_capacity = 512 * 1024;
  wsize = surf_protocol_workspace_size(&wcfg);
  s->workspace_mem = malloc(wsize);
  if (!s->workspace_mem ||
      surf_protocol_workspace_init(&s->workspace, s->workspace_mem, wsize, &wcfg) != SURF_PROTOCOL_OK)
    goto fail;
  s->cmd_cap = 4096;
  s->cmd = malloc(s->cmd_cap);
  if (!s->cmd) goto fail;

  g_cb = *cb;
  g_view_w = width > 0 ? width : 960;
  g_view_h = height > 0 ? height : 544;
  g_state = s;
  pthread_mutex_lock(&g_lock);
  g_running = 1;
  pthread_mutex_unlock(&g_lock);
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 512 * 1024); /* RSA + TLS need stack */
  if (pthread_create(&g_thread, &attr, network_thread, s) != 0) {
    pthread_attr_destroy(&attr);
    g_running = 0;
    g_state = NULL;
    goto fail;
  }
  pthread_attr_destroy(&attr);
  g_thread_started = 1;
  return 0;
fail:
  state_free(s);
  return -1;
}

void sv_stop(void) {
  if (!g_thread_started) return;
  pthread_mutex_lock(&g_lock);
  g_running = 0;
  pthread_mutex_unlock(&g_lock);
  pthread_join(g_thread, NULL);
  g_thread_started = 0;
  queue_clear();
  state_free(g_state);
  g_state = NULL;
  memset(&g_cb, 0, sizeof(g_cb));
}
