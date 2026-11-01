/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Memory over the console, for tools/dino.py.
 *
 * The test harness reads and writes variables by their ELF address, and
 * did it over SWD -- which on this board is a marginal link that drops
 * out for good until the probe is reseated. The console cable is the
 * one that never goes away, so the same two operations live here too:
 *
 *   DLE 'R' aaaaaaaa nnnn LF        ->  "@M " + 2n hex digits + LF
 *   DLE 'W' aaaaaaaa hh hh ... LF   ->  "@M" LF
 *
 * DLE (0x10) is not a key anything sends, so a key stream never starts
 * one by accident. Only RAM, PSRAM and flash are reachable; anything
 * else answers "@E". A read that is word-aligned in address and length
 * is done a word at a time, so a 32-bit counter reads whole.
 */

#include "menu.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <pico/stdlib.h>

#include "loader.h"
#include "spectrum.h"
#include "storage.h"
#include "ff.h"

#define DLE 0x10

static bool readable(const uint32_t a, const uint32_t n) {
    const uint32_t e = a + n;
    if (e < a) return false;
    return (a >= 0x10000000u && e <= 0x12000000u)          // flash and PSRAM, XIP
        || (a >= 0x20000000u && e <= 0x20082000u);         // SRAM
}

static bool writable(const uint32_t a, const uint32_t n) {
    const uint32_t e = a + n;
    if (e < a) return false;
    return (a >= 0x11000000u && e <= 0x12000000u)
        || (a >= 0x20000000u && e <= 0x20082000u);
}

static int hexval(const int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The rest of a command, up to LF; false if it never comes. */
static bool read_line(char *buf, const unsigned max, unsigned *len) {
    unsigned n = 0;
    for (;;) {
        const int c = getchar_timeout_us(100000);
        if (c == PICO_ERROR_TIMEOUT) return false;
        if (c == '\n' || c == '\r') break;
        if (n < max) buf[n++] = (char)c;
    }
    *len = n;
    return true;
}

static bool hex_at(const char *s, const unsigned digits, uint32_t *out) {
    uint32_t v = 0;
    for (unsigned i = 0; i < digits; i++) {
        const int h = hexval(s[i]);
        if (h < 0) return false;
        v = (v << 4) | (uint32_t)h;
    }
    *out = v;
    return true;
}

static void command(void) {
    static char line[2 + 8 + 2 * 512];
    unsigned n;
    const int op = getchar_timeout_us(100000);
    if (!read_line(line, sizeof line, &n)) return;
    uint32_t a, len;
    if (n < 8 || !hex_at(line, 8, &a)) { puts("@E"); return; }

    if (op == 'R') {
        if (n < 12 || !hex_at(line + 8, 4, &len) || len > 1024 || !readable(a, len)) {
            puts("@E");
            return;
        }
        printf("@M ");
        if (!(a & 3u) && !(len & 3u)) {
            for (uint32_t i = 0; i < len; i += 4) {
                const uint32_t w = *(volatile const uint32_t *)(uintptr_t)(a + i);
                printf("%02x%02x%02x%02x", (unsigned)(w & 0xFF), (unsigned)((w >> 8) & 0xFF),
                       (unsigned)((w >> 16) & 0xFF), (unsigned)(w >> 24));
            }
        } else {
            for (uint32_t i = 0; i < len; i++)
                printf("%02x", *(volatile const uint8_t *)(uintptr_t)(a + i));
        }
        printf("\n");
        return;
    }

    if (op == 'W') {
        len = (n - 8) / 2;
        if (!len || !writable(a, len)) { puts("@E"); return; }
        uint8_t b[512];
        for (uint32_t i = 0; i < len; i++) {
            uint32_t v;
            if (!hex_at(line + 8 + 2 * i, 2, &v)) { puts("@E"); return; }
            b[i] = (uint8_t)v;
        }
        if (!(a & 3u) && len == 4) {
            *(volatile uint32_t *)(uintptr_t)a = (uint32_t)b[0] | ((uint32_t)b[1] << 8)
                                               | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        } else if (!(a & 1u) && len == 2) {
            *(volatile uint16_t *)(uintptr_t)a = (uint16_t)(b[0] | (b[1] << 8));
        } else {
            for (uint32_t i = 0; i < len; i++) *(volatile uint8_t *)(uintptr_t)(a + i) = b[i];
        }
        puts("@M");
        return;
    }
    puts("@E");
}

int console_getchar(void) {
    for (;;) {
        const int c = getchar_timeout_us(0);
        if (c != DLE) return c;
        command();
    }
}

/*
 * Loading a file, for the harness: what the browser does when Enter is
 * pressed on it, without walking the browser there. A tool writes the
 * path and sets harness_req; the main loop calls harness_poll().
 */
volatile char    harness_path[96];
volatile uint8_t harness_req;
volatile uint8_t harness_ok;
char             harness_detail[64];

/*
 * ...and writing one: harness_req = 2 puts harness_len bytes from
 * harness_src (anywhere the tool has filled, the PSRAM usually) into the
 * file at harness_off, creating it. How a test gets a disk image onto the
 * card without taking the card out.
 */
volatile uint32_t harness_src, harness_len, harness_off;

static bool harness_write(const char *path) {
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_OPEN_ALWAYS) != FR_OK) return false;
    UINT put = 0;
    const bool ok = f_lseek(&f, harness_off) == FR_OK
                 && f_write(&f, (const void *)(uintptr_t)harness_src, harness_len, &put) == FR_OK
                 && put == harness_len;
    f_close(&f);
    snprintf(harness_detail, sizeof harness_detail, "%lu bytes at %lu",
             (unsigned long)put, (unsigned long)harness_off);
    return ok;
}

void harness_poll(void) {
    if (!harness_req) return;
    char path[sizeof harness_path];
    memcpy(path, (const char *)harness_path, sizeof path);
    path[sizeof path - 1] = '\0';
    if (harness_req == 2) {
        harness_ok = harness_write(path) ? 1u : 0u;
        harness_req = 0;
        return;
    }
    spec_pause();
    harness_ok = loader_load(path, storage_kind(path), harness_detail, sizeof harness_detail) ? 1u : 0u;
    spec_resume();
    harness_req = 0;
}
