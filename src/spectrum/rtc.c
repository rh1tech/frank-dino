/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * After pico-speccy's devices/storage/RTC.cpp
 * (https://github.com/drewpo28/pico-speccy, GPL-3.0) by drew.po28: the
 * MC146818 behind Mr Gluk's ports, with its NVRAM persisted to the card.
 */

/*
 * The Mr Gluk clock: an MC146818 on #DFF7 (register select) and #BFF7
 * (data), as the Pentagon, Scorpion, KAY and ATM machines have it.
 *
 * This board has no RTC and no battery, so the clock starts at the time
 * the firmware was built and counts from power-on; a program that sets
 * it moves it, until the next power cycle. The 50 bytes of NVRAM, which
 * is what the BIOSes keep their settings in, are written to /cmos.nvr a
 * moment after the last change and read back at boot.
 */
#include "rtc.h"

#include <stdio.h>
#include <string.h>

#include "pico/time.h"

#ifndef RTC_HOST_TEST
#include "ff.h"
#endif

static uint8_t  sel;
static uint8_t  nvram[64];          // 0-13 the clock's own, 14-63 the NVRAM
static int64_t  offset_s;           // seconds added to build time + uptime
static volatile bool     dirty;
static volatile uint64_t dirty_at;

// ---------------------------------------------------------------------------
// Calendar
// ---------------------------------------------------------------------------
/* Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm). */
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? (unsigned)-3 : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    *d = doy - (153u * mp + 2u) / 5u + 1u;
    *m = mp < 10u ? mp + 3u : mp - 9u;
    *y = (int)(yoe + (unsigned)era * 400u + (*m <= 2u));
}

/* The build's own date and time, which is where the clock starts. */
static int64_t build_epoch(void) {
    static const char *const mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__, *t = __TIME__;
    unsigned m = 1;
    for (unsigned i = 0; i < 12; i++) if (!memcmp(d, mon + i * 3u, 3)) m = i + 1u;
    const unsigned day = (unsigned)((d[4] == ' ' ? 0 : d[4] - '0') * 10 + (d[5] - '0'));
    const int year = (d[7] - '0') * 1000 + (d[8] - '0') * 100 + (d[9] - '0') * 10 + (d[10] - '0');
    const int hh = (t[0] - '0') * 10 + (t[1] - '0');
    const int mm = (t[3] - '0') * 10 + (t[4] - '0');
    const int ss = (t[6] - '0') * 10 + (t[7] - '0');
    return days_from_civil(year, m, day) * 86400 + hh * 3600 + mm * 60 + ss;
}

static int64_t now_s(void) {
    return build_epoch() + (int64_t)(time_us_64() / 1000000u) + offset_s;
}

/* Register B bit 2: binary; clear (the default) is BCD. */
static uint8_t enc(const unsigned v) {
    return (nvram[11] & 0x04u) ? (uint8_t)v : (uint8_t)(((v / 10u) << 4) | (v % 10u));
}
static unsigned dec(const uint8_t v) {
    return (nvram[11] & 0x04u) ? v : (unsigned)((v >> 4) * 10u + (v & 15u));
}

static uint8_t clock_reg(const uint8_t r) {
    const int64_t s = now_s();
    const int64_t days = s >= 0 ? s / 86400 : (s - 86399) / 86400;
    const int64_t sod = s - days * 86400;
    int y; unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    switch (r) {
        case 0: return enc((unsigned)(sod % 60));
        case 2: return enc((unsigned)((sod / 60) % 60));
        case 4: return enc((unsigned)(sod / 3600));
        case 6: return (uint8_t)(((days + 4) % 7 + 7) % 7 + 1);    // 1 = Sunday
        case 7: return enc(d);
        case 8: return enc(m);
        case 9: return enc((unsigned)(y % 100));
        case 10: return (uint8_t)(nvram[10] & 0x7Fu);               // never mid-update
        case 12: return 0;
        case 13: return 0x80;                                       // the battery is fine
        default: return nvram[r];
    }
}

/* A write to a time register moves the whole clock to agree. */
static void set_clock_reg(const uint8_t r, const uint8_t v) {
    const int64_t s = now_s();
    const int64_t days = s >= 0 ? s / 86400 : (s - 86399) / 86400;
    int64_t sod = s - days * 86400;
    int y; unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    unsigned hh = (unsigned)(sod / 3600), mi = (unsigned)((sod / 60) % 60), ss = (unsigned)(sod % 60);
    switch (r) {
        case 0: ss = dec(v) % 60u; break;
        case 2: mi = dec(v) % 60u; break;
        case 4: hh = dec(v & 0x7Fu) % 24u; break;
        case 7: d = dec(v) ? dec(v) : 1u; break;
        case 8: m = (dec(v) >= 1u && dec(v) <= 12u) ? dec(v) : m; break;
        case 9: y = 2000 + (int)dec(v); break;
        default: return;
    }
    const int64_t want = days_from_civil(y, m, d) * 86400 + hh * 3600 + mi * 60 + ss;
    offset_s += want - s;
}

// ---------------------------------------------------------------------------
// Ports
// ---------------------------------------------------------------------------
void rtc_select(const uint8_t reg) { sel = reg & 0x3Fu; }

uint8_t rtc_read(void) {
    return sel < 14 ? clock_reg(sel) : nvram[sel];
}

void rtc_write(const uint8_t v) {
    if (sel < 10 && sel != 1 && sel != 3 && sel != 5) { set_clock_reg(sel, v); return; }
    if (nvram[sel] == v) return;
    nvram[sel] = v;
    if (sel >= 14) { dirty = true; dirty_at = time_us_64(); }
}

// ---------------------------------------------------------------------------
// The card
// ---------------------------------------------------------------------------
#define NVR_PATH "/cmos.nvr"

void rtc_init(void) {
    memset(nvram, 0, sizeof nvram);
    nvram[11] = 0x02;                       // 24-hour, BCD
#ifndef RTC_HOST_TEST
    FIL f;
    if (f_open(&f, NVR_PATH, FA_READ) == FR_OK) {
        UINT got = 0;
        uint8_t buf[50];
        if (f_read(&f, buf, sizeof buf, &got) == FR_OK && got == sizeof buf)
            memcpy(nvram + 14, buf, sizeof buf);
        f_close(&f);
    }
#endif
}

void rtc_task(void) {
#ifndef RTC_HOST_TEST
    // A second and a half after the last write: a BIOS writes its settings
    // a byte at a time, and the card should see one file, not fifty.
    if (!dirty || time_us_64() - dirty_at < 1500000u) return;
    dirty = false;
    FIL f;
    if (f_open(&f, NVR_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    UINT put = 0;
    f_write(&f, nvram + 14, 50, &put);
    f_close(&f);
#endif
}
