#
# frank-dino — an RP2350B acting as the whole machine around a real Z80
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-dino
# SPDX-License-Identifier: GPL-3.0-or-later
#
# psram — the 8 MB on XIP CS1, configured into the address map.

add_library(psram INTERFACE)

target_sources(psram INTERFACE ${CMAKE_CURRENT_LIST_DIR}/psram.c)

target_include_directories(psram INTERFACE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(psram INTERFACE dino_core hardware_clocks hardware_gpio hardware_sync)
