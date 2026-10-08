# BHTP/1 Specification

Saniya Sanjiv Patil (24bcs10246), Network Architecture project

BHTP/1 is a small version of HTTP that sends binary frames instead of lines of text. A client asks for a file, the server sends it back, and both of them use a single TCP connection to do it.

Every number in this protocol is unsigned and big-endian, which means the most important byte comes first.

## 1. Opening the connection

The client opens one TCP connection to the server. Before anything else, it sends eight fixed bytes: `42 48 54 50 2F 31 0D 0A`, which spell out `BHTP/1\r\n`.

If the server receives anything else as its first eight bytes, it closes the connection straight away. That way, a normal web browser or any other program that wanders in is turned away, instead of having its text misread as frames.

After those eight bytes, the two sides only ever send frames. The connection stays open after each response, so the client can keep sending requests on it. The client must never open a second connection to the same server.

## 2. The frame header

Every frame starts with a 9-byte header, and the payload follows right after it.

The first three bytes are the **length**: how many payload bytes come after the header. The fourth byte is the **type**, which says what kind of frame this is. The fifth byte holds the **flags**, a set of on/off switches. The last four bytes are the **stream ID**, which says which request the frame belongs to.

The very first bit of the stream ID is reserved. Senders always set it to 0, and receivers ignore it. Receivers also ignore any flag they don't recognise.

### Why I picked these sizes

I used the same layout as HTTP/2, which splits the header into 24, 8, 8 and 31 bits. Here is why each size makes sense.

The length is 24 bits, so a single frame can hold up to about 16 MB. That gives the receiver a hard limit on how much memory one frame can ever need. With 32 bits, one sender could push a 4 GB frame and hog the connection. With only 16 bits, frames would top out at 64 KB, and big files would be chopped into lots of tiny pieces. If a file is bigger than one frame, it simply goes out as several frames.

The type gets a whole byte, which allows 256 different kinds of frame. Version 1 only uses three, so there is plenty of space left for new ones later.

The flags also get a whole byte, which gives each frame eight separate on/off options. Keeping type and flags as full bytes also means nobody has to do awkward bit-shifting to read them.

The stream ID is 31 bits. Every response carries the ID of the request it is answering, which means a future version could send several requests at once over the same connection without changing the header. HTTP/2 uses 31 bits rather than 32 so that the number stays positive even in languages like Java that only have signed integers. The spare bit is kept aside for later use.

Altogether the header is just 9 bytes, which adds very little overhead to each frame.

## 3. Kinds of frame

Version 1 has three frame types.

A **DATA** frame (type `0x0`) carries part of a file's contents.

A **HEADERS** frame (type `0x1`) carries the request or response headers, laid out as described in section 4.

A **GOAWAY** frame (type `0x7`) has no payload and simply means "I'm closing the connection". Its stream ID is always 0.

DATA and HEADERS frames can carry the **END_STREAM** flag (value `0x01`). It means "this is my last frame for this request".

### Frame types nobody has heard of

**A receiver that meets a frame type it does not know MUST skip it cleanly.** It reads exactly as many bytes as the length field says, throws them away, and carries on with the next frame as if nothing happened. It must not close the connection or reply with an error.

This one rule is what makes a version 2 possible. New frame types can be added later, and older programs will just step over them.

## 4. How headers are written

The payload of a HEADERS frame is a list of header fields placed one after another until the payload runs out. There's no count at the start, because the frame's length already says where the list ends.

Each field begins with a one-byte **index**.

If the index is between 1 and 10, the header name comes from the list of ten names below, so the name itself is never sent. Straight after the index come two bytes giving the value's length, and then the value.

If the index is 0, the name is spelled out instead. After the index there is one byte for the name's length, then the name itself, and after that the two-byte value length and the value. Spelled-out names must be between 1 and 255 characters, all lowercase, with no spaces.

If the index is 11 or higher, the field is reserved for the future. It has the same shape as a numbered field, so the receiver can skip it safely.

Values can be up to 65,535 bytes long and must never contain a zero byte.

### The ten numbered names

These are the only header names my client and server actually send. They are numbered in this order: `:method` is 1, `:path` is 2, `:status` is 3, `host` is 4, `user-agent` is 5, `accept` is 6, `content-type` is 7, `content-length` is 8, `server` is 9 and `last-modified` is 10.

Names that start with a colon are special, and they must always use their number rather than being spelled out. Giving every common name a number means an ordinary request or response carries no name text at all, which is the same trick HTTP/2's header compression starts with.

## 5. Asking for a file and getting it back

### The request

The client gives each request a stream ID. The first request uses 1, the next one 3, then 5, and so on. Each request is a single HEADERS frame with the END_STREAM flag set. It must include `:method`, which is either `GET` or `HEAD`, and `:path`, which must start with a slash. It should also include `host`.

Requests in version 1 never have a body, so the server ignores any DATA frames a client sends.

### The response

The server answers using the same stream ID as the request. First it sends one HEADERS frame containing `:status` (such as `200`), and usually `content-type` and `content-length` as well. Then it sends the file in one or more DATA frames.

The last frame of the response has END_STREAM set. If there's no body at all, for example in reply to a `HEAD` request or for an empty file, the HEADERS frame carries END_STREAM itself.

The server finishes one response before it reads the next request.

### Finding the file

The server takes `:path`, drops anything after a `?` or `#`, and looks for that file inside its root folder. A path that ends in a slash, or points at a folder, means the `index.html` inside it. Any path containing `..`, or one that leads outside the root folder, is never served.

### Status codes

The server replies **200** when it finds the file, and the body is the file's contents.

It replies **404** when the file doesn't exist, or when the path tries to leave the root folder.

It replies **405** when the method is anything other than `GET` or `HEAD`.

It replies **400** when the request is broken. That covers headers that can't be read (for example a length that runs past the end of the payload, a badly formed name, or a zero byte in a value), a missing `:method` or `:path`, a path that doesn't start with a slash, a stream ID of 0, or a HEADERS frame bigger than 16,384 bytes.

A broken request does not break the connection. Because every frame states its own length, the server always knows where the next frame starts. It sends back a 400 for that request and keeps listening. The connection only ends when the first eight bytes are wrong, when a GOAWAY arrives, or when the other side hangs up.

## 6. How the client behaves

My client, `bcurl`, sends the eight opening bytes and then its requests, and prints the body of each response to the screen. With the `-v` option, it also prints every frame it sends and receives as a hexdump. It ignores frames meant for other streams and skips frame types it doesn't know.

When it finishes, `bcurl` exits with 0 if everything worked, 22 if any response had a 4xx or 5xx status, 7 if it couldn't connect at all, and 8 if the connection dropped or the server's reply didn't make sense.

## 7. Leaving room for version 2

A version 2 can add new frame types, new flags and new numbered header names, and it can send several requests at the same time. Version 1 programs will keep working alongside it, because they skip anything they don't recognise.

If version 2 ever needs to change the frame header itself, it will open with `BHTP/2\r\n` instead. A version 1 server won't recognise that and will simply close the connection, so nothing gets misread.
