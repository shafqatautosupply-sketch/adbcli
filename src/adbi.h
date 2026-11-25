#ifndef ADBI_H
#define ADBI_H
#include <stdint.h>
#include <stddef.h>
#include <openssl/ssl.h>
#include <openssl/evp.h>

#define ADBI_VERSION "1.0"

/* ---- keys.c ---- */
/* Loads (or generates on first use) the ADB RSA key.  publine receives the
 * "<base64 android pubkey> user@host" text that gets installed on the device. */
int keys_load(EVP_PKEY **pkey, char *publine, size_t publen);
X509 *keys_make_cert(EVP_PKEY *pk);
const char *adbi_home(void);          /* ~/.adbi */
int adbi_path(char *out, size_t n, const char *name);

/* ---- pairing.c ---- */
int adbi_pair(const char *host, int port, const char *code);

/* ---- mdns.c ---- */
#define MDNS_CONNECT "_adb-tls-connect._tcp"
#define MDNS_PAIRING "_adb-tls-pairing._tcp"
/* Browse for a service.  If list!=0 print every hit.  If want_host!=NULL only
 * accept results whose address equals it.  Returns 1 and fills host/port with
 * the first match, 0 if none, -1 if mDNS support is not compiled in. */
int mdns_find(const char *type, const char *want_host, int timeout_ms, int list,
              char *host, size_t hlen, int *port);

/* ---- conn.c ---- */
typedef struct adb_conn {
    int fd;
    SSL *ssl;
    SSL_CTX *ctx;
    int use_tls;
    uint32_t maxdata;
    int shell_v2;
    char banner[4096];
    uint32_t local, remote;
    int opened, closed, ok_pending;
    uint8_t *rb; size_t rlen, roff, rcap;   /* stream receive buffer */
    uint8_t *pkt;                           /* scratch payload */
} adb_conn;

int tcp_connect(const char *host, int port, int timeout_ms);
int adb_connect(adb_conn *c, const char *host, int port);
void adb_close(adb_conn *c);
int stream_open(adb_conn *c, const char *service);
int stream_write(adb_conn *c, const void *buf, size_t len);
/* wait for incoming packet or for extra_fd; returns 0 ok, -1 error/closed.
 * *extra_ready is set when extra_fd is readable. */
int adb_wait(adb_conn *c, int extra_fd, int *extra_ready);
size_t rb_avail(adb_conn *c);
uint8_t *rb_ptr(adb_conn *c);
void rb_consume(adb_conn *c, size_t n);
int stream_read_exact(adb_conn *c, void *buf, size_t n);
void stream_close(adb_conn *c);

/* helpers */
void put32le(uint8_t *p, uint32_t v);
uint32_t get32le(const uint8_t *p);
#endif
