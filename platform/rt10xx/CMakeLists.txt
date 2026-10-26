# SPDX-License-Identifier: GPL-3.0-or-later
# platform/rt10xx (OS-PORT-RT10XX): the Tracker+ platform layer (NXP i.MX RT10xx, Cortex-M7) and its phase-1 probe image. Implements the
# HAL contracts as the phases land; nothing below `app` includes it. See README.md and docs/specs/OS-PORT-RT10XX.md.
#
# Two parts:
#   * the pure parts (memory map, FlexRAM and MPU words, clock values, interrupt map, image facts) as `platform_rt10xx_pure`, built on
#     every platform so that the host tests (tests/*_test.cpp here, globbed by tests/CMakeLists.txt) run them;
#   * with -DO3OS_PLATFORM=rt10xx, the image: `o3os.elf` linked with the generated rt10xx.ld, `o3os.hex` / `o3os.bin` / `o3os.ptf`
#     behind the static checks of the image (a failing image is deleted) and their ctests.

set(_rt10xx_tools "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set(_mk66_tools "${PROJECT_SOURCE_DIR}/platform/mk66/tools")
set(_rt10xx_header "${CMAKE_CURRENT_SOURCE_DIR}/include/o3os/platform/rt10xx/mem_map.h")
set(_rt10xx_template "${CMAKE_CURRENT_SOURCE_DIR}/rt10xx.ld.in")

# Headers only so far (the hardware halves are the image's sources below). The crash record's format and pure functions are the
# MK66's, reused as they are (platform.h says why): hence the dependency on platform_mk66_pure.
# Phase 4: the first source of the pure library, the Tracker+ firmware file's streaming validator (pure/: chip-neutral code, host-tested).
o3os_library(platform_rt10xx_pure SOURCES pure/ptf_validator.cpp DEPENDS hal platform_mk66_pure)
# Phase 2: the OTG1 device driver's pure half (usb/include: the chip's USB facts and the driver as a template over a register policy) joins
# the headers; its host test drives it with a fake controller (tests/rt10xx_usb_ci_test.cpp). The usb layer is what it is written against.
target_include_directories(platform_rt10xx_pure INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/usb/include)
target_link_libraries(platform_rt10xx_pure INTERFACE usb)

if(O3OS_PLATFORM STREQUAL "host" OR O3OS_PLATFORM STREQUAL "rt10xx")
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME rt10xx_make_ptf_selftest COMMAND ${Python3_EXECUTABLE} -I ${_rt10xx_tools}/make_ptf.py --self-test)
  add_test(NAME rt10xx_check_image_selftest COMMAND ${Python3_EXECUTABLE} -I ${_rt10xx_tools}/check_image.py --self-test)
  add_test(NAME rt10xx_ld_selftest COMMAND ${Python3_EXECUTABLE} -I ${_rt10xx_tools}/rt10xx_ld.py --self-test)
  # The template and the header agree (the script is generated in memory with gen_ld.py's parser and renderer, and verified by rt10xx_ld.py's
  # own checks of this chip's regions).
  add_test(NAME rt10xx_ld COMMAND ${Python3_EXECUTABLE} -I ${_rt10xx_tools}/rt10xx_ld.py check
           --header ${_rt10xx_header} --template ${_rt10xx_template} --gen-ld ${_mk66_tools}/gen_ld.py)
endif()

if(NOT O3OS_PLATFORM STREQUAL "rt10xx")
  o3os_label_tests()
  return()
endif()

# ---- The image -------------------------------------------------------------------------------------------------------------------
option(O3OS_RT10XX_FAULT_DRILL "Probe image: raise one deliberate UsageFault 10 s after power-up, to prove the crash record round trip on hardware (a separate image)" OFF)
option(O3OS_RT10XX_DCACHE "Image: enable the data cache after the SDRAM tests (the SDRAM and the flash are write-back cacheable; TCMs and OCRAM are not cached). OFF for an A/B on the unit" ON)
option(O3OS_RT10XX_SAFE_BOOT "Diagnostic image: keep the bootloader's clocks, no MPU, no SDRAM, data cache off, then USB; the skipped steps are console commands (`boot ...`)" OFF)
option(O3OS_RT10XX_AUDIO_AUTOSTART "Image: start the audio path (muted) at boot instead of on the console's `audio start`; an A/B image for a unit whose pads are known to be right" OFF)
option(O3OS_RT10XX_SDRAM_FULL_TEST "Image: the boot's SDRAM capacity test covers every word (several seconds) instead of every 256th; `diag sdram test full` runs it on request either way" OFF)

set(_rt10xx_ld "${CMAKE_CURRENT_BINARY_DIR}/rt10xx.ld")
add_custom_command(OUTPUT ${_rt10xx_ld}
  COMMAND ${Python3_EXECUTABLE} -I ${_rt10xx_tools}/rt10xx_ld.py generate --header ${_rt10xx_header} --template ${_rt10xx_template} --gen-ld ${_mk66_tools}/gen_ld.py --out ${_rt10xx_ld}
  DEPENDS ${_mk66_tools}/gen_ld.py ${_rt10xx_tools}/rt10xx_ld.py ${_rt10xx_template} ${_rt10xx_header}
  COMMENT "Generating rt10xx.ld from mem_map.h"
  VERBATIM)
add_custom_target(rt10xx_ld_script DEPENDS ${_rt10xx_ld})

add_executable(o3os src/startup.cpp src/boot.cpp src/sdram.cpp src/crash_store.cpp src/clock.cpp src/i2c.cpp src/audio.cpp src/midi_din.cpp src/usdhc_bus.cpp src/card.cpp src/display.cpp src/input.cpp src/firmware.cpp src/main.cpp)
# Phase 2: the OTG1 device's image half and the `diag` console (usb/src), with the build identity `version` prints (the MK66's generator,
# platform/mk66/tools/gen_build_info.cmake: git hash, "-dirty" if the tree differs), written only when it changed.
set(_rt10xx_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/gen")
add_custom_target(o3os_build_info
  COMMAND ${CMAKE_COMMAND} -DSRC=${PROJECT_SOURCE_DIR} -DOUT=${_rt10xx_gen_dir}/o3os_build_info.h -P ${_mk66_tools}/gen_build_info.cmake
  BYPRODUCTS ${_rt10xx_gen_dir}/o3os_build_info.h
  COMMENT "o3os_build_info.h (rt10xx)"
  VERBATIM)
add_dependencies(o3os o3os_build_info)
target_include_directories(o3os PRIVATE ${_rt10xx_gen_dir})
target_sources(o3os PRIVATE usb/src/usb1.cpp usb/src/console.cpp)
target_link_libraries(o3os PRIVATE platform_rt10xx_pure platform_mk66_pure usb audio hal core)
o3os_strict_warnings(o3os)
target_compile_definitions(o3os PRIVATE
  O3OS_RT10XX_FAULT_DRILL=$<BOOL:${O3OS_RT10XX_FAULT_DRILL}>
  O3OS_RT10XX_DCACHE=$<BOOL:${O3OS_RT10XX_DCACHE}>
  O3OS_RT10XX_SDRAM_FULL_TEST=$<BOOL:${O3OS_RT10XX_SDRAM_FULL_TEST}>
  O3OS_RT10XX_AUDIO_AUTOSTART=$<BOOL:${O3OS_RT10XX_AUDIO_AUTOSTART}>
  O3OS_RT10XX_SAFE_BOOT=$<BOOL:${O3OS_RT10XX_SAFE_BOOT}>)
# The reset sequence is the C runtime: its copy and zero loops must stay loops, not become memcpy / memset calls.
set_source_files_properties(src/startup.cpp PROPERTIES COMPILE_OPTIONS -fno-tree-loop-distribute-patterns)
set_target_properties(o3os PROPERTIES OUTPUT_NAME o3os SUFFIX .elf)
add_dependencies(o3os rt10xx_ld_script)
set_property(TARGET o3os APPEND PROPERTY LINK_DEPENDS ${_rt10xx_ld})
target_link_options(o3os PRIVATE
  -nostartfiles -T${_rt10xx_ld} --specs=nano.specs
  -Wl,--gc-sections -Wl,--orphan-handling=error -Wl,--print-memory-usage -Wl,-Map=$<TARGET_FILE:o3os>.map)

# HEX, BIN and the .ptf, behind the static checks. objcopy writes the ELF entry (Reset_Handler, Thumb) as the start-address record, as
# the official files carry one; --gap-fill 0xFF closes a hole the linker script already asserts against. The .ptf is made from the BIN
# by make_ptf.py (CRLF, 16-byte records, the sector/page end rule, the name rule) and checked like the HEX.
set(_rt10xx_hex "${CMAKE_CURRENT_BINARY_DIR}/o3os.hex")
set(_rt10xx_bin "${CMAKE_CURRENT_BINARY_DIR}/o3os.bin")
set(_rt10xx_ptf "${CMAKE_CURRENT_BINARY_DIR}/TrackerPlus_o3os.ptf")
set(_rt10xx_ramfunc_region "0x00000000:0x18000")
add_custom_command(OUTPUT ${_rt10xx_hex} ${_rt10xx_bin} ${_rt10xx_ptf}
  COMMAND ${CMAKE_SIZE} -A -d $<TARGET_FILE:o3os>
  COMMAND ${CMAKE_OBJCOPY} -O ihex --gap-fill 0xFF $<TARGET_FILE:o3os> ${_rt10xx_hex}
  COMMAND ${CMAKE_OBJCOPY} -O binary --gap-fill 0xFF $<TARGET_FILE:o3os> ${_rt10xx_bin}
  COMMAND ${Python3_EXECUTABLE} -I ${_rt10xx_tools}/check_image.py check ${_rt10xx_hex} --ramfunc ${_rt10xx_ramfunc_region} --delete-on-fail
          --json ${CMAKE_CURRENT_BINARY_DIR}/o3os.image-check.json
  COMMAND ${Python3_EXECUTABLE} -I ${_rt10xx_tools}/make_ptf.py ${_rt10xx_bin} ${_rt10xx_ptf}
  COMMAND ${Python3_EXECUTABLE} -I ${_rt10xx_tools}/check_image.py check ${_rt10xx_ptf} --ramfunc ${_rt10xx_ramfunc_region} --delete-on-fail
  DEPENDS o3os ${_rt10xx_tools}/check_image.py ${_rt10xx_tools}/make_ptf.py
  COMMENT "o3os.hex, o3os.bin, TrackerPlus_o3os.ptf and their static image checks"
  VERBATIM)
add_custom_target(o3os_image ALL DEPENDS ${_rt10xx_hex} ${_rt10xx_bin} ${_rt10xx_ptf})

# ---- ctest: the static checks of the built image -------------------------------------------------------------------------------
add_test(NAME rt10xx_ld_file COMMAND ${Python3_EXECUTABLE} -I ${_rt10xx_tools}/rt10xx_ld.py check
         --header ${_rt10xx_header} --template ${_rt10xx_template} --gen-ld ${_mk66_tools}/gen_ld.py --ld ${_rt10xx_ld})
add_test(NAME rt10xx_image COMMAND ${Python3_EXECUTABLE} -I ${_rt10xx_tools}/check_image.py check ${_rt10xx_hex} --ramfunc ${_rt10xx_ramfunc_region})
# Phase 3: the audio hot path in the ITCM calls nothing in flash (the MK66's checker reads the section, whatever the chip).
add_test(NAME rt10xx_ramfunc COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/check_ramfunc.py check $<TARGET_FILE:o3os> --objdump ${CMAKE_OBJDUMP})
add_test(NAME rt10xx_ptf COMMAND ${Python3_EXECUTABLE} -I ${_rt10xx_tools}/check_image.py check ${_rt10xx_ptf} --ramfunc ${_rt10xx_ramfunc_region} --expect-ptf)
set_tests_properties(rt10xx_ld_file rt10xx_image rt10xx_ptf rt10xx_ramfunc PROPERTIES TIMEOUT 60)

o3os_label_tests()
