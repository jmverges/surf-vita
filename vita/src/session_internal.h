/* Internal helpers for the Surf session layer (not part of the app contract).
 * session_net.c: platform shim, pinned TLS, minimal HTTP/1.1, WebSocket, JSON.
 * session.c:     identity, server record, pairing, network thread. */
#ifndef SURF_VITA_SESSION_INTERNAL_H
#define SURF_VITA_SESSION_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ssl.h>

#define SV_APP_VERSION "0.17.0"
#define SV_COMPAT_VERSION "1"
#define SV_WIRE_VERSION "20260831-1"
#define SV_USER_AGENT "Surf Vita/" SV_APP_VERSION

#define SV_HTTP_MAX_BODY (64u * 1024u)
#define SV_WS_MAX_MESSAGE (4u * 1024u * 1024u)

/* ---- platform ---- */
int sv_platform_net_init(char *err, size_t err_len);
uint64_t sv_now_ns(void); /* monotonic */
void sv_sleep_ms(unsigned ms);
const char *sv_data_dir(void); /* no trailing slash */
int sv_mkdir(const char *path);
/* Reads a whole file (<= max bytes) into a malloc'd NUL-terminated buffer. */
int sv_read_file(const char *path, char **out, size_t *out_len, size_t max);
/* Writes path.tmp, then replaces path. */
int sv_write_file_atomic(const char *path, const void *data, size_t len);

/* ---- crypto ---- */
int sv_crypto_init(char *err, size_t err_len);
mbedtls_ctr_drbg_context *sv_drbg(void);
int sv_random(void *out, size_t len);
void sv_hex(const uint8_t *in, size_t len, char *out); /* out: 2*len+1 */
/* base64url without padding. Returns length or -1. */
int sv_b64url(const uint8_t *in, size_t len, char *out, size_t cap);

/* ---- TLS connection ---- */
typedef struct sv_conn {
  int fd;
  int ready;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  uint8_t fingerprint[32]; /* SHA-256 of the presented leaf certificate */
} sv_conn;

/* pin == NULL only for the very first pairing probe. With a pin, a different
 * leaf certificate fails the connection before any application data. */
int sv_conn_open(sv_conn *c, const char *host, uint16_t port, const uint8_t *pin,
                 char *err, size_t err_len);
int sv_conn_write_all(sv_conn *c, const void *data, size_t len);
/* >0 bytes, 0 = no data within the socket timeout, <0 closed/error. */
int sv_conn_read(sv_conn *c, void *buf, size_t len);
void sv_conn_close(sv_conn *c);

/* ---- HTTP (one request per connection) ---- */
typedef struct sv_http_response {
  int status;
  char *body; /* malloc'd, NUL-terminated */
  size_t body_len;
  char cookie[512]; /* "surf_device=..." when Set-Cookie carried it */
} sv_http_response;

int sv_http_request(const char *host, uint16_t port, const uint8_t *pin,
                    const char *method, const char *path, const char *cookie,
                    const char *json_body, sv_http_response *resp,
                    uint8_t observed_fp[32], char *err, size_t err_len);
void sv_http_free(sv_http_response *resp);

/* ---- WebSocket ---- */
enum { SV_WS_TEXT = 1, SV_WS_BINARY = 2, SV_WS_CLOSE = 8, SV_WS_PING = 9, SV_WS_PONG = 10 };

typedef struct sv_ws {
  sv_conn conn;
  uint8_t *rx;      /* raw socket bytes */
  size_t rx_len, rx_cap;
  uint8_t *msg;     /* message reassembly */
  size_t msg_len, msg_cap;
  int msg_opcode;   /* 0 when no fragmented message is pending */
  uint64_t last_rx_ns; /* when the last complete frame (pings included) arrived */
} sv_ws;

int sv_ws_connect(sv_ws *ws, const char *host, uint16_t port, const uint8_t *pin,
                  const char *path, char *err, size_t err_len);
int sv_ws_send(sv_ws *ws, int opcode, const void *data, size_t len);
/* Returns 1 with a complete message (valid until the next call), 0 when no
 * complete message is available yet, <0 on error/close. Pings are answered
 * internally. */
int sv_ws_poll(sv_ws *ws, int *opcode, const uint8_t **data, size_t *len);
void sv_ws_close(sv_ws *ws);

/* ---- tiny JSON (top-level object fields of trusted-shape HTTP replies) ---- */
int sv_json_string(const char *json, const char *key, char *out, size_t cap);
int sv_json_int(const char *json, const char *key, long long *out);
int sv_json_bool(const char *json, const char *key, int *out);
/* Raw value span (for arrays). */
int sv_json_raw(const char *json, const char *key, const char **start, size_t *len);
/* Escapes a string for embedding in JSON. Returns 0 ok. */
int sv_json_escape(const char *in, char *out, size_t cap);

#endif
