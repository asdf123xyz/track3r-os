# SPDX-License-Identifier: GPL-3.0-or-later
# platform/mk66/input (OS-007d): buttons, pads, encoder and the pad LEDs. The pure parts (the key map, the TCA8418 protocol, the lockout
# debounce, the detent accumulator, the scan with its publishing policy, the IS31FL3731 driver) are added to `platform_mk66_pure`, so
# the host tests and the QEMU suite (tests/mk66_input_*_test.cpp) run them; the I2C master and the board's registers
# (src/i2c_master.cpp, src/device.cpp) are added to the image by the block of platform/mk66/CMakeLists.txt.
target_sources(platform_mk66_pure PRIVATE src/tca8418.cpp src/scanner.cpp src/pad_leds.cpp)
target_include_directories(platform_mk66_pure PUBLIC include)

# Every public header compiles on its own (the same check o3os_library() gives the layers).
file(GLOB_RECURSE _input_headers CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")
set(_input_stubs "")
foreach(header IN LISTS _input_headers)
  string(MAKE_C_IDENTIFIER ${header} id)
  set(stub "${CMAKE_CURRENT_BINARY_DIR}/hdrcheck/${id}.cpp")
  file(CONFIGURE OUTPUT ${stub} CONTENT "#include <@header@>\n" @ONLY)
  list(APPEND _input_stubs ${stub})
endforeach()
if(_input_stubs)
  add_library(platform_mk66_input_hdrcheck OBJECT ${_input_stubs})
  target_link_libraries(platform_mk66_input_hdrcheck PRIVATE platform_mk66_pure)
  o3os_strict_warnings(platform_mk66_input_hdrcheck)
endif()
