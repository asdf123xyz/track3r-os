// SPDX-License-Identifier: GPL-3.0-or-later
//
// The FlexRAM bank word (flexram.h): decoded bank by bank, it gives the RAM map of mem_map.h.
#include <doctest/doctest.h>

#include <o3os/platform/rt10xx/flexram.h>

namespace flexram = o3os::platform::rt10xx::flexram;
namespace mem = o3os::platform::rt10xx::mem;

TEST_CASE("rt10xx flexram: the bank word decodes to 128 KiB ITCM, 352 KiB DTCM, 32 KiB OCRAM") {
  CHECK(flexram::kBankConfig == 0xAAAAABFDu);
  CHECK(flexram::kGpr16BankCfgSel == 4u);
  CHECK(flexram::banks_of(flexram::kBankConfig, flexram::Bank::Ocram) == 1u);
  CHECK(flexram::banks_of(flexram::kBankConfig, flexram::Bank::Itcm) == 4u);
  CHECK(flexram::banks_of(flexram::kBankConfig, flexram::Bank::Dtcm) == 11u);
  CHECK(flexram::banks_of(flexram::kBankConfig, flexram::Bank::Unused) == 0u);
  CHECK(flexram::bytes_of(flexram::kBankConfig, flexram::Bank::Itcm) == mem::kItcmSize);
  CHECK(flexram::bytes_of(flexram::kBankConfig, flexram::Bank::Dtcm) == mem::kDtcmSize);
  CHECK(flexram::bytes_of(flexram::kBankConfig, flexram::Bank::Ocram) == mem::kOcramSize);
}

TEST_CASE("rt10xx flexram: bank order (bank 0 OCRAM, 1..4 ITCM, 5..15 DTCM)") {
  CHECK(flexram::bank_of(flexram::kBankConfig, 0) == flexram::Bank::Ocram);
  for (unsigned i = 1; i <= 4; ++i) {
    CHECK(flexram::bank_of(flexram::kBankConfig, i) == flexram::Bank::Itcm);
  }
  for (unsigned i = 5; i < flexram::kBankCount; ++i) {
    CHECK(flexram::bank_of(flexram::kBankConfig, i) == flexram::Bank::Dtcm);
  }
  // Another word, to show the decoder is not the constant: the part's fuse-like default of 128/128/256.
  const std::uint32_t half = 0x55AAFFFFu;  // banks 0..7 ITCM, 8..11 DTCM, 12..15 OCRAM
  CHECK(flexram::bytes_of(half, flexram::Bank::Itcm) == 256u * 1024u);
  CHECK(flexram::bytes_of(half, flexram::Bank::Dtcm) == 128u * 1024u);
  CHECK(flexram::bytes_of(half, flexram::Bank::Ocram) == 128u * 1024u);
}
