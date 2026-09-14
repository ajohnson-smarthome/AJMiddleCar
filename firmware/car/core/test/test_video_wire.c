#include "../main/video_wire.h"
#include "contract.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The header vectors, byte for byte as contract/car-api.json's video.vectors spells them.
   tools/test_gen_contract.py checks this file carries every one of them. */
static const uint8_t V_KEY0[12]  = {0x01,0x01,0x03,0x00,0x00,0x07,0x00,0x1e,0x00,0x0c,0x65,0x39};  /* 010103000007001e000c6539 */
static const uint8_t V_EDGE[12]  = {0x01,0x00,0xff,0x00,0xff,0xff,0x02,0x03,0xff,0xff,0xff,0xff};  /* 0100ff00ffff0203ffffffff */
static const uint8_t V_PROTO[12] = {0x02,0x01,0x03,0x00,0x00,0x07,0x00,0x1e,0x00,0x0c,0x65,0x39};  /* 020103000007001e000c6539 */
static const uint8_t V_CHUNK[12] = {0x01,0x01,0x03,0x00,0x00,0x07,0x1e,0x1e,0x00,0x0c,0x65,0x39};  /* 0101030000071e1e000c6539 */
static const uint8_t V_COUNT[12] = {0x01,0x01,0x03,0x00,0x00,0x07,0x00,0x00,0x00,0x00,0x00,0x00};  /* 010103000007000000000000 */
static const uint8_t V_RESV[12]  = {0x01,0x01,0x03,0x01,0x00,0x07,0x00,0x1e,0x00,0x0c,0x65,0x39};  /* 010103010007001e000c6539 */
static const uint8_t V_FLAG[12]  = {0x01,0x02,0x03,0x00,0x00,0x07,0x00,0x1e,0x00,0x0c,0x65,0x39};  /* 010203000007001e000c6539 */

static uint8_t g_frame[VIDEO_CHUNK_BYTES * 4];
static uint8_t g_out[VIDEO_CHUNK_BYTES * 4];
static uint8_t g_dgram[VIDEO_HEADER_BYTES + VIDEO_CHUNK_BYTES];

static void fill(uint8_t *p, size_t n, uint8_t seed) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(seed + i * 7); }

/* Send frame `frame` of `len` bytes as chunks in `order[]` (n entries); skip index `skip`
   when >= 0. Returns the last event. */
static vw_rx_event_t send(vw_rx_t *rx, uint8_t stream, uint16_t frame, bool key, size_t len,
                          const unsigned *order, unsigned n, int skip, size_t *out_len) {
    vw_header_t base = { .proto = VIDEO_WIRE_PROTO, .flags = key ? VW_FLAG_KEY : 0,
                         .stream = stream, .frame = frame, .captured_ms = 1000 + frame };
    vw_rx_event_t ev = VW_RX_NONE;
    for (unsigned k = 0; k < n; k++) {
        if ((int)order[k] == skip) continue;
        size_t d = vw_chunk(&base, g_frame, len, order[k], g_dgram);
        assert(d > 0);
        ev = vw_rx_feed(rx, g_dgram, d, g_out, sizeof(g_out), out_len);
    }
    return ev;
}

int main(void) {
    /* --- header vectors ------------------------------------------------------- */
    vw_header_t h = { .proto = 1, .flags = 1, .stream = 3, .frame = 7, .chunk = 0, .count = 30, .captured_ms = 812345 };
    uint8_t b[12];
    vw_pack(&h, b);
    assert(memcmp(b, V_KEY0, 12) == 0);
    vw_header_t u;
    assert(vw_unpack(V_KEY0, 12, &u) == 0 && u.proto == 1 && u.flags == 1 && u.stream == 3 &&
           u.frame == 7 && u.chunk == 0 && u.count == 30 && u.captured_ms == 812345);
    assert(vw_unpack(V_EDGE, 12, &u) == 0 && u.stream == 255 && u.frame == 65535 && u.chunk == 2 &&
           u.count == 3 && u.captured_ms == 4294967295u);
    h = (vw_header_t){ .proto = 1, .flags = 0, .stream = 255, .frame = 65535, .chunk = 2, .count = 3, .captured_ms = 4294967295u };
    vw_pack(&h, b);
    assert(memcmp(b, V_EDGE, 12) == 0);
    assert(vw_unpack(V_PROTO, 12, &u) == -1);
    assert(vw_unpack(V_CHUNK, 12, &u) == -1);
    assert(vw_unpack(V_COUNT, 12, &u) == -1);
    assert(vw_unpack(V_RESV, 12, &u) == -1);
    assert(vw_unpack(V_FLAG, 12, &u) == -1);
    assert(vw_unpack(V_KEY0, 11, &u) == -1);

    /* --- frame ordering across the u16 edge ----------------------------------- */
    assert(vw_frame_newer(1, 0) && vw_frame_newer(0, 65535) && !vw_frame_newer(65535, 0) &&
           !vw_frame_newer(5, 5) && vw_frame_newer(32768, 1) && !vw_frame_newer(1, 32768));

    /* --- chunking --------------------------------------------------------------- */
    assert(vw_chunk_count(0) == 0 && vw_chunk_count(1) == 1 && vw_chunk_count(VIDEO_CHUNK_BYTES) == 1 &&
           vw_chunk_count(VIDEO_CHUNK_BYTES + 1) == 2 && vw_chunk_count((size_t)VIDEO_CHUNK_BYTES * 255) == 255 &&
           vw_chunk_count((size_t)VIDEO_CHUNK_BYTES * 255 + 1) == 0);
    fill(g_frame, 3000, 1);
    vw_header_t base = { .proto = 1, .flags = VW_FLAG_KEY, .stream = 1, .frame = 9, .captured_ms = 5 };
    assert(vw_chunk(&base, g_frame, 3000, 0, g_dgram) == VIDEO_HEADER_BYTES + VIDEO_CHUNK_BYTES);
    assert(vw_unpack(g_dgram, 12, &u) == 0 && u.chunk == 0 && u.count == 3 && u.flags == VW_FLAG_KEY);
    assert(memcmp(g_dgram + 12, g_frame, VIDEO_CHUNK_BYTES) == 0);
    assert(vw_chunk(&base, g_frame, 3000, 2, g_dgram) == VIDEO_HEADER_BYTES + (3000 - 2 * VIDEO_CHUNK_BYTES));
    assert(vw_unpack(g_dgram, 12, &u) == 0 && u.chunk == 2);
    assert(vw_chunk(&base, g_frame, 3000, 3, g_dgram) == 0);

    /* --- receiver ----------------------------------------------------------------- */
    const unsigned in_order[3] = {0, 1, 2}, shuffled[3] = {2, 0, 1};
    size_t out_len = 0;
    vw_rx_t rx;
    vw_rx_init(&rx);

    /* A P-frame before any keyframe is withheld. */
    assert(send(&rx, 1, 0, false, 3000, in_order, 3, -1, &out_len) == VW_RX_NONE);
    /* A keyframe is delivered, whole and in order. */
    fill(g_frame, 3000, 42);
    assert(send(&rx, 1, 1, true, 3000, in_order, 3, -1, &out_len) == VW_RX_FRAME);
    assert(out_len == 3000 && memcmp(g_out, g_frame, 3000) == 0);
    /* Chunks out of order still make a frame. */
    fill(g_frame, 2900, 7);
    assert(send(&rx, 1, 2, false, 2900, shuffled, 3, -1, &out_len) == VW_RX_FRAME && out_len == 2900);
    assert(memcmp(g_out, g_frame, 2900) == 0);
    /* A duplicate does not count twice, and does not break the frame. */
    const unsigned dup[4] = {0, 1, 1, 2};
    assert(send(&rx, 1, 3, false, 3000, dup, 4, -1, &out_len) == VW_RX_FRAME && out_len == 3000);
    /* Loss: frame 4 misses a chunk; the first chunk of frame 5 reports it. */
    assert(send(&rx, 1, 4, false, 3000, in_order, 3, 1, &out_len) == VW_RX_NONE);
    const unsigned first[1] = {0};
    assert(send(&rx, 1, 5, false, 3000, first, 1, -1, &out_len) == VW_RX_LOSS);
    assert(rx.dropped == 1);
    const unsigned rest[2] = {1, 2};
    assert(send(&rx, 1, 5, false, 3000, rest, 2, -1, &out_len) == VW_RX_NONE);   /* complete, but withheld */
    assert(send(&rx, 1, 6, false, 3000, in_order, 3, -1, &out_len) == VW_RX_NONE);
    assert(send(&rx, 1, 7, true, 3000, in_order, 3, -1, &out_len) == VW_RX_FRAME);  /* the key unlocks */
    assert(send(&rx, 1, 8, false, 3000, in_order, 3, -1, &out_len) == VW_RX_FRAME);
    /* A straggler from a finished frame is ignored and changes nothing. */
    assert(send(&rx, 1, 7, true, 3000, first, 1, -1, &out_len) == VW_RX_NONE);
    assert(send(&rx, 1, 9, false, 3000, in_order, 3, -1, &out_len) == VW_RX_FRAME);
    /* A loss whose successor is a one-chunk keyframe: delivered, no key to ask for. */
    assert(send(&rx, 1, 10, false, 3000, in_order, 3, 2, &out_len) == VW_RX_NONE);
    assert(send(&rx, 1, 11, true, 100, first, 1, -1, &out_len) == VW_RX_FRAME && out_len == 100);
    assert(rx.dropped == 2);
    /* The counter's edge. */
    vw_rx_init(&rx);
    assert(send(&rx, 2, 65535, true, 3000, in_order, 3, -1, &out_len) == VW_RX_FRAME);
    assert(send(&rx, 2, 0, false, 3000, in_order, 3, -1, &out_len) == VW_RX_FRAME);
    /* A new stream resets everything and waits for its keyframe. */
    assert(send(&rx, 3, 0, false, 3000, in_order, 3, -1, &out_len) == VW_RX_NONE);
    assert(send(&rx, 3, 1, true, 3000, in_order, 3, -1, &out_len) == VW_RX_FRAME);
    /* A chunk whose count disagrees with the frame in progress: corrupt, lost. */
    assert(send(&rx, 3, 2, false, 3000, first, 1, -1, &out_len) == VW_RX_NONE);
    {
        vw_header_t bad = { .proto = 1, .flags = 0, .stream = 3, .frame = 2, .chunk = 1, .count = 4, .captured_ms = 0 };
        vw_pack(&bad, g_dgram);
        memset(g_dgram + 12, 0, VIDEO_CHUNK_BYTES);
        assert(vw_rx_feed(&rx, g_dgram, 12 + VIDEO_CHUNK_BYTES, g_out, sizeof(g_out), &out_len) == VW_RX_LOSS);
    }
    /* The length invariant: a middle chunk that is short is not a chunk. */
    {
        vw_header_t mid = { .proto = 1, .flags = 0, .stream = 3, .frame = 3, .chunk = 0, .count = 3, .captured_ms = 0 };
        vw_pack(&mid, g_dgram);
        assert(vw_rx_feed(&rx, g_dgram, 12 + 100, g_out, sizeof(g_out), &out_len) == VW_RX_BAD);
        assert(vw_rx_feed(&rx, g_dgram, 12, g_out, sizeof(g_out), &out_len) == VW_RX_BAD);
        assert(vw_rx_feed(&rx, g_dgram, 12 + VIDEO_CHUNK_BYTES + 1, g_out, sizeof(g_out), &out_len) == VW_RX_BAD);
    }
    /* A bad header touches nothing. */
    assert(vw_rx_feed(&rx, V_PROTO, 12, g_out, sizeof(g_out), &out_len) == VW_RX_BAD);
    /* A frame the buffer cannot hold is dropped, not truncated. */
    {
        vw_rx_t small;
        vw_rx_init(&small);
        vw_header_t big = { .proto = 1, .flags = VW_FLAG_KEY, .stream = 1, .frame = 0, .chunk = 0, .count = 200, .captured_ms = 0 };
        vw_pack(&big, g_dgram);
        assert(vw_rx_feed(&small, g_dgram, 12 + VIDEO_CHUNK_BYTES, g_out, sizeof(g_out), &out_len) == VW_RX_LOSS);
        assert(small.dropped == 1);
    }

    printf("test_video_wire: OK\n");
    return 0;
}
