// SPDX-License-Identifier: GPL-3.0-or-later
//
// What only a target can show about the platform layer itself: the reset sequence (.data copy, .bss zero, static
// constructors), the FPU, the SysTick clock and the heap. Runs on QEMU as `qemu_platform`.
#include <doctest/doctest.h>

#include <o3os/core/tick.h>
#include <o3os/platform/qemu/clock.h>
#include <o3os/platform/qemu/runtime.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" void* _sbrk(std::ptrdiff_t increment);  // src/syscalls.cpp

namespace {

namespace qemu = o3os::platform::qemu;

// Initialised data: its value exists only in the load image, so Reset_Handler's copy has to place it.
std::uint32_t g_data_word = 0x1234ABCDu;
const char* g_data_pointer = "data in RAM, pointer from the load image";
std::uint8_t g_data_bytes[7] = {1, 2, 3, 4, 5, 6, 7};

// Zero-initialised data: .bss, which Reset_Handler has to clear. The array is larger than the stack paint guard.
std::uint32_t g_bss_words[512];
std::uint8_t g_bss_byte;

// Static constructors run before main(). The constructor reads a volatile, so the compiler cannot fold the object
// into constant data: its value exists only if .init_array was run.
volatile std::uint32_t g_seed = 0xC0FFEE00u;
struct Constructed {
  std::uint32_t value;
  Constructed() : value(g_seed + 0x42u) {}
};
Constructed g_constructed;

}  // namespace

TEST_CASE("startup: .data comes from the load image") {
  CHECK(g_data_word == 0x1234ABCDu);
  CHECK(std::strcmp(g_data_pointer, "data in RAM, pointer from the load image") == 0);
  for (std::uint8_t i = 0; i < 7; ++i) {
    CHECK(g_data_bytes[i] == i + 1);
  }
  // Writable, i.e. in RAM and not read-only.
  g_data_word = 1;
  CHECK(g_data_word == 1);
  g_data_word = 0x1234ABCDu;
}

TEST_CASE("startup: .bss is zero") {
  std::uint32_t any = g_bss_byte;
  for (const std::uint32_t word : g_bss_words) {
    any |= word;
  }
  CHECK(any == 0);
}

TEST_CASE("startup: static constructors ran before main") {
  CHECK(g_constructed.value == 0xC0FFEE42u);
}

TEST_CASE("startup: the FPU is enabled and single precision works") {
  // volatile: the compiler must not fold the arithmetic away; it runs as VFP instructions on the target.
  volatile float a = 1.5f;
  volatile float b = 2.25f;
  const float product = a * b;
  CHECK(product == 3.375f);
  CHECK(static_cast<std::int32_t>(product * 1000.0f) == 3375);
  volatile float zero = 0.0f;
  CHECK(1.0f / zero > 1.0e30f);  // IEEE infinity, no trap
}

TEST_CASE("startup: heap and stack are usable and measured") {
  const qemu::MemoryUse before = qemu::memory_use();
  CHECK(before.stack_intact);
  CHECK(before.stack_size == 60u * 1024u);  // 64 KiB of stack, 4 KiB of it for exceptions (mps2_an386.ld)
  CHECK(before.stack_peak > 0);
  CHECK(before.stack_peak < before.stack_size);

  // Growing the heap moves the _sbrk high-water mark.
  std::vector<std::uint8_t> block(8192, 0x5A);
  const qemu::MemoryUse after = qemu::memory_use();
  CHECK(after.heap_peak >= before.heap_peak);
  CHECK(after.heap_peak >= block.size());
  CHECK(block.front() == 0x5A);
  CHECK(block.back() == 0x5A);

  // Stack depth is seen: touch a few KiB of stack.
  volatile std::uint8_t big[4096];
  for (std::size_t i = 0; i < sizeof big; ++i) {
    big[i] = static_cast<std::uint8_t>(i);
  }
  CHECK(qemu::memory_use().stack_peak >= sizeof big);
}

TEST_CASE("heap: _sbrk keeps the break between the heap start and the stack guard") {
  void* const failed = reinterpret_cast<void*>(-1);
  void* const here = _sbrk(0);
  errno = 0;
  CHECK(_sbrk(-0x100000) == failed);  // below the start of the heap
  CHECK(errno == ENOMEM);
  errno = 0;
  CHECK(_sbrk(0x10000000) == failed);  // far past the guard
  CHECK(errno == ENOMEM);
  CHECK(_sbrk(0) == here);  // refused calls moved nothing
}

TEST_CASE("clock: SysTick counts milliseconds and microseconds") {
  qemu::SysTickClock& clock = qemu::system_clock();
  const std::uint64_t ms0 = clock.uptime_ms();
  const o3os::core::Tick us0 = clock.now();

  // Busy-wait for 20 ms of clock time. Bounded: a dead SysTick must fail the test, not hang it.
  std::uint64_t ms = ms0;
  std::uint32_t spins = 0;
  while (ms < ms0 + 20 && spins < 50'000'000u) {
    ms = clock.uptime_ms();
    ++spins;
  }
  CHECK(ms >= ms0 + 20);
  if (ms < ms0 + 20) {
    return;  // this tree builds without exceptions, so doctest's REQUIRE is unavailable
  }

  const o3os::core::Tick us1 = clock.now();
  const std::uint32_t elapsed_us = o3os::core::tick_elapsed(us1, us0);
  CHECK(elapsed_us >= 19000u);   // at least the 20 ms waited for, less one tick of slack
  CHECK(elapsed_us < 1000000u);  // and nowhere near a second

  // Both widths describe the same instant, within the millisecond the two reads straddle.
  const std::uint64_t ms_now = clock.uptime_ms();
  const o3os::core::Tick us_now = clock.now();
  const std::uint64_t ms_after = clock.uptime_ms();
  const std::uint32_t us_in_ms = us_now - static_cast<std::uint32_t>(ms_now * 1000u);
  CHECK((ms_after == ms_now || ms_after == ms_now + 1));
  CHECK(us_in_ms < 2000u);
}

TEST_CASE("clock: now() is monotonic and its microseconds fill one millisecond") {
  qemu::SysTickClock& clock = qemu::system_clock();
  o3os::core::Tick last = clock.now();
  std::uint32_t backwards = 0;
  std::uint32_t max_fraction = 0;  // largest microsecond count seen inside a millisecond
  const std::uint64_t until = clock.uptime_ms() + 20;
  std::uint64_t ms = clock.uptime_ms();
  while (ms < until) {
    const o3os::core::Tick t = clock.now();
    const std::uint64_t ms_after = clock.uptime_ms();
    if (o3os::core::tick_elapsed(t, last) > 0x80000000u) {
      ++backwards;
    }
    last = t;
    if (ms_after == ms) {  // both reads in the same millisecond: the difference is the fraction
      const std::uint32_t fraction = t - static_cast<std::uint32_t>(ms * 1000u);
      if (fraction > max_fraction) {
        max_fraction = fraction;
      }
    }
    ms = ms_after;
  }
  CHECK(backwards == 0);
  // The sub-millisecond scale (core clock / 1000 counts per ms, / 25 counts per us) is right when the fraction
  // climbs to just under 1000 before the millisecond ticks over; a wrong core clock constant shows here.
  CHECK(max_fraction >= 750u);
  CHECK(max_fraction <= 1000u);
}

TEST_CASE("clock: the hal::Clock contract is the one in use") {
  // Through the interface, as the rest of the OS sees it.
  const o3os::hal::Clock& clock = qemu::system_clock();
  CHECK(clock.uptime_ms() >= 20);  // the clock test above waited 20 ms
}
