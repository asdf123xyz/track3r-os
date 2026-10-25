# SPDX-License-Identifier: GPL-3.0-or-later
# platform/mk66/usb (OS-006b): the MK66's USB0 device driver and the `diag` console's platform half. The pure parts (the controller's
# register facts and clock arithmetic, the unique-ID serial number, the text of the platform's diag commands) are added to
# `platform_mk66_pure`, so the host tests and the QEMU suite (tests/mk66_usb_*_test.cpp) run them; the register policy, the driver
# objects, the interrupt handler and the command table (src/usb0.cpp, src/console.cpp) are added to the image by the block of
# platform/mk66/CMakeLists.txt. The device logic itself (control pipe, endpoints, rings, shell) is the usb library.
target_sources(platform_mk66_pure PRIVATE src/diag_text.cpp src/msc_bridge.cpp src/log_command.cpp src/mem_access.cpp src/hil_inject.cpp src/shot_stream.cpp src/screenshot_service.cpp)
target_include_directories(platform_mk66_pure PUBLIC include)
target_link_libraries(platform_mk66_pure PUBLIC usb)

# Every public header compiles on its own (the same check o3os_library() gives the layers).
file(GLOB_RECURSE _usb0_headers CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")
set(_usb0_stubs "")
foreach(header IN LISTS _usb0_headers)
  string(MAKE_C_IDENTIFIER ${header} id)
  set(stub "${CMAKE_CURRENT_BINARY_DIR}/hdrcheck/${id}.cpp")
  file(CONFIGURE OUTPUT ${stub} CONTENT "#include <@header@>\n" @ONLY)
  list(APPEND _usb0_stubs ${stub})
endforeach()
if(_usb0_stubs)
  add_library(platform_mk66_usb_hdrcheck OBJECT ${_usb0_stubs})
  target_link_libraries(platform_mk66_usb_hdrcheck PRIVATE platform_mk66_pure)
  o3os_strict_warnings(platform_mk66_usb_hdrcheck)
endif()
