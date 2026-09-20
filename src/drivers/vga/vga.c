/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "vga.h"

#include <string.h>
#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/irq.h>
#include <hardware/pio.h>

#include "dino.h"       // likely()/unlikely(), and VGA_BASE_PIN
#include "font8x16.h"

#ifndef VGA_BASE_PIN
#define VGA_BASE_PIN 30
#endif

#define PIO_VGA     pio1
#define VGA_DMA_IRQ DMA_IRQ_0
#define VGA_PINS    8

// ============================================================================
// The wire format
// ============================================================================
//
// One byte per pixel, straight out of a PIO `out pins, 8` onto GP30-GP37,
// which is the R2R ladder and the two sync lines:
//
//   bit 7  VSYNC   bit 6  HSYNC   bits 5-4  R   bits 3-2  G   bits 1-0  B
//
// Both syncs are active low at 640x480/60, so an active-video byte has
// both high — every colour below carries 0xC0 and the sync templates
// clear one or both bits.
#define PX(r, g, b) ((uint8_t)(0xC0u | ((r) << 4) | ((g) << 2) | (b)))

/*
 * Sixteen colours, two bits a channel.
 *
 * The low-intensity half uses 10 rather than 01 for a lit channel — two
 * thirds, not one third. frank-xt8086 learned this the expensive way:
 * with 01 the whole palette came off the capture stick uniformly dim, and
 * CGA blue measured at a quarter of full scale instead of two thirds.
 * A two-resistor ladder has no room to be subtle about it.
 */
static const uint8_t palette16[16] = {
    PX(0, 0, 0),  // 0  black
    PX(0, 0, 2),  // 1  blue
    PX(0, 2, 0),  // 2  green
    PX(0, 2, 2),  // 3  cyan
    PX(2, 0, 0),  // 4  red
    PX(2, 0, 2),  // 5  magenta
    PX(2, 1, 0),  // 6  brown    — red plus half green
    PX(2, 2, 2),  // 7  light grey
    PX(1, 1, 1),  // 8  dark grey
    PX(0, 0, 3),  // 9  bright blue
    PX(0, 3, 0),  // 10 bright green
    PX(0, 3, 3),  // 11 bright cyan
    PX(3, 0, 0),  // 12 bright red
    PX(3, 0, 3),  // 13 bright magenta
    PX(3, 3, 0),  // 14 yellow
    PX(3, 3, 3),  // 15 white
};

// ============================================================================
// Timing
// ============================================================================
//
// Industry-standard 640x480 at 59.94 Hz: 800 pixels and 525 lines at
// 25.175 MHz, both syncs negative. One output byte per pixel, so a line
// is 800 bytes whatever is in it.
#define PIXEL_CLOCK      25175000
#define LINE_BYTES       800
#define VISIBLE_BYTES    640
#define TOTAL_LINES      525
#define VISIBLE_LINES    480
#define VSYNC_START_LINE 490
#define VSYNC_END_LINE   491

/*
 * Where the picture starts inside the line buffer.
 *
 * The DMA starts each buffer at byte 0 and the line begins with the sync
 * pulse, so the visible 640 bytes sit at an offset: the front porch and
 * the sync and the back porch come first. 144 bytes is 16 front porch +
 * 96 sync + 32 of the 48-byte back porch — the remaining 16 are the shift
 * that puts the picture where a display expects it. Moving this moves the
 * image sideways and nothing else.
 */
#define PICTURE_OFFSET_BYTES 144
#define HSYNC_BYTES          96

uint8_t vga_text[VGA_COLS * VGA_ROWS * 2];
volatile uint32_t vga_frames;

// --- PIO: one instruction, `out pins, 8`, wrapped onto itself ---
static uint16_t vga_pio_instructions[] = { 0x6008 };
static const struct pio_program vga_pio_program = {
    .instructions = vga_pio_instructions,
    .length = 1,
    .origin = -1,
};

// scanline_buffers: [0] blank line, [1] vsync line, [2] the picture line
enum { VBLANK, VSYNC, IMAGE, SCANLINE_BUFFERS };
static uint32_t *scanline_buffers[SCANLINE_BUFFERS];
static uint32_t  scanline_mem[SCANLINE_BUFFERS * (LINE_BYTES / 4)] __attribute__((aligned(32)));

/*
 * A second physical buffer behind the IMAGE slot.
 *
 * The DMA control channel reads scanline_buffers[IMAGE] the instant it
 * fires, to hand the data channel a source for the line going out now —
 * and the interrupt that follows immediately starts rendering the *next*
 * line into it. With one buffer those are the same memory, and whichever
 * words the data channel has not reached yet come out as the new line
 * instead of the old one: a sparse, timing-dependent tear. Alternating
 * gives each buffer a full line to drain.
 */
static uint32_t image_buffer_b[LINE_BYTES / 4] __attribute__((aligned(32)));

static uint sm;
static int dma_ctrl_channel;
static int dma_data_channel;

/*
 * Nibble to four pixel masks.
 *
 * expand4[n] has 0xFF in the byte of every lit pixel and 0x00 elsewhere,
 * with bit 0 of n becoming the *first* byte.
 *
 * Bit 0, not bit 7. In this font the least significant bit of a glyph
 * byte is the **leftmost** pixel, which is the opposite of the usual
 * convention and is not something the data announces — every character
 * worth checking by eye is very nearly symmetrical. Assuming bit 7 put
 * every glyph on the screen mirrored, and mirrored text is legible
 * enough in a thumbnail that it took a capture to notice at all. Check
 * it against 'L' (0x4C) if this is ever in doubt: it is the one letter
 * with nothing symmetrical about it.
 *
 * The first byte the DMA sends is the low byte of the word, so the low
 * nibble makes the first word. Sixty-four bytes of table turns a glyph
 * row into two masked selects.
 */
static uint32_t expand4[16];

static void build_expand4(void) {
    for (int n = 0; n < 16; n++) {
        uint32_t v = 0;
        for (int b = 0; b < 4; b++)
            if (n & (1u << b)) v |= 0xFFu << (b * 8);
        expand4[n] = v;
    }
}

// ============================================================================
// One scanline
// ============================================================================

void __time_critical_func(vga_scanline)(void) {
    static uint32_t line = 0;
    static bool image_toggle = false;

    dma_hw->ints0 = 1u << dma_ctrl_channel;

    if (unlikely(++line == TOTAL_LINES)) {
        line = 0;
        vga_frames++;
    }

    if (unlikely(line >= VISIBLE_LINES)) {
        const int which = (line >= VSYNC_START_LINE && line <= VSYNC_END_LINE) ? VSYNC : VBLANK;
        dma_channel_set_read_addr(dma_ctrl_channel, &scanline_buffers[which], false);
        return;
    }

    image_toggle = !image_toggle;
    scanline_buffers[IMAGE] = image_toggle ? image_buffer_b
                                           : &scanline_mem[IMAGE * (LINE_BYTES / 4)];
    dma_channel_set_read_addr(dma_ctrl_channel, &scanline_buffers[IMAGE], false);

    uint32_t *__restrict out = scanline_buffers[IMAGE] + PICTURE_OFFSET_BYTES / 4;

    const uint32_t glyph_line = line & 15u;
    const uint8_t *__restrict cell = &vga_text[(line >> 4) * (VGA_COLS * 2)];

    for (int col = 0; col < VGA_COLS; col++) {
        const uint8_t  ch   = *cell++;
        const uint8_t  attr = *cell++;
        const uint8_t  bits = font_8x16[ch * 16 + glyph_line];

        const uint32_t fg = (uint32_t)palette16[attr & 0x0F] * 0x01010101u;
        const uint32_t bg = (uint32_t)palette16[attr >> 4]   * 0x01010101u;

        const uint32_t m0 = expand4[bits & 15];     // pixels 0-3, the left half
        const uint32_t m1 = expand4[bits >> 4];     // pixels 4-7

        *out++ = (fg & m0) | (bg & ~m0);
        *out++ = (fg & m1) | (bg & ~m1);
    }
}

// ============================================================================
// Bring-up
// ============================================================================

static void build_sync_templates(void) {
    const uint8_t active = 0xC0;            // both syncs idle high
    const uint8_t hsync  = active ^ 0x40;   // H low
    const uint8_t vsync  = active ^ 0x80;   // V low, H idle
    const uint8_t both   = active ^ 0xC0;   // the sync pulse inside a V line

    memset(scanline_buffers[VBLANK], active, LINE_BYTES);
    memset(scanline_buffers[VBLANK], hsync,  HSYNC_BYTES);

    memset(scanline_buffers[VSYNC], vsync, LINE_BYTES);
    memset(scanline_buffers[VSYNC], both,  HSYNC_BYTES);

    // The picture lines start life as blank lines and only ever have
    // their visible 640 bytes overwritten, so the sync and the porches
    // are written once here and never touched again.
    memcpy(scanline_buffers[IMAGE], scanline_buffers[VBLANK], LINE_BYTES);
    memcpy(image_buffer_b,          scanline_buffers[VBLANK], LINE_BYTES);
}

static void configure_dma(void) {
    dma_channel_config data_cfg = dma_channel_get_default_config(dma_data_channel);
    channel_config_set_transfer_data_size(&data_cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&data_cfg, true);
    channel_config_set_write_increment(&data_cfg, false);
    channel_config_set_dreq(&data_cfg, (PIO_VGA == pio0 ? DREQ_PIO0_TX0 : DREQ_PIO1_TX0) + sm);
    channel_config_set_chain_to(&data_cfg, dma_ctrl_channel);
    dma_channel_configure(dma_data_channel, &data_cfg,
                          &PIO_VGA->txf[sm], scanline_buffers[VBLANK],
                          LINE_BYTES / 4, false);

    // The control channel's whole job is to reload the data channel's
    // read address from a pointer the scanline interrupt has already
    // updated, then chain straight back into it. Two channels ping-pong
    // for ever and the CPU is only ever asked what the *next* line is.
    dma_channel_config ctrl_cfg = dma_channel_get_default_config(dma_ctrl_channel);
    channel_config_set_transfer_data_size(&ctrl_cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&ctrl_cfg, false);
    channel_config_set_write_increment(&ctrl_cfg, false);
    channel_config_set_chain_to(&ctrl_cfg, dma_data_channel);
    dma_channel_configure(dma_ctrl_channel, &ctrl_cfg,
                          &dma_hw->ch[dma_data_channel].read_addr,
                          &scanline_buffers[VBLANK], 1, false);
}

void vga_init(void) {
    build_expand4();

    for (int i = 0; i < SCANLINE_BUFFERS; i++)
        scanline_buffers[i] = &scanline_mem[i * (LINE_BYTES / 4)];

    memset(vga_text, 0, sizeof vga_text);
    for (unsigned i = 0; i < sizeof vga_text; i += 2) {
        vga_text[i]     = ' ';
        vga_text[i + 1] = 0x07;
    }

    // The ladder is on GP30-37, which a state machine in the default
    // window cannot reach past GP31. Moving PIO1's window to 16 puts all
    // eight inside it.
    pio_set_gpio_base(PIO_VGA, 16);
    const uint offset = pio_add_program(PIO_VGA, &vga_pio_program);
    sm = pio_claim_unused_sm(PIO_VGA, true);

    for (int i = 0; i < VGA_PINS; i++) {
        gpio_init(VGA_BASE_PIN + i);
        gpio_set_dir(VGA_BASE_PIN + i, GPIO_OUT);
        pio_gpio_init(PIO_VGA, VGA_BASE_PIN + i);
    }
    pio_sm_set_consecutive_pindirs(PIO_VGA, sm, VGA_BASE_PIN, VGA_PINS, true);

    pio_sm_config cfg = pio_get_default_sm_config();
    sm_config_set_out_pin_base(&cfg, VGA_BASE_PIN);
    sm_config_set_wrap(&cfg, offset, offset + vga_pio_program.length - 1);
    sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_TX);
    sm_config_set_out_shift(&cfg, true, true, 32);
    sm_config_set_out_pins(&cfg, VGA_BASE_PIN, VGA_PINS);

    pio_sm_init(PIO_VGA, sm, offset, &cfg);
    pio_sm_set_enabled(PIO_VGA, sm, true);
    pio_sm_set_clkdiv(PIO_VGA, sm, (float)clock_get_hz(clk_sys) / PIXEL_CLOCK);

    dma_ctrl_channel = dma_claim_unused_channel(true);
    dma_data_channel = dma_claim_unused_channel(true);
    configure_dma();
    build_sync_templates();

    irq_set_exclusive_handler(VGA_DMA_IRQ, vga_scanline);
    dma_channel_set_irq0_enabled(dma_ctrl_channel, true);
    irq_set_enabled(VGA_DMA_IRQ, true);

    dma_start_channel_mask(1u << dma_data_channel);
}
