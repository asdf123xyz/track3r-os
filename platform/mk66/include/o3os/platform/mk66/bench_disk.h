// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// `diag bench disk` (OS-USBDISK U1d, docs/specs/OS-USBDISK.md section 11 and 3.4 item 10): the figures that decide the MSC running mode before any of it is built, as pure data and text. No hardware,
// host-tested (tests/mk66_bench_disk_test.cpp). The measuring is src/bench.cpp (the card reads, the window) and usb/src/usb0.cpp (the pass timing, bench images only).
//
// WHAT IS MEASURED, AND WHY EACH NUMBER IS THERE
//   * `BENCH disk_rd1|rd8|rd32 <min> <calls> avg <cycles> max <cycles> (<x> KiB/s avg, <y> at min, <z> us per block avg)`: DWT cycles of one BlockDevice::read call of 1, 8 and 32 blocks, sequential from
//     block 0, render NOT held off (the wall time a copy would see; the max carries the interrupts). rd1 is what the MSC engine does today (one block per service(), CMD17); rd8 and rd32
//     are CMD18 (sd_policy.h kMaxBlocksPerCommand = 32). The spec's CMD25 trigger is "single-block rate under 300 KB/s" (F7); the read side of that comparison is here.
//   * `BENCH disk_wr1|wr8|wr32 ...`: the write arm (Q14, decided 2026-10-17). The same line as the reads, for one FatFs f_write of 1, 8 and 32 blocks INSIDE the pre-allocated scratch file
//     `o3-bench/disk-scratch.bin` (save/bench_scratch.h): the card sees consecutive CMD24 (the driver loops Card::write_blocks one block at a time; CMD25 is not on the allowlist), the card's
//     program busy included, and FatFs's own cluster walk (a FAT sector read at most once per 128 clusters). Only on a card the Tracker owns; otherwise ONE line
//     `BENCH disk_wr n/a: <why>` and nothing is written, nothing is even read under a computer. The arm never writes a raw LBA (F6 iii): the file is made, filled once and kept.
//     `BENCH disk_wr scratch ...` says whether the file was made now (the fill, once) or kept; `BENCH disk_wr driver ...` is what the driver does (the CMD25 record, spec 11.1).
//     max is NOT capped here (a write call is many milliseconds: CallStat, not the Stat of the CPU kernels).
//   * `DISK usb <ms> ms isr <n> entries ... cycles/packet <x> ...`: over a window (`bench disk reset` opens it), the USB interrupt's cycles (a sum the ISR keeps) divided by the bulk packets it moved,
//     its load in hundredths of a percent, and the worst single entry. SOF, CDC and MIDI tokens are in the sum: read an idle window first (nothing copying) and subtract it.
//   * `DISK pass <ms> ms passes <n> avg <cycles> max <cycles> (<us> us) hist a/b/c/d/e/f`: the main-loop stall of the MSC pump (usb0::disk_pass: the slot follow and up to 4 rounds of card I/O),
//     wall time with the interrupts in it, as a histogram of passes: < 25 us (nothing to do), < 100 us, < 1 ms, < 5 ms, < 50 ms, >= 50 ms. The max is the F7 release criterion.
//   * `DISK bound ...`: the driver's own worst case for one failing call (sd_policy.h), for the record next to the measured max.

#include <o3os/audio/diag.h>
#include <o3os/platform/mk66/bench_report.h>
#include <o3os/platform/mk66/storage/sd_policy.h>

#include <cstddef>
#include <cstdint>

namespace o3os::platform::mk66::bench::disk {

inline constexpr std::uint32_t kCyclesPerUs = 180;
inline constexpr std::uint32_t kCyclesPerMs = 180'000;
inline constexpr std::uint32_t kMaxWindowMs = 60'000;  // the ISR's cycle sum is 32 bits: it wraps after about 79 s at a saturated interrupt, so a window is read within 60 s

constexpr std::uint32_t us_of(std::uint32_t cycles) noexcept { return (cycles + kCyclesPerUs / 2u) / kCyclesPerUs; }

// KiB per second for `blocks` 512-byte blocks moved in `cycles`: blocks * 0.5 KiB * 180e6 / cycles. 32-bit: blocks <= 47 keeps blocks * 90e6 under 2^32 (a bigger count is refused with 0).
constexpr std::uint32_t kib_per_s(std::uint32_t blocks, std::uint32_t cycles) noexcept {
  return (cycles == 0u || blocks > 47u) ? 0u : (blocks * 90'000'000u) / cycles;
}

// ---- the pump pass -------------------------------------------------------------------------------------------------------------------------
// One entry per main-loop pass that reached the pump. Main loop only (the writer and the reader are the same context).
class PassStat {
 public:
  static constexpr unsigned kBuckets = 6;
  // Upper edges in cycles of the first five buckets: 25 us, 100 us, 1 ms, 5 ms, 50 ms. The last bucket is everything above.
  static constexpr std::uint32_t kEdge[kBuckets - 1] = {4'500u, 18'000u, 180'000u, 900'000u, 9'000'000u};

  void add(std::uint32_t cycles) noexcept {
    unsigned b = 0;
    while (b < kBuckets - 1u && cycles >= kEdge[b]) {
      ++b;
    }
    ++bucket_[b];
    ++n_;
    sum_ += cycles;
    max_ = cycles > max_ ? cycles : max_;
  }
  void reset() noexcept { *this = PassStat{}; }
  std::uint32_t count() const noexcept { return n_; }
  std::uint32_t max() const noexcept { return max_; }
  std::uint32_t avg() const noexcept { return n_ != 0u ? static_cast<std::uint32_t>((sum_ + n_ / 2u) / n_) : 0u; }
  std::uint32_t bucket(unsigned i) const noexcept { return i < kBuckets ? bucket_[i] : 0u; }

 private:
  std::uint32_t bucket_[kBuckets] = {};
  std::uint32_t n_ = 0;
  std::uint32_t max_ = 0;
  std::uint64_t sum_ = 0;
};

// ---- the USB window ------------------------------------------------------------------------------------------------------------------------
// The counters of usb::DeviceCounters that the window needs, as deltas since `bench disk reset` (the sums are modulo 2^32, so a subtraction is right while the window is under one wrap).
struct UsbWindow {
  std::uint32_t ms = 0;
  std::uint32_t entries = 0;     // USB interrupt entries
  std::uint32_t cycles = 0;      // the cycles inside them
  std::uint32_t in_packets = 0;  // bulk IN packets armed for the MSC
  std::uint32_t out_packets = 0;
  std::uint32_t last = 0;        // the cycles of the latest entry
  std::uint32_t max = 0;         // the worst entry since the window opened (the bench resets the driver's maximum with the window)
};

constexpr std::uint32_t usb_packets(const UsbWindow& w) noexcept { return w.in_packets + w.out_packets; }
// Cycles per MSC packet, counting everything the interrupt did in the window; 0 with no packet.
constexpr std::uint32_t usb_cycles_per_packet(const UsbWindow& w) noexcept { return usb_packets(w) != 0u ? w.cycles / usb_packets(w) : 0u; }
// The interrupt's share of the core in hundredths of a percent (1234 = 12.34 %); 0 for an empty window.
constexpr std::uint32_t usb_load_x100(const UsbWindow& w) noexcept { return w.ms != 0u && w.ms <= kMaxWindowMs ? w.cycles / (w.ms * (kCyclesPerMs / 10'000u)) : 0u; }

namespace detail {
inline void percent(audio::diag::Text& t, std::uint32_t x100) {
  t.dec(x100 / 100u).chr('.').chr(static_cast<char>('0' + (x100 / 10u) % 10u)).chr(static_cast<char>('0' + x100 % 10u)).chr('%');
}
}  // namespace detail

// Microseconds per block in tenths, from the average cycles of a call of `blocks_per_call` blocks (64-bit: a slow card's 32-block write is billions of cycles, and x10 wraps 32 bits at 2.4 s).
constexpr std::uint32_t us_x10_per_block(std::uint32_t cycles, std::uint32_t blocks_per_call) noexcept {
  return blocks_per_call != 0u ? static_cast<std::uint32_t>((static_cast<std::uint64_t>(cycles) * 10u) / (static_cast<std::uint64_t>(kCyclesPerUs) * blocks_per_call)) : 0u;
}

// The statistics of the write arm's calls: min, max and mean cycles, NOT capped (bench_report.h Stat caps at 1,000,000 cycles = 5.5 ms, a kernel's scale; one programmed block is 1-3 ms and a
// 32-block call tens of ms). The sum is 64-bit: 2^32 cycles is 24 s per call.
class CallStat {
 public:
  void add(std::uint32_t cycles) noexcept {
    min_ = cycles < min_ ? cycles : min_;
    max_ = cycles > max_ ? cycles : max_;
    sum_ += cycles;
    ++n_;
  }
  std::uint32_t count() const noexcept { return n_; }
  std::uint32_t min() const noexcept { return n_ != 0u ? min_ : 0u; }
  std::uint32_t max() const noexcept { return max_; }
  std::uint32_t avg() const noexcept { return n_ != 0u ? static_cast<std::uint32_t>((sum_ + n_ / 2u) / n_) : 0u; }

 private:
  std::uint32_t min_ = 0xFFFFFFFFu;
  std::uint32_t max_ = 0;
  std::uint64_t sum_ = 0;
  std::uint32_t n_ = 0;
};

// `BENCH disk_rd<n>|disk_wr<n> <min> <calls> avg <avg> max <max> (...)`: `s` (a Stat or a CallStat) is the statistics of one call of `blocks_per_call` blocks.
template <class S>
inline std::size_t format_timed(const char* name, const S& s, std::uint32_t blocks_per_call, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  t.str("BENCH ").str(name).chr(' ').dec(s.min()).chr(' ').dec(s.count()).str(" avg ").dec(s.avg()).str(" max ").dec(s.max());
  const std::uint32_t us10 = us_x10_per_block(s.avg(), blocks_per_call);
  t.str(" (").dec(kib_per_s(blocks_per_call, s.avg())).str(" KiB/s avg, ").dec(kib_per_s(blocks_per_call, s.min())).str(" at min, ").dec(us10 / 10u).chr('.').dec(us10 % 10u).str(" us per block avg)\n");
  return t.size();
}

inline std::size_t format_read(const char* name, const Stat& s, std::uint32_t blocks_per_call, char* out, std::size_t cap) noexcept {
  return format_timed(name, s, blocks_per_call, out, cap);
}
inline std::size_t format_write(const char* name, const CallStat& s, std::uint32_t blocks_per_call, char* out, std::size_t cap) noexcept {
  return format_timed(name, s, blocks_per_call, out, cap);
}

// `BENCH disk_wr filling scratch to <n> MiB (...)`: printed before the one-time fill, which holds the main loop for seconds on a slow card (nothing else is on the console until it ends).
inline std::size_t format_filling(std::uint32_t bytes, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  t.str("BENCH disk_wr filling scratch to ").dec(bytes >> 20).str(" MiB (first run, once: the main loop waits, a few seconds)\n");
  return t.size();
}

// `BENCH disk_wr scratch <path> <KiB> KiB created (filled <KiB> KiB in <ms> ms)` or `... kept`: the one-time fill, or the file that was there.
inline std::size_t format_scratch(const char* path, std::uint32_t kib, std::uint32_t filled_bytes, std::uint32_t fill_ms, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  t.str("BENCH disk_wr scratch ").str(path).chr(' ').dec(kib).str(" KiB ");
  if (filled_bytes != 0u) {
    t.str("created (filled ").dec(filled_bytes / 1024u).str(" KiB in ").dec(fill_ms).str(" ms, once)\n");
  } else {
    t.str("kept (bench disk clean removes it)\n");
  }
  return t.size();
}

// What the driver does for a multi-block write today (the CMD25 record, spec 11.1 item 1): the facts of storage/sd_protocol.h and Card::write_blocks, as text.
inline std::size_t format_write_driver(char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  t.str("BENCH disk_wr driver CMD24 per block (Card::write_blocks loops write_one; CMD25 and ACMD23 are not on the allowlist): wr8 and wr32 are 8 and 32 single-block programs\n");
  return t.size();
}

// `BENCH disk_clean ok ...` / `n/a: <why>`.
inline std::size_t format_clean(const char* path, const char* why, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  t.str("BENCH disk_clean ");
  if (why == nullptr) {
    t.str("ok (").str(path).str(" removed or not there)\n");
  } else {
    t.str("n/a: ").str(why).nl();
  }
  return t.size();
}

inline std::size_t format_usb(const UsbWindow& w, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  if (w.ms == 0u) {
    t.str("DISK usb n/a: no window (bench disk reset opens one)\n");
    return t.size();
  }
  if (w.ms > kMaxWindowMs) {
    t.str("DISK usb n/a: the window is over 60 s, the cycle sum may have wrapped (bench disk reset, read again)\n");
    return t.size();
  }
  t.str("DISK usb ").dec(w.ms).str(" ms entries ").dec(w.entries).str(" packets in ").dec(w.in_packets).str(" out ").dec(w.out_packets);
  t.str(" load ");
  detail::percent(t, usb_load_x100(w));
  t.str(" cycles/packet ").dec(usb_cycles_per_packet(w)).str(" cycles/entry ").dec(w.entries != 0u ? w.cycles / w.entries : 0u).str(" last ").dec(w.last).str(" max ").dec(w.max);
  t.str(" (").dec(us_of(w.max)).str(" us)\n");
  return t.size();
}

inline std::size_t format_pass(std::uint32_t window_ms, const PassStat& p, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  t.str("DISK pass ").dec(window_ms).str(" ms passes ").dec(p.count()).str(" avg ").dec(p.avg()).str(" max ").dec(p.max()).str(" (").dec(us_of(p.max())).str(" us) hist ");
  for (unsigned i = 0; i < PassStat::kBuckets; ++i) {
    t.dec(p.bucket(i)).chr(i + 1u < PassStat::kBuckets ? '/' : '\n');
  }
  return t.size();
}

// The driver's bounds for one failing call, in whole milliseconds, from the same constants the driver's deadlines are made of.
inline std::size_t format_bounds(char* out, std::size_t cap) noexcept {
  namespace pol = storage::sd::policy;
  audio::diag::Text t(out, cap);
  t.str("DISK bound write-block ").dec(pol::worst_write_block_us() / 1000u).str(" ms read-call ").dec(pol::worst_read_call_us() / 1000u).str(" ms (sd_policy.h: a card that fails, once)\n");
  return t.size();
}

}  // namespace o3os::platform::mk66::bench::disk
