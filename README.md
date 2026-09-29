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

### Building and running unit tests

```bat
build_test.bat
```

The script builds `ssl_test.exe` and runs the tests **against a running server** (default `https 127.0.0.1 443`, including stress tests). The server must be started before running `build_test.bat`.

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
| `root` | `www` | Root directory for static files |
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
| `domains` | `localhost,127.0.0.1` | Allowed domains |
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
| GET | `/*` | Static files from the `www/` directory (`server-base.exe` serves only this; API/WS/SSE rows above apply to `server.exe`) |

> `www` redirect: with `www_redirect: true` (default), a request whose `Host` starts with `www.` and whose bare domain is listed in `domains` is answered with `301 Moved Permanently` to the same scheme/port/URI on the bare domain.

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

## Unit Tests

The tests run against a started server:

```bat
:: 1. Start the server
server.exe

:: 2. In another terminal - build and run the tests
build_test.bat
```

Available `ssl_test.exe` run modes:

```bat
ssl_test.exe https 127.0.0.1 443:: TLS unit tests
ssl_test.exe http 127.0.0.1 8443:: HTTP unit tests
ssl_test.exe https 127.0.0.1 443 stress:: + stress tests (parallel requests, connection storm)
```

Each test prints the HTTP status it received: `0` - connection was not established, `1` - connection established but no status received, `2xx/3xx/4xx/5xx` - the server's response code.
