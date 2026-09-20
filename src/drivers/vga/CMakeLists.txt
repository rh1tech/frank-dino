#
# frank-dino — an RP2350B acting as the whole machine around a real Z80
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-dino
# SPDX-License-Identifier: GPL-3.0-or-later
#
# vga — 640x480 at 60 Hz, 80x30 text, out of the six-bit R2R ladder and
# the two sync lines on GP30-GP37. VGA_BASE_PIN comes from core/dino.h,
# which this links against for that and for likely()/unlikely().

add_library(vga INTERFACE)

target_sources(vga INTERFACE ${CMAKE_CURRENT_LIST_DIR}/vga.c)

target_include_directories(vga INTERFACE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(vga INTERFACE
        dino_core
        hardware_pio
        hardware_dma
        hardware_clocks)
