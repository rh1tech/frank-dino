#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef unsigned int uint;
typedef struct { uint32_t read_addr, write_addr, transfer_count, ctrl_trig, al1_ctrl; } dma_ch_t;
typedef struct { dma_ch_t ch[16]; volatile uint32_t ints0, inte0, ints1, inte1; } dma_hw_t;
extern dma_hw_t *dma_hw;
typedef struct { uint32_t ctrl; } dma_channel_config;
static inline int  dma_claim_unused_channel(bool r) { (void)r; return 0; }
static inline void dma_channel_set_read_addr(uint c, const volatile void *a, bool t) { (void)c; (void)a; (void)t; }
static inline void dma_channel_set_write_addr(uint c, volatile void *a, bool t) { (void)c; (void)a; (void)t; }
static inline void dma_channel_set_trans_count(uint c, uint32_t n, bool t) { (void)c; (void)n; (void)t; }
static inline void dma_channel_configure(uint c, const dma_channel_config *g, volatile void *w, const volatile void *r, uint32_t n, bool t) { (void)c; (void)g; (void)w; (void)r; (void)n; (void)t; }
static inline dma_channel_config dma_channel_get_default_config(uint c) { (void)c; dma_channel_config g = {0}; return g; }
static inline void channel_config_set_transfer_data_size(dma_channel_config *g, int s) { (void)g; (void)s; }
static inline void channel_config_set_read_increment(dma_channel_config *g, bool b) { (void)g; (void)b; }
static inline void channel_config_set_write_increment(dma_channel_config *g, bool b) { (void)g; (void)b; }
static inline void channel_config_set_dreq(dma_channel_config *g, uint d) { (void)g; (void)d; }
static inline void channel_config_set_chain_to(dma_channel_config *g, uint c) { (void)g; (void)c; }
static inline void dma_channel_start(uint c) { (void)c; }
static inline void dma_channel_abort(uint c) { (void)c; }
#define DMA_SIZE_8  0
#define DMA_SIZE_16 1
#define DMA_SIZE_32 2
static inline void dma_channel_set_irq0_enabled(uint c, bool e) { (void)c; (void)e; }
static inline void dma_channel_set_irq1_enabled(uint c, bool e) { (void)c; (void)e; }
static inline void dma_channel_acknowledge_irq0(uint c) { (void)c; }
static inline void dma_channel_acknowledge_irq1(uint c) { (void)c; }
static inline bool dma_channel_get_irq0_status(uint c) { (void)c; return false; }
static inline void dma_start_channel_mask(uint32_t m) { (void)m; }
