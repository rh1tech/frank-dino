#
# frank-dino — an RP2350B acting as the whole machine around a real Z80
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-dino
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usbkbd — a USB keyboard onto the Spectrum's matrix, via TinyUSB host.
#
# Only built when USB_KEYBOARD is on, because host and device modes are
# the same pins behind S1 and cannot both be useful at once.

add_library(usbkbd INTERFACE)

target_sources(usbkbd INTERFACE ${CMAKE_CURRENT_LIST_DIR}/usbkbd.c)

# tusb_config.h has to be found before the SDK's own, so this directory
# goes on the include path of everything that pulls TinyUSB in.
target_include_directories(usbkbd INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/../../spectrum
        ${CMAKE_CURRENT_LIST_DIR}/../../ui)

target_link_libraries(usbkbd INTERFACE
        tinyusb_host
        tinyusb_board
        pico_stdlib)
