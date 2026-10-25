// SPDX-License-Identifier: GPL-3.0-or-later
//
// The probe: a suite whose only test case fails on purpose. It is registered as ctest `qemu_probe` with
// EXPECT_FAILURE, which passes only if QEMU ends with exit status 1 and the output names this failure. That proves
// a failing CHECK travels the whole way: doctest -> main's status -> exit() -> SYS_EXIT_EXTENDED -> QEMU's own exit
// status -> ctest. It is in no real suite, so it never fails a normal run.
#include <doctest/doctest.h>

TEST_CASE("probe: deliberate failure") {
  CHECK_MESSAGE(1 + 1 == 3, "this failure is the point of the probe");
}
