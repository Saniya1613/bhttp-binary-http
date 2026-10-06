# BHTP/1 — HTTP, in binary

Network Architecture course project · **Saniya Sanjiv Patil** · 24bcs10246

Two tracks, one protocol: a server (`bserve`) and a client (`bcurl`) that talk only through the spec.

| Deliverable | File |
|---|---|
| 1. The spec (two pages) | [`SPEC.md`](SPEC.md) |
| 2. The program | [`src/`](src) — `bserve.c`, `bcurl.c`, shared `proto.c` |
| 3. Annotated hexdump of one request + response | [`HEXDUMP.md`](HEXDUMP.md) |

## Build

```sh
make            # needs a C compiler; Linux or macOS
```

## Run

```sh
./bserve ./www 9000                      # Track 1 — serve ./www on port 9000
./bcurl -v localhost:9000/index.html     # Track 2 — body to stdout, frames hexdumped to stderr
```

`bcurl` options:

- `-v` hexdump every frame sent and received (DATA payloads cut at 256 bytes); `-vv` dumps them in full
- `-I` send `HEAD` instead of `GET`
- extra paths are fetched on the **same** connection: `./bcurl localhost:9000/ /css/style.css /missing`
- exit code: `0` ok, `22` if any response was 4xx/5xx, `7` cannot connect, `8` protocol error

`bserve -v ./www 9000` hexdumps every frame on the server side too.

## Protocol in one glance

```
preface  "BHTP/1\r\n"                         (client, once)
frame    Length:24 | Type:8 | Flags:8 | R:1 Stream:31 | payload
types    0x0 DATA · 0x1 HEADERS · 0x7 GOAWAY · anything else → skip Length bytes
headers  index:8 [name_len:8 name] value_len:16 value   (index 1–10 = static table)
```

See [`SPEC.md`](SPEC.md) for the full rules and the reasoning behind the field widths.

## Test

```sh
make test
```

`tests/interop_test.py` is written only from the spec (no shared code). It drives `bserve` with hand-built frames — 200, 404, 400, 405, keep-alive, path traversal, oversized headers, unknown frame types, unknown header indices, the reserved bit, a bad preface — and runs `bcurl` against a deliberately odd mock server that injects unknown frames, to check the client skips them and never opens a second connection.
