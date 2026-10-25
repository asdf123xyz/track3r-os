# SPDX-License-Identifier: GPL-3.0-or-later
# Rudolph Riedel's EmbeddedVideoEngine (FT800-FT813, MIT, tag 5.0.9), pinned and unmodified (THIRD_PARTY.md), used as a
# host-only test oracle (OS-007c): its register and command definitions, its EVE_init() order, its coprocessor fault recovery and
# its command-FIFO burst are run against the same FT812 model as the display driver's own code, and the two are compared
# (tests/eve_riedel_test.cpp). It is never part of an image: the driver in platform/mk66/display is written fresh, from the
# Programmers Guide, following this library's order with attribution.
#
# Built here as a C library with the test port (tests/support/eve_riedel_port/EVE_target.h, the library's own per-MCU target
# headers are not vendored) and the Tracker's panel through the library's EVE_CUSTOM_MODULE_H hook.
if(NOT O3OS_PLATFORM STREQUAL "host")
  return()
endif()
enable_language(C)

set(_port "${PROJECT_SOURCE_DIR}/tests/support/eve_riedel_port")
# The library's own code, vendored byte-identical, is not held to this project's warning policy (-Wall -Wextra -Werror ...): it is MIT
# C written for a hundred targets and the tests that use it are. -w keeps its noise out of the build log.
add_library(eve_riedel_oracle STATIC
  EVE_commands.c
  "${PROJECT_SOURCE_DIR}/tests/support/eve_riedel_oracle.c")
target_include_directories(eve_riedel_oracle PRIVATE "${_port}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(eve_riedel_oracle PRIVATE -w -std=gnu11)
target_compile_definitions(eve_riedel_oracle PRIVATE EVE_CUSTOM_MODULE_H="o3_panel.h")

# The library's own "EVE_RVT70" preset (no custom module header), read by one small translation unit, in a library of its own so
# that the two configurations never share a macro.
add_library(eve_riedel_rvt70 STATIC "${PROJECT_SOURCE_DIR}/tests/support/eve_riedel_rvt70.c")
target_include_directories(eve_riedel_rvt70 PRIVATE "${_port}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(eve_riedel_rvt70 PRIVATE -w -std=gnu11)
target_compile_definitions(eve_riedel_rvt70 PRIVATE EVE_RVT70)
target_link_libraries(eve_riedel_oracle PUBLIC eve_riedel_rvt70)
