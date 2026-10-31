#
# frank-dino — an RP2350B acting as the whole machine around a real Z80
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-dino
# SPDX-License-Identifier: GPL-3.0-or-later
#
# chips — the FM and PSG cores ported from pico-speccy (GPL-3.0), in C++
# as they were written, behind a C face (chips.h) for sound.c.

add_library(chips INTERFACE)

target_sources(chips INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/chips.cpp
        ${CMAKE_CURRENT_LIST_DIR}/OpnFm.cpp
        ${CMAKE_CURRENT_LIST_DIR}/OplFm.cpp
        ${CMAKE_CURRENT_LIST_DIR}/OpllFm.cpp
        ${CMAKE_CURRENT_LIST_DIR}/FmTables.cpp
        ${CMAKE_CURRENT_LIST_DIR}/SnSound.cpp
)

target_include_directories(chips INTERFACE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(chips INTERFACE pico_stdlib)
