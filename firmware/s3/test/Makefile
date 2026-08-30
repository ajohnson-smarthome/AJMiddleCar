CC = cc
CFLAGS = -I../main -Wall -Wextra -Werror -std=c11

all: test_net_cfg test_wifi_state test_udp_sess

test_net_cfg: test_net_cfg.c ../main/net_cfg.c ../main/net_cfg.h ../main/dongle_contract.inc
	$(CC) $(CFLAGS) -o $@ test_net_cfg.c ../main/net_cfg.c

test_wifi_state: test_wifi_state.c ../main/wifi_state.c ../main/wifi_state.h ../main/dongle_contract.inc
	$(CC) $(CFLAGS) -o $@ test_wifi_state.c ../main/wifi_state.c

test_udp_sess: test_udp_sess.c ../main/udp_sess.c ../main/udp_sess.h
	$(CC) $(CFLAGS) -o $@ test_udp_sess.c ../main/udp_sess.c

run: all
	./test_net_cfg
	./test_wifi_state
	./test_udp_sess

clean:
	rm -f test_net_cfg test_wifi_state test_udp_sess
