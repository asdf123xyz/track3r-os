# SPDX-License-Identifier: GPL-3.0-or-later
# formats: byte-stream codecs over core::ByteSource / ByteSink into the model. formats/native is the project
# format (docs/formats/o3-project.md, OS-004a); formats/og reads and writes the Tracker OG's own files
# (docs/formats/og.md, OS-004b); format_registry / builtin_formats are the Strategy + registry they plug into
# (ADR 0009 rev 1); wav_sample.cpp makes the WAV importer of formats/wav (its own library, OS-002c) a registered sample format.
# settings_chunks / settings_file (OS-002d): the SETS and LRNB chunks of settings, shared by the project format and the device settings file
# (docs/formats/o3-settings.md); iff_sample.cpp (OS-SAMPFMT): AIFF / AIFF-C and IFF 8SVX as one more registered sample format. Depends on core, model and formats_wav.
o3os_library(formats
  SOURCES src/crc32.cpp src/native_reader.cpp src/native_writer.cpp src/native_state.cpp
          src/format_registry.cpp src/builtin_formats.cpp src/wav_sample.cpp src/wt_surge.cpp src/iff_sample.cpp src/sample_probe.cpp
          src/settings_chunks.cpp src/settings_file.cpp src/volume_level.cpp
          src/og_common.cpp src/og_project.cpp src/og_pattern.cpp src/og_instrument.cpp src/og_pamd.cpp
  DEPENDS core model formats_wav)

# OS-008a: OctaMED / MED (MMD0-MMD3) reader, tempo conversion and flow linearisation (docs/formats/med.md, slices M0 and M1).
# Headers live in med/include (included as <o3os/formats/med/x.h>); the generic header check above covers formats/include only,
# so the med headers get their own stubs. builtin_formats.cpp registers "med" (OS-008b).
target_sources(formats PRIVATE med/src/mmd_reader.cpp med/src/med_tempo.cpp med/src/med_flow.cpp)
# OS-008b (slice M2): the mapping into the model (map.h), the importer job and its registered strategy (import.h).
target_sources(formats PRIVATE med/src/med_map.cpp med/src/med_import.cpp)
target_include_directories(formats PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/med/include)
file(GLOB_RECURSE _med_headers CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/med/include
     ${CMAKE_CURRENT_SOURCE_DIR}/med/include/*.h)
set(_med_stubs "")
foreach(header IN LISTS _med_headers)
  string(MAKE_C_IDENTIFIER ${header} id)
  set(stub "${CMAKE_CURRENT_BINARY_DIR}/hdrcheck/${id}.cpp")
  file(CONFIGURE OUTPUT ${stub} CONTENT "#include <@header@>\n" @ONLY)
  list(APPEND _med_stubs ${stub})
endforeach()
if(_med_stubs)
  add_library(formats_med_hdrcheck OBJECT ${_med_stubs})
  target_link_libraries(formats_med_hdrcheck PRIVATE formats)
  o3os_strict_warnings(formats_med_hdrcheck)
  target_compile_options(formats_med_hdrcheck PRIVATE -fsingle-precision-constant)
endif()

# OS-MODIMP: ProTracker MOD and Scream Tracker 3 S3M importers (docs/formats/mod.md): the reader (mod/module.h), the flow, the mapping and the
# importer job with its registered strategy. Headers live in mod/include (included as <o3os/formats/mod/x.h>) and get their own stubs below.
target_sources(formats PRIVATE mod/src/module.cpp mod/src/flow.cpp mod/src/map.cpp mod/src/import.cpp)
target_include_directories(formats PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/mod/include)
file(GLOB_RECURSE _mod_headers CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/mod/include
     ${CMAKE_CURRENT_SOURCE_DIR}/mod/include/*.h)
set(_mod_stubs "")
foreach(header IN LISTS _mod_headers)
  string(MAKE_C_IDENTIFIER ${header} id)
  set(stub "${CMAKE_CURRENT_BINARY_DIR}/hdrcheck/${id}.cpp")
  file(CONFIGURE OUTPUT ${stub} CONTENT "#include <@header@>\n" @ONLY)
  list(APPEND _mod_stubs ${stub})
endforeach()
if(_mod_stubs)
  add_library(formats_mod_hdrcheck OBJECT ${_mod_stubs})
  target_link_libraries(formats_mod_hdrcheck PRIVATE formats)
  o3os_strict_warnings(formats_mod_hdrcheck)
  target_compile_options(formats_mod_hdrcheck PRIVATE -fsingle-precision-constant)
endif()
