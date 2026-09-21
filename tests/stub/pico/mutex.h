#pragma once
#include <stdbool.h>
typedef struct { int d; } recursive_mutex_t;
static inline void recursive_mutex_init(recursive_mutex_t *m) { (void)m; }
static inline void recursive_mutex_enter_blocking(recursive_mutex_t *m) { (void)m; }
static inline bool recursive_mutex_try_enter(recursive_mutex_t *m, uint32_t *o) { (void)m; (void)o; return true; }
static inline void recursive_mutex_exit(recursive_mutex_t *m) { (void)m; }
