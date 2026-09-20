#
# frank-dino — an RP2350B acting as the whole machine around a real Z80
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-dino
# SPDX-License-Identifier: GPL-3.0-or-later
#
# fatfs — ChaN's FatFs, vendored. See 00readme.txt for its own licence.

if (NOT TARGET fatfs)
    add_library(fatfs INTERFACE)

    target_sources(fatfs INTERFACE
            ${CMAKE_CURRENT_LIST_DIR}/ff.c
            ${CMAKE_CURRENT_LIST_DIR}/f_util.c
            ${CMAKE_CURRENT_LIST_DIR}/ffsystem.c
            ${CMAKE_CURRENT_LIST_DIR}/ffunicode.c
    )

    target_include_directories(fatfs INTERFACE ${CMAKE_CURRENT_LIST_DIR})
    target_link_libraries(fatfs INTERFACE pico_stdlib hardware_clocks hardware_spi)
endif()
