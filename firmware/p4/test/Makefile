CC = cc
CFLAGS = -I../main -Wall -Wextra -Werror -std=c11
LDLIBS = -lm
# The schema itself, for the two wire values the C generator emits no symbol for.
CONTRACT = $(abspath ../../../contract/car-api.json)

all: test_mixer test_motors test_control_proto test_watchdog test_calibration test_ramp test_trim test_telemetry test_recovery test_wheel test_cfg_table test_link test_contract_wire

test_mixer: test_mixer.c ../main/mixer.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_motors: test_motors.c ../main/motors.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_control_proto: test_control_proto.c ../main/control_proto.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_watchdog: test_watchdog.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_calibration: test_calibration.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_ramp: test_ramp.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_trim: test_trim.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_telemetry: test_telemetry.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_recovery: test_recovery.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_wheel: test_wheel.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_cfg_table: test_cfg_table.c ../main/cfg_table.inc ../main/cfg_contract.h
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

test_link: test_link.c ../main/link.h ../main/cfg_table.inc
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

test_contract_wire: test_contract_wire.c ../main/telemetry.h ../main/identity.h $(CONTRACT)
	$(CC) $(CFLAGS) -DCONTRACT_JSON='"$(CONTRACT)"' -o $@ $< $(LDLIBS)

run: all
	./test_mixer && ./test_motors && ./test_control_proto && ./test_watchdog && ./test_calibration && ./test_ramp && ./test_trim && ./test_telemetry && ./test_recovery && ./test_wheel && ./test_cfg_table && ./test_link && ./test_contract_wire

clean:
	rm -f test_contract_wire test_link test_cfg_table test_mixer test_motors test_control_proto test_watchdog test_calibration test_ramp test_trim test_telemetry test_recovery test_wheel
