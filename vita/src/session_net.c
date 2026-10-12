/* Platform shim, pinned TLS (mbedTLS), minimal HTTP/1.1, WebSocket client and
 * a tiny JSON field reader for the Surf session layer. Everything read from
 * the network is bounded; malformed input fails the call, never the app. */
#include "session_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/base64.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include <pthread.h>

#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#endif

#define SV_SOCKET_TIMEOUT_MS 8
#define SV_HTTP_TIMEOUT_MS 20000
#define SV_HANDSHAKE_TIMEOUT_MS 20000

static void set_err(char *err, size_t err_len, const char *fmt, const char *detail) {
  if (err == NULL || err_len == 0) return;
  snprintf(err, err_len, fmt, detail ? detail : "");
}

/* ------------------------------------------------------------------ platform */

#ifdef __vita__
static int g_net_ready;
static pthread_mutex_t g_net_lock = PTHREAD_MUTEX_INITIALIZER;
#define SV_NET_POOL (1024 * 1024)

int sv_platform_net_init(char *err, size_t err_len) {
  int ret = 0;
  pthread_mutex_lock(&g_net_lock);
  if (!g_net_ready) {
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if (sceNetShowNetstat() < 0) {
      static void *pool;
      SceNetInitParam param;
      if (pool == NULL) pool = malloc(SV_NET_POOL);
      param.memory = pool;
      param.size = SV_NET_POOL;
      param.flags = 0;
      if (pool == NULL || sceNetInit(&param) < 0) ret = -1;
    }
    if (ret == 0) {
      int r = sceNetCtlInit();
      /* Already-initialized is fine (another component may own it). */
      (void)r;
      g_net_ready = 1;
    }
  }
  pthread_mutex_unlock(&g_net_lock);
  if (ret != 0) set_err(err, err_len, "Network initialization failed%s", NULL);
  return ret;
}

uint64_t sv_now_ns(void) { return (uint64_t)sceKernelGetProcessTimeWide() * 1000u; }
void sv_sleep_ms(unsigned ms) { sceKernelDelayThread(ms * 1000u); }
const char *sv_data_dir(void) { return "ux0:data/surfvita"; }
int sv_mkdir(const char *path) {
  int r = sceIoMkdir(path, 0777);
  return (r >= 0 || r == (int)0x80010011 /* SCE_ERROR_ERRNO_EEXIST */) ? 0 : -1;
}

int sv_read_file(const char *path, char **out, size_t *out_len, size_t max) {
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  char *buf;
  size_t len = 0;
  if (fd < 0) return -1;
  buf = malloc(max + 1);
  if (buf == NULL) { sceIoClose(fd); return -1; }
  for (;;) {
    int n;
    if (len == max) { /* too large */
      free(buf); sceIoClose(fd); return -1;
    }
    n = sceIoRead(fd, buf + len, (SceSize)(max - len));
    if (n < 0) { free(buf); sceIoClose(fd); return -1; }
    if (n == 0) break;
    len += (size_t)n;
  }
  sceIoClose(fd);
  buf[len] = 0;
  *out = buf;
  if (out_len) *out_len = len;
  return 0;
}

int sv_write_file_atomic(const char *path, const void *data, size_t len) {
  char tmp[512];
  SceUID fd;
  size_t done = 0;
  if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return -1;
  fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0600);
  if (fd < 0) return -1;
  while (done < len) {
    int n = sceIoWrite(fd, (const char *)data + done, (SceSize)(len - done));
    if (n <= 0) { sceIoClose(fd); sceIoRemove(tmp); return -1; }
    done += (size_t)n;
  }
  sceIoClose(fd);
  /* sceIoRename does not replace an existing file. The reader falls back to
   * path.tmp if a crash lands between remove and rename. */
  if (sceIoRename(tmp, path) < 0) {
    sceIoRemove(path);
    if (sceIoRename(tmp, path) < 0) return -1;
  }
  return 0;
}
#else
int sv_platform_net_init(char *err, size_t err_len) { (void)err; (void)err_len; return 0; }
uint64_t sv_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}
void sv_sleep_ms(unsigned ms) { usleep(ms * 1000u); }
const char *sv_data_dir(void) {
  const char *v = getenv("SURFVITA_HOME");
  return v && *v ? v : "./surfvita-data";
}
int sv_mkdir(const char *path) { return (mkdir(path, 0700) == 0 || errno == EEXIST) ? 0 : -1; }
int sv_read_file(const char *path, char **out, size_t *out_len, size_t max) {
  FILE *f = fopen(path, "rb");
  char *buf;
  size_t len;
  if (!f) return -1;
  buf = malloc(max + 1);
  if (!buf) { fclose(f); return -1; }
  len = fread(buf, 1, max + 1, f);
  fclose(f);
  if (len > max) { free(buf); return -1; }
  buf[len] = 0;
  *out = buf;
  if (out_len) *out_len = len;
  return 0;
}
int sv_write_file_atomic(const char *path, const void *data, size_t len) {
  char tmp[512];
  FILE *f;
  if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return -1;
  f = fopen(tmp, "wb");
  if (!f) return -1;
  chmod(tmp, 0600);
  if (fwrite(data, 1, len, f) != len || fflush(f) != 0) { fclose(f); remove(tmp); return -1; }
  fsync(fileno(f));
  fclose(f);
  return rename(tmp, path) == 0 ? 0 : -1;
}
#endif

/* -------------------------------------------------------------------- crypto */

static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static int g_crypto_ready;
static pthread_mutex_t g_crypto_lock = PTHREAD_MUTEX_INITIALIZER;

int sv_crypto_init(char *err, size_t err_len) {
  int ret = 0;
  pthread_mutex_lock(&g_crypto_lock);
  if (!g_crypto_ready) {
    static const char pers[] = "surf-vita-session";
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    if (psa_crypto_init() != PSA_SUCCESS) ret = -1; /* required for TLS 1.3 */
    if (ret == 0 && mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                                          (const unsigned char *)pers, sizeof(pers) - 1) != 0)
      ret = -1;
    if (ret == 0) g_crypto_ready = 1;
  }
  pthread_mutex_unlock(&g_crypto_lock);
  if (ret != 0) set_err(err, err_len, "Crypto initialization failed%s", NULL);
  return ret;
}

mbedtls_ctr_drbg_context *sv_drbg(void) { return &g_drbg; }

int sv_random(void *out, size_t len) {
  return mbedtls_ctr_drbg_random(&g_drbg, out, len) == 0 ? 0 : -1;
}

void sv_hex(const uint8_t *in, size_t len, char *out) {
  static const char digits[] = "0123456789abcdef";
  size_t i;
  for (i = 0; i < len; i++) {
    out[i * 2] = digits[in[i] >> 4];
    out[i * 2 + 1] = digits[in[i] & 15];
  }
  out[len * 2] = 0;
}

int sv_b64url(const uint8_t *in, size_t len, char *out, size_t cap) {
  size_t olen = 0, i;
  if (mbedtls_base64_encode((unsigned char *)out, cap, &olen, in, len) != 0) return -1;
  while (olen > 0 && out[olen - 1] == '=') olen--;
  out[olen] = 0;
  for (i = 0; i < olen; i++) {
    if (out[i] == '+') out[i] = '-';
    else if (out[i] == '/') out[i] = '_';
  }
  return (int)olen;
}

/* ---------------------------------------------------------------------- TCP */

static int tcp_connect(const char *host, uint16_t port) {
#ifdef __vita__
  SceNetSockaddrIn addr;
  int fd, one = 1, timeout_us = SV_SOCKET_TIMEOUT_MS * 1000;
  memset(&addr, 0, sizeof(addr));
  addr.sin_len = sizeof(addr);
  addr.sin_family = SCE_NET_AF_INET;
  addr.sin_port = sceNetHtons(port);
  if (sceNetInetPton(SCE_NET_AF_INET, host, &addr.sin_addr) <= 0) return -1;
  fd = sceNetSocket("surf", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (sceNetConnect(fd, (const SceNetSockaddr *)&addr, sizeof(addr)) < 0) {
    sceNetSocketClose(fd);
    return -1;
  }
  sceNetSetsockopt(fd, SCE_NET_IPPROTO_TCP, SCE_NET_TCP_NODELAY, &one, sizeof(one));
  sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &timeout_us, sizeof(timeout_us));
  return fd;
#else
  struct sockaddr_in addr;
  struct timeval tv;
  int fd, one = 1;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) return -1;
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  tv.tv_sec = 0;
  tv.tv_usec = SV_SOCKET_TIMEOUT_MS * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return fd;
#endif
}

static void tcp_close(int fd) {
#ifdef __vita__
  sceNetSocketClose(fd);
#else
  close(fd);
#endif
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
  int fd = *(int *)ctx;
#ifdef __vita__
  int n = sceNetSend(fd, buf, (unsigned int)len, 0);
  if (n >= 0) return n;
  if (n == (int)SCE_NET_ERROR_EAGAIN) return MBEDTLS_ERR_SSL_WANT_WRITE;
  return MBEDTLS_ERR_NET_SEND_FAILED;
#else
  ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
  if (n >= 0) return (int)n;
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_WRITE;
  return MBEDTLS_ERR_NET_SEND_FAILED;
#endif
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
  int fd = *(int *)ctx;
#ifdef __vita__
  int n = sceNetRecv(fd, buf, (unsigned int)len, 0);
  if (n >= 0) return n;
  if (n == (int)SCE_NET_ERROR_EAGAIN || n == (int)SCE_NET_ERROR_ETIMEDOUT)
    return MBEDTLS_ERR_SSL_WANT_READ;
  return MBEDTLS_ERR_NET_RECV_FAILED;
#else
  ssize_t n = recv(fd, buf, len, 0);
  if (n >= 0) return (int)n;
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
  return MBEDTLS_ERR_NET_RECV_FAILED;
#endif
}

/* ---------------------------------------------------------------------- TLS */

int sv_conn_open(sv_conn *c, const char *host, uint16_t port, const uint8_t *pin,
                 char *err, size_t err_len) {
  const mbedtls_x509_crt *peer;
  uint64_t deadline;
  int ret;
  memset(c, 0, sizeof(*c));
  c->fd = -1;
  mbedtls_ssl_init(&c->ssl);
  mbedtls_ssl_config_init(&c->conf);
  if (sv_crypto_init(err, err_len) != 0) goto fail;
  c->fd = tcp_connect(host, port);
  if (c->fd < 0) {
    set_err(err, err_len, "Could not reach the Surf server%s", NULL);
    goto fail;
  }
  if (mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                  MBEDTLS_SSL_PRESET_DEFAULT) != 0)
    goto fail_tls;
  /* No CA: the trust anchor is the SHA-256 leaf fingerprint, checked below
   * before any application byte is sent. Handshake signatures are still
   * verified against the presented certificate's key. */
  mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
  mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, sv_drbg());
  mbedtls_ssl_conf_min_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_2);
#if defined(MBEDTLS_SSL_SESSION_TICKETS)
  mbedtls_ssl_conf_session_tickets(&c->conf, MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
#endif
  if (mbedtls_ssl_setup(&c->ssl, &c->conf) != 0) goto fail_tls;
  mbedtls_ssl_set_hostname(&c->ssl, NULL);
  mbedtls_ssl_set_bio(&c->ssl, &c->fd, bio_send, bio_recv, NULL);
  deadline = sv_now_ns() + (uint64_t)SV_HANDSHAKE_TIMEOUT_MS * 1000000u;
  while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
    if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) goto fail_tls;
    if (sv_now_ns() > deadline) goto fail_tls;
  }
  peer = mbedtls_ssl_get_peer_cert(&c->ssl);
  if (peer == NULL || peer->raw.p == NULL || peer->raw.len == 0) {
    set_err(err, err_len, "Server did not present a certificate%s", NULL);
    goto fail;
  }
  if (mbedtls_sha256(peer->raw.p, peer->raw.len, c->fingerprint, 0) != 0) goto fail_tls;
  if (pin != NULL && memcmp(pin, c->fingerprint, 32) != 0) {
    set_err(err, err_len, "Server Identity Changed%s", NULL);
    goto fail;
  }
  c->ready = 1;
  return 0;
fail_tls:
  set_err(err, err_len, "Secure connection failed%s", NULL);
fail:
  sv_conn_close(c);
  return -1;
}

int sv_conn_write_all(sv_conn *c, const void *data, size_t len) {
  const unsigned char *p = data;
  uint64_t deadline = sv_now_ns() + 10000000000ull;
  if (!c->ready) return -1;
  while (len > 0) {
    int n = mbedtls_ssl_write(&c->ssl, p, len);
    if (n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_WANT_READ) {
      if (sv_now_ns() > deadline) return -1;
      continue;
    }
    if (n <= 0) return -1;
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

int sv_conn_read(sv_conn *c, void *buf, size_t len) {
  int n;
  if (!c->ready) return -1;
  n = mbedtls_ssl_read(&c->ssl, buf, len);
  if (n > 0) return n;
  if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
      || n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
#endif
  )
    return 0;
  return -1; /* 0 (EOF), close notify, or error */
}

void sv_conn_close(sv_conn *c) {
  if (c->ready) mbedtls_ssl_close_notify(&c->ssl);
  c->ready = 0;
  if (c->fd >= 0) tcp_close(c->fd);
  c->fd = -1;
  mbedtls_ssl_free(&c->ssl);
  mbedtls_ssl_config_free(&c->conf);
}

/* --------------------------------------------------------------------- HTTP */

static const char *find_ci(const char *hay, size_t hay_len, const char *needle) {
  size_t n = strlen(needle), i, j;
  if (n == 0 || hay_len < n) return NULL;
  for (i = 0; i + n <= hay_len; i++) {
    for (j = 0; j < n; j++)
      if (tolower((unsigned char)hay[i + j]) != tolower((unsigned char)needle[j])) break;
    if (j == n) return hay + i;
  }
  return NULL;
}

/* Value of header `name` (e.g. "content-length") within the header block. */
static int header_value(const char *headers, size_t len, const char *name, char *out, size_t cap) {
  char pattern[64];
  const char *p = headers, *end = headers + len;
  snprintf(pattern, sizeof(pattern), "\r\n%s:", name);
  while ((p = find_ci(p, (size_t)(end - p), pattern)) != NULL) {
    const char *v = p + strlen(pattern), *e;
    size_t n;
    while (v < end && (*v == ' ' || *v == '\t')) v++;
    e = v;
    while (e < end && *e != '\r') e++;
    n = (size_t)(e - v);
    if (n >= cap) n = cap - 1;
    memcpy(out, v, n);
    out[n] = 0;
    return 0;
  }
  return -1;
}

static int dechunk(char *body, size_t *len) {
  size_t in = 0, out = 0, total = *len;
  for (;;) {
    size_t size = 0;
    int digits = 0;
    while (in < total && isxdigit((unsigned char)body[in])) {
      int c = tolower((unsigned char)body[in++]);
      if (++digits > 7) return -1;
      size = size * 16 + (size_t)(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    if (digits == 0) return -1;
    while (in < total && body[in] != '\n') in++;
    if (in >= total) return -1;
    in++;
    if (size == 0) break;
    if (size > total - in) return -1;
    memmove(body + out, body + in, size);
    out += size;
    in += size;
    if (in + 2 > total || body[in] != '\r' || body[in + 1] != '\n') return -1;
    in += 2;
  }
  body[out] = 0;
  *len = out;
  return 0;
}

int sv_http_request(const char *host, uint16_t port, const uint8_t *pin,
                    const char *method, const char *path, const char *cookie,
                    const char *json_body, sv_http_response *resp,
                    uint8_t observed_fp[32], char *err, size_t err_len) {
  sv_conn conn;
  char head[1536];
  char *buf = NULL;
  size_t cap = SV_HTTP_MAX_BODY + 8192, len = 0, header_len = 0;
  long content_length = -1;
  int chunked = 0, hl;
  uint64_t deadline;
  size_t body_len = json_body ? strlen(json_body) : 0;

  memset(resp, 0, sizeof(*resp));
  if (sv_conn_open(&conn, host, port, pin, err, err_len) != 0) return -1;
  if (observed_fp) memcpy(observed_fp, conn.fingerprint, 32);
  hl = snprintf(head, sizeof(head),
                "%s %s HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: " SV_USER_AGENT
                "\r\nAccept: application/json\r\nConnection: close\r\n",
                method, path, host, (unsigned)port);
  if (hl < 0 || (size_t)hl >= sizeof(head)) goto fail_proto;
  if (cookie && *cookie) {
    int n = snprintf(head + hl, sizeof(head) - (size_t)hl, "Cookie: %s\r\n", cookie);
    if (n < 0 || (size_t)n >= sizeof(head) - (size_t)hl) goto fail_proto;
    hl += n;
  }
  {
    int n = json_body
                ? snprintf(head + hl, sizeof(head) - (size_t)hl,
                           "Content-Type: application/json\r\nContent-Length: %u\r\n\r\n",
                           (unsigned)body_len)
                : snprintf(head + hl, sizeof(head) - (size_t)hl,
                           strcmp(method, "POST") == 0 ? "Content-Length: 0\r\n\r\n" : "\r\n");
    if (n < 0 || (size_t)n >= sizeof(head) - (size_t)hl) goto fail_proto;
    hl += n;
  }
  if (sv_conn_write_all(&conn, head, (size_t)hl) != 0 ||
      (body_len && sv_conn_write_all(&conn, json_body, body_len) != 0)) {
    set_err(err, err_len, "Connection to the server failed%s", NULL);
    goto fail;
  }

  buf = malloc(cap + 1);
  if (!buf) goto fail_proto;
  deadline = sv_now_ns() + (uint64_t)SV_HTTP_TIMEOUT_MS * 1000000u;
  for (;;) {
    int n;
    if (header_len == 0) {
      char *e;
      buf[len] = 0;
      e = strstr(buf, "\r\n\r\n");
      if (e) {
        char value[64];
        header_len = (size_t)(e - buf) + 4;
        if (header_value(buf, header_len, "content-length", value, sizeof(value)) == 0) {
          char *endp;
          content_length = strtol(value, &endp, 10);
          if (content_length < 0 || content_length > (long)SV_HTTP_MAX_BODY) goto fail_proto;
        }
        if (header_value(buf, header_len, "transfer-encoding", value, sizeof(value)) == 0 &&
            find_ci(value, strlen(value), "chunked"))
          chunked = 1;
      }
    }
    if (header_len && !chunked && content_length >= 0 &&
        len - header_len >= (size_t)content_length)
      break;
    if (len >= cap) goto fail_proto;
    n = sv_conn_read(&conn, buf + len, cap - len);
    if (n < 0) break; /* EOF: Connection: close */
    if (n == 0) {
      if (sv_now_ns() > deadline) {
        set_err(err, err_len, "The server did not answer in time%s", NULL);
        goto fail;
      }
      continue;
    }
    len += (size_t)n;
  }
  buf[len] = 0;
  if (header_len == 0 || strncmp(buf, "HTTP/1.", 7) != 0 || len < 12) goto fail_proto;
  resp->status = atoi(buf + 9);
  {
    char cookie_value[600];
    const char *p = buf, *end = buf + header_len;
    /* Several Set-Cookie headers are possible; keep surf_device. */
    while ((p = find_ci(p, (size_t)(end - p), "\r\nset-cookie:")) != NULL) {
      const char *v = p + 13, *e;
      size_t n;
      while (*v == ' ') v++;
      e = v;
      while (e < end && *e != ';' && *e != '\r') e++;
      n = (size_t)(e - v);
      if (n < sizeof(cookie_value) && n < sizeof(resp->cookie) && strncmp(v, "surf_device=", 12) == 0) {
        memcpy(resp->cookie, v, n);
        resp->cookie[n] = 0;
      }
      p = v;
    }
  }
  {
    size_t blen = len - header_len;
    if (!chunked && content_length >= 0 && blen > (size_t)content_length) blen = (size_t)content_length;
    memmove(buf, buf + header_len, blen);
    buf[blen] = 0;
    if (chunked && dechunk(buf, &blen) != 0) goto fail_proto;
    if (!chunked && content_length >= 0 && blen < (size_t)content_length) goto fail_proto;
    resp->body = buf;
    resp->body_len = blen;
  }
  sv_conn_close(&conn);
  return 0;
fail_proto:
  set_err(err, err_len, "Unexpected reply from the server%s", NULL);
fail:
  free(buf);
  sv_conn_close(&conn);
  memset(resp, 0, sizeof(*resp));
  return -1;
}

void sv_http_free(sv_http_response *resp) {
  free(resp->body);
  resp->body = NULL;
}

/* ---------------------------------------------------------------- WebSocket */

#define SV_WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define SV_WS_RX_INITIAL (256u * 1024u)

int sv_ws_connect(sv_ws *ws, const char *host, uint16_t port, const uint8_t *pin,
                  const char *path, char *err, size_t err_len) {
  uint8_t nonce[16], digest[20];
  char key[32], expect[64], got[64], req[2048];
  size_t klen = 0, elen = 0;
  int n;
  uint64_t deadline;
  memset(ws, 0, sizeof(*ws));
  ws->conn.fd = -1;
  if (sv_conn_open(&ws->conn, host, port, pin, err, err_len) != 0) return -1;
  ws->rx_cap = SV_WS_RX_INITIAL;
  ws->rx = malloc(ws->rx_cap);
  if (!ws->rx || sv_random(nonce, sizeof(nonce)) != 0 ||
      mbedtls_base64_encode((unsigned char *)key, sizeof(key), &klen, nonce, sizeof(nonce)) != 0)
    goto fail;
  key[klen] = 0;
  n = snprintf(req, sizeof(req),
               "GET %s HTTP/1.1\r\nHost: %s:%u\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nUser-Agent: " SV_USER_AGENT
               "\r\n\r\n",
               path, host, (unsigned)port, key);
  if (n < 0 || (size_t)n >= sizeof(req) || sv_conn_write_all(&ws->conn, req, (size_t)n) != 0)
    goto fail;
  deadline = sv_now_ns() + (uint64_t)SV_HTTP_TIMEOUT_MS * 1000000u;
  for (;;) {
    char *e;
    int r;
    if (ws->rx_len >= 16384) goto fail;
    r = sv_conn_read(&ws->conn, ws->rx + ws->rx_len, 16384 - ws->rx_len);
    if (r < 0) goto fail;
    if (r == 0) {
      if (sv_now_ns() > deadline) goto fail;
      continue;
    }
    ws->rx_len += (size_t)r;
    ws->rx[ws->rx_len] = 0;
    e = strstr((char *)ws->rx, "\r\n\r\n");
    if (e) {
      size_t hlen = (size_t)(e - (char *)ws->rx) + 4;
      char sha_in[128];
      if (strncmp((char *)ws->rx, "HTTP/1.1 101", 12) != 0) {
        set_err(err, err_len, "The server refused the browser stream%s", NULL);
        goto fail_quiet;
      }
      snprintf(sha_in, sizeof(sha_in), "%s" SV_WS_GUID, key);
      if (mbedtls_sha1((const unsigned char *)sha_in, strlen(sha_in), digest) != 0 ||
          mbedtls_base64_encode((unsigned char *)expect, sizeof(expect), &elen, digest, 20) != 0)
        goto fail;
      expect[elen] = 0;
      if (header_value((char *)ws->rx, hlen, "sec-websocket-accept", got, sizeof(got)) != 0 ||
          strcmp(got, expect) != 0)
        goto fail;
      memmove(ws->rx, ws->rx + hlen, ws->rx_len - hlen);
      ws->rx_len -= hlen;
      return 0;
    }
  }
fail:
  set_err(err, err_len, "Browser stream handshake failed%s", NULL);
fail_quiet:
  sv_ws_close(ws);
  return -1;
}

int sv_ws_send(sv_ws *ws, int opcode, const void *data, size_t len) {
  uint8_t stack[1024];
  uint8_t *frame = stack, mask[4];
  size_t h = 2, i;
  int ret;
  if (len > SV_WS_MAX_MESSAGE) return -1;
  if (len + 14 > sizeof(stack)) {
    frame = malloc(len + 14);
    if (!frame) return -1;
  }
  frame[0] = (uint8_t)(0x80 | (opcode & 0x0f));
  if (len < 126) {
    frame[1] = (uint8_t)(0x80 | len);
  } else if (len <= 0xffff) {
    frame[1] = 0x80 | 126;
    frame[2] = (uint8_t)(len >> 8);
    frame[3] = (uint8_t)len;
    h = 4;
  } else {
    frame[1] = 0x80 | 127;
    for (i = 0; i < 8; i++) frame[2 + i] = (uint8_t)((uint64_t)len >> (56 - 8 * i));
    h = 10;
  }
  if (sv_random(mask, 4) != 0) {
    if (frame != stack) free(frame);
    return -1;
  }
  memcpy(frame + h, mask, 4);
  h += 4;
  for (i = 0; i < len; i++) frame[h + i] = ((const uint8_t *)data)[i] ^ mask[i & 3];
  ret = sv_conn_write_all(&ws->conn, frame, h + len);
  if (frame != stack) free(frame);
  return ret;
}

int sv_ws_poll(sv_ws *ws, int *opcode, const uint8_t **data, size_t *len) {
  for (;;) {
    size_t avail = ws->rx_len, h = 2, plen, i;
    uint8_t *p = ws->rx;
    int fin, op, r;
    if (avail >= 2) {
      fin = p[0] & 0x80;
      op = p[0] & 0x0f;
      if (p[0] & 0x70) return -1;       /* no extensions negotiated */
      if (p[1] & 0x80) return -1;       /* server frames are never masked */
      plen = p[1] & 0x7f;
      if (plen == 126) {
        if (avail < 4) goto need_more;
        plen = ((size_t)p[2] << 8) | p[3];
        h = 4;
      } else if (plen == 127) {
        uint64_t v = 0;
        if (avail < 10) goto need_more;
        for (i = 0; i < 8; i++) v = (v << 8) | p[2 + i];
        if (v > SV_WS_MAX_MESSAGE) return -1;
        plen = (size_t)v;
        h = 10;
      }
      if (plen > SV_WS_MAX_MESSAGE) return -1;
      if (h + plen > ws->rx_cap) {
        uint8_t *grown = realloc(ws->rx, h + plen + 16384);
        if (!grown) return -1;
        ws->rx = grown;
        ws->rx_cap = h + plen + 16384;
        p = ws->rx;
      }
      if (avail < h + plen) goto need_more;
      ws->last_rx_ns = sv_now_ns();
      {
        uint8_t *payload = p + h;
        size_t frame_len = h + plen;
        if (op >= 8) { /* control */
          if (!fin || plen > 125) return -1;
          if (op == SV_WS_PING) {
            uint8_t copy[125];
            memcpy(copy, payload, plen);
            memmove(ws->rx, ws->rx + frame_len, avail - frame_len);
            ws->rx_len -= frame_len;
            if (sv_ws_send(ws, SV_WS_PONG, copy, plen) != 0) return -1;
            continue;
          }
          if (op == SV_WS_CLOSE) {
            sv_ws_send(ws, SV_WS_CLOSE, payload, plen >= 2 ? 2 : 0);
            return -1;
          }
          memmove(ws->rx, ws->rx + frame_len, avail - frame_len);
          ws->rx_len -= frame_len;
          continue; /* pong or reserved control */
        }
        if (op != 0 && op != SV_WS_TEXT && op != SV_WS_BINARY) return -1;
        if (op == 0 && ws->msg_opcode == 0) return -1;
        if (op != 0 && ws->msg_opcode != 0) return -1;
        if (op != 0 && fin) {
          /* Common case: whole message in one frame. Copy out of rx so rx
           * can be compacted immediately. */
          if (plen + 1 > ws->msg_cap) {
            uint8_t *grown = realloc(ws->msg, plen + 1);
            if (!grown) return -1;
            ws->msg = grown;
            ws->msg_cap = plen + 1;
          }
          memcpy(ws->msg, payload, plen);
          ws->msg[plen] = 0;
          memmove(ws->rx, ws->rx + frame_len, avail - frame_len);
          ws->rx_len -= frame_len;
          *opcode = op;
          *data = ws->msg;
          *len = plen;
          return 1;
        }
        if (op != 0) {
          ws->msg_opcode = op;
          ws->msg_len = 0;
        }
        if (ws->msg_len + plen > SV_WS_MAX_MESSAGE) return -1;
        if (ws->msg_len + plen + 1 > ws->msg_cap) {
          size_t want = ws->msg_len + plen + 1;
          uint8_t *grown = realloc(ws->msg, want);
          if (!grown) return -1;
          ws->msg = grown;
          ws->msg_cap = want;
        }
        memcpy(ws->msg + ws->msg_len, payload, plen);
        ws->msg_len += plen;
        memmove(ws->rx, ws->rx + frame_len, avail - frame_len);
        ws->rx_len -= frame_len;
        if (fin) {
          *opcode = ws->msg_opcode;
          ws->msg_opcode = 0;
          ws->msg[ws->msg_len] = 0;
          *data = ws->msg;
          *len = ws->msg_len;
          return 1;
        }
        continue;
      }
    }
  need_more:
    if (ws->rx_len >= ws->rx_cap) return -1;
    r = sv_conn_read(&ws->conn, ws->rx + ws->rx_len, ws->rx_cap - ws->rx_len);
    if (r < 0) return -1;
    if (r == 0) return 0;
    ws->rx_len += (size_t)r;
  }
}

void sv_ws_close(sv_ws *ws) {
  if (ws->conn.ready) {
    static const uint8_t normal[2] = {0x03, 0xe8};
    sv_ws_send(ws, SV_WS_CLOSE, normal, 2);
  }
  sv_conn_close(&ws->conn);
  free(ws->rx);
  free(ws->msg);
  ws->rx = ws->msg = NULL;
  ws->rx_len = ws->rx_cap = ws->msg_len = ws->msg_cap = 0;
  ws->msg_opcode = 0;
}

/* --------------------------------------------------------------------- JSON */

static const char *skip_ws(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  return p;
}

static const char *skip_string(const char *p) {
  if (*p != '"') return NULL;
  p++;
  while (*p && *p != '"') {
    if (*p == '\\') {
      p++;
      if (!*p) return NULL;
    }
    p++;
  }
  return *p == '"' ? p + 1 : NULL;
}

static const char *skip_value(const char *p, int depth) {
  p = skip_ws(p);
  if (depth > 32) return NULL;
  if (*p == '"') return skip_string(p);
  if (*p == '{' || *p == '[') {
    char close = *p == '{' ? '}' : ']';
    p = skip_ws(p + 1);
    if (*p == close) return p + 1;
    for (;;) {
      if (close == '}') {
        p = skip_string(skip_ws(p));
        if (!p) return NULL;
        p = skip_ws(p);
        if (*p != ':') return NULL;
        p++;
      }
      p = skip_value(p, depth + 1);
      if (!p) return NULL;
      p = skip_ws(p);
      if (*p == ',') { p++; continue; }
      if (*p == close) return p + 1;
      return NULL;
    }
  }
  if (!*p) return NULL;
  while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' &&
         *p != '\t')
    p++;
  return p;
}

int sv_json_raw(const char *json, const char *key, const char **start, size_t *len) {
  const char *p;
  size_t klen = strlen(key);
  if (!json) return -1;
  p = skip_ws(json);
  if (*p != '{') return -1;
  p = skip_ws(p + 1);
  if (*p == '}') return -1;
  for (;;) {
    const char *ks = p, *ke, *vs, *ve;
    ke = skip_string(ks);
    if (!ke) return -1;
    p = skip_ws(ke);
    if (*p != ':') return -1;
    vs = skip_ws(p + 1);
    ve = skip_value(vs, 0);
    if (!ve) return -1;
    if ((size_t)(ke - ks) == klen + 2 && memcmp(ks + 1, key, klen) == 0) {
      *start = vs;
      *len = (size_t)(ve - vs);
      return 0;
    }
    p = skip_ws(ve);
    if (*p == ',') { p = skip_ws(p + 1); continue; }
    return -1;
  }
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = (char)tolower((unsigned char)c);
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

int sv_json_string(const char *json, const char *key, char *out, size_t cap) {
  const char *s, *end;
  size_t len, o = 0;
  if (cap == 0 || sv_json_raw(json, key, &s, &len) != 0 || len < 2 || *s != '"') return -1;
  end = s + len - 1;
  s++;
  while (s < end) {
    unsigned cp;
    char c = *s++;
    if (c != '\\') {
      if (o + 1 >= cap) return -1;
      out[o++] = c;
      continue;
    }
    if (s >= end) return -1;
    c = *s++;
    switch (c) {
    case 'n': cp = '\n'; break;
    case 't': cp = '\t'; break;
    case 'r': cp = '\r'; break;
    case 'b': cp = '\b'; break;
    case 'f': cp = '\f'; break;
    case 'u': {
      int i;
      cp = 0;
      if (end - s < 4) return -1;
      for (i = 0; i < 4; i++) {
        int v = hexval(s[i]);
        if (v < 0) return -1;
        cp = cp * 16 + (unsigned)v;
      }
      s += 4;
      if (cp >= 0xd800 && cp <= 0xdfff) cp = '?';
      break;
    }
    default: cp = (unsigned char)c; break;
    }
    if (cp < 0x80) {
      if (o + 1 >= cap) return -1;
      out[o++] = (char)cp;
    } else if (cp < 0x800) {
      if (o + 2 >= cap) return -1;
      out[o++] = (char)(0xc0 | (cp >> 6));
      out[o++] = (char)(0x80 | (cp & 0x3f));
    } else {
      if (o + 3 >= cap) return -1;
      out[o++] = (char)(0xe0 | (cp >> 12));
      out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
      out[o++] = (char)(0x80 | (cp & 0x3f));
    }
  }
  out[o] = 0;
  return 0;
}

int sv_json_int(const char *json, const char *key, long long *out) {
  const char *s;
  size_t len;
  char tmp[32];
  char *endp;
  if (sv_json_raw(json, key, &s, &len) != 0 || len == 0 || len >= sizeof(tmp)) return -1;
  memcpy(tmp, s, len);
  tmp[len] = 0;
  *out = strtoll(tmp, &endp, 10);
  return *endp == 0 ? 0 : -1;
}

int sv_json_bool(const char *json, const char *key, int *out) {
  const char *s;
  size_t len;
  if (sv_json_raw(json, key, &s, &len) != 0) return -1;
  if (len == 4 && memcmp(s, "true", 4) == 0) { *out = 1; return 0; }
  if (len == 5 && memcmp(s, "false", 5) == 0) { *out = 0; return 0; }
  return -1;
}

int sv_json_escape(const char *in, char *out, size_t cap) {
  size_t o = 0;
  for (; *in; in++) {
    unsigned char c = (unsigned char)*in;
    char tmp[8];
    const char *rep = NULL;
    size_t n;
    if (c == '"') rep = "\\\"";
    else if (c == '\\') rep = "\\\\";
    else if (c < 0x20) {
      snprintf(tmp, sizeof(tmp), "\\u%04x", c);
      rep = tmp;
    }
    n = rep ? strlen(rep) : 1;
    if (o + n >= cap) return -1;
    if (rep) memcpy(out + o, rep, n);
    else out[o] = (char)c;
    o += n;
  }
  out[o] = 0;
  return 0;
}
