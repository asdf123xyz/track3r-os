// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pads of the audio path and of the codec's control bus on the Tracker+ (OS-PORT-RT10XX phase 3): ASSUMED, not observed. The board's
// wiring of SAI1 and LPI2C is an open research question (docs/specs/OS-PORT-RT10XX.md section 2.5, question 3); nothing in the
// Modular-Trackular observations names a pad. What is here is the wiring every i.MX RT1052 + SGTL5000 design of the vendor's and the
// hobbyist ecosystem uses (the Teensy 4 audio shield's, with the receiver as the clock master): SAI1's MCLK, RX_BCLK, RX_SYNC and the two
// data lines on their only ALT3 pads, the codec on LPI2C1's usual pads. If the unit's `audio start` reports a clock fault or a codec that
// does not answer, this header is the first thing to change; `diag pins` reads the pads before anything drives them.
//
// Nothing here is driven at boot: the pads are muxed only by `audio start` (audio.cpp) and `i2c` bring-up, on the maintainer's command.

#include <cstdint>

namespace o3os::platform::rt10xx::audio_pins {

// A pad of the IOMUXC: its SW_MUX_CTL and SW_PAD_CTL register addresses, the ALT function, and the daisy-chain select register (0 when
// the signal is an output of the chip, or has one pad only) with the value that names this pad.
struct Pad {
  const char* name;
  std::uintptr_t mux_ctl;
  std::uintptr_t pad_ctl;
  std::uint32_t alt;
  std::uintptr_t select_input;
  std::uint32_t select_value;
};

// IOMUXC base and the register of each pad (RM "IOMUXC": the SW_MUX_CTL_PAD registers from 0x14 in the pad order EMC_00..41, AD_B0_00..15,
// AD_B1_00..15, B0_00..15, B1_00..15, SD_B0_00..05, SD_B1_00..11; the SW_PAD_CTL registers 0x1F0 further).
inline constexpr std::uintptr_t kIomuxc = 0x401F8000u;
constexpr std::uintptr_t mux_of(unsigned pad_index) noexcept { return kIomuxc + 0x014u + 4u * pad_index; }
constexpr std::uintptr_t pad_of(unsigned pad_index) noexcept { return kIomuxc + 0x204u + 4u * pad_index; }
inline constexpr unsigned kAdB1First = 42 + 16;  // AD_B1_00's index
inline constexpr unsigned kB1First = 42 + 32 + 16;  // B1_00's index
inline constexpr unsigned kAdB0First = 42;

// The daisy-chain select registers of SAI1 and LPI2C1 (RM "IOMUXC", the *_SELECT_INPUT registers).
inline constexpr std::uintptr_t kSai1RxBclkSelect = kIomuxc + 0x588u;
inline constexpr std::uintptr_t kSai1RxData0Select = kIomuxc + 0x58Cu;
inline constexpr std::uintptr_t kSai1RxSyncSelect = kIomuxc + 0x59Cu;
inline constexpr std::uintptr_t kLpi2c1SclSelect = kIomuxc + 0x4CCu;
inline constexpr std::uintptr_t kLpi2c1SdaSelect = kIomuxc + 0x4D0u;

// SAI1 (the assumption: the receiver is the clock master, the transmitter synchronous to it, as the vendor's board files have it).
inline constexpr Pad kMclk = {"GPIO_AD_B1_09 SAI1_MCLK", mux_of(kAdB1First + 9), pad_of(kAdB1First + 9), 3, 0, 0};
inline constexpr Pad kBclk = {"GPIO_AD_B1_11 SAI1_RX_BCLK", mux_of(kAdB1First + 11), pad_of(kAdB1First + 11), 3, kSai1RxBclkSelect, 1};
inline constexpr Pad kLrclk = {"GPIO_AD_B1_10 SAI1_RX_SYNC", mux_of(kAdB1First + 10), pad_of(kAdB1First + 10), 3, kSai1RxSyncSelect, 1};
inline constexpr Pad kTxData = {"GPIO_B1_01 SAI1_TX_DATA00", mux_of(kB1First + 1), pad_of(kB1First + 1), 3, 0, 0};
inline constexpr Pad kRxData = {"GPIO_B1_00 SAI1_RX_DATA00", mux_of(kB1First + 0), pad_of(kB1First + 0), 3, kSai1RxData0Select, 2};
// The pad word of the audio signals: fast slew, drive 6, 100 MHz speed, keeper.
inline constexpr std::uint32_t kAudioPadWord = (1u << 0) | (6u << 3) | (2u << 6) | (1u << 12);

// The codec's control bus: LPI2C1 on GPIO_AD_B1_00 (SCL) and GPIO_AD_B1_01 (SDA), ALT3, open drain with a 22 kOhm pull-up.
inline constexpr Pad kScl = {"GPIO_AD_B1_00 LPI2C1_SCL", mux_of(kAdB1First + 0), pad_of(kAdB1First + 0), 3, kLpi2c1SclSelect, 1};
inline constexpr Pad kSda = {"GPIO_AD_B1_01 LPI2C1_SDA", mux_of(kAdB1First + 1), pad_of(kAdB1First + 1), 3, kLpi2c1SdaSelect, 1};
// SW_PAD_CTL: ODE (bit 11), PKE (12), PUE (13), PUS 3 = 22k pull-up (bits 15:14), SPEED 2 (bits 7:6), DSE 6 (bits 5:3), SRE fast (bit 0).
inline constexpr std::uint32_t kI2cPadWord = (1u << 0) | (6u << 3) | (2u << 6) | (1u << 11) | (1u << 12) | (1u << 13) | (3u << 14);
// SW_MUX_CTL SION (bit 4): the input path stays on while the pad is an open-drain output, so the master sees the line it drives.
inline constexpr std::uint32_t kMuxSion = 1u << 4;
// The same two pads as GPIOs, for the bus recovery (i2c.cpp recover()): GPIO_AD_B1_00..15 are GPIO1_IO16..31 (ALT5); GPIO1 at 0x401B_8000
// (DR 0x00, GDIR 0x04, PSR 0x08). The GPIO block's clock is CCGR1 CG13 (gpio1), which the recovery opens itself (nothing at boot does).
inline constexpr std::uintptr_t kGpio1 = 0x401B8000u;
inline constexpr std::uintptr_t kGpio1Ccgr = 0x400FC06Cu;  // CCM_CCGR1
inline constexpr std::uint32_t kGpio1Gate = 0x3u << 26;
inline constexpr std::uintptr_t kGpioDr = 0x00u, kGpioGdir = 0x04u, kGpioPsr = 0x08u;
inline constexpr std::uint32_t kGpioAlt = 5;
inline constexpr unsigned kSclGpioBit = 16;
inline constexpr unsigned kSdaGpioBit = 17;
inline constexpr std::uint8_t kCodecI2cAddress = 0x0A;  // SGTL5000 with CTRL_ADR0_CS low (the MK66 board's address; assumed here too)

inline constexpr const Pad* kAudioPads[] = {&kMclk, &kBclk, &kLrclk, &kTxData, &kRxData};
inline constexpr unsigned kAudioPadCount = sizeof(kAudioPads) / sizeof(kAudioPads[0]);

static_assert(mux_of(0) == 0x401F8014u && pad_of(0) == 0x401F8204u, "EMC_00's registers");
static_assert(kMclk.mux_ctl == 0x401F8014u + 4u * (42u + 16u + 9u), "AD_B1_09's mux register");

}  // namespace o3os::platform::rt10xx::audio_pins
