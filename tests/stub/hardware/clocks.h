#pragma once
#include <stdint.h>
static inline uint32_t clock_get_hz(int c) { (void)c; return 150000000u; }
#define clk_sys 0
