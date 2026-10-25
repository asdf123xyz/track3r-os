# SPDX-License-Identifier: GPL-3.0-or-later
# platform/host: host fakes of the HAL contracts plus host utilities (SHA-256, PNG writer) and, since OS-007e, DirStorage: a host directory as the
# read-only SD card (hal/storage.h); OS-010a adds FakeStorage, the directory side of a card in memory (the gallery's card, the browser's tests); OS-E2E-1b adds DirCard, a scratch
# directory as a card with a write side (the simulator's `card` command), and ScratchDir.
# Standard library allowed; never linked by core..app.
o3os_library(platform_host SOURCES src/fakes.cpp src/sha256.cpp src/png.cpp src/stb_image_write_impl.cpp src/dir_storage.cpp src/dir_card.cpp src/fake_storage.cpp src/slow_card.cpp
  DEPENDS hal)

# stb_image_write.h is vendored third-party code (third_party/stb), found as <stb/stb_image_write.h> through a
# SYSTEM include directory. Its implementation lives in stb_image_write_impl.cpp, the only file built without the
# first-party warning flags (-w): stb is not written to them. Its output is checked by the PNG unit tests.
target_include_directories(platform_host SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party)
set_source_files_properties(src/stb_image_write_impl.cpp PROPERTIES COMPILE_OPTIONS "-w;-O2")  # -O2: its deflate is most of a PNG's cost

# The SHA-256 of a framebuffer and the PNG encoder run once per picture of every gallery and scenario: optimised even in the
# unoptimised sanitizer build, where at -O0 they cost about 0.4 s a picture and made the gallery tests take minutes.
set_source_files_properties(src/sha256.cpp src/png.cpp PROPERTIES COMPILE_OPTIONS "-O2")


# OS-QANET D1: the card on a FAT32 image file (FatFsStorage, FatImage), its own library: it needs platform_mk66_pure's FatFs.
add_subdirectory(fat)
