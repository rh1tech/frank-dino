#
# frank-dino — an RP2350B acting as the whole machine around a real Z80
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-dino
# SPDX-License-Identifier: GPL-3.0-or-later
#
# spectrum — the half of a ZX Spectrum 128K that is not the CPU.

#
# The ROM images are pulled into spectrum.c by .incbin from inline asm,
# which nothing in the build system can see. Without this, replacing a
# .rom leaves the old bytes in the binary and the next flash quietly
# ships them -- which cost a debugging session once already.
#
file(GLOB DINO_ROMS ${CMAKE_CURRENT_LIST_DIR}/../../roms/*.rom)
set_property(SOURCE ${CMAKE_CURRENT_LIST_DIR}/spectrum.c
        ${CMAKE_CURRENT_LIST_DIR}/saa.c
             APPEND PROPERTY OBJECT_DEPENDS "${DINO_ROMS}")

add_library(spectrum INTERFACE)

target_sources(spectrum INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/spectrum.c
        ${CMAKE_CURRENT_LIST_DIR}/saa.c
        ${CMAKE_CURRENT_LIST_DIR}/ay.c
        ${CMAKE_CURRENT_LIST_DIR}/launch.c
        ${CMAKE_CURRENT_LIST_DIR}/storage.c
        ${CMAKE_CURRENT_LIST_DIR}/loader.c
        ${CMAKE_CURRENT_LIST_DIR}/tape.c
        ${CMAKE_CURRENT_LIST_DIR}/dck.c
        ${CMAKE_CURRENT_LIST_DIR}/divmmc.c
        ${CMAKE_CURRENT_LIST_DIR}/fdc.c
        ${CMAKE_CURRENT_LIST_DIR}/zip.c
        ${CMAKE_CURRENT_LIST_DIR}/settings.c
        ${CMAKE_CURRENT_LIST_DIR}/beta.c)

target_include_directories(spectrum INTERFACE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(spectrum INTERFACE dino_core audio fatfs miniz psram hardware_gpio)
