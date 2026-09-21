/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The bring-up sequence is ported from pico-speccy (GPL-3.0-or-later),
 * which runs the same ESP-PSRAM64H on the same family of board.
 */

#include "psram.h"

#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <hardware/structs/qmi.h>
#include <hardware/structs/xip_ctrl.h>
#include <hardware/sync.h>

#include "dino.h"

static uint32_t found;

uint32_t psram_size(void) { return found; }

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
/*
 * Divide the system clock down to something the chip will keep up with.
 *
 * An APS6404 is good for 133 MHz; this board runs the RP2350 at 504, so
 * the divisor is four and the read strobe needs an extra cycle of delay
 * on top. The select and deselect limits are the chip's: a chip select
 * may not stay asserted for more than 8 us and must be released for at
 * least 18 ns, both expressed here in system clocks.
 */
#define PSRAM_MAX_HZ (133 * MHZ)

static void __not_in_flash_func(retime)(void) {
    const int clock_hz = (int)clock_get_hz(clk_sys);

    int divisor = (clock_hz + PSRAM_MAX_HZ - 1) / PSRAM_MAX_HZ;
    if (divisor == 1 && clock_hz > 100 * MHZ) divisor = 2;

    int rxdelay = divisor;
    if (clock_hz / divisor > 100 * MHZ) rxdelay += 1;

    const int period_fs   = (int)(1000000000000000ll / clock_hz);
    const int max_select  = (125 * 1000000) / period_fs;                 // 8 us
    const int min_deselect = (18 * 1000000 + (period_fs - 1)) / period_fs
                             - (divisor + 1) / 2;                        // 18 ns

    qmi_hw->m[1].timing = 1u << QMI_M1_TIMING_COOLDOWN_LSB |
                          QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
                          (uint32_t)max_select   << QMI_M1_TIMING_MAX_SELECT_LSB |
                          (uint32_t)min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
                          (uint32_t)rxdelay      << QMI_M1_TIMING_RXDELAY_LSB |
                          (uint32_t)divisor      << QMI_M1_TIMING_CLKDIV_LSB;
}

// ---------------------------------------------------------------------------
// Is there a chip?
// ---------------------------------------------------------------------------
/*
 * Exit QPI in case a warm reboot left the chip in it, then Read ID.
 *
 * An APS6404-compatible part answers with manufacturer 0x0D and a
 * known-good-die byte of 0x5D. With no chip fitted the four data lines
 * float and read back as 0x00 or 0xFF, which is how "assembled without
 * PSRAM" is told from "assembled with".
 *
 * Three attempts, because the first transaction after the chip's own
 * reset is documented to come back as noise on several of these parts.
 */
static bool __no_inline_not_in_flash_func(detect)(void) {
    for (int attempt = 0; attempt < 3; attempt++) {
        qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
        qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
                            (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB) |
                            0xF5;                       // exit QPI
        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) tight_loop_contents();
        (void)qmi_hw->direct_rx;
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
        for (volatile int d = 0; d < 64; d++) { }       // min deselect

        qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
        uint8_t mfid = 0, kgd = 0;
        for (int i = 0; i < 6; i++) {                   // 0x9F + 3 address, then two
            qmi_hw->direct_tx = (i == 0) ? 0x9F : 0xFF;
            while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS)) tight_loop_contents();
            while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) tight_loop_contents();
            const uint8_t b = (uint8_t)qmi_hw->direct_rx;
            if      (i == 4) mfid = b;
            else if (i == 5) kgd  = b;
        }
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
        for (volatile int d = 0; d < 64; d++) { }

        if (kgd == 0x5D || mfid == 0x5D) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// How much
// ---------------------------------------------------------------------------
/*
 * Plant a marker in each power-of-two region and see which survives.
 *
 * These chips alias: a 4 MB part mapped into a 16 MB window answers the
 * top quarter with the bottom quarter's contents. So writing a different
 * value across each region and then reading the very top says how large
 * the part really is, in megabytes.
 *
 * A floating bus can read back a consistent value too, which is why the
 * answer is only believed when it is one of the four values the probe
 * actually wrote.
 */
#define MB(n) ((uint32_t)(n) << 20)

static uint32_t __not_in_flash_func(probe_size)(void) {
    volatile uint8_t *const p = (volatile uint8_t *)PSRAM_BASE;

    for (uint32_t i = MB(8);  i < MB(16); i += 4096) p[i] = 16;
    for (uint32_t i = MB(4);  i < MB(8);  i += 4096) p[i] = 8;
    for (uint32_t i = MB(1);  i < MB(4);  i += 4096) p[i] = 4;
    for (uint32_t i = 0;      i < MB(1);  i += 4096) p[i] = 1;

    const uint8_t res = p[MB(16) - 4096];
    for (uint32_t i = MB(16) - MB(1); i < MB(16); i += 4096)
        if (p[i] != res) return 0;                  // not even self-consistent

    if (res != 1 && res != 4 && res != 8 && res != 16) return 0;
    return (uint32_t)res << 20;
}

// ---------------------------------------------------------------------------

uint32_t __no_inline_not_in_flash_func(psram_init)(void) {
    gpio_set_function(PSRAM_CS_PIN, GPIO_FUNC_XIP_CS1);

    /*
     * The direct-mode window, and the discipline it needs.
     *
     * While DIRECT_CSR.EN is set, CS0 -- the flash -- is unavailable, so
     * any instruction fetched from XIP stalls the core for ever. That
     * covers a call into flash-resident code and any interrupt whose
     * handler lives there. Hence: interrupts off for the whole window,
     * nothing but RAM-resident code inside it, and the window closed
     * before the retiming and format registers are written, which are
     * plain stores and do not need direct mode at all.
     *
     * This firmware is a copy_to_ram build, so nothing is in XIP to begin
     * with -- but the rule costs nothing and is the difference between
     * this working and an intermittent hang if that ever changes.
     */
    const uint32_t ints = save_and_disable_interrupts();

    qmi_hw->direct_csr = 30u << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) tight_loop_contents();

    if (!detect()) {
        qmi_hw->direct_csr = 0;
        restore_interrupts(ints);
        found = 0;
        return 0;
    }

    // Again with automatic chip select, and faster.
    qmi_hw->direct_csr = 10u << QMI_DIRECT_CSR_CLKDIV_LSB |
                         QMI_DIRECT_CSR_EN_BITS |
                         QMI_DIRECT_CSR_AUTO_CS1N_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) tight_loop_contents();

    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | 0x35;       // enter QPI
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) tight_loop_contents();

    qmi_hw->direct_csr = 0;
    restore_interrupts(ints);

    retime();

    // Fast Read Quad, 0xEB, six dummy cycles; everything quad-width.
    qmi_hw->m[1].rfmt =
        QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB |
        QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q   << QMI_M0_RFMT_ADDR_WIDTH_LSB   |
        QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB |
        QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M0_RFMT_DUMMY_WIDTH_LSB  |
        QMI_M0_RFMT_DATA_WIDTH_VALUE_Q   << QMI_M0_RFMT_DATA_WIDTH_LSB   |
        QMI_M0_RFMT_PREFIX_LEN_VALUE_8   << QMI_M0_RFMT_PREFIX_LEN_LSB   |
        6u                               << QMI_M0_RFMT_DUMMY_LEN_LSB;
    qmi_hw->m[1].rcmd = 0xEB;

    // Quad Write, 0x38, no dummy cycles.
    qmi_hw->m[1].wfmt =
        QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB |
        QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q   << QMI_M0_WFMT_ADDR_WIDTH_LSB   |
        QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB |
        QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M0_WFMT_DUMMY_WIDTH_LSB  |
        QMI_M0_WFMT_DATA_WIDTH_VALUE_Q   << QMI_M0_WFMT_DATA_WIDTH_LSB   |
        QMI_M0_WFMT_PREFIX_LEN_VALUE_8   << QMI_M0_WFMT_PREFIX_LEN_LSB;
    qmi_hw->m[1].wcmd = 0x38;

    // Without this the window is read-only and every store is discarded
    // in silence, which looks exactly like memory that does not hold.
    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);

    found = probe_size();
    return found;
}

// ---------------------------------------------------------------------------

/*
 * A pattern that depends on its own address.
 *
 * Which is the point: a stuck address line, a short between two of them
 * or a receive delay that samples a bit early all produce the right
 * *kind* of data at the wrong place, and a constant pattern cannot see
 * any of it. The multiply is a cheap mixer, not a hash.
 */
static inline uint32_t pattern(const uint32_t addr) {
    return (addr * 2654435761u) ^ (addr << 7) ^ 0xA5A5A5A5u;
}

uint32_t psram_test(const uint32_t from, const uint32_t bytes) {
    if (!found || from + bytes > found) return 0xFFFFFFFFu;

    volatile uint32_t *const p = (volatile uint32_t *)(PSRAM_BASE + from);
    const uint32_t words = bytes / 4;

    for (uint32_t i = 0; i < words; i++) p[i] = pattern(i);

    uint32_t bad = 0;
    for (uint32_t i = 0; i < words; i++)
        if (p[i] != pattern(i)) bad++;

    return bad;
}
