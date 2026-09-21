#
# frank-dino — an RP2350B acting as the whole machine around a real Z80
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-dino
# SPDX-License-Identifier: GPL-3.0-or-later
#
# spectrum — the half of a ZX Spectrum 128K that is not the CPU.

add_library(spectrum INTERFACE)

target_sources(spectrum INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/spectrum.c
        ${CMAKE_CURRENT_LIST_DIR}/ay.c
        ${CMAKE_CURRENT_LIST_DIR}/launch.c
        ${CMAKE_CURRENT_LIST_DIR}/storage.c
        ${CMAKE_CURRENT_LIST_DIR}/loader.c)

target_include_directories(spectrum INTERFACE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(spectrum INTERFACE dino_core audio fatfs hardware_gpio)
