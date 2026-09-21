#
# frank-dino — an RP2350B acting as the whole machine around a real Z80
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# miniz — Rich Geldreich's deflate/inflate, as pico-speccy vendors it.
#
# MIT / unlicense, not GPL: see the notices in miniz.c. The file is here
# byte for byte and is not to be edited -- everything this firmware needs
# to turn off is a define, which is the whole reason it is one file.
#
# What is turned off, and why it matters here: this is a copy_to_ram
# build, so every byte of code that links is a byte of the 512 KB. All
# that survives is tinfl, the raw inflate -- no deflate, no archive
# reader, no zlib wrappers, no stdio, no malloc.

add_library(miniz INTERFACE)

target_sources(miniz INTERFACE ${CMAKE_CURRENT_LIST_DIR}/miniz.c)

target_include_directories(miniz INTERFACE ${CMAKE_CURRENT_LIST_DIR})

# miniz.h already turns off stdio, time, deflate and the archive reader
# for an embedded target -- pico-speccy left those in the header. What is
# added here is the pair they still need and this does not: the zlib
# compatibility wrappers (inflateInit2 and friends), because zip.c calls
# tinfl directly, and malloc, because there is none.
target_compile_definitions(miniz INTERFACE
        MINIZ_NO_ZLIB_APIS
        MINIZ_NO_MALLOC)

# Not ours: -Wall -Wextra on a vendored amalgamation is noise nobody acts on.
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/miniz.c
        PROPERTIES COMPILE_OPTIONS "-w")
