#ifndef BOARD_H
#define BOARD_H

// Everything this firmware assumes about the physical board lives here, and only here.
// The twin of firmware/p4/main/board.h, same role: "every assumption about the physical
// board... Bring-up edits this file and nothing else." (CLAUDE.md)
//
// Board: a third-party carrier silkscreened `ESP32-23 2022-V1.3`, ESP32-S3 module, native
// USB. Not Espressif's own hardware, so there is no vendor pinout doc to defer to here.
//
// Wired and confirmed on the bench 2026-09-06 against a GM009605 v4.3 panel. The first guess
// here had these two the other way round -- SDA on 8, SCL on 9 -- and the panel answered
// nothing: `display_hal: panel does not answer at 0x3c on SDA 8 / SCL 9`. The pins themselves
// were never the problem, only which was which, and swapping them is why this file exists.
//
// They stay clear of what this board already commits elsewhere -- GPIO19/20 are the native USB
// D-/D+, GPIO33-37 the octal PSRAM bus -- and I2C runs on no other bus in this project.
#define BOARD_I2C_SDA    9
#define BOARD_I2C_SCL    8

#define BOARD_I2C_HZ     400000

// SSD1306/SH1106-class modules ship strapped to one of two 7-bit addresses depending on the
// board's own address pad -- check the panel before trusting this value.
#define BOARD_OLED_ADDR  0x3C   /* modules ship as 0x3C or 0x3D; check the board */

// The strap this board's BOOT button drives -- GPIO0, the ESP32-S3's standard boot-select pin,
// not a pin this project chose. Recorded here (rather than left implicit) because it is a
// physical-board fact like the rest of this file, and a future screen or input feature that
// wants a button has to share it with the strap.
#define BOARD_BOOT_GPIO  0

#endif // BOARD_H
