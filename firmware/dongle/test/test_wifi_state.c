#include <stdio.h>
#include <string.h>

#include "wifi_state.h"

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) { printf("FAIL: %s\n", what); failures++; }
}

static void test_idle_until_configured(void)
{
    wifi_sm_t sm;
    wifi_state_init(&sm);
    check(sm.state == WIFI_IDLE, "starts idle");
    check(!wifi_state_step(&sm, WIFI_EV_DISCONNECTED), "idle ignores a disconnect");
    check(sm.state == WIFI_IDLE, "idle stays idle without a configuration");
}

static void test_configured_starts_joining(void)
{
    wifi_sm_t sm;
    wifi_state_init(&sm);
    check(wifi_state_step(&sm, WIFI_EV_CONFIGURED), "configuring asks for a connect");
    check(sm.state == WIFI_JOINING, "configuring enters joining");
}

static void test_got_ip_is_connected(void)
{
    wifi_sm_t sm;
    wifi_state_init(&sm);
    wifi_state_step(&sm, WIFI_EV_CONFIGURED);
    check(!wifi_state_step(&sm, WIFI_EV_GOT_IP), "an address does not ask for another connect");
    check(sm.state == WIFI_CONNECTED, "an address means connected");
}

static void test_retries_are_bounded(void)
{
    wifi_sm_t sm;
    wifi_state_init(&sm);
    wifi_state_step(&sm, WIFI_EV_CONFIGURED);
    for (int i = 1; i < WIFI_JOIN_ATTEMPTS; i++) {
        check(wifi_state_step(&sm, WIFI_EV_DISCONNECTED), "a failed attempt retries");
        check(sm.state == WIFI_JOINING, "still joining while the budget lasts");
    }
    check(!wifi_state_step(&sm, WIFI_EV_DISCONNECTED), "the last failure does not retry");
    check(sm.state == WIFI_FAILED, "the budget runs out into failed");
}

static void test_failed_is_held(void)
{
    wifi_sm_t sm;
    wifi_state_init(&sm);
    wifi_state_step(&sm, WIFI_EV_CONFIGURED);
    for (int i = 0; i < WIFI_JOIN_ATTEMPTS; i++) wifi_state_step(&sm, WIFI_EV_DISCONNECTED);
    check(sm.state == WIFI_FAILED, "failed after the budget");
    check(!wifi_state_step(&sm, WIFI_EV_DISCONNECTED), "failed does not retry on its own");
    check(sm.state == WIFI_FAILED, "failed is held, not left");
}

static void test_a_new_configuration_leaves_failed(void)
{
    wifi_sm_t sm;
    wifi_state_init(&sm);
    wifi_state_step(&sm, WIFI_EV_CONFIGURED);
    for (int i = 0; i < WIFI_JOIN_ATTEMPTS; i++) wifi_state_step(&sm, WIFI_EV_DISCONNECTED);
    check(wifi_state_step(&sm, WIFI_EV_CONFIGURED), "a new POST asks for a connect");
    check(sm.state == WIFI_JOINING, "a new POST leaves failed");
}

/* The budget is counted in CONNECT REQUESTS, because that is what a "join attempt" is. From
 * CONFIGURED the first request is wifi_sta_join's own esp_wifi_connect and the machine issues
 * four more (four `true`s) before giving up: five. From a drop the machine issues the first
 * request itself — the drop's DISCONNECTED returns true — so a fresh budget is five `true`s
 * from the drop onward, and the counter behind them must open at 0, not 1.
 *
 * This test used to count DISCONNECTED EVENTS instead, and defended attempts = 1 on the drop
 * by name. Event-counting made the two entry points look alike while they were not: a
 * reconnect got four connects to CONFIGURED's five, and the panel — which renders the
 * ordinal attempts + 1 — opened a reconnect at «Попытка 2 из 5» with nothing having failed
 * yet. */
static void test_a_dropped_link_rejoins_with_a_full_budget(void)
{
    wifi_sm_t sm;
    wifi_state_init(&sm);
    wifi_state_step(&sm, WIFI_EV_CONFIGURED);
    wifi_state_step(&sm, WIFI_EV_GOT_IP);

    int connects = 0;
    if (wifi_state_step(&sm, WIFI_EV_DISCONNECTED)) connects++;
    check(sm.state == WIFI_JOINING, "a dropped link goes back to joining");
    check(sm.attempts == 0, "and opens a fresh budget: none consumed yet");

    while (sm.state == WIFI_JOINING) {
        if (wifi_state_step(&sm, WIFI_EV_DISCONNECTED)) connects++;
    }
    check(sm.state == WIFI_FAILED, "a full fresh budget ends in failed, same as the first one");
    check(connects == WIFI_JOIN_ATTEMPTS, "five connect requests after a drop, not four");
}

/* The same count from CONFIGURED, so the two entry points are pinned to the same number: one
 * connect that wifi_sta_join issues outside the machine, plus every `true` the machine
 * returns. */
static void test_a_configured_join_gets_the_same_budget(void)
{
    wifi_sm_t sm;
    wifi_state_init(&sm);
    int connects = wifi_state_step(&sm, WIFI_EV_CONFIGURED) ? 1 : 0;
    while (sm.state == WIFI_JOINING) {
        if (wifi_state_step(&sm, WIFI_EV_DISCONNECTED)) connects++;
    }
    check(connects == WIFI_JOIN_ATTEMPTS, "five connect requests from a configuration");
}

/* wifi_sta_join can fail between tearing the old association down and asking for the new
 * one — esp_wifi_set_config or esp_wifi_connect refusing. The radio is then idle with no
 * request in flight and nothing coming to retry it, and the machine has to say so: not
 * "connected" to a link that was just dropped, not "joining" with nothing joining. FAILED is
 * the state whose meaning that already is — no further attempts until a new configuration. */
static void test_an_aborted_join_is_failed_from_anywhere(void)
{
    wifi_sm_t sm;

    wifi_state_init(&sm);
    wifi_state_step(&sm, WIFI_EV_CONFIGURED);
    wifi_state_step(&sm, WIFI_EV_GOT_IP);
    check(!wifi_state_step(&sm, WIFI_EV_ABORTED), "an abort asks for no connect");
    check(sm.state == WIFI_FAILED, "an abort from connected is failed, not connected");

    wifi_state_init(&sm);
    wifi_state_step(&sm, WIFI_EV_CONFIGURED);
    check(!wifi_state_step(&sm, WIFI_EV_ABORTED), "an abort asks for no connect");
    check(sm.state == WIFI_FAILED, "an abort from joining is failed, not joining");

    wifi_state_init(&sm);
    wifi_state_step(&sm, WIFI_EV_ABORTED);
    check(sm.state == WIFI_FAILED, "an abort before anything was ever configured is failed");

    /* And a new configuration still leaves it, like every other FAILED. */
    check(wifi_state_step(&sm, WIFI_EV_CONFIGURED), "a re-POST retries");
    check(sm.state == WIFI_JOINING && sm.attempts == 0, "with the whole budget");
}

static void test_a_late_address_leaves_failed(void)
{
    /* WIFI_FAILED means no further connection attempts are made — it does not mean an
       attempt already in flight is disowned. The realistic path: the budget's last attempt
       finally associates and gets an address just after wifi_state_step already reported
       WIFI_FAILED for the DISCONNECTED that preceded it. That address is proof the join
       worked and must be believed, not discarded because the state machine gave up on
       asking for more attempts. */
    wifi_sm_t sm;
    wifi_state_init(&sm);
    wifi_state_step(&sm, WIFI_EV_CONFIGURED);
    for (int i = 0; i < WIFI_JOIN_ATTEMPTS; i++) wifi_state_step(&sm, WIFI_EV_DISCONNECTED);
    check(sm.state == WIFI_FAILED, "failed after the budget, as the other tests already show");
    check(!wifi_state_step(&sm, WIFI_EV_GOT_IP), "a late address does not ask for another connect");
    check(sm.state == WIFI_CONNECTED, "a late address is still believed, even out of a failed budget");
}

static void test_renewal_does_not_disturb_connected(void)
{
    wifi_sm_t sm;
    wifi_state_init(&sm);
    wifi_state_step(&sm, WIFI_EV_CONFIGURED);
    wifi_state_step(&sm, WIFI_EV_GOT_IP);
    check(!wifi_state_step(&sm, WIFI_EV_GOT_IP), "a DHCP renewal asks for nothing");
    check(sm.state == WIFI_CONNECTED, "a DHCP renewal leaves the state alone");
}

static void test_names_are_the_contract_s(void)
{
    wifi_sm_t sm;
    wifi_state_init(&sm);
    check(strcmp(wifi_state_name(&sm), DONGLE_STATE_IDLE) == 0, "idle spells the contract's word");
    wifi_state_step(&sm, WIFI_EV_CONFIGURED);
    check(strcmp(wifi_state_name(&sm), DONGLE_STATE_JOINING) == 0, "joining spells it");
    wifi_state_step(&sm, WIFI_EV_GOT_IP);
    check(strcmp(wifi_state_name(&sm), DONGLE_STATE_CONNECTED) == 0, "connected spells it");
    wifi_state_init(&sm);
    wifi_state_step(&sm, WIFI_EV_CONFIGURED);
    for (int i = 0; i < WIFI_JOIN_ATTEMPTS; i++) wifi_state_step(&sm, WIFI_EV_DISCONNECTED);
    check(strcmp(wifi_state_name(&sm), DONGLE_STATE_FAILED) == 0, "failed spells it");
}

int main(void)
{
    test_idle_until_configured();
    test_configured_starts_joining();
    test_got_ip_is_connected();
    test_retries_are_bounded();
    test_failed_is_held();
    test_a_new_configuration_leaves_failed();
    test_a_dropped_link_rejoins_with_a_full_budget();
    test_a_configured_join_gets_the_same_budget();
    test_an_aborted_join_is_failed_from_anywhere();
    test_a_late_address_leaves_failed();
    test_renewal_does_not_disturb_connected();
    test_names_are_the_contract_s();
    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("wifi_state: all checks passed\n");
    return 0;
}
