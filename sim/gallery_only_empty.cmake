# SPDX-License-Identifier: GPL-3.0-or-later
#
# ctest helper: `o3sim --gallery <dir> --only ""` (an empty prefix, which would select everything by accident) is a usage error, exit 2.
#   cmake -DO3SIM=<o3sim> -DDIR=<dir> -P gallery_only_empty.cmake
execute_process(COMMAND "${O3SIM}" --gallery "${DIR}" --only "" RESULT_VARIABLE _code ERROR_VARIABLE _err)
if(NOT "${_code}" STREQUAL "2")
  message(FATAL_ERROR "an empty --only exited '${_code}', expected 2\nstderr: ${_err}")
endif()
if(NOT "${_err}" MATCHES "--only needs a non-empty")
  message(FATAL_ERROR "stderr does not name the empty prefix:\n${_err}")
endif()
message(STATUS "an empty --only is refused: ${_err}")
