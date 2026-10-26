// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The FlexRAM bank configuration of the Tracker+ (OS-PORT-RT10XX). The i.MX RT10xx's 512 KiB of on-chip RAM is sixteen 32 KiB banks,
// each of which IOMUXC_GPR_GPR17[FLEXRAM_BANK_CFG] assigns to the ITCM, the DTCM or the OCRAM (two bits a bank, bank 0 in bits 1:0), once
// IOMUXC_GPR_GPR16[FLEXRAM_BANK_CFG_SEL] says the register, not the fuses, decides (RT1050 RM, "FlexRAM" and "IOMUXC_GPR" chapters).
//
// The value is the one the official firmware's reset handler writes (FINDINGS section 2: GPR17 = 0xaaaaabfd, GPR16 |= 4), used here as a
// fact of the board (it is what the unit's RAM map is: 128 KiB ITCM, 352 KiB DTCM, 32 KiB OCRAM, mem_map.h). The decoder below is what
// the host tests and the static_asserts check the word against, so the map and the word cannot disagree.
//
// The reset sequence writes it before it touches the stack (the stack top 0x2005_8000 only exists once the DTCM has its 352 KiB; the
// fuse default of the part may give it less): src/startup.cpp, in the naked part of Reset_Handler.

#include <o3os/platform/rt10xx/mem_map.h>

#include <cstdint>

namespace o3os::platform::rt10xx::flexram {

inline constexpr std::uint32_t kBankBytes = 32u * mem::kKiB;
inline constexpr unsigned kBankCount = 16;

// GPR17[FLEXRAM_BANK_CFG], the word the unit runs with.
inline constexpr std::uint32_t kBankConfig = 0xAAAAABFDu;
// GPR16[FLEXRAM_BANK_CFG_SEL]: bit 2. Set after GPR17 is written.
inline constexpr std::uint32_t kGpr16BankCfgSel = 1u << 2;

enum class Bank : std::uint8_t { Unused = 0, Ocram = 1, Dtcm = 2, Itcm = 3 };

constexpr Bank bank_of(std::uint32_t config, unsigned bank) noexcept {
  return static_cast<Bank>((config >> (2u * bank)) & 0x3u);
}

constexpr unsigned banks_of(std::uint32_t config, Bank kind) noexcept {
  unsigned n = 0;
  for (unsigned i = 0; i < kBankCount; ++i) {
    if (bank_of(config, i) == kind) {
      ++n;
    }
  }
  return n;
}

constexpr std::uint32_t bytes_of(std::uint32_t config, Bank kind) noexcept { return banks_of(config, kind) * kBankBytes; }

static_assert(bytes_of(kBankConfig, Bank::Itcm) == mem::kItcmSize, "the bank word gives the ITCM mem_map.h says: 128 KiB (four banks)");
static_assert(bytes_of(kBankConfig, Bank::Dtcm) == mem::kDtcmSize, "the bank word gives the DTCM mem_map.h says: 352 KiB (eleven banks)");
static_assert(bytes_of(kBankConfig, Bank::Ocram) == mem::kOcramSize, "the bank word gives the OCRAM mem_map.h says: 32 KiB (one bank)");
static_assert(banks_of(kBankConfig, Bank::Unused) == 0u, "every bank is used");

}  // namespace o3os::platform::rt10xx::flexram
