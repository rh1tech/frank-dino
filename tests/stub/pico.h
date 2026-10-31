#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* The host is one core; call it core 1, where the machine runs. */
static inline unsigned get_core_num(void) { return 1; }
