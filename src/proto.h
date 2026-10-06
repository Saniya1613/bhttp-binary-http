/*
 * proto.h — BHTP/1 wire format (see SPEC.md)
 *
 * Author: Saniya Sanjiv Patil (24bcs10246)
 */
#ifndef BHTP_PROTO_H
#define BHTP_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Connection preface: the client sends these 8 bytes exactly once. */
#define BHTP_PREFACE      "BHTP/1\r\n"
#define BHTP_PREFACE_LEN  8

/* Frame header: 9 bytes, network byte order.
 *   Length (24) | Type (8) | Flags (8) | R (1) | Stream ID (31)          */
#define FRAME_HDR_LEN     9
#define FRAME_MAX_LEN     0xFFFFFF      /* 2^24 - 1                         */
#define DATA_CHUNK        16384         /* what we send per DATA frame      */
#define HEADERS_MAX_LEN   16384         /* largest HEADERS we accept        */

/* Frame types */
#define FT_DATA     0x0
#define FT_HEADERS  0x1
#define FT_GOAWAY   0x7

/* Flags */
#define FL_END_STREAM 0x01

/* Static table: the ten names we actually send (index 1..10). */
#define STATIC_COUNT 10
extern const char *const STATIC_NAMES[STATIC_COUNT + 1]; /* [0] unused */

typedef struct {
    uint32_t length;
    uint8_t  type;
    uint8_t  flags;
    uint32_t stream;
} frame_hdr;

#define MAX_FIELDS 64
typedef struct {
    char name[256];
    char value[4096];
} field;

typedef struct {
    int   n;
    field f[MAX_FIELDS];
} field_list;

/* ---- byte helpers ---- */
void     put_frame_hdr(uint8_t out[FRAME_HDR_LEN], const frame_hdr *h);
void     get_frame_hdr(const uint8_t in[FRAME_HDR_LEN], frame_hdr *h);

/* ---- blocking socket I/O (return 0 ok, -1 error/EOF) ---- */
int      read_full(int fd, void *buf, size_t n);
int      write_full(int fd, const void *buf, size_t n);
int      skip_bytes(int fd, size_t n);

/* Read one frame header + payload. Payload is malloc'd into *payload
 * unless h->length > max_keep, in which case the payload is drained from
 * the socket, *payload is NULL and 1 is returned ("too big, skipped").
 * Returns 0 on success, 1 on skipped-too-big, -1 on EOF/error.          */
int      read_frame(int fd, frame_hdr *h, uint8_t **payload, uint32_t max_keep);
int      write_frame(int fd, uint8_t type, uint8_t flags, uint32_t stream,
                     const void *payload, uint32_t len);

/* ---- header block (static table + literal) ---- */
/* Encode: returns encoded length, or -1 if it does not fit in cap.     */
int      hb_encode(const field_list *fl, uint8_t *out, size_t cap);
/* Decode: returns 0 ok, -1 malformed.                                  */
int      hb_decode(const uint8_t *in, size_t len, field_list *fl);
void     fl_add(field_list *fl, const char *name, const char *value);
const char *fl_get(const field_list *fl, const char *name);

/* ---- debugging ---- */
const char *frame_type_name(uint8_t t);
void     hexdump(FILE *out, const char *prefix, const uint8_t *buf, size_t len);
void     dump_frame(FILE *out, char dir, const frame_hdr *h,
                    const uint8_t *payload, size_t shown);

#endif
