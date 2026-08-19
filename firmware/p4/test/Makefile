CC = cc
CFLAGS = -I../main -Wall -Wextra -Werror -std=c11
LDLIBS = -lm

all: test_mixer test_motors test_control_proto test_watchdog test_calibration test_ramp test_trim test_telemetry test_recovery test_wheel

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

run: all
	./test_mixer && ./test_motors && ./test_control_proto && ./test_watchdog && ./test_calibration && ./test_ramp && ./test_trim && ./test_telemetry && ./test_recovery && ./test_wheel

clean:
	rm -f test_mixer test_motors test_control_proto test_watchdog test_calibration test_ramp test_trim test_telemetry test_recovery test_wheel
