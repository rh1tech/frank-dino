/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The settings tree, in pico-speccy's model (src/ui/UiTree.cpp).
 *
 * Theirs is 75 KB because it covers twenty machines, four disk
 * interfaces, six sound cards and a network stack. This is the same
 * shape over what this board has: six sub-menus off a root, every option
 * an explicit {label, value} pair, every conditional row a visible() or
 * an enabled() predicate rather than arithmetic on row numbers.
 *
 * The action and info functions came from the settings page this
 * replaces, unchanged apart from where they put their sentence: the
 * footer status line rather than a row of their own.
 */

#include "ui_model.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "beta.h"
#include "tapein.h"
#include "dck.h"
#include "divmmc.h"
#include "fdc.h"
#include "menu.h"
#include "psram.h"
#include "settings.h"
#include "spectrum.h"
#include "screen.h"
#include "storage.h"
#include "usbdisk.h"
#include "tape.h"
#include "ui_nav.h"
#include "ui_capture.h"
#include "ui_dialog.h"
#include "ui_render.h"
#include "vga.h"
#if USB_KEYBOARD
#include "usbkbd.h"
#endif
#include "z80_bus.h"

/* ── values ──────────────────────────────────────────────────────────── */

int32_t ui_node_value(const ui_node_t *n) {
    return n->slot ? (int32_t)*n->slot : 0;
}

void ui_node_set_value(const ui_node_t *n, const int32_t v) {
    if (!n->slot) return;
    if ((int32_t)*n->slot == v) return;
    *n->slot = (uint8_t)v;
    if (n->changed) n->changed();
}

/*
 * What the left column shows for a row.
 *
 * A radio shows its option's short label where it has one ("128K" rather
 * than "ZX Spectrum 128K"), because the left pane is 24 characters wide
 * and the long spelling is already in the right pane next to the ring. A
 * K_INT shows the number. Anything with a vlabel() shows what that says,
 * which is how an action row reports live state.
 */
const char *ui_node_value_label(const ui_node_t *n) {
    static char buf[UI_DYN_VALUE_LEN];

    if (n->vlabel) return n->vlabel();

    switch (n->kind) {
        case K_RADIO:
        case K_BOOL: {
            const int32_t v = ui_node_value(n);
            uint8_t cnt;
            const ui_option_t *o = ui_node_options(n, &cnt);
            for (uint8_t i = 0; i < cnt; i++)
                if (o[i].value == v) return o[i].slabel ? o[i].slabel : o[i].label;
            return "";
        }
        case K_INT:
            snprintf(buf, sizeof buf, "%ld", (long)ui_node_value(n));
            return buf;
        default:
            return "";
    }
}

/* ── option tables ───────────────────────────────────────────────────── */

static const ui_option_t opt_machine[] = {
    { "ZX Spectrum 48K",  MACHINE_48K,      "48K"      },
    { "ZX Spectrum 128K", MACHINE_128K,     "128K"     },
    { "ZX Spectrum +2",   MACHINE_PLUS2,    "+2"       },
    { "ZX Spectrum +3",   MACHINE_PLUS3,    "+3"       },
    { "Pentagon 128",     MACHINE_PENTAGON, "Pentagon" },
    { "Scorpion ZS-256",  MACHINE_SCORPION, "Scorpion" },
    { "Profi 1024",       MACHINE_PROFI,    "Profi"    },
    { "Timex TC2068",     MACHINE_TIMEX,    "Timex"    },
};

static const ui_option_t opt_turbo[] = {
    { "3.5 MHz (as Sinclair)", 0, "3.5 MHz" },
    { "5.3 MHz",               1, "5.3 MHz" },
    { "7 MHz",                 2, "7 MHz"   },
};

static const ui_option_t opt_output[] = {
    { "Jack / buzzer",     OUTPUT_JACK, "Jack" },
    { "HDMI (line level)", OUTPUT_HDMI, "HDMI" },
};

static const ui_option_t opt_joystick[] = {
    { "None",       JOY_NONE,       NULL      },
    { "Kempston",   JOY_KEMPSTON,   NULL      },
    { "Sinclair 1", JOY_SINCLAIR1,  "Sincl 1" },
    { "Sinclair 2", JOY_SINCLAIR2,  "Sincl 2" },
    { "Cursor",     JOY_CURSOR,     NULL      },
    { "Fuller",     JOY_FULLER,     NULL      },
};

static const ui_option_t opt_kport[] = {
    { "#1F", 0, NULL },
    { "#DF", 1, NULL },
};

static const ui_option_t opt_uitheme[] = {
    { "Black panels",     UI_THEME_BLACK, "Black" },
    { "Navy (pico-spec)", UI_THEME_NAVY,  "Navy"  },
    { "Slate, dithered",  UI_THEME_SLATE, "Slate" },
};

/* ── what the rows do ────────────────────────────────────────────────── */

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ui_status(buf);
}

static void machine_changed(void) {
    spec_set_machine((machine_t)settings.machine);
    say("%s started", spec_machine_name());
}

/*
 * A faster Z80 than the one Sinclair specified.
 *
 * The clock is ours, edge by edge, and the part in the socket is a
 * Z84C00-10 -- so this is not a trick, it is the CPU actually running
 * faster. Asking for 7 MHz does not make it so, though: past some speed
 * the RP2350 cannot generate every edge and answer every bus cycle, the
 * pace bottoms out and the machine runs slow rather than fast. That
 * failure is invisible from the inside, so the measured frame rate goes
 * on the status line and the user can see 41 Hz and pick a lower one.
 */
static void turbo_changed(void) {
    const uint32_t fps = spec_tune_clock();
    say("%lu.%02lu fps", (unsigned long)(fps / 1000), (unsigned long)((fps % 1000) / 10));
}

static void output_changed(void) {
    say(settings.output == OUTPUT_HDMI ? "A third of full scale" : "Full scale");
}

static void betadisk_changed(void) {
    if (settings.betadisk) settings.esxdos = 0;
    /* The ROM it brings in replaces the BASIC ROM, so the machine has to
     * be restarted around it -- and re-tuned, which is the part that is
     * easy to miss: the interface costs an inspection of every opcode
     * fetch, so turning it on makes the frame loop heavier. */
    spec_set_machine((machine_t)settings.machine);
    const uint32_t fps = spec_tune_clock();
    say("Beta Disk %s, %lu.%02lu fps", settings.betadisk ? "on" : "off",
        (unsigned long)(fps / 1000), (unsigned long)((fps % 1000) / 10));
}

static bool betadisk_on(void) { return settings.betadisk != 0; }
static bool esxdos_on(void)   { return settings.esxdos != 0; }

/*
 * The two interfaces cannot both be on.
 *
 * Both page themselves in on an opcode fetch at #3D00-#3DFF, so with
 * the pair enabled they take turns owning the bottom of the address
 * space and neither works. Turning one on turns the other off rather
 * than letting the user find that out from a black screen.
 */
static void esxdos_changed(void) {
    if (settings.esxdos) settings.betadisk = 0;
    spec_set_machine((machine_t)settings.machine);
    say(settings.esxdos ? "esxDOS on, Beta Disk off" : "esxDOS off");
}

static const char *mmc_info(void) {
    static char buf[UI_DYN_VALUE_LEN];
    if (!divmmc_mounted()) return "no image";
    divmmc_stats(buf, sizeof buf);
    return buf;
}

static void mmc_action(void) {
    if (!divmmc_mounted()) { say("No card image is in it."); return; }
    if (!ui_confirm("Take the card image out?", "DivMMC", false)) return;
    divmmc_eject();
    say("Card out.");
}

/* ── live state ──────────────────────────────────────────────────────── */

static const char *tape_info(void) {
    static char buf[UI_DYN_VALUE_LEN];
    if (!tape_mounted()) return "no tape";
    snprintf(buf, sizeof buf, "%lu/%lu",
             (unsigned long)tape_block_index() + 1, (unsigned long)tape_block_count());
    return buf;
}

static const char *disk_info(void) {
    static char buf[UI_DYN_VALUE_LEN];
    if (!beta_mounted()) return "no disk";
    snprintf(buf, sizeof buf, "%u tracks", beta_tracks());
    return buf;
}

static bool is_plus3(void) { return spec_machine == MACHINE_PLUS3; }
static bool is_timex(void) { return spec_machine == MACHINE_TIMEX; }

static const char *dck_info(void) {
    static char buf[UI_DYN_VALUE_LEN];
    if (!dck_mounted()) return "empty";
    unsigned kb = 0;
    for (unsigned i = 0; i < DCK_CHUNKS; i++) if (dck_present() & (1u << i)) kb += 8;
    snprintf(buf, sizeof buf, "%u KB", kb);
    return buf;
}

static void dck_action(void) {
    if (!dck_mounted()) { say("The dock is empty."); return; }
    if (!ui_confirm("Take the cartridge out?", "Timex dock", false)) return;
    dck_eject();
    say("Cartridge out.");
}

static const char *dsk_info(void) {
    static char buf[UI_DYN_VALUE_LEN];
    if (!fdc_mounted()) return "no disk";
    snprintf(buf, sizeof buf, "%u trk %u side", fdc_tracks(), fdc_sides());
    return buf;
}

static const char *upd_info(void) {
    static char buf[UI_DYN_VALUE_LEN];
    fdc_stats(buf, sizeof buf);
    return buf;
}

static void dsk_action(void) {
    if (!fdc_mounted()) { say("No disk is in the drive."); return; }

    if (fdc_dirty()) {
        const char *btns[3] = { "Save + eject", "Eject", "Cancel" };
        const int c = ui_choice("This disk has been written to.", btns, 3, 0, 2);
        if (c == 2) return;
        if (c == 0) {
            char d[80];
            if (!fdc_save(d, sizeof d)) { say("%s", d); return; }
        }
    } else if (!ui_confirm("Eject the disk?", "+3 drive", false)) {
        return;
    }

    fdc_eject();
    say("Disk ejected.");
}

static void dsk_save_action(void) {
    char d[80];
    fdc_save(d, sizeof d);
    say("%s", d);
}

static const char *dsk_state(void) {
    if (!fdc_mounted()) return "no disk";
    return fdc_dirty() ? "changed" : "saved";
}

static const char *fdc_info(void) {
    static char buf[UI_DYN_VALUE_LEN];
    beta_stats(buf, sizeof buf);
    return buf;
}

/*
 * What the board turned out to have, rather than what it was built with.
 *
 * The PSRAM is a fitted-or-not part and the probe runs too early for its
 * answer to reach a console that has not enumerated yet, so the one
 * place it is reliably readable is here.
 */
static const char *usb_info(void) {
    static char buf[UI_DYN_VALUE_LEN];
    if (!usbdisk_present()) return "none";
    const uint64_t n = usbdisk_size();
    if (n >= (1ull << 30)) snprintf(buf, sizeof buf, "%lu GB", (unsigned long)(n >> 30));
    else                   snprintf(buf, sizeof buf, "%lu MB", (unsigned long)(n >> 20));
    return buf;
}

static const char *psram_info(void) {
    static char buf[UI_DYN_VALUE_LEN];
    const uint32_t n = psram_size();
    if (!n) return "not fitted";
    snprintf(buf, sizeof buf, "%lu MB", (unsigned long)(n >> 20));
    return buf;
}

/*
 * Whether the DAC is being kept fed.
 *
 * An underrun is not silence -- audio_task() holds the last sample -- so
 * it is inaudible as a gap and very audible as a tone a per cent or two
 * flat with a roughness on it. The count is the only way to tell that
 * from a tune that is simply written that way.
 */
static const char *audio_info(void) {
    static char buf[UI_DYN_VALUE_LEN];
    snprintf(buf, sizeof buf, "%lu-%lu-%lu",
             (unsigned long)audio_underruns, (unsigned long)audio_overruns,
             (unsigned long)audio_clips);
    return buf;
}

/*
 * Where the CPU is, and whether it is anywhere.
 *
 * A machine that has crashed and a machine that is running look the same
 * from here -- both are a screen full of something. The address of the
 * last opcode fetch says which, and usually says where: a tight loop
 * repeats one address, a runaway walks, and a stopped CPU does not move
 * at all between two visits to this page.
 */
static const char *z80_info(void) {
    static char buf[UI_DYN_VALUE_LEN];
    snprintf(buf, sizeof buf, "PC~%04X p%lu", z80_stats.last_opfetch_addr,
             (unsigned long)z80_get_pace());
    return buf;
}

#if USB_KEYBOARD
static const char *kbd_info(void) {
    if (usbkbd_present() && usbkbd_pad_present()) return "kbd + pad";
    if (usbkbd_pad_present())                     return "gamepad";
    return usbkbd_present() ? "attached" : "nothing";
}
#endif

/* ── pages ───────────────────────────────────────────────────────────── */

/*
 * The page bodies are built over in the PSRAM.
 *
 * Six kilobytes of formatted text is six kilobytes the SRAM has not got,
 * and nothing reads it except the rasteriser, with the Z80 stopped.
 */
static char page_buf[6144] __attribute__((section(".psram")));

/*
 * Everything the firmware knows about the machine it is running, on one
 * scrollable page -- pico-speccy's Emulator info, over what this board
 * actually has. The markup codes are theirs: a 0x02 and a letter switch
 * ink for the rest of the line, so the whole page is one snprintf chain
 * rather than a list of coloured fragments.
 */
static void info_page(void) {
    unsigned pass = 0, fail = 0;
    scr_report_tally(&pass, &fail);

    const char *yn[2] = { UI_MK_DIM "Off", UI_MK_OK "On" };
    char clk[32];

    snprintf(page_buf, sizeof page_buf,
        UI_MK_HEAD "--- Machine ---\n"
        UI_MK_TEXT "Architecture   : " UI_MK_WHITE "%s\n"
        UI_MK_TEXT "Z80            : " UI_MK_WHITE "Z84C00, a real one\n"
        UI_MK_TEXT "Clock          : " UI_MK_WHITE "%s\n"
        UI_MK_TEXT "Bus pace       : " UI_MK_WHITE "%lu (floor %lu)\n"
        UI_MK_TEXT "Issue 2 keys   : %s\n"
        "\n"
        UI_MK_HEAD "--- Video ---\n"
        UI_MK_TEXT "Output         : " UI_MK_WHITE "640x480 60 Hz over HDMI\n"
        UI_MK_TEXT "Colour depth   : " UI_MK_WHITE "2 bits a channel, 64 colours\n"
        UI_MK_TEXT "Scanlines      : %s\n"
        UI_MK_TEXT "ULA+ ports     : %s\n"
        UI_MK_TEXT "ULA+ palette   : %s\n"
        UI_MK_TEXT "Gigascreen     : %s\n"
        UI_MK_TEXT "Timex video    : %s\n"
        UI_MK_TEXT "Menu colours   : " UI_MK_WHITE "%s\n"
        "\n"
        UI_MK_HEAD "--- Audio ---\n"
        UI_MK_TEXT "DAC            : " UI_MK_WHITE "PWM on GP46, 8 bits at 1.97 MHz\n"
        UI_MK_TEXT "Shaping        : " UI_MK_WHITE "8x oversampled, 1st order\n"
        UI_MK_TEXT "Output (S2)    : " UI_MK_WHITE "%s\n"
        UI_MK_TEXT "AY-3-8912      : " UI_MK_WHITE "libayemu, 1.7734 MHz\n"
        UI_MK_TEXT "TurboSound     : %s\n"
        UI_MK_TEXT "Covox / SounDrive: %s / %s\n"
        UI_MK_TEXT "Ring under/over/clip: " UI_MK_WHITE "%lu / %lu / %lu\n"
        "\n"
        UI_MK_HEAD "--- Storage ---\n"
        UI_MK_TEXT "Card           : %s\n"
        UI_MK_TEXT "USB stick      : " UI_MK_WHITE "%s\n"
        UI_MK_TEXT "Tape           : " UI_MK_WHITE "%s\n"
        UI_MK_TEXT "Beta Disk      : %s\n"
        UI_MK_TEXT "Disk           : " UI_MK_WHITE "%s\n"
        UI_MK_TEXT "FDC            : " UI_MK_WHITE "%s\n"
        UI_MK_TEXT "+3 drive       : " UI_MK_WHITE "%s\n"
        UI_MK_TEXT "Timex dock     : " UI_MK_WHITE "%s\n"
        UI_MK_TEXT "DivMMC         : %s\n"
        UI_MK_TEXT "  card image   : " UI_MK_WHITE "%s\n"
        UI_MK_TEXT "PSRAM          : " UI_MK_WHITE "%s\n"
        "\n"
        UI_MK_HEAD "--- Boot ---\n"
        UI_MK_TEXT "Self-test      : " UI_MK_OK "%u passed" UI_MK_TEXT "%s\n"
        UI_MK_TEXT "Firmware       : " UI_MK_WHITE "%s\n",
        spec_machine_name(),
        (snprintf(clk, sizeof clk, "%s", opt_turbo[settings.turbo < 3 ? settings.turbo : 0].label), clk),
        (unsigned long)z80_get_pace(), (unsigned long)z80_get_pace_floor(),
        yn[settings.issue2 ? 1 : 0],
        yn[settings.scanlines ? 1 : 0],
        yn[settings.ulaplus ? 1 : 0],
        spec_ulaplus_on ? UI_MK_OK "enabled by the guest" : UI_MK_DIM "not enabled",
        yn[settings.gigascreen ? 1 : 0],
        settings.timex ? (spec_timex_mode == 6 ? UI_MK_OK "hi-res"
                        : spec_timex_mode == 2 ? UI_MK_OK "hi-colour"
                        : spec_timex_mode == 1 ? UI_MK_OK "second screen"
                                               : UI_MK_OK "on, standard mode")
                       : UI_MK_DIM "Off",
        opt_uitheme[settings.ui_theme < UI_THEME_COUNT ? settings.ui_theme : 0].label,
        opt_output[settings.output < OUTPUT_COUNT ? settings.output : 0].label,
        yn[settings.turbosound ? 1 : 0],
        yn[settings.covox ? 1 : 0], yn[settings.soundrive ? 1 : 0],
        (unsigned long)audio_underruns, (unsigned long)audio_overruns,
        (unsigned long)audio_clips,
        storage_mounted() ? UI_MK_OK "mounted" : UI_MK_BAD "no card",
        usb_info(),
        tape_info(), yn[settings.betadisk ? 1 : 0], disk_info(), fdc_info(),
        dsk_info(),
        dck_info(),
        yn[settings.esxdos ? 1 : 0],
        mmc_info(),
        psram_info(),
        pass, fail ? UI_MK_BAD "  and some failed" : "",
        FIRMWARE_VERSION);

    ui_text_page("Emulator info", page_buf, spec_machine_name());
}

/*
 * The last two hundred and fifty-six bus cycles, on the screen.
 *
 * There is no other window into a machine whose CPU is a physical part:
 * a program that has gone wrong and a program that is working look
 * identical from outside, and the debugger can read the RP2350 but not
 * the Z80. The bus engine keeps a ring of every cycle it serviced and
 * this is the end of it -- which is where a CPU that has stopped getting
 * anywhere is going round and round.
 *
 * Recording is off unless asked for: it is two stores per cycle, on the
 * path that generates the clock.
 */
static void trace_page(void) {
    static const char *const cycle_name[] = {
        "?", "fetch", "read", "write", "in", "out", "int"
    };

    if (!z80_trace_on) {
        snprintf(page_buf, sizeof page_buf,
                 UI_MK_DIM "Bus recording is off.\n\n"
                 UI_MK_TEXT "Turn on " UI_MK_WHITE "Record the bus" UI_MK_TEXT
                 " in this menu,\nleave it, let the machine run, then come\nback here.\n\n"
                 UI_MK_DIM "It costs two stores on every bus cycle, which\n"
                 "is why it is not on by default.");
        ui_text_page("Bus trace", page_buf, NULL);
        return;
    }

    const uint32_t n = z80_trace_count();
    size_t used = (size_t)snprintf(page_buf, sizeof page_buf,
                                   UI_MK_DIM "  #  addr  data  cycle\n");
    for (uint32_t i = 0; i < n && used + 48 < sizeof page_buf; i++) {
        const bus_event_t e = z80_trace_get(i);
        const unsigned k = e.kind < (sizeof cycle_name / sizeof cycle_name[0]) ? e.kind : 0;
        used += (size_t)snprintf(page_buf + used, sizeof page_buf - used,
                                 UI_MK_TEXT "%3lu  " UI_MK_WHITE "%04X  " UI_MK_TEXT
                                 "%02X    %s%s\n",
                                 (unsigned long)i, e.addr, e.data,
                                 e.kind == BUS_OPFETCH ? UI_MK_OK : UI_MK_TEXT,
                                 cycle_name[k]);
    }
    if (!n) snprintf(page_buf, sizeof page_buf, UI_MK_DIM "Nothing recorded yet.");

    char right[24];
    snprintf(right, sizeof right, "%lu cycles", (unsigned long)n);
    ui_text_page("Bus trace", page_buf, right);
}

/* ── actions ─────────────────────────────────────────────────────────── */

static void tape_action(void) {
    if (!tape_mounted()) { say("No tape is mounted."); return; }
    if (!ui_confirm("Eject the tape?", "Tape", false)) return;
    tape_eject();
    say("Tape ejected.");
}

/*
 * Eject, having first offered to keep whatever the guest wrote.
 *
 * A game that saves its progress writes into the image in the PSRAM and
 * nowhere else, so an eject is where that work is lost. Asking is the
 * whole point: it is the only moment the firmware knows something is
 * about to go.
 */
static void disk_action(void) {
    if (!beta_mounted()) { say("No disk is in the drive."); return; }

    if (beta_dirty()) {
        const char *btns[3] = { "Save + eject", "Eject", "Cancel" };
        const int c = ui_choice("This disk has been written to.", btns, 3, 0, 2);
        if (c == 2) return;
        if (c == 0) {
            char d[80];
            if (!beta_save(d, sizeof d)) { say("%s", d); return; }
        }
    } else if (!ui_confirm("Eject the disk?", "Beta Disk", false)) {
        return;
    }

    beta_eject();
    say("Disk ejected.");
}

static void disk_save_action(void) {
    char d[80];
    beta_save(d, sizeof d);
    say("%s", d);
}

static const char *disk_state(void) {
    if (!beta_mounted()) return "no disk";
    return beta_dirty() ? "changed" : "saved";
}

static void tone_action(void) {
    const bool on = !spec_test_tone_is_on();
    spec_test_tone(on);
    say(on ? "441 Hz -- leave the menu to hear it" : "Test tone off.");
}

static void trace_changed(void) {
    z80_trace_on = settings.bus_trace != 0;
    if (z80_trace_on) z80_trace_reset();
    say("Bus recording %s", z80_trace_on ? "on" : "off");
}

static void psram_test_action(void) {
    if (!psram_size()) { say("No PSRAM to test."); return; }
    ui_status("Testing 4 MB...");
    ui_flush_dirty();
    const uint32_t bad = psram_test(1u << 20, 4u << 20);
    say(bad ? "%lu words wrong of 1048576" : "1048576 words, all correct",
        (unsigned long)bad);
}

/*
 * The machine's screen, to the card.
 *
 * The picture is still in spec_ram while this menu is standing in front
 * of it, so what lands on the card is the game and not the menu.
 */
static void capture_action(void) {
    char detail[64];
    ui_capture_screen(detail, sizeof detail);
    say("%s", detail);
}

static void rescan_action(void) {
    if (!storage_mounted() && !storage_mount()) { say("Still no card."); return; }
    menu_rescan_wanted = true;
    say("The card will be read again.");
}

static void save_action(void) {
    if (!storage_mounted()) { say("No card to save to."); return; }
    if (settings_save()) { say("Saved to /frank-dino.cfg"); return; }
    ui_toast("Could not write /frank-dino.cfg", true, 1500);
    say("Not saved.");
}

static void defaults_action(void) {
    /*
     * The one row here that throws work away, so it is the one that
     * asks. The default highlight is No: Enter must not destroy.
     */
    if (!ui_confirm("Discard every setting and\nstart again from the defaults?",
                    "Restore defaults", false)) {
        say("Left alone.");
        return;
    }
    settings_defaults();
    vga_build_ui_palette();
    spec_set_machine((machine_t)settings.machine);
    say("Defaults restored.");
}

static void reset_action(void) {
    /*
     * A reset loses whatever is in the machine, which after a tape load
     * is half an hour of somebody's evening.
     */
    if (!ui_confirm("Reset the machine?\nAnything loaded will be lost.",
                    "Reset", false)) {
        say("Left running.");
        return;
    }
    spec_set_machine((machine_t)settings.machine);
    say("%s reset.", spec_machine_name());
}

/* ── the tree ────────────────────────────────────────────────────────── */

static const ui_node_t machine_kids[] = {
    UI_RADIO("Machine",           &settings.machine,  opt_machine, machine_changed),
    UI_BOOL ("Issue 2 keyboard",  &settings.issue2,   NULL),
    UI_RADIO("Z80 speed",         &settings.turbo,    opt_turbo,   turbo_changed),
    UI_ACTION("Reset the machine", reset_action),
};

static const ui_node_t audio_kids[] = {
    UI_RADIO("Output (S2)",       &settings.output,     opt_output, output_changed),
    UI_INT  ("Master volume",     &settings.volume,     0, 10, 1, NULL),
    UI_INT  ("Beeper volume",     &settings.beeper_vol, 0, 10, 1, NULL),
    UI_INT  ("AY volume",         &settings.ay_vol,     0, 10, 1, NULL),
    UI_BOOL ("AY-3-8912 on a 48K", &settings.ay48,      NULL),
    UI_BOOL ("TurboSound",        &settings.turbosound, NULL),
    UI_BOOL ("Covox (#FB)",       &settings.covox,      NULL),
    UI_BOOL ("SounDrive",         &settings.soundrive,  NULL),
    UI_BOOL ("SAA1099 (#FF)",     &settings.saa,        NULL),
    UI_ACTION("Test tone (441 Hz)", tone_action),
    UI_ACTIONV("Audio ring",       NULL, audio_info),
};

/*
 * The menu's own colours, which the menu is standing in front of you
 * while you change -- so the table is rebuilt and the whole surface
 * repainted before this returns, and the choice shows itself.
 */
static void uitheme_changed(void) {
    vga_build_ui_palette();
    ui_draw_frame_once();
    ui_mark_dirty(D_ALL);
}

/*
 * Turning ULA+ off has to put the palette back: the guest may have left
 * it enabled, and the attribute byte means something different while it
 * is. The machine is paused here, so clearing the flag is enough -- the
 * next frame renders in the ordinary sixteen.
 */
static void ulaplus_changed(void) {
    if (!settings.ulaplus) spec_ulaplus_on = false;
    say(settings.ulaplus ? "Ports #BF3B/#FF3B answer now"
                         : "Back to the sixteen ZX colours");
}

/*
 * Turning the SCLD off has to clear the mode as well as the ports: the
 * guest may have left hi-res selected, and a machine without a SCLD in
 * it should go back to looking like one immediately.
 */
static void timex_changed(void) {
    if (!settings.timex) { spec_timex_mode = 0; spec_timex_ink = 0; }
    say(settings.timex ? "Port #FF selects the video mode"
                       : "Port #FF is the floating bus again");
}

static void gigascreen_changed(void) {
    say(settings.gigascreen ? "Banks 5 and 7 blended, no flicker"
                            : "One screen at a time again");
}

static const ui_node_t video_kids[] = {
    UI_BOOL ("Scanlines",    &settings.scanlines, NULL),
    UI_BOOL ("ULA+ (64 colours)", &settings.ulaplus, ulaplus_changed),
    UI_BOOL ("Gigascreen",   &settings.gigascreen, gigascreen_changed),
    UI_BOOL ("Timex video (#FF)", &settings.timex, timex_changed),
    UI_RADIO("Menu colours", &settings.ui_theme,  opt_uitheme, uitheme_changed),
};

static const ui_node_t input_kids[] = {
    UI_RADIO("Joystick",           &settings.joystick,      opt_joystick, NULL),
    UI_RADIO("Kempston port",      &settings.kempston_port, opt_kport,    NULL),
    UI_BOOL ("Cursor keys as joy", &settings.cursor_as_joy, NULL),
};

/*
 * The tape input shares GP45 with the console's RX -- the schematic
 * calls the net TAPE_UART_RX -- so switching it on takes the pin and
 * nothing typed at the console arrives until it goes off again.
 */
static void real_player_changed(void) {
    if (settings.real_player) tapein_start();
    else                      tapein_stop();
    say(settings.real_player ? "Tape input on J9; console RX is off"
                             : "Tape input off; console RX is back");
}

static bool real_player_on(void) { return settings.real_player != 0; }

/* Live, so the level can be set by watching it rather than by guessing. */
static const char *tapein_info(void) {
    static char buf[32];
    tapein_stats(buf, sizeof buf);
    return buf;
}

static const ui_node_t storage_kids[] = {
    UI_BOOL   ("Auto-start tapes", &settings.tape_autostart, NULL),
    UI_BOOL   ("Real tape on J9",  &settings.real_player, real_player_changed),
    UI_ACTIONV_EN("  Tape input",  NULL, tapein_info, real_player_on),
    UI_BOOL   ("Write-protect disks", &settings.disk_wrprot, NULL),
    UI_ACTIONV("Eject tape",       tape_action, tape_info),
    UI_BOOL   ("Beta Disk (TR-DOS)", &settings.betadisk, betadisk_changed),
    UI_ACTIONV_EN("  Eject disk",     disk_action, disk_info, betadisk_on),
    UI_ACTIONV_EN("  Save disk",      disk_save_action, disk_state, betadisk_on),
    UI_ACTIONV_EN("  Last FDC command", NULL,      fdc_info,  betadisk_on),
    UI_ACTIONV_EN("+3 drive",        dsk_action, dsk_info, is_plus3),
    UI_ACTIONV_EN("  Save +3 disk",  dsk_save_action, dsk_state, is_plus3),
    UI_ACTIONV_EN("Timex dock",     dck_action, dck_info, is_timex),
    UI_ACTIONV_EN("  uPD765",        NULL,       upd_info, is_plus3),
    UI_ACTION ("Screenshot to card", capture_action),
    UI_ACTIONV("USB stick",       NULL, usb_info),
    UI_BOOL   ("DivMMC / esxDOS", &settings.esxdos, esxdos_changed),
    UI_ACTIONV_EN("  Card image",  mmc_action, mmc_info, esxdos_on),
    UI_ACTION ("Re-read the card", rescan_action),
};

static const ui_node_t system_kids[] = {
    UI_ACTIONV("PSRAM",          NULL, psram_info),
    UI_ACTION ("Test the PSRAM", psram_test_action),
    UI_ACTIONV("Z80",            NULL, z80_info),
#if USB_KEYBOARD
    UI_ACTIONV("USB keyboard",   NULL, kbd_info),
#endif
    UI_BOOL   ("Record the bus", &settings.bus_trace, trace_changed),
    UI_PAGE   ("Bus trace",      trace_page),
};

static const ui_node_t root[] = {
    UI_SUB("Machine",  machine_kids, NULL),
    UI_SUB("Audio",    audio_kids,   NULL),
    UI_SUB("Video",    video_kids,   NULL),
    UI_SUB("Input",    input_kids,   NULL),
    UI_SUB("Storage",  storage_kids, NULL),
    UI_SUB("System",   system_kids,  NULL),
    UI_PAGE("Emulator info", info_page),
    UI_ACTION("Save settings to the card", save_action),
    UI_ACTION("Restore defaults",          defaults_action),
};

const ui_node_t *ui_root_nodes(void) { return root; }
uint8_t          ui_root_count(void) { return UI_COUNT(root); }

const char *ui_machine_line(void) {
    static char buf[48];
    snprintf(buf, sizeof buf, "Machine: %s", spec_machine_name());
    return buf;
}
