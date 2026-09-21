#pragma once
#include <stdint.h>
#include <stdbool.h>
#define GPIO_IN  false
#define GPIO_OUT true
enum gpio_function { GPIO_FUNC_SIO = 5, GPIO_FUNC_PIO0 = 6 };
static inline void gpio_put(uint32_t p, bool v) { (void)p; (void)v; }
static inline bool gpio_get(uint32_t p) { (void)p; return true; }
static inline void gpio_init(uint32_t p) { (void)p; }
static inline void gpio_deinit(uint32_t p) { (void)p; }
static inline void gpio_set_dir(uint32_t p, bool out) { (void)p; (void)out; }
static inline void gpio_set_pulls(uint32_t p, bool up, bool down) { (void)p; (void)up; (void)down; }
static inline void gpio_pull_up(uint32_t p) { (void)p; }
static inline void gpio_disable_pulls(uint32_t p) { (void)p; }
static inline void gpio_set_function(uint32_t p, int f) { (void)p; (void)f; }
static inline void gpio_set_input_hysteresis_enabled(uint32_t p, bool e) { (void)p; (void)e; }
static inline void gpio_set_slew_rate(uint32_t p, int s) { (void)p; (void)s; }
static inline void gpio_set_drive_strength(uint32_t p, int s) { (void)p; (void)s; }
