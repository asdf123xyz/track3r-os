# SPDX-License-Identifier: GPL-3.0-or-later
# platform/mk66/power (OS-POWER P1): the shutdown that a hold of the Power switch starts (power_service.h) as a pure state machine over a PowerHost, so the host tests and the QEMU suite
# run the very code the image runs (tests/mk66_power_service_test.cpp). OS-POWER P2 adds the VLPR off's sequence and poll loop over an OffPlatform (power_off.h, tests/mk66_power_off_test.cpp) and the
# record of the last shutdown (power_record.h). Only src/power_host_mk66.cpp (the image's PowerHost and OffPlatform: the stop of the engine and the audio, the card, USB, the lights, the restart or the VLPR
# off, the registers) is the image's: the block of platform/mk66/CMakeLists.txt adds it to the executable.
target_sources(platform_mk66_pure PRIVATE src/power_service.cpp src/power_report.cpp src/power_off.cpp)
target_include_directories(platform_mk66_pure PUBLIC include)

# Every public header compiles on its own (the same check o3os_library() gives the layers).
file(GLOB_RECURSE _power_headers CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")
set(_power_stubs "")
foreach(header IN LISTS _power_headers)
  string(MAKE_C_IDENTIFIER ${header} id)
  set(stub "${CMAKE_CURRENT_BINARY_DIR}/hdrcheck/${id}.cpp")
  file(CONFIGURE OUTPUT ${stub} CONTENT "#include <@header@>\n" @ONLY)
  list(APPEND _power_stubs ${stub})
endforeach()
if(_power_stubs)
  add_library(platform_mk66_power_hdrcheck OBJECT ${_power_stubs})
  target_link_libraries(platform_mk66_power_hdrcheck PRIVATE platform_mk66_pure)
  o3os_strict_warnings(platform_mk66_power_hdrcheck)
endif()

o3os_label_tests()
