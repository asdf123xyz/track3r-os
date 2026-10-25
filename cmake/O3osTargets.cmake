# SPDX-License-Identifier: GPL-3.0-or-later
#
# Helpers shared by the layer directories.

# Strict warnings for first-party layers (core, hal, eve, ui, app, model, formats, platform, tests).
# PRIVATE on purpose: they never leak into consumers. sim/ and third_party/ may opt out, with a comment.
function(o3os_strict_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wshadow -Wdouble-promotion -Wconversion)
  if(O3OS_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()

# The layered libraries. Their code is held to the single-precision rule: besides the checker flagging
# unsuffixed floating literals (tools/check_layers.py), the compiler treats every unsuffixed floating
# constant as float, so a stray `x / 3.0` can never pull in the software double routines.
set(O3OS_RANKED_LAYERS core hal eve ui app model formats audio midi seq module)

# o3os_library(<name> [SOURCES <src>...] [DEPENDS <lib>...])
#
# One library per layer. Public headers live in <layer>/include/o3os/<layer>/ and are included as
# <o3os/<layer>/x.h>, so a layer sees another layer's headers only through a link dependency.
#   * STATIC when it has sources, INTERFACE (headers only) otherwise.
#   * Every public header is also compiled on its own into <name>_hdrcheck (self-containment).
#   * On mk66 the size of each archive is printed after it is built.
function(o3os_library name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPENDS" ${ARGN})
  set(inc "${CMAKE_CURRENT_SOURCE_DIR}/include")

  if(ARG_SOURCES)
    add_library(${name} STATIC ${ARG_SOURCES})
    set_target_properties(${name} PROPERTIES OUTPUT_NAME o3os_${name})
    target_include_directories(${name} PUBLIC ${inc})
    target_link_libraries(${name} PUBLIC ${ARG_DEPENDS})
    o3os_strict_warnings(${name})
    if(name IN_LIST O3OS_RANKED_LAYERS)
      target_compile_options(${name} PRIVATE -fsingle-precision-constant)
    endif()
    if(O3OS_PLATFORM STREQUAL "mk66")
      add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${CMAKE_SIZE} -t $<TARGET_FILE:${name}>
        VERBATIM)
    endif()
  else()
    add_library(${name} INTERFACE)
    target_include_directories(${name} INTERFACE ${inc})
    target_link_libraries(${name} INTERFACE ${ARG_DEPENDS})
  endif()

  file(GLOB_RECURSE headers CONFIGURE_DEPENDS RELATIVE ${inc} ${inc}/*.h)
  set(stubs "")
  foreach(header IN LISTS headers)
    string(MAKE_C_IDENTIFIER ${header} id)
    set(stub "${CMAKE_CURRENT_BINARY_DIR}/hdrcheck/${id}.cpp")
    file(CONFIGURE OUTPUT ${stub} CONTENT "#include <@header@>\n" @ONLY)
    list(APPEND stubs ${stub})
  endforeach()
  if(stubs)
    add_library(${name}_hdrcheck OBJECT ${stubs})
    target_link_libraries(${name}_hdrcheck PRIVATE ${name})
    o3os_strict_warnings(${name}_hdrcheck)
    if(name IN_LIST O3OS_RANKED_LAYERS)
      target_compile_options(${name}_hdrcheck PRIVATE -fsingle-precision-constant)
    endif()
  endif()
endfunction()
