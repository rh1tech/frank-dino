#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <hardware/gpio.h>   /* as the real hardware/pio.h does */
typedef unsigned int uint;
struct pio_program { const uint16_t *instructions; uint8_t length; int8_t origin; };
typedef struct pio_hw { int idx; volatile uint32_t txf[4], rxf[4]; } *PIO;
typedef struct { uint32_t clkdiv, execctrl, shiftctrl, pinctrl; } pio_sm_config;
extern struct pio_hw pio0_hw_inst, pio1_hw_inst;
#define pio0 (&pio0_hw_inst)
#define pio1 (&pio1_hw_inst)
static inline uint  pio_add_program(PIO p, const struct pio_program *q) { (void)p; (void)q; return 0; }
static inline uint  pio_claim_unused_sm(PIO p, bool r) { (void)p; (void)r; return 0; }
static inline void  pio_sm_set_enabled(PIO p, uint s, bool e) { (void)p; (void)s; (void)e; }
static inline void  pio_sm_init(PIO p, uint s, uint o, const pio_sm_config *c) { (void)p; (void)s; (void)o; (void)c; }
static inline void  pio_sm_set_consecutive_pindirs(PIO p, uint s, uint b, uint n, bool o) { (void)p; (void)s; (void)b; (void)n; (void)o; }
static inline void  pio_gpio_init(PIO p, uint g) { (void)p; (void)g; }
static inline void  pio_sm_clear_fifos(PIO p, uint s) { (void)p; (void)s; }
static inline uint  pio_get_dreq(PIO p, uint s, bool t) { (void)p; (void)s; (void)t; return 0; }
static inline pio_sm_config pio_get_default_sm_config(void) { pio_sm_config c = {0,0,0,0}; return c; }
static inline void sm_config_set_out_pins(pio_sm_config *c, uint b, uint n) { (void)c; (void)b; (void)n; }
static inline void sm_config_set_out_shift(pio_sm_config *c, bool r, bool a, uint n) { (void)c; (void)r; (void)a; (void)n; }
static inline void sm_config_set_fifo_join(pio_sm_config *c, int j) { (void)c; (void)j; }
static inline void sm_config_set_clkdiv(pio_sm_config *c, float d) { (void)c; (void)d; }
static inline void sm_config_set_clkdiv_int_frac(pio_sm_config *c, uint16_t i, uint8_t f) { (void)c; (void)i; (void)f; }
static inline void sm_config_set_wrap(pio_sm_config *c, uint a, uint b) { (void)c; (void)a; (void)b; }
#define PIO_FIFO_JOIN_TX 1
#define DREQ_PIO0_TX0 0
#define DREQ_PIO1_TX0 8
static inline void pio_set_gpio_base(PIO p, uint b) { (void)p; (void)b; }
static inline void sm_config_set_out_pin_base(pio_sm_config *c, uint b) { (void)c; (void)b; }
static inline void sm_config_set_out_pin_count(pio_sm_config *c, uint n) { (void)c; (void)n; }
static inline void pio_sm_set_clkdiv(PIO p, uint s, float d) { (void)p; (void)s; (void)d; }
static inline void pio_sm_set_clkdiv_int_frac(PIO p, uint s, uint16_t i, uint8_t f) { (void)p; (void)s; (void)i; (void)f; }
