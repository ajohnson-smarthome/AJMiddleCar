#ifndef VIDEO_WIRE_H
#define VIDEO_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "contract.h"   /* VIDEO_HEADER_BYTES, VIDEO_CHUNK_BYTES, VIDEO_WIRE_PROTO */

// The video datagram: a 12-byte header and up to VIDEO_CHUNK_BYTES of an H.264 Annex B
// frame. The only binary message in the project. Three receivers (this, VideoWire.swift,
// video_wire.py) implement the rules below; the vectors in contract/car-api.json are what
// keeps their headers byte-identical, and test_video_wire.c is what keeps their verdicts
// identical on the cases the vectors cannot express (lengths, ordering, loss).

#define VW_FLAG_KEY   0x01u
#define VW_MAX_CHUNKS 255u

typedef struct {
    uint8_t  proto;        // VIDEO_WIRE_PROTO
    uint8_t  flags;        // VW_FLAG_KEY or 0
    uint8_t  stream;       // +1 per stream start; a change resets the receiver
    uint16_t frame;        // encoded-frame counter, from 0 per stream, modulo 2^16
    uint8_t  chunk;        // 0-based index within the frame
    uint8_t  count;        // chunks in the frame, 1..255
    uint32_t captured_ms;  // the car's clock at capture; diagnostic only
} vw_header_t;

static inline void vw_pack(const vw_header_t *h, uint8_t out[VIDEO_HEADER_BYTES]) {
    out[0] = h->proto;
    out[1] = h->flags;
    out[2] = h->stream;
    out[3] = 0;
    out[4] = (uint8_t)(h->frame >> 8);
    out[5] = (uint8_t)(h->frame);
    out[6] = h->chunk;
    out[7] = h->count;
    out[8]  = (uint8_t)(h->captured_ms >> 24);
    out[9]  = (uint8_t)(h->captured_ms >> 16);
    out[10] = (uint8_t)(h->captured_ms >> 8);
    out[11] = (uint8_t)(h->captured_ms);
}

// -1: not a header this receiver accepts. Strict on purpose — a set reserved byte or an
// undefined flag is a format nobody agreed on, and a new format gets a new proto.
static inline int vw_unpack(const uint8_t *b, size_t n, vw_header_t *h) {
    if (b == NULL || n < VIDEO_HEADER_BYTES) return -1;
    if (b[0] != VIDEO_WIRE_PROTO) return -1;
    if (b[1] & (uint8_t)~VW_FLAG_KEY) return -1;
    if (b[3] != 0) return -1;
    if (b[7] == 0 || b[6] >= b[7]) return -1;
    h->proto = b[0];
    h->flags = b[1];
    h->stream = b[2];
    h->frame = (uint16_t)(((uint16_t)b[4] << 8) | b[5]);
    h->chunk = b[6];
    h->count = b[7];
    h->captured_ms = ((uint32_t)b[8] << 24) | ((uint32_t)b[9] << 16) | ((uint32_t)b[10] << 8) | b[11];
    return 0;
}

// RFC 1982 on a 16-bit counter: at 15 fps it wraps in 73 minutes, which is a drive.
static inline bool vw_frame_newer(uint16_t a, uint16_t b) {
    return (int16_t)(a - b) > 0;
}

// Chunks a frame of `len` bytes needs; 0 when it is empty or would not fit 255.
static inline unsigned vw_chunk_count(size_t len) {
    if (len == 0) return 0;
    size_t n = (len + VIDEO_CHUNK_BYTES - 1) / VIDEO_CHUNK_BYTES;
    return n > VW_MAX_CHUNKS ? 0 : (unsigned)n;
}

// Datagram `i` of a frame into `out` (VIDEO_HEADER_BYTES + VIDEO_CHUNK_BYTES or more).
// `base` supplies proto/flags/stream/frame/captured_ms; chunk and count are filled here.
// Returns the datagram length, or 0 when `i` is out of range or the frame does not fit.
static inline size_t vw_chunk(const vw_header_t *base, const uint8_t *frame, size_t len,
                              unsigned i, uint8_t *out) {
    unsigned n = vw_chunk_count(len);
    if (n == 0 || i >= n) return 0;
    vw_header_t h = *base;
    h.chunk = (uint8_t)i;
    h.count = (uint8_t)n;
    vw_pack(&h, out);
    size_t off = (size_t)i * VIDEO_CHUNK_BYTES;
    size_t take = len - off < VIDEO_CHUNK_BYTES ? len - off : VIDEO_CHUNK_BYTES;
    memcpy(out + VIDEO_HEADER_BYTES, frame + off, take);
    return VIDEO_HEADER_BYTES + take;
}

/* ---- receiver ------------------------------------------------------------------- */

typedef enum {
    VW_RX_NONE = 0,  // placed, ignored (older, duplicate), or a P-frame withheld after a loss
    VW_RX_FRAME,     // a whole frame is in the caller's buffer, *out_len bytes
    VW_RX_LOSS,      // a frame was abandoned or a chunk was corrupt: ask the car for a keyframe
    VW_RX_BAD        // not a datagram this receiver accepts; nothing changed
} vw_rx_event_t;

typedef struct {
    bool     have_stream;
    uint8_t  stream;
    bool     have_last;
    uint16_t last_frame;   // newest frame finished or abandoned
    bool     in_progress;
    uint16_t frame;
    uint8_t  count, flags;
    uint8_t  got[32];      // one bit per chunk index
    unsigned n_got;
    size_t   last_len;     // payload of the last chunk, once it arrived
    bool     wait_key;     // after a loss, P-frames are garbage until an IDR
    uint32_t dropped;      // frames abandoned since init
} vw_rx_t;

static inline void vw_rx_init(vw_rx_t *r) {
    memset(r, 0, sizeof(*r));
    r->wait_key = true;
}

static inline void vw_rx_abandon(vw_rx_t *r) {
    r->in_progress = false;
    r->have_last = true;
    r->last_frame = r->frame;
    r->wait_key = true;
    r->dropped++;
}

static inline void vw_rx_begin(vw_rx_t *r, const vw_header_t *h) {
    r->in_progress = true;
    r->frame = h->frame;
    r->count = h->count;
    r->flags = h->flags;
    memset(r->got, 0, sizeof(r->got));
    r->n_got = 0;
    r->last_len = 0;
}

static inline vw_rx_event_t vw_rx_feed(vw_rx_t *r, const uint8_t *dgram, size_t n,
                                       uint8_t *buf, size_t cap, size_t *out_len) {
    vw_header_t h;
    if (vw_unpack(dgram, n, &h) != 0) return VW_RX_BAD;
    size_t payload = n - VIDEO_HEADER_BYTES;
    bool last = h.chunk == h.count - 1;
    if (payload == 0 || payload > VIDEO_CHUNK_BYTES || (!last && payload != VIDEO_CHUNK_BYTES)) return VW_RX_BAD;

    if (!r->have_stream || h.stream != r->stream) {
        r->have_stream = true;
        r->stream = h.stream;
        r->in_progress = false;
        r->have_last = false;
        r->wait_key = true;
    }

    vw_rx_event_t ev = VW_RX_NONE;
    if (r->in_progress && h.frame != r->frame) {
        if (!vw_frame_newer(h.frame, r->frame)) return VW_RX_NONE;   /* older: a straggler */
        vw_rx_abandon(r);                                            /* newer: the old one is lost */
        ev = VW_RX_LOSS;
    }
    if (!r->in_progress) {
        if (r->have_last && !vw_frame_newer(h.frame, r->last_frame)) return ev;
        if ((size_t)h.count * VIDEO_CHUNK_BYTES > cap) {
            vw_rx_begin(r, &h);
            vw_rx_abandon(r);
            return VW_RX_LOSS;
        }
        vw_rx_begin(r, &h);
    } else if (h.count != r->count || h.flags != r->flags) {
        vw_rx_abandon(r);
        return VW_RX_LOSS;
    }

    uint8_t bit = (uint8_t)(1u << (h.chunk & 7));
    if (r->got[h.chunk >> 3] & bit) return ev;                       /* a duplicate */
    memcpy(buf + (size_t)h.chunk * VIDEO_CHUNK_BYTES, dgram + VIDEO_HEADER_BYTES, payload);
    r->got[h.chunk >> 3] |= bit;
    r->n_got++;
    if (last) r->last_len = payload;
    if (r->n_got < r->count) return ev;

    r->in_progress = false;
    r->have_last = true;
    r->last_frame = r->frame;
    if (r->flags & VW_FLAG_KEY) r->wait_key = false;
    if (r->wait_key) return ev;                                      /* complete, but not decodable */
    *out_len = (size_t)(r->count - 1) * VIDEO_CHUNK_BYTES + r->last_len;
    return VW_RX_FRAME;
}

#endif // VIDEO_WIRE_H
