# Annotated hexdump — one complete request and response

**Saniya Sanjiv Patil** · 24bcs10246

Captured with:

```
$ ./bserve ./www 9000 &
$ ./bcurl -v localhost:9000/index.html
```

The raw `-v` output is in [`docs/capture.txt`](docs/capture.txt). Every byte that crossed the wire is accounted for below. (`www/index.html` had its mtime set to 2026-10-06 09:00:00 UTC so the capture is reproducible.)

---

## Client → server (67 bytes)

### Connection preface (8 bytes, once per connection)

```
42 48 54 50 2f 31 0d 0a      "BHTP/1\r\n"   — SPEC §1; anything else and the server closes
```

### HEADERS frame — the request (9 + 50 bytes)

Frame header:

```
00 00 32         Length    = 0x000032 = 50 payload bytes
01               Type      = 0x1 HEADERS
01               Flags     = 0x01 END_STREAM  (no request body follows)
00 00 00 01      R = 0, Stream ID = 1          (first request: 1, then 3, 5, …)
```

Header block (50 bytes). Every name here is in the static table, so no name strings are sent:

```
offset  bytes                                    meaning
0x00    01                                       index 1  = :method
0x01    00 03                                    value_len = 3
0x03    47 45 54                                 "GET"
0x06    02                                       index 2  = :path
0x07    00 0b                                    value_len = 11
0x09    2f 69 6e 64 65 78 2e 68 74 6d 6c         "/index.html"
0x14    04                                       index 4  = host
0x15    00 09                                    value_len = 9
0x17    6c 6f 63 61 6c 68 6f 73 74               "localhost"
0x20    05                                       index 5  = user-agent
0x21    00 09                                    value_len = 9
0x23    62 63 75 72 6c 2f 31 2e 30               "bcurl/1.0"
0x2c    06                                       index 6  = accept
0x2d    00 03                                    value_len = 3
0x2f    2a 2f 2a                                 "*/*"
0x32    — end (6 + 14 + 12 + 12 + 6 = 50 = Length ✓)
```

---

## Server → client (277 bytes)

### HEADERS frame — the response head (9 + 84 bytes)

Frame header:

```
00 00 54         Length    = 0x54 = 84 payload bytes
01               Type      = 0x1 HEADERS
00               Flags     = 0  (no END_STREAM: a body follows)
00 00 00 01      Stream ID = 1  (answers request 1)
```

Header block (84 bytes):

```
offset  bytes                                    meaning
0x00    03                                       index 3  = :status
0x01    00 03                                    value_len = 3
0x03    32 30 30                                 "200"
0x06    07                                       index 7  = content-type
0x07    00 18                                    value_len = 24
0x09    74 65 78 74 2f 68 74 6d 6c 3b 20 63      "text/html; charset=utf-8"
        68 61 72 73 65 74 3d 75 74 66 2d 38
0x21    08                                       index 8  = content-length
0x22    00 03                                    value_len = 3
0x24    31 37 35                                 "175"
0x27    09                                       index 9  = server
0x28    00 0a                                    value_len = 10
0x2a    62 73 65 72 76 65 2f 31 2e 30            "bserve/1.0"
0x34    0a                                       index 10 = last-modified
0x35    00 1d                                    value_len = 29
0x37    54 75 65 2c 20 30 36 20 4f 63 74 20      "Tue, 06 Oct 2026 09:00:00 GMT"
        32 30 32 36 20 30 39 3a 30 30 3a 30
        30 20 47 4d 54
0x54    — end (6 + 27 + 6 + 13 + 32 = 84 = Length ✓)
```

### DATA frame — the body (9 + 175 bytes)

Frame header:

```
00 00 af         Length    = 0xaf = 175 payload bytes  (matches content-length)
00               Type      = 0x0 DATA
01               Flags     = 0x01 END_STREAM  (last frame of this response)
00 00 00 01      Stream ID = 1
```

Payload: the 175 bytes of `www/index.html`, unchanged:

```
0000  3c 21 64 6f 63 74 79 70  65 20 68 74 6d 6c 3e 0a  |<!doctype html>.|
0010  3c 68 74 6d 6c 3e 0a 3c  68 65 61 64 3e 3c 74 69  |<html>.<head><ti|
0020  74 6c 65 3e 42 48 54 50  2f 31 3c 2f 74 69 74 6c  |tle>BHTP/1</titl|
0030  65 3e 3c 6c 69 6e 6b 20  72 65 6c 3d 22 73 74 79  |e><link rel="sty|
0040  6c 65 73 68 65 65 74 22  20 68 72 65 66 3d 22 2f  |lesheet" href="/|
0050  63 73 73 2f 73 74 79 6c  65 2e 63 73 73 22 3e 3c  |css/style.css"><|
0060  2f 68 65 61 64 3e 0a 3c  62 6f 64 79 3e 3c 68 31  |/head>.<body><h1|
0070  3e 48 65 6c 6c 6f 20 6f  76 65 72 20 42 48 54 50  |>Hello over BHTP|
0080  2f 31 3c 2f 68 31 3e 3c  70 3e 53 65 72 76 65 64  |/1</h1><p>Served|
0090  20 62 79 20 62 73 65 72  76 65 2e 3c 2f 70 3e 3c  | by bserve.</p><|
00a0  2f 62 6f 64 79 3e 0a 3c  2f 68 74 6d 6c 3e 0a     |/body>.</html>.|
```

END_STREAM is set, so the response for stream 1 is complete. The connection stays open: bcurl could now send stream 3 on it. Here it had nothing else to fetch, so it closed the socket; the server saw EOF and its connection loop ended.

---

## What the bytes cost

| | BHTP/1 | Equivalent HTTP/1.1 text |
|---|---|---|
| Request | 8 preface + 59 = **67 B** | `GET /index.html HTTP/1.1` + 3 headers = 86 B |
| Response head | **93 B** | status line + 5 headers = 146 B |
| Body | 175 + 9 framing | 175 |

The saving comes from the static table (no name strings) and from replacing `": "` / `"\r\n"` delimiters with length prefixes — which is also why the receiver never scans for a delimiter and always knows how many bytes to read next.
