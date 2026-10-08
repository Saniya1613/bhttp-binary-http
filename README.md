# BHTP/1: HTTP, in binary

Network Architecture project by Saniya Sanjiv Patil (24bcs10246)

This project is a small binary version of HTTP. It has two programs: `bserve`, a server that hands out files, and `bcurl`, a client that asks for them. The two only talk to each other through the protocol written down in the spec.

There are three parts to hand in. The spec is in [SPEC.md](SPEC.md). The code is in the [src](src) folder: `bserve.c` is the server, `bcurl.c` is the client, and `proto.c` holds the code they share. The annotated hexdump of one complete request and response is in [HEXDUMP.md](HEXDUMP.md).

## Building it

You just need a C compiler. Run `make` in this folder and it builds both programs. It works on macOS and Linux.

## Running it

Open two terminal windows. In the first one, start the server and point it at the `www` folder:

```
./bserve ./www 9000
```

In the second one, ask for a page:

```
./bcurl -v localhost:9000/index.html
```

The page is printed to the screen. Because of `-v`, every frame that goes back and forth is also printed as a hexdump. Press Ctrl + C in the first window to stop the server.

A few other things you can try with `bcurl`:

- Add more paths to fetch them all over the same connection, for example `./bcurl localhost:9000/ /css/style.css /missing`.
- Use `-I` to send a `HEAD` request instead of `GET`.
- Use `-vv` instead of `-v` to see large file bodies in full. With `-v`, they're cut off after 256 bytes.

`bcurl` exits with 0 when everything worked, 22 if any response was an error such as 404, 7 if it couldn't connect, and 8 if the connection broke partway through.

You can also start the server as `./bserve -v ./www 9000` to see the frames from the server's side.

## Testing it

Run `make test`. The tests in `tests/interop_test.py` were written only from the spec and don't share any code with the C programs. They send hand-built frames to the server and check how it handles normal requests, missing files, broken requests, oversized headers, unknown frame types and more. They also run `bcurl` against a deliberately strange fake server that mixes in unknown frames, to make sure the client skips them and never opens a second connection.
