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
    printf("test_tcp_pending: all passed\n");
    return 0;
}
