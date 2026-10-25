// SPDX-License-Identifier: GPL-3.0-or-later
//
// `diag bench disk` (bench_disk.h; OS-USBDISK U1d): the pass histogram, the USB window arithmetic and the lines. Host and Cortex-M4F (`qemu_mk66`). The measuring (bench.cpp, usb0.cpp) needs the
// cycle counter, the card and a computer on the cable and is the unit's HIL item.
#include <doctest/doctest.h>

#include <o3os/platform/mk66/bench_disk.h>
#include <o3os/platform/mk66/bench_report.h>

#include <cstring>
#include <string>

namespace bench = o3os::platform::mk66::bench;
namespace disk = o3os::platform::mk66::bench::disk;

namespace {
std::string text(std::size_t n, const char* buf) { return std::string(buf, n); }
}  // namespace

TEST_CASE("bench disk: unit conversions") {
  CHECK(disk::us_of(180) == 1u);
  CHECK(disk::us_of(269) == 1u);
  CHECK(disk::us_of(270) == 2u);
  // 1 block in 8,000 cycles (44.4 us) is 512 B / 44.4 us = 11.25 MB/s = 11,250 KiB/s... in KiB: 0.5 KiB * 180e6 / 8000 = 11,250.
  CHECK(disk::kib_per_s(1, 8000) == 11250u);
  CHECK(disk::kib_per_s(32, 180'000) == 32u * 500u);  // 16 KiB in 1 ms
  CHECK(disk::kib_per_s(1, 0) == 0u);
  CHECK(disk::kib_per_s(48, 1000) == 0u);  // the 32-bit product would wrap: refused
  CHECK(disk::kib_per_s(47, 1'000'000) == 4230u);
  CHECK(disk::us_x10_per_block(8000, 1) == 444u);  // 44.4 us
  CHECK(disk::us_x10_per_block(8 * 9000, 8) == 500u);
  CHECK(disk::us_x10_per_block(100, 0) == 0u);
}

TEST_CASE("bench disk: the pass histogram puts each pass in the bucket of its cost, and keeps the max and the mean") {
  disk::PassStat p;
  CHECK(p.count() == 0u);
  CHECK(p.avg() == 0u);
  p.add(100);        // < 25 us
  p.add(4499);       // < 25 us
  p.add(4500);       // < 100 us (the edge belongs to the next bucket)
  p.add(17999);      // < 100 us
  p.add(18000);      // < 1 ms
  p.add(179999);     // < 1 ms
  p.add(180000);     // < 5 ms
  p.add(899999);     // < 5 ms
  p.add(900000);     // < 50 ms
  p.add(8999999);    // < 50 ms
  p.add(9000000);    // >= 50 ms
  p.add(0xFFFFFFFFu);  // a counter wrap in the last bucket, not a crash
  CHECK(p.count() == 12u);
  const std::uint32_t want[disk::PassStat::kBuckets] = {2, 2, 2, 2, 2, 2};
  for (unsigned i = 0; i < disk::PassStat::kBuckets; ++i) {
    INFO(i);
    CHECK(p.bucket(i) == want[i]);
  }
  CHECK(p.bucket(disk::PassStat::kBuckets) == 0u);
  CHECK(p.max() == 0xFFFFFFFFu);
  p.reset();
  CHECK(p.count() == 0u);
  CHECK(p.max() == 0u);
  for (unsigned i = 0; i < disk::PassStat::kBuckets; ++i) {
    CHECK(p.bucket(i) == 0u);
  }
  p.add(1000);
  p.add(3000);
  p.add(3001);
  CHECK(p.avg() == 2334u);  // (7001 + 1) / 3 rounded
  CHECK(p.max() == 3001u);
}

TEST_CASE("bench disk: the sum of a long window does not overflow the mean") {
  disk::PassStat p;
  for (unsigned i = 0; i < 3000; ++i) {
    p.add(200'000'000u);  // 3000 passes of 200M cycles: the sum is far over 2^32
  }
  CHECK(p.avg() == 200'000'000u);
}

TEST_CASE("bench disk: the USB window arithmetic") {
  disk::UsbWindow w;
  CHECK(disk::usb_cycles_per_packet(w) == 0u);  // no packet
  CHECK(disk::usb_load_x100(w) == 0u);           // no window
  w.ms = 1000;
  w.entries = 2500;
  w.cycles = 9'000'000;  // 5 % of 180e6
  w.in_packets = 300;
  w.out_packets = 300;
  CHECK(disk::usb_packets(w) == 600u);
  CHECK(disk::usb_cycles_per_packet(w) == 15'000u);
  CHECK(disk::usb_load_x100(w) == 500u);  // 5.00 %
  w.ms = 60'000;
  w.cycles = 4'000'000'000u;  // saturated, just under one wrap
  CHECK(disk::usb_load_x100(w) == 3703u);  // 37.03 %
  w.ms = 60'001;
  CHECK(disk::usb_load_x100(w) == 0u);  // over the wrap guard: no figure
}

TEST_CASE("bench disk: the lines") {
  char buf[300];
  bench::Stat s;
  s.add(8000);
  s.add(9000);
  s.add(10000);
  std::string line = text(disk::format_read("disk_rd1", s, 1, buf, sizeof buf), buf);
  CHECK(line == "BENCH disk_rd1 8000 3 avg 9000 max 10000 (10000 KiB/s avg, 11250 at min, 50.0 us per block avg)\n");
  bench::Stat s8;
  s8.add(8 * 6000);
  line = text(disk::format_read("disk_rd8", s8, 8, buf, sizeof buf), buf);
  CHECK(line == "BENCH disk_rd8 48000 1 avg 48000 max 48000 (15000 KiB/s avg, 15000 at min, 33.3 us per block avg)\n");

  // The write arm: a call of tens of milliseconds is not capped (the CPU kernels' Stat caps at 1,000,000 cycles = 5.5 ms), and the per-block time does not wrap 32 bits.
  disk::CallStat ws;
  ws.add(3'600'000);  // 20 ms for 8 blocks
  ws.add(4'500'000);
  ws.add(400'000'000);  // a 2.2 s stall: the max keeps it
  line = text(disk::format_write("disk_wr8", ws, 8, buf, sizeof buf), buf);
  CHECK(line == "BENCH disk_wr8 3600000 3 avg 136033333 max 400000000 (5 KiB/s avg, 200 at min, 94467.5 us per block avg)\n");
  CHECK(ws.max() == 400'000'000u);
  CHECK(disk::us_x10_per_block(2'000'000'000u, 1) == 111'111'111u);  // 11.1 s for one block: 64-bit inside

  line = text(disk::format_scratch("o3-bench/disk-scratch.bin", 1024, 1024u * 1024u, 4321, buf, sizeof buf), buf);
  CHECK(line == "BENCH disk_wr scratch o3-bench/disk-scratch.bin 1024 KiB created (filled 1024 KiB in 4321 ms, once)\n");
  line = text(disk::format_scratch("o3-bench/disk-scratch.bin", 1024, 0, 0, buf, sizeof buf), buf);
  CHECK(line == "BENCH disk_wr scratch o3-bench/disk-scratch.bin 1024 KiB kept (bench disk clean removes it)\n");
  line = text(disk::format_filling(1024u * 1024u, buf, sizeof buf), buf);
  CHECK(line == "BENCH disk_wr filling scratch to 1 MiB (first run, once: the main loop waits, a few seconds)\n");
  line = text(disk::format_write_driver(buf, sizeof buf), buf);
  CHECK(line.find("BENCH disk_wr driver CMD24 per block") == 0u);
  CHECK(line.find("CMD25") != std::string::npos);
  CHECK(line.back() == '\n');
  line = text(bench::format_na("disk_wr", "the card is write protected", buf, sizeof buf), buf);
  CHECK(line == "BENCH disk_wr n/a: the card is write protected\n");
  line = text(disk::format_clean("o3-bench/disk-scratch.bin", nullptr, buf, sizeof buf), buf);
  CHECK(line == "BENCH disk_clean ok (o3-bench/disk-scratch.bin removed or not there)\n");
  line = text(disk::format_clean("o3-bench/disk-scratch.bin", "the card is busy", buf, sizeof buf), buf);
  CHECK(line == "BENCH disk_clean n/a: the card is busy\n");

  disk::UsbWindow w;
  line = text(disk::format_usb(w, buf, sizeof buf), buf);
  CHECK(line == "DISK usb n/a: no window (bench disk reset opens one)\n");
  w.ms = 1000;
  w.entries = 2500;
  w.cycles = 9'000'000;
  w.in_packets = 300;
  w.out_packets = 300;
  w.last = 400;
  w.max = 1800;
  line = text(disk::format_usb(w, buf, sizeof buf), buf);
  CHECK(line == "DISK usb 1000 ms entries 2500 packets in 300 out 300 load 5.00% cycles/packet 15000 cycles/entry 3600 last 400 max 1800 (10 us)\n");
  w.ms = 61'000;
  line = text(disk::format_usb(w, buf, sizeof buf), buf);
  CHECK(line.find("n/a: the window is over 60 s") != std::string::npos);

  disk::PassStat p;
  p.add(1000);
  p.add(20000);
  p.add(2'000'000);
  line = text(disk::format_pass(5000, p, buf, sizeof buf), buf);
  CHECK(line == "DISK pass 5000 ms passes 3 avg 673667 max 2000000 (11111 us) hist 1/0/1/0/1/0\n");

  line = text(disk::format_bounds(buf, sizeof buf), buf);
  CHECK(line.find("DISK bound write-block 1280 ms read-call 474 ms") == 0u);
}

TEST_CASE("bench disk: a short buffer cuts a line, never overruns") {
  char buf[16];
  disk::PassStat p;
  p.add(1);
  const std::size_t n = disk::format_pass(10, p, buf, sizeof buf);
  CHECK(n <= sizeof buf);
}
