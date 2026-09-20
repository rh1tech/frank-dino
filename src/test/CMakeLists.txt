#
# frank-dino — an RP2350B acting as the whole machine around a real Z80
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-dino
# SPDX-License-Identifier: GPL-3.0-or-later
#
# test — the stage-0 suite. It runs on the board, against the real CPU;
# there is no host-side half of this and there cannot be, because every
# assertion here is about a wire.

add_library(dino_test INTERFACE)

target_sources(dino_test INTERFACE ${CMAKE_CURRENT_LIST_DIR}/selftest.c)

target_include_directories(dino_test INTERFACE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(dino_test INTERFACE dino_core dino_ui)
