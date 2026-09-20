/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Stage 0. The Z80 is real; everything it can see is this firmware.
 *
 * The split across the two cores is not about throughput. Core 0 owns the
 * video, which means it owns a DMA interrupt that fires every 31.7 us and
 * runs for a couple of them. Core 1 generates the Z80's clock in a
 * software loop, and an interrupt landing in the middle of that stretches
 * a clock phase — harmless for a fully static CMOS part, which is the
 * whole reason this design works, but it makes the measured clock jitter
 * by a factor of two and there is no reason to accept that. So: video on
 * core 0, the CPU on core 1, and the two never share an interrupt.
 */

#include <stdio.h>
#include <string.h>

#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <hardware/structs/qmi.h>
#include <hardware/vreg.h>
#include <pico/multicore.h>
#include <pico/stdlib.h>

#include "dino.h"
#include "screen.h"
#include "selftest.h"
#include "vga.h"
#include "z80_bus.h"

// ============================================================================
// The heartbeat program
// ============================================================================
//
//   0000  0E 00      LD C,00h
//   0002  79         LD A,C      ; loop
//   0003  D3 00      OUT (00h),A
//   0005  0C         INC C
//   0006  C3 02 00   JP loop
//
// What it is for: after the tests have finished there is nothing on the
// screen that changes, and a frozen report and a frozen machine look the
// same in a photograph. This puts a number on the screen that only
// advances if the CPU is still fetching, still executing and still
// driving an I/O cycle the engine still recognises.
static const uint8_t prog_heartbeat[] = {
    0x0E, 0x00,
    0x79,
    0xD3, 0x00,
    0x0C,
    0xC3, 0x02, 0x00,
};

/*
 * Core 1's stack, in main RAM rather than in SCRATCH_X.
 *
 * The SDK gives core 1 PICO_CORE1_STACK_SIZE bytes, which defaults to
 * 2 KB, carved out of the 4 KB SCRATCH_X region — and SCRATCH_X is a
 * fixed hardware block, so it cannot simply be made bigger.
 *
 * The self-test does not fit in that. It nests a test's own buffers
 * inside scr_fail()'s formatting buffers, and because every test is a
 * static function called exactly once, the compiler inlines them all into
 * selftest_run_all() and merges their locals into one frame that is live
 * for the whole run. Adding an 80-byte buffer to the *last* test was
 * enough to overflow it, and what faulted was the *first* one — core 1
 * died in the wiring test, several hundred lines away from the edit that
 * caused it, which is a memorably unhelpful way to spend an afternoon.
 *
 * 16 KB out of 512 KB, and the fault mode stops being a puzzle.
 */
static uint32_t core1_stack[4096] __attribute__((aligned(8)));

static volatile uint8_t  heartbeat_value;
static volatile uint32_t heartbeat_count;

static void heartbeat_out(const uint16_t port, const uint8_t value) {
    if ((port & 0xFF) != 0x00) return;
    heartbeat_value = value;
    heartbeat_count++;
}

// ============================================================================
// The header
// ============================================================================
static void draw_header(void) {
    scr_clear(ATTR(C_GREY, C_BLACK));
    scr_box(0, 0, VGA_COLS, VGA_ROWS, NULL, ATTR(C_BLUE, C_BLACK));

    scr_printf(2, 1, ATTR(C_YELLOW, C_BLACK), "FRANK DINO  %s", FIRMWARE_VERSION);
    scr_put(VGA_COLS - 28, 1, "dino_z80  Z84C00 + RP2350B", ATTR(C_DGREY, C_BLACK));

    scr_printf(2, 2, ATTR(C_GREY, C_BLACK),
               "RP2350B at %u MHz   video 640x480@60 over HDMI   console 115200 on J1",
               (unsigned)(clock_get_hz(clk_sys) / MHZ));

    scr_put(2, 3, "Stage 0: no emulation, no ROM - just the CPU and the bus.",
            ATTR(C_DGREY, C_BLACK));

    for (int x = 1; x < VGA_COLS - 1; x++)
        scr_put(x, 4, "\xC4", ATTR(C_BLUE, C_BLACK));
}

// ============================================================================
// Core 1: the CPU
// ============================================================================
static void core1_main(void) {
    z80_bus_init();

    const selftest_result_t r = selftest_run_all();

    scr_blank();
    if (r.failed == 0 && r.passed > 1) {
        scr_log(ATTR(C_LGREEN, C_BLACK), "%lu/%lu passed. The Z80 works and the bus is sound.",
                (unsigned long)r.passed, (unsigned long)r.run);
    } else {
        scr_log(ATTR(C_LRED, C_BLACK), "%lu of %lu tests failed.",
                (unsigned long)r.failed, (unsigned long)r.run);
        selftest_dump_trace(64);
    }

    printf("[dino] selftest: %lu run, %lu passed, %lu failed, best %lu Hz at pace %lu\n",
           (unsigned long)r.run, (unsigned long)r.passed, (unsigned long)r.failed,
           (unsigned long)r.z80_clock_hz, (unsigned long)r.best_pace);

    if (r.failed) {
        // Nothing to run. Leaving the CPU in reset is the honest state,
        // and it puts D1 back on so the board says so without a screen.
        while (true) tight_loop_contents();
    }

    // --- the heartbeat ---
    z80_io_out = heartbeat_out;
    memset(z80_mem, 0xFF, GUEST_MEM_SIZE);
    memcpy(z80_mem, prog_heartbeat, sizeof prog_heartbeat);
    z80_reset();

    const int row = VGA_ROWS - 2;
    absolute_time_t next = get_absolute_time();

    while (true) {
        // A slice, then a look at the clock. 20000 T-states is a few
        // milliseconds of Z80 at any pace this board reaches, so the
        // display is responsive without checking the time every cycle.
        z80_run(20000);

        if (absolute_time_diff_us(get_absolute_time(), next) > 0) continue;
        next = make_timeout_time_ms(200);

        scr_printf(2, row, ATTR(C_LCYAN, C_BLACK),
                   "Z80 running: OUT(00) = %02X   %lu I/O writes   %llu T-states",
                   heartbeat_value, (unsigned long)heartbeat_count,
                   (unsigned long long)z80_stats.t_states);
    }
}

// ============================================================================
// Core 0
// ============================================================================
int main(void) {
    /*
     * GP46 is the input of an audio amplifier and a reset leaves it
     * floating, which is a pin-shaped aerial in front of a speaker. This
     * board has nothing to say through it in stage 0, so it is driven low
     * and left there — two instructions against a second of hiss at every
     * boot.
     */
    gpio_init(SPEAKER_PIN);
    gpio_set_dir(SPEAKER_PIN, GPIO_OUT);
    gpio_put(SPEAKER_PIN, 0);

    vreg_disable_voltage_limit();
    vreg_set_voltage(CPU_SPEED_MHZ > 400 ? VREG_VOLTAGE_1_60 : VREG_VOLTAGE_1_30);
    busy_wait_at_least_cycles((SYS_CLK_VREG_VOLTAGE_AUTO_ADJUST_DELAY_US * (uint64_t)XOSC_HZ) / 1000000);

    qmi_hw->m[0].timing = 0x60007305;   // 5x flash divisor, for the faster clock
    set_sys_clock_hz(PICO_CLOCK_SPEED, true);

    stdio_init_all();
    printf("\n[dino] %s %s  clk %u MHz\n",
           FIRMWARE_NAME, FIRMWARE_VERSION, (unsigned)(clock_get_hz(clk_sys) / MHZ));

    // Video before anything else that might fail, so a failure has
    // somewhere to be seen from the far end of an HDMI cable. The DMA
    // interrupt it installs belongs to this core and stays here.
    vga_init();
    scr_init();
    draw_header();

    multicore_launch_core1_with_stack(core1_main, core1_stack, sizeof core1_stack);

    /*
     * Core 0's remaining job is to be the only core that talks to stdio.
     *
     * It owns the video interrupt and, with USB enabled, TinyUSB's timer
     * task — and TinyUSB is not multicore safe. Core 1 queues its log
     * lines and this drains them, so a printf can never block the loop
     * that is generating the Z80's clock, and can never race tud_task().
     */
    while (true) {
        scr_pump();
        sleep_ms(2);
    }
}
