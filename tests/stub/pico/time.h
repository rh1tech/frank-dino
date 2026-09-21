#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef uint64_t absolute_time_t;
static inline uint64_t time_us_64(void) { return 0; }
static inline uint32_t time_us_32(void) { return 0; }
static inline absolute_time_t get_absolute_time(void) { return 0; }
static inline int64_t absolute_time_diff_us(absolute_time_t a, absolute_time_t b) { return (int64_t)(b - a); }
static inline void sleep_ms(uint32_t ms) { (void)ms; }
static inline void sleep_us(uint64_t us) { (void)us; }
