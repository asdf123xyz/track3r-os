# SPDX-License-Identifier: GPL-3.0-or-later
# platform/mk66/save (OS-007n): the card's write side for saves (project, WAV, device settings). The FatFs-backed hal::storage::StorageWriter is pure and joins
# `platform_mk66_pure`, so the host tests and the QEMU suite run it against a software volume (tests/mk66_save_writer_test.cpp); the save job that drives it is
# app/atomic_save.h. bench_scratch.cpp (OS-USBDISK U1d, Q14) is the scratch file of
# `diag bench disk`: compiled everywhere for the tests, linked into an image only by the bench (O3OS_BENCH); the linker drops it from a release image.
target_sources(platform_mk66_pure PRIVATE src/fat_storage_writer.cpp src/bench_scratch.cpp)
target_include_directories(platform_mk66_pure PUBLIC include)

file(GLOB_RECURSE _save_headers CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")
set(_save_stubs "")
foreach(header IN LISTS _save_headers)
  string(MAKE_C_IDENTIFIER ${header} id)
  set(stub "${CMAKE_CURRENT_BINARY_DIR}/hdrcheck/${id}.cpp")
  file(CONFIGURE OUTPUT ${stub} CONTENT "#include <@header@>\n" @ONLY)
  list(APPEND _save_stubs ${stub})
endforeach()
if(_save_stubs)
  add_library(platform_mk66_save_hdrcheck OBJECT ${_save_stubs})
  target_link_libraries(platform_mk66_save_hdrcheck PRIVATE platform_mk66_pure)
  o3os_strict_warnings(platform_mk66_save_hdrcheck)
endif()
