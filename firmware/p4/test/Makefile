CC = cc
CFLAGS = -I../main -Wall -Wextra -Werror -std=c11
LDLIBS = -lm
# The schema itself, for the two wire values the C generator emits no symbol for.
CONTRACT = $(abspath ../../../contract/car-api.json)

all: test_mixer test_motors test_control_proto test_watchdog test_calibration test_ramp test_trim test_telemetry test_recovery test_wheel test_cfg_table test_link test_rt_session test_rt_glue test_contract_wire test_radio_ota

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

test_radio_ota: test_radio_ota.c ../main/radio_ota.c
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

# The session lifecycle, against the real parser: the datagrams are wire bytes, so the
# frames the rules are applied to are the ones the car would actually build.
test_rt_session: test_rt_session.c ../main/rt_link.h ../main/control_proto.c
	$(CC) $(CFLAGS) -o $@ test_rt_session.c ../main/control_proto.c $(LDLIBS)

test_contract_wire: test_contract_wire.c ../main/telemetry.h ../main/identity.h $(CONTRACT)
	$(CC) $(CFLAGS) -DCONTRACT_JSON='"$(CONTRACT)"' -o $@ $< $(LDLIBS)

# The lifecycle's side effects over a recording effects table — the impure orderings
# rt_link.c executes, pinned the way test_state.py pins them on the mock.
test_rt_glue: test_rt_glue.c ../main/rt_glue.h ../main/rt_link.h ../main/link.h
	$(CC) $(CFLAGS) -o $@ test_rt_glue.c $(LDLIBS)

run: all
	./test_mixer && ./test_motors && ./test_control_proto && ./test_watchdog && ./test_calibration && ./test_ramp && ./test_trim && ./test_radio_ota && ./test_telemetry && ./test_recovery && ./test_wheel && ./test_cfg_table && ./test_link && ./test_rt_session && ./test_rt_glue && ./test_contract_wire

clean:
	rm -f test_contract_wire test_rt_session test_rt_glue test_link test_cfg_table test_mixer test_motors test_control_proto test_watchdog test_calibration test_ramp test_trim test_radio_ota test_telemetry test_recovery test_wheel
