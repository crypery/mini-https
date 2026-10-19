/*
 * Git: https://github.com/crypery
 * Author: https://crypery.com
 * License: GNU AGPL v3 (Affero GPL)
 */

/*
 * server-base - main entry point (static site example server).
 *
 * Minimal example: serves static files from the www/ directory only
 * (no API / WebSocket / SSE / upload routes).
 * Loads config.json, initializes logging and the HTTP server,
 * runs the server until shutdown, then prints statistics and
 * cleans up resources.
 */

#include "log.h"
#include "json.h"
#include "sett.h"
#include "http.h"
#include "compat.h"
#include <stdbool.h>
#include <time.h>

/* --- Default config written when config.json is missing --- */

static const char *sett_default =
    "{"
    "    \"ssl_use\": true,\n"
    "    \"port\": 81,\n"
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
    "    \"domains\": {\n"
    "        \"localhost\": \"www\",\n"
    "        \"127.0.0.1\": \"www\"\n"
    "    },\n"
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

    http->cert_file = sett_get_string(sett, "cert_file", "localhost.crt");
    if (!http->cert_file || strlen(http->cert_file)==0) { LOG_ERROR("[err] cert_file failed\n"); return 1; }

    http->key_file = sett_get_string(sett, "key_file", "localhost.key");
    if (!http->key_file || strlen(http->key_file)==0) { LOG_ERROR("[err] key_file failed\n"); return 1; }
  
    http->log_file = sett_get_string(sett, "log_file", "server.log");
    if (!http->log_file || strlen(http->log_file)==0) { LOG_ERROR("[err] log_file failed\n"); return 1; }

    http->server_name = sett_get_string(sett, "server_name", DEF_SERVER_NAME);
    if (!http->server_name || strlen(http->server_name)==0) { LOG_ERROR("[err] server_name failed\n"); return 1; }

    /* Domains: a JSON object mapping "domain" -> "root path" (per-domain
     * document roots). Logging is not initialized yet here, so errors go
     * straight to stderr. */
    {
        JsonValue *dv = sett_get_json(sett, "domains");
        if (!json_is_object(dv)) { fprintf(stderr, "[err] domains failed (expected an object: \"domain\": \"root path\")\n"); return 1; }
        for (int i = 0; i < dv->count; i++) {
            JsonValue *v = dv->children[i];
            if (!dv->keys[i] || !json_is_string(v) || !json_string(v)[0]) {
                fprintf(stderr, "[err] domains failed (\"%s\" root is not a non-empty string)\n", dv->keys[i] ? dv->keys[i] : "?");
                return 1;
            }
            if (http_domain(http, dv->keys[i], json_string(v)) != 0) {
                fprintf(stderr, "[err] domains failed (%s)\n", dv->keys[i]);
                return 1;
            }
        }
        if (http->domain_count == 0) { fprintf(stderr, "[err] domains failed (empty)\n"); return 1; }
    }

    http->www_redirect = sett_get_bool(sett, "www_redirect", true) ? 1 : 0;

    log_init(http->log_output, http->log_level, http->log_file);
    LOG_INFO("[main] config loaded (port=%d, ssl=%d, upload=%s, max_body_mb=%d, domains=%d)\n",
             http->port, http->ssl_use, http->upload_dir, http->max_body_mb, http->domain_count);

    /* Start the server */
    if (http_run(http, http_log_cb) != 0) { LOG_ERROR("[err] http_run failed\n"); http_free(http); return 1; }

    http_print_statistics(http);

    /* Clean up resources */
    http_free(http);
    log_close();
    sett_free(sett);
    return 0;
}
