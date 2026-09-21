#
# frank-dino — an RP2350B acting as the whole machine around a real Z80
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usbdisk — a USB stick as FatFs volume 1, and the diskio router that
# lets there be more than one volume at all.

add_library(usbdisk INTERFACE)

target_sources(usbdisk INTERFACE ${CMAKE_CURRENT_LIST_DIR}/usbdisk.c)

target_include_directories(usbdisk INTERFACE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(usbdisk INTERFACE dino_core fatfs sdcard)
