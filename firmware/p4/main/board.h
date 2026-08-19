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
// UNVERIFIED until bring-up — see docs/bringup.md. These two compile and do not collide with
// the radio, but they are not claimed to match the board's silkscreen. Check the pinout before
// wiring the PCA9685.
#define BOARD_I2C_SDA        20
#define BOARD_I2C_SCL        21

#define BOARD_I2C_HZ         400000
#define BOARD_PWM_HZ         1000

// Two PCA9685 boards on the same bus, one per axle: the front axle's two motors on 0x40, the
// rear's on 0x41 (its A0 jumper set). Logical channels 0..7 split four per board — 0..3 front,
// 4..7 rear — which is the only place that split is written down.
//
// Note this does not fix which corner is which: the calibration wizard discovers that by
// spinning each pair and asking which wheel turned. The split only has to match how the motors
// are physically cabled to the boards, not any particular corner assignment.
#define BOARD_PCA_ADDR_FRONT  0x40
#define BOARD_PCA_ADDR_REAR   0x41
#define BOARD_PCA_CH_PER_CHIP 4

// Expected esp_hosted slave version on the C6. The radio is wire-flashed once and pinned,
// so a mismatch is otherwise silent; status_api compares this against what the co-processor
// actually reports and flags it in /status.
#define BOARD_RADIO_SLAVE_FW "3.0.6"

#endif // BOARD_H
