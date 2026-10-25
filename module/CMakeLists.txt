# SPDX-License-Identifier: GPL-3.0-or-later
# module: the loader of native modules from the SD card (ADR 0002, OS-012a): the file format's checks, the relocation of the image
# into two places (hot code in the SRAM_L slab, the rest in SDRAM), the stepped file reader and the set of loaded modules. The SDK
# header (sdk/o3module.h, MIT) is the contract with the modules themselves. Depends on core and hal (the storage contract); the audio
# engine runs the loaded modules (audio/module_voices.h) and depends on this; the layer does not know the engine. No heap, no double.
o3os_library(module
  SOURCES
    src/image.cpp
    src/loader.cpp
    src/view.cpp
  DEPENDS core hal)
target_include_directories(module PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
