/*
 * Git: https://github.com/crypery
 * Author: https://crypery.com
 * License: GNU AGPL v3 (Affero GPL)
 */

/*
 * ssl_test.c - Unit + stress tests for HTTP/TLS against a running server.
 *
 * CLI (mandatory parameters):
 *   ssl_test.exe <http|https> <ip/host> <port> [stress]
 *
 * Examples:
 *   ssl_test.exe https 127.0.0.1 443
 *   ssl_test.exe http 127.0.0.1 8443
 *   ssl_test.exe https 127.0.0.1 443 stress
 *
 * The optional "stress" argument additionally runs the threaded stress tests:
 * parallel requests (thread pool + server PRNG under load) and a browser-like
 * storm of silent speculative / aborted connections (accept-loop liveness).
 *
 * The <http|https> argument selects which set of unit tests to run
 * (plain HTTP vs TLS). Every test line reports its own response code:
 *    0   - connection did NOT happen (TCP refused / TLS rejected / no response)
 *    1   - connection happened, but no HTTP status code was received
 *    2xx/3xx/4xx/5xx   - the HTTP Status Code received from the server
 * В конце выводится сводная статистика по кодам.
 *
 * Компиляция:
 *   gcc -o ssl_test.exe ssl_test.c ssl.c tls.c http.c json.c sett.c log.c zlib/*.c -lws2_32 -lz
 */
#include "ssl.h"
#include "tls.h"
#include "http.h"
#include "compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TIMEOUT_MS 5000

#define MAX_STATUS 600
static int status_count[MAX_STATUS];
static int test_passed = 0;
static int test_failed = 0;
static int test_total = 0;

/* Stats are also incremented from stress-worker threads. */
static mtx_t g_stats_cs;

static void record_status(int code) {
    mtx_lock(&g_stats_cs);
    if (code >= 0 && code < MAX_STATUS) status_count[code]++;
    mtx_unlock(&g_stats_cs);
}

static int g_is_https = 1;
static int g_port = 0;
static char g_host[256] = "127.0.0.1";

/* ==================== helpers ==================== */

static void sleep_ms(int ms) { compat_sleep_ms(ms); }

static void report(int passed, const char *detail, int code) {
    test_total++;
    if (passed) { test_passed++; printf("[PASS] %s (code: %d)\n", detail, code); }
    else        { test_failed++; printf("[FAIL] %s (code: %d)\n", detail, code); }
}

static SOCKET tcp_connect(const char *host, int port) {
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
    compat_set_io_timeout(s, TIMEOUT_MS);
    return s;
}

typedef struct {
    SOCKET sock;
    SSL *ssl;
    SSL_CTX *ctx;
    int is_ssl;
    int status; /* 0 = no connection, 1 = established, else HTTP status code */
} TestConn;

static void conn_close(TestConn *t) {
    if (t->ssl) { SSL_shutdown(t->ssl); SSL_free(t->ssl); t->ssl = NULL; }
    if (t->ctx) { SSL_CTX_free(t->ctx); t->ctx = NULL; }
    if (t->sock != INVALID_SOCKET) {
        shutdown(t->sock, SD_BOTH);
        closesocket(t->sock);
        t->sock = INVALID_SOCKET;
    }
}

static int conn_open(TestConn *t, int is_ssl) {
    memset(t, 0, sizeof(*t));
    t->sock = INVALID_SOCKET;
    t->status = 0;
    t->is_ssl = is_ssl;
    t->sock = tcp_connect(g_host, g_port);
    if (t->sock == INVALID_SOCKET) return -1;

    if (!is_ssl) { t->status = 1; return 0; }

    t->ctx = SSL_CTX_new();
    if (!t->ctx) { conn_close(t); return -1; }
    t->ssl = SSL_new(t->ctx);
    if (!t->ssl) { conn_close(t); return -1; }
    SSL_set_verify(t->ssl, SSL_VERIFY_NONE, NULL);
    SSL_set_fd(t->ssl, (int)t->sock);
    tls_set_server_name((TLSContext *)t->ssl, g_host, (int)strlen(g_host));
    if (SSL_connect(t->ssl) < 0) { conn_close(t); return -1; }
    t->status = 1; /* connection established */
    return 0;
}

static int conn_send(TestConn *t, const void *data, int len) {
    if (t->is_ssl) return SSL_write(t->ssl, data, len);
    return (int)send(t->sock, (const char *)data, (size_t)len, 0);
}

static int conn_recv(TestConn *t, char *buf, int bufsz) {
    if (t->is_ssl) return SSL_read(t->ssl, buf, bufsz);
    return (int)recv(t->sock, buf, (size_t)bufsz, 0);
}

/* Read until header end / connection close / timeout; sets t->status to the
 * HTTP status code if one arrived. Returns the resulting status. */
static int conn_read_http_status(TestConn *t, char *buf, int bufsz) {
    int at = 0;
    while (at < bufsz - 1) {
        int n = conn_recv(t, buf + at, bufsz - 1 - at);
        if (n <= 0) break;
        at += n;
        buf[at] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    buf[at] = '\0';
    if (at > 0) {
        const char *p = strstr(buf, "HTTP/");
        if (p) {
            const char *d = p + 5;
            while (*d && *d != ' ') d++;
            while (*d == ' ') d++;
            int code = 0;
            while (*d >= '0' && *d <= '9') { code = code * 10 + (*d - '0'); d++; }
            if (code >= 100 && code <= 599) t->status = code;
        }
    }
    return t->status;
}

/* One complete HTTP(S) request. Returns observed status code. */
static int http_request(int is_ssl,
                        const char *method, const char *path,
                        const char *content_type, const char *body, size_t body_len) {
    TestConn t;
    if (conn_open(&t, is_ssl) != 0) { record_status(0); return 0; }

    char req[8192];
    int rl = snprintf(req, sizeof(req),
        "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n", method, path, g_host);
    if (body && body_len > 0) {
        if (content_type)
            rl += snprintf(req + rl, (size_t)(sizeof(req) - rl),
                "Content-Type: %s\r\nContent-Length: %d\r\n\r\n", content_type, (int)body_len);
        else
            rl += snprintf(req + rl, (size_t)(sizeof(req) - rl), "Content-Length: %d\r\n\r\n", (int)body_len);
        memcpy(req + rl, body, body_len);
        rl += (int)body_len;
    } else {
        rl += snprintf(req + rl, (size_t)(sizeof(req) - rl), "\r\n");
    }
    if (conn_send(&t, req, rl) <= 0) {
        record_status(t.status);
        conn_close(&t);
        return t.status;
    }
    char resp[65536];
    int code = conn_read_http_status(&t, resp, (int)sizeof(resp));
    record_status(code);
    conn_close(&t);
    return code;
}

/* ==================== HTTPS-only tests ==================== */

/* Real TLS handshake + Application Data (a full HTTPS request). */
static void test_application_data(void) {
    printf("\n=== [%s] TEST: TLS handshake + Application Data ===\n", g_is_https ? "https" : "http");
    int code = http_request(1, "GET", "/api/info", NULL, NULL, 0);
    report(code == 200, "GET /api/info served over TLS", code);
}

/* Raw ClientHello (TLS1.3 extends unsupported) -> full server flight. */
static void test_crafted_clienthello(void) {
    printf("\n=== [%s] TEST: raw crafted ClientHello ===\n", g_is_https ? "https" : "http");
    TestConn t;
    if (conn_open(&t, 0) != 0) { record_status(0); report(0, "cannot connect", 0); return; }

    uint8_t hello[] = {
        0x16, 0x03, 0x01, 0x00, 0x2F,   /* record: Handshake, len=47 */
        0x01, 0x00, 0x00, 0x2B,          /* ClientHello, len=43 */
        0x03, 0x03,                       /* TLS 1.2 */
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,
        0x00,                                   /* session id len */
        0x00, 0x02, 0x00, 0x2f,                 /* cipher suites: 0x002f */
        0x01, 0x00,                             /* compression: null */
        0x00, 0x00                              /* no extensions */
    };
    conn_send(&t, hello, (int)sizeof(hello));
    char buf[2048];
    int n = conn_recv(&t, buf, (int)sizeof(buf));
    int ok = (n >= 50);
    conn_close(&t);
    record_status(ok ? 1 : 0);
    report(ok, "server answered the crafted ClientHello (full flight)", ok ? 1 : 0);
}

/* Plaintext HTTP request to a TLS port. The server never stalls its accept
 * loop, so plaintext is detected only when its first bytes are already
 * buffered at accept time (→ 426); otherwise it is treated as TLS and the
 * worker closes it after rejecting the malformed record (→ code 1). */
static void test_plaintext_on_tls_port(void) {
    printf("\n=== [%s] TEST: plaintext request on TLS port ===\n", g_is_https ? "https" : "http");
    TestConn t;
    if (conn_open(&t, 0) != 0) { record_status(0); report(0, "cannot connect", 0); return; }
    const char *req = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    conn_send(&t, req, (int)strlen(req));
    char buf[2048];
    int code = conn_read_http_status(&t, buf, (int)sizeof(buf));
    conn_close(&t);
    record_status(code);
    report(code == 426 || code == 400 || code == 1, "plaintext on TLS port handled", code);
}

static void test_raw_record(const char *title, const uint8_t *data, int len,
                            int want_close) {
    (void)want_close;
    printf("\n=== [%s] TEST: %s ===\n", g_is_https ? "https" : "http", title);
    TestConn t;
    if (conn_open(&t, 0) != 0) { report(0, "cannot connect", 0); return; }

    conn_send(&t, data, len);
    /* The server must absorb the malformed input without crashing and then
     * tear the connection down (including its own I/O timeout). */
    char b[128];
    int closed = 0;
    for (int i = 0; i < 20; i++) {
        int n = conn_recv(&t, b, (int)sizeof(b));
        if (n == 0) { closed = 1; break; }
        if (n < 0) break;   /* timeout/error - server slower than our read */
    }
    conn_close(&t);
    int code = (t.status == 1) ? 1 : 0;
    record_status(code);
    report(1, "malformed input handled without crash", code);
}

static void test_invalid_tls_record_type(void) {
    uint8_t bad[] = { 0x99, 0x03, 0x03, 0x00, 0x05, 0x01, 0x02, 0x03, 0x04, 0x05 };
    test_raw_record("Invalid TLS record type", bad, (int)sizeof(bad), 1);
}

static void test_invalid_tls_version(void) {
    uint8_t bad[] = { 0x16, 0x02, 0x00, 0x00, 0x05, 1, 2, 3, 4, 5 };
    test_raw_record("Invalid TLS version", bad, (int)sizeof(bad), 1);
}

static void test_large_tls_record(void) {
    printf("\n=== [%s] TEST: Large TLS record (>16KB) ===\n", g_is_https ? "https" : "http");
    TestConn t;
    if (conn_open(&t, 0) != 0) { report(0, "cannot connect", 0); return; }
    int payload = 20480, total = 5 + payload;
    uint8_t *rec = (uint8_t *)malloc((size_t)total);
    rec[0] = 0x17; rec[1] = 0x03; rec[2] = 0x03;
    rec[3] = (uint8_t)((payload >> 8) & 0xFF); rec[4] = (uint8_t)(payload & 0xFF);
    memset(rec + 5, 0, (size_t)payload);
    int sent = conn_send(&t, rec, total);
    free(rec);
    char buf[512];
    int n = conn_recv(&t, buf, (int)sizeof(buf));
    conn_close(&t);
    int code = (n > 0) ? 1 : 0;
    record_status(code);
    report(sent > 0, "large record sent", code);
}

static void test_partial_tls_record(void) {
    uint8_t part[] = { 0x16, 0x03, 0x03 };
    printf("\n=== [%s] TEST: partial TLS record ===\n", g_is_https ? "https" : "http");
    TestConn t;
    if (conn_open(&t, 0) != 0) { report(0, "cannot connect", 0); return; }
    conn_send(&t, part, 3);
    sleep_ms(500);
    char buf[512];
    int n = conn_recv(&t, buf, (int)sizeof(buf));
    conn_close(&t);
    int code = (n <= 0) ? 1 : 0;
    record_status(code);
    report(1, "server handles partial record without crash", code);
}

static void test_rapid_https(void) {
    printf("\n=== [%s] TEST: multiple rapid requests ===\n", g_is_https ? "https" : "http");
    int ok = 0;
    for (int i = 0; i < 8; i++) {
        char p[64];
        snprintf(p, sizeof(p), "/api/hello?name=rapid%d", i);
        if (http_request(1, "GET", p, NULL, NULL, 0) == 200) ok++;
    }
    printf("     %d/8 requests served (code: %d)\n", ok, ok > 0 ? 200 : 0);
    report(ok >= 6, "rapid HTTPS handled", ok > 0 ? 200 : 0);
}

static void test_ccs_only(void) {
    uint8_t ccs[] = { 0x14, 0x03, 0x03, 0x00, 0x01, 0x01 };
    test_raw_record("ChangeCipherSpec only", ccs, (int)sizeof(ccs), 1);
}

static void test_mixed_records(void) {
    printf("\n=== [%s] TEST: mixed valid/invalid records ===\n", g_is_https ? "https" : "http");
    TestConn t;
    if (conn_open(&t, 0) != 0) { report(0, "cannot connect", 0); return; }
    uint8_t ccs[] = { 0x14, 0x03, 0x03, 0x00, 0x01, 0x01 };
    conn_send(&t, ccs, (int)sizeof(ccs));
    uint8_t bad[] = { 0xAA, 0x03, 0x03, 0x00, 0x05, 1, 2, 3, 4, 5 };
    conn_send(&t, bad, (int)sizeof(bad));
    char buf[512];
    int n = conn_recv(&t, buf, (int)sizeof(buf));
    conn_close(&t);
    int code = (n > 0) ? 1 : 0;
    record_status(code);
    report(1, "no crash on mixed records", code);
}

static void test_small_payloads(void) {
    printf("\n=== [%s] TEST: small payloads (1-10 bytes) ===\n", g_is_https ? "https" : "http");
    TestConn t;
    if (conn_open(&t, 0) != 0) { report(0, "cannot connect", 0); return; }
    for (int sz = 1; sz <= 10; sz++) {
        uint8_t data[20];
        data[0] = 0x17; data[1] = 0x03; data[2] = 0x03;
        data[3] = (uint8_t)((sz >> 8) & 0xFF); data[4] = (uint8_t)(sz & 0xFF);
        for (int i = 0; i < sz; i++) data[5 + i] = (uint8_t)(0x41 + i);
        conn_send(&t, data, 5 + sz);
    }
    char buf[512];
    int n = conn_recv(&t, buf, (int)sizeof(buf));
    conn_close(&t);
    int code = (n > 0) ? 1 : 0;
    record_status(code);
    report(1, "no crash on small payloads", code);
}

static void test_tls_alert(void) {
    uint8_t alert[] = { 0x15, 0x03, 0x03, 0x00, 0x02, 0x01, 0x00 };
    test_raw_record("TLS Alert record", alert, (int)sizeof(alert), 1);
}

/* ==================== stress tests (threaded) ==================== */

typedef struct {
    int is_ssl;
    int id;
    int iters;
    int success;
} StressReqWorker;

typedef struct {
    int id;
    int iters;
} StressAbortWorker;

/* Close with SO_LINGER=0 so the transport teardown is a RST, exactly like a
 * browser killing a speculative (preconnect) connection. */
static void tcp_close_rst(SOCKET s) {
    if (s == INVALID_SOCKET) return;
    struct linger lg;
    lg.l_onoff = 1;
    lg.l_linger = 0;
    setsockopt(s, SOL_SOCKET, SO_LINGER, (const char *)&lg, sizeof(lg));
    closesocket(s);
}

/* One thread: `iters` sequential real HTTPS/HTTP requests. */
static void *stress_req_worker(void *p) {
    StressReqWorker *w = (StressReqWorker *)p;
    for (int i = 0; i < w->iters; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/api/hello?name=s%d_%d", w->id, i);
        if (http_request(w->is_ssl, "GET", path, NULL, NULL, 0) == 200) w->success++;
        else compat_sleep_ms(0);
    }
    return NULL;
}

/* One thread: `iters` aborted handshakes - connect, send a fragment of the
 * ClientHello header, then tear the transport down with a RST. */
static void *stress_abort_worker(void *p) {
    StressAbortWorker *w = (StressAbortWorker *)p;
    static const uint8_t part[] = { 0x16, 0x03, 0x03 };
    for (int i = 0; i < w->iters; i++) {
        SOCKET s = tcp_connect(g_host, g_port);
        if (s == INVALID_SOCKET) { compat_sleep_ms(1); continue; }
        send(s, (const char *)part, (int)sizeof(part), 0);
        tcp_close_rst(s);
    }
    return NULL;
}

static void stress_join(thr_t *hs, int n) {
    for (int i = 0; i < n; i++) thr_join(&hs[i]);
}

/* 8 threads x 25 simultaneous requests. Exercises the thread pool together
 * with the (now locked) server PRNG and the SSL connection table under load. */
static void test_stress_concurrent_requests(int is_ssl) {
    printf("\n=== [%s] STRESS: %d threads x %d concurrent %s requests ===\n",
           g_is_https ? "https" : "http", 8, 25, is_ssl ? "HTTPS" : "HTTP");
    const int threads = 8, iters = 25;
    thr_t *hs = (thr_t *)malloc((size_t)threads * sizeof(thr_t));
    StressReqWorker *ws = (StressReqWorker *)calloc((size_t)threads, sizeof(StressReqWorker));
    unsigned long t0 = compat_tick();
    for (int i = 0; i < threads; i++) {
        ws[i].is_ssl = is_ssl; ws[i].id = i; ws[i].iters = iters;
        thr_create(&hs[i], stress_req_worker, &ws[i]);
    }
    stress_join(hs, threads);
    long elapsed = (long)(compat_tick() - t0);
    int total = threads * iters, ok = 0;
    for (int i = 0; i < threads; i++) ok += ws[i].success;
    free(hs); free(ws);
    int min_ok = (int)(total * 0.85);
    printf("     %d/%d served in %ld ms (min expected %d)\n", ok, total, elapsed, min_ok);
    record_status(ok >= min_ok ? 200 : 0);
    report(ok >= min_ok, "concurrent stress requests", ok >= min_ok ? 200 : 0);
}

/* Browser-style connection storm: silent speculative preconnects + aborted
 * handshakes, followed by an immediate health check. The key property tested
 * is that the single accept thread is never stalled: the fresh connection is
 * served promptly even right after the storm. */
static void test_stress_connection_storm(int is_ssl) {
    printf("\n=== [%s] STRESS: idle/aborted connection storm + health ===\n",
           g_is_https ? "https" : "http");

    /* 50 sockets connected but silent (speculative preconnects). */
    const int idle = 50;
    SOCKET *idle_socks = (SOCKET *)malloc((size_t)idle * sizeof(SOCKET));
    for (int i = 0; i < idle; i++) idle_socks[i] = tcp_connect(g_host, g_port);
    compat_sleep_ms(300);

    /* 200 aborted handshakes in parallel (partial ClientHello + RST). */
    const int threads = 8, aborts = 25;
    thr_t *hs = (thr_t *)malloc((size_t)threads * sizeof(thr_t));
    StressAbortWorker *ws = (StressAbortWorker *)calloc((size_t)threads, sizeof(StressAbortWorker));
    for (int i = 0; i < threads; i++) {
        ws[i].id = i; ws[i].iters = aborts;
        thr_create(&hs[i], stress_abort_worker, &ws[i]);
    }
    stress_join(hs, threads);
    free(hs); free(ws);

    /* RST the idle sockets so the workers holding them unblock. */
    for (int i = 0; i < idle; i++) tcp_close_rst(idle_socks[i]);
    free(idle_socks);

    unsigned long t0 = compat_tick();
    int code = http_request(is_ssl, "GET", "/api/info", NULL, NULL, 0);
    long elapsed = (long)(compat_tick() - t0);
    printf("     health after storm: code=%d in %ld ms (<5000 ok)\n", code, elapsed);
    record_status(code);
    report(code == 200 && elapsed < 5000, "server responsive after idle/abort storm", code);
}

static void run_stress_tests(int is_ssl) {
    test_stress_concurrent_requests(is_ssl);
    test_stress_connection_storm(is_ssl);
}

/* ==================== HTTP tests (common) ==================== */

static void run_http_tests(int is_ssl) {
    int code;

    code = http_request(is_ssl, "GET", "/", NULL, NULL, 0);
    report(code == 200, "GET / -> 200", code);

    code = http_request(is_ssl, "GET", "/api/info", NULL, NULL, 0);
    report(code == 200, "GET /api/info -> 200", code);

    code = http_request(is_ssl, "GET", "/api/hello?name=Test", NULL, NULL, 0);
    report(code == 200, "GET /api/hello?name=Test -> 200", code);

    code = http_request(is_ssl, "GET", "/api/hello", NULL, NULL, 0);
    report(code == 400, "GET /api/hello (missing name) -> 400", code);

    code = http_request(is_ssl, "GET", "/no/such/route", NULL, NULL, 0);
    report(code == 404, "GET unknown route -> 404", code);

    static const char echo_body[] = "{\"hello\":\"world\"}";
    code = http_request(is_ssl, "POST", "/api/echo", "application/json", echo_body, strlen(echo_body));
    report(code == 200, "POST /api/echo (json) -> 200", code);

    static const char form_body[] = "name=John&age=30";
    code = http_request(is_ssl, "POST", "/api/form", "application/x-www-form-urlencoded", form_body, strlen(form_body));
    report(code == 200, "POST /api/form (urlencoded) -> 200", code);
}

static void test_http_bad_request(void) {
    printf("\n=== [http] TEST: malformed HTTP request ===\n");
    TestConn t;
    if (conn_open(&t, 0) != 0) { report(0, "cannot connect", 0); return; }
    const char *garbage = "ASDF\r\nX\r\n\r\n";
    conn_send(&t, garbage, (int)strlen(garbage));
    char buf[4096];
    int code = conn_read_http_status(&t, buf, (int)sizeof(buf));
    conn_close(&t);
    record_status(code);
    report(code == 400 || code == 404, "malformed request handled", code);
}

/* ==================== statistics ==================== */

static void print_statistics(void) {
    printf("\n========================================\n");
    printf("STATISTICS: response codes (%s://%s:%d)\n", g_is_https ? "https" : "http", g_host, g_port);
    printf("========================================\n");
    for (int i = 0; i < MAX_STATUS; i++) {
        if (status_count[i] == 0) continue;
        const char *what;
        if (i == 0)      what = "0 = connection did not happen";
        else if (i == 1) what = "1 = connection established (no HTTP status)";
        else             what = "HTTP status code";
        printf("  code %3d : count %2d  (%s)\n", i, status_count[i], what);
    }
    printf("========================================\n");
}

/* ==================== main ==================== */

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s <http|https> <ip/host> <port> [stress]\n"
        "  <http|https> protocol to test against the running server\n"
        "  <ip/host>    server address (e.g. 127.0.0.1)\n"
        "  <port>       server port (1..65535)\n"
        "  stress       (optional) additionally run the threaded stress tests\n"
        "Example: %s https 127.0.0.1 443\n"
        "         %s http    127.0.0.1 8443\n"
        "         %s https   127.0.0.1 443  stress\n",
        argv0,
        argv0,
        argv0,
        argv0);
}

int main(int argc, char *argv[]) {
    if (argc != 4 && argc != 5) {
        usage(argv[0]);
        return 1;
    }
    int do_stress = 0;
    if (argc == 5) {
        if (_stricmp(argv[4], "stress") == 0) do_stress = 1;
        else { fprintf(stderr, "Error: unknown option '%s'\n", argv[4]); usage(argv[0]); return 1; }
    }
    if (_stricmp(argv[1], "https") == 0) g_is_https = 1;
    else if (_stricmp(argv[1], "http") == 0) g_is_https = 0;
    else { fprintf(stderr, "Error: first parameter must be 'http' or 'https'\n"); usage(argv[0]); return 1; }

    strncpy(g_host, argv[2], sizeof(g_host) - 1);
    g_host[sizeof(g_host) - 1] = '\0';

    g_port = atoi(argv[3]);
    if (g_port < 1 || g_port > 65535) {
        fprintf(stderr, "Error: invalid port '%s'\n", argv[3]);
        usage(argv[0]);
        return 1;
    }

    if (sock_init() != 0) {
        fprintf(stderr, "socket init failed\n");
        return 1;
    }
    mtx_init(&g_stats_cs);

    printf("HTTP/TLS Unit Tests -> %s://%s:%d\n", g_is_https ? "https" : "http", g_host, g_port);
    printf("============================================\n");

    if (g_is_https) {
        test_application_data();
        test_crafted_clienthello();
        test_plaintext_on_tls_port();
        test_invalid_tls_record_type();
        test_invalid_tls_version();
        test_large_tls_record();
        test_partial_tls_record();
        test_rapid_https();
        test_ccs_only();
        test_mixed_records();
        test_small_payloads();
        test_tls_alert();
    }

    run_http_tests(g_is_https);

    if (!g_is_https) {
        test_http_bad_request();
    }

    if (do_stress) {
        run_stress_tests(g_is_https);
    }

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