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
#include "ui_report.h"
#include "usbdisk.h"
#include "audio.h"
#include "sound.h"
#include "tapeplay.h"
#include "rtc.h"
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
#include "tapein.h"
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
        scr_log(ATTR(C_ACCENT, C_PANEL), "%lu/%lu passed. The Z80 works and the bus is sound.",
                (unsigned long)r.passed, (unsigned long)r.run);
    else {
        scr_log(ATTR(C_ICON_R, C_PANEL), "%lu of %lu tests failed.",
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
    scr_log(ATTR(C_WHITE, C_PANEL), "ZX Spectrum 128K - a real Z84C00 on the bus");
    scr_blank();

    if (!selftest_quick_check()) {
        scr_blank();
        scr_log(ATTR(C_ICON_Y, C_PANEL), "Build with SELFTEST=ON for the full stage-0 report.");
        while (true) tight_loop_contents();
    }

    // Calibrate against plain NOPs rather than the ROM: this runs the CPU
    // for tens of thousands of T-states at a dozen different speeds, and
    // it should not be doing that inside a machine that is booting.
    z80_map_flat();
    memset(z80_mem, 0x00, GUEST_MEM_SIZE);
    z80_reset();

    const uint32_t hz = z80_calibrate(SPEC_CPU_HZ);
    scr_log(ATTR(C_TEXT_DIM, C_PANEL), "Z80 clock: %lu.%03lu MHz (asked for %u.%03u)",
            (unsigned long)(hz / 1000000), (unsigned long)((hz / 1000) % 1000),
            SPEC_CPU_HZ / 1000000, (SPEC_CPU_HZ / 1000) % 1000);

    /*
     * And how hard it may be driven, measured rather than assumed.
     *
     * Everything that tunes for a frame rate is clamped to this. Without
     * it the tuner will happily buy 50 Hz by sampling the CPU's pins
     * before they are valid, and a Spectrum that keeps perfect time and
     * computes nonsense is the most confusing failure this firmware has.
     */
    const uint32_t floor = selftest_probe_pace_floor();
    z80_set_pace_floor(floor);
    scr_log(ATTR(C_TEXT_DIM, C_PANEL), "Bus engine: correct down to a pace of %lu; floor set there.",
            (unsigned long)floor);
#if USB_KEYBOARD
    scr_log(ATTR(C_TEXT_DIM, C_PANEL), "128K ROM, 8 x 16K RAM, USB keyboard on the USB-A ports.");
#else
    scr_log(ATTR(C_TEXT_DIM, C_PANEL), "128K ROM, 8 x 16K RAM, keys over the console.");
#endif
    if (psram_size())
        scr_pass("PSRAM (U9, GP47)", "%lu MB mapped at %08X",
                 (unsigned long)(psram_size() >> 20), PSRAM_BASE);
    else
        scr_log(ATTR(C_ICON_Y, C_PANEL), "PSRAM (U9, GP47)             none found");

    scr_log(ATTR(C_TEXT_DIM, C_PANEL), "Sound: beeper + %s, stereo on GP46/GP44, %u Hz PWM DAC.",
            settings.turbosound ? "TurboSound (2x AY-3-8912)" : "AY-3-8912",
            (unsigned)AUDIO_RATE_HZ);
    scr_blank();
    scr_log(ATTR(C_ICON_C, C_PANEL), "Starting...");

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
    const uint32_t cap = spec_timing.capacity_mfps;
    scr_log(ATTR(C_TEXT_DIM, C_PANEL), "Frame rate %lu.%02lu Hz at pace %lu; could do %lu.%02lu (%lu%% spare)",
            (unsigned long)(fps / 1000), (unsigned long)((fps % 1000) / 10),
            (unsigned long)z80_get_pace(),
            (unsigned long)(cap / 1000), (unsigned long)((cap % 1000) / 10),
            (unsigned long)(cap > fps ? (cap - fps) * 100u / cap : 0u));

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
     * GP46 and GP44 are the inputs of an audio amplifier and a reset
     * leaves them floating, which is a pair of pin-shaped aerials in
     * front of a speaker. Until the DAC owns them they are driven low —
     * a few instructions against a second of hiss at every boot.
     */
    const uint audio_pins[] = { AUDIO_L_PIN, AUDIO_R_PIN };
    for (unsigned i = 0; i < 2; i++) {
        gpio_init(audio_pins[i]);
        gpio_set_dir(audio_pins[i], GPIO_OUT);
        gpio_put(audio_pins[i], 0);
    }

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
    /*
     * The PSRAM, and the board does not run without it.
     *
     * U9 is soldered down, not an option: the guest's disk and tape
     * images, the menus' framebuffer, the machines with more than
     * 128 KB and the inflate window all live over there, and a board
     * that comes up without it is a board with a fault. So this is a
     * stop, not a degradation -- there is no useful subset to fall back
     * to and pretending otherwise only hides the fault.
     *
     * The USB console is the one diagnostic channel that does not
     * itself need the PSRAM: the menus draw into it, so a board without
     * it cannot put a message on the screen. A USB_KEYBOARD build has no
     * console at all, and there the stop shows as a board that never
     * gets past a black screen.
     */
    if (!psram_init()) {
        for (;;) {
            printf("[dino] FATAL: no PSRAM answered on XIP CS1 (U9, GP47).\n"
                   "       This board cannot run without it. Check U9 and R39 on GP47.\n");
            sleep_ms(2000);
        }
    }

    // Video before anything else that might fail, so a failure has
    // somewhere to be seen from the far end of an HDMI cable. The DMA
    // interrupt it installs belongs to this core and stays here.
    vga_init();
    scr_init();

    /*
     * The report takes the screen here, in the menus' own window.
     *
     * After vga_init() because it sets the video mode, and after
     * psram_init() because the surface it draws on is over there. With
     * no PSRAM fitted it declines and the eighty-column text mode stays
     * up, which is the only configuration that still sees the old
     * report.
     */
    ui_report_begin();

    // Before core 1 exists: it claims DMA channels and a pacing timer,
    // and core 1 starts pushing samples the moment the Spectrum starts.
    audio_init();
    sound_init();

#if USB_KEYBOARD
    /*
     * The host stack, which nothing else will start.
     *
     * After psram_init(), because that runs with interrupts off and with
     * the flash unreachable, and TinyUSB installs an interrupt handler.
     * Before core 1, because everything USB happens on this core --
     * tuh_task() is called from the loop at the bottom of this file and
     * enumeration finishes while core 1 is still printing its report.
     */
    usbkbd_init();

    /*
     * And then wait a moment for something to turn up.
     *
     * Not politeness: in a USB_KEYBOARD build the keyboard is the only
     * input there is -- S1 has given the console's pins to the host
     * controller -- so "did it enumerate" is a question with no other
     * way to ask it. A hub and a keyboard behind it take a few hundred
     * milliseconds; a second and a half is generous and only costs the
     * boot screen, which is on display for longer than that anyway.
     */
    const absolute_time_t usb_deadline = make_timeout_time_ms(1500);
    while (!usbkbd_present() && !usbkbd_pad_present()
           && absolute_time_diff_us(get_absolute_time(), usb_deadline) > 0)
        usbkbd_task();

    if (usbkbd_present() && usbkbd_pad_present())
        scr_pass("USB keyboard", "with a gamepad, on the USB-A port");
    else if (usbkbd_pad_present())
        scr_pass("USB gamepad", "enumerated on the USB-A port");
    else if (usbkbd_present())
        scr_pass("USB keyboard", "enumerated on the USB-A port");
    else
        scr_log(ATTR(C_ICON_Y, C_PANEL),
                "USB keyboard                 none yet - check S1 is in the USB-A position");
#endif

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
    rtc_init();                         // its NVRAM lives on the card

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

    /*
     * The flat memory stage 0 tests the bus with is RAM bank 0.
     *
     * Safe because the order is: core 1 runs the self-test, and only
     * afterwards does spec_init() lay a machine over the banks. The test
     * fills it with patterns and the machine overwrites them, which is
     * exactly the right relationship -- and it is sixteen kilobytes of
     * .bss that used to exist for the first second after reset and then
     * never again. See z80_bus.h.
     */
    z80_mem = spec_ram[0];

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
        sound_task();
        tapeplay_task();
        rtc_task();
        audio_task();
        /*
         * The tape input follows its setting, wherever the setting was
         * changed -- the menu, the card at start-up, or a debugger.
         * One place that acts on it, so it cannot be set and not
         * applied.
         */
        if (settings.real_player != tapein_running())
            settings.real_player ? tapein_start() : tapein_stop();
#if USB_KEYBOARD
        usbkbd_task();

        /*
         * Mount or unmount a USB stick as it comes and goes.
         *
         * In the loop, because a stick may be plugged in at any moment.
         * Here and not in the TinyUSB callback because f_mount() reads
         * the medium, and reading the medium needs tuh_task() to run --
         * so mounting from inside tuh_task() would wait for itself.
         */
        usbdisk_task();


        /*
         * F1, asked for from inside a TinyUSB callback and acted on here.
         *
         * In this build the keyboard is the only input there is, so this
         * is the only way into the browser -- there is no console to
         * press Tab at.
         */
        /*
         * Pause stops the clock. The Z80 is static, so a paused machine is
         * exactly where it was -- registers, refresh, all of it -- and
         * Pause again carries on as if nothing happened.
         */
        if (usbkbd_take_pause_request() && spectrum_running) {
            if (spec_is_paused()) spec_resume();
            else                  spec_pause();
        }

        if (usbkbd_take_menu_request() && spectrum_running) {
            // F12 goes straight to the settings; F11 opens the browser,
            // which is where the settings live anyway.
            menu_open_settings(usbkbd_menu_request_was_settings());
            usbkbd_release_all();       // nothing left held on the way in
            menu_run();
            usbkbd_release_all();       // ...or on the way out
        }
#endif
        scr_pump();
        if (spectrum_running) harness_poll();

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
        const int c = console_getchar();
        if (c != PICO_ERROR_TIMEOUT) {
            console_chars++;
            console_last = (uint8_t)c;

            /*
             * The menu only exists once the machine does.
             *
             * Before that, core 1 is calibrating and self-testing: it is
             * driving the Z80 but it is not in the frame loop, so it
             * never reaches a pause point and spec_pause() times out
             * after 200 ms and returns anyway. Both cores then clock the
             * same CPU, which leaves the machine in a state nothing
             * resets it out of -- a Tab pressed a second after flashing
             * was enough, and the wreckage survived every reset the menu
             * could ask for afterwards.
             */
            if (menu_is_open())                          menu_key((char)c);
            else if (c == '\t' && spectrum_running)      menu_run();
            else if (spectrum_running)                   spec_key_ascii((char)c);
            else                                         scr_replay();
        }

        sleep_ms(2);
    }
}
