# BHTP/1 — HTTP semantics in a binary frame

**Saniya Sanjiv Patil** · 24bcs10246 · Network Architecture course project

BHTP/1 carries an HTTP-style request and response (method, path, status, headers, body) over one TCP connection as length-prefixed binary frames. All integers are unsigned, **big-endian** (network byte order). "MUST", "SHOULD" and "MAY" are as in RFC 2119.

## 1. Connection

1. The client opens **one** TCP connection and first sends the 8-byte preface `42 48 54 50 2F 31 0D 0A` (ASCII `BHTP/1\r\n`).
2. A server that receives any other 8 bytes MUST close the connection without replying. (A stray HTTP/1.1 client, or a future `BHTP/2` client, is thus refused cleanly instead of being misparsed as frames.)
3. After the preface both sides exchange frames only. The connection stays open after a response; the client MAY send further requests on it. Either side MAY close it at any time; a client SHOULD close once it has no more requests.
4. A client MUST NOT open a second connection to the same server while one is open.

## 2. Frame header (9 bytes, fixed)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-----------------------------------------------+---------------+
|                 Length (24)                   |   Type (8)    |
+---------------+-+-------------------------------------------+-+
|   Flags (8)   |R|               Stream ID (31)                |
+---------------+-+---------------------------------------------+
|                   Payload (Length bytes) ...                  |
```

| Field | Bits | Meaning |
|---|---|---|
| Length | 24 | Payload bytes after the 9-byte header (0 … 16 777 215). |
| Type | 8 | Frame type, §3. |
| Flags | 8 | Per-type bit flags. Unknown flags MUST be ignored and SHOULD be sent as 0. |
| R | 1 | Reserved. Sent as 0, MUST be ignored on receipt. |
| Stream ID | 31 | Which request/response this frame belongs to. 0 = the connection itself. |

**Why these widths (and why HTTP/2 chose 24/8/8/31).**
*Length 24:* the receiver can always size a buffer before reading, and 16 MiB is a hard upper bound on that buffer. 32 bits would let one 4 GiB frame monopolise the connection; 16 bits (64 KiB) would chop large files into many small frames for no gain. Bodies larger than one frame are simply split into several DATA frames. *Type 8 / Flags 8:* one byte each keeps the header byte-aligned with no bit-shifting, gives 256 types — far more than v1 uses, which is the room v2 grows into — and 8 independent booleans per type. *Stream 31 + R:* every response names the request it answers, so a v2 can interleave several requests on the same connection without changing the header. 31 rather than 32 bits keeps the value positive in languages with only signed 32-bit ints (Java), and the spare bit is reserved for future use. Total = 9 bytes: under 0.06 % overhead on a 16 KiB DATA frame.

## 3. Frame types

| Type | Name | Payload | Flags |
|---|---|---|---|
| `0x0` | DATA | Body bytes (any length, may be 0). | END_STREAM `0x01` |
| `0x1` | HEADERS | A header block, §4. | END_STREAM `0x01` |
| `0x7` | GOAWAY | Empty. Sender is closing the connection. Stream ID MUST be 0. | — |
| others | — | **A receiver meeting a frame type it does not know MUST skip it cleanly:** read and discard exactly `Length` payload bytes, then carry on with the next frame as if it had not arrived. It MUST NOT close the connection or send an error because of it. | — |

END_STREAM means "this is the sender's last frame for this stream".

## 4. Header block (payload of HEADERS)

A sequence of fields, back to back, until the payload ends. There is no count; `Length` bounds the block.

```
field := index (1 byte)
         [ name_len (1 byte)  name (name_len bytes) ]   -- only if index == 0
         value_len (2 bytes)  value (value_len bytes)
```

* **index 1–10** — the name is from the static table below (no name bytes on the wire).
* **index 0** — literal name follows: 1–255 bytes of lowercase visible ASCII (`0x21–0x7E`, no `A–Z`).
* **index 11–255** — reserved for future static-table entries. The receiver MUST skip the field (its layout is the same as indices 1–10, so it can).

Values are 0–65 535 bytes of UTF-8 and MUST NOT contain `0x00`. Names beginning with `:` are pseudo-headers and MUST come from the table.

| # | Name | # | Name |
|---|---|---|---|
| 1 | `:method` | 6 | `accept` |
| 2 | `:path` | 7 | `content-type` |
| 3 | `:status` | 8 | `content-length` |
| 4 | `host` | 9 | `server` |
| 5 | `user-agent` | 10 | `last-modified` |

These are exactly the ten names a bcurl/bserve exchange sends, so a normal exchange carries no name strings at all — HPACK's first two mechanisms (static table + length-prefixed literals), without Huffman or the dynamic table.

## 5. Request and response

**Request.** The client picks a new Stream ID — odd, non-zero, larger than any it used before on this connection (1, 3, 5, …) — and sends one HEADERS frame with END_STREAM set, containing `:method` (`GET` or `HEAD`) and `:path` (MUST start with `/`), and SHOULD contain `host`. v1 requests have no body; a server MUST ignore DATA frames from a client.

**Response.** The server answers on the **same Stream ID**: one HEADERS frame containing `:status` (3 ASCII digits) and SHOULD contain `content-type` and `content-length`, then zero or more DATA frames. The last frame of the response carries END_STREAM — the HEADERS frame itself if there is no body (`HEAD`, empty file). The response to one request completes before the server reads the next one in v1.

**Path mapping.** `:path` (with any `?query` or `#fragment` removed) is appended to the server's root directory; a path ending in `/`, or naming a directory, means `index.html` inside it. A path with a `..` segment, or one that resolves outside the root, MUST NOT be served.

| Status | When |
|---|---|
| 200 | File found; body is its bytes. |
| 400 | Malformed request: header block does not parse (a length runs past the payload, bad literal name, `0x00` in a value), `:method`/`:path` missing, `:path` not starting with `/`, Stream ID 0, or HEADERS larger than the server's limit (bserve: 16 384 bytes). |
| 404 | No such file, or path outside the root. |
| 405 | `:method` other than `GET`/`HEAD`. |

Because every frame is length-prefixed, a malformed *request* never desynchronises the connection: the server replies 400 on that stream and keeps reading frames. Only EOF, a bad preface, or GOAWAY ends the connection.

## 6. Client behaviour (bcurl)

Sends the preface and requests as above; writes DATA payloads of the response to stdout; with `-v`, prints every frame sent and received as a hexdump on stderr. Ignores frames for other streams and skips unknown types. Exits **0** if every status was below 400, **22** if any was 4xx/5xx, **7** if it could not connect, **8** if the connection broke or the response was malformed.

## 7. Room for version 2

v2 can add frame types (e.g. PING, a body-carrying POST, flow control), flags, and static-table entries 11–255, and can multiplex streams — and a v1 peer still works, because it skips what it does not know. A change to the header layout itself needs a new preface (`BHTP/2\r\n`), which a v1 server refuses by closing.
