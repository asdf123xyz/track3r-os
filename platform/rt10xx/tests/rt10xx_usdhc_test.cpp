// SPDX-License-Identifier: GPL-3.0-or-later
//
// The uSDHC1 facts (storage/usdhc_facts.h): the command and data words of every command the card logic sends, the clock root from the
// analog words, the divider plans, the pads.
#include <doctest/doctest.h>

#include <o3os/platform/rt10xx/storage/usdhc_facts.h>

namespace f = o3os::platform::rt10xx::storage::usdhc;
using Cmd = o3os::platform::mk66::storage::sd::Cmd;

namespace {
std::uint32_t field(std::uint32_t v, unsigned shift, unsigned bits) { return (v >> shift) & ((1u << bits) - 1u); }
}  // namespace

TEST_CASE("rt10xx usdhc: the command words") {
  CHECK(f::xfr_typ(Cmd::GoIdle) == 0u);
  const std::uint32_t cmd8 = f::xfr_typ(Cmd::SendIfCond);
  CHECK(field(cmd8, 24, 6) == 8u);
  CHECK(field(cmd8, 16, 2) == 2u);  // 48 bits
  CHECK((cmd8 & (f::kXferCccen | f::kXferCicen)) == (f::kXferCccen | f::kXferCicen));
  const std::uint32_t cid = f::xfr_typ(Cmd::AllSendCid);
  CHECK(field(cid, 16, 2) == 1u);  // 136 bits
  CHECK((cid & f::kXferCccen) != 0u);
  CHECK((cid & f::kXferCicen) == 0u);
  const std::uint32_t acmd41 = f::xfr_typ(Cmd::AcmdSdSendOpCond);
  CHECK(field(acmd41, 24, 6) == 41u);
  CHECK((acmd41 & (f::kXferCccen | f::kXferCicen)) == 0u);  // the OCR has no CRC and no index
  CHECK(field(f::xfr_typ(Cmd::SelectCard), 16, 2) == 3u);    // R1b
  CHECK(field(f::xfr_typ(Cmd::StopTransmission), 22, 2) == 3u);  // abort
  CHECK((f::xfr_typ(Cmd::ReadSingleBlock) & f::kXferDpsel) != 0u);
  CHECK((f::xfr_typ(Cmd::WriteBlock) & f::kXferDpsel) != 0u);
  CHECK((f::xfr_typ(Cmd::SendStatus) & f::kXferDpsel) == 0u);
  // The data bits are MIX_CTRL's, never the command word's (bits 5:0 of CMD_XFR_TYP stay 0).
  const Cmd data_cmds[] = {Cmd::ReadSingleBlock, Cmd::ReadMultipleBlock, Cmd::WriteBlock, Cmd::SwitchFunc};
  for (Cmd c : data_cmds) {
    CHECK((f::xfr_typ(c) & 0x3Fu) == 0u);
  }
}

TEST_CASE("rt10xx usdhc: the data bits") {
  CHECK(f::mix_ctrl(Cmd::ReadSingleBlock) == f::kMixDtdsel);
  CHECK(f::mix_ctrl(Cmd::SwitchFunc) == f::kMixDtdsel);
  CHECK(f::mix_ctrl(Cmd::ReadMultipleBlock) == (f::kMixDtdsel | f::kMixMsbsel | f::kMixBcen | f::kMixAc12en));
  CHECK(f::mix_ctrl(Cmd::WriteBlock) == 0u);  // a write: direction 0, one block
  CHECK(f::mix_ctrl(Cmd::GoIdle) == 0u);
  CHECK(f::blk_att(512, 8) == (512u | (8u << 16)));
}

TEST_CASE("rt10xx usdhc: the clock root and the plans") {
  CHECK(f::root_hz(f::kPllLock | 1u, 24u << f::kPfd2FracShift, 1) == 198'000'000u);
  CHECK(f::root_hz(f::kPllLock | 1u, 24u << f::kPfd2FracShift, 0) == 396'000'000u);
  CHECK(f::root_hz(f::kPllLock | 1u, 35u << f::kPfd2FracShift, 1) == static_cast<std::uint32_t>(528'000'000ull * 18u / 35u / 2u));
  CHECK(f::root_hz(f::kPllLock | f::kPllBypass, 24u << f::kPfd2FracShift, 1) == 0u);
  CHECK(f::root_hz(f::kPllLock | f::kPllPowerDown, 24u << f::kPfd2FracShift, 1) == 0u);
  CHECK(f::root_hz(f::kPllLock, 5u << f::kPfd2FracShift, 1) == 0u);  // a fraction outside the PFD's range
  const f::ClockPlan id = f::plan_clock(198'000'000u, 400'000u);
  CHECK(id.ok);
  CHECK(id.hz <= 400'000u);
  CHECK(id.hz > 300'000u);
  const f::ClockPlan hs = f::plan_clock(198'000'000u, 50'000'000u);
  CHECK(hs.sdclkfs == 0u);  // prescaler 1 (the smallest prescaler on a tie) ...
  CHECK(hs.dvs == 3u);      // ... and divisor 4
  CHECK(f::plan_clock(0, 400'000u).ok == false);
  const std::uint32_t sys = f::sys_clock_fields(hs);
  CHECK(field(sys, 4, 4) == 3u);
  CHECK(field(sys, 8, 8) == 0u);
  CHECK(field(sys, 16, 4) == 0xEu);
  CHECK((sys & 0xFu) == 0u);  // the reserved bits 3:0 are not written by the plan
}

TEST_CASE("rt10xx usdhc: the bus width, the watermarks, the pads") {
  CHECK(f::prot_bus_width(0, 4) == ((1u << 1) | (2u << 4)));
  CHECK(f::prot_bus_width(0xFFFFFFFFu, 1) == ((0xFFFFFFFFu & ~((3u << 1) | (3u << 4))) | (2u << 4)));
  CHECK(f::wtmk(0x08100810u) == ((0x08100810u & ~f::kWtmkMask) | 16u | (16u << 16)));
  CHECK(f::kData3.mux_ctl == f::kCmd.mux_ctl + 20u);
  CHECK(f::kBase == 0x402C0000u);
}
