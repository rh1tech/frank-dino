/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "divmmc.h"

#include <stdio.h>
#include <string.h>

#include <pico.h>

#include "dino.h"

#include "ff.h"
#include "settings.h"
#include "spectrum.h"

/* ── memory ──────────────────────────────────────────────────────────── */
/*
 * The ROM, and the banks.
 *
 * The interface's pages go into the bus engine's page table, so the CPU
 * fetches opcodes straight out of them and they have to be in SRAM. A
 * hundred and twenty-eight kilobytes of it is more than this chip has to
 * spare, so the same arrangement as the Scorpion's high banks applies:
 * the four esxDOS actually runs in are resident and the rest -- the
 * RAMdisk -- live in the PSRAM behind one staging page.
 *
 * Banks 0-3 are the resident ones because that is where esxDOS keeps
 * itself: bank 3 is the workspace MAPRAM exposes at #0000 and 0-2 hold
 * its code. The higher banks are storage, switched rarely, and a switch
 * to one costs two 8 KB transfers.
 */
#define DIVMMC_RESIDENT 4

static uint8_t esxdos_rom[DIVMMC_ROM_SIZE] __attribute__((aligned(4)));
static uint8_t bank_lo[DIVMMC_RESIDENT][DIVMMC_BANK_SIZE] __attribute__((aligned(4)));
static uint8_t bank_hi[DIVMMC_BANKS - DIVMMC_RESIDENT][DIVMMC_BANK_SIZE]
    __attribute__((section(".psram")));
static uint8_t bank_stage[DIVMMC_BANK_SIZE] __attribute__((aligned(4)));
static int     staged = -1;

extern const uint8_t spec_romesx[DIVMMC_ROM_SIZE];

static uint8_t *bank_ptr(const unsigned b) {
    if (b < DIVMMC_RESIDENT) return bank_lo[b];
    if (b >= DIVMMC_BANKS)   return bank_lo[0];
    if (staged == (int)b)    return bank_stage;
    if (staged >= DIVMMC_RESIDENT)
        memcpy(bank_hi[staged - DIVMMC_RESIDENT], bank_stage, DIVMMC_BANK_SIZE);
    memcpy(bank_stage, bank_hi[b - DIVMMC_RESIDENT], DIVMMC_BANK_SIZE);
    staged = (int)b;
    return bank_stage;
}

/* ── state ───────────────────────────────────────────────────────────── */

static bool    automap;         /* the CPU walked into an entry point */
static bool    conmem;          /* #E3 bit 7: mapped whatever the CPU does */
static bool    mapram;          /* #E3 bit 6: bank 3 at #0000 instead of the ROM */
static uint8_t bank_sel;        /* #E3 bits 0-3 */
static bool    map_after, unmap_after;

bool divmmc_mapped(void) { return settings.esxdos && (conmem || automap); }
bool divmmc_lo_writable(void) { return mapram; }

uint8_t *divmmc_page_lo(void) {
    return mapram ? bank_ptr(3) : esxdos_rom;
}
uint8_t *divmmc_page_hi(void) {
    return bank_ptr(bank_sel & (DIVMMC_BANKS - 1));
}

/* Defined with the SPI state machine, below. */
static void card_reset(void);

void divmmc_reset(void) {
    automap = conmem = mapram = false;
    card_reset();
    bank_sel = 0;
    map_after = unmap_after = false;
    memcpy(esxdos_rom, spec_romesx, DIVMMC_ROM_SIZE);
}

/* ── the automap ─────────────────────────────────────────────────────── */
/*
 * Their preOpcFetch/postOpcFetch pair, folded into the one hook this bus
 * engine has.
 *
 * Theirs maps immediately after the opcode byte is read; this applies it
 * at the START of the next opcode fetch instead. The difference is only
 * visible to an instruction AT an entry point that reads its own
 * operands -- and every entry point is an RST or an interrupt vector,
 * where the instruction is one byte and there are none. The operands of
 * anything else are fetched before the next opcode, so they see the same
 * map either way.
 *
 * The early return is theirs too: every address that can arm anything is
 * below #4000, so a fetch from RAM costs one compare rather than six.
 */
void __not_in_flash_func(divmmc_opfetch)(const uint16_t addr) {
    if (map_after)   { map_after = false;   automap = true;  apply_paging_hook(); }
    if (unmap_after) { unmap_after = false; automap = false; apply_paging_hook(); }

    if (likely(addr >= 0x4000)) return;

    if (!automap) {
        if (addr == 0x0000 || addr == 0x0008 || addr == 0x0038
            || addr == 0x0066 || addr == 0x04C6 || addr == 0x0562)
            map_after = true;
    }

    /* #3D00-#3DFF maps at once, before the fetch -- the Beta Disk's rule. */
    if (addr >= 0x3D00 && addr <= 0x3DFF) {
        if (!automap) { automap = true; apply_paging_hook(); }
    }

    if (automap && addr >= 0x1FF8 && addr <= 0x1FFF) unmap_after = true;
}

/* ── the card ────────────────────────────────────────────────────────── */
/*
 * A .mmc or .hdf image, read a block at a time.
 *
 * Nothing is buffered beyond the one 512-byte sector in flight: a card
 * image is hundreds of megabytes and the guest only ever asks for a
 * sector of it. The read happens inside the guest's OUT, which stalls
 * the Z80 for as long as the microSD takes -- which is also true of the
 * real interface, and is why esxDOS polls for the data token instead of
 * assuming the card is ready.
 *
 * The card is the same microSD FatFs uses. That is safe because the two
 * never overlap: core 0 only touches the card with the machine paused,
 * and this only runs with the machine going.
 */
static FIL      card;
static bool     card_open;
static bool     sdhc;           /* sector-addressed rather than byte-addressed */
static uint32_t n_rd, n_wr;
static char     card_path[64];

static uint8_t  sector[512];

bool divmmc_mounted(void) { return card_open; }

void divmmc_eject(void) {
    if (card_open) f_close(&card);
    card_open = false;
    card_path[0] = 0;
}

bool divmmc_mount(const char *path, char *detail, const unsigned detail_len) {
    divmmc_eject();
    if (!path) { snprintf(detail, detail_len, "card removed"); return true; }

    if (f_open(&card, path, FA_READ | FA_WRITE) != FR_OK) {
        /* Read-only is better than nothing: a card image on a write-protected
         * volume should still boot. */
        if (f_open(&card, path, FA_READ) != FR_OK) {
            snprintf(detail, detail_len, "cannot open that image");
            return false;
        }
    }
    card_open = true;
    /*
     * SDHC is sector-addressed and everything before it byte-addressed,
     * and esxDOS asks which through CMD8/ACMD41. Images above 2 GB can
     * only be SDHC; below that the byte-addressed answer is the one the
     * older ROMs expect.
     */
    sdhc = (f_size(&card) > (2u << 30));
    snprintf(card_path, sizeof card_path, "%s", path);
    n_rd = n_wr = 0;
    snprintf(detail, detail_len, "%lu MB, %s",
             (unsigned long)(f_size(&card) >> 20), sdhc ? "SDHC" : "SD");
    return true;
}

void divmmc_stats(char *out, const unsigned len) {
    if (!card_open) { snprintf(out, len, "no image"); return; }
    snprintf(out, len, "%lu rd %lu wr", (unsigned long)n_rd, (unsigned long)n_wr);
}

/* The byte offset of an address, whichever way the card is addressed. */
static uint64_t byte_at(const uint32_t addr) {
    return sdhc ? (uint64_t)addr * 512u : (uint64_t)addr;
}

static bool sector_load(const uint32_t addr) {
    if (!card_open) return false;
    UINT got = 0;
    if (f_lseek(&card, (FSIZE_t)byte_at(addr)) != FR_OK) return false;
    if (f_read(&card, sector, 512, &got) != FR_OK) return false;
    if (got < 512) memset(sector + got, 0xFF, 512 - got);   /* past the end */
    n_rd++;
    return true;
}

static bool sector_store(const uint32_t addr) {
    if (!card_open) return false;
    UINT put = 0;
    if (f_lseek(&card, (FSIZE_t)byte_at(addr)) != FR_OK) return false;
    if (f_write(&card, sector, 512, &put) != FR_OK || put != 512) return false;
    f_sync(&card);
    n_wr++;
    return true;
}

/* ── the SPI card, as a state machine ────────────────────────────────── */
/*
 * Their mmc_write/mmc_read pair. The guest bit-bangs SD commands through
 * #EB one byte at a time and reads the answers back the same way, so
 * this is a six-byte command frame, a response, and then whatever the
 * command produces.
 *
 * What is here is the set esxDOS uses: GO_IDLE, SEND_OP_COND, SEND_IF_COND,
 * SEND_CSD, SEND_CID, STOP_TRANSMISSION, SET_BLOCKLEN, READ_SINGLE and
 * READ_MULTIPLE, WRITE_SINGLE and WRITE_MULTIPLE, APP_CMD, SEND_OP_COND
 * (ACMD41) and READ_OCR.
 */
static bool     cs_active;
static uint8_t  cmd;
/*
 * The next byte the card owes the host, or -1.
 *
 * Every command answers with an R1 status BEFORE whatever else it
 * produces, and a written block answers 0x05 after its CRC. Without a
 * slot of its own that byte is overtaken by the data it is supposed to
 * precede -- the host reads the data token where it expected the
 * status, and every read after that is one byte out.
 */
static int      resp = -1;
static int      cmd_idx;
static uint8_t  params[6];
static int      rd_idx = -1;    /* -1 idle, 0 the token, 1..512 data, 513-514 CRC */
static int      wr_idx = -1;
static uint32_t rw_addr;
static bool     multi_read, multi_write;
static int      reg_idx = -1;   /* CSD/CID/OCR playback */
static uint8_t  reg_buf[20];
static int      reg_len;

static void begin_reg(const uint8_t *p, const int n) {
    memcpy(reg_buf, p, (size_t)n);
    reg_len = n;
    reg_idx = 0;
}

static void card_reset(void) {
    cs_active = false;
    cmd_idx = 0;
    rd_idx = wr_idx = reg_idx = -1;
    resp = -1;
    multi_read = multi_write = false;
}

void divmmc_card_cs(const uint8_t v) {
    /* Bit 0 low selects the card, as on the real port. */
    cs_active = (v & 1u) == 0u;
    if (!cs_active) { cmd_idx = 0; rd_idx = wr_idx = -1; reg_idx = -1; resp = -1; }
}

static void card_write(const uint8_t v) {
    if (!cs_active) return;

    /* A write block's data phase: a token, 512 bytes, two CRC bytes. */
    if (wr_idx >= 0) {
        if (wr_idx == 0) {
            if (v == 0xFE || v == 0xFC) { wr_idx = 1; return; }
            if (v == 0xFD) { wr_idx = -1; multi_write = false; return; }  /* stop */
            return;                                                      /* gap */
        }
        if (wr_idx <= 512) { sector[wr_idx - 1] = v; wr_idx++; return; }
        if (++wr_idx >= 515) {
            sector_store(rw_addr);
            rw_addr += sdhc ? 1u : 512u;
            wr_idx = multi_write ? 0 : -1;
            resp = 0x05;                    /* data accepted */
        }
        return;
    }

    if (cmd_idx == 0) {
        /* A command frame has bits 7:6 = 01. Anything else -- and the
         * guest sends a great many 0xFF -- is a fill byte the card
         * silently discards. */
        if ((v & 0xC0u) != 0x40u) return;
        cmd = v;
        cmd_idx = 1;
        return;
    }

    if (cmd_idx <= 5) params[cmd_idx - 1] = v;
    if (++cmd_idx < 6) return;
    cmd_idx = 0;

    const uint32_t arg = ((uint32_t)params[0] << 24) | ((uint32_t)params[1] << 16)
                       | ((uint32_t)params[2] << 8)  | params[3];

    switch (cmd & 0x3Fu) {
        case 0:  resp = 1; break;                           /* GO_IDLE_STATE */
        case 1:  resp = 0; break;                           /* SEND_OP_COND */
        case 8:  resp = 1;                                  /* SEND_IF_COND */
                 begin_reg((const uint8_t[]){ 0, 0, 1, params[3] }, 4);
                 break;
        case 9: {                                           /* SEND_CSD */
            /*
             * A CSD version 1 saying "this card is as big as the image".
             * esxDOS reads it to size the card; the fields are the
             * ones that matter and the rest are a plausible constant.
             */
            static uint8_t csd[19];
            csd[0] = 0xFE;                                  /* the data token */
            memset(csd + 1, 0, 16);
            csd[1] = 0x00;                                  /* CSD_STRUCTURE 1.0 */
            csd[6] = 0x09;                                  /* READ_BL_LEN = 512 */
            const uint32_t blocks = card_open
                ? (uint32_t)(f_size(&card) / 512u) : 0u;
            const uint32_t csize = (blocks / 1024u) ? (blocks / 1024u) - 1u : 0u;
            csd[7]  = (uint8_t)((csize >> 10) & 0x03u);
            csd[8]  = (uint8_t)((csize >> 2) & 0xFFu);
            csd[9]  = (uint8_t)(((csize & 3u) << 6) | 0x01u);
            csd[10] = 0x7F; csd[11] = 0x80;                 /* mult = 7 -> x512 */
            begin_reg(csd, 19);
            resp = 0;
            break;
        }
        case 10: {                                          /* SEND_CID */
            static const uint8_t cid[19] = {
                0xFE, 'F', 'D', 'I', 'N', 'O', ' ', '1', 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0
            };
            begin_reg(cid, 19);
            resp = 0;
            break;
        }
        case 12: resp = 1; multi_read = false; rd_idx = -1; break; /* STOP_TRANSMISSION */
        case 16: resp = 0; break;                           /* SET_BLOCKLEN */
        case 17:                                            /* READ_SINGLE_BLOCK */
        case 18:                                            /* READ_MULTIPLE_BLOCK */
            resp = 0;
            rw_addr = arg;
            multi_read = ((cmd & 0x3Fu) == 18u);
            sector_load(rw_addr);
            rd_idx = 0;
            break;
        case 24:                                            /* WRITE_BLOCK */
        case 25:                                            /* WRITE_MULTIPLE_BLOCK */
            resp = 0;
            rw_addr = arg;
            multi_write = ((cmd & 0x3Fu) == 25u);
            wr_idx = 0;
            break;
        case 55: resp = 0; break;                           /* APP_CMD */
        case 41: resp = 0; break;                           /* ACMD41: ready */
        case 58:                                            /* READ_OCR */
            resp = 0;
            begin_reg((const uint8_t[]){ sdhc ? 0xC0u : 0x80u, 0xFF, 0x80, 0x00 }, 4);
            break;
        default: resp = 4; break;                           /* illegal command */
    }
}

static uint8_t card_read(void) {
    if (!cs_active) return 0xFF;

    /* The status first, always: it is what the host is waiting for. */
    if (resp >= 0) { const uint8_t v = (uint8_t)resp; resp = -1; return v; }

    /* A register or a block being played back, token first. */
    if (reg_idx >= 0) {
        const uint8_t v = reg_buf[reg_idx];
        if (++reg_idx >= reg_len) reg_idx = -1;
        return v;
    }

    if (rd_idx >= 0) {
        if (rd_idx == 0) { rd_idx = 1; return 0xFE; }        /* the data token */
        if (rd_idx <= 512) return sector[rd_idx++ - 1];
        if (++rd_idx <= 515) return 0xFF;                    /* two CRC bytes */
        if (multi_read) {
            rw_addr += sdhc ? 1u : 512u;
            sector_load(rw_addr);
            rd_idx = 0;
            return 0xFF;
        }
        rd_idx = -1;
        return 0xFF;
    }

    /*
     * Nothing owed. A real card holds the line high between transfers
     * and the host's polling loops are written expecting that.
     */
    return 0xFF;
}

/* ── the ports ───────────────────────────────────────────────────────── */
/*
 * #E3 control, #E7 card select, #EB data -- decoded on the low byte, as
 * every divIDE-family interface is.
 */
bool __not_in_flash_func(divmmc_port_out)(const uint16_t port, const uint8_t value) {
    if (!settings.esxdos) return false;

    switch (port & 0xFFu) {
        case 0xE3:
            conmem = (value & 0x80u) != 0;
            /*
             * MAPRAM is sticky: once the ROM has set it, only a reset
             * clears it. That is what stops a program from putting the
             * EEPROM back under esxDOS's feet.
             */
            if (value & 0x40u) mapram = true;
            bank_sel = (uint8_t)(value & 0x0Fu);
            apply_paging_hook();
            return true;
        case 0xE7: divmmc_card_cs(value); return true;
        case 0xEB: card_write(value); return true;
        default:   return false;
    }
}

/*
 * The Z-Controller: the same SD card behind Pentagon/Evo-style ports, #77
 * the configuration (bit 1 low selects the card) and #57 the SPI data.
 *
 * pico-speccy's Z-Controller drives the host's own card raw; this one is
 * given the card image instead -- the same one esxDOS sees -- because the
 * host card is this firmware's own filing system, and a guest writing
 * sectors to it raw is a guest that can destroy it.
 */
bool __not_in_flash_func(divmmc_zc_out)(const uint16_t port, const uint8_t value) {
    if (!settings.zcontroller) return false;
    switch (port & 0xFFu) {
        case 0x77: divmmc_card_cs((value & 0x02u) ? 1u : 0u); return true;
        case 0x57: card_write(value); return true;
        default:   return false;
    }
}

bool __not_in_flash_func(divmmc_zc_in)(const uint16_t port, uint8_t *value) {
    if (!settings.zcontroller) return false;
    switch (port & 0xFFu) {
        case 0x57: *value = card_read(); return true;
        // Status: bit 0 clear = a card is in, bit 1 clear = writable.
        case 0x77: *value = card_open ? 0x00u : 0x01u; return true;
        default:   return false;
    }
}

bool __not_in_flash_func(divmmc_port_in)(const uint16_t port, uint8_t *value) {
    if (!settings.esxdos) return false;
    if ((port & 0xFFu) != 0xEB) return false;
    *value = card_read();
    return true;
}
