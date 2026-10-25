// SPDX-License-Identifier: GPL-3.0-or-later
//
// Probe: a stack overflow made of sparse frames, which is the case a handful of painted guard words misses. Each
// call reserves 1 KiB of stack and touches ONE byte of it; 80 calls need 80 KiB of a 60 KiB thread stack, so the
// frames walk through the guard below it and on towards the heap. Without the MPU guard this exits 0 with a
// corrupted heap; with it the first touch inside the guard is a MemManage fault, reported as "FAULT stack overflow"
// with exit status 3. Registered as ctest `qemu_probe_stack` (EXPECT_FAILURE stack_overflow), in no real suite.
#include <doctest/doctest.h>

#include <cstdint>

namespace {

[[gnu::noinline]] std::uint32_t descend(std::uint32_t depth, volatile std::uint32_t* sink) {
  volatile std::uint8_t frame[1024];
  const std::uint32_t slot = (depth * 37u) % sizeof frame;  // a different byte of each frame
  frame[slot] = static_cast<std::uint8_t>(depth);
  *sink = frame[slot];
  return depth == 0 ? 0u : descend(depth - 1, sink) + frame[slot];
}

}  // namespace

TEST_CASE("probe: sparse frames overflow the stack") {
  volatile std::uint32_t sink = 0;
  CHECK(descend(80, &sink) != 0xFFFFFFFFu);  // never reached in a passing guard; here only to use the result
}
