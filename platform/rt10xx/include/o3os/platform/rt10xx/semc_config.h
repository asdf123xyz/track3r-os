// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The SDRAM controller (SEMC) configuration of the rt10xx image (OS-PORT-RT10XX phase 2), as register words computed from the part's
// timings and the controller's clock, so the compiler and the host tests check the arithmetic and the boot code only writes the words.
// Source: the i.MX RT1050 Reference Manual, chapter "Smart External Memory Controller (SEMC)" (the register layout, the IP command
// protocol, the +1 rule of the timing fields) and chapter "IOMUXC" (the pads: every SEMC signal has exactly one pad, its ALT0 function).
//
// What is known of the board: 64 MiB of SDRAM answer at 0x8000_0000 (observed: Modular-Trackular FINDINGS 2 and 15). NOT known, and
// assumed here (docs/specs/OS-PORT-RT10XX.md, open research question 2): the part, its bus width and its timings. The assumption is the
// only 64 MiB arrangement a 16-bit SEMC port makes with one chip select: a 512 Mbit part organised 32 M x 16 (four banks, 13 row and 10
// column addresses; IS42S16320 / W9825G6-class), with the "-7" speed grade's timings taken conservatively. The register words follow from
// these numbers; a different part changes `kPart` and nothing else. The SEMC runs at 150 MHz, the core clock / 4 (clock_config.h), no
// second PLL involved.
//
// Nothing here has run on the unit. The boot step (src/sdram.cpp) records its outcome instead of halting, so the console can report it.

#include <o3os/platform/rt10xx/clock_config.h>
#include <o3os/platform/rt10xx/mem_map.h>

#include <cstdint>

namespace o3os::platform::rt10xx::semc {

// ---- The clock ----------------------------------------------------------------------------------------------------------------------
// CCM_CBCDR: SEMC_CLK_SEL (bit 6) = 0 takes the periph clock (the core's 600 MHz), SEMC_PODF (bits 18:16) divides it by the value + 1.
inline constexpr std::uint32_t kSemcPodf = 3u;
inline constexpr std::uint32_t kClockHz = clock::kCoreHz / (kSemcPodf + 1u);
// Nanoseconds to whole clock cycles, rounded up (exact integer arithmetic: ns x Hz / 1e9).
constexpr std::uint32_t cycles_of_ns(std::uint32_t ns) noexcept {
  return static_cast<std::uint32_t>((static_cast<std::uint64_t>(ns) * kClockHz + 999'999'999u) / 1'000'000'000u);
}
constexpr std::uint32_t ns_of_cycles(std::uint32_t cycles) noexcept {
  return static_cast<std::uint32_t>(static_cast<std::uint64_t>(cycles) * 1'000'000'000u / kClockHz);
}
static_assert(kClockHz == 150'000'000u, "SEMC at 150 MHz");
static_assert(cycles_of_ns(20) == 3u && cycles_of_ns(70) == 11u && cycles_of_ns(14) == 3u && cycles_of_ns(15) == 3u, "the rounding");

// ---- The part (the assumption) ------------------------------------------------------------------------------------------------------
struct Part {
  const char* name;
  std::uint32_t bytes;
  std::uint32_t data_bits;     // the port width: 16
  std::uint32_t column_bits;   // 10 for a 32 M x 16 part (13 rows x 10 columns x 4 banks x 16 bits)
  std::uint32_t row_bits;
  std::uint32_t banks;
  std::uint32_t cas_latency;   // 3 at 150 MHz
  std::uint32_t t_rp_ns;       // PRECHARGE to ACTIVE
  std::uint32_t t_rcd_ns;      // ACTIVE to READ/WRITE
  std::uint32_t t_rc_ns;       // ACTIVE to ACTIVE, same bank (REFRESH to ACTIVE too)
  std::uint32_t t_ras_ns;      // ACTIVE to PRECHARGE, minimum
  std::uint32_t t_wr_ns;       // WRITE recovery
  std::uint32_t t_rrd_ns;      // ACTIVE to ACTIVE, other bank
  std::uint32_t t_rfc_ns;      // REFRESH to REFRESH (auto refresh period)
  std::uint32_t t_xsr_ns;      // self-refresh exit to any command
  std::uint32_t t_cke_ns;      // CKE off minimum
  std::uint32_t refresh_rows;  // rows refreshed in one refresh interval
  std::uint32_t refresh_interval_ms;
};
inline constexpr Part kPart = {"32Mx16 SDR SDRAM, -7 grade (assumed)", 64u * mem::kMiB, 16, 10, 13, 4, 3,
                               20, 20, 70, 50, 15, 15, 70, 75, 15, 8192, 64};
static_assert(kPart.bytes == mem::kSdramSize, "the part is the SDRAM mem_map.h describes");
static_assert((1u << (kPart.row_bits + kPart.column_bits)) * kPart.banks * (kPart.data_bits / 8u) == kPart.bytes, "rows x columns x banks x width is the size");

// ---- The pads (RM "IOMUXC": GPIO_EMC_nn, ALT0 = the SEMC function) ----------------------------------------------------------------
// A 16-bit SDRAM on CS0 uses pads 0..39: DATA00..07 on EMC_00..07, DM00 on EMC_08, ADDR00..09 on EMC_09..18, ADDR11 on EMC_19, ADDR12
// on EMC_20, BA0 and BA1 on EMC_21 and EMC_22, ADDR10 on EMC_23, CAS, RAS, CLK, CKE, WE, CS0 on EMC_24..29, DATA08..15 on EMC_30..37,
// DM01 on EMC_38, DQS on EMC_39 (the read strobe loopback pad). EMC_40 (RDY) and EMC_41 (CSX00) are NAND/NOR signals: not touched.
inline constexpr std::uintptr_t kIomuxcBase = 0x401F8000u;
inline constexpr std::uintptr_t kMuxCtlEmc00 = kIomuxcBase + 0x014u;  // SW_MUX_CTL_PAD_GPIO_EMC_00, then one word per pad
inline constexpr std::uintptr_t kPadCtlEmc00 = kIomuxcBase + 0x204u;  // SW_PAD_CTL_PAD_GPIO_EMC_00, then one word per pad
inline constexpr unsigned kFirstPad = 0;
inline constexpr unsigned kPadCount = 40;  // EMC_00..EMC_39
inline constexpr std::uint32_t kMuxSemc = 0x0u;  // ALT0, SION off
// SW_PAD_CTL: SRE fast (bit 0), DSE 7 (bits 5:3, R0/7), SPEED 3 (bits 7:6, 200 MHz), PKE (bit 12), HYS (bit 16); no pull, not open drain.
inline constexpr std::uint32_t kPadWord = (1u << 0) | (7u << 3) | (3u << 6) | (1u << 12) | (1u << 16);
static_assert(kPadWord == 0x000110F9u, "the SEMC pad word");

// ---- The registers (RM "SEMC memory map") ---------------------------------------------------------------------------------------------
inline constexpr std::uintptr_t kBase = 0x402F0000u;
inline constexpr std::uintptr_t kMcr = kBase + 0x00u;
inline constexpr std::uintptr_t kIocr = kBase + 0x04u;
inline constexpr std::uintptr_t kBmcr0 = kBase + 0x08u;
inline constexpr std::uintptr_t kBmcr1 = kBase + 0x0Cu;
inline constexpr std::uintptr_t kBr0 = kBase + 0x10u;  // BR0..BR8 follow, one word each
inline constexpr std::uintptr_t kInten = kBase + 0x38u;
inline constexpr std::uintptr_t kIntr = kBase + 0x3Cu;
inline constexpr std::uintptr_t kSdramcr0 = kBase + 0x40u;
inline constexpr std::uintptr_t kSdramcr1 = kBase + 0x44u;
inline constexpr std::uintptr_t kSdramcr2 = kBase + 0x48u;
inline constexpr std::uintptr_t kSdramcr3 = kBase + 0x4Cu;
inline constexpr std::uintptr_t kIpcr0 = kBase + 0x90u;
inline constexpr std::uintptr_t kIpcr1 = kBase + 0x94u;
inline constexpr std::uintptr_t kIpcr2 = kBase + 0x98u;
inline constexpr std::uintptr_t kIpcmd = kBase + 0x9Cu;
inline constexpr std::uintptr_t kIptxdat = kBase + 0xA0u;
inline constexpr std::uintptr_t kIprxdat = kBase + 0xB0u;
inline constexpr std::uintptr_t kSts0 = kBase + 0xC0u;

// MCR: SWRST (bit 0), MDIS (bit 1), DQSMODE (bit 2: 1 = the read strobe looped back from the DQS pad, the vendor's choice for SDR SDRAM; 0 = internal),
// BTO (bits 20:16, bus timeout, 0x1F = the longest), CTO (bits 31:24, command timeout, 0 = none).
inline constexpr std::uint32_t kMcrSwrst = 1u << 0;
inline constexpr std::uint32_t kMcrMdis = 1u << 1;
inline constexpr std::uint32_t kMcrDqsModePad = 1u << 2;
inline constexpr std::uint32_t kMcrWord = kMcrDqsModePad | (0x1Fu << 16);
// BMCR0/1: the AXI bus arbitration weights (queue A: WQOS, WAGE, WSH, WRWS; queue B adds WPH and WBR). The vendor's defaults.
inline constexpr std::uint32_t kBmcr0Word = (5u << 0) | (8u << 4) | (0x40u << 8) | (0x10u << 16);
inline constexpr std::uint32_t kBmcr1Word = (5u << 0) | (8u << 4) | (0x60u << 8) | (0x24u << 16) | (0x40u << 24);
// IOCR: every mux at its SDRAM meaning (A8 is A8): 0.
inline constexpr std::uint32_t kIocrWord = 0u;
// BR0: the base address (bits 31:12), MS (bits 5:1: the size as log2(bytes / 4 KiB)), VLD (bit 0).
constexpr std::uint32_t memory_size_field(std::uint32_t bytes) noexcept {
  std::uint32_t n = 0;
  for (std::uint32_t v = bytes / 4096u; v > 1u; v >>= 1u) {
    ++n;
  }
  return n;
}
inline constexpr std::uint32_t kBr0Word = mem::kSdramBase | (memory_size_field(kPart.bytes) << 1) | 1u;
static_assert(memory_size_field(64u * mem::kMiB) == 14u && kBr0Word == 0x8000001Du, "BR0: 64 MiB at 0x8000_0000, valid");

// SDRAMCR0: PS (bit 0: 1 = 16-bit port), BL (bits 6:4: 0 = 1, 1 = 2, 2 = 4, 3 = 8), COL (bits 9:8: 12 minus the column bits), CL (bits 11:10: the CAS latency, 3 = 3).
inline constexpr std::uint32_t kBurstLength = 8u;
inline constexpr std::uint32_t kBurstField = 3u;
inline constexpr std::uint32_t kSdramcr0Word = 1u | (kBurstField << 4) | ((12u - kPart.column_bits) << 8) | (kPart.cas_latency << 10);
static_assert(kSdramcr0Word == 0x00000E31u, "CR0: 16-bit, burst 8, 10 columns, CL 3");

// The timing fields hold the time minus one cycle (RM: "the actual time is the field + 1").
constexpr std::uint32_t field_of_ns(std::uint32_t ns) noexcept { return cycles_of_ns(ns) - 1u; }
constexpr std::uint32_t field_of_cycles(std::uint32_t cycles) noexcept { return cycles - 1u; }
// SDRAMCR1: PRE2ACT (3:0) tRP, ACT2RW (7:4) tRCD, RFRC (12:8) tRC, WRC (15:13) tWR, CKEOFF (19:16) tCKE, ACT2PRE (23:20) tRAS.
inline constexpr std::uint32_t kSdramcr1Word = (field_of_ns(kPart.t_rp_ns) << 0) | (field_of_ns(kPart.t_rcd_ns) << 4) | (field_of_ns(kPart.t_rc_ns) << 8) |
                                               (field_of_ns(kPart.t_wr_ns) << 13) | (field_of_ns(kPart.t_cke_ns) << 16) | (field_of_ns(kPart.t_ras_ns) << 20);
// SDRAMCR2: SRRC (7:0) tXSR, REF2REF (15:8) tRFC, ACT2ACT (23:16) tRRD, ITO (31:24) the idle timeout (0: 256 x the prescaler).
inline constexpr std::uint32_t kSdramcr2Word = (field_of_ns(kPart.t_xsr_ns) << 0) | (field_of_ns(kPart.t_rfc_ns) << 8) | (field_of_ns(kPart.t_rrd_ns) << 16);
// SDRAMCR3: REN (bit 0), REBL (3:1, refreshes a burst minus one), PRESCALE (15:8: the prescaler's period in 16-cycle units), RT (23:16: the refresh
// timer in prescaler periods), UT (31:24: the urgent threshold in prescaler periods). One refresh every RT x PRESCALE x 16 cycles, which must
// stay under the part's interval / rows (64 ms / 8192 = 7.81 us).
inline constexpr std::uint32_t kPrescale = 10u;  // 160 cycles = 1.067 us
inline constexpr std::uint32_t kRefreshTimer = 7u;  // 7.47 us
inline constexpr std::uint32_t kUrgentThreshold = 7u;
constexpr std::uint32_t refresh_period_ns() noexcept { return ns_of_cycles(kRefreshTimer * kPrescale * 16u); }
constexpr std::uint32_t refresh_required_ns() noexcept { return kPart.refresh_interval_ms * 1'000'000u / kPart.refresh_rows; }
static_assert(refresh_period_ns() < refresh_required_ns(), "the refresh timer fires inside the part's interval per row");
static_assert(refresh_period_ns() * 10u > refresh_required_ns() * 8u, "and not absurdly often (within 20 % of it)");
inline constexpr std::uint32_t kSdramcr3Ren = 1u << 0;
inline constexpr std::uint32_t kSdramcr3Word = (0u << 1) | (kPrescale << 8) | (kRefreshTimer << 16) | (kUrgentThreshold << 24);
static_assert(kSdramcr3Word == 0x07070A00u, "CR3 without REN");

// The IP command port: IPCR0 the slave address (the SDRAM's base), IPCR1 the data size in bytes (the port width), IPCR2 the byte masks (none).
inline constexpr std::uint32_t kIpcr0Word = mem::kSdramBase;
inline constexpr std::uint32_t kIpcr1Word = kPart.data_bits / 8u;
inline constexpr std::uint32_t kIpcr2Word = 0u;
// IPCMD: the key in bits 31:16, the SDRAM command in bits 15:0.
inline constexpr std::uint32_t kIpcmdKey = 0xA55Au << 16;
inline constexpr std::uint32_t kCmdRead = 0x8u;
inline constexpr std::uint32_t kCmdWrite = 0x9u;
inline constexpr std::uint32_t kCmdModeSet = 0xAu;
inline constexpr std::uint32_t kCmdActive = 0xBu;
inline constexpr std::uint32_t kCmdAutoRefresh = 0xCu;
inline constexpr std::uint32_t kCmdSelfRefresh = 0xDu;
inline constexpr std::uint32_t kCmdPrecharge = 0xEu;
inline constexpr std::uint32_t kCmdPrechargeAll = 0xFu;
// INTR: IPCMDDONE (bit 0), IPCMDERR (bit 1), AXICMDERR (bit 2), AXIBUSERR (bit 3). Write 1 to clear.
inline constexpr std::uint32_t kIntrIpCmdDone = 1u << 0;
inline constexpr std::uint32_t kIntrIpCmdErr = 1u << 1;
inline constexpr std::uint32_t kIntrAll = 0x3Fu;
// STS0: IDLE (bit 0).
inline constexpr std::uint32_t kSts0Idle = 1u << 0;
// The mode register the MODESET command writes (JEDEC SDR SDRAM): burst length (bits 2:0, 3 = 8), sequential (bit 3 = 0), CAS latency (bits 6:4).
inline constexpr std::uint32_t kModeRegister = kBurstField | (kPart.cas_latency << 4);
static_assert(kModeRegister == 0x33u, "mode register: burst 8 sequential, CL 3");
// The part needs its clock stable for this long before the first command (the JEDEC 100 us, doubled), and two auto refreshes before the mode set.
inline constexpr std::uint32_t kPowerUpDelayUs = 200u;
inline constexpr unsigned kInitRefreshes = 2;

// ---- The tests (src/sdram.cpp runs them through mk66's memtest.h, which is chip-neutral) --------------------------------------------
// The wiring tests (data bus, byte lanes, address bus) touch a handful of words. The capacity test writes and reads back every word
// (two passes, 64 MiB: several seconds, uncached), so the boot runs it on every kSparseStride-th word by default and the whole array only
// with O3OS_RT10XX_SDRAM_FULL_TEST or on request (`diag sdram test`).
inline constexpr std::uint32_t kSparseStride = 256u;
inline constexpr std::uint32_t kFillSeed = 0x5EED5EEDu;

}  // namespace o3os::platform::rt10xx::semc
