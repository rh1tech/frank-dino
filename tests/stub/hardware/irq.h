#pragma once
#include <stdint.h>
typedef unsigned int uint;
typedef void (*irq_handler_t)(void);
static inline void irq_set_exclusive_handler(uint n, irq_handler_t h) { (void)n; (void)h; }
static inline void irq_set_enabled(uint n, bool e) { (void)n; (void)e; }
static inline void irq_set_priority(uint n, uint8_t p) { (void)n; (void)p; }
#define DMA_IRQ_0 0
#define DMA_IRQ_1 1
