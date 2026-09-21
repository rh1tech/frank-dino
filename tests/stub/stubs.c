/*
 * Host-test stand-ins for the peripherals spectrum.c talks to.
 *
 * Everything here is inert by default so a paging test sees a bare
 * machine, and the few things a test has to steer -- whether the Beta
 * Disk ROM is in, whether DivMMC is mapped, what the dock holds -- are
 * plain globals it can set.
 */
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "spectrum.h"
#include "beta.h"
#include "dck.h"
#include "divmmc.h"
#include "fdc.h"
#include "ay.h"
#include "tape.h"
#include "audio.h"
#include "settings.h"
#include "z80_bus.h"

/* --- the knobs a test turns ------------------------------------------ */
bool     stub_beta_paged = false;
bool     stub_beta_boot  = false;
bool     stub_divmmc_mapped = false;
uint8_t  stub_dock[8][8192];
bool     stub_dock_writable[8];

/* --- settings: defaults, as a fresh card would give ------------------ */
settings_t settings;

#ifndef REAL_BETA
/* --- Beta Disk ------------------------------------------------------- */
bool beta_rom_paged(void)        { return stub_beta_paged; }
void beta_set_rom_paged(bool on) { stub_beta_paged = on; }
bool beta_take_boot_request(void){ bool b = stub_beta_boot; stub_beta_boot = false; return b; }
void beta_reset(void)            { }
bool beta_port_in(uint16_t p, uint8_t *v)  { (void)p; (void)v; return false; }
bool beta_port_out(uint16_t p, uint8_t v)  { (void)p; (void)v; return false; }

#endif

#ifndef REAL_DCK
/* --- Timex dock ------------------------------------------------------ */
uint8_t *dck_chunk(unsigned i)   { return stub_dock[i & 7]; }
bool     dck_writable(unsigned i){ return stub_dock_writable[i & 7]; }
bool     dck_mounted(void)       { return false; }
void     dck_eject(void)         { }

#endif

#ifndef REAL_DIVMMC
/* --- DivMMC ---------------------------------------------------------- */
static uint8_t divmmc_lo[8192], divmmc_hi[8192];
bool     divmmc_mapped(void)      { return stub_divmmc_mapped; }
bool     divmmc_lo_writable(void) { return false; }
uint8_t *divmmc_page_lo(void)     { return divmmc_lo; }
uint8_t *divmmc_page_hi(void)     { return divmmc_hi; }
void     divmmc_opfetch(uint16_t a) { (void)a; }
void     divmmc_reset(void)       { stub_divmmc_mapped = false; }
bool     divmmc_port_in(uint16_t p, uint8_t *v)  { (void)p; (void)v; return false; }
bool     divmmc_port_out(uint16_t p, uint8_t v)  { (void)p; (void)v; return false; }

#endif

#ifndef REAL_FDC
/* --- uPD765 ---------------------------------------------------------- */
void fdc_reset(void) { }
bool fdc_port_in(uint16_t p, uint8_t *v)  { (void)p; (void)v; return false; }
bool fdc_port_out(uint16_t p, uint8_t v)  { (void)p; (void)v; return false; }

#endif

#ifndef REAL_AY
/* --- AY -------------------------------------------------------------- */
void    ay_reset(ay_t *a)              { (void)a; }
void    ay_select(ay_t *a, uint8_t r)  { (void)a; (void)r; }
void    ay_write(ay_t *a, uint8_t v)   { (void)a; (void)v; }
uint8_t ay_read(const ay_t *a)         { (void)a; return 0xFF; }
int32_t ay_render(ay_t *a, uint32_t t) { (void)a; (void)t; return 0; }

#endif

#ifndef REAL_TAPE
/* --- tape ------------------------------------------------------------ */
void tape_repatch_rom(void)     { }
void tape_machine_changed(void) { }
bool tape_port_in(uint16_t p, uint8_t *v)  { (void)p; (void)v; return false; }
bool tape_port_out(uint16_t p, uint8_t v)  { (void)p; (void)v; return false; }

#endif

/* --- audio ----------------------------------------------------------- */
void     audio_push(int32_t s)     { (void)s; }
void     audio_reset_stats(void)   { }
uint32_t audio_ring_level(void)    { return 0; }
void     audio_set_enabled(bool o) { (void)o; }

/* --- the SIO block the bus engine bit-bangs --------------------------
 * A real block of memory rather than a pointer into nowhere, so
 * z80_reset() and friends can run harmlessly on the host. Nothing here
 * models a Z80; the paging tests drive the port handler directly. */
#include <hardware/structs/sio.h>
static sio_hw_t sio_block;
sio_hw_t *sio_hw = &sio_block;

/* --- machine state, when spectrum.c itself is not in the link -------- */
#ifndef REAL_SPECTRUM
/* The real esxDOS image, by the same route the firmware embeds it. */
IMPORT_BIN("roms/esxdos.rom", spec_romesx);
extern const uint8_t spec_romesx[8192];
machine_t spec_machine = MACHINE_128K;
volatile uint8_t spec_port_7ffd = 0, spec_port_1ffd = 0, spec_port_dffd = 0;
volatile uint8_t spec_screen_bank = 5;
volatile uint8_t spec_timex_hsr = 0;
volatile bool    spec_timex_exrom = false;
uint8_t spec_ram[SPEC_RAM_BANKS][SPEC_BANK_SIZE];
void apply_paging_hook(void) { }
uint8_t *spec_bank_ptr(unsigned b) { return spec_ram[b & 7]; }
void     spec_bank_flush(void) { }
/* One 16 KB page standing in for the machine's BASIC ROM, so the tape
 * trap has somewhere to patch. */
static uint8_t basic_slot_page[SPEC_BANK_SIZE];
uint8_t *spec_basic_slot(void) { return basic_slot_page; }
volatile uint8_t spec_border = 7;
volatile bool    spec_ulaplus_on = false, spec_ulaplus_dirty = false;
volatile uint8_t spec_ulaplus_reg = 0;
uint8_t          spec_ulaplus_pal[64];
volatile uint32_t spec_frames = 0;
volatile bool     spec_flash = false;
volatile uint8_t  spec_timex_mode = 0, spec_timex_ink = 0;
#endif

/* --- the DMA block the video driver programs ------------------------- */
#include <hardware/dma.h>
#include <hardware/pio.h>
static dma_hw_t dma_block;
dma_hw_t *dma_hw = &dma_block;
struct pio_hw pio0_hw_inst, pio1_hw_inst;

/* The UI surface, when the video driver itself is not in the link. */
#ifndef REAL_VGA
#include "vga.h"
uint8_t vga_ui_fb[VGA_UI_W * VGA_UI_H];
void vga_set_mode(vga_mode_t m) { (void)m; }
void vga_build_ui_palette(void) { }
#endif
