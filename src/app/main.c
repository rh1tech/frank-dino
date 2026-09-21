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
#include "audio.h"
#if USB_KEYBOARD
#include "usbkbd.h"
#endif
#include "menu.h"
#include "storage.h"
#include "selftest.h"
#include "ay.h"
#include "launch.h"
#include "psram.h"
#include "settings.h"
#include "spectrum.h"
#include "vga.h"
#include "z80_bus.h"

/*
 * Core 1's stack, in main RAM rather than in SCRATCH_X.
 *
 * The SDK gives core 1 PICO_CORE1_STACK_SIZE bytes, which defaults to
 * 2 KB, carved out of the 4 KB SCRATCH_X region — and SCRATCH_X is a
 * fixed hardware block that cannot be made bigger. The self-test does not
 * fit in that: every test is a static function called once, so the
 * compiler inlines them all into selftest_run_all() and merges their
 * buffers into one frame that is live for the whole run.
 *
 * 16 KB out of 512 KB.
 */
static uint32_t core1_stack[4096] __attribute__((aligned(8)));

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

/* Set once the machine is up, so core 0 knows where console keys go. */
volatile bool spectrum_running = false;

/*
 * Every byte the console has delivered, and the last one.
 *
 * Bring-up only, and readable over SWD. "The keys do nothing" has three
 * causes that look identical from the far end of a cable — the wire, the
 * host, or this firmware — and a counter that does not move separates
 * the third from the other two without printing anything into a console
 * that is itself the thing under suspicion.
 */
volatile uint32_t console_chars;
volatile uint8_t  console_last;

static void core1_main(void) {
    z80_bus_init();

#if SELFTEST
    /*
     * The whole stage-0 suite, and then stop.
     *
     * A build that runs the tests does not go on to be a Spectrum: the
     * tests reset the CPU thirty times, fill all 64 KB with patterns and
     * leave the pace wherever the speed ramp ended. Handing that to a ROM
     * would be starting a machine out of a state nothing chose.
     */
    const selftest_result_t r = selftest_run_all();
    scr_blank();
    if (r.failed == 0)
        scr_log(ATTR(C_LGREEN, C_BLACK), "%lu/%lu passed. The Z80 works and the bus is sound.",
                (unsigned long)r.passed, (unsigned long)r.run);
    else {
        scr_log(ATTR(C_LRED, C_BLACK), "%lu of %lu tests failed.",
                (unsigned long)r.failed, (unsigned long)r.run);
        selftest_dump_trace(64);
    }
    while (true) tight_loop_contents();
#else
    /*
     * Enough of stage 0 to know the CPU is there, and no more.
     *
     * A dead or missing Z80 gives a black screen and nothing else, and
     * that is indistinguishable from a dozen other faults. Two tests cost
     * a few milliseconds and turn it into a sentence. The rest of the
     * suite is a build option, because it is destructive.
     */
    scr_log(ATTR(C_WHITE, C_BLACK), "ZX Spectrum 128K - a real Z84C00 on the bus");
    scr_blank();

    if (!selftest_quick_check()) {
        scr_blank();
        scr_log(ATTR(C_YELLOW, C_BLACK), "Build with SELFTEST=ON for the full stage-0 report.");
        while (true) tight_loop_contents();
    }

    // Calibrate against plain NOPs rather than the ROM: this runs the CPU
    // for tens of thousands of T-states at a dozen different speeds, and
    // it should not be doing that inside a machine that is booting.
    z80_map_flat();
    memset(z80_mem, 0x00, GUEST_MEM_SIZE);
    z80_reset();

    const uint32_t hz = z80_calibrate(SPEC_CPU_HZ);
    scr_log(ATTR(C_GREY, C_BLACK), "Z80 clock: %lu.%03lu MHz (asked for %u.%03u)",
            (unsigned long)(hz / 1000000), (unsigned long)((hz / 1000) % 1000),
            SPEC_CPU_HZ / 1000000, (SPEC_CPU_HZ / 1000) % 1000);
#if USB_KEYBOARD
    scr_log(ATTR(C_GREY, C_BLACK), "128K ROM, 8 x 16K RAM, USB keyboard on the USB-A ports.");
#else
    scr_log(ATTR(C_GREY, C_BLACK), "128K ROM, 8 x 16K RAM, keys over the console.");
#endif
    if (psram_size())
        scr_pass("PSRAM (U7, GP47)", "%lu MB mapped at %08X",
                 (unsigned long)(psram_size() >> 20), PSRAM_BASE);
    else
        scr_log(ATTR(C_YELLOW, C_BLACK), "PSRAM (U7, GP47)             none found");

    scr_log(ATTR(C_GREY, C_BLACK), "Sound: beeper + %s on GP46, %u Hz PWM DAC.",
            settings.turbosound ? "TurboSound (2x AY-3-8912)" : "AY-3-8912",
            (unsigned)AUDIO_RATE_HZ);
    scr_blank();
    scr_log(ATTR(C_LCYAN, C_BLACK), "Starting...");

    // Long enough to read off a capture stick, which is the only way
    // anyone sees this screen.
    sleep_ms(1500);

    spec_set_machine((machine_t)settings.machine);

    /*
     * Prove the snapshot machinery before anything depends on it, then
     * put the machine back.
     *
     * The self-test leaves the CPU running its own probe program with
     * RAM full of test values -- which is what it is for, and which is
     * emphatically not a Spectrum. Without the second spec_init() the
     * machine spins in the probe for ever and the screen stays black,
     * which it duly did.
     */
    char detail[80];
    if (ay_selftest(detail, sizeof detail)) scr_pass("AY-3-8912", "%s", detail);
    else                                    scr_fail("AY-3-8912", "%s", detail);

    const bool tramp_ok = spec_launch_selftest(detail, sizeof detail);
    spec_init();

    if (tramp_ok) scr_pass("Snapshot trampoline", "%s", detail);
    else          scr_fail("Snapshot trampoline", "%s", detail);

    // The real loop, measured while running it. See spec_tune_clock().
    // scr_log, not printf: this is core 1, and core 1 does not talk to
    // stdio -- it queues, and core 0 prints. See src/ui/screen.h.
    const uint32_t fps = spec_tune_clock();
    scr_log(ATTR(C_GREY, C_BLACK), "Frame rate %lu.%02lu Hz at pace %lu (%lu.%03lu MHz)",
            (unsigned long)(fps / 1000), (unsigned long)((fps % 1000) / 10),
            (unsigned long)z80_get_pace(),
            (unsigned long)((uint64_t)fps * SPEC_TSTATES_PER_FRAME / 1000000000ull),
            (unsigned long)(((uint64_t)fps * SPEC_TSTATES_PER_FRAME / 1000000ull) % 1000));

    /*
     * Start the machine the user actually sees, after the tuning and not
     * before it.
     *
     * The tuner runs the Spectrum at deliberately wrong speeds for
     * eighty-odd frames while it converges -- which is exactly the first
     * second and a half of the 128K ROM's own start-up, RAM test and
     * all. Living through that left the machine with a black screen and
     * a ROM that had given up, while every path that cold-started it
     * afterwards (the browser's machine key, loading a snapshot) worked
     * perfectly. So: tune on a machine that is about to be thrown away,
     * then start a fresh one.
     */
    spec_set_machine((machine_t)settings.machine);

    vga_set_mode(VGA_MODE_SPECTRUM);
    spectrum_running = true;

    while (true) spec_run_frame();
#endif
}

// ============================================================================
// Core 0
// ============================================================================
int main(void) {
    /*
     * GP46 is the input of an audio amplifier and a reset leaves it
     * floating, which is a pin-shaped aerial in front of a speaker. This
     * board's beeper is the Spectrum's, and until the ULA model owns it
     * the pin is driven low — two instructions against a second of hiss
     * at every boot.
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

    /*
     * The PSRAM, before anything that might live in it.
     *
     * Configuring it is a window in which the flash is unreachable, so
     * it goes as early as possible and before any interrupt source is
     * running. Nothing depends on it being present -- a board assembled
     * without the chip reports zero and carries on -- but what is put
     * there has to know, which is why the size is kept.
     */
    const uint32_t psram = psram_init();

    // Video before anything else that might fail, so a failure has
    // somewhere to be seen from the far end of an HDMI cable. The DMA
    // interrupt it installs belongs to this core and stays here.
    vga_init();
    scr_init();
    draw_header();

    // Before core 1 exists: it claims DMA channels and a pacing timer,
    // and core 1 starts pushing samples the moment the Spectrum starts.
    audio_init();

    /*
     * The card, mounted here and nowhere else.
     *
     * FatFs is not re-entrant across cores and the Z80 does not stop for
     * it, so everything that touches the filesystem happens on core 0,
     * and only while the machine is paused -- which is also when a file
     * browser is on the screen.
     */
    if (storage_mount()) printf("[sd] card mounted\n");
    else                 printf("[sd] no card\n");

    /*
     * Settings before core 1, because core 1 starts a machine with them.
     *
     * settings_load() falls back to the defaults on a card that has no
     * config -- or on no card at all -- so there is nothing to handle
     * here: after this call the settings are always valid, and the
     * return value only says whether a file was found.
     */
    if (settings_load()) printf("[cfg] settings loaded\n");
    else                 printf("[cfg] defaults\n");

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
        audio_task();
#if USB_KEYBOARD
        usbkbd_task();
#endif
        scr_pump();

        /*
         * The console is the keyboard.
         *
         * There is no keyboard socket on this board yet. Before the
         * machine starts, a keypress replays the boot report; afterwards
         * it is a Spectrum key. Which means the whole machine can be
         * driven from a script over the same cable that flashes it —
         * type "1" at the 128K menu, screenshot the result, and that is
         * a regression test.
         */
        const int c = getchar_timeout_us(0);
        if (c != PICO_ERROR_TIMEOUT) {
            console_chars++;
            console_last = (uint8_t)c;

            if (menu_is_open())          menu_key((char)c);
            else if (c == '\t')          menu_run();     // modal; returns when closed
            else if (spectrum_running)   spec_key_ascii((char)c);
            else                         scr_replay();
        }

        sleep_ms(2);
    }
}
