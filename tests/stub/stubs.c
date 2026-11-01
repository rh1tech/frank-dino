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
volatile bool beta_ports_open;
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
bool     divmmc_zc_in(uint16_t p, uint8_t *v)    { (void)p; (void)v; return false; }
bool     divmmc_zc_out(uint16_t p, uint8_t v)    { (void)p; (void)v; return false; }

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
void    ay_render_stereo(ay_t *a, uint32_t t, ay_stereo_t m, int32_t *l, int32_t *r) {
    (void)a; (void)t; (void)m; *l = *r = 0;
}

#endif

#ifndef REAL_TAPE
/* --- tape ------------------------------------------------------------ */
void tape_repatch_rom(void)     { }
void tape_machine_changed(void) { }
bool tape_port_in(uint16_t p, uint8_t *v)  { (void)p; (void)v; return false; }
bool tape_port_out(uint16_t p, uint8_t v)  { (void)p; (void)v; return false; }
bool tape_mounted(void)         { return false; }

#endif

/* --- the real-tape input: never running on a host -------------------- */
bool tapein_running(void)              { return false; }
void tapein_frame_started(uint32_t t)  { (void)t; }
bool tapein_level(uint32_t t)          { (void)t; return false; }

/* --- audio ----------------------------------------------------------- */
#ifndef REAL_AUDIO_CAPTURE
void     audio_push(int32_t l, int32_t r) { (void)l; (void)r; }
#endif
void     audio_reset_stats(void)   { }
uint32_t audio_ring_level(void)    { return 0; }
uint32_t audio_rate_millihz(void)  { return 44100000u; }
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
const uint8_t *spec_bank_host(unsigned b) { return spec_ram[b & 7]; }
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
volatile bool     spec_giga_auto = false;
volatile bool     spec_pent16c = false;
volatile uint8_t  spec_port_eff7 = 0;
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

/* --- the trampoline's page, which the NMI dump borrows ---------------- */
#ifndef REAL_LAUNCH
static uint8_t stub_launch_page[16384];
uint8_t *spec_launch_page(void) { return stub_launch_page; }
#endif

/* --- the beam-accurate frame: off, unless the test links the real one - */
#ifndef REAL_ULA
#include "ula_frame.h"
volatile bool ula_beam;
const ula_frame_t *ula_frame_latest(void) { return NULL; }
void ula_border(uint32_t t, uint8_t c) { (void)t; (void)c; }
void ula_rewatch(void) { }
void ula_frame_begin(void) { }
void ula_frame_end(void) { }
void ula_set_enabled(bool on) { (void)on; }
#endif

/* --- the ATM, for the video test: never the running machine there --- */
#ifndef REAL_ATM
#include "atm.h"
volatile bool atm_pal_dirty;
void atm_palette_rgb2(uint8_t rgb[16][3]) { memset(rgb, 0, 48); }
atm_vmode_t atm_video_mode(void) { return ATM_VM_ZX; }
uint8_t atm_border_index(void) { return 0; }
const uint8_t *atm_font_ptr(void) { static uint8_t f[2048]; return f; }
#endif

/* --- the FM and PSG cores: silent, unless a test links them ----------- */
#ifndef REAL_CHIPS
#include "chips.h"
void chips_init(int r) { (void)r; }
void chips_reset(void) { }
void chips_opn_write(int c, bool d, uint8_t v) { (void)c; (void)d; (void)v; }
void chips_opl_write(int p, uint8_t v) { (void)p; (void)v; }
void chips_opll_write(bool d, uint8_t v) { (void)d; (void)v; }
void chips_sn_write(int c, uint8_t v) { (void)c; (void)v; }
void chips_sample(int32_t *l, int32_t *r) { *l = *r = 0; }
bool chips_any(void) { return false; }
#endif

/* --- the hard disk: never fitted on a host --------------------------- */
#ifndef REAL_IDE
bool ide_port_out(uint16_t p, uint8_t v) { (void)p; (void)v; return false; }
bool ide_port_in(uint16_t p, uint8_t *v) { (void)p; (void)v; return false; }
void ide_reset(void) { }
bool ide_mounted(void) { return false; }
uint8_t ide_smuc_read(unsigned r) { (void)r; return 0; }
void    ide_smuc_write(unsigned r, uint8_t v) { (void)r; (void)v; }
uint8_t ide_smuc_latch_read(void) { return 0; }
void    ide_smuc_latch_write(uint8_t v) { (void)v; }
void    ide_smuc_control(uint8_t v) { (void)v; }
#endif

/* --- ROMs from the card: there is no card on a host ------------------- */
#include "customrom.h"
bool customrom_active, customrom_trdos_active;
const char *customrom_tag(machine_t m, unsigned *b) { (void)m; *b = 0; return NULL; }
bool customrom_load(machine_t m, uint8_t *const *s, unsigned b) { (void)m; (void)s; (void)b; return false; }
bool customrom_trdos(uint8_t *d) { (void)d; return false; }

/* --- the clock: answered by rtc.c where a test links it -------------- */
#ifndef REAL_RTC
void    rtc_select(uint8_t r) { (void)r; }
uint8_t rtc_read(void) { return 0x55; }
void    rtc_write(uint8_t v) { (void)v; }
#endif
