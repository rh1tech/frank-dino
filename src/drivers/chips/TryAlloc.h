/*
 * frank-dino: the one thing pico-speccy's FM cores need from its app/,
 * a malloc that does not panic. newlib's returns NULL here.
 */
#pragma once
#include <stdlib.h>
static inline void *tryMalloc(size_t n) { return malloc(n); }
static inline void *tryCalloc(size_t n) { return calloc(n, 1); }
