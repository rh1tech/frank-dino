#
# frank-dino — an RP2350B acting as the whole machine around a real Z80
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-dino
# SPDX-License-Identifier: GPL-3.0-or-later
#
# audio — GP46 as a PWM DAC, fed by a ring core 1 fills in step with the Z80.

add_library(audio INTERFACE)

target_sources(audio INTERFACE ${CMAKE_CURRENT_LIST_DIR}/audio.c)

target_include_directories(audio INTERFACE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(audio INTERFACE
        dino_core
        hardware_pwm
        hardware_dma
        hardware_clocks)
