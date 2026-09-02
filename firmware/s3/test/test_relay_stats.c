#include <stdio.h>
#include "relay_stats.h"

static int failures;
static void check(int ok, const char *what)
{
    if (!ok) { printf("FAIL: %s\n", what); failures++; }
}

static void test_rate_is_tenths_of_a_packet_per_second(void)
{
    relay_stats_t s;
    relay_stats_init(&s, 4, 4);
    relay_stats_sample(&s, 0);                       /* start the window */
    for (int i = 0; i < 10; i++) relay_stats_forwarded(&s, true);
    for (int i = 0; i < 5; i++) relay_stats_forwarded(&s, false);
    relay_stats_sample(&s, 1000);
    check(s.to_car_x10 == 100, "10 packets in 1 s is 10.0 to the car");
    check(s.to_phone_x10 == 50, "5 packets in 1 s is 5.0 to the phone");
}

static void test_a_second_window_does_not_count_the_first(void)
{
    relay_stats_t s;
    relay_stats_init(&s, 4, 4);
    relay_stats_sample(&s, 0);
    for (int i = 0; i < 10; i++) relay_stats_forwarded(&s, true);
    relay_stats_sample(&s, 1000);
    relay_stats_sample(&s, 2000);
    check(s.to_car_x10 == 0, "an idle window reads zero, not the previous rate");
}

static void test_half_second_window_scales(void)
{
    relay_stats_t s;
    relay_stats_init(&s, 4, 4);
    relay_stats_sample(&s, 0);
    for (int i = 0; i < 5; i++) relay_stats_forwarded(&s, true);
    relay_stats_sample(&s, 500);
    check(s.to_car_x10 == 100, "5 packets in half a second is 10.0/s");
}

static void test_zero_length_window_keeps_the_previous_reading(void)
{
    relay_stats_t s;
    relay_stats_init(&s, 4, 4);
    relay_stats_sample(&s, 0);
    for (int i = 0; i < 10; i++) relay_stats_forwarded(&s, true);
    relay_stats_sample(&s, 1000);
    relay_stats_sample(&s, 1000);   /* same timestamp: no time has passed */
    check(s.to_car_x10 == 100, "a zero-length window does not divide by zero");
}

static void test_errno_latches_and_counts_repeats(void)
{
    relay_stats_t s;
    relay_stats_init(&s, 4, 4);
    check(s.last_errno == 0 && s.errno_count == 0, "starts with no fault");
    relay_stats_failed(&s, 12);
    relay_stats_failed(&s, 12);
    check(s.last_errno == 12 && s.errno_count == 2, "the same fault twice counts twice");
    relay_stats_failed(&s, 118);
    check(s.last_errno == 118 && s.errno_count == 1,
          "a different fault replaces it and restarts the count");
}

static void test_each_relay_records_only_its_own_slots(void)
{
    relay_stats_t s;
    relay_stats_init(&s, 4, 4);
    relay_stats_udp_slots(&s, 1);
    relay_stats_tcp_slots(&s, 3);
    check(s.udp_used == 1 && s.udp_max == 4, "udp slots");
    check(s.tcp_used == 3 && s.tcp_max == 4, "tcp slots");
}

int main(void)
{
    test_rate_is_tenths_of_a_packet_per_second();
    test_a_second_window_does_not_count_the_first();
    test_half_second_window_scales();
    test_zero_length_window_keeps_the_previous_reading();
    test_errno_latches_and_counts_repeats();
    test_each_relay_records_only_its_own_slots();
    printf(failures ? "relay_stats: %d FAILED\n" : "relay_stats: ok\n", failures);
    return failures != 0;
}
