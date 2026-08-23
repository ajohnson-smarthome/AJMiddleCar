#ifndef BOARD_H
#define BOARD_H

// Everything this firmware assumes about the physical board lives here, and only here.
//
// Waveshare ESP32-P4-Module-DEV-KIT: ESP32-P4NRW32 (dual-core RISC-V, 360 MHz) with an
// ESP32-C6 wired over SDIO as the WiFi/BT radio, 32 MB PSRAM, 16 MB flash, 28 programmable
// GPIOs on the 2x20 header.
//
// The radio's SDIO link reserves GPIO 14..19 (D0..D3, CLK, CMD) and GPIO 54 (co-processor
// reset) — pinned in sdkconfig.defaults. Anything assigned here must avoid those.
//
// These are the board's own default I2C pins, per Waveshare's documentation for the
// ESP32-P4-Module-DEV-KIT (the same pair its Qwiic connectors carry). They are clear of the
// radio's SDIO block, and unlike the numbers that stood here before — which were chosen only to
// compile — they are brought out on the 2x20 header as physical pins 3 and 5, silkscreened
// `SDA`/`SCL` rather than by GPIO number, in the Raspberry Pi positions.
//
// Confirmed on the bench 2026-08-21: both PCA9685 boards answer here and initialise. The
// on-board ES8311 audio codec shares this bus at 0x18, which makes a scan self-checking — if
// 0x18 answers and 0x40 does not, the bus is fine and the fault is in the PCA wiring.
#define BOARD_I2C_SDA        7
#define BOARD_I2C_SCL        8

#define BOARD_I2C_HZ         400000
#define BOARD_PWM_HZ         1000

// Two PCA9685 boards on the same bus, one per axle: the front axle's two motors on 0x40, the
// rear's on 0x60. The rear address is whatever its address pads happen to say — ours came back
// as 0x60 (A5 bridged) rather than the 0x41 an A0 bridge would give, and there is no reason to
// unsolder it: the address is data, and this is the file that holds it. Logical channels 0..7 split four per board — 0..3 front,
// 4..7 rear — which is the only place that split is written down.
//
// Note this does not fix which corner is which: the calibration wizard discovers that by
// spinning each pair and asking which wheel turned. The split only has to match how the motors
// are physically cabled to the boards, not any particular corner assignment.
#define BOARD_PCA_ADDR_FRONT  0x40
#define BOARD_PCA_ADDR_REAR   0x60
#define BOARD_PCA_CH_PER_CHIP 4

// Stiction crutch for the JGB37-520B on a BTS7960: a stopped motor under even minimal
// load ignores small duty — it hums and stays put, while the same duty keeps a spinning
// motor spinning. So commands above the deadzone land at a duty floor (motors.c), and a
// channel leaving standstill gets a short burst of start-assist duty past the ramp
// (link.h). These numbers are UN-MEASURED bench guesses (encoders not wired, 2026-08-24)
// and are THE tuning knobs — turn them here, not in the code that uses them. The real fix
// is a per-wheel start duty measured by the calibration wizard once encoders land.
#define BOARD_DUTY_FLOOR  1100   /* ~27%: smallest duty commanded above the deadzone */
#define BOARD_KICK_DUTY   2600   /* ~63%: start-assist duty for a channel leaving standstill */
#define BOARD_KICK_TICKS  3      /* x LINK_TICK_MS (20 ms) = 60 ms of kick */

// The C6 runs esp_hosted's slave image, delivered out of band (firmware/c6/README.md) —
// over SDIO from the host is the recorded route, the UART header the fallback. The
// EXPECTED slave version is no longer pinned here by hand: status_api derives it at
// compile time from the host component's own version macros (eh_common_fw_version.h),
// so the expectation cannot drift from firmware/p4/main/idf_component.yml.

#endif // BOARD_H
