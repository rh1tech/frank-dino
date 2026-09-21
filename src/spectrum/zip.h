/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Reading a .zip, ported from pico-speccy src/ZipExtract.cpp.
 *
 * Their local-file-header walk and their two extract paths -- stored and
 * deflate -- over miniz's tinfl, which is the only part of miniz that
 * links here.
 *
 * Why a walk of the local headers rather than the central directory: it
 * is what theirs does, and the reason is that an archive written by
 * something that streams has a central directory whose offsets are wrong
 * while the local headers are always right. The cost is that a member's
 * sizes may be in a data descriptor after the data rather than in the
 * header, which is the one case this refuses -- and says so.
 *
 * Nothing is decompressed into memory. A member is extracted to a temp
 * file on the card and the ordinary loader opens it, so a 700 KB .trd
 * inside a zip costs a file and not a buffer.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "storage.h"

/* Where an extracted member lands, as theirs does it. */
#define ZIP_TEMP_PATH "/.dino-unzip.tmp"

#define ZIP_MAX_MEMBERS 64

typedef struct {
    char        name[STORAGE_MAX_NAME];
    uint32_t    size;           /* uncompressed */
    file_kind_t kind;
    uint32_t    offset;         /* of the local header, for zip_extract() */
} zip_member_t;

/*
 * List the members this firmware could open, newest header first.
 *
 * Returns the number written, or -1 if the file is not a zip. Members in
 * formats nothing here reads are left out: a zip of a game and its
 * loading screen and a text file should offer the game.
 */
int zip_list(const char *path, zip_member_t *out, int max);

/*
 * Extract one member to ZIP_TEMP_PATH.
 *
 * `detail` is filled either way. Returns false on a compression method
 * that is not stored or deflate, on a truncated archive, or on a write
 * that the card refused.
 */
bool zip_extract(const char *path, const zip_member_t *m,
                 char *detail, unsigned detail_len);
