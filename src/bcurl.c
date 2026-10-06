/*
 * bcurl — BHTP/1 client (Track 2)
 *
 *   usage: ./bcurl [-v|-vv] [-I] host:port/path [/more/paths ...]
 *
 *   Body goes to stdout. -v hexdumps every frame to stderr (DATA payloads
 *   cut at 256 bytes; -vv dumps them in full). -I sends HEAD instead of GET.
 *   Extra paths are fetched one after another on the SAME connection —
 *   bcurl never opens a second connection.
 *
 *   exit: 0 all responses 1xx-3xx | 22 any 4xx/5xx | 7 connect failed
 *         8 protocol error / connection lost | 2 usage
 *
 * Author: Saniya Sanjiv Patil (24bcs10246)
 */
#include "proto.h"

#include <netdb.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define USER_AGENT "bcurl/1.0"

static int g_verbose = 0;

typedef struct { char host[256]; char port[8]; char path[2048]; } url_t;

/* host[:port][/path]  (optional "bhtp://" prefix, [v6] literals ok) */
static int parse_url(const char *s, url_t *u)
{
    if (strncmp(s, "bhtp://", 7) == 0) s += 7;
    const char *hs = s, *he;
    if (*s == '[') {                              /* [::1]:9000/x */
        hs = s + 1;
        he = strchr(hs, ']');
        if (!he) return -1;
        s = he + 1;
    } else {
        he = s + strcspn(s, ":/");
        s = he;
    }
    if ((size_t)(he - hs) >= sizeof u->host || he == hs) return -1;
    memcpy(u->host, hs, he - hs); u->host[he - hs] = 0;

    strcpy(u->port, "9000");
    if (*s == ':') {
        s++;
        size_t pl = strcspn(s, "/");
        if (pl == 0 || pl >= sizeof u->port) return -1;
        memcpy(u->port, s, pl); u->port[pl] = 0;
        s += pl;
    }
    snprintf(u->path, sizeof u->path, "%s", *s ? s : "/");
    return 0;
}

static int dial(const url_t *u)
{
    struct addrinfo hints = { 0 }, *res, *rp;
    hints.ai_socktype = SOCK_STREAM;
    int e = getaddrinfo(u->host, u->port, &hints, &res);
    if (e) { fprintf(stderr, "bcurl: %s: %s\n", u->host, gai_strerror(e)); return -1; }
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) fprintf(stderr, "bcurl: cannot connect to %s:%s\n", u->host, u->port);
    return fd;
}

static void vdump(char dir, const frame_hdr *h, const uint8_t *p)
{
    if (!g_verbose) return;
    size_t shown = h->length;
    if (h->type == FT_DATA && g_verbose < 2 && shown > 256) shown = 256;
    dump_frame(stderr, dir, h, p, p ? shown : 0);
}

/* Send one request, read its response. Returns HTTP status, or -1. */
static int fetch(int fd, const url_t *u, uint32_t stream, int head)
{
    field_list fl = { 0 };
    char host[300];
    if (strcmp(u->port, "9000") == 0) snprintf(host, sizeof host, "%s", u->host);
    else snprintf(host, sizeof host, "%s:%s", u->host, u->port);
    fl_add(&fl, ":method", head ? "HEAD" : "GET");
    fl_add(&fl, ":path", u->path);
    fl_add(&fl, "host", host);
    fl_add(&fl, "user-agent", USER_AGENT);
    fl_add(&fl, "accept", "*/*");

    uint8_t buf[HEADERS_MAX_LEN];
    int n = hb_encode(&fl, buf, sizeof buf);
    if (n < 0) { fprintf(stderr, "bcurl: request too large\n"); return -1; }
    frame_hdr out = { (uint32_t)n, FT_HEADERS, FL_END_STREAM, stream };
    vdump('>', &out, buf);
    if (write_frame(fd, FT_HEADERS, FL_END_STREAM, stream, buf, (uint32_t)n) < 0) return -1;

    int status = -1;
    for (;;) {
        frame_hdr h;
        uint8_t *p;
        int rc = read_frame(fd, &h, &p, FRAME_MAX_LEN);
        if (rc < 0) { fprintf(stderr, "bcurl: connection closed mid-response\n"); return -1; }
        vdump('<', &h, p);

        if (h.type == FT_GOAWAY) {
            free(p);
            fprintf(stderr, "bcurl: server sent GOAWAY\n");
            return -1;
        }
        if (h.type != FT_HEADERS && h.type != FT_DATA) {   /* MUST skip */
            if (g_verbose) fprintf(stderr, "* skipped unknown frame type 0x%02x\n", h.type);
            free(p);
            continue;
        }
        if (h.stream != stream) { free(p); continue; }      /* not ours */

        if (h.type == FT_HEADERS) {
            field_list rsp;
            if (status != -1 || hb_decode(p, h.length, &rsp) < 0) {
                free(p);
                fprintf(stderr, "bcurl: malformed response headers\n");
                return -1;
            }
            const char *st = fl_get(&rsp, ":status");
            if (!st) { free(p); fprintf(stderr, "bcurl: response without :status\n"); return -1; }
            status = atoi(st);
            if (head)                                         /* curl -I style */
                for (int i = 0; i < rsp.n; i++)
                    printf("%s: %s\n", rsp.f[i].name, rsp.f[i].value);
        } else {                                              /* DATA */
            if (status == -1) {
                free(p);
                fprintf(stderr, "bcurl: DATA before HEADERS\n");
                return -1;
            }
            fwrite(p, 1, h.length, stdout);
        }
        int end = h.flags & FL_END_STREAM;
        free(p);
        if (end) break;
    }
    fflush(stdout);
    return status;
}

int main(int argc, char **argv)
{
    int argi = 1, head = 0;
    for (; argi < argc && argv[argi][0] == '-'; argi++) {
        if (strcmp(argv[argi], "-v") == 0) g_verbose = 1;
        else if (strcmp(argv[argi], "-vv") == 0) g_verbose = 2;
        else if (strcmp(argv[argi], "-I") == 0) head = 1;
        else break;
    }
    if (argi >= argc) {
        fprintf(stderr, "usage: %s [-v|-vv] [-I] host:port/path [/more/paths ...]\n", argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);

    url_t base;
    if (parse_url(argv[argi], &base) < 0) { fprintf(stderr, "bcurl: bad url %s\n", argv[argi]); return 2; }

    int fd = dial(&base);
    if (fd < 0) return 7;
    if (g_verbose) {
        fprintf(stderr, "* connected to %s port %s (one connection for all requests)\n",
                base.host, base.port);
        fprintf(stderr, "> PREFACE\n");
        hexdump(stderr, ">   ", (const uint8_t *)BHTP_PREFACE, BHTP_PREFACE_LEN);
    }
    if (write_full(fd, BHTP_PREFACE, BHTP_PREFACE_LEN) < 0) { close(fd); return 8; }

    int exitcode = 0;
    uint32_t stream = 1;
    for (int i = argi; i < argc; i++, stream += 2) {
        url_t u = base;
        if (i > argi) {
            if (argv[i][0] == '/') snprintf(u.path, sizeof u.path, "%s", argv[i]);
            else {
                url_t v;
                if (parse_url(argv[i], &v) < 0 || strcmp(v.host, base.host) ||
                    strcmp(v.port, base.port)) {
                    fprintf(stderr, "bcurl: %s is not on %s:%s — refusing a second connection\n",
                            argv[i], base.host, base.port);
                    exitcode = 2;
                    continue;
                }
                u = v;
            }
        }
        int st = fetch(fd, &u, stream, head);
        if (st < 0) { exitcode = 8; break; }
        if (g_verbose) fprintf(stderr, "* %s -> %d\n", u.path, st);
        if (st >= 400 && exitcode == 0) exitcode = 22;
    }
    close(fd);
    return exitcode;
}
