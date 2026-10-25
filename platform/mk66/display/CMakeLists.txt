# SPDX-License-Identifier: GPL-3.0-or-later
# platform/mk66/display (OS-007c): the FT812 display driver. The pure parts (register facts, panel timing, SPI rates, the chip
# bring-up, the hal::Display with its staging and swap logic) are added to `platform_mk66_pure`, so the host tests and the
# QEMU suite (tests/mk66_display_*_test.cpp) run them; the DSPI2 / eDMA code that touches the hardware (src/dspi.cpp,
# src/edma.cpp, src/device.cpp) is added to the image by the block at the end of platform/mk66/CMakeLists.txt.
target_sources(platform_mk66_pure PRIVATE src/chip.cpp src/display.cpp)
target_include_directories(platform_mk66_pure PUBLIC include)

# Every public header of the display driver compiles on its own (same check o3os_library() gives the layers).
file(GLOB_RECURSE _display_headers CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")
set(_display_stubs "")
foreach(header IN LISTS _display_headers)
  string(MAKE_C_IDENTIFIER ${header} id)
  set(stub "${CMAKE_CURRENT_BINARY_DIR}/hdrcheck/${id}.cpp")
  file(CONFIGURE OUTPUT ${stub} CONTENT "#include <@header@>\n" @ONLY)
  list(APPEND _display_stubs ${stub})
endforeach()
if(_display_stubs)
  add_library(platform_mk66_display_hdrcheck OBJECT ${_display_stubs})
  target_link_libraries(platform_mk66_display_hdrcheck PRIVATE platform_mk66_pure)
  o3os_strict_warnings(platform_mk66_display_hdrcheck)
endif()
