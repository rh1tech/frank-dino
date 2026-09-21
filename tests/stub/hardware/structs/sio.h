#pragma once
#include <stdint.h>
typedef struct {
    volatile uint32_t gpio_in, gpio_hi_in, gpio_out, gpio_set, gpio_clr, gpio_togl;
    volatile uint32_t gpio_oe, gpio_oe_set, gpio_oe_clr;
    volatile uint32_t gpio_hi_out, gpio_hi_set, gpio_hi_clr, gpio_hi_oe, gpio_hi_oe_set, gpio_hi_oe_clr;
} sio_hw_t;
extern sio_hw_t *sio_hw;
