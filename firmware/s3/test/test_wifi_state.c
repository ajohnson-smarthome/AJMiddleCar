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

static void test_a_dropped_link_rejoins_with_a_full_budget(void)
{
    wifi_sm_t sm;
    wifi_state_init(&sm);
    wifi_state_step(&sm, WIFI_EV_CONFIGURED);
    wifi_state_step(&sm, WIFI_EV_GOT_IP);
    check(wifi_state_step(&sm, WIFI_EV_DISCONNECTED), "a dropped link retries");
    check(sm.state == WIFI_JOINING, "a dropped link goes back to joining");
    /* A link that worked once gets the whole budget again, not the remainder of an old one:
       the car powering off and on is the ordinary case, not an escalating failure. */
    for (int i = 2; i < WIFI_JOIN_ATTEMPTS; i++) {
        check(wifi_state_step(&sm, WIFI_EV_DISCONNECTED), "the budget restarted");
    }
    check(sm.state == WIFI_JOINING, "still joining at the end of a full budget");
    /* Five DISCONNECTEDs total since the reconnect (the one above the loop, plus the loop's
       three, plus this one) is the claim itself: a fresh budget of WIFI_JOIN_ATTEMPTS, not
       the remainder of the one spent before GOT_IP. A bug that granted attempts = 0 instead
       of 1 on reconnect would still pass every check above; only running the budget to
       exhaustion catches it. */
    check(!wifi_state_step(&sm, WIFI_EV_DISCONNECTED), "the restarted budget still runs out");
    check(sm.state == WIFI_FAILED, "a full fresh budget ends in failed, same as the first one");
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
    test_a_late_address_leaves_failed();
    test_renewal_does_not_disturb_connected();
    test_names_are_the_contract_s();
    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("wifi_state: all checks passed\n");
    return 0;
}
