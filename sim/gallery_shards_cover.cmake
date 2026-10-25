# SPDX-License-Identifier: GPL-3.0-or-later
#
# ctest helper: the gallery's shards (`o3sim --gallery <dir> --shard i/N --list`, nothing is drawn) together hold every golden key of
# the whole gallery (`--list` without a shard), each exactly once, and none is empty. A change of the shard logic or of N that drops or
# doubles a shot fails here without rendering anything.
#   cmake -DO3SIM=<o3sim> -DSHARDS=<N> -P gallery_shards_cover.cmake
function(list_keys out)  # remaining arguments: o3sim's own
  execute_process(COMMAND "${O3SIM}" --gallery "${CMAKE_CURRENT_LIST_DIR}/no-such-dir" --list ${ARGN}
                  RESULT_VARIABLE _code OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT "${_code}" STREQUAL "0")
    message(FATAL_ERROR "o3sim --list ${ARGN} exited ${_code}: ${_err}")
  endif()
  string(STRIP "${_out}" _out)
  string(REPLACE "\n" ";" _keys "${_out}")
  set(${out} "${_keys}" PARENT_SCOPE)
endfunction()

list_keys(_all)
list(LENGTH _all _total)
if(_total LESS 1)
  message(FATAL_ERROR "the gallery lists no shot")
endif()
set(_union "")
foreach(_i RANGE 1 ${SHARDS})
  list_keys(_part --shard ${_i}/${SHARDS})
  list(LENGTH _part _n)
  if(_n LESS 1)
    message(FATAL_ERROR "shard ${_i}/${SHARDS} is empty")
  endif()
  message(STATUS "shard ${_i}/${SHARDS}: ${_n} shots")
  list(APPEND _union ${_part})
endforeach()
list(LENGTH _union _count)
list(REMOVE_DUPLICATES _union)
list(LENGTH _union _distinct)
if(NOT _count EQUAL _total OR NOT _distinct EQUAL _total)
  message(FATAL_ERROR "the ${SHARDS} shards hold ${_count} shots (${_distinct} distinct), the gallery has ${_total}")
endif()
list(SORT _union)
list(SORT _all)
if(NOT "${_union}" STREQUAL "${_all}")
  message(FATAL_ERROR "the shards hold different shots than the gallery")
endif()
message(STATUS "the ${SHARDS} shards hold the ${_total} shots of the gallery, each once")
