/*
 * proto.c — BHTP/1 framing, header blocks, hexdump (see SPEC.md)
 *
 * Author: Saniya Sanjiv Patil (24bcs10246)
 */
#include "proto.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *const STATIC_NAMES[STATIC_COUNT + 1] = {
    NULL,
    ":method",        /* 1  */
    ":path",          /* 2  */
    ":status",        /* 3  */
    "host",           /* 4  */
    "user-agent",     /* 5  */
    "accept",         /* 6  */
    "content-type",   /* 7  */
    "content-length", /* 8  */
    "server",         /* 9  */
    "last-modified",  /* 10 */
};

/* ------------------------------------------------------------------ */
/* frame header                                                        */
/* ------------------------------------------------------------------ */

void put_frame_hdr(uint8_t o[FRAME_HDR_LEN], const frame_hdr *h)
{
    o[0] = (h->length >> 16) & 0xFF;
    o[1] = (h->length >> 8) & 0xFF;
    o[2] = h->length & 0xFF;
    o[3] = h->type;
    o[4] = h->flags;
    o[5] = (h->stream >> 24) & 0x7F;      /* R bit always sent as 0 */
    o[6] = (h->stream >> 16) & 0xFF;
    o[7] = (h->stream >> 8) & 0xFF;
    o[8] = h->stream & 0xFF;
}

void get_frame_hdr(const uint8_t i[FRAME_HDR_LEN], frame_hdr *h)
{
    h->length = ((uint32_t)i[0] << 16) | ((uint32_t)i[1] << 8) | i[2];
    h->type   = i[3];
    h->flags  = i[4];
    h->stream = ((uint32_t)(i[5] & 0x7F) << 24) | ((uint32_t)i[6] << 16) |
                ((uint32_t)i[7] << 8) | i[8];  /* R bit ignored on receipt */
}

/* ------------------------------------------------------------------ */
/* socket I/O                                                          */
/* ------------------------------------------------------------------ */

int read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r == 0) return -1;                      /* EOF */
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        p += r; n -= (size_t)r;
    }
    return 0;
}

int write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

int skip_bytes(int fd, size_t n)
{
    uint8_t junk[4096];
    while (n > 0) {
        size_t k = n < sizeof junk ? n : sizeof junk;
        if (read_full(fd, junk, k) < 0) return -1;
        n -= k;
    }
    return 0;
}

int read_frame(int fd, frame_hdr *h, uint8_t **payload, uint32_t max_keep)
{
    uint8_t raw[FRAME_HDR_LEN];
    *payload = NULL;
    if (read_full(fd, raw, FRAME_HDR_LEN) < 0) return -1;
    get_frame_hdr(raw, h);
    if (h->length > max_keep) {
        if (skip_bytes(fd, h->length) < 0) return -1;
        return 1;
    }
    *payload = malloc(h->length ? h->length : 1);
    if (!*payload) return -1;
    if (h->length && read_full(fd, *payload, h->length) < 0) {
        free(*payload); *payload = NULL;
        return -1;
    }
    return 0;
}

int write_frame(int fd, uint8_t type, uint8_t flags, uint32_t stream,
                const void *payload, uint32_t len)
{
    uint8_t raw[FRAME_HDR_LEN];
    frame_hdr h = { len, type, flags, stream };
    if (len > FRAME_MAX_LEN) return -1;
    put_frame_hdr(raw, &h);
    if (write_full(fd, raw, FRAME_HDR_LEN) < 0) return -1;
    if (len && write_full(fd, payload, len) < 0) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* header block                                                        */
/*                                                                     */
/*   field := index:u8  [name_len:u8 name]  value_len:u16 value        */
/*   index 0      -> literal name follows                              */
/*   index 1..10  -> STATIC_NAMES[index]                               */
/*   index 11..255-> reserved; receiver MUST skip the field            */
/* ------------------------------------------------------------------ */

static int static_index(const char *name)
{
    for (int i = 1; i <= STATIC_COUNT; i++)
        if (strcmp(STATIC_NAMES[i], name) == 0) return i;
    return 0;
}

int hb_encode(const field_list *fl, uint8_t *out, size_t cap)
{
    size_t pos = 0;
    for (int i = 0; i < fl->n; i++) {
        const char *name = fl->f[i].name, *val = fl->f[i].value;
        size_t nl = strlen(name), vl = strlen(val);
        int idx = static_index(name);
        size_t need = 1 + (idx ? 0 : 1 + nl) + 2 + vl;
        if (nl == 0 || nl > 255 || vl > 0xFFFF || pos + need > cap) return -1;
        out[pos++] = (uint8_t)idx;
        if (!idx) {
            out[pos++] = (uint8_t)nl;
            memcpy(out + pos, name, nl); pos += nl;
        }
        out[pos++] = (vl >> 8) & 0xFF;
        out[pos++] = vl & 0xFF;
        memcpy(out + pos, val, vl); pos += vl;
    }
    return (int)pos;
}

static int valid_name(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c <= 0x20 || c >= 0x7F || isupper(c)) return 0;
    }
    return 1;
}

int hb_decode(const uint8_t *in, size_t len, field_list *fl)
{
    size_t pos = 0;
    fl->n = 0;
    while (pos < len) {
        uint8_t idx = in[pos++];
        const uint8_t *name = NULL; size_t nl = 0;
        if (idx == 0) {
            if (pos >= len) return -1;
            nl = in[pos++];
            if (nl == 0 || pos + nl > len) return -1;
            name = in + pos; pos += nl;
            if (!valid_name(name, nl)) return -1;
        }
        if (pos + 2 > len) return -1;
        size_t vl = ((size_t)in[pos] << 8) | in[pos + 1];
        pos += 2;
        if (pos + vl > len) return -1;
        const uint8_t *val = in + pos; pos += vl;

        if (idx > STATIC_COUNT) continue;           /* unknown index: skip */
        if (memchr(val, 0, vl)) return -1;          /* no NULs in values   */
        if (fl->n >= MAX_FIELDS || vl >= sizeof fl->f[0].value) return -1;

        field *f = &fl->f[fl->n++];
        if (idx) strcpy(f->name, STATIC_NAMES[idx]);
        else { memcpy(f->name, name, nl); f->name[nl] = 0; }
        memcpy(f->value, val, vl); f->value[vl] = 0;
    }
    return 0;
}

void fl_add(field_list *fl, const char *name, const char *value)
{
    if (fl->n >= MAX_FIELDS) return;
    field *f = &fl->f[fl->n++];
    snprintf(f->name, sizeof f->name, "%s", name);
    snprintf(f->value, sizeof f->value, "%s", value);
}

const char *fl_get(const field_list *fl, const char *name)
{
    for (int i = 0; i < fl->n; i++)
        if (strcmp(fl->f[i].name, name) == 0) return fl->f[i].value;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* debugging                                                           */
/* ------------------------------------------------------------------ */

const char *frame_type_name(uint8_t t)
{
    switch (t) {
    case FT_DATA:    return "DATA";
    case FT_HEADERS: return "HEADERS";
    case FT_GOAWAY:  return "GOAWAY";
    default:         return "UNKNOWN";
    }
}

void hexdump(FILE *out, const char *prefix, const uint8_t *buf, size_t len)
{
    for (size_t off = 0; off < len; off += 16) {
        fprintf(out, "%s%04zx  ", prefix, off);
        for (size_t i = 0; i < 16; i++) {
            if (off + i < len) fprintf(out, "%02x ", buf[off + i]);
            else fputs("   ", out);
            if (i == 7) fputc(' ', out);
        }
        fputs(" |", out);
        for (size_t i = 0; i < 16 && off + i < len; i++) {
            uint8_t c = buf[off + i];
            fputc(c >= 0x20 && c < 0x7F ? c : '.', out);
        }
        fputs("|\n", out);
    }
}

void dump_frame(FILE *out, char dir, const frame_hdr *h,
                const uint8_t *payload, size_t shown)
{
    uint8_t raw[FRAME_HDR_LEN];
    put_frame_hdr(raw, h);
    fprintf(out, "%c %s frame  length=%u type=0x%02x flags=0x%02x%s stream=%u\n",
            dir, frame_type_name(h->type), h->length, h->type, h->flags,
            (h->flags & FL_END_STREAM) ? "(END_STREAM)" : "", h->stream);
    fprintf(out, "%c   header : ", dir);
    for (int i = 0; i < FRAME_HDR_LEN; i++) fprintf(out, "%02x ", raw[i]);
    fputc('\n', out);
    if (h->length && payload) {
        char pfx[8];
        snprintf(pfx, sizeof pfx, "%c   ", dir);
        hexdump(out, pfx, payload, shown);
        if (shown < h->length)
            fprintf(out, "%c   ... %u more payload bytes not shown\n",
                    dir, h->length - (unsigned)shown);
    }
    if (h->type == FT_HEADERS && payload) {
        field_list fl;
        if (hb_decode(payload, h->length, &fl) == 0)
            for (int i = 0; i < fl.n; i++)
                fprintf(out, "%c   %s: %s\n", dir, fl.f[i].name, fl.f[i].value);
        else
            fprintf(out, "%c   (malformed header block)\n", dir);
    }
}
