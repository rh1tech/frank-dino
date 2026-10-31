#pragma once
#include <stdint.h>
/* The microsecond counter vga.c times its lines with: always zero here. */
typedef struct { volatile uint32_t timerawl; } timer_hw_t;
static timer_hw_t timer0_hw_stub;
#define timer0_hw (&timer0_hw_stub)
