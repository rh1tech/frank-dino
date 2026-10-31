#
# frank-dino — an RP2350B acting as the whole machine around a real Z80
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-dino
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ui — the report in the text buffer, and the menu on its own 320x240 surface.

add_library(dino_ui INTERFACE)

target_sources(dino_ui INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/screen.c
        ${CMAKE_CURRENT_LIST_DIR}/menu.c
        ${CMAKE_CURRENT_LIST_DIR}/conmon.c
        ${CMAKE_CURRENT_LIST_DIR}/ui_font.c
        ${CMAKE_CURRENT_LIST_DIR}/ui_gfx.c
        ${CMAKE_CURRENT_LIST_DIR}/ui_render.c
        ${CMAKE_CURRENT_LIST_DIR}/ui_report.c
        ${CMAKE_CURRENT_LIST_DIR}/ui_nav.c
        ${CMAKE_CURRENT_LIST_DIR}/ui_browser.c
        ${CMAKE_CURRENT_LIST_DIR}/ui_capture.c
        ${CMAKE_CURRENT_LIST_DIR}/ui_dialog.c
        ${CMAKE_CURRENT_LIST_DIR}/ui_tree.c)

target_include_directories(dino_ui INTERFACE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(dino_ui INTERFACE vga spectrum psram dino_core pico_stdio)
