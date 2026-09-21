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
#include "settings.h"
#include "spectrum.h"

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
/*
 * pico-speccy's UI palette, from src/ui/UiGfx.cpp (kUiPaletteVga).
 *
 * Their own comment explains why these particular values: an on-grid
 * RGB888 comes out of a four-levels-per-channel VGA path solid rather
 * than dithered, and the dark end of a slate theme cannot survive being
 * rounded to that grid -- so the darks were re-spread by hand over
 * black, navy and blue while the text, selection and icon hues stay on
 * the nearest grid point.
 *
 * Four levels per channel is exactly what R48-R57 give this board, so
 * the work of choosing them is already done and this is a transcription.
 */
static const uint8_t palette16[16] = {
    PX(0, 0, 0),  // 0  C_BG        000000
    PX(0, 0, 1),  // 1  C_PANEL     000055
    PX(0, 0, 2),  // 2  C_PANEL_ALT 0000AA
    PX(1, 1, 1),  // 3  C_SEP       555555
    PX(3, 3, 3),  // 4  C_TEXT      FFFFFF
    PX(2, 2, 2),  // 5  C_TEXT_DIM  AAAAAA
    PX(3, 3, 3),  // 6  C_WHITE     FFFFFF
    PX(1, 1, 3),  // 7  C_SEL_BG    5555FF
    PX(0, 0, 2),  // 8  C_SEL_BAND  0000AA
    PX(1, 3, 2),  // 9  C_ACCENT    55FFAA
    PX(0, 0, 0),  // 10 C_FOOT_BG   000000
    PX(0, 0, 0),  // 11 C_SHADOW    000000
    PX(3, 1, 1),  // 12 C_ICON_R    FF5555
    PX(3, 2, 1),  // 13 C_ICON_Y    FFAA55
    PX(1, 2, 3),  // 14 C_ICON_C    55AAFF
    PX(1, 1, 1),  // 15 C_DISABLED  555555
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

static volatile vga_mode_t vga_mode = VGA_MODE_TEXT;

void vga_set_mode(const vga_mode_t mode) { vga_mode = mode; }

// ============================================================================
// The Spectrum's picture inside a 640x480 one
// ============================================================================
//
// 256x192 doubled to 512x384, centred, and the 64 columns and 48 lines
// left over on each side are the border — which is what the border is on
// a real television, so the proportions come out close to right rather
// than being a frame drawn around a small picture.
#define SPEC_X0     64          // first visible picture byte in the line
#define SPEC_Y0     48          // first picture scanline
#define SPEC_H      384         // 192 doubled

/*
 * Sixteen colours, two bits a channel.
 *
 * Bright is full scale and normal is two thirds, which is the same
 * decision the text palette makes and for the same reason: on a
 * two-resistor ladder the one-third step is too dark to read. Black has
 * no bright variant on a Spectrum and none here.
 *
 * The attribute's colour bits are GRB, not RGB — bit 0 is blue, bit 1 is
 * red, bit 2 is green. Getting that backwards swaps red and blue on
 * every screen ever written, which looks deliberate enough to miss.
 */
static uint8_t spec_palette[16];

/*
 * The same fifteen colours, one step darker.
 *
 * Scanlines on a doubled picture cost nothing if the darker version is
 * precomputed: each Spectrum pixel row already occupies two output
 * lines, so the effect is a choice of palette on the second of them
 * rather than any per-pixel work. With two bits per channel there is
 * exactly one step to give, which is what makes it a toggle and not a
 * strength.
 */
static uint8_t spec_palette_dim[16];

static void build_spec_palette(void) {
    for (int i = 0; i < 16; i++) {
        const int lvl = (i & 8) ? 3 : 2;
        const int c   = i & 7;
        spec_palette[i] = PX((c & 2) ? lvl : 0,      // red
                             (c & 4) ? lvl : 0,      // green
                             (c & 1) ? lvl : 0);     // blue

        const int dim = lvl - 1;
        spec_palette_dim[i] = PX((c & 2) ? dim : 0,
                                 (c & 4) ? dim : 0,
                                 (c & 1) ? dim : 0);
    }
}

/*
 * Nibble to four doubled pixels.
 *
 * dmask2[n][0] covers bits 3 and 2 of the nibble, each becoming two
 * output bytes; dmask2[n][1] covers bits 1 and 0. Bit 3 is leftmost,
 * because in Spectrum screen memory bit 7 of a byte is the leftmost
 * pixel — the opposite convention to this firmware's font, which is a
 * good way to get one of them wrong.
 */
static uint32_t dmask2[16][2];

static void build_dmask2(void) {
    for (int n = 0; n < 16; n++) {
        // word 0: bit3 bit3 bit2 bit2   word 1: bit1 bit1 bit0 bit0
        const uint32_t b3 = (n & 8) ? 0xFFFFu : 0, b2 = (n & 4) ? 0xFFFF0000u : 0;
        const uint32_t b1 = (n & 2) ? 0xFFFFu : 0, b0 = (n & 1) ? 0xFFFF0000u : 0;
        dmask2[n][0] = b3 | b2;
        dmask2[n][1] = b1 | b0;
    }
}

/*
 * One Spectrum scanline, straight out of the guest's RAM.
 *
 * The screen address is the famous one: the third of the screen, the
 * character row within it and the pixel row within the character are
 * three separate fields of the address and they are not in that order.
 * Bits 6-5 of y pick the third, bits 2-0 pick the line within a
 * character cell, and bits 5-3 pick the row of cells.
 */
__force_inline static void render_spectrum_line(uint32_t *__restrict out, const uint32_t line) {
    /*
     * Which of the two palettes this output line uses.
     *
     * The picture is doubled, so every odd line of a pair is the one to
     * darken -- including in the border, or the effect stops at the edge
     * of the paper and looks like a fault rather than a filter.
     */
    const uint8_t *__restrict pal =
        (settings.scanlines && (line & 1)) ? spec_palette_dim : spec_palette;

    const uint8_t  border_px = pal[spec_border & 7];
    const uint32_t border32  = (uint32_t)border_px * 0x01010101u;

    if (line < SPEC_Y0 || line >= SPEC_Y0 + SPEC_H) {
        for (int i = 0; i < VISIBLE_BYTES / 4; i++) out[i] = border32;
        return;
    }

    const uint32_t y = (line - SPEC_Y0) >> 1;           // 0..191

    const uint8_t *__restrict scr = spec_ram[spec_screen_bank];
    const uint8_t *__restrict bmp = scr + (((y & 0xC0) << 5) | ((y & 0x38) << 2) | ((y & 0x07) << 8));
    const uint8_t *__restrict att = scr + 0x1800 + ((y >> 3) << 5);

    const bool flash = spec_flash;

    for (int i = 0; i < SPEC_X0 / 4; i++) *out++ = border32;

    for (int col = 0; col < 32; col++) {
        const uint8_t bits = bmp[col];
        const uint8_t a    = att[col];

        const uint8_t bright = (a & 0x40) >> 3;         // -> palette bit 3
        uint32_t ink   = (uint32_t)pal[(a & 0x07) | bright] * 0x01010101u;
        uint32_t paper = (uint32_t)pal[((a >> 3) & 0x07) | bright] * 0x01010101u;

        // FLASH swaps the pair, every sixteen frames, for this cell only.
        if (unlikely(flash && (a & 0x80))) { const uint32_t t = ink; ink = paper; paper = t; }

        const uint32_t *hi = dmask2[bits >> 4];
        const uint32_t *lo = dmask2[bits & 15];

        *out++ = (ink & hi[0]) | (paper & ~hi[0]);
        *out++ = (ink & hi[1]) | (paper & ~hi[1]);
        *out++ = (ink & lo[0]) | (paper & ~lo[0]);
        *out++ = (ink & lo[1]) | (paper & ~lo[1]);
    }

    for (int i = 0; i < SPEC_X0 / 4; i++) *out++ = border32;
}

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

    if (vga_mode == VGA_MODE_SPECTRUM) {
        render_spectrum_line(out, line);
        return;
    }

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
    build_dmask2();
    build_spec_palette();

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
