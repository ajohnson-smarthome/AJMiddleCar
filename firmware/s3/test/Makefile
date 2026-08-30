CC = cc
CFLAGS = -I../main -Wall -Wextra -Werror -std=c11

all: test_net_cfg

test_net_cfg: test_net_cfg.c ../main/net_cfg.c ../main/net_cfg.h ../main/dongle_contract.inc
	$(CC) $(CFLAGS) -o $@ test_net_cfg.c ../main/net_cfg.c

run: all
	./test_net_cfg

clean:
	rm -f test_net_cfg
