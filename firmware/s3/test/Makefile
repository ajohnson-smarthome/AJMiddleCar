CC = cc
CFLAGS = -I../main -Wall -Wextra -Werror -std=c11

all: test_net_cfg test_wifi_state

test_net_cfg: test_net_cfg.c ../main/net_cfg.c ../main/net_cfg.h ../main/dongle_contract.inc
	$(CC) $(CFLAGS) -o $@ test_net_cfg.c ../main/net_cfg.c

test_wifi_state: test_wifi_state.c ../main/wifi_state.c ../main/wifi_state.h ../main/dongle_contract.inc
	$(CC) $(CFLAGS) -o $@ test_wifi_state.c ../main/wifi_state.c

run: all
	./test_net_cfg
	./test_wifi_state

clean:
	rm -f test_net_cfg test_wifi_state
