# SPDX-License-Identifier: GPL-3.0-or-later
#
# ctest helper: diff_list.txt belongs to a `--gallery ... --check` run. A run without --check removes one left by an
# earlier run (it would otherwise be read as this run's verdicts); a run with --check writes a fresh one.
#   cmake -DO3SIM=<o3sim> -DDIR=<scratch dir> -DGOLDENS=<goldens.txt> [-DONLY=<screen prefix> -DEXPECT_OK=<shot>] -P gallery_stale.cmake
# ONLY limits both runs to the screens whose name starts with it (--only), EXPECT_OK is a shot of them that the fresh list must hold as
# "ok gallery/<shot>" (default: pattern_typical, of the whole gallery). The test proves the same with a few pictures instead of 285.
if(DEFINED ONLY)
  set(_only --only "${ONLY}")
else()
  set(_only "")
endif()
if(NOT DEFINED EXPECT_OK)
  set(EXPECT_OK pattern_typical)
endif()
file(REMOVE_RECURSE "${DIR}")
file(MAKE_DIRECTORY "${DIR}")
file(WRITE "${DIR}/diff_list.txt" "a stale list from an earlier run\n")

# The run without --check renders one theme (--theme cannot go with --check, which needs them all): the stale list's removal
# does not depend on how many pictures there are, and a full run under the sanitizers takes about 20 s.
execute_process(COMMAND "${O3SIM}" --gallery "${DIR}" --theme amiga ${_only} RESULT_VARIABLE _code OUTPUT_QUIET ERROR_VARIABLE _err)
if(NOT "${_code}" STREQUAL "0")
  message(FATAL_ERROR "gallery without --check exited ${_code}: ${_err}")
endif()
if(EXISTS "${DIR}/diff_list.txt")
  message(FATAL_ERROR "a gallery run without --check left the stale diff_list.txt")
endif()
if(NOT EXISTS "${DIR}/contact_sheet.png")
  message(FATAL_ERROR "the gallery wrote no contact sheet")
endif()

execute_process(COMMAND "${O3SIM}" --gallery "${DIR}" --goldens "${GOLDENS}" --check ${_only}
                RESULT_VARIABLE _code OUTPUT_QUIET ERROR_VARIABLE _err)
if(NOT "${_code}" STREQUAL "0")
  message(FATAL_ERROR "gallery --check exited ${_code}: ${_err}")
endif()
file(READ "${DIR}/diff_list.txt" _list)
if(NOT "${_list}" MATCHES "ok gallery/${EXPECT_OK}")
  message(FATAL_ERROR "gallery --check wrote no fresh diff_list.txt: '${_list}'")
endif()
message(STATUS "diff_list.txt follows --check as expected")
