# SPDX-License-Identifier: GPL-3.0-or-later
# pt2-clone's Paula emulator (Olav Sorensen, BSD-3-Clause; THIRD_PARTY.md): pt2_paula.c, pt2_blep.c and pt2_rcfilters.c byte for byte, built for the HOST ONLY as the
# ORACLE of audio/paula.h (tests/paula_oracle_test.cpp, the dsp-port skill's gate 2). Never linked into an image. pt2_header.h and pt2_audio.h are ours (stubs).
# Compiled without warnings and without UBSan on purpose: it is the original's code.
add_library(pt2_paula_oracle STATIC pt2_paula.c pt2_blep.c pt2_rcfilters.c)
target_include_directories(pt2_paula_oracle PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(pt2_paula_oracle PRIVATE -w -O2 -fno-sanitize=undefined -fno-sanitize-recover=all)
target_compile_definitions(pt2_paula_oracle PUBLIC O3_PT2_PERIOD_TABLE="${CMAKE_CURRENT_SOURCE_DIR}/pt2_period_table.inc")
