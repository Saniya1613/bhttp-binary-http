# Annotated hexdump

Saniya Sanjiv Patil (24bcs10246)

This walks through every byte of one full request and response. I started the server with `./bserve ./www 9000` and fetched the home page with `./bcurl -v localhost:9000/index.html`. The raw output from `-v` is saved in [docs/capture.txt](docs/capture.txt).

Altogether the client sent 67 bytes and the server sent back 277.

## What the client sends

### The opening bytes

The client starts with `42 48 54 50 2f 31 0d 0a`. In ASCII that reads `BHTP/1` followed by a carriage return and a newline. It's sent once per connection, and it tells the server that this client speaks version 1 of the protocol.

### The request frame

Next comes the frame header: `00 00 32 01 01 00 00 00 01`.

The first three bytes, `00 00 32`, are the length. `0x32` is 50, so 50 bytes of payload follow. The next byte, `01`, is the type, which means HEADERS. After that, `01` is the flags byte with END_STREAM set, because a request has no body. The last four bytes, `00 00 00 01`, give stream ID 1, since this is the first request on the connection.

Then come the 50 bytes of headers. Every name used here has a number, so none of them are spelled out.

`01 00 03 47 45 54` is `:method` (number 1) with a 3-byte value, `GET`.

`02 00 0b` then `2f 69 6e 64 65 78 2e 68 74 6d 6c` is `:path` (number 2) with an 11-byte value, `/index.html`.

`04 00 09` then `6c 6f 63 61 6c 68 6f 73 74` is `host` (number 4) with a 9-byte value, `localhost`.

`05 00 09` then `62 63 75 72 6c 2f 31 2e 30` is `user-agent` (number 5) with a 9-byte value, `bcurl/1.0`.

`06 00 03 2a 2f 2a` is `accept` (number 6) with a 3-byte value, `*/*`.

Adding those up gives 6 + 14 + 12 + 12 + 6 = 50 bytes, which matches the length in the frame header.

## What the server sends back

### The response headers

The server's first frame header is `00 00 54 01 00 00 00 00 01`.

The length is `0x54`, which is 84 bytes. The type is `01`, HEADERS again. This time the flags byte is `00`, because the body is still to come. The stream ID is 1, which shows this is the answer to request 1.

The 84 bytes of headers are:

`03 00 03 32 30 30` is `:status` (number 3) with the value `200`.

`07 00 18` then 24 bytes is `content-type` (number 7) with the value `text/html; charset=utf-8`.

`08 00 03 31 37 35` is `content-length` (number 8) with the value `175`.

`09 00 0a` then 10 bytes is `server` (number 9) with the value `bserve/1.0`.

`0a 00 1d` then 29 bytes is `last-modified` (number 10) with the value `Tue, 06 Oct 2026 09:00:00 GMT`.

That's 6 + 27 + 6 + 13 + 32 = 84 bytes, matching the length.

### The body

The last frame header is `00 00 af 00 01 00 00 00 01`.

The length is `0xaf`, which is 175 bytes, the same number the server promised in `content-length`. The type is `00`, which means DATA. The flags byte is `01`, END_STREAM, because this is the last frame of the response. The stream ID is 1 again.

The 175 bytes that follow are the contents of `www/index.html`, sent exactly as they are on disk. They begin with `3c 21 64 6f 63 74 79 70 65`, which is `<!doctype`, and end with `3c 2f 68 74 6d 6c 3e 0a`, which is `</html>` and a newline. The full dump is in [docs/capture.txt](docs/capture.txt).

## After the response

Because END_STREAM is set, the client knows the response to stream 1 is complete. The connection is still open at this point, and the client could send a second request on stream 3. In this run it had nothing else to fetch, so it closed the connection.

## How much space it saves

The same request written as ordinary HTTP/1.1 text takes 81 bytes. In BHTP/1 it takes 67, even counting the 8 opening bytes. The response headers shrink from 146 bytes to 93.

Most of the saving comes from sending a number in place of each header name. The rest comes from replacing the colons, spaces and line breaks with length bytes. Those length bytes also mean the receiver always knows exactly how much to read next, so it never has to scan through text looking for the end of a line.
