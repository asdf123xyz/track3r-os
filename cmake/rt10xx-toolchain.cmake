# SPDX-License-Identifier: GPL-3.0-or-later
#
# Toolchain file of the rt10xx build (OS-PORT-RT10XX): the same arm-none-eabi toolchain as the MK66 and QEMU builds
# (cmake/arm-none-eabi-toolchain.cmake says how it is found), with the Cortex-M7 flags of the i.MX RT10xx of the Tracker+.
#
# -mfpu=fpv5-sp-d16, not fpv5-d16: the core has a double-precision FPU, but this tree's rule is "no double in hot paths"
# (CLAUDE.md), and a single-precision FPU keeps a stray double a visible library call (and a size cost) instead of a silent
# fast path that the MK66 build would not have. The flags are the one difference from the Cortex-M4F file.
include("${CMAKE_CURRENT_LIST_DIR}/arm-none-eabi-toolchain.cmake")
set(CMAKE_SYSTEM_PROCESSOR cortex-m7)
set(ARM_CPU_FLAGS "-mthumb -mcpu=cortex-m7 -mfpu=fpv5-sp-d16 -mfloat-abi=hard")
set(CMAKE_C_FLAGS_INIT "${ARM_CPU_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${ARM_CPU_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${ARM_CPU_FLAGS}")
