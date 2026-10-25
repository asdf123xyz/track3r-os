# SPDX-License-Identifier: GPL-3.0-or-later
# platform/mk66/storage (OS-007e): the read-only SD card. The pure parts (the SD command set and the decoding of its responses, the SDHC
# register facts and the clock arithmetic, the timeout policy, the card's identification sequence and block reads over the SdBus seam, the
# FatFs-backed hal::storage::Storage, the text of `diag sd` and the `ls` pager) are added to `platform_mk66_pure`, so the host tests and the
# QEMU suite (tests/mk66_storage_*_test.cpp) run them against a software card; the SDHC register policy and the driver objects
# (src/sdhc_bus.cpp, src/device.cpp) are added to the image by the block of platform/mk66/CMakeLists.txt. FatFs itself is
# third_party/fatfs (read-only configuration), linked into the pure library: the host and QEMU tests read a FAT32 volume through it.
target_sources(platform_mk66_pure PRIVATE src/sd_protocol.cpp src/sd_card.cpp src/fat_storage.cpp src/sd_text.cpp src/ls_command.cpp src/card_swap.cpp)
target_include_directories(platform_mk66_pure PUBLIC include)
target_link_libraries(platform_mk66_pure PUBLIC o3os_fatfs)

# Every public header compiles on its own (the same check o3os_library() gives the layers).
file(GLOB_RECURSE _storage_headers CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")
set(_storage_stubs "")
foreach(header IN LISTS _storage_headers)
  string(MAKE_C_IDENTIFIER ${header} id)
  set(stub "${CMAKE_CURRENT_BINARY_DIR}/hdrcheck/${id}.cpp")
  file(CONFIGURE OUTPUT ${stub} CONTENT "#include <@header@>\n" @ONLY)
  list(APPEND _storage_stubs ${stub})
endforeach()
if(_storage_stubs)
  add_library(platform_mk66_storage_hdrcheck OBJECT ${_storage_stubs})
  target_link_libraries(platform_mk66_storage_hdrcheck PRIVATE platform_mk66_pure)
  o3os_strict_warnings(platform_mk66_storage_hdrcheck)
endif()
