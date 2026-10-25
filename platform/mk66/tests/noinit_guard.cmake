# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run by ctest (cmake -P) as `mk66_noinit_guard`: the noinit macros of sections.h accept what they should and refuse an
# initialised object at compile time. Inputs: CXX, FLAGS (joined with "|"), INCLUDE, DIR (the tests directory), WORK.
string(REPLACE "|" ";" flags "${FLAGS}")

function(compile source)
  execute_process(COMMAND ${CXX} ${flags} -I ${INCLUDE} -c ${DIR}/${source} -o ${WORK}/${source}.o
                  RESULT_VARIABLE status ERROR_VARIABLE err OUTPUT_VARIABLE out)
  set(COMPILE_STATUS "${status}" PARENT_SCOPE)
  set(COMPILE_OUTPUT "${out}${err}" PARENT_SCOPE)
endfunction()

compile(noinit_guard_ok.cpp)
if(NOT COMPILE_STATUS EQUAL 0)
  message(FATAL_ERROR "noinit_guard_ok.cpp must compile:\n${COMPILE_OUTPUT}")
endif()

foreach(bad noinit_guard_bad.cpp noinit_guard_bad_sram.cpp)
  compile(${bad})
  if(COMPILE_STATUS EQUAL 0)
    message(FATAL_ERROR "${bad} compiled: an initialiser on a noinit object would be dropped silently")
  endif()
  if(NOT COMPILE_OUTPUT MATCHES "only zero initializers are allowed in section")
    message(FATAL_ERROR "${bad} failed, but not for the expected reason:\n${COMPILE_OUTPUT}")
  endif()
endforeach()
message("noinit guard: accepted objects compile, initialised ones are refused")
