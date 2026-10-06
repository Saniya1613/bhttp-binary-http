/*
 * bserve — BHTP/1 static file server (Track 1)
 *
 *   usage: ./bserve [-v] <root-dir> <port>
 *
 * Author: Saniya Sanjiv Patil (24bcs10246)
 */
#include "proto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SERVER_NAME "bserve/1.0"

static char g_root[PATH_MAX];
static int  g_verbose = 0;

static const char *mime_for(const char *path)
{
    static const struct { const char *ext, *type; } tbl[] = {
        { ".html", "text/html; charset=utf-8" },
        { ".htm",  "text/html; charset=utf-8" },
        { ".css",  "text/css" },
        { ".js",   "application/javascript" },
        { ".json", "application/json" },
        { ".txt",  "text/plain; charset=utf-8" },
        { ".png",  "image/png" },
        { ".jpg",  "image/jpeg" },
        { ".jpeg", "image/jpeg" },
        { ".gif",  "image/gif" },
        { ".svg",  "image/svg+xml" },
        { ".ico",  "image/x-icon" },
        { ".pdf",  "application/pdf" },
    };
    const char *dot = strrchr(path, '.');
    if (dot)
        for (size_t i = 0; i < sizeof tbl / sizeof tbl[0]; i++)
            if (strcasecmp(dot, tbl[i].ext) == 0) return tbl[i].type;
    return "application/octet-stream";
}

/* Send one frame, hexdumping it first when -v is on. */
static int send_frame(int fd, uint8_t type, uint8_t flags, uint32_t stream,
                      const uint8_t *p, uint32_t len)
{
    if (g_verbose) {
        frame_hdr h = { len, type, flags, stream };
        dump_frame(stderr, '<', &h, p, type == FT_DATA && len > 256 ? 256 : len);
    }
    return write_frame(fd, type, flags, stream, p, len);
}

static int send_headers(int fd, uint32_t stream, const field_list *fl, int end)
{
    uint8_t buf[HEADERS_MAX_LEN];
    int n = hb_encode(fl, buf, sizeof buf);
    if (n < 0) return -1;
    return send_frame(fd, FT_HEADERS, end ? FL_END_STREAM : 0, stream, buf, (uint32_t)n);
}

/* Small status reply with a text/plain body (404, 400, 405, 500). */
static int send_error(int fd, uint32_t stream, int status, const char *reason, int head_only)
{
    char body[128], st[8], cl[16];
    int bl = snprintf(body, sizeof body, "%d %s\n", status, reason);
    snprintf(st, sizeof st, "%d", status);
    snprintf(cl, sizeof cl, "%d", bl);

    field_list fl = { 0 };
    fl_add(&fl, ":status", st);
    fl_add(&fl, "content-type", "text/plain; charset=utf-8");
    fl_add(&fl, "content-length", cl);
    fl_add(&fl, "server", SERVER_NAME);
    if (head_only) return send_headers(fd, stream, &fl, 1);
    if (send_headers(fd, stream, &fl, 0) < 0) return -1;
    return send_frame(fd, FT_DATA, FL_END_STREAM, stream, (uint8_t *)body, (uint32_t)bl);
}

/* Map a request path to a file under g_root. Returns 0 and fills out[],
 * or -1 if the path is outside the root / does not exist.               */
static int map_path(const char *reqpath, char out[PATH_MAX])
{
    char rel[PATH_MAX];
    size_t n = strcspn(reqpath, "?#");             /* drop query/fragment */
    if (n >= sizeof rel - 16) return -1;
    memcpy(rel, reqpath, n); rel[n] = 0;

    /* refuse any ".." segment outright */
    for (char *p = rel; (p = strstr(p, "..")) != NULL; p += 2)
        if ((p == rel || p[-1] == '/') && (p[2] == 0 || p[2] == '/')) return -1;

    if (rel[n - 1] == '/') strcat(rel, "index.html");

    char joined[PATH_MAX * 2];
    snprintf(joined, sizeof joined, "%s%s", g_root, rel);
    if (!realpath(joined, out)) return -1;

    size_t rl = strlen(g_root);                    /* symlink escape check */
    if (strncmp(out, g_root, rl) != 0 || (out[rl] != 0 && out[rl] != '/')) return -1;

    struct stat st;
    if (stat(out, &st) < 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        if (strlen(out) + 12 >= PATH_MAX) return -1;
        strcat(out, "/index.html");
        if (stat(out, &st) < 0) return -1;
    }
    return S_ISREG(st.st_mode) ? 0 : -1;
}

static void log_req(const char *peer, uint32_t stream, const char *m, const char *p, int status)
{
    fprintf(stderr, "[bserve] %s stream=%u %s %s -> %d\n",
            peer, stream, m ? m : "-", p ? p : "-", status);
}

/* Handle one request (a HEADERS frame). Returns -1 to drop the connection. */
static int handle_request(int fd, const char *peer, const frame_hdr *h,
                          const uint8_t *payload, int too_big)
{
    field_list req;
    uint32_t s = h->stream;

    if (too_big || s == 0 || hb_decode(payload, h->length, &req) < 0) {
        log_req(peer, s, NULL, NULL, 400);
        return send_error(fd, s, 400, "Bad Request", 0);
    }
    const char *method = fl_get(&req, ":method");
    const char *path   = fl_get(&req, ":path");
    if (!method || !path || path[0] != '/') {
        log_req(peer, s, method, path, 400);
        return send_error(fd, s, 400, "Bad Request", 0);
    }
    int head = strcmp(method, "HEAD") == 0;
    if (!head && strcmp(method, "GET") != 0) {
        log_req(peer, s, method, path, 405);
        return send_error(fd, s, 405, "Method Not Allowed", 0);
    }

    char file[PATH_MAX];
    int ffd = -1;
    struct stat st;
    if (map_path(path, file) < 0 || (ffd = open(file, O_RDONLY)) < 0 ||
        fstat(ffd, &st) < 0) {
        if (ffd >= 0) close(ffd);
        log_req(peer, s, method, path, 404);
        return send_error(fd, s, 404, "Not Found", head);
    }

    char cl[32], lm[64];
    snprintf(cl, sizeof cl, "%lld", (long long)st.st_size);
    struct tm tm;
    gmtime_r(&st.st_mtime, &tm);
    strftime(lm, sizeof lm, "%a, %d %b %Y %H:%M:%S GMT", &tm);

    field_list rsp = { 0 };
    fl_add(&rsp, ":status", "200");
    fl_add(&rsp, "content-type", mime_for(file));
    fl_add(&rsp, "content-length", cl);
    fl_add(&rsp, "server", SERVER_NAME);
    fl_add(&rsp, "last-modified", lm);

    int end_now = head || st.st_size == 0;
    int rc = send_headers(fd, s, &rsp, end_now);
    log_req(peer, s, method, path, 200);
    if (rc < 0 || end_now) { close(ffd); return rc; }

    uint8_t buf[DATA_CHUNK];
    off_t left = st.st_size;
    while (left > 0) {
        ssize_t r = read(ffd, buf, sizeof buf);
        if (r <= 0) { close(ffd); return -1; }     /* file shrank: give up */
        left -= r;
        if (send_frame(fd, FT_DATA, left == 0 ? FL_END_STREAM : 0, s,
                       buf, (uint32_t)r) < 0) { close(ffd); return -1; }
    }
    close(ffd);
    return 0;
}

static void serve_connection(int fd, const char *peer)
{
    char pre[BHTP_PREFACE_LEN];
    if (read_full(fd, pre, BHTP_PREFACE_LEN) < 0 ||
        memcmp(pre, BHTP_PREFACE, BHTP_PREFACE_LEN) != 0) {
        fprintf(stderr, "[bserve] %s bad preface, closing\n", peer);
        return;
    }
    if (g_verbose) {
        fprintf(stderr, "> PREFACE\n");
        hexdump(stderr, ">   ", (uint8_t *)pre, BHTP_PREFACE_LEN);
    }

    for (;;) {                                     /* keep the connection open */
        frame_hdr h;
        uint8_t *payload;
        int rc = read_frame(fd, &h, &payload, HEADERS_MAX_LEN);
        if (rc < 0) break;                         /* peer closed */
        if (g_verbose) dump_frame(stderr, '>', &h, payload, payload ? h.length : 0);

        switch (h.type) {
        case FT_HEADERS:
            rc = handle_request(fd, peer, &h, payload, rc == 1);
            break;
        case FT_GOAWAY:
            free(payload);
            return;
        case FT_DATA:            /* v1 requests carry no body: ignore */
        default:                 /* unknown type: MUST skip (already drained) */
            if (h.type != FT_DATA)
                fprintf(stderr, "[bserve] %s skipped unknown frame type 0x%02x (%u bytes)\n",
                        peer, h.type, h.length);
            rc = 0;
            break;
        }
        free(payload);
        if (rc < 0) break;
    }
}

int main(int argc, char **argv)
{
    int argi = 1;
    if (argi < argc && strcmp(argv[argi], "-v") == 0) { g_verbose = 1; argi++; }
    if (argc - argi != 2) {
        fprintf(stderr, "usage: %s [-v] <root-dir> <port>\n", argv[0]);
        return 2;
    }
    if (!realpath(argv[argi], g_root)) { perror(argv[argi]); return 2; }
    int port = atoi(argv[argi + 1]);
    if (port <= 0 || port > 65535) { fprintf(stderr, "bad port\n"); return 2; }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);                      /* auto-reap children */

    /* Prefer a dual-stack IPv6 socket; fall back to IPv4. */
    int ls = socket(AF_INET6, SOCK_STREAM, 0), one = 1, zero = 0;
    if (ls >= 0) {
        struct sockaddr_in6 a6 = { 0 };
        a6.sin6_family = AF_INET6; a6.sin6_addr = in6addr_any; a6.sin6_port = htons(port);
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        setsockopt(ls, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
        if (bind(ls, (struct sockaddr *)&a6, sizeof a6) < 0) { close(ls); ls = -1; }
    }
    if (ls < 0) {
        struct sockaddr_in a4 = { 0 };
        a4.sin_family = AF_INET; a4.sin_addr.s_addr = htonl(INADDR_ANY); a4.sin_port = htons(port);
        ls = socket(AF_INET, SOCK_STREAM, 0);
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (ls < 0 || bind(ls, (struct sockaddr *)&a4, sizeof a4) < 0) { perror("bind"); return 1; }
    }
    if (listen(ls, 64) < 0) { perror("listen"); return 1; }
    fprintf(stderr, "[bserve] serving %s on port %d\n", g_root, port);

    for (;;) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        int c = accept(ls, (struct sockaddr *)&ss, &sl);
        if (c < 0) { if (errno == EINTR) continue; perror("accept"); continue; }

        char peer[INET6_ADDRSTRLEN + 8] = "?";
        if (ss.ss_family == AF_INET6) {
            struct sockaddr_in6 *p = (struct sockaddr_in6 *)&ss;
            char ip[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &p->sin6_addr, ip, sizeof ip);
            snprintf(peer, sizeof peer, "[%s]:%d", ip, ntohs(p->sin6_port));
        } else if (ss.ss_family == AF_INET) {
            struct sockaddr_in *p = (struct sockaddr_in *)&ss;
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &p->sin_addr, ip, sizeof ip);
            snprintf(peer, sizeof peer, "%s:%d", ip, ntohs(p->sin_port));
        }

        pid_t pid = fork();
        if (pid == 0) {
            close(ls);
            serve_connection(c, peer);
            close(c);
            _exit(0);
        }
        close(c);
    }
}
