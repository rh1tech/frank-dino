#pragma once
static inline void __dmb(void) {}
static inline void __sev(void) {}
static inline void __wfe(void) {}
static inline uint32_t save_and_disable_interrupts(void) { return 0; }
static inline void restore_interrupts(uint32_t s) { (void)s; }
