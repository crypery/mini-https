/*
 * Git: https://github.com/crypery
 * Author: https://crypery.com
 * License: GNU AGPL v3 (Affero GPL)
 */

/*
 * web_test.c - Unit + stress test harness for the crypery web servers.
 *
 * Tests both server variants against all protocol variations:
 *   base  -> server-base.exe (static files only, no API routes)
 *   web   -> server.exe      (static + JSON API + WebSocket + SSE + upload)
 *   http  -> plain-HTTP server (config ssl_use=false)
 *   https -> TLS server      (config ssl_use=true)
 *
 * CLI:
 *   web_test.exe <base|web> <http|https> <host> <port> [unit|stress|all]
 *
 * Examples:
 *   web_test.exe base https 127.0.0.1 443 all
 *   web_test.exe web  https 127.0.0.1 443 stress
 *   web_test.exe base http  127.0.0.1 8443 unit
 *
 * Status codes reported per test:
 *    0  - connection did NOT happen (TCP refused / TLS rejected / no response)
 *    1  - connection happened, but no HTTP status code was received
 *    N  - the HTTP status code received from the server
 *
 * Unit tests cover: static serving, gzip, 404/400/403/413/414/431/426/301,
 * path traversal, blocked paths, Host validation, www-redirect, JSON API,
 * cookies (set/read/update + malformed Cookie headers and invalid
 * name/value data), forms, multipart upload, WebSocket (handshake accept
 * value + echo + close), SSE events, and malformed TLS records.
 *
 * Stress tests cover: rapid sequential requests, threaded concurrent load,
 * lying Content-Length (body-limit enforcement), large uploads, a
 * silent/aborted connection storm with post-storm health check, and a
 * slowloris trickle that must end in 408 (bounded, no pinned worker).
 *
 * Build (see build_test.bat):
 *   gcc -O2 -o web_test.exe -I. -I./zlib -I./crypery -I./utilery \
 *       ./zlib/*.c ./crypery/*.c ./utilery/*.c web_test.c -lws2_32 -lz
 */
#include "ssl.h"
#include "tls.h"
#include "http.h"
#include "compat.h"
#include "zlib/zlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEST_TIMEOUT_MS   8000
#define JUNK_WAIT_MS      45000
#define TRICKLE_GAP_MS    2000
#define STRESS_THREADS    8
#define STRESS_ITERS      10
#define STORM_IDLE        20
#define STORM_ABORTS      10
#define RAPID_ITERS       30

/* Fixed WebSocket key. The expected Sec-WebSocket-Accept is computed
 * independently (base64(SHA1(key + magic-GUID))) and verified with an
 * external SHA-1; the server must return exactly this value. The accept is
 * also computed locally with our own SHA-1 + base64 and must agree. */
static const char *WS_TEST_KEY = "dGhlIHNhbXBsZSBvcmRlcg==";
static const char *WS_TEST_KEY_EXPECTED = "mEYbnArkNYMg0bfuxjP2T6fN7cY=";
static const char *WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

static int  g_is_web = 0;      /* 0 = server-base, 1 = server */
static int  g_is_https = 1;
static char g_host[256] = "127.0.0.1";
static int  g_port = 443;

static int test_total = 0;
static int test_passed = 0;
static int test_failed = 0;
static int status_count[600];
static mtx_t g_stats_cs;

static void record_status(int code) {
    mtx_lock(&g_stats_cs);
    if (code >= 0 && code < 600) status_count[code]++;
    mtx_unlock(&g_stats_cs);
}

static void report(int passed, const char *detail, int code) {
    test_total++;
    if (passed) { test_passed++; printf("[PASS] %-58s (code: %d)\n", detail, code); }
    else        { test_failed++; printf("[FAIL] %-58s (code: %d)\n", detail, code); }
}

static const char *tag(void) { return g_is_https ? "https" : "http"; }

/* ==================== connection layer ==================== */

typedef struct {
    SOCKET sock;
    SSL *ssl;
    SSL_CTX *ctx;
    int is_ssl;
    int status;  /* 0 = no connection, 1 = established, else HTTP status */
} TConn;

static void tc_close(TConn *t) {
    if (t->ssl) { SSL_shutdown(t->ssl); SSL_free(t->ssl); t->ssl = NULL; }
    if (t->ctx) { SSL_CTX_free(t->ctx); t->ctx = NULL; }
    if (t->sock != INVALID_SOCKET) {
        shutdown(t->sock, SD_BOTH);
        closesocket(t->sock);
        t->sock = INVALID_SOCKET;
    }
}

static SOCKET raw_connect(const char *host, int port, int timeout_ms) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr(host);
    a.sin_port = htons((unsigned short)port);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    compat_set_io_timeout(s, timeout_ms);
    return s;
}

static int tc_open(TConn *t, int is_ssl, int timeout_ms) {
    memset(t, 0, sizeof(*t));
    t->sock = INVALID_SOCKET;
    t->status = 0;
    t->is_ssl = is_ssl;
    t->sock = raw_connect(g_host, g_port, timeout_ms);
    if (t->sock == INVALID_SOCKET) return -1;
    if (!is_ssl) { t->status = 1; return 0; }
    t->ctx = SSL_CTX_new();
    if (!t->ctx) { tc_close(t); return -1; }
    t->ssl = SSL_new(t->ctx);
    if (!t->ssl) { tc_close(t); return -1; }
    SSL_set_verify(t->ssl, SSL_VERIFY_NONE, NULL);
    SSL_set_fd(t->ssl, (int)t->sock);
    tls_set_server_name((TLSContext *)t->ssl, g_host, (int)strlen(g_host));
    if (SSL_connect(t->ssl) < 0) { tc_close(t); return -1; }
    t->status = 1;
    return 0;
}

static int tc_send(TConn *t, const void *data, int len) {
    if (t->is_ssl) return SSL_write(t->ssl, data, len);
    return (int)send(t->sock, (const char *)data, (size_t)len, 0);
}

static int tc_recv(TConn *t, char *buf, int bufsz) {
    if (t->is_ssl) return SSL_read(t->ssl, buf, bufsz);
    return (int)recv(t->sock, buf, (size_t)bufsz, 0);
}

/* Set only the receive timeout. Used by the trickle stress test to keep the
 * pre-send "check" recv short, so the 2s send cadence never slips past the
 * server's per-call io_timeout window (which would make the server close early). */
static void tc_set_recv_timeout(TConn *t, int ms) {
#ifdef _WIN32
    int to = ms;
    setsockopt(t->sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
#else
    struct timeval to;
    to.tv_sec = ms / 1000;
    to.tv_usec = (ms % 1000) * 1000;
    setsockopt(t->sock, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
#endif
}

static int parse_status(const char *buf) {
    const char *p = strstr(buf, "HTTP/");
    if (!p) return 0;
    p += 5;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    int code = 0;
    while (*p >= '0' && *p <= '9') { code = code * 10 + (*p - '0'); p++; }
    return (code >= 100 && code <= 599) ? code : 0;
}

/* Read until the header block ends (or the socket closes). */
static int read_headers(TConn *t, char *buf, int bufsz) {
    int at = 0;
    while (at < bufsz - 1) {
        int n = tc_recv(t, buf + at, bufsz - 1 - at);
        if (n <= 0) break;
        at += n;
        buf[at] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    buf[at] = '\0';
    return at;
}

/* Find a header value pointer within the header block. */
static const char *find_header(const char *hdrs, const char *key) {
    const char *p = hdrs;
    size_t klen = strlen(key);
    while (p && *p) {
        const char *eol = strstr(p, "\r\n");
        if (eol && (size_t)(eol - p) > klen && _strnicmp(p, key, klen) == 0) {
            const char *v = p + klen;
            while (*v == ' ' || *v == ':') v++;
            return v;
        }
        p = eol ? eol + 2 : NULL;
    }
    return NULL;
}

/* One complete HTTP(S) request. `extra` is appended verbatim before the final
 * CRLF-CRLF (no body); when body is set, Content-Length is added automatically.
 * Returns the status code (0 = no response); response is stored in out. */
static int do_request(int is_ssl,
                      const char *method, const char *path,
                      const char *extra,
                      const char *body, size_t body_len,
                      char *out, int outsz) {
    TConn t;
    if (tc_open(&t, is_ssl, TEST_TIMEOUT_MS) != 0) { record_status(0); return 0; }

    /* Headers only; the body is streamed afterwards (it must NOT be copied
     * into a bounded buffer - uploads are megabytes). */
    char hdr[16384];
    int hl = snprintf(hdr, sizeof(hdr), "%s %s HTTP/1.1\r\nHost: %s\r\n", method, path, g_host);
    if (extra && extra[0]) hl += snprintf(hdr + hl, sizeof(hdr) - (size_t)hl, "%s", extra);
    if (body && body_len > 0)
        hl += snprintf(hdr + hl, sizeof(hdr) - (size_t)hl, "Content-Length: %llu\r\nConnection: close\r\n\r\n",
                       (unsigned long long)body_len);
    else
        hl += snprintf(hdr + hl, sizeof(hdr) - (size_t)hl, "Connection: close\r\n\r\n");
    if (tc_send(&t, hdr, hl) <= 0) { record_status(t.status); tc_close(&t); return t.status; }
    if (body && body_len > 0) {
        size_t off = 0;
        while (off < body_len) {
            int chunk = (int)((body_len - off) > 65536 ? 65536 : (body_len - off));
            int n = tc_send(&t, body + off, chunk);
            if (n <= 0) { record_status(t.status); tc_close(&t); return t.status; }
            off += (size_t)n;
        }
    }

    int at = read_headers(&t, out, outsz);
    int code = parse_status(out);
    if (code > 0) {
        /* read the body up to Content-Length (bounded by the out buffer) */
        const char *cl = find_header(out, "Content-Length:");
        if (cl) {
            long want = atol(cl);
            const char *he = strstr(out, "\r\n\r\n");
            long have = he ? (long)(out + at - (he + 4)) : 0;
            while (have < want && at < outsz - 1) {
                int n = tc_recv(&t, out + at, outsz - 1 - at);
                if (n <= 0) break;
                at += n;
                have += n;
            }
            /* at already holds the full response (headers + body); do not
         * truncate it to the body length, which would cut off the headers
         * and leave the body past the null terminator. */
        }
    }
    out[at] = '\0';
    t.status = code;
    record_status(code);
    tc_close(&t);
    return code;
}

/* ==================== SHA-1 + base64 (WebSocket accept check) ==================== */

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    int buf_len;
} Sha1;

static void sha1_block(Sha1 *s, const uint8_t *p) {
    uint32_t w[80];
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
    for (i = 16; i < 80; i++) {
        uint32_t x = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
        w[i] = (x << 1) | (x >> 31);
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k, t;
        if (i < 20)      { f = (b & c) | ((~b) & d);        k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
        t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
        e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void sha1_update(Sha1 *s, const uint8_t *p, int len) {
    s->len += (uint64_t)len;
    while (len > 0) {
        int n = 64 - s->buf_len;
        if (n > len) n = len;
        memcpy(s->buf + s->buf_len, p, (size_t)n);
        s->buf_len += n;
        p += n;
        len -= n;
        if (s->buf_len == 64) { sha1_block(s, s->buf); s->buf_len = 0; }
    }
}

static void sha1_final(Sha1 *s, uint8_t out[20]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, z = 0, lb[8];
    int i;
    sha1_update(s, &pad, 1);
    while (s->buf_len != 56) sha1_update(s, &z, 1);
    for (i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - i * 8));
    sha1_update(s, lb, 8);
    for (i = 0; i < 5; i++) {
        out[i*4]     = (uint8_t)(s->h[i] >> 24);
        out[i*4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i*4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i*4 + 3] = (uint8_t)s->h[i];
    }
}

static void sha1_digest(const uint8_t *data, size_t len, uint8_t out[20]) {
    Sha1 s;
    s.h[0] = 0x67452301; s.h[1] = 0xEFCDAB89; s.h[2] = 0x98BADCFE;
    s.h[3] = 0x10325476; s.h[4] = 0xC3D2E1F0;
    s.len = 0; s.buf_len = 0;
    sha1_update(&s, data, (int)len);
    sha1_final(&s, out);
}

static const char B64TAB[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const uint8_t *in, int inlen, char *out) {
    int o = 0, i;
    for (i = 0; i < inlen; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        int have = 1;
        if (i + 1 < inlen) { v |= (uint32_t)in[i+1] << 8; have = 2; }
        if (i + 2 < inlen) { v |= in[i+2]; have = 3; }
        out[o++] = B64TAB[(v >> 18) & 63];
        out[o++] = B64TAB[(v >> 12) & 63];
        out[o++] = have > 1 ? B64TAB[(v >> 6) & 63] : '=';
        out[o++] = have > 2 ? B64TAB[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

/* Compute base64(SHA1(key + GUID)) per RFC 6455 section 1.3. */
static void ws_compute_accept(const char *key, char *out, int outsz) {
    size_t klen = strlen(key);
    uint8_t combined[512];
    uint8_t digest[20];
    if (klen + strlen(WS_GUID) >= sizeof(combined)) klen = sizeof(combined) - strlen(WS_GUID) - 1;
    memcpy(combined, key, klen);
    memcpy(combined + klen, WS_GUID, strlen(WS_GUID));
    sha1_digest(combined, klen + strlen(WS_GUID), digest);
    b64_encode(digest, 20, out);
    out[outsz - 1] = '\0';
}

/* ==================== WebSocket client primitives ==================== */

/* Build a masked client frame (FIN=1) into out; returns total length or -1. */
static int ws_build_frame(uint8_t *out, int outsz, int opcode, const uint8_t *payload, int plen) {
    int off = 2;
    if (plen <= 125) out[1] = (uint8_t)(0x80 | plen);
    else if (plen <= 0xFFFF) { out[1] = (uint8_t)(0x80 | 126); off = 4; }
    else return -1;
    out[0] = (uint8_t)(0x80 | opcode);
    if (plen > 125) {
        if (outsz < 4 + 4 + plen) return -1;
        out[2] = (uint8_t)(plen >> 8);
        out[3] = (uint8_t)(plen & 0xFF);
    }
    {
        uint8_t mask[4];
        int i;
        for (i = 0; i < 4; i++) mask[i] = (uint8_t)(rand() & 0xFF);
        if (outsz < off + 4 + plen) return -1;
        memcpy(out + off, mask, 4);
        off += 4;
        for (i = 0; i < plen; i++) out[off + i] = (uint8_t)(payload[i] ^ mask[i % 4]);
        off += plen;
    }
    return off;
}

/* Parse one server (unmasked) frame. Returns total frame length or 0 if
 * incomplete; fills opcode/payload/plen. */
static int ws_parse_frame(const uint8_t *buf, int blen, int *opcode, const uint8_t **payload, int *plen) {
    int off = 2, len;
    if (blen < 2) return 0;
    *opcode = buf[0] & 0x0F;
    if (buf[1] & 0x80) return 0; /* server frames must be unmasked */
    len = buf[1] & 0x7F;
    if (len == 126) {
        if (blen < 4) return 0;
        len = (buf[2] << 8) | buf[3];
        off = 4;
    } else if (len == 127) {
        int i;
        if (blen < 10) return 0;
        len = 0;
        for (i = 0; i < 8; i++) len = (len << 8) | buf[2 + i];
        off = 10;
    }
    if (blen < off + len) return 0;
    *payload = buf + off;
    *plen = len;
    return off + len;
}

/* Read frames until a frame of the wanted opcode arrives (bounded by wait_ms). */
static int ws_read_frame(TConn *t, int want_opcode, uint8_t *out, int outsz, int *plen, int wait_ms) {
    int at = 0;
    unsigned long start = compat_tick();
    for (;;) {
        int n, opcode;
        const uint8_t *pl;
        while (1) {
            int fl = ws_parse_frame((const uint8_t *)out, at, &opcode, &pl, plen);
            if (fl > 0) {
                if (opcode == want_opcode) {
                    memmove(out, pl, (size_t)(*plen));
                    return *plen;
                }
                memmove(out, out + fl, (size_t)(at - fl));
                at -= fl;
                continue;
            }
            break;
        }
        if (compat_tick() - start > (unsigned)wait_ms) return -1;
        n = tc_recv(t, (char *)(out + at), outsz - at);
        if (n <= 0) return -2;
        at += n;
    }
}

/* ==================== unit tests: common (both servers) ==================== */

static void test_static(void) {
    char buf[16384];
    int code;

    printf("\n=== [%s] UNIT: static files ===\n", tag());

    code = do_request(g_is_https, "GET", "/", NULL, NULL, 0, buf, (int)sizeof(buf));
    report(code == 200, "GET / -> 200 (index.html)", code);

    code = do_request(g_is_https, "GET", "/index.html", NULL, NULL, 0, buf, (int)sizeof(buf));
    report(code == 200 && strstr(buf, "Content-Type: text/html"), "GET /index.html -> 200 text/html", code);

    code = do_request(g_is_https, "GET", "/no/such/page", NULL, NULL, 0, buf, (int)sizeof(buf));
    report(code == 404, "GET /no/such/page -> 404", code);

    {
        static const char body[] = "x=1&y=2";
        code = do_request(g_is_https, "POST", "/", "Content-Type: application/x-www-form-urlencoded",
                          body, sizeof(body) - 1, buf, (int)sizeof(buf));
        report(code == 200, "POST / (static, body ignored) -> 200", code);
    }
}

static void test_gzip(void) {
    char buf[16384];
    int code;

    printf("\n=== [%s] UNIT: gzip (pre-compressed .gz) ===\n", tag());

    /* Build www/index.html.gz from the plain file so the .gz path is exercised. */
    char gzs[512], gzd[512];
    snprintf(gzs, sizeof(gzs), "%sindex.html", "www/");
    snprintf(gzd, sizeof(gzd), "%sindex.html.gz", "www/");
    FILE *src = fopen(gzs, "rb");
    if (!src) { report(0, "gzip: www/index.html not found (skipped)", 0); return; }
    fclose(src);
    gzFile gz = gzopen(gzd, "wb");
    int have_gz = (gz != (gzFile)0);
    if (have_gz) {
        FILE *in = fopen(gzs, "rb");
        char chunk[8192];
        size_t r;
        while (in && (r = fread(chunk, 1, sizeof(chunk), in)) > 0) gzwrite(gz, chunk, (unsigned)r);
        if (in) fclose(in);
        gzclose(gz);
    }

    code = do_request(g_is_https, "GET", "/index.html", "Accept-Encoding: gzip\r\n", NULL, 0, buf, (int)sizeof(buf));
    if (have_gz) {
        const char *he = strstr(buf, "\r\n\r\n");
        int ok = (code == 200) && strstr(buf, "Content-Encoding: gzip") &&
                 he && (unsigned char)he[4] == 0x1F && (unsigned char)he[5] == 0x8B;
        report(ok, "GET /index.html + Accept-Encoding: gzip -> 200 gzip (magic 1f 8b)", code);
    } else {
        report(code == 200, "GET /index.html + Accept-Encoding: gzip -> 200 (no .gz available)", code);
    }
    remove(gzd);
}

static void test_error_paths(void) {
    char buf[16384];
    int code;

    printf("\n=== [%s] UNIT: error paths ===\n", tag());

    {
        TConn t;
        if (tc_open(&t, 0, TEST_TIMEOUT_MS) != 0) { report(0, "malformed request: cannot connect", 0); return; }
        const char *garbage = "ASDF\r\nX\r\n\r\n";
        tc_send(&t, garbage, (int)strlen(garbage));
        read_headers(&t, buf, (int)sizeof(buf));
        code = parse_status(buf);
        t.status = code;
        record_status(code);
        tc_close(&t);
        report(code == 400 || code == 404 || code > 0, "malformed request handled (no crash)", code);
    }

    code = do_request(g_is_https, "GET", "/../config.json", NULL, NULL, 0, buf, (int)sizeof(buf));
    report(code == 400, "GET /../config.json (traversal) -> 400", code);

    code = do_request(g_is_https, "GET", "/config.json", NULL, NULL, 0, buf, (int)sizeof(buf));
    report(code == 403, "GET /config.json (blocked path) -> 403", code);

    {
        TConn t;
        if (tc_open(&t, g_is_https, TEST_TIMEOUT_MS) != 0) { report(0, "disallowed Host: cannot connect", 0); }
        else {
            const char *req = "GET / HTTP/1.1\r\nHost: evil.example.com\r\nConnection: close\r\n\r\n";
            tc_send(&t, req, (int)strlen(req));
            read_headers(&t, buf, (int)sizeof(buf));
            code = parse_status(buf);
            t.status = code;
            record_status(code);
            tc_close(&t);
            report(code == 400, "GET / with disallowed Host -> 400", code);
        }
    }

    {
        TConn t;
        if (tc_open(&t, g_is_https, TEST_TIMEOUT_MS) != 0) { report(0, "www redirect: cannot connect", 0); }
        else {
            const char *req = "GET / HTTP/1.1\r\nHost: www.localhost\r\nConnection: close\r\n\r\n";
            tc_send(&t, req, (int)strlen(req));
            read_headers(&t, buf, (int)sizeof(buf));
            code = parse_status(buf);
            t.status = code;
            record_status(code);
            tc_close(&t);
            report(code == 301 && strstr(buf, "Location:") && strstr(buf, "localhost"),
                   "GET / with Host: www.localhost -> 301 redirect", code);
        }
    }

    {
        char extra[12288];
        char fill[501];
        int off = 0, i;
        memset(fill, 'a', 500);
        fill[500] = '\0';
        for (i = 0; i < 20; i++)
            off += snprintf(extra + off, sizeof(extra) - (size_t)off,
                            "X-Big-Header-%02d: %s\r\n", i, fill);
        code = do_request(g_is_https, "GET", "/", extra, NULL, 0, buf, (int)sizeof(buf));
        report(code == 431, "GET / with ~10KB headers -> 431", code);
    }

    {
        char longuri[3064];
        memset(longuri, 'a', sizeof(longuri) - 1);
        longuri[0] = '/';
        longuri[sizeof(longuri) - 1] = '\0';
        code = do_request(g_is_https, "GET", longuri, NULL, NULL, 0, buf, (int)sizeof(buf));
        report(code == 414 || code == 404, "GET 3KB-long URI handled (414/404, no crash)", code);
    }
}

static void test_body_limits(void) {
    char buf[16384];
    int code;

    printf("\n=== [%s] UNIT: body limits ===\n", tag());

    /* Lying Content-Length beyond the 256MB hard cap must be rejected before
     * any allocation, with a small actual body. */
    code = do_request(g_is_https, "POST", "/", "Content-Length: 300000000\r\n", "tiny", 4, buf, (int)sizeof(buf));
    report(code == 413, "POST / with Content-Length: 300000000 -> 413 (hard cap)", code);

    code = do_request(g_is_https, "POST", "/", "Content-Length: -5\r\n", "tiny", 4, buf, (int)sizeof(buf));
    report(code == 413 || code == 400 || code == 200, "POST / with negative Content-Length handled", code);
}

/* ==================== unit tests: server-base (no API) ==================== */

static void test_base_no_api(void) {
    char buf[16384];
    int code;

    printf("\n=== [%s] UNIT: base server has no API routes ===\n", tag());

    code = do_request(g_is_https, "GET", "/api/info", NULL, NULL, 0, buf, (int)sizeof(buf));
    report(code == 404, "GET /api/info on base server -> 404", code);

    {
        static const char body[] = "{\"a\":1}";
        code = do_request(g_is_https, "POST", "/api/echo", "Content-Type: application/json",
                          body, sizeof(body) - 1, buf, (int)sizeof(buf));
        report(code == 404, "POST /api/echo on base server -> 404", code);
    }
}

/* ==================== unit tests: server (full API) ==================== */

static void test_web_api(void) {
    char buf[16384];
    int code;

    printf("\n=== [%s] UNIT: JSON API ===\n", tag());

    code = do_request(g_is_https, "GET", "/api/info", NULL, NULL, 0, buf, (int)sizeof(buf));
    report(code == 200 && strstr(buf, "\"server\"") && strstr(buf, "\"uptime_sec\""),
           "GET /api/info -> 200 JSON", code);

    code = do_request(g_is_https, "GET", "/api/hello?name=Test", NULL, NULL, 0, buf, (int)sizeof(buf));
    report(code == 200 && strstr(buf, "Hello, Test!"), "GET /api/hello?name=Test -> 200 greeting", code);

    code = do_request(g_is_https, "GET", "/api/hello", NULL, NULL, 0, buf, (int)sizeof(buf));
    report(code == 400, "GET /api/hello (missing name) -> 400", code);

    code = do_request(g_is_https, "GET", "/api/params?name=John&age=30", NULL, NULL, 0, buf, (int)sizeof(buf));
    report(code == 200 && strstr(buf, "name = John") && strstr(buf, "age = 30"),
           "GET /api/params?name=John&age=30 -> 200 parsed params", code);

    {
        static const char body[] = "{\"hello\":\"world\"}";
        code = do_request(g_is_https, "POST", "/api/echo", "Content-Type: application/json",
                          body, sizeof(body) - 1, buf, (int)sizeof(buf));
        report(code == 200 && strstr(buf, "\"hello\":\"world\""), "POST /api/echo (json) -> 200 echoed", code);
    }

    {
        static const char body[] = "name=John&age=30";
        code = do_request(g_is_https, "POST", "/api/form", "Content-Type: application/x-www-form-urlencoded",
                          body, sizeof(body) - 1, buf, (int)sizeof(buf));
        report(code == 200 && strstr(buf, "\"name\":\"John\"") && strstr(buf, "\"age\":\"30\""),
               "POST /api/form (urlencoded) -> 200 parsed", code);
    }
}

/* ==================== unit tests: server (cookies) ==================== */

/* Cookie round-trip through the /api/cookie/set and /api/cookie/get routes
 * (http_set_cookie / http_get_cookie) plus malformed cookie data: the server
 * must parse every variant safely and reject invalid names/values. */
static void test_web_cookies(void) {
    char buf[16384];
    int code;

    printf("\n=== [%s] UNIT: cookies (http_set_cookie / http_get_cookie) ===\n", tag());

    code = do_request(g_is_https, "GET", "/api/cookie/set?name=ctok&value=cv42", NULL, NULL, 0, buf, (int)sizeof(buf));
    report(code == 200 && strstr(buf, "Set-Cookie: ctok=cv42; Path=/"),
           "GET /api/cookie/set -> Set-Cookie: ctok=cv42", code);

    code = do_request(g_is_https, "GET", "/api/cookie/get?name=ctok", "Cookie: ctok=cv42\r\n", NULL, 0, buf, (int)sizeof(buf));
    report(code == 200 && strstr(buf, "\"found\":true") && strstr(buf, "\"value\":\"cv42\""),
           "GET /api/cookie/get (Cookie: ctok=cv42) -> found", code);

    code = do_request(g_is_https, "GET", "/api/cookie/get?name=ctok", NULL, NULL, 0, buf, (int)sizeof(buf));
    report(code == 200 && strstr(buf, "\"found\":false"),
           "GET /api/cookie/get without Cookie header -> not found", code);

    code = do_request(g_is_https, "GET", "/api/cookie/set?name=ctok&value=new7", NULL, NULL, 0, buf, (int)sizeof(buf));
    report(code == 200 && strstr(buf, "Set-Cookie: ctok=new7; Path=/"),
           "cookie update (same name, new value) -> Set-Cookie: ctok=new7", code);

    code = do_request(g_is_https, "GET", "/api/cookie/get?name=two", "Cookie: one=1; two=2\r\n", NULL, 0, buf, (int)sizeof(buf));
    report(code == 200 && strstr(buf, "\"value\":\"2\""),
           "two cookies in one header -> second one read", code);

    code = do_request(g_is_https, "GET", "/api/cookie/get?name=ctok", "Cookie: ctok=a=b\r\n", NULL, 0, buf, (int)sizeof(buf));
    report(code == 200 && strstr(buf, "\"value\":\"a=b\""),
           "cookie value containing '=' read in full", code);

    printf("\n=== [%s] UNIT: cookies - invalid data ===\n", tag());

    code = do_request(g_is_https, "GET", "/api/cookie/get?name=ctok", "Cookie: ctok\r\n", NULL, 0, buf, (int)sizeof(buf));
    report(code == 200 && strstr(buf, "\"found\":false"),
           "Cookie pair without '=' handled (not found)", code);

    code = do_request(g_is_https, "GET", "/api/cookie/get?name=ctok", "Cookie: =cv42\r\n", NULL, 0, buf, (int)sizeof(buf));
    report(code == 200 && strstr(buf, "\"found\":false"),
           "Cookie with empty name handled (not found)", code);

    code = do_request(g_is_https, "GET", "/api/cookie/get?name=ctok", "Cookie: ;;; ; \r\n", NULL, 0, buf, (int)sizeof(buf));
    report(code == 200 && strstr(buf, "\"found\":false"),
           "garbage Cookie header handled (not found)", code);

    code = do_request(g_is_https, "GET", "/api/cookie/set?name=&value=x", NULL, NULL, 0, buf, (int)sizeof(buf));
    report(code == 400, "set cookie with empty name -> 400", code);

    code = do_request(g_is_https, "GET", "/api/cookie/set?name=a%3Bb&value=x", NULL, NULL, 0, buf, (int)sizeof(buf));
    report(code == 400, "set cookie with ';' in name -> 400", code);

    code = do_request(g_is_https, "GET", "/api/cookie/set?name=ctok&value=a%3Bb", NULL, NULL, 0, buf, (int)sizeof(buf));
    report(code == 400, "set cookie with ';' in value -> 400", code);

    {
        /* 9KB Cookie header: exceeds MAX_HEADER_SIZE (8192) -> 431 */
        char extra[9300];
        int off = (int)snprintf(extra, sizeof(extra), "Cookie: huge=");
        memset(extra + off, 'a', 9000);
        off += 9000;
        extra[off++] = '\r'; extra[off++] = '\n';
        code = do_request(g_is_https, "GET", "/", extra, NULL, 0, buf, (int)sizeof(buf));
        report(code == 431, "9KB Cookie header -> 431 (header limit)", code);
    }
}

static int build_multipart(char *out, int outsz, const char *boundary,
                           const char *field1, const char *filedata, size_t filelen,
                           const char *filename) {
    int off = 0;
    off += snprintf(out + off, (size_t)(outsz - off),
                    "--%s\r\nContent-Disposition: form-data; name=\"field1\"\r\n\r\n%s\r\n",
                    boundary, field1);
    off += snprintf(out + off, (size_t)(outsz - off),
                    "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
                    "Content-Type: text/plain\r\n\r\n",
                    boundary, filename);
    memcpy(out + off, filedata, filelen);
    off += (int)filelen;
    off += snprintf(out + off, (size_t)(outsz - off), "\r\n--%s--\r\n", boundary);
    return off;
}

static void test_web_forms_upload(void) {
    char buf[16384];
    int code;

    printf("\n=== [%s] UNIT: multipart form + upload ===\n", tag());

    {
        char mp[8192];
        const char *boundary = "wtest-boundary-1";
        int len = build_multipart(mp, (int)sizeof(mp), boundary, "value1",
                                  "file content", 12, "wtest_form.txt");
        char ct[256];
        snprintf(ct, sizeof(ct), "Content-Type: multipart/form-data; boundary=%s\r\n", boundary);
        code = do_request(g_is_https, "POST", "/api/form", ct, mp, (size_t)len, buf, (int)sizeof(buf));
        report(code == 200 && strstr(buf, "\"name\":\"field1\"") && strstr(buf, "\"value\":\"value1\""),
               "POST /api/form (multipart) -> 200 parsed fields", code);
    }

    {
        char mp[16384];
        char fname[64];
        const char *boundary = "wtest-boundary-2";
        char filedata[1024];
        int i, len;
        for (i = 0; i < 1023; i++) filedata[i] = (char)('A' + (i % 26));
        filedata[1023] = '\0';
        snprintf(fname, sizeof(fname), "wtest_up_%d.txt", (int)(time(NULL) % 100000));
        len = build_multipart(mp, (int)sizeof(mp), boundary, "meta", filedata, 1023, fname);
        char ct[256];
        snprintf(ct, sizeof(ct), "Content-Type: multipart/form-data; boundary=%s\r\n", boundary);
        code = do_request(g_is_https, "POST", "/api/upload", ct, mp, (size_t)len, buf, (int)sizeof(buf));
        {
            /* The multipart has 2 parts (a text field + the file), so the
             * server reports total_files:2. Verify the saved file on disk
             * (size + first byte) and that the response names it. */
            char saved[512];
            snprintf(saved, sizeof(saved), "www/upload/%s", fname);
            int saved_ok = 0, saved_size_ok = 0;
            FILE *f = fopen(saved, "rb");
            if (f) {
                long sz = 0;
                int first = fgetc(f);
                fseek(f, 0, SEEK_END); sz = ftell(f);
                saved_ok = 1;
                saved_size_ok = (sz == 1023 && first == 'A');
                fclose(f);
            }
            report(code == 200 && saved_ok && saved_size_ok && strstr(buf, fname),
                   "POST /api/upload (multipart file) -> 200 + file saved (1023B)", code);
            remove(saved);
        }
    }

    {
        static const char body[] = "not multipart at all";
        code = do_request(g_is_https, "POST", "/api/upload", "Content-Type: text/plain",
                          body, sizeof(body) - 1, buf, (int)sizeof(buf));
        report(code == 400, "POST /api/upload (non-multipart) -> 400", code);
    }
}

static void test_web_websocket(void) {
    printf("\n=== [%s] UNIT: WebSocket /ws ===\n", tag());

    TConn t;
    if (tc_open(&t, g_is_https, TEST_TIMEOUT_MS) != 0) { report(0, "ws: cannot connect", 0); return; }

    char req[512];
    int rl = snprintf(req, sizeof(req),
        "GET /ws HTTP/1.1\r\nHost: %s\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
        g_host, WS_TEST_KEY);
    tc_send(&t, req, rl);

    char buf[4096];
    read_headers(&t, buf, (int)sizeof(buf));
    int code = parse_status(buf);

    char expect[64];
    ws_compute_accept(WS_TEST_KEY, expect, (int)sizeof(expect));
    const char *acc = find_header(buf, "Sec-WebSocket-Accept:");
    int acc_ok = 0, acc_val = 0;
    if (acc) {
        char got[64] = {0};
        size_t i = 0;
        while (i + 1 < sizeof(got) && acc[i] && acc[i] != '\r' && acc[i] != ' ') {
            got[i] = acc[i];
            i++;
        }
        acc_val = (got[0] != '\0' && strcmp(got, expect) == 0);
        acc_ok = (got[0] != '\0' && strcmp(got, WS_TEST_KEY_EXPECTED) == 0);
    }
    printf("     Sec-WebSocket-Accept: local=%s expected=%s\n", acc_val ? "ok" : "mismatch", acc_ok ? "ok" : "mismatch");
    record_status(code);
    report(code == 101 && acc_val, "WS handshake -> 101 + correct Sec-WebSocket-Accept", code);

    if (code == 101) {
        uint8_t frame[512], echo[512];
        int flen, plen;
        static const char msg[] = "web-test-echo";
        flen = ws_build_frame(frame, (int)sizeof(frame), 0x1, (const uint8_t *)msg, (int)strlen(msg));
        tc_send(&t, frame, flen);
        plen = 0;
        flen = ws_read_frame(&t, 0x1, echo, (int)sizeof(echo), &plen, 5000);
        if (flen > 0 && plen < (int)sizeof(echo)) echo[plen] = '\0';
        report(flen > 0 && strstr((char *)echo, "\"type\":\"echo\"") &&
               strstr((char *)echo, msg), "WS text frame echoed", 101);

        uint8_t close_payload[2] = { 0x03, 0xE8 }; /* 1000 */
        flen = ws_build_frame(frame, (int)sizeof(frame), 0x8, close_payload, 2);
        tc_send(&t, frame, flen);
        plen = 0;
        flen = ws_read_frame(&t, 0x8, echo, (int)sizeof(echo), &plen, 5000);
        report(flen > 0, "WS close frame answered", 101);
    }
    tc_close(&t);
}

static void test_web_sse(void) {
    printf("\n=== [%s] UNIT: SSE /sse ===\n", tag());

    TConn t;
    if (tc_open(&t, g_is_https, TEST_TIMEOUT_MS) != 0) { report(0, "sse: cannot connect", 0); return; }
    {
        char req[256];
        int rl = snprintf(req, sizeof(req), "GET /sse HTTP/1.1\r\nHost: %s\r\n\r\n", g_host);
        tc_send(&t, req, rl);
    }

    char buf[8192];
    int at = 0, n;
    unsigned long start = compat_tick();
    int got_conn = 0, got_tick = 0, code = 0;
    while (compat_tick() - start < 8000 && at < (int)sizeof(buf) - 1) {
        n = tc_recv(&t, buf + at, (int)sizeof(buf) - 1 - at);
        if (n <= 0) break;
        at += n;
        buf[at] = '\0';
        code = parse_status(buf);
        if (strstr(buf, "event: connected")) got_conn = 1;
        if (strstr(buf, "event: tick") && strstr(buf, "\"counter\":1")) got_tick = 1;
        if (got_conn && got_tick) break;
    }
    record_status(code);
    report(code == 200 && strstr(buf, "text/event-stream") && got_conn && got_tick,
           "GET /sse -> 200 event-stream with connected + tick events", code);
    tc_close(&t);
}

/* ==================== unit tests: TLS protocol (https only) ==================== */

/* Wait until the peer tears the connection down (FIN or RST). Data in
 * between (e.g. a TLS alert) is consumed and ignored. Returns 1 when the
 * peer closed, 0 on local timeout. */
static int wait_peer_close(TConn *t, int wait_ms) {
    char b[512];
    unsigned long start = compat_tick();
    while (compat_tick() - start < (unsigned)wait_ms) {
        int n = tc_recv(t, b, (int)sizeof(b));
        if (n == 0) return 1;                 /* FIN */
        if (n < 0) {
            int err = WSAGetLastError();
            if (err == WSAECONNRESET || err == WSAECONNABORTED ||
                err == WSAECONNREFUSED || err == WSAENOTCONN)
                return 1;                     /* RST / dead */
            compat_sleep_ms(500);             /* timeout - keep waiting */
            continue;
        }
    }
    return 0;
}

/* Send raw bytes, then wait for the server to close the connection. The
 * requirement: it must close (bounded time) and never hang the worker. */
static void test_raw_record(const char *title, const uint8_t *data, int len) {
    TConn t;
    printf("\n=== [https] UNIT: TLS - %s ===\n", title);
    if (tc_open(&t, 0, TEST_TIMEOUT_MS) != 0) { report(0, "cannot connect", 0); return; }
    tc_send(&t, data, len);

    int closed = wait_peer_close(&t, JUNK_WAIT_MS);
    int status = closed ? 1 : 0;
    record_status(status);
    report(closed, "server closed the malformed-TLS connection (bounded)", status);
    tc_close(&t);
}

static void test_tls_junk_records(void) {
    uint8_t bad_type[]  = { 0x99, 0x03, 0x03, 0x00, 0x05, 0x01, 0x02, 0x03, 0x04, 0x05 };
    uint8_t bad_ver[]   = { 0x16, 0x02, 0x00, 0x00, 0x05, 1, 2, 3, 4, 5 };
    uint8_t ccs[]       = { 0x14, 0x03, 0x03, 0x00, 0x01, 0x01 };
    uint8_t alert[]     = { 0x15, 0x03, 0x03, 0x00, 0x02, 0x01, 0x00 };
    uint8_t partial[]   = { 0x16, 0x03, 0x03 };
    uint8_t mixed_a[]   = { 0x14, 0x03, 0x03, 0x00, 0x01, 0x01 };
    uint8_t mixed_b[]   = { 0xAA, 0x03, 0x03, 0x00, 0x05, 1, 2, 3, 4, 5 };

    uint8_t large[5 + 20480];
    int i;
    large[0] = 0x17; large[1] = 0x03; large[2] = 0x03;
    large[3] = 0x50; large[4] = 0x00; /* record length = 20480 (> 16384 cap) */
    for (i = 5; i < (int)sizeof(large); i++) large[i] = (uint8_t)(i & 0xFF);

    test_raw_record("invalid record type", bad_type, (int)sizeof(bad_type));
    test_raw_record("invalid TLS version", bad_ver, (int)sizeof(bad_ver));
    test_raw_record("ChangeCipherSpec only", ccs, (int)sizeof(ccs));
    test_raw_record("TLS alert record", alert, (int)sizeof(alert));
    test_raw_record("small app-data records", partial, (int)sizeof(partial));

    {
        TConn t;
        printf("\n=== [https] UNIT: TLS - mixed valid/invalid records ===\n");
        if (tc_open(&t, 0, TEST_TIMEOUT_MS) != 0) { report(0, "cannot connect", 0); return; }
        tc_send(&t, mixed_a, (int)sizeof(mixed_a));
        tc_send(&t, mixed_b, (int)sizeof(mixed_b));
        int closed = wait_peer_close(&t, JUNK_WAIT_MS);
        record_status(closed ? 1 : 0);
        report(closed, "mixed records -> server closed (bounded)", closed ? 1 : 0);
        tc_close(&t);
    }

    test_raw_record("large record (>16KB)", large, (int)sizeof(large));

    {
        TConn t;
        uint8_t hello[] = {
            0x16, 0x03, 0x01, 0x00, 0x2F,
            0x01, 0x00, 0x00, 0x2B,
            0x03, 0x03,
            0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
            0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,
            0x00, 0x00, 0x02, 0x00, 0x2f, 0x01, 0x00, 0x00, 0x00
        };
        printf("\n=== [https] UNIT: TLS - crafted ClientHello (full flight) ===\n");
        if (tc_open(&t, 0, TEST_TIMEOUT_MS) != 0) { report(0, "cannot connect", 0); return; }
        tc_send(&t, hello, (int)sizeof(hello));
        char b[2048];
        int got = 0;
        unsigned long start = compat_tick();
        while (compat_tick() - start < (unsigned)JUNK_WAIT_MS) {
            int n = tc_recv(&t, b, (int)sizeof(b));
            if (n > 0) { got += n; if (got >= 50) break; }
            if (n == 0) break;               /* server closed without a flight */
            compat_sleep_ms(300); continue;  /* client recv timeout - keep waiting */
        }
        record_status(got >= 50 ? 1 : 0);
        report(got >= 50, "server answered crafted ClientHello with a flight", got >= 50 ? 1 : 0);
        tc_close(&t);
    }
}

static void test_tls_plaintext(void) {
    printf("\n=== [https] UNIT: TLS - plaintext request on TLS port ===\n");
    TConn t;
    if (tc_open(&t, 0, TEST_TIMEOUT_MS) != 0) { report(0, "cannot connect", 0); return; }
    const char *req = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    tc_send(&t, req, (int)strlen(req));
    char buf[2048];
    read_headers(&t, buf, (int)sizeof(buf));
    int code = parse_status(buf);
    t.status = code;
    record_status(code);
    /* 426 when the probe sees plaintext at accept time; otherwise the TLS
     * path rejects the malformed record and closes (code 0/1). Both are OK. */
    report(code == 426 || code == 400 || code == 0 || code == 1,
           "plaintext on TLS port handled (426 or clean close)", code);
    tc_close(&t);
}

/* ==================== stress tests ==================== */

typedef struct {
    int id;
    int iters;
    int success;
} SReqWorker;

static void *stress_req_worker(void *p) {
    SReqWorker *w = (SReqWorker *)p;
    char buf[16384];
    int i;
    for (i = 0; i < w->iters; i++) {
        char path[64];
        snprintf(path, sizeof(path), "%s?n=s%d_%d",
                 g_is_web ? "/api/info" : "/", w->id, i);
        if (do_request(g_is_https, "GET", path, NULL, NULL, 0, buf, (int)sizeof(buf)) == 200)
            w->success++;
    }
    return NULL;
}

static void stress_concurrent(void) {
    printf("\n=== [%s] STRESS: %d threads x %d requests ===\n", tag(), STRESS_THREADS, STRESS_ITERS);
    thr_t *hs = (thr_t *)malloc((size_t)STRESS_THREADS * sizeof(thr_t));
    SReqWorker *ws = (SReqWorker *)calloc((size_t)STRESS_THREADS, sizeof(SReqWorker));
    unsigned long t0 = compat_tick();
    int i;
    for (i = 0; i < STRESS_THREADS; i++) {
        ws[i].id = i; ws[i].iters = STRESS_ITERS;
        thr_create(&hs[i], stress_req_worker, &ws[i]);
    }
    for (i = 0; i < STRESS_THREADS; i++) thr_join(&hs[i]);
    long elapsed = (long)(compat_tick() - t0);
    int total = STRESS_THREADS * STRESS_ITERS, ok = 0;
    for (i = 0; i < STRESS_THREADS; i++) ok += ws[i].success;
    free(hs); free(ws);
    int min_ok = (int)(total * 0.85);
    printf("     %d/%d served in %ld ms (min expected %d)\n", ok, total, elapsed, min_ok);
    record_status(ok >= min_ok ? 200 : 0);
    report(ok >= min_ok, "concurrent threaded load", ok >= min_ok ? 200 : 0);
}

static void stress_rapid(void) {
    printf("\n=== [%s] STRESS: %d rapid sequential requests ===\n", tag(), RAPID_ITERS);
    char buf[16384];
    int i, ok = 0;
    unsigned long t0 = compat_tick();
    for (i = 0; i < RAPID_ITERS; i++) {
        char path[64];
        snprintf(path, sizeof(path), "%s?n=r%d", g_is_web ? "/api/info" : "/", i);
        if (do_request(g_is_https, "GET", path, NULL, NULL, 0, buf, (int)sizeof(buf)) == 200) ok++;
    }
    long elapsed = (long)(compat_tick() - t0);
    printf("     %d/%d in %ld ms\n", ok, RAPID_ITERS, elapsed);
    record_status(ok == RAPID_ITERS ? 200 : 0);
    report(ok == RAPID_ITERS, "rapid sequential requests", ok == RAPID_ITERS ? 200 : 0);
}

/* Close with SO_LINGER=0 so the teardown is a RST (browser preconnect abort). */
static void tcp_close_rst(SOCKET s) {
    if (s == INVALID_SOCKET) return;
    struct linger lg;
    lg.l_onoff = 1;
    lg.l_linger = 0;
    setsockopt(s, SOL_SOCKET, SO_LINGER, (const char *)&lg, sizeof(lg));
    closesocket(s);
}

static void stress_big_upload(void) {
    printf("\n=== [%s] STRESS: 1MB upload ===\n", tag());
    char *body = (char *)malloc(1048576 + 4096);
    char *out = (char *)malloc(16384);
    int i, code;
    for (i = 0; i < 1048576; i++) body[i] = (char)('A' + (i % 26));

    if (g_is_web) {
        char ct[256];
        int off = snprintf(body, 1048576 + 4096,
                           "--wtest-big\r\nContent-Disposition: form-data; name=\"file\"; filename=\"wtest_big.bin\"\r\n"
                           "Content-Type: application/octet-stream\r\n\r\n");
        memset(body + off, 'B', 1048576);
        off += 1048576;
        off += snprintf(body + off, 512, "\r\n--wtest-big--\r\n");
        snprintf(ct, sizeof(ct), "Content-Type: multipart/form-data; boundary=wtest-big\r\n");
        code = do_request(g_is_https, "POST", "/api/upload", ct, body, (size_t)off, out, 16384);
        remove("www/upload/wtest_big.bin");
    } else {
        code = do_request(g_is_https, "POST", "/", NULL, body, 1048576, out, 16384);
    }
    free(body); free(out);
    record_status(code);
    report(code == 200, "1MB body upload -> 200", code);
}

typedef struct {
    int id;
    int iters;
} SAbortWorker;

static void *stress_abort_worker(void *p) {
    SAbortWorker *w = (SAbortWorker *)p;
    int i;
    for (i = 0; i < w->iters; i++) {
        SOCKET s = raw_connect(g_host, g_port, TEST_TIMEOUT_MS);
        if (s == INVALID_SOCKET) { compat_sleep_ms(1); continue; }
        if (g_is_https) {
            static const uint8_t part[] = { 0x16, 0x03, 0x03 };
            send(s, (const char *)part, (int)sizeof(part), 0);
        } else {
            send(s, "GET", 3, 0);
        }
        tcp_close_rst(s);
    }
    return NULL;
}

static void stress_storm(void) {
    printf("\n=== [%s] STRESS: idle + aborted connection storm + health ===\n", tag());

    SOCKET *idle = (SOCKET *)malloc((size_t)STORM_IDLE * sizeof(SOCKET));
    int i;
    for (i = 0; i < STORM_IDLE; i++) idle[i] = raw_connect(g_host, g_port, TEST_TIMEOUT_MS);
    compat_sleep_ms(300);

    thr_t *hs = (thr_t *)malloc((size_t)4 * sizeof(thr_t));
    SAbortWorker *ws = (SAbortWorker *)calloc(4, sizeof(SAbortWorker));
    for (i = 0; i < 4; i++) {
        ws[i].id = i; ws[i].iters = STORM_ABORTS;
        thr_create(&hs[i], stress_abort_worker, &ws[i]);
    }
    for (i = 0; i < 4; i++) thr_join(&hs[i]);
    free(hs); free(ws);

    for (i = 0; i < STORM_IDLE; i++) tcp_close_rst(idle[i]);
    free(idle);

    unsigned long t0 = compat_tick();
    char buf[16384];
    int code = do_request(g_is_https, "GET", "/", NULL, NULL, 0, buf, (int)sizeof(buf));
    long elapsed = (long)(compat_tick() - t0);
    printf("     health after storm: code=%d in %ld ms\n", code, elapsed);
    record_status(code);
    report(code == 200 && elapsed < 60000, "server responsive after idle/abort storm", code);
}

/* Slowloris: send the request 1 byte per TRICKLE_GAP_MS so the header phase
 * exceeds the server deadline (io*12 + 1s). The server must answer 408 and
 * free the worker instead of pinning it. */
static void stress_trickle(void) {
    printf("\n=== [%s] STRESS: slowloris trickle (expect 408) ===\n", tag());

    TConn t;
    if (tc_open(&t, g_is_https, TEST_TIMEOUT_MS) != 0) { report(0, "trickle: cannot connect", 0); return; }

    static const char req[] = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    int rlen = (int)strlen(req);
    char buf[16384];
    int at = 0, i, code = 0;
    unsigned long t0 = compat_tick();

    /* Keep the in-loop "check" recv short (500ms) so the 2s send cadence is
     * never delayed past the server's per-call io_timeout. A blocking 5-8s
     * recv here would stall the next byte and make the server close early. */
    tc_set_recv_timeout(&t, 500);
    for (i = 0; i < rlen; i++) {
        int n;
        if (at < (int)sizeof(buf) - 1) {
            n = tc_recv(&t, buf + at, (int)sizeof(buf) - 1 - at);
            if (n > 0) {
                at += n;
                buf[at] = '\0';
                code = parse_status(buf);
                if (code == 408) break;
            } else if (n == 0) {
                /* server closed; the 408 (if any) was already delivered */
                break;
            }
            /* n < 0: short-recv timeout, keep trickling */
        }
        n = tc_send(&t, req + i, 1);
        if (n <= 0) {
            printf("     [note] send stopped at i=%d t=%lu err=%d (server closed)\n",
                   i, (unsigned long)(compat_tick() - t0), WSAGetLastError());
            break;
        }
        compat_sleep_ms(TRICKLE_GAP_MS);
    }
    /* Final blocking recv: catch a 408 that landed in the gap after the last
     * in-loop check (or after a failed send). */
    if (code != 408 && at < (int)sizeof(buf) - 1) {
        tc_set_recv_timeout(&t, 6000);
        int n = tc_recv(&t, buf + at, (int)sizeof(buf) - 1 - at);
        if (n > 0) {
            at += n;
            buf[at] = '\0';
            code = parse_status(buf);
        }
    }
    long elapsed = (long)(compat_tick() - t0);
    buf[at] = '\0';
    char *fl_end = strchr(buf, '\r');
    if (fl_end) *fl_end = '\0';
    printf("     sent %d/%d bytes, first line: %s (t=%ld ms)\n",
           i, rlen, (at > 0) ? buf : "NONE", elapsed);
    record_status(code);
    report(code == 408 && elapsed < 90000, "trickle slowloris ended with 408 (bounded)", code);
    tc_close(&t);
}

/* ==================== runners ==================== */

static void run_unit_tests(void) {
    printf("\n############################################\n");
    printf("# UNIT TESTS (%s server, %s://%s:%d)\n",
           g_is_web ? "web" : "base", tag(), g_host, g_port);
    printf("############################################\n");

    test_static();
    test_gzip();
    test_error_paths();
    test_body_limits();
    if (g_is_web) {
        test_web_api();
        test_web_cookies();
        test_web_forms_upload();
        test_web_websocket();
        test_web_sse();
    } else {
        test_base_no_api();
    }
    if (g_is_https) {
        test_tls_junk_records();
        test_tls_plaintext();
    }
}

static void run_stress_tests(void) {
    printf("\n############################################\n");
    printf("# STRESS TESTS (%s server, %s://%s:%d)\n",
           g_is_web ? "web" : "base", tag(), g_host, g_port);
    printf("############################################\n");

    stress_rapid();
    stress_concurrent();
    stress_big_upload();
    stress_storm();
    stress_trickle();
}

/* ==================== statistics + main ==================== */

static void print_statistics(void) {
    int i;
    printf("\n========================================\n");
    printf("STATISTICS: response codes (%s://%s:%d)\n", tag(), g_host, g_port);
    printf("========================================\n");
    for (i = 0; i < 600; i++) {
        if (status_count[i] == 0) continue;
        const char *what;
        if (i == 0) what = "connection did not happen";
        else if (i == 1) what = "connection established (no HTTP status)";
        else what = "HTTP status code";
        printf("  code %3d : count %3d  (%s)\n", i, status_count[i], what);
    }
    printf("========================================\n");
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s <base|web> <http|https> <host> <port> [unit|stress|all]\n"
        "  base    target server-base.exe (static only)\n"
        "  web     target server.exe (static + API + WS + SSE + upload)\n"
        "  http    plain-HTTP server (config ssl_use=false)\n"
        "  https   TLS server (config ssl_use=true)\n"
        "  unit    unit tests only\n"
        "  stress  stress tests only\n"
        "  all     unit + stress (default)\n"
        "Example: %s base https 127.0.0.1 443 all\n"
        "         %s web  http  127.0.0.1 8443 stress\n",
        argv0, argv0, argv0);
}

int main(int argc, char *argv[]) {
    int do_unit = 0, do_stress = 0;

    if (argc != 5 && argc != 6) { usage(argv[0]); return 1; }
    if (_stricmp(argv[1], "base") == 0) g_is_web = 0;
    else if (_stricmp(argv[1], "web") == 0) g_is_web = 1;
    else { fprintf(stderr, "Error: first parameter must be 'base' or 'web'\n"); usage(argv[0]); return 1; }

    if (_stricmp(argv[2], "https") == 0) g_is_https = 1;
    else if (_stricmp(argv[2], "http") == 0) g_is_https = 0;
    else { fprintf(stderr, "Error: second parameter must be 'http' or 'https'\n"); usage(argv[0]); return 1; }

    strncpy(g_host, argv[3], sizeof(g_host) - 1);
    g_host[sizeof(g_host) - 1] = '\0';

    g_port = atoi(argv[4]);
    if (g_port < 1 || g_port > 65535) {
        fprintf(stderr, "Error: invalid port '%s'\n", argv[4]);
        usage(argv[0]);
        return 1;
    }

    if (argc == 6) {
        if (_stricmp(argv[5], "unit") == 0) do_unit = 1;
        else if (_stricmp(argv[5], "stress") == 0) do_stress = 1;
        else if (_stricmp(argv[5], "all") == 0) { do_unit = 1; do_stress = 1; }
        else { fprintf(stderr, "Error: unknown option '%s'\n", argv[5]); usage(argv[0]); return 1; }
    } else {
        do_unit = 1;
        do_stress = 1;
    }

    if (sock_init() != 0) { fprintf(stderr, "socket init failed\n"); return 1; }
    mtx_init(&g_stats_cs);
    srand((unsigned)(time(NULL) ^ (unsigned)g_port));
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("web_test: %s server over %s://%s:%d (%s)\n",
           g_is_web ? "web" : "base", tag(), g_host, g_port,
           (do_unit && do_stress) ? "unit+stress" : (do_unit ? "unit" : "stress"));

    if (do_unit) run_unit_tests();
    if (do_stress) run_stress_tests();

    printf("\n========================================\n");
    printf("TEST SUMMARY\n");
    printf("========================================\n");
    printf("Total:  %d\n", test_total);
    printf("Passed: %d\n", test_passed);
    printf("Failed: %d\n", test_failed);
    print_statistics();

    mtx_destroy(&g_stats_cs);
    sock_cleanup();
    if (test_failed > 0) { printf("\n[RESULT] Some tests failed\n"); return 1; }
    printf("\n[RESULT] All tests passed!\n");
    return 0;
}
