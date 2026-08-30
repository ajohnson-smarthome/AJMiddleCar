#include "../main/udp_sess.h"
#include <assert.h>
#include <stdio.h>

/* Five made-up (address, port) pairs. udp_sess treats both as opaque numbers — any distinct
 * values would do, and none of them needs to look like a real IPv4 address. */
#define ADDR_A 0x0A000001u
#define ADDR_B 0x0A000002u
#define ADDR_C 0x0A000003u
#define ADDR_D 0x0A000004u
#define ADDR_E 0x0A000005u
#define PORT_1 50001u
#define PORT_2 50002u

static void test_new_peer_takes_a_free_slot(void) {
    udp_sess_table_t t;
    udp_sess_init(&t);
    int idx = udp_sess_touch(&t, ADDR_A, PORT_1, 1000u);
    assert(idx >= 0 && idx < UDP_SESS_MAX);
    assert(t.s[idx].used);
    assert(t.s[idx].addr == ADDR_A);
    assert(t.s[idx].port == PORT_1);
    assert(t.s[idx].last_ms == 1000u);
}

static void test_same_peer_returns_the_same_slot(void) {
    udp_sess_table_t t;
    udp_sess_init(&t);
    int first = udp_sess_touch(&t, ADDR_A, PORT_1, 1000u);
    int second = udp_sess_touch(&t, ADDR_A, PORT_1, 2000u);
    assert(first == second);
}

static void test_a_different_port_is_a_different_session(void) {
    udp_sess_table_t t;
    udp_sess_init(&t);
    int a = udp_sess_touch(&t, ADDR_A, PORT_1, 1000u);
    int b = udp_sess_touch(&t, ADDR_A, PORT_2, 1000u);
    assert(a != b);
    assert(udp_sess_find(&t, ADDR_A, PORT_1) == a);
    assert(udp_sess_find(&t, ADDR_A, PORT_2) == b);
}

static void test_a_different_address_is_a_different_session(void) {
    /* Same claim as the port test, the other half of what identifies a peer. */
    udp_sess_table_t t;
    udp_sess_init(&t);
    int a = udp_sess_touch(&t, ADDR_A, PORT_1, 1000u);
    int b = udp_sess_touch(&t, ADDR_B, PORT_1, 1000u);
    assert(a != b);
}

static void test_find_is_negative_one_for_an_unknown_peer(void) {
    udp_sess_table_t t;
    udp_sess_init(&t);
    assert(udp_sess_find(&t, ADDR_A, PORT_1) == -1);
    udp_sess_touch(&t, ADDR_A, PORT_1, 1000u);
    assert(udp_sess_find(&t, ADDR_B, PORT_1) == -1);
}

static void test_find_does_not_create_or_touch_the_deadline(void) {
    udp_sess_table_t t;
    udp_sess_init(&t);
    int idx = udp_sess_touch(&t, ADDR_A, PORT_1, 1000u);
    assert(udp_sess_find(&t, ADDR_A, PORT_1) == idx);
    assert(t.s[idx].last_ms == 1000u);  /* a lookup alone must not move the deadline */
}

static void test_the_table_fills_and_then_evicts_the_least_recently_used(void) {
    /* Filled out of chronological order on purpose: C, A, D, B land in slots 0..3 with
     * last_ms 300, 100, 400, 200 respectively. An eviction rule that picked the first-filled
     * slot instead of the actually-smallest last_ms would still pass a fill-in-order version
     * of this test, so the fill order and the recency order are deliberately made to disagree.
     * The true LRU is A, at last_ms 100, sitting in slot 1 — not slot 0. */
    udp_sess_table_t t;
    udp_sess_init(&t);
    assert(UDP_SESS_MAX == 4);  /* this test's shape assumes exactly four slots */
    udp_sess_touch(&t, ADDR_C, PORT_1, 300u);
    int a = udp_sess_touch(&t, ADDR_A, PORT_1, 100u);
    udp_sess_touch(&t, ADDR_D, PORT_1, 400u);
    udp_sess_touch(&t, ADDR_B, PORT_1, 200u);

    int e = udp_sess_touch(&t, ADDR_E, PORT_1, 500u);
    assert(e == a);  /* the new peer took A's slot specifically, not merely a free-looking one */
    assert(udp_sess_find(&t, ADDR_A, PORT_1) == -1);  /* A is gone */
    assert(udp_sess_find(&t, ADDR_E, PORT_1) == e);
    assert(udp_sess_find(&t, ADDR_C, PORT_1) != -1);  /* nobody else was touched */
    assert(udp_sess_find(&t, ADDR_D, PORT_1) != -1);
    assert(udp_sess_find(&t, ADDR_B, PORT_1) != -1);
}

static void test_touching_an_existing_peer_on_a_full_table_evicts_no_one(void) {
    udp_sess_table_t t;
    udp_sess_init(&t);
    int a = udp_sess_touch(&t, ADDR_A, PORT_1, 100u);
    udp_sess_touch(&t, ADDR_B, PORT_1, 200u);
    udp_sess_touch(&t, ADDR_C, PORT_1, 300u);
    udp_sess_touch(&t, ADDR_D, PORT_1, 400u);

    int again = udp_sess_touch(&t, ADDR_A, PORT_1, 900u);  /* A, again, table already full */
    assert(again == a);
    assert(udp_sess_find(&t, ADDR_B, PORT_1) != -1);
    assert(udp_sess_find(&t, ADDR_C, PORT_1) != -1);
    assert(udp_sess_find(&t, ADDR_D, PORT_1) != -1);
    assert(t.s[a].last_ms == 900u);
}

static void test_touch_never_refuses_a_full_table(void) {
    /* "Never fails" is the header's own claim: a relay that refused a packet because its
     * table happened to be full of abandoned sessions would drop a live drive instead. */
    udp_sess_table_t t;
    udp_sess_init(&t);
    udp_sess_touch(&t, ADDR_A, PORT_1, 100u);
    udp_sess_touch(&t, ADDR_B, PORT_1, 200u);
    udp_sess_touch(&t, ADDR_C, PORT_1, 300u);
    udp_sess_touch(&t, ADDR_D, PORT_1, 400u);
    int idx = udp_sess_touch(&t, ADDR_E, PORT_1, 500u);
    assert(idx >= 0 && idx < UDP_SESS_MAX);
}

static void test_expiry_frees_a_slot(void) {
    udp_sess_table_t t;
    udp_sess_init(&t);
    int idx = udp_sess_touch(&t, ADDR_A, PORT_1, 0u);
    uint32_t freed = udp_sess_expire(&t, 20000u, UDP_SESS_IDLE_MS);
    assert(freed == (1u << idx));
    assert(!t.s[idx].used);
    assert(udp_sess_find(&t, ADDR_A, PORT_1) == -1);
}

static void test_expiry_leaves_a_fresh_session_alone(void) {
    udp_sess_table_t t;
    udp_sess_init(&t);
    int idx = udp_sess_touch(&t, ADDR_A, PORT_1, 1000u);
    uint32_t freed = udp_sess_expire(&t, 5000u, UDP_SESS_IDLE_MS);  /* 4000ms idle, well under 10s */
    assert(freed == 0u);
    assert(t.s[idx].used);
    assert(udp_sess_find(&t, ADDR_A, PORT_1) == idx);
}

static void test_a_touch_moves_a_sessions_deadline(void) {
    udp_sess_table_t t;
    udp_sess_init(&t);
    int idx = udp_sess_touch(&t, ADDR_A, PORT_1, 0u);
    /* Without the touch below, 0 + UDP_SESS_IDLE_MS = 10000 would already be gone by 15000. */
    udp_sess_touch(&t, ADDR_A, PORT_1, 9000u);
    uint32_t freed = udp_sess_expire(&t, 15000u, UDP_SESS_IDLE_MS);
    assert(freed == 0u);
    assert(t.s[idx].used);
}

static void test_expire_boundary_is_exact(void) {
    /* "silent for longer than idle_ms" is a strict >, not >=: exactly idle_ms of silence is
     * not yet expired, one more ms of it is. */
    udp_sess_table_t t;
    udp_sess_init(&t);
    int idx = udp_sess_touch(&t, ADDR_A, PORT_1, 1000u);
    assert(udp_sess_expire(&t, 1000u + UDP_SESS_IDLE_MS, UDP_SESS_IDLE_MS) == 0u);
    assert(t.s[idx].used);
    assert(udp_sess_expire(&t, 1000u + UDP_SESS_IDLE_MS + 1u, UDP_SESS_IDLE_MS) == (1u << idx));
}

static void test_expire_returns_a_bitmask_of_every_freed_slot(void) {
    udp_sess_table_t t;
    udp_sess_init(&t);
    int a = udp_sess_touch(&t, ADDR_A, PORT_1, 0u);
    int b = udp_sess_touch(&t, ADDR_B, PORT_1, 0u);
    int c = udp_sess_touch(&t, ADDR_C, PORT_1, 15000u);  /* only 5000ms idle at t=20000: stays fresh */
    uint32_t freed = udp_sess_expire(&t, 20000u, UDP_SESS_IDLE_MS);
    assert(freed == ((1u << a) | (1u << b)));
    assert(t.s[c].used);
}

int main(void) {
    test_new_peer_takes_a_free_slot();
    test_same_peer_returns_the_same_slot();
    test_a_different_port_is_a_different_session();
    test_a_different_address_is_a_different_session();
    test_find_is_negative_one_for_an_unknown_peer();
    test_find_does_not_create_or_touch_the_deadline();
    test_the_table_fills_and_then_evicts_the_least_recently_used();
    test_touching_an_existing_peer_on_a_full_table_evicts_no_one();
    test_touch_never_refuses_a_full_table();
    test_expiry_frees_a_slot();
    test_expiry_leaves_a_fresh_session_alone();
    test_a_touch_moves_a_sessions_deadline();
    test_expire_boundary_is_exact();
    test_expire_returns_a_bitmask_of_every_freed_slot();
    printf("test_udp_sess: all passed\n");
    return 0;
}
