/* Host-test prelude: neutralise the firmware's placement attributes. */
#pragma once
#define __not_in_flash_func(f) f
#define __not_in_flash(g) g
#define __time_critical_func(f) f
#ifndef __force_inline
#define __force_inline inline
#endif
#define count_of(a) (sizeof(a) / sizeof((a)[0]))
#define tight_loop_contents() ((void)0)
#define make_timeout_time_ms(ms) ((absolute_time_t)(ms))

/* Mach-O has no ".section .rodata.x"; the const data section is __TEXT,__const
 * and every C symbol carries a leading underscore. Same bytes, same names. */
#ifdef __APPLE__
#define IMPORT_BIN(file, sym) asm (\
    ".const_data\n"               \
    ".balign 4\n"                 \
    ".global _" #sym "\n"         \
    "_" #sym ":\n"                \
    ".incbin \"" file "\"\n"      \
    ".balign 4\n"                 \
    ".text\n")
#else
#define IMPORT_BIN(file, sym) asm (\
    ".section .rodata." #sym "\n" \
    ".balign 4\n"                 \
    ".global " #sym "\n"          \
    #sym ":\n"                    \
    ".incbin \"" file "\"\n"      \
    ".balign 4\n"                 \
    ".section \".text\"\n")
#endif
#define busy_wait_us(u) ((void)(u))
#define busy_wait_us_32(u) ((void)(u))
