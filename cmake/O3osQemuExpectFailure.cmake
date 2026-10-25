# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run by ctest (cmake -P) for a harness probe: runs the probe image on QEMU and passes only if it failed in the way
# the probe is meant to. Inputs: QEMU, QEMU_ARGS (joined with "|"), KERNEL, TIMEOUT, EXPECT (test_failure or
# stack_overflow).
#
# The status must be exactly the expected one and the output must carry the expected text: a QEMU that could not start,
# a different fault or a timeout would also be "non-zero", and must not make a probe pass.
if(EXPECT STREQUAL "test_failure")
  # What the runner returns for a failed doctest run.
  set(want_status 1)
  set(want_text "probe: deliberate failure")
  set(want_summary "test cases: +1 \\| +0 passed \\| +1 failed")
elseif(EXPECT STREQUAL "stack_overflow")
  # A touch of the MPU guard below the thread stack: reported by the fault handler (src/startup.cpp).
  set(want_status 3)
  set(want_text "probe: sparse frames")
  set(want_summary "o3os-qemu: FAULT stack overflow")
else()
  message(FATAL_ERROR "probe: unknown EXPECT '${EXPECT}'")
endif()

string(REPLACE "|" ";" qemu_args "${QEMU_ARGS}")
execute_process(
  COMMAND ${QEMU} ${qemu_args} -kernel ${KERNEL}
  RESULT_VARIABLE status
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
  TIMEOUT ${TIMEOUT})
set(all "${out}${err}")
message("${all}")
message("qemu exit status: ${status}")

if(NOT status MATCHES "^[0-9]+$")
  message(FATAL_ERROR "probe ${EXPECT}: QEMU did not exit normally (${status})")
elseif(status EQUAL 0)
  message(FATAL_ERROR "probe ${EXPECT}: the deliberate failure came out as exit status 0: failures do not propagate")
elseif(NOT status EQUAL want_status)
  message(FATAL_ERROR "probe ${EXPECT}: exit status ${status}, expected ${want_status}")
elseif(NOT all MATCHES "${want_summary}")
  message(FATAL_ERROR "probe ${EXPECT}: exit status ${want_status}, but the output lacks '${want_summary}'")
endif()
if(EXPECT STREQUAL "test_failure" AND NOT all MATCHES "${want_text}")
  message(FATAL_ERROR "probe ${EXPECT}: exit status ${want_status}, but not from the probe's failed test case")
endif()
message("probe ${EXPECT}: failed with exit status ${want_status}, as intended")
