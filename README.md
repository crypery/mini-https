# Mini HTTPS Server

> [Russian version](README_RU.md)

> [China version](README_CN.md)

A self-contained mini HTTPS server written in C/CPP, built on the **crypery HTTPS Library**. It requires no OpenSSL or other external dependencies: the TLS stack, the HTTPS server, and all utilities are bundled in a single project.

## Description

The server accepts HTTP/1.1 requests over TCP or TLS (HTTPS), serves static files from the `www/` directory, provides a JSON API, WebSocket and Server-Sent Events, accepts file uploads (multipart), and works with a pool of worker threads. All configuration is stored in `config.json`; on the first run the file is created automatically with default values.

## Features

- **HTTPS without OpenSSL** - built-in TLS stack (TLS 1.2/1.3, AES-GCM, RSA/X25519/P-256, SHA-256, HMAC-SHA256), self-signed certificate support
- **HTTP/1.1** - routing, static files, gzip response compression, chunked handling
- **WebSocket** - upgrade, text/binary frames, ping/pong, close
- **Server-Sent Events (SSE)** - streaming event delivery
- **File uploads** - multipart/form-data with saving to `www/upload`
- **Forms and query parameters** - parsing of `application/x-www-form-urlencoded` and `multipart/form-data`
- **JSON API** - responses in JSON format
- **Worker thread pool** - configurable size (1-64)
- **Logging** - to console, to file, or both; DEBUG/INFO/WARN/ERROR levels; log broadcast to connected WebSocket clients
- **Request body size limit** (`max_body_mb`, 0 - unlimited)
- **`www` to bare-domain redirect** (`www_redirect`, default `true`) - requests to `www.<domain>` get a `301 Moved Permanently` to the bare domain, keeping scheme, port and URI: `http://www.localhost/...` -> `http://localhost/...`, `https://www.localhost/...` -> `https://localhost/...` (only when the bare domain is in `domains`)
- **Configurable timeouts** - IO, TLS handshake, accept, etc.
- **Cross-platform** - Windows (Winsock) and POSIX (Berkeley sockets + pthreads)

## Modules Used

| Module | Purpose |
|---|---|
| `crypery` | HTTPS library: HTTP/1.1 server (routing, static files, gzip, WebSocket, SSE, multipart), TLS/SSL stack (handshake, encryption, certificates) and an HTTP/HTTPS client |
| `utilery` | Utilities: JSON parser, logging module, configuration loading, UTF-8 codec and regular expressions |
| `zlib` | gzip compression/decompression for HTTP responses |
| `compat.h` | Portability layer: sockets, threads and synchronization primitives for Windows and POSIX |

## Compilation

A GCC compiler is required (MinGW-w64 on Windows).

### Building the servers

Two example servers are provided:

- `server-base.c` - static-site example server: serves static files from `www/` only (no API / WebSocket / SSE / upload routes).
- `server.c` - full-featured example server: static files plus JSON API, WebSocket, SSE, file upload and form/query parsing.

```bat
build_base.bat
```

The script compiles the project and produces `server-base.exe` (static-site server).

```bat
build_web.bat
```

The script compiles the project and produces `server.exe` (full-featured server).

### Building the test harness

```bat
build_test.bat
```

The script builds `web_test.exe`, a test harness that runs unit and stress tests **against a running server** (see the `Testing (web_test.c)` section). Start `server.exe` / `server-base.exe` before running `web_test.exe`.

## Usage

### Starting the server

```bat
server.exe
:: or the static-site variant
server-base.exe
```

The server reads `config.json` from the current directory. If the file does not exist, it is created with default parameters.

### Main configuration options (`config.json`)

| Option | Default | Description |
|---|---|---|
| `ssl_use` | `true` | Enable HTTPS (TLS) |
| `port` | `443` | Listening port |
| `upload_dir` | `www/upload` | Directory for uploaded files |
| `cert_file` / `key_file` | `localhost.crt` / `localhost.key` | TLS certificate and key |
| `max_clients` | `256` | Maximum number of simultaneous clients |
| `worker_threads` | `4` | Number of worker threads |
| `max_body_mb` | `0` | Maximum request body size in MB (0 - unlimited) |
| `timeout_io` / `timeout_handshake` / `timeout_tls_poll` / `timeout_flight` / `timeout_accept` | - | Timeouts in milliseconds |
| `log_file` | `server.log` | Log file |
| `log_output` | `2` | 0 - off, 1 - console+file, 2 - console, 3 - file |
| `log_level` | `3` | 0 - DEBUG, 1 - INFO, 2 - WARN, 3 - ERROR |
| `server_name` | `nginx/1.25.2` | Value of the `Server` header |
| `domains` | `{"localhost":"www","127.0.0.1":"www"}` | Allowed domains as a JSON object: `"domain": "root path"` - each domain gets its own document root |
| `www_redirect` | `true` | `true` - 301 redirect `www.<domain>` to the bare domain (scheme, port and URI are kept); `false` - serve `www.` hosts as usual |

### Endpoints

| Method | Path | Description |
|---|---|---|
| GET | `/api/info` | Server information (uptime, counters, headers) as JSON |
| GET | `/api/hello?name=...` | Personalized greeting (required `name` parameter) |
| GET | `/api/params?k=v` | Parsed query parameters as HTML |
| GET | `/ws` | WebSocket endpoint (echo, ping/pong) |
| GET | `/sse` | Server-Sent Events stream |
| POST | `/api/echo` | Returns the request body as JSON |
| POST | `/api/upload` | File upload (multipart/form-data) |
| POST | `/api/form` | Form parsing (urlencoded or multipart) |
| GET | `/*` | Static files from the request domain's document root (`server-base.exe` serves only this; API/WS/SSE rows above apply to `server.exe`) |

> `www` redirect: with `www_redirect: true` (default), a request whose `Host` starts with `www.` and whose bare domain is listed in `domains` is answered with `301 Moved Permanently` to the same scheme/port/URI on the bare domain.

> Per-domain roots: the `domains` object maps each allowed `Host` to its own document root, e.g. `"test.localhost": "test"` serves `test/` for `Host: test.localhost` while `localhost` keeps serving `www/`. Requests with a `Host` not present in `domains` are rejected with `400`. API routes are also registered per domain: the server registers the route set for every domain from `config.json` via `http_route(http, domain, method, path, handler)`.

### Examples

```bat
:: Server information
curl -k https://127.0.0.1/api/info

:: Greeting
curl -k "https://127.0.0.1/api/hello?name=World"

:: Echo
curl -k -X POST -d "hello" https://127.0.0.1/api/echo

:: File upload
curl -k -X POST -F "file=@test.txt" https://127.0.0.1/api/upload

:: Form
curl -k -X POST -d "a=1&b=2" https://127.0.0.1/api/form

:: www redirect (301 to the bare domain, scheme/port/URI are kept)
curl -k -i -H "Host: www.localhost" https://127.0.0.1/
:: -> HTTP/1.1 301 Moved Permanently, Location: https://localhost/
```

## Testing (`web_test.c`)

`web_test.c` is a standalone test harness that runs **unit** and **stress** tests against a **running** server. It can target either `server-base.exe` (static only) or `server.exe` (static + API + WS + SSE + upload), over plain HTTP or TLS. Each check prints `[PASS]`/`[FAIL]` and the status code it received; a summary and a histogram of all response codes are printed at the end.

The server must be started first, and the `base`/`web` and `http`/`https` arguments must match the server you started and its `config.json`:

```bat
:: 1. Start the server (in one terminal)
server.exe            :: or server-base.exe

:: 2. In another terminal - build and run the tests
build_test.bat
web_test.exe <base|web> <http|https> <host> <port> [unit|stress|all]

:: Examples
web_test.exe base https 127.0.0.1 443 all     :: server-base, TLS, unit + stress
web_test.exe web  https 127.0.0.1 443 unit    :: server,     TLS, unit only
web_test.exe base http  127.0.0.1 8443 stress :: server-base, plain HTTP, stress only
```

### Unit tests

- **Static files** - `GET /` and `GET /index.html` -> `200`, missing page -> `404`, `POST /` (body ignored) -> `200`
- **gzip** - creates a pre-compressed `www/index.html.gz`, checks `Content-Encoding: gzip` and the `1f 8b` magic bytes, then removes it
- **Error paths** - malformed request, `../` traversal -> `400`, blocked path (`config.json`) -> `403`, disallowed `Host` -> `400`, `www.` host -> `301` redirect, ~10 KB of headers -> `431`, 3 KB-long URI -> `414`/`404` (no crash)
- **Body limits** - `Content-Length: 300000000` -> `413` (hard cap), negative `Content-Length` handled
- **Web API** *(server only)* - `GET /api/info` (JSON), `GET /api/hello?name=` (greeting) and missing `name` -> `400`, `GET /api/params` (parsed HTML), `POST /api/echo` (JSON echo)
- **Forms and upload** *(server only)* - `POST /api/form` urlencoded and multipart; `POST /api/upload` multipart (file saved to `www/upload` and verified on disk), non-multipart upload -> `400`
- **WebSocket** *(server only)* - `GET /ws` -> `101` with a correct `Sec-WebSocket-Accept` (checked with an independent SHA-1 + base64), text-frame echo, close frame
- **SSE** *(server only)* - `GET /sse` -> `200` `text/event-stream` with `connected` + `tick` events
- **No-API check** *(server-base only)* - `GET /api/info` and `POST /api/echo` -> `404`
- **TLS robustness** *(HTTPS only)* - invalid record type / version, ChangeCipherSpec-only, TLS alert, small app-data records, mixed valid/invalid records, >16 KB record, crafted ClientHello; plaintext request on a TLS port -> `426` or a clean close (all bounded, no pinned worker)

### Stress tests

- **Rapid** - 30 sequential requests
- **Concurrent** - 8 threads x 10 requests
- **1 MB upload** - large body to `/api/upload` (server) or `/` (base)
- **Idle + abort storm** - many idle and abruptly closed connections, then a health check (the server must stay responsive)
- **Slowloris trickle** - the request is sent 1 byte every 2 s so the header phase exceeds the server deadline; the server must answer `408` and free the worker

### Output

Each test prints the status it received: `0` - connection was not established, `1` - connection established but no status received, `2xx/3xx/4xx/5xx` - the server's response code.
