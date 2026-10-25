# SPDX-License-Identifier: GPL-3.0-or-later
#
# ctest helper: run a command and require an exact exit code and, optionally, a text on stderr.
#   cmake -DEXPECT=<code> [-DSTDERR=<regex>] -DCOMMAND=<program|arg|arg...> -P expect_exit.cmake
# The command's arguments are separated by '|' because a ';' would split a test argument into a list.
string(REPLACE "|" ";" _command "${COMMAND}")
execute_process(COMMAND ${_command} RESULT_VARIABLE _code OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT "${_code}" STREQUAL "${EXPECT}")
  message(FATAL_ERROR "expected exit code ${EXPECT}, got '${_code}'\nstdout: ${_out}\nstderr: ${_err}")
endif()
if(DEFINED STDERR AND NOT "${_err}" MATCHES "${STDERR}")
  message(FATAL_ERROR "stderr does not match '${STDERR}':\n${_err}")
endif()
message(STATUS "exit code ${_code} as expected; stderr: ${_err}")
