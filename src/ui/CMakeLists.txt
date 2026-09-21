#
# frank-dino — an RP2350B acting as the whole machine around a real Z80
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-dino
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ui — the report, drawn into the text buffer and echoed to the console.

add_library(dino_ui INTERFACE)

target_sources(dino_ui INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/screen.c
        ${CMAKE_CURRENT_LIST_DIR}/menu.c
        ${CMAKE_CURRENT_LIST_DIR}/options.c)

target_include_directories(dino_ui INTERFACE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(dino_ui INTERFACE vga spectrum psram pico_stdio)
