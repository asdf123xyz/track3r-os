// SPDX-License-Identifier: GPL-3.0-or-later
//
// The SEMC words (semc_config.h): the clock arithmetic, the part's geometry, the register words decoded field by field, the refresh timer
// inside the part's interval, the pads.
#include <doctest/doctest.h>

#include <o3os/platform/rt10xx/semc_config.h>

namespace semc = o3os::platform::rt10xx::semc;

namespace {
std::uint32_t field(std::uint32_t v, unsigned shift, unsigned bits) { return (v >> shift) & ((1u << bits) - 1u); }
}  // namespace

TEST_CASE("rt10xx semc: the clock") {
  CHECK(semc::kClockHz == 150'000'000u);
  CHECK(semc::kSemcPodf == 3u);
  CHECK(semc::cycles_of_ns(0) == 0u);
  CHECK(semc::cycles_of_ns(1) == 1u);
  CHECK(semc::cycles_of_ns(20) == 3u);
  CHECK(semc::cycles_of_ns(70) == 11u);
  CHECK(semc::ns_of_cycles(150) == 1000u);
}

TEST_CASE("rt10xx semc: the part and the base register") {
  CHECK(semc::kPart.bytes == 64u * 1024u * 1024u);
  CHECK(semc::kPart.data_bits == 16u);
  CHECK(semc::kPart.column_bits == 10u);
  CHECK(semc::kPart.row_bits == 13u);
  CHECK(semc::kPart.banks == 4u);
  CHECK(semc::kBr0Word == 0x8000001Du);
  CHECK(semc::memory_size_field(4096) == 0u);
  CHECK(semc::memory_size_field(8192) == 1u);
  CHECK(semc::memory_size_field(32u * 1024u * 1024u) == 13u);
}

TEST_CASE("rt10xx semc: SDRAMCR0..3 decoded") {
  CHECK(field(semc::kSdramcr0Word, 0, 1) == 1u);                           // 16-bit port
  CHECK(field(semc::kSdramcr0Word, 4, 3) == 3u);                           // burst 8
  CHECK(field(semc::kSdramcr0Word, 8, 2) == 2u);                           // 10 columns
  CHECK(field(semc::kSdramcr0Word, 10, 2) == 3u);                          // CL 3
  CHECK(field(semc::kSdramcr1Word, 0, 4) + 1u == semc::cycles_of_ns(semc::kPart.t_rp_ns));
  CHECK(field(semc::kSdramcr1Word, 4, 4) + 1u == semc::cycles_of_ns(semc::kPart.t_rcd_ns));
  CHECK(field(semc::kSdramcr1Word, 8, 5) + 1u == semc::cycles_of_ns(semc::kPart.t_rc_ns));
  CHECK(field(semc::kSdramcr1Word, 13, 3) + 1u == semc::cycles_of_ns(semc::kPart.t_wr_ns));
  CHECK(field(semc::kSdramcr1Word, 16, 4) + 1u == semc::cycles_of_ns(semc::kPart.t_cke_ns));
  CHECK(field(semc::kSdramcr1Word, 20, 4) + 1u == semc::cycles_of_ns(semc::kPart.t_ras_ns));
  CHECK(field(semc::kSdramcr2Word, 0, 8) + 1u == semc::cycles_of_ns(semc::kPart.t_xsr_ns));
  CHECK(field(semc::kSdramcr2Word, 8, 8) + 1u == semc::cycles_of_ns(semc::kPart.t_rfc_ns));
  CHECK(field(semc::kSdramcr2Word, 16, 8) + 1u == semc::cycles_of_ns(semc::kPart.t_rrd_ns));
  CHECK(field(semc::kSdramcr3Word, 0, 1) == 0u);                           // REN off in the word; the boot sets it last
  CHECK(field(semc::kSdramcr3Word, 8, 8) == semc::kPrescale);
  CHECK(field(semc::kSdramcr3Word, 16, 8) == semc::kRefreshTimer);
  CHECK(field(semc::kSdramcr3Word, 24, 8) == semc::kUrgentThreshold);
  // No field overflows its width: each timing fits (the compiler would silently wrap a shift into the neighbour).
  CHECK(semc::cycles_of_ns(semc::kPart.t_rc_ns) <= 32u);
  CHECK(semc::cycles_of_ns(semc::kPart.t_wr_ns) <= 8u);
  CHECK(semc::cycles_of_ns(semc::kPart.t_rp_ns) <= 16u);
  CHECK(semc::cycles_of_ns(semc::kPart.t_ras_ns) <= 16u);
}

TEST_CASE("rt10xx semc: the refresh timer and the mode register") {
  CHECK(semc::refresh_required_ns() == 7812u);
  CHECK(semc::refresh_period_ns() < semc::refresh_required_ns());
  CHECK(semc::refresh_period_ns() > 6000u);
  CHECK(semc::kModeRegister == 0x33u);
  CHECK(semc::kIpcmdKey == 0xA55A0000u);
  CHECK(semc::kInitRefreshes == 2u);
}

TEST_CASE("rt10xx semc: the pads") {
  CHECK(semc::kPadCount == 40u);
  CHECK(semc::kMuxCtlEmc00 == 0x401F8014u);
  CHECK(semc::kPadCtlEmc00 == 0x401F8204u);
  CHECK(semc::kPadWord == 0x000110F9u);
  CHECK(semc::kMuxSemc == 0u);
  CHECK(semc::kBase == 0x402F0000u);
}
