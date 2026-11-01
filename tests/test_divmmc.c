/*
 * DivMMC: the automap, the paging, and the card behind it.
 *
 * The automap is the part with teeth. A fetch at an entry point maps
 * esxDOS in AFTER the fetch completes, so the instruction at the entry
 * point still comes from the Spectrum's ROM; a fetch in #1FF8-#1FFF
 * maps it out the same way; a fetch in #3D00-#3DFF maps it in
 * immediately, before the fetch. Getting the off-by-one-fetch wrong
 * gives a machine that boots and then wanders, so it is asserted here
 * fetch by fetch.
 *
 * The card is driven with the real SPI command sequence an esxDOS build
 * issues at start-up -- CMD0, CMD8, ACMD41, CMD58 -- and then read from,
 * against an image whose every block is identifiable.
 */
#include <stdio.h>
#include <string.h>

#include "divmmc.h"
#include "spectrum.h"
#include "settings.h"
#include "tap.h"

#define CTRL 0xE3
#define CS   0xE7
#define DATA 0xEB

static void out(const uint16_t p, const uint8_t v) { divmmc_port_out(p, v); }
static uint8_t in(const uint16_t p) { uint8_t v = 0x00; divmmc_port_in(p, &v); return v; }

/* Clock a command frame out and wait for the card to answer. */
static uint8_t card_cmd(const uint8_t cmd, const uint32_t arg, const uint8_t crc) {
    out(DATA, (uint8_t)(0x40 | cmd));
    out(DATA, (uint8_t)(arg >> 24)); out(DATA, (uint8_t)(arg >> 16));
    out(DATA, (uint8_t)(arg >> 8));  out(DATA, (uint8_t)arg);
    out(DATA, crc);
    for (int i = 0; i < 16; i++) {           /* Ncr: up to 8 bytes, allow more */
        const uint8_t r = in(DATA);
        if (!(r & 0x80)) return r;
    }
    return 0xFF;
}

static void select(const bool on) { out(CS, on ? 0x00 : 0x01); }

int main(void) {
    char d[128];

    settings.esxdos = true;
    spec_machine    = MACHINE_128K;

    /* ---------------------------------------------------------------- */
    GROUP("ports");
    settings.esxdos = false;
    uint8_t v = 0;
    CHECK(!divmmc_port_out(CTRL, 0x80), "silent when esxDOS is off");
    CHECK(!divmmc_port_in(DATA, &v),    "and answers nothing");
    settings.esxdos = true;
    CHECK(divmmc_port_out(CTRL, 0x00), "#E3 is ours when it is on");
    CHECK(divmmc_port_in(DATA, &v),    "#EB is ours");
    CHECK(!divmmc_port_out(0xE4, 0),   "#E4 is not");
    /* Decoded on the low byte, like every divIDE-family interface. */
    CHECK(divmmc_port_out(0x12E3, 0x00), "#E3 decodes on the low byte");

    /* ---------------------------------------------------------------- */
    GROUP("automap");
    divmmc_reset();
    CHECK(!divmmc_mapped(), "reset leaves it paged out");

    /* An entry point maps in AFTER its own fetch. */
    static const uint16_t ENTRY[] = { 0x0000, 0x0008, 0x0038, 0x0066, 0x04C6, 0x0562 };
    for (unsigned i = 0; i < count_of(ENTRY); i++) {
        divmmc_reset();
        divmmc_opfetch(ENTRY[i]);
        CHECK(!divmmc_mapped(), "entry %04X: not yet mapped during its own fetch", ENTRY[i]);
        divmmc_opfetch(0x8000);
        CHECK(divmmc_mapped(), "entry %04X: mapped by the next fetch", ENTRY[i]);
    }
    /* An address that is not an entry point must do nothing. */
    divmmc_reset();
    divmmc_opfetch(0x0010); divmmc_opfetch(0x8000);
    CHECK(!divmmc_mapped(), "an ordinary address does not map it in");

    /* #1FF8-#1FFF maps out, also one fetch later. */
    divmmc_reset();
    divmmc_opfetch(0x0000); divmmc_opfetch(0x8000);
    CHECK(divmmc_mapped(), "mapped in");
    divmmc_opfetch(0x1FF8);
    CHECK(divmmc_mapped(), "still mapped during the #1FF8 fetch itself");
    divmmc_opfetch(0x8000);
    CHECK(!divmmc_mapped(), "#1FF8 mapped it out");
    divmmc_reset(); divmmc_opfetch(0x0000); divmmc_opfetch(0x8000);
    divmmc_opfetch(0x1FFF); divmmc_opfetch(0x8000);
    CHECK(!divmmc_mapped(), "#1FFF maps out too");
    divmmc_reset(); divmmc_opfetch(0x0000); divmmc_opfetch(0x8000);
    divmmc_opfetch(0x1FF7); divmmc_opfetch(0x8000);
    CHECK(divmmc_mapped(), "#1FF7 is below the window and does not");

    /* #3D00-#3DFF maps in IMMEDIATELY -- before the fetch, not after. */
    divmmc_reset();
    divmmc_opfetch(0x3D00);
    CHECK(divmmc_mapped(), "#3D00 maps in at once");
    divmmc_reset();
    divmmc_opfetch(0x3DFF);
    CHECK(divmmc_mapped(), "#3DFF maps in at once");
    divmmc_reset();
    divmmc_opfetch(0x3E00);
    CHECK(!divmmc_mapped(), "#3E00 is past the window");

    /* ---------------------------------------------------------------- */
    GROUP("paging");
    divmmc_reset();
    /* CONMEM forces it in regardless of the automap. */
    out(CTRL, 0x80);
    CHECK(divmmc_mapped(), "CONMEM pages it in");
    CHECK(!divmmc_lo_writable(), "the low page is the EEPROM, not RAM");
    out(CTRL, 0x00);
    CHECK(!divmmc_mapped(), "clearing CONMEM pages it out");

    /*
     * The bank select picks which 8 KB shows at #2000.
     *
     * Only the four banks esxDOS runs in are resident; the twelve above
     * them are the RAMdisk and live in PSRAM behind a single staging
     * page, so two of them are the same pointer and only their CONTENTS
     * differ. The thing that can actually break is the flush on the way
     * out -- write a bank, page to another, come back, and the write
     * has to still be there.
     */
    for (unsigned b = 0; b < DIVMMC_BANKS; b++) {
        out(CTRL, (uint8_t)(0x80 | b));
        uint8_t *p = divmmc_page_hi();
        CHECK(p != NULL, "bank %u has a page", b);
        memset(p, (int)(0x10 + b), DIVMMC_BANK_SIZE);
    }
    for (unsigned b = 0; b < DIVMMC_BANKS; b++) {
        out(CTRL, (uint8_t)(0x80 | b));
        const uint8_t *p = divmmc_page_hi();
        const uint8_t want = (uint8_t)(0x10 + b);
        EQ(p[0], want, "bank %u first byte survived the switch", b);
        EQ(p[DIVMMC_BANK_SIZE - 1], want, "bank %u last byte survived", b);
    }
    /* Interleaved: the case that catches a missing flush. */
    out(CTRL, 0x80 | 5);  memset(divmmc_page_hi(), 0xA1, DIVMMC_BANK_SIZE);
    out(CTRL, 0x80 | 9);  memset(divmmc_page_hi(), 0xB2, DIVMMC_BANK_SIZE);
    out(CTRL, 0x80 | 5);  EQ(divmmc_page_hi()[0], 0xA1, "bank 5 after a detour");
    out(CTRL, 0x80 | 9);  EQ(divmmc_page_hi()[0], 0xB2, "bank 9 after a detour");
    /* The bank number is four bits; anything above wraps into them. */
    out(CTRL, 0x80 | 5);
    const uint8_t *b5 = divmmc_page_hi();
    out(CTRL, (uint8_t)(0x80 | 0x15));
    EQ(divmmc_page_hi(), (long long)(size_t)b5, "bank select is four bits");

    /* MAPRAM is sticky: only a reset clears it. */
    out(CTRL, 0xC0);
    CHECK(divmmc_lo_writable(), "MAPRAM puts RAM under #0000");
    out(CTRL, 0x80);
    CHECK(divmmc_lo_writable(), "MAPRAM survives a write that does not set it");
    divmmc_reset();
    out(CTRL, 0x80);
    CHECK(!divmmc_lo_writable(), "and only a reset clears it");

    /* ---------------------------------------------------------------- */
    GROUP("card");
    CHECK(divmmc_mount("tests/fixtures/card.mmc", d, sizeof d), "mount: %s", d);
    CHECK(divmmc_mounted(), "mounted");

    select(true);
    EQ(card_cmd(0, 0, 0x95), 0x01, "CMD0 GO_IDLE_STATE -> idle");
    /* CMD8 answers R7: the R1 byte then four more. */
    EQ(card_cmd(8, 0x1AA, 0x87), 0x01, "CMD8 SEND_IF_COND -> idle");
    in(DATA); in(DATA);
    EQ(in(DATA), 0x01, "CMD8 echoes the voltage range");
    EQ(in(DATA), 0xAA, "CMD8 echoes the check pattern");
    /* CRC_ON_OFF before ACMD41 says "idle", which EVO Reset Service
     * waits on. APP_CMD says ready whatever: esxDOS 0.8.9 hangs on an
     * idle one, and loops on ACMD41's response anyway. */
    EQ(card_cmd(59, 0, 0x91), 0x01, "CMD59 CRC_ON_OFF -> idle");
    EQ(card_cmd(55, 0, 0x65), 0x00, "CMD55 APP_CMD accepted");
    EQ(card_cmd(41, 0x40000000, 0x77), 0x00, "ACMD41 -> ready");
    EQ(card_cmd(59, 0, 0x91), 0x00, "and CMD59 after it -> ready");
    EQ(card_cmd(58, 0, 0xFD), 0x00, "CMD58 READ_OCR");
    const uint8_t ocr0 = in(DATA);
    in(DATA); in(DATA); in(DATA);
    /*
     * A small image is a standard-capacity card, so the OCR must say so
     * and CMD17 must then take a BYTE address. The two have to agree:
     * a card claiming high capacity while addressing by byte reads a
     * 512th of the way into the image on every access.
     */
    EQ(ocr0, 0x80, "OCR says standard capacity for a small image");
    const uint32_t UNIT = 512;              /* byte-addressed */

    /* Read a few blocks and check we got the ones we asked for. */
    for (uint32_t blk = 0; blk < 64; blk += 21) {
        EQ(card_cmd(17, blk * UNIT, 0xFF), 0x00, "CMD17 block %u accepted", blk);
        uint8_t tok = 0xFF;
        for (int i = 0; i < 64 && tok != 0xFE; i++) tok = in(DATA);
        EQ(tok, 0xFE, "block %u start token", blk);
        uint8_t buf[512];
        for (int i = 0; i < 512; i++) buf[i] = in(DATA);
        in(DATA); in(DATA);                    /* CRC16, discarded */
        EQ(buf[0], 0xAA, "block %u marker", blk);
        EQ(buf[511], blk & 0xFF, "block %u is the block we asked for", blk);
        unsigned bad = 0;
        const uint8_t fill = (uint8_t)((blk * 3 + 1) & 0xFF);
        for (int i = 1; i < 511; i++) if (buf[i] != fill) bad++;
        EQ(bad, 0, "block %u body", blk);
    }

    /* ---------------------------------------------------------------- */
    GROUP("card write");
    uint8_t pat[512];
    for (int i = 0; i < 512; i++) pat[i] = (uint8_t)(0xE7 ^ (i * 5));
    EQ(card_cmd(24, 9 * UNIT, 0xFF), 0x00, "CMD24 WRITE_BLOCK accepted");
    out(DATA, 0xFE);                         /* the data token */
    for (int i = 0; i < 512; i++) out(DATA, pat[i]);
    out(DATA, 0x00); out(DATA, 0x00);        /* CRC16 */
    uint8_t dr = 0xFF;
    for (int i = 0; i < 16 && (dr & 0x11) != 0x01; i++) dr = in(DATA);
    EQ(dr & 0x1F, 0x05, "the card accepted the data");

    /* Read it back through the card, then straight off the file. */
    EQ(card_cmd(17, 9 * UNIT, 0xFF), 0x00, "CMD17 for the block just written");
    uint8_t tok = 0xFF;
    for (int i = 0; i < 64 && tok != 0xFE; i++) tok = in(DATA);
    EQ(tok, 0xFE, "start token");
    uint8_t back[512];
    for (int i = 0; i < 512; i++) back[i] = in(DATA);
    EQ(memcmp(back, pat, 512), 0, "the write is visible to a read");

    divmmc_eject();
    FILE *cf = fopen("tests/fixtures/card.mmc", "rb");
    CHECK(cf != NULL, "reopen the image");
    if (cf) {
        uint8_t onfile[512];
        fseek(cf, 9 * 512, SEEK_SET);
        CHECK(fread(onfile, 1, 512, cf) == 512, "read block 9 off the file");
        fclose(cf);
        EQ(memcmp(onfile, pat, 512), 0, "the write reached the card image");
    }
    CHECK(!divmmc_mounted(), "eject");

    return tap_done("divmmc");
}
