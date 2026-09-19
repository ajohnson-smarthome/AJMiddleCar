#include "../main/tcp_pending.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_a_fresh_backlog_is_empty(void) {
    tcp_pending_t p;
    tcp_pending_clear(&p);
    assert(tcp_pending_empty(&p));
}

static void test_a_fully_sent_stash_leaves_nothing_pending(void) {
    /* pump_read's common case: send() placed every byte of the chunk it just read. sent == n
     * means the remainder tcp_pending_stash keeps is zero bytes — empty on arrival. */
    tcp_pending_t p;
    tcp_pending_clear(&p);
    tcp_pending_stash(&p, "hello", 5, 5);
    assert(tcp_pending_empty(&p));
}

static void test_a_wholly_unsent_stash_keeps_every_byte(void) {
    /* send() placed nothing at all (EAGAIN on the first attempt): sent == 0, so the whole
     * chunk becomes the backlog. */
    tcp_pending_t p;
    tcp_pending_clear(&p);
    tcp_pending_stash(&p, "hello", 5, 0);
    assert(!tcp_pending_empty(&p));
    assert(p.len == 5);
    assert(p.off == 0);
    assert(memcmp(p.buf, "hello", 5) == 0);
}

static void test_a_partial_stash_keeps_only_the_unsent_tail(void) {
    tcp_pending_t p;
    tcp_pending_clear(&p);
    tcp_pending_stash(&p, "hello world", 11, 6);   /* "hello " (6 bytes) went out */
    assert(!tcp_pending_empty(&p));
    assert(p.len == 5);
    assert(p.off == 0);
    assert(memcmp(p.buf, "world", 5) == 0);   /* the unsent tail, copied to the front */
}

static void test_advance_moves_the_offset_without_clearing(void) {
    tcp_pending_t p;
    tcp_pending_clear(&p);
    tcp_pending_stash(&p, "hello world", 11, 0);   /* 11 bytes pending */
    tcp_pending_advance(&p, 4);                    /* flush_pending sent 4 of them */
    assert(!tcp_pending_empty(&p));                /* 7 bytes still remain */
    assert(p.len == 11);
    assert(p.off == 4);
}

static void test_advance_to_exactly_len_clears_the_backlog(void) {
    tcp_pending_t p;
    tcp_pending_clear(&p);
    tcp_pending_stash(&p, "hello", 5, 0);
    tcp_pending_advance(&p, 5);
    assert(tcp_pending_empty(&p));
    assert(p.len == 0);
    assert(p.off == 0);
}

static void test_advance_can_span_more_than_one_call(void) {
    /* Three separate flush_pending passes, each moving the backlog a bit further, matching
     * how a real destination socket drains under sustained backpressure — a few bytes
     * accepted per select() pass, not all at once. */
    tcp_pending_t p;
    tcp_pending_clear(&p);
    tcp_pending_stash(&p, "abcdefghij", 10, 0);
    tcp_pending_advance(&p, 3);
    assert(p.off == 3 && !tcp_pending_empty(&p));
    tcp_pending_advance(&p, 4);
    assert(p.off == 7 && !tcp_pending_empty(&p));
    tcp_pending_advance(&p, 3);
    assert(tcp_pending_empty(&p));
}

static void test_clear_after_a_partial_advance_forgets_the_backlog(void) {
    /* close_slot's use: whatever a connection left mid-flush must not survive into a slot
     * a later accept() reuses. */
    tcp_pending_t p;
    tcp_pending_clear(&p);
    tcp_pending_stash(&p, "hello", 5, 0);
    tcp_pending_advance(&p, 2);
    tcp_pending_clear(&p);
    assert(tcp_pending_empty(&p));
    assert(p.len == 0);
    assert(p.off == 0);
}

static void test_a_new_stash_replaces_whatever_was_there(void) {
    /* Only reachable in practice once a previous backlog has drained back to empty — see
     * tcp_pending_stash's documented precondition — but the function itself does not assume
     * that; this pins the actual overwrite behaviour if that precondition is ever violated. */
    tcp_pending_t p;
    tcp_pending_clear(&p);
    tcp_pending_stash(&p, "first", 5, 0);
    tcp_pending_stash(&p, "second-chunk", 12, 0);
    assert(p.len == 12);
    assert(p.off == 0);
    assert(memcmp(p.buf, "second-chunk", 12) == 0);
}

/* tcp_pending.h documents `0 < w <= however many remain` as a precondition and does not check
 * it. An overshoot used to latch the backlog non-empty forever: relay_tcp.c's fd-set build then
 * never offers that direction's source for reading again, so the connection stalls, and
 * flush_pending's `int remaining = p->len - p->off` goes negative and reaches send() as a
 * size_t of about four gigabytes out of a 1460-byte buffer. This module is pure and host-tested
 * precisely so its bookkeeping does not rest on every caller getting it right. */
static void test_an_overshooting_advance_still_clears_the_backlog(void) {
    tcp_pending_t p;
    tcp_pending_clear(&p);
    tcp_pending_stash(&p, "abcdefgh", 8, 0);
    assert(!tcp_pending_empty(&p));

    tcp_pending_advance(&p, 99);
    assert(tcp_pending_empty(&p));
}

/* handle_connecting's use (relay_tcp.c): the request a phone sends before the car has answered
 * the connect is read for real — so the phone's hangup behind it can be seen and scored the
 * moment it happens — and parked here, with no send() attempted, until the slot goes ACTIVE
 * and one flush hands the car exactly the bytes the phone sent. Reading keeps going while the
 * backlog has room, because the hangup is a FIN queued behind whatever the phone sent last: a
 * request that arrives in two segments (headers, then a small body) must land here in order,
 * and the phone must stay offered for reading until the room is gone. */
static void test_a_request_read_during_the_connect_waits_whole_for_the_car(void) {
    static const char head[] = "POST /config HTTP/1.1\r\nHost: 192.168.7.1\r\nContent-Length: 14\r\n\r\n";
    static const char body[] = "{\"ramp\":{...}}";
    const int hn = (int)sizeof(head) - 1, bn = (int)sizeof(body) - 1;
    tcp_pending_t p;
    tcp_pending_clear(&p);
    assert(tcp_pending_room(&p) == TCP_PENDING_BUF_LEN);   /* empty: the whole buffer is room */

    tcp_pending_append(&p, head, hn);              /* the first segment; nothing sent yet */
    assert(!tcp_pending_empty(&p));
    assert(tcp_pending_room(&p) == TCP_PENDING_BUF_LEN - hn);   /* ...and the phone stays readable */
    tcp_pending_append(&p, body, bn);              /* the second, behind the first */
    assert(p.len == hn + bn && p.off == 0);
    assert(memcmp(p.buf, head, (size_t)hn) == 0);
    assert(memcmp(p.buf + hn, body, (size_t)bn) == 0);

    tcp_pending_advance(&p, hn + bn);              /* the first flush once the slot is ACTIVE */
    assert(tcp_pending_empty(&p));
    assert(tcp_pending_room(&p) == TCP_PENDING_BUF_LEN);
}

/* A request longer than the backlog (a firmware upload) fills it; at zero room the fd-set
 * build stops offering the phone, so the level-triggered bytes it cannot take never spin the
 * task, and the hangup behind them is left to the connect deadline. Filling must be exact:
 * the last append takes precisely what is left, never a byte more. */
static void test_room_reaches_zero_and_no_further(void) {
    char chunk[TCP_PENDING_BUF_LEN];
    memset(chunk, 'x', sizeof(chunk));
    tcp_pending_t p;
    tcp_pending_clear(&p);
    tcp_pending_append(&p, chunk, TCP_PENDING_BUF_LEN - 3);
    assert(tcp_pending_room(&p) == 3);
    tcp_pending_append(&p, "abc", 3);              /* exactly the room left */
    assert(tcp_pending_room(&p) == 0);
    assert(p.len == TCP_PENDING_BUF_LEN);
    assert(memcmp(p.buf + TCP_PENDING_BUF_LEN - 3, "abc", 3) == 0);
    /* Room comes back as the flush moves bytes out, and clear forgets it all. */
    tcp_pending_advance(&p, 100);
    assert(!tcp_pending_empty(&p));
    tcp_pending_clear(&p);
    assert(tcp_pending_room(&p) == TCP_PENDING_BUF_LEN);
}

int main(void) {
    test_a_fresh_backlog_is_empty();
    test_a_fully_sent_stash_leaves_nothing_pending();
    test_a_wholly_unsent_stash_keeps_every_byte();
    test_a_partial_stash_keeps_only_the_unsent_tail();
    test_advance_moves_the_offset_without_clearing();
    test_advance_to_exactly_len_clears_the_backlog();
    test_advance_can_span_more_than_one_call();
    test_clear_after_a_partial_advance_forgets_the_backlog();
    test_a_new_stash_replaces_whatever_was_there();
    test_an_overshooting_advance_still_clears_the_backlog();
    test_a_request_read_during_the_connect_waits_whole_for_the_car();
    test_room_reaches_zero_and_no_further();
    printf("test_tcp_pending: all passed\n");
    return 0;
}
