/*
 * Git: https://github.com/crypery
 * Author: https://crypery.com
 * License: GNU AGPL v3 (Affero GPL)
 */

/*
 * mini-c-server - main entry point (full-featured example server).
 *
 * Full example: static files plus JSON API, WebSocket, SSE,
 * file upload (multipart) and form/query parsing.
 * Loads config.json, initializes logging and the HTTP server, registers the
 * API routes, runs the server until shutdown, then prints statistics and
 * cleans up resources.
 *
 * Example:
 *   server.exe                # start the server using config.json
 *   curl -k https://127.0.0.1/api/info
 */
#include "log.h"
#include "json.h"
#include "sett.h"
#include "http.h"
#include "compat.h"
#include <stdbool.h>
#include <time.h>

/* Handler declarations */
/* GET /api/info - return server info as JSON */
int handle_get_info(http_server_t *http, http_ctx_t *c, const char *method, const char *uri, const char *body, size_t body_len);
/* POST /api/echo - echo request body as JSON */
int handle_post_echo(http_server_t *http, http_ctx_t *c, const char *method, const char *uri, const char *body, size_t body_len);
/* GET /api/hello - personalized greeting with name query param */
int handle_get_hello(http_server_t *http, http_ctx_t *c, const char *method, const char *uri, const char *body, size_t body_len);
/* POST /api/upload - handle multipart file upload */
int handle_post_upload(http_server_t *http, http_ctx_t *c, const char *method, const char *uri, const char *body, size_t body_len);
/* GET /ws - WebSocket endpoint handler */
int handle_get_ws(http_server_t *http, http_ctx_t *c, const char *method, const char *uri, const char *body, size_t body_len);
/* GET /sse - Server-Sent Events stream handler */
int handle_get_sse(http_server_t *http, http_ctx_t *c, const char *method, const char *uri, const char *body, size_t body_len);
/* GET /api/params - display parsed query params as HTML */
int handle_get_params(http_server_t *http, http_ctx_t *c, const char *method, const char *uri, const char *body, size_t body_len);
/* POST /api/form - parse form data (urlencoded or multipart) */
int handle_post_form(http_server_t *http, http_ctx_t *c, const char *method, const char *uri, const char *body, size_t body_len);

/* --- WebSocket log broadcast --- */

#define WS_MAX_CLIENTS 256

/* Connected WebSocket client contexts (guarded by g_ws_lock) */
static http_ctx_t *g_ws_clients[WS_MAX_CLIENTS];
static int g_ws_count = 0;
static mtx_t g_ws_lock;

/* Register a connected WebSocket client */
static void ws_register(http_ctx_t *c) {
    mtx_lock(&g_ws_lock);
    if (g_ws_count < WS_MAX_CLIENTS) g_ws_clients[g_ws_count++] = c;
    mtx_unlock(&g_ws_lock);
}

/* Remove a disconnected WebSocket client */
static void ws_unregister(http_ctx_t *c) {
    mtx_lock(&g_ws_lock);
    for (int i = 0; i < g_ws_count; i++) {
        if (g_ws_clients[i] == c) {
            g_ws_clients[i] = g_ws_clients[g_ws_count - 1];
            g_ws_count--;
            break;
        }
    }
    mtx_unlock(&g_ws_lock);
}

/* Broadcast a log line to all connected WebSocket clients */
static void ws_log_broadcast(const char *msg) {
    mtx_lock(&g_ws_lock);
    for (int i = 0; i < g_ws_count; i++) {
        http_ws_send_text(g_ws_clients[i], msg);
    }
    mtx_unlock(&g_ws_lock);
}

/* --- helpers --- */

/* Create a directory and all missing parents (split on '/' and '\') */
static void ensure_dir(const char *path) {
    char tmp[2048];
    strncpy(tmp, path, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char save = *p; *p = '\0';
            if (tmp[0]) compat_mkdir(tmp);
            *p = save;
        }
    }
    if (tmp[0]) compat_mkdir(tmp);
}

/* Build a JSON object of the request header lines (skips the request line) */
static void headers_to_json(char *out, size_t out_size, const char *raw, size_t rlen) {
    const char *p = raw;
    const char *end = raw + rlen;
    while (p < end && *p != '\n') p++;   /* skip request line */
    if (p < end) p++;
    int first = 1;
    char *w = out;
    size_t left = out_size;
    #define W(...) do { int n = snprintf(w, left, __VA_ARGS__); if (n < 0) return; if ((size_t)n >= left) return; w += n; left -= (size_t)n; } while (0)
    W("{");
    while (p < end) {
        const char *le = p;
        while (le < end && *le != '\r' && *le != '\n') le++;
        if (le == p) break;
        const char *colon = p;
        while (colon < le && *colon != ':') colon++;
        if (colon < le) {
            char key[128], val[256], kbuf[512], vbuf[512];
            size_t kl = (size_t)(colon - p); if (kl > 127) kl = 127;
            memcpy(key, p, kl); key[kl] = '\0';
            const char *vs = colon + 1;
            while (vs < le && *vs == ' ') vs++;
            size_t vl = (size_t)(le - vs); if (vl > 255) vl = 255;
            memcpy(val, vs, vl); val[vl] = '\0';
            http_html_escape_str(kbuf, sizeof(kbuf), key);
            http_html_escape_str(vbuf, sizeof(vbuf), val);
            if (!first) W(",");
            first = 0;
            W("\"%s\":\"%s\"", kbuf, vbuf);
        }
        p = le;
        while (p < end && (*p == '\r' || *p == '\n')) p++;
    }
    W("}");
    #undef W
}

/* --- API handlers --- */

/* GET /api/info - return server info as JSON */
int handle_get_info(http_server_t *http, http_ctx_t *c, const char *method, const char *uri, const char *body, size_t body_len) {
    (void)method; (void)uri; (void)body; (void)body_len;
    char hdr_json[16384];
    headers_to_json(hdr_json, sizeof(hdr_json), http_req_buf(c), c->rlen);
    time_t now = time(NULL);
    double uptime = difftime(now, http->start_time);
    char json[32768];
    snprintf(json, sizeof(json),
        "{\"server\":\"%s\",\"uptime_sec\":%.0f,\"requests\":%ld,"
        "\"clients\":%d,\"ws_clients\":%d,\"upload_dir\":\"%s\",\"method\":\"%s\","
        "\"features\":[\"POST\",\"WebSocket\",\"SSE\",\"MultipartUpload\",\"JSON\",\"QueryParams\"],"
        "\"headers\":%s}",
        http->server_name, uptime, http->total_req, http->max_clients, g_ws_count,
        http->upload_dir ? http->upload_dir : "",method, hdr_json);
    http_send_json_response(http, c, 200, json);
    http_update_status_stats(http, 200);
    return 1;
}

/* POST /api/echo - echo request body as JSON */
int handle_post_echo(http_server_t *http, http_ctx_t *c, const char *method, const char *uri, const char *body, size_t body_len) {
    (void)method; (void)uri;
    char json[4096];
    snprintf(json, sizeof(json),
        "{\"echo\":%.*s,\"method\":\"%s\",\"uri\":\"%s\",\"body_len\":%zu}",
        (int)body_len, body, method, uri, body_len);
    http_send_json_response(http, c, 200, json);
    http_update_status_stats(http, 200);
    return 1;
}

/* GET /api/hello - personalized greeting with name query param */
int handle_get_hello(http_server_t *http, http_ctx_t *c, const char *method, const char *uri, const char *body, size_t body_len) {
    (void)method; (void)body; (void)body_len;
    http_query_string_t qs;
    http_query_init(&qs);
    char clean_uri[2048];
    strncpy(clean_uri, uri, sizeof(clean_uri) - 1);
    clean_uri[sizeof(clean_uri) - 1] = '\0';
    char *qs_start = strchr(clean_uri, '?');
    if (qs_start) { *qs_start = '\0'; http_query_parse(&qs, qs_start + 1); }
    const char *name = http_query_get(&qs, "name");
    if (!name || name[0] == '\0') {
        http_send_json_response(http, c, 400, "{\"error\":\"Query parameter 'name' is required\"}");
        http_query_destroy(&qs);
        return 1;
    }
    char json[512];
    snprintf(json, sizeof(json),
        "{\"message\":\"Hello, %s!\",\"name\":\"%s\",\"query_params\":{\"name\":\"%s\"}}",
        name, name, name);
    http_query_destroy(&qs);
    http_send_json_response(http, c, 200, json);
    http_update_status_stats(http, 200);
    return 1;
}

/* POST /api/upload - handle multipart file upload */
int handle_post_upload(http_server_t *http, http_ctx_t *c, const char *method, const char *uri, const char *body, size_t body_len) {
    (void)method; (void)uri;
    if (body_len == 0) { http_send_json_response(http, c, 400, "{\"error\":\"Empty body\"}"); return 1; }
    char *boundary = NULL;
    const char *ct = http_get_content_type(http_req_buf(c));
    if (ct && strstr(ct, "multipart/form-data")) {
        const char *bnd = strstr(ct, "boundary=");
        if (bnd) { bnd += 9; boundary = (char *)malloc(strlen(bnd) + 1); strcpy(boundary, bnd); }
    }
    if (!boundary) { http_send_json_response(http, c, 400, "{\"error\":\"Content-Type must be multipart/form-data\"}"); return 1; }
    http_multipart_form_t form;
    int field_count = http_multipart_parse(&form, body, body_len, boundary);
    free(boundary);
    if (field_count == 0) { http_send_json_response(http, c, 400, "{\"error\":\"No fields found\"}"); return 1; }
    char json[8192];
    int joff = snprintf(json, sizeof(json), "{\"upload\":{\"files\":[");
    for (int i = 0; i < form.count; i++) {
        const http_multipart_field_t *f = &form.fields[i];
        if (i > 0) joff += snprintf(json + joff, (size_t)(sizeof(json) - joff), ",");
        char name_buf[512], fname_buf[512], ct_buf[512];
        http_html_escape_str(name_buf, sizeof(name_buf), f->name ? f->name : "");
        http_html_escape_str(fname_buf, sizeof(fname_buf), f->filename ? f->filename : "");
        http_html_escape_str(ct_buf, sizeof(ct_buf), f->content_type ? f->content_type : "application/octet-stream");
        joff += snprintf(json + joff, (size_t)(sizeof(json) - joff),
            "{\"field\":\"%s\",\"filename\":\"%s\",\"content_type\":\"%s\",\"size\":%zu",
            name_buf, fname_buf, ct_buf, f->data_len);
        if (f->filename && f->filename[0]) {
            const char *dir = http->upload_dir && http->upload_dir[0] ? http->upload_dir : DEF_UPLOAD_DIR;
            ensure_dir(dir);
            char path[2048];
            snprintf(path, sizeof(path), "%s/%s", dir, f->filename);
            FILE *fp = fopen(path, "wb");
            if (fp) { fwrite(f->data, 1, f->data_len, fp); fclose(fp); joff += snprintf(json + joff, (size_t)(sizeof(json) - joff), ",\"saved\":\"%s\"", path); }
            else { joff += snprintf(json + joff, (size_t)(sizeof(json) - joff), ",\"saved\":false"); }
        }
        joff += snprintf(json + joff, (size_t)(sizeof(json) - joff), "}");
    }
    joff += snprintf(json + joff, (size_t)(sizeof(json) - joff), "],\"total_files\":%d}}", form.count);
    http_multipart_destroy(&form);
    http_send_json_response(http, c, 200, json);
    http_update_status_stats(http, 200);
    return 1;
}

/* GET /ws - WebSocket endpoint handler */
int handle_get_ws(http_server_t *http, http_ctx_t *c, const char *method, const char *uri, const char *body, size_t body_len) {
    (void)method; (void)uri; (void)body; (void)body_len;
    if (!http_ws_handshake(c, http_req_buf(c), c->rlen)) { http_send_json_response(http, c, 400, "{\"error\":\"WebSocket upgrade failed\"}"); return 1; }
    ws_register(c);
    unsigned char rb[65536];
    size_t rlen = 0;
    int ws_active = 1;
    while (ws_active) {
        fd_set rf; FD_ZERO(&rf); FD_SET(c->fd, &rf);
        struct timeval tv = {0, 100000};
        int ready = select((int)c->fd + 1, &rf, NULL, NULL, &tv);
        if (ready <= 0) continue;
        if (!FD_ISSET(c->fd, &rf)) continue;
        int n = http_sock_read_data(c, (char *)(rb + rlen), (int)(sizeof(rb) - rlen));
        if (n <= 0) { ws_active = 0; break; }
        rlen += (size_t)n;
        size_t processed = 0;
        while (processed + 2 <= rlen) {
            http_ws_frame_t frame;
            memset(&frame, 0, sizeof(frame));
            int frame_len = http_ws_read_frame(c->fd, rb, rlen, processed, &frame);
            if (frame_len <= 0) break;
            unsigned char opcode = (rb[processed] & 0x0F);
            switch (opcode) {
                case HTTP_WS_OPCODE_TEXT: {
                    char json_resp[4096];
                    snprintf(json_resp, sizeof(json_resp), "{\"type\":\"echo\",\"data\":\"%.*s\"}", (int)frame.payload_len, frame.payload);
                    mtx_lock(&g_ws_lock);
                    http_ws_send_text(c, json_resp);
                    mtx_unlock(&g_ws_lock);
                    break;
                }
                case HTTP_WS_OPCODE_BINARY: {
                    char resp[4096];
                    snprintf(resp, sizeof(resp), "{\"type\":\"binary_echo\",\"size\":%d}", (int)frame.payload_len);
                    mtx_lock(&g_ws_lock);
                    http_ws_send_text(c, resp);
                    mtx_unlock(&g_ws_lock);
                    break;
                }
                case HTTP_WS_OPCODE_PING: {
                    mtx_lock(&g_ws_lock);
                    http_ws_send_pong(c, frame.payload, frame.payload_len);
                    mtx_unlock(&g_ws_lock);
                    break;
                }
                case HTTP_WS_OPCODE_CLOSE: { ws_active = 0; mtx_lock(&g_ws_lock); http_ws_send_close(c, 1000, "close"); mtx_unlock(&g_ws_lock); break; }
            }
            processed += (size_t)frame_len;
        }
        if (processed > 0 && processed < rlen) memmove(rb, rb + processed, rlen - processed);
        rlen -= processed;
    }
    ws_unregister(c);
    http_update_status_stats(http, 101);
    return 1;
}

/* GET /sse - Server-Sent Events stream handler */
int handle_get_sse(http_server_t *http, http_ctx_t *c, const char *method, const char *uri, const char *body, size_t body_len) {
    (void)method; (void)uri; (void)body; (void)body_len;
    http_update_status_stats(http, 200);
    http_sse_init_response(c);
    http_sse_send(c, "connected", "{\"message\":\"SSE connection established\"}", "0", NULL);
    int counter = 0;
    while (1) {
        fd_set rf; FD_ZERO(&rf); FD_SET(c->fd, &rf);
        struct timeval tv = {0, 500000};
        int ready = select((int)c->fd + 1, &rf, NULL, NULL, &tv);
        if (ready > 0 && FD_ISSET(c->fd, &rf)) {
            char probe[1];
            if (http_sock_read_data(c, probe, 1) <= 0) break;   /* client disconnected */
        }
        counter++;
        char json_msg[512];
        snprintf(json_msg, sizeof(json_msg), "{\"counter\":%d,\"time\":%ld}", counter, (long)time(NULL));
        http_sse_send(c, "tick", json_msg, NULL, NULL);
    }
    return 1;
}

/* GET /api/params - display parsed query params as HTML */
int handle_get_params(http_server_t *http, http_ctx_t *c, const char *method, const char *uri, const char *body, size_t body_len) {
    (void)method; (void)body; (void)body_len;
    http_query_string_t qs;
    http_query_init(&qs);
    char clean_uri[2048];
    strncpy(clean_uri, uri, sizeof(clean_uri) - 1);
    clean_uri[sizeof(clean_uri) - 1] = '\0';
    char *qs_start = strchr(clean_uri, '?');
    if (qs_start) { *qs_start = '\0'; http_query_parse(&qs, qs_start + 1); }
    char html[8192];
    int hoff = snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head><title>Query Params</title></head><body>"
        "<h1>Parsed Query Parameters</h1><pre style='background:#f0f0f0;padding:10px'>");
    if (qs.count == 0) {
        hoff += snprintf(html + hoff, (size_t)(sizeof(html) - hoff), "No query parameters found.\n\nExample: /api/params?name=John&age=30&city=NYC");
    } else {
        for (int i = 0; i < qs.count; i++) {
            char key_buf[512], val_buf[512];
            http_html_escape_str(key_buf, sizeof(key_buf), qs.params[i].key ? qs.params[i].key : "");
            http_html_escape_str(val_buf, sizeof(val_buf), qs.params[i].value ? qs.params[i].value : "");
            hoff += snprintf(html + hoff, (size_t)(sizeof(html) - hoff), "%s = %s\n", key_buf, val_buf);
        }
    }
    hoff += snprintf(html + hoff, (size_t)(sizeof(html) - hoff), "</pre></body></html>");
    http_query_destroy(&qs);
    http_send_html_response(http, c, 200, html);
    http_update_status_stats(http, 200);
    return 1;
}

/* POST /api/form - parse form data (urlencoded or multipart) */
int handle_post_form(http_server_t *http, http_ctx_t *c, const char *method, const char *uri, const char *body, size_t body_len) {
    (void)method; (void)uri;
    if (body_len == 0) { http_send_json_response(http, c, 400, "{\"error\":\"Empty body\"}"); return 1; }
    const char *ct = http_get_content_type(http_req_buf(c));
    if (ct && strstr(ct, "application/x-www-form-urlencoded")) {
        http_query_string_t qs;
        http_query_init(&qs);
        http_query_parse(&qs, body);
        char json[8192];
        int joff = snprintf(json, sizeof(json), "{\"form\":{");
        for (int i = 0; i < qs.count; i++) {
            if (i > 0) joff += snprintf(json + joff, (size_t)(sizeof(json) - joff), ",");
            char kbuf[512], vbuf[512];
            http_html_escape_str(kbuf, sizeof(kbuf), qs.params[i].key ? qs.params[i].key : "");
            http_html_escape_str(vbuf, sizeof(vbuf), qs.params[i].value ? qs.params[i].value : "");
            joff += snprintf(json + joff, (size_t)(sizeof(json) - joff), "\"%s\":\"%s\"", kbuf, vbuf);
        }
        joff += snprintf(json + joff, (size_t)(sizeof(json) - joff), "}}");
        http_query_destroy(&qs);
        http_send_json_response(http, c, 200, json);
        http_update_status_stats(http, 200);
        return 1;
    }
    if (ct && strstr(ct, "multipart/form-data")) {
        char *boundary = NULL;
        const char *bnd = strstr(ct, "boundary=");
        if (bnd) { bnd += 9; boundary = (char *)malloc(strlen(bnd) + 1); strcpy(boundary, bnd); }
        if (boundary) {
            http_multipart_form_t form;
            int field_count = http_multipart_parse(&form, body, body_len, boundary);
            free(boundary);
            char json[8192];
            int joff = snprintf(json, sizeof(json), "{\"multipart\":{\"fields\":[");
            for (int i = 0; i < field_count; i++) {
                if (i > 0) joff += snprintf(json + joff, (size_t)(sizeof(json) - joff), ",");
                char nbuf[512];
                http_html_escape_str(nbuf, sizeof(nbuf), form.fields[i].name ? form.fields[i].name : "");
                joff += snprintf(json + joff, (size_t)(sizeof(json) - joff),
                    "{\"name\":\"%s\",\"value\":\"%.*s\"}",
                    nbuf, (int)form.fields[i].data_len, (char *)form.fields[i].data);
            }
            joff += snprintf(json + joff, (size_t)(sizeof(json) - joff), "]}}");
            http_multipart_destroy(&form);
            http_send_json_response(http, c, 200, json);
            http_update_status_stats(http, 200);
            return 1;
        }
    }
    char json[4096];
    snprintf(json, sizeof(json), "{\"raw_body\":\"%.*s\"}", (int)body_len, body);
    http_send_json_response(http, c, 200, json);
    http_update_status_stats(http, 200);
    return 1;
}

/* --- Default config written when config.json is missing --- */

static const char *sett_default =
    "{"
    "    \"ssl_use\": true,\n"
    "    \"port\": 81,\n"
    "    \"root\": \"www\",\n"
    "    \"upload_dir\": \"www/upload\",\n"
    "    \"cert_file\": \"localhost.crt\",\n"
    "    \"key_file\": \"localhost.key\",\n"
    "    \"max_clients\": 256,\n"
    "    \"timeout_io\": 5000,\n"
    "    \"timeout_handshake\": 10000,\n"
    "    \"timeout_tls_poll\": 500,\n"
    "    \"timeout_flight\": 5000,\n"
    "    \"timeout_accept\": 500,\n"
    "    \"worker_threads\": 4,\n"
    "    \"max_body_mb\": 0,\n"
    "    \"log_file\": \"server.log\",\n"
    "    \"log_output\": 2,\n"
    "    \"log_level\": 3,\n"
    "    \"server_name\": \"nginx/1.25.2\",\n"
    "    \"domains\": \"localhost,127.0.0.1\",\n"
    "    \"www_redirect\": true\n"
    "}\n";

/* --- Entry point --- */

/* Logging callback for the http module: forwards every message to the logging
 * subsystem, which writes it as configured (console / file / level). */
static void http_log_cb(int level, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_vmsg((LogLevel)level, fmt, args);
    va_end(args);
}

int main(void) {
    printf("Web server starting...\n");
    fflush(stdout);

    if (sock_init() != 0) { printf("[err] socket init failed\n"); return 1; }

    FILE *cfg = fopen("config.json", "rb");
    if (!cfg) {
        cfg = fopen("config.json", "w");
        if (!cfg) { fprintf(stderr, "[FATAL] Cannot create config.json\n"); sock_cleanup(); return 1; }
        fputs(sett_default, cfg);
        fclose(cfg);
        printf("[cfg] config.json not found, created with default values\n");
    } else { fclose(cfg); }

    Sett *sett = sett_init("config.json");
    if (sett == NULL) { fprintf(stderr, "[FATAL] Cannot load config.json\n"); sock_cleanup(); return 1; }

    /* Initialize the HTTP module with all configuration parameters */
    http_server_t *http = http_init();
    if (http == NULL) { LOG_ERROR("[err] http_init failed\n"); return 1; }

    http->port  = sett_get_int(sett, "port", 443);
    if (http->port  < 1 || http->port  > 65535) {  LOG_ERROR("[err] port failed\n"); return 1; }

    http->max_clients = sett_get_int(sett, "max_clients", MAX_CLIENTS);
    if (http->max_clients < 1 || http->max_clients > 4096) { LOG_ERROR("[err] max_clients failed\n"); return 1; }

    http->timeout_io_ms = sett_get_int(sett, "timeout_io", TIMEOUT_IO);
    if (http->timeout_io_ms < 0 || http->timeout_io_ms > 60000) { LOG_ERROR("[err] timeout_io_ms failed\n"); return 1; }

    http->timeout_handshake_ms = sett_get_int(sett, "timeout_handshake", TIMEOUT_HANDSHAKE);
    if (http->timeout_handshake_ms < 0 || http->timeout_handshake_ms > 120000) { LOG_ERROR("[err] timeout_handshake_ms failed\n"); return 1;  }

    http->timeout_tls_poll_ms = sett_get_int(sett, "timeout_tls_poll", TIMEOUT_TLS_POLL);
    if (http->timeout_tls_poll_ms < 0 || http->timeout_tls_poll_ms > 60000) { LOG_ERROR("[err] timeout_tls_poll failed\n"); return 1; }

    http->timeout_flight_ms = sett_get_int(sett, "timeout_flight", TIMEOUT_FLIGHT);
    if (http->timeout_flight_ms < 0 || http->timeout_flight_ms > 60000) { LOG_ERROR("[err] timeout_flight failed\n"); return 1; }

    http->timeout_accept_ms = sett_get_int(sett, "timeout_accept", TIMEOUT_ACCEPT);
    if (http->timeout_accept_ms < 0 || http->timeout_accept_ms > 60000) { LOG_ERROR("[err] timeout_accept failed\n"); return 1; }

    http->worker_threads = sett_get_int(sett, "worker_threads", NWORKERS);
    if (http->worker_threads < 1 || http->worker_threads > 64) { LOG_ERROR("[err] worker_threads failed\n"); return 1; }

    http->max_body_mb = sett_get_int(sett, "max_body_mb", DEF_MAX_BODY_MB);
    if (http->max_body_mb < 0 || http->max_body_mb > 8192) { LOG_ERROR("[err] max_body_mb failed (0=unlimited, 1..8192 MB)\n"); return 1; }

    http->ssl_use = sett_get_bool(sett, "ssl_use", true);

    http->log_output = sett_get_int(sett, "log_output", HTTP_LOG_OUTPUT_CONSOLE);
    if (http->log_output < 0 || http->log_output > 3) { LOG_ERROR("[err] log_output failed\n"); return 1; }

    http->log_level = sett_get_int(sett, "log_level", LOG_LEVEL_ERROR);
    if (http->log_level < LOG_LEVEL_DEBUG || http->log_level > LOG_LEVEL_ERROR) { LOG_ERROR("[err] log_level failed\n"); return 1; }

    http->root = sett_get_string(sett, "root", "www");
    if (!http->root || strlen(http->root)==0) { LOG_ERROR("[err] root failed\n"); return 1; }

    http->upload_dir = sett_get_string(sett, "upload_dir", DEF_UPLOAD_DIR);
    if (!http->upload_dir || strlen(http->upload_dir)==0) { LOG_ERROR("[err] upload_dir failed\n"); return 1; }
    ensure_dir(http->upload_dir);

    http->cert_file = sett_get_string(sett, "cert_file", "localhost.crt");
    if (!http->cert_file || strlen(http->cert_file)==0) { LOG_ERROR("[err] cert_file failed\n"); return 1; }

    http->key_file = sett_get_string(sett, "key_file", "localhost.key");
    if (!http->key_file || strlen(http->key_file)==0) { LOG_ERROR("[err] key_file failed\n"); return 1; }
  
    http->log_file = sett_get_string(sett, "log_file", "server.log");
    if (!http->log_file || strlen(http->log_file)==0) { LOG_ERROR("[err] log_file failed\n"); return 1; }

    http->server_name = sett_get_string(sett, "server_name", DEF_SERVER_NAME);
    if (!http->server_name || strlen(http->server_name)==0) { LOG_ERROR("[err] server_name failed\n"); return 1; }

    http->domains = sett_get_string(sett, "domains", DEF_DOMAINS);
    if (!http->domains || strlen(http->domains)==0) { LOG_ERROR("[err] domains failed\n"); return 1; }

    http->www_redirect = sett_get_bool(sett, "www_redirect", true) ? 1 : 0;

    log_init(http->log_output, http->log_level, http->log_file);
    log_set_callback(ws_log_broadcast);
    mtx_init(&g_ws_lock);
    LOG_INFO("[main] config loaded (port=%d, ssl=%d, root=%s, upload=%s, max_body_mb=%d)\n",
             http->port, http->ssl_use, http->root, http->upload_dir, http->max_body_mb);

    /* Register API routes */
    http_route(http, "GET",  "/api/info",   handle_get_info);
    http_route(http, "GET",  "/api/hello",  handle_get_hello);
    http_route(http, "GET",  "/api/params", handle_get_params);
    http_route(http, "GET",  "/ws",         handle_get_ws);
    http_route(http, "GET",  "/sse",        handle_get_sse);
    http_route(http, "POST", "/api/echo",   handle_post_echo);
    http_route(http, "POST", "/api/upload", handle_post_upload);
    http_route(http, "POST", "/api/form",   handle_post_form);

    /* Start the server */
    if (http_run(http, http_log_cb) != 0) { LOG_ERROR("[err] http_run failed\n"); http_free(http); return 1; }

    http_print_statistics(http);

    /* Clean up resources */
    mtx_destroy(&g_ws_lock);
    http_free(http);
    log_close();
    sett_free(sett);
    return 0;
}
