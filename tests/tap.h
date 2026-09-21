/* A very small check-and-count harness, shared by the host tests. */
#pragma once
#include <stdio.h>
#include <string.h>

static int tap_pass, tap_fail;
static const char *tap_group = "";

#define GROUP(n) (tap_group = (n))

#define CHECK(cond, ...) do {                                           \
        if (cond) { tap_pass++; }                                       \
        else { tap_fail++;                                              \
               printf("  FAIL  [%s] ", tap_group);                      \
               printf(__VA_ARGS__); printf("\n"); }                     \
    } while (0)

#define EQ(got, want, ...) do {                                         \
        long long g_ = (long long)(got), w_ = (long long)(want);        \
        if (g_ == w_) { tap_pass++; }                                   \
        else { tap_fail++;                                              \
               printf("  FAIL  [%s] ", tap_group);                      \
               printf(__VA_ARGS__);                                     \
               printf(": got %lld (0x%llX), want %lld (0x%llX)\n",      \
                      g_, g_, w_, w_); }                                \
    } while (0)

#define STREQ(got, want, ...) do {                                      \
        const char *g_ = (got), *w_ = (want);                           \
        if (strcmp(g_, w_) == 0) { tap_pass++; }                        \
        else { tap_fail++;                                              \
               printf("  FAIL  [%s] ", tap_group);                      \
               printf(__VA_ARGS__);                                     \
               printf(": got \"%s\", want \"%s\"\n", g_, w_); }         \
    } while (0)

static int tap_done(const char *name) {
    printf("%-22s %3d passed, %d failed\n", name, tap_pass, tap_fail);
    return tap_fail ? 1 : 0;
}
