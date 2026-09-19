CC = cc
CFLAGS = -I../main -Wall -Wextra -Werror -std=c11

all: test_net_cfg test_wifi_state test_udp_sess test_tcp_pending test_relay_stats test_screens test_status_json test_rate_gate test_uplink test_ota_reply

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

test_screens: test_screens.c ../main/screens.c ../main/screens.h ../main/dongle_contract.inc
	$(CC) $(CFLAGS) -o $@ test_screens.c ../main/screens.c

test_status_json: test_status_json.c ../main/status_json.c ../main/status_json.h ../main/net_cfg.c ../main/dongle_contract.inc
	$(CC) $(CFLAGS) -o $@ test_status_json.c ../main/status_json.c ../main/net_cfg.c

test_rate_gate: test_rate_gate.c ../main/rate_gate.c ../main/rate_gate.h
	$(CC) $(CFLAGS) -o $@ test_rate_gate.c ../main/rate_gate.c $(LDLIBS)

test_uplink: test_uplink.c ../main/uplink.c ../main/uplink.h
	$(CC) $(CFLAGS) -o $@ test_uplink.c ../main/uplink.c

# The map from an esp_ota_* refusal to the error envelope, the twin of the car's: which IDF
# codes are the body's fault (400), which the board's (500), and the one that is a refusal to
# start over an unconfirmed image (409). Pinned here because the handler once answered with
# an envelope and then returned ESP_FAIL, and the envelope never reached the client (AJM-137).
test_ota_reply: test_ota_reply.c ../main/ota_reply.h ../main/dongle_contract.inc
	$(CC) $(CFLAGS) -o $@ $<

run: all
	./test_net_cfg
	./test_wifi_state
	./test_udp_sess
	./test_tcp_pending
	./test_relay_stats
	./test_screens
	./test_status_json
	./test_rate_gate
	./test_uplink
	./test_ota_reply

clean:
	rm -f test_net_cfg test_wifi_state test_udp_sess test_tcp_pending test_relay_stats test_screens test_status_json test_rate_gate test_uplink test_ota_reply
