#
# frank-dino — an RP2350B acting as the whole machine around a real Z80
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-dino
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The top of the build. Configure with `cmake -S src/app -B build`.
#
cmake_minimum_required(VERSION 3.13)

# Two roots, because two different things live at them. FW_ROOT is the
# repository — where boards/ and version.txt are. SRC_ROOT is the code.
set(SRC_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(FW_ROOT  ${CMAKE_CURRENT_LIST_DIR}/../..)

# boards/ stays at the repository root rather than under src/: this is the
# path the SDK's board-header convention expects, and it is configuration
# for the build rather than code the firmware compiles.
set(PICO_BOARD_HEADER_DIRS ${FW_ROOT}/boards CACHE PATH "" FORCE)
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD "frank_dino" CACHE STRING "" FORCE)
endif()
set(PICO_PLATFORM rp2350-arm-s)

# Version, from version.txt at the repo root: "MAJOR MINOR", rendered as
# MAJOR.MM. One file is the single source of truth for the banner and for
# release artefact names, so a release cannot ship a binary whose reported
# version disagrees with its filename.
file(READ ${FW_ROOT}/version.txt _ver_raw)
string(STRIP "${_ver_raw}" _ver_raw)
string(REPLACE " " ";" _ver_list "${_ver_raw}")
list(GET _ver_list 0 _ver_major)
list(GET _ver_list 1 _ver_minor)
string(LENGTH "${_ver_minor}" _ver_minor_len)
if(_ver_minor_len LESS 2)
    set(_ver_minor "0${_ver_minor}")
endif()
set(DINO_VERSION "${_ver_major}.${_ver_minor}")

include(${FW_ROOT}/pico_sdk_import.cmake)

project(frank-dino C CXX ASM)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)

pico_sdk_init()

message(STATUS "frank-dino ${DINO_VERSION} for ${PICO_BOARD}")

# ---------------------------------------------------------------------------
# Knobs
# ---------------------------------------------------------------------------
#
# CPU_SPEED sets the ceiling on how fast the Z80 can be clocked, because
# the Z80's clock comes out of a software loop on core 1. It does not set
# whether the firmware is *correct*: a Z84C00 is fully static, so a slower
# RP2350 makes a slower Z80 and nothing else.
#
# Stage 1 asked for the megahertz. With two AY chips rendered inside the
# same loop, 378 MHz reached 49.34 Hz of frame rate with the pace already
# at zero -- flat out and still 1.3 % slow, with no way to trim it. 504 MHz
# is what frank-xt8086 runs on this board and puts the headroom back.
set(CPU_SPEED 504 CACHE STRING "System clock, MHz")

# The full stage-0 suite instead of the Spectrum.
#
# Not a mode the machine can enter at runtime, because the tests are
# destructive: thirty resets, all 64 KB filled with patterns, and the
# pace left wherever the speed ramp finished. A build does one or the
# other.
option(SELFTEST "Run the stage-0 test suite instead of booting the Spectrum" OFF)

# A USB keyboard instead of a USB console.
#
# S1 routes the RP2350's D+/D- either to the USB-C socket or to the hub
# and the USB-A ports -- device or host, and the firmware cannot ask which
# way it is pointing. So this picks one at build time:
#
#   OFF  USB is a CDC console. keys.sh types at the machine, `picotool
#        reboot -u -f` reaches BOOTSEL, and there is no keyboard.
#   ON   USB is a HID host. A real keyboard works; the console is UART
#        only, and flashing is SWD only.
option(USB_KEYBOARD "USB HID keyboard instead of the USB console" OFF)

# ---------------------------------------------------------------------------
# Components
# ---------------------------------------------------------------------------
add_subdirectory(${SRC_ROOT}/core        core)
if(USB_KEYBOARD)
    add_subdirectory(${SRC_ROOT}/drivers/usbkbd drivers/usbkbd)
endif()
add_subdirectory(${SRC_ROOT}/drivers/fatfs  drivers/fatfs)
add_subdirectory(${SRC_ROOT}/drivers/sdcard drivers/sdcard)
add_subdirectory(${SRC_ROOT}/drivers/audio drivers/audio)
add_subdirectory(${SRC_ROOT}/drivers/miniz drivers/miniz)
add_subdirectory(${SRC_ROOT}/drivers/psram drivers/psram)
add_subdirectory(${SRC_ROOT}/drivers/usbdisk drivers/usbdisk)
add_subdirectory(${SRC_ROOT}/spectrum    spectrum)
add_subdirectory(${SRC_ROOT}/drivers/vga drivers/vga)
add_subdirectory(${SRC_ROOT}/ui          ui)
add_subdirectory(${SRC_ROOT}/test        test)

add_executable(frank-dino main.c)

target_include_directories(frank-dino PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${FW_ROOT})

target_compile_definitions(frank-dino PRIVATE
        FIRMWARE_NAME="frank-dino"
        FIRMWARE_VERSION="${DINO_VERSION}"

        CPU_SPEED_MHZ=${CPU_SPEED}
        SELFTEST=$<BOOL:${SELFTEST}>
        USB_KEYBOARD=$<BOOL:${USB_KEYBOARD}>

        # microSD on J3. The macro names say SPI0; the driver has always
        # used spi1, because GP40-43 have no SPI0 mapping at all. Left
        # spelled the upstream way so an update still applies.
        SDCARD_PIN_SPI0_MISO=40
        SDCARD_PIN_SPI0_CS=41
        SDCARD_PIN_SPI0_SCK=42
        SDCARD_PIN_SPI0_MOSI=43

        PICO_FLASH_SIZE_BYTES=16777216
        PICO_PRINTF_ALWAYS_INCLUDED=0

        # picotool's way in. The vendor interface is what `picotool load -f`
        # talks to to reboot a running board into BOOTSEL.
        PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE=1
        PICO_STDIO_USB_RESET_INTERFACE_SUPPORT_RESET_TO_BOOTSEL=1

        # Never wait for a host to open the port. The board has to run the
        # tests with nothing attached but an HDMI cable.
        PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0
)

target_link_libraries(frank-dino PRIVATE
        dino_core
        dino_ui
        dino_test
        spectrum
        audio
        miniz
        psram
        usbdisk
        vga
        sdcard

        pico_runtime
        pico_stdlib
        pico_stdio
        pico_multicore
        hardware_pio
        hardware_dma
        hardware_vreg
)

# .incbin in src/spectrum/spectrum.c names its ROMs relative to the
# repository root, and creates no dependency CMake can see -- so a changed
# ROM would otherwise leave the firmware linked against the copy it last
# saw, with no symptom but wrong behaviour.
set_source_files_properties(${SRC_ROOT}/spectrum/spectrum.c PROPERTIES
        OBJECT_DEPENDS "${FW_ROOT}/roms/128-0.rom;${FW_ROOT}/roms/128-1.rom;${FW_ROOT}/roms/48.rom;${FW_ROOT}/roms/plus2.rom")

# ---------------------------------------------------------------------------
# Console
# ---------------------------------------------------------------------------
#
# Both, and the USB half is not about the console.
#
# UART0 on J1 is the intended console. USB is enabled for the reset
# interface that comes with it: picotool can then put a running board into
# BOOTSEL by itself, and flashing stops needing two buttons pressed in the
# right order by someone standing at the bench. On a board whose SWD does
# not answer, that is the difference between a one-command edit-flash-look
# loop and a conversation.
#
# It costs the USB controller, which in stage 1 a keyboard will want back
# for HID host — the two cannot both be had. Until then there is no
# keyboard, and there is a great deal of reflashing.
#
# S1 selects which way the mux (U2) points: to J6 for device mode, which
# is what this needs, or to the hub for USB-A host. See docs/pinout.md.
pico_enable_stdio_uart(frank-dino 1)

if(USB_KEYBOARD)
    # The USB controller is a HID host now, so it cannot also enumerate as
    # a CDC device. UART only, and SWD for flashing.
    pico_enable_stdio_usb(frank-dino 0)
    target_link_libraries(frank-dino PRIVATE usbkbd)
    set(FAMILY rp2350)
    set(BOARD pico_sdk)
else()
    pico_enable_stdio_usb(frank-dino 1)
endif()

# ---------------------------------------------------------------------------
# Image
# ---------------------------------------------------------------------------
#
# XIP from flash, with the bus engine pinned in SRAM.
#
# This used to be copy_to_ram, and the reason was sound: the bus loop
# generates the Z80's clock edge by edge, and a flash fetch that misses
# the XIP cache in the middle of it stretches a clock phase by a
# microsecond. That is survivable -- the CPU is static -- but it puts a
# factor of two of jitter into the one number stage 0 exists to measure.
#
# The cure was worse than the disease. copy_to_ram puts the WHOLE image
# in SRAM: 98 KB of code and 123 KB of constants, none of which the bus
# loop ever executes, on a part with 512 KB where the guest alone wants
# 128. Every feature after the first few was then a question of what to
# take out.
#
# pico-speccy runs on the same chip and emulates the Z80 in software,
# and it is an ordinary XIP build -- it pins the handful of functions
# that matter with __not_in_flash_func() and leaves the rest in flash.
# This does the same. What is pinned: the bus engine, the memory and I/O
# handlers, the video scanline, the audio task, and the ROM images,
# which the engine reads on every opcode fetch.
#
# The check that this is right is selftest's measured Z80 clock and the
# pace floor it finds. If either moves, something on the path is still
# in flash.
pico_set_linker_script(frank-dino ${SRC_ROOT}/app/memmap.ld)

pico_set_program_name(frank-dino "frank-dino")
pico_set_program_version(frank-dino "${DINO_VERSION}")

target_compile_options(frank-dino PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)

target_link_options(frank-dino PRIVATE
        -Xlinker --print-memory-usage
        -Wl,--gc-sections)

pico_add_extra_outputs(frank-dino)
