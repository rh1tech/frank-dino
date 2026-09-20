// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------
//
// dino_z80: an RP2350B acting as the whole machine around a real Z80.
//
// It gets a header of its own rather than sharing frank_xt8086.h, because
// the two boards disagree on the one thing a board header decides. On
// xt8086 the microSD is on GP40-43 and the console on GP44/45 — the same
// here — but GP38/GP39 are the DS3231's I2C bus there and the Z80's ~INT
// and ~NMI here, and a header that declared I2C on them would invite an
// SDK helper to drive two of the CPU's inputs.
//
// GP0-GP29 belong to the Z80 (U3) and the pins it needs. Everything below
// GP30 is the CPU's; see src/core/dino.h and docs/pinout.md.
//
// Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
// https://github.com/rh1tech/frank-dino
// SPDX-License-Identifier: GPL-3.0-or-later
//
#ifndef _BOARDS_FRANK_DINO_H
#define _BOARDS_FRANK_DINO_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

#define FRANK_DINO

// --- RP2350 VARIANT ---
// RP2350B: 48 GPIOs, so PIO needs the movable 32-pin window — the video
// ladder sits on GP30-37 and a state machine left in the default window
// cannot see past GP31.
#define PICO_RP2350A 0

pico_board_cmake_set_default(PICO_PIO_USE_GPIO_BASE, 1)
#ifndef PICO_PIO_USE_GPIO_BASE
#define PICO_PIO_USE_GPIO_BASE 1
#endif

// --- UART (J1: pin 1 = TX, pin 2 = GND, pin 3 = RX) ---
// GP44/GP45, which is still UART0: the peripheral's pin map repeats every
// sixteen GPIOs in blocks of four, and GP44-47 lands back on UART0 just as
// GP0-3 does.
//
// GP45 is shared with the tape input (Q5's collector, R47 pull-up). A tape
// lead and a console cable cannot both be attached; the console wins,
// because without it there is no way to see what the firmware thinks.
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 44
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 45
#endif

// --- LED ---
// There is no firmware-controlled LED on this board. D1 is driven from
// the Z80's ~RESET line through R1, so it reports whether the CPU is held
// in reset — which is the CPU's business, not an SDK helper's.

// --- SPI (microSD, J3) ---
// SPI1, because GP40-43 have no SPI0 mapping at all.
#ifndef PICO_DEFAULT_SPI
#define PICO_DEFAULT_SPI 1
#endif
#ifndef PICO_DEFAULT_SPI_SCK_PIN
#define PICO_DEFAULT_SPI_SCK_PIN 42
#endif
#ifndef PICO_DEFAULT_SPI_TX_PIN
#define PICO_DEFAULT_SPI_TX_PIN 43
#endif
#ifndef PICO_DEFAULT_SPI_RX_PIN
#define PICO_DEFAULT_SPI_RX_PIN 40
#endif
#ifndef PICO_DEFAULT_SPI_CSN_PIN
#define PICO_DEFAULT_SPI_CSN_PIN 41
#endif

// --- I2C ---
// Deliberately not declared. This board has no DS3231 and no I2C bus of
// its own; GP38/GP39, which carried it on xt8086, are the Z80's ~INT and
// ~NMI here.

// --- FLASH (U6, W25Q128JVPIQ, 16 MB) ---
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif
