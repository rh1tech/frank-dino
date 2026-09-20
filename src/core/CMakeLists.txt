#
# frank-dino — an RP2350B acting as the whole machine around a real Z80
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-dino
# SPDX-License-Identifier: GPL-3.0-or-later
#
# core — the bus engine, the pin map, the guest's memory.

add_library(dino_core INTERFACE)

target_sources(dino_core INTERFACE ${CMAKE_CURRENT_LIST_DIR}/z80_bus.c)

target_include_directories(dino_core INTERFACE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(dino_core INTERFACE
        pico_stdlib
        hardware_clocks
        hardware_gpio)
