CC = cc
CFLAGS = -I../main -Wall -Wextra -Werror -std=c11

all: test_net_cfg test_wifi_state test_udp_sess test_tcp_pending test_relay_stats

test_net_cfg: test_net_cfg.c ../main/net_cfg.c ../main/net_cfg.h ../main/dongle_contract.inc
	$(CC) $(CFLAGS) -o $@ test_net_cfg.c ../main/net_cfg.c

test_wifi_state: test_wifi_state.c ../main/wifi_state.c ../main/wifi_state.h ../main/dongle_contract.inc
	$(CC) $(CFLAGS) -o $@ test_wifi_state.c ../main/wifi_state.c

test_udp_sess: test_udp_sess.c ../main/udp_sess.c ../main/udp_sess.h
	$(CC) $(CFLAGS) -o $@ test_udp_sess.c ../main/udp_sess.c

test_tcp_pending: test_tcp_pending.c ../main/tcp_pending.c ../main/tcp_pending.h
	$(CC) $(CFLAGS) -o $@ test_tcp_pending.c ../main/tcp_pending.c

test_relay_stats: test_relay_stats.c ../main/relay_stats.c ../main/relay_stats.h
	$(CC) $(CFLAGS) -o $@ test_relay_stats.c ../main/relay_stats.c

run: all
	./test_net_cfg
	./test_wifi_state
	./test_udp_sess
	./test_tcp_pending
	./test_relay_stats

clean:
	rm -f test_net_cfg test_wifi_state test_udp_sess test_tcp_pending test_relay_stats
