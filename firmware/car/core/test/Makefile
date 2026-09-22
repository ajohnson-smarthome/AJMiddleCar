CC = cc
CFLAGS = -I../main -Wall -Wextra -Werror -std=c11
LDLIBS = -lm
# The schema itself, for the two wire values the C generator emits no symbol for.
CONTRACT = $(abspath ../../../../contract/car-api.json)

all: test_mixer test_motors test_control_proto test_watchdog test_calibration test_ramp test_trim test_telemetry test_recovery test_wheel test_cfg_table test_cfg_value test_calib_wire test_link test_rt_session test_rt_glue test_contract_wire test_radio_ota test_device_json test_video_wire test_video_sub test_video_retry test_frame_crop test_ota_reply test_calib_spin test_battery_soc test_battery

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

# The state-of-charge rule of car/battery-monitor (start window, coulombs, rest pull-in),
# in the numbers of its scenarios — the pack's constants are battery_pack.h's.
test_battery_soc: test_battery_soc.c ../main/battery_soc.c ../main/battery_soc.h ../main/battery_pack.h
	$(CC) $(CFLAGS) -o $@ test_battery_soc.c ../main/battery_soc.c $(LDLIBS)

# The monitor's verdict of car/battery-monitor — `absent` after N failed reads, `ok` on the
# next good one, `low` with hysteresis — pinned here; the task in battery.c only feeds it.
test_battery: test_battery.c ../main/battery.h ../main/battery_pack.h ../main/cfg_table.inc
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

test_trim: test_trim.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_radio_ota: test_radio_ota.c ../main/radio_ota.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_telemetry: test_telemetry.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_recovery: test_recovery.c ../main/recovery.h ../main/cfg_table.inc ../main/cfg_contract.h
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

test_wheel: test_wheel.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_cfg_table: test_cfg_table.c ../main/cfg_table.inc ../main/cfg_contract.h
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

test_cfg_value: test_cfg_value.c ../main/cfg_value.h ../main/cfg_table.inc ../main/cfg_contract.h
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

test_calib_wire: test_calib_wire.c ../main/calib_wire.h ../main/cfg_table.inc
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

test_link: test_link.c ../main/link.h ../main/cfg_table.inc
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# POST /calibration/spin's sequence after validation, over a recorder: which effects a
# refusal skips is what the wizard's 409 promises (AJM-100: a pulse on a dead bus said ok).
test_calib_spin: test_calib_spin.c ../main/calib_spin.h ../main/cfg_table.inc
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# The session lifecycle, against the real parser: the datagrams are wire bytes, so the
# frames the rules are applied to are the ones the car would actually build.
test_rt_session: test_rt_session.c ../main/rt_link.h ../main/control_proto.c
	$(CC) $(CFLAGS) -o $@ test_rt_session.c ../main/control_proto.c $(LDLIBS)

test_contract_wire: test_contract_wire.c ../main/telemetry.h ../main/identity.h $(CONTRACT)
	$(CC) $(CFLAGS) -DCONTRACT_JSON='"$(CONTRACT)"' -o $@ $< $(LDLIBS)

test_device_json: test_device_json.c ../main/device_json.h ../main/identity.h ../main/cfg_table.inc
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

test_video_wire: test_video_wire.c ../main/video_wire.h ../main/cfg_table.inc
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

test_video_sub: test_video_sub.c ../main/video_sub.h ../main/control_proto.c ../main/cfg_table.inc
	$(CC) $(CFLAGS) -o $@ test_video_sub.c ../main/control_proto.c $(LDLIBS)

# The series of failed stream starts: N in a row without a frame reads `off`, and the
# retries after that are rarer than once a second (AJM-94: a sensor dead mid-run read
# `idle` — "nobody watching" — while the driver watched, and the log ran at 1 Hz).
test_video_retry: test_video_retry.c ../main/video_retry.h
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

test_frame_crop: test_frame_crop.c ../main/frame_crop.h ../main/cfg_table.inc
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# The map from an esp_ota_* refusal to the error envelope: which IDF codes are the body's
# fault (400) and which the board's (500), pinned here because the one time it was written
# inline the first block's rejected header answered as a flash failure (AJM-37).
test_ota_reply: test_ota_reply.c ../main/ota_reply.h ../main/cfg_table.inc
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# The lifecycle's side effects over a recording effects table — the impure orderings
# rt_link.c executes, pinned the way test_state.py pins them on the mock.
test_rt_glue: test_rt_glue.c ../main/rt_glue.h ../main/rt_link.h ../main/link.h ../main/recovery.h
	$(CC) $(CFLAGS) -o $@ test_rt_glue.c $(LDLIBS)

run: all
	./test_mixer && ./test_motors && ./test_control_proto && ./test_watchdog && ./test_calibration && ./test_ramp && ./test_trim && ./test_radio_ota && ./test_telemetry && ./test_recovery && ./test_wheel && ./test_cfg_table && ./test_cfg_value && ./test_calib_wire && ./test_link && ./test_rt_session && ./test_rt_glue && ./test_contract_wire && ./test_device_json && ./test_video_wire && ./test_video_sub && ./test_video_retry && ./test_frame_crop && ./test_ota_reply && ./test_calib_spin && ./test_battery_soc && ./test_battery

clean:
	rm -f test_contract_wire test_rt_session test_rt_glue test_link test_cfg_table test_cfg_value test_calib_wire test_mixer test_motors test_control_proto test_watchdog test_calibration test_ramp test_trim test_radio_ota test_telemetry test_recovery test_wheel test_device_json test_video_wire test_video_sub test_video_retry test_frame_crop test_ota_reply test_calib_spin test_battery_soc test_battery
