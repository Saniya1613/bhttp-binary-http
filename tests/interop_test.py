#!/usr/bin/env python3
"""
Interop tests for BHTP/1, written ONLY from SPEC.md — no code shared with
the C implementation. If these pass, a stranger's implementation of the
spec should talk to bserve/bcurl.

    python3 tests/interop_test.py            (run from the repo root, after `make`)

Author: Saniya Sanjiv Patil (24bcs10246)
"""
import os, socket, struct, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WWW = os.path.join(ROOT, "www")
PORT = 19000 + os.getpid() % 1000

PREFACE = b"BHTP/1\r\n"
DATA, HEADERS, GOAWAY = 0x0, 0x1, 0x7
END_STREAM = 0x1
STATIC = [None, ":method", ":path", ":status", "host", "user-agent", "accept",
          "content-type", "content-length", "server", "last-modified"]

# ---------------------------------------------------------------- wire helpers
def frame(ftype, flags, stream, payload=b""):
    n = len(payload)
    return struct.pack(">BHBBI", n >> 16, n & 0xFFFF, ftype, flags, stream & 0x7FFFFFFF) + payload

def field(name, value):
    v = value.encode()
    if name in STATIC:
        return bytes([STATIC.index(name)]) + struct.pack(">H", len(v)) + v
    n = name.encode()
    return b"\x00" + bytes([len(n)]) + n + struct.pack(">H", len(v)) + v

def block(*pairs):
    return b"".join(field(k, v) for k, v in pairs)

def decode_block(b):
    out, i = [], 0
    while i < len(b):
        idx = b[i]; i += 1
        if idx == 0:
            nl = b[i]; i += 1
            name = b[i:i + nl].decode(); i += nl
        else:
            name = STATIC[idx] if idx <= 10 else None
        vl = struct.unpack(">H", b[i:i + 2])[0]; i += 2
        val = b[i:i + vl].decode(); i += vl
        if name: out.append((name, val))
    return dict(out)

def recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise EOFError
        buf += chunk
    return buf

def read_frame(s):
    h = recv_exact(s, 9)
    hi, lo, t, fl, sid = struct.unpack(">BHBBI", h)
    n = (hi << 16) | lo
    return t, fl, sid & 0x7FFFFFFF, recv_exact(s, n) if n else b""

def read_response(s, stream):
    hdrs, body = None, b""
    while True:
        t, fl, sid, p = read_frame(s)
        if sid != stream or t not in (DATA, HEADERS):
            continue
        if t == HEADERS:
            hdrs = decode_block(p)
        else:
            body += p
        if fl & END_STREAM:
            return hdrs, body

def connect():
    s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    s.sendall(PREFACE)
    return s

def get(s, stream, path, method="GET", extra=b""):
    s.sendall(frame(HEADERS, END_STREAM, stream,
                    block((":method", method), (":path", path), ("host", "localhost")) + extra))
    return read_response(s, stream)

# ---------------------------------------------------------------- tiny runner
results = []
def test(fn):
    try:
        fn(); results.append((fn.__name__, True, ""))
    except Exception as e:
        results.append((fn.__name__, False, repr(e)))
    return fn

def eq(a, b):
    if a != b:
        raise AssertionError(f"{a!r} != {b!r}")

def file(name):
    with open(os.path.join(WWW, name), "rb") as f:
        return f.read()

# ---------------------------------------------------------------- server tests
def server_tests():
    @test
    def get_200():
        s = connect()
        h, b = get(s, 1, "/index.html")
        eq(h[":status"], "200"); eq(b, file("index.html"))
        eq(h["content-length"], str(len(b)))
        eq(h["content-type"].split(";")[0], "text/html")

    @test
    def root_maps_to_index():
        h, b = get(connect(), 1, "/")
        eq(h[":status"], "200"); eq(b, file("index.html"))

    @test
    def keep_alive_three_requests_one_socket():
        s = connect()
        for sid, path in ((1, "/index.html"), (3, "/css/style.css"), (5, "/hello.txt")):
            h, b = get(s, sid, path)
            eq(h[":status"], "200"); eq(b, file(path.lstrip("/")))

    @test
    def big_file_multiple_data_frames():
        h, b = get(connect(), 1, "/big.bin")
        eq(h[":status"], "200"); eq(b, file("big.bin"))

    @test
    def not_found_404():
        h, b = get(connect(), 1, "/does-not-exist.html")
        eq(h[":status"], "404")

    @test
    def traversal_is_404():
        h, _ = get(connect(), 1, "/../../etc/passwd")
        eq(h[":status"], "404")

    @test
    def head_has_no_body():
        h, b = get(connect(), 1, "/index.html", method="HEAD")
        eq(h[":status"], "200"); eq(b, b""); eq(h["content-length"], str(len(file("index.html"))))

    @test
    def post_is_405():
        h, _ = get(connect(), 1, "/index.html", method="POST")
        eq(h[":status"], "405")

    @test
    def truncated_block_is_400_and_connection_survives():
        s = connect()
        bad = block((":method", "GET")) + b"\x02\x00\x50/ind"   # value_len says 80, only 4 bytes
        s.sendall(frame(HEADERS, END_STREAM, 1, bad))
        h, _ = read_response(s, 1)
        eq(h[":status"], "400")
        h, _ = get(s, 3, "/hello.txt")                           # still usable
        eq(h[":status"], "200")

    @test
    def missing_path_is_400():
        s = connect()
        s.sendall(frame(HEADERS, END_STREAM, 1, block((":method", "GET"))))
        h, _ = read_response(s, 1)
        eq(h[":status"], "400")

    @test
    def oversized_headers_is_400_and_connection_survives():
        s = connect()
        s.sendall(frame(HEADERS, END_STREAM, 1,
                        block((":method", "GET"), (":path", "/"), ("x-pad", "a" * 20000))))
        h, _ = read_response(s, 1)
        eq(h[":status"], "400")
        h, _ = get(s, 3, "/hello.txt")
        eq(h[":status"], "200")

    @test
    def unknown_frame_type_is_skipped():
        s = connect()
        s.sendall(frame(0x42, 0xFF, 1, b"version-2 stuff " * 100))  # type nobody knows
        s.sendall(frame(0xFE, 0, 0, b""))                            # empty one too
        h, b = get(s, 1, "/hello.txt")
        eq(h[":status"], "200"); eq(b, b"hello\n")

    @test
    def unknown_header_index_is_skipped():
        extra = bytes([200]) + struct.pack(">H", 5) + b"v2!!!"
        h, b = get(connect(), 1, "/hello.txt", extra=extra)
        eq(h[":status"], "200"); eq(b, b"hello\n")

    @test
    def reserved_bit_ignored():
        s = connect()
        p = block((":method", "GET"), (":path", "/hello.txt"))
        raw = bytearray(frame(HEADERS, END_STREAM, 7, p)); raw[5] |= 0x80   # set R
        s.sendall(bytes(raw))
        h, _ = read_response(s, 7)
        eq(h[":status"], "200")

    @test
    def literal_header_names_accepted():
        h, _ = get(connect(), 1, "/hello.txt", extra=field("x-student", "24bcs10246"))
        eq(h[":status"], "200")

    @test
    def bad_preface_closes():
        s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
        s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
        try:
            eq(s.recv(100), b"")          # orderly close ...
        except ConnectionResetError:
            pass                          # ... or RST (unread bytes) — both are "closed"

# ---------------------------------------------------------------- client tests
# A deliberately weird mock server: it sends unknown frames and unknown header
# indices and splits the body, to check bcurl follows the skip rules.
def mock_server(port, stop):
    ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", port)); ls.listen(8); ls.settimeout(0.2)
    conns = []
    while not stop.is_set():
        try:
            c, _ = ls.accept()
        except socket.timeout:
            continue
        conns.append(1)
        c.settimeout(5)
        try:
            assert recv_exact(c, 8) == PREFACE
            while True:
                t, fl, sid, p = read_frame(c)
                if t != HEADERS:
                    continue
                path = decode_block(p)[":path"]
                c.sendall(frame(0x33, 0, sid, b"ignore me"))           # unknown type
                if path == "/missing":
                    c.sendall(frame(HEADERS, 0, sid, block((":status", "404"))))
                    c.sendall(frame(DATA, END_STREAM, sid, b"nope\n"))
                    continue
                hdr = block((":status", "200"), ("content-type", "text/plain"))
                hdr += bytes([99]) + struct.pack(">H", 3) + b"???"    # unknown index
                c.sendall(frame(HEADERS, 0, sid, hdr))
                c.sendall(frame(DATA, 0, sid, b"part1-"))
                c.sendall(frame(0x99, 0, 0, b"\x00" * 50))            # unknown, stream 0
                c.sendall(frame(DATA, END_STREAM, sid, b"part2\n"))
        except (EOFError, OSError, AssertionError):
            pass
        c.close()
    ls.close()
    mock_server.connections = len(conns)

def bcurl(*args):
    return subprocess.run([os.path.join(ROOT, "bcurl"), *args], capture_output=True, timeout=10)

def client_tests():
    mport = PORT + 1
    stop = threading.Event()
    th = threading.Thread(target=mock_server, args=(mport, stop), daemon=True); th.start()
    time.sleep(0.3)

    @test
    def bcurl_skips_unknown_frames_and_indices():
        r = bcurl(f"127.0.0.1:{mport}/x")
        eq(r.returncode, 0); eq(r.stdout, b"part1-part2\n")

    @test
    def bcurl_exit_nonzero_on_404():
        r = bcurl(f"127.0.0.1:{mport}/missing")
        eq(r.returncode, 22); eq(r.stdout, b"nope\n")

    @test
    def bcurl_many_paths_one_connection():
        before = len_conns()
        r = bcurl(f"127.0.0.1:{mport}/a", "/b", "/c")
        eq(r.returncode, 0); eq(r.stdout, b"part1-part2\n" * 3)
        time.sleep(0.3)
        eq(len_conns() - before, 1)

    @test
    def bcurl_against_bserve_verbose():
        r = bcurl("-v", f"127.0.0.1:{PORT}/hello.txt")
        eq(r.returncode, 0); eq(r.stdout, b"hello\n")
        assert b"HEADERS frame" in r.stderr and b"DATA frame" in r.stderr

    stop.set(); th.join()

_conn_count = [0]
def len_conns():
    return _conn_count[0]

# count accepted connections without touching mock internals
_orig_accept = socket.socket.accept
def _counting_accept(self):
    c = _orig_accept(self)
    if self.getsockname()[1] == PORT + 1:
        _conn_count[0] += 1
    return c
socket.socket.accept = _counting_accept

# ---------------------------------------------------------------- main
if __name__ == "__main__":
    big = os.path.join(WWW, "big.bin")            # 50 000 bytes -> several DATA frames
    if not os.path.exists(big):
        with open(big, "wb") as f:
            f.write(bytes((i * 31 + 7) % 256 for i in range(50000)))
    srv = subprocess.Popen([os.path.join(ROOT, "bserve"), WWW, str(PORT)],
                           stderr=subprocess.DEVNULL)
    time.sleep(0.4)
    try:
        server_tests()
        client_tests()
    finally:
        srv.terminate()
    width = max(len(n) for n, _, _ in results)
    for name, ok, err in results:
        print(f"  {'PASS' if ok else 'FAIL'}  {name.ljust(width)}  {err}")
    failed = sum(1 for _, ok, _ in results if not ok)
    print(f"\n{len(results) - failed}/{len(results)} passed")
    sys.exit(1 if failed else 0)
