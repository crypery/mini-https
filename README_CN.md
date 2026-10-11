# 迷你 HTTPS 服务器

> [English version](README.md)

> [Russian version](README_RU.md)

一个自包含的 C 语言迷你 HTTPS 服务器，基于 **crypery HTTPS Library** 构建。无需 OpenSSL 或其他外部依赖：TLS 协议栈、HTTPS 服务器和所有工具库都集成在同一个项目中。

## 简介

服务器通过 TCP 或 TLS（HTTPS）接收 HTTP/1.1 请求，从 `www/` 目录提供静态文件，提供 JSON API、WebSocket 和 Server-Sent Events（SSE），支持文件上传（multipart），并使用工作线程池处理请求。所有配置都保存在 `config.json` 中；首次运行时会自动创建该文件并填入默认值。

## 特性

- **无需 OpenSSL 的 HTTPS** - 内置 TLS 协议栈（TLS 1.2/1.3、AES-GCM、RSA/X25519/P-256、SHA-256、HMAC-SHA256），支持自签名证书
- **HTTP/1.1** - 路由、静态文件、gzip 响应压缩、chunked 处理
- **WebSocket** - upgrade、文本/二进制帧、ping/pong、close
- **Server-Sent Events（SSE）** - 事件流式推送
- **文件上传** - multipart/form-data，文件保存到 `www/upload`
- **表单与查询参数** - 解析 `application/x-www-form-urlencoded` 和 `multipart/form-data`
- **JSON API** - 以 JSON 格式返回响应
- **工作线程池** - 数量可配置（1-64）
- **日志** - 输出到控制台、文件或两者兼有；DEBUG/INFO/WARN/ERROR 级别；向已连接的 WebSocket 客户端广播日志
- **请求体大小限制**（`max_body_mb`，0 - 不限制）
- **`www` 到主域名的重定向**（`www_redirect`，默认为 `true`）- 对 `www.<域名>` 的请求返回 `301 Moved Permanently` 到主域名，保持 scheme、端口和 URI：`http://www.localhost/...` -> `http://localhost/...`，`https://www.localhost/...` -> `https://localhost/...`（仅当主域名在 `domains` 中时）
- **可配置超时** - IO、TLS 握手、accept 等
- **跨平台** - Windows（Winsock）和 POSIX（Berkeley sockets + pthreads）

## 使用的模块

| 模块 | 用途 |
|---|---|
| `crypery` | HTTPS 库：HTTP/1.1 服务器（路由、静态文件、gzip、WebSocket、SSE、multipart）、TLS/SSL 协议栈（握手、加密、证书）以及 HTTP/HTTPS 客户端 |
| `utilery` | 工具库：JSON 解析器、日志模块、配置加载、UTF-8 编解码和正则表达式 |
| `zlib` | HTTP 响应的 gzip 压缩/解压缩 |
| `compat.h` | 可移植性层：Windows 和 POSIX 的套接字、线程和同步原语 |

## 编译

需要 GCC 编译器（Windows 上使用 MinGW-w64）。

### 构建服务器

提供两个示例服务器：

- `server-base.c` - 静态网站示例服务器：仅从 `www/` 提供静态文件（无 API / WebSocket / SSE / 文件上传路由）。
- `server.c` - 全功能示例服务器：静态文件 + JSON API、WebSocket、SSE、文件上传和表单/查询参数解析。

```bat
build_base.bat
```

该脚本编译项目并生成 `server-base.exe`（静态网站服务器）。

```bat
build_web.bat
```

该脚本编译项目并生成 `server.exe`（全功能服务器）。

### 构建测试工具

```bat
build_test.bat
```

该脚本构建 `web_test.exe`，一个**针对正在运行的服务器**执行单元测试和压力测试的测试工具（见 `测试 (web_test.c)` 一节）。运行 `web_test.exe` 之前必须先启动 `server.exe` / `server-base.exe`。

## 使用

### 启动服务器

```bat
server.exe
:: 或静态网站版本
server-base.exe
```

服务器从当前目录读取 `config.json`。如果文件不存在，将使用默认参数自动创建。

### 主要配置项（`config.json`）

| 参数 | 默认值 | 说明 |
|---|---|---|
| `ssl_use` | `true` | 启用 HTTPS（TLS） |
| `port` | `443` | 监听端口 |
| `root` | `www` | 静态文件根目录 |
| `upload_dir` | `www/upload` | 上传文件保存目录 |
| `cert_file` / `key_file` | `localhost.crt` / `localhost.key` | TLS 证书和密钥 |
| `max_clients` | `256` | 最大并发客户端数 |
| `worker_threads` | `4` | 工作线程数 |
| `max_body_mb` | `0` | 请求体最大大小（MB，0 - 不限制） |
| `timeout_io` / `timeout_handshake` / `timeout_tls_poll` / `timeout_flight` / `timeout_accept` | - | 超时时间（毫秒） |
| `log_file` | `server.log` | 日志文件 |
| `log_output` | `2` | 0 - 关闭，1 - 控制台+文件，2 - 控制台，3 - 文件 |
| `log_level` | `3` | 0 - DEBUG，1 - INFO，2 - WARN，3 - ERROR |
| `server_name` | `nginx/1.25.2` | `Server` 响应头的值 |
| `domains` | `localhost,127.0.0.1` | 允许的域名 |
| `www_redirect` | `true` | `true` - 将 `www.<域名>` 301 重定向到主域名（保持 scheme、端口和 URI）；`false` - 按常规方式处理 `www.` 主机 |

### 接口端点

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/info` | 服务器信息（运行时间、计数器、请求头），JSON 格式 |
| GET | `/api/hello?name=...` | 个性化问候（必需参数 `name`） |
| GET | `/api/params?k=v` | 以 HTML 显示解析后的查询参数 |
| GET | `/ws` | WebSocket 端点（echo、ping/pong） |
| GET | `/sse` | Server-Sent Events 事件流 |
| POST | `/api/echo` | 以 JSON 返回请求体 |
| POST | `/api/upload` | 文件上传（multipart/form-data） |
| POST | `/api/form` | 表单解析（urlencoded 或 multipart） |
| GET | `/*` | `www/` 目录中的静态文件（`server-base.exe` 仅提供此项；以上 API/WS/SSE 适用于 `server.exe`） |

> `www` 重定向：当 `www_redirect: true`（默认）时，若请求的 `Host` 以 `www.` 开头且主域名在 `domains` 列表中，服务器返回 `301 Moved Permanently`，重定向到主域名的相同 scheme/端口/URI。

### 示例

```bat
:: 服务器信息
curl -k https://127.0.0.1/api/info

:: 问候
curl -k "https://127.0.0.1/api/hello?name=World"

:: Echo
curl -k -X POST -d "hello" https://127.0.0.1/api/echo

:: 文件上传
curl -k -X POST -F "file=@test.txt" https://127.0.0.1/api/upload

:: 表单
curl -k -X POST -d "a=1&b=2" https://127.0.0.1/api/form

:: www 重定向（301 到主域名，保持 scheme/端口/URI）
curl -k -i -H "Host: www.localhost" https://127.0.0.1/
:: -> HTTP/1.1 301 Moved Permanently, Location: https://localhost/
```

## 测试（`web_test.c`）

`web_test.c` 是一个独立的测试工具，针对**正在运行的**服务器执行**单元测试**和**压力测试**。它可以针对 `server-base.exe`（仅静态文件）或 `server.exe`（静态文件 + API + WS + SSE + 上传），通过纯 HTTP 或 TLS 运行。每个检查项都会打印 `[PASS]`/`[FAIL]` 以及收到的状态码；最后会打印汇总和所有响应码的直方图。

必须先启动服务器，并且 `base`/`web` 与 `http`/`https` 参数要与你启动的服务器及其 `config.json` 相匹配：

```bat
:: 1. 启动服务器（在一个终端中）
server.exe            :: 或 server-base.exe

:: 2. 在另一个终端中构建并运行测试
build_test.bat
web_test.exe <base|web> <http|https> <host> <port> [unit|stress|all]

:: 示例
web_test.exe base https 127.0.0.1 443 all     :: server-base，TLS，单元测试 + 压力测试
web_test.exe web  https 127.0.0.1 443 unit    :: server，    TLS，仅单元测试
web_test.exe base http  127.0.0.1 8443 stress :: server-base，纯 HTTP，仅压力测试
```

### 单元测试

- **静态文件** - `GET /` 和 `GET /index.html` -> `200`，不存在的页面 -> `404`，`POST /`（忽略请求体）-> `200`
- **gzip** - 创建预压缩的 `www/index.html.gz`，检查 `Content-Encoding: gzip` 和 `1f 8b` 魔数，然后删除该文件
- **错误路径** - 畸形请求、`../` 路径穿越 -> `400`、被阻止的路径（`config.json`）-> `403`、不允许的 `Host` -> `400`、`www.` 主机 -> `301` 重定向、约 10 KB 的请求头 -> `431`、3 KB 长的 URI -> `414`/`404`（不崩溃）
- **请求体大小限制** - `Content-Length: 300000000` -> `413`（硬性上限），处理负的 `Content-Length`
- **Web API**（仅 server）- `GET /api/info`（JSON）、`GET /api/hello?name=`（问候语）以及缺少 `name` -> `400`、`GET /api/params`（解析后的 HTML）、`POST /api/echo`（JSON 回显）
- **表单与上传**（仅 server）- `POST /api/form` urlencoded 和 multipart；`POST /api/upload` multipart（文件保存到 `www/upload` 并在磁盘上校验），非 multipart 上传 -> `400`
- **WebSocket**（仅 server）- `GET /ws` -> `101` 且 `Sec-WebSocket-Accept` 正确（用独立的 SHA-1 + base64 校验）、文本帧回显、关闭帧
- **SSE**（仅 server）- `GET /sse` -> `200` `text/event-stream`，包含 `connected` + `tick` 事件
- **无 API 检查**（仅 server-base）- `GET /api/info` 和 `POST /api/echo` -> `404`
- **TLS 健壮性**（仅 HTTPS）- 非法记录类型/版本、仅 ChangeCipherSpec、TLS alert、小的应用数据记录、合法/非法记录混合、>16 KB 记录、构造的 ClientHello；在 TLS 端口发送明文请求 -> `426` 或干净地关闭连接（全部有界，不占用工作线程）

### 压力测试

- **快速** - 30 个顺序请求
- **并发** - 8 个线程 x 10 个请求
- **1 MB 上传** - 大请求体，发往 `/api/upload`（server）或 `/`（base）
- **空闲 + 中断风暴** - 大量空闲连接和突然关闭的连接，随后进行健康检查（服务器必须保持响应）
- **Slowloris 滴漏** - 每 2 秒发送 1 个字节，使请求头阶段超过服务器超时；服务器必须返回 `408` 并释放工作线程

### 输出

每个测试都会打印收到的状态码：`0` - 连接未建立，`1` - 连接已建立但未收到状态码，`2xx/3xx/4xx/5xx` - 服务器响应码。
