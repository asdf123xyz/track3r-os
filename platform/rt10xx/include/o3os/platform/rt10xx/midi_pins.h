// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pads of the DIN MIDI port on the Tracker+ (OS-PORT-RT10XX phase 3): ASSUMED, not observed. That the jack is LPUART5 is an observed
// fact (Modular-Trackular FINDINGS section 12); which pads carry it is an open research question (docs/specs/OS-PORT-RT10XX.md section
// 2.5). LPUART5 has two pad pairs on the RT1052: GPIO_B1_12/13 (ALT1) and GPIO_EMC_23/24 (ALT2); the EMC pads are the SDRAM's address
// bus on every design with a SEMC SDRAM, so B1_12/13 is the assumption. If `midi start` on the unit sends nothing a MIDI monitor sees,
// this header is the first thing to change. Nothing here is driven at boot: `midi start` muxes the pads (midi_din.cpp).

#include <o3os/platform/rt10xx/audio_pins.h>

#include <cstdint>

namespace o3os::platform::rt10xx::midi_pins {

using audio_pins::Pad;

// The daisy-chain select registers of LPUART5 (RM "IOMUXC"): RX 0x540, TX 0x544; value 1 names the GPIO_B1 pads.
inline constexpr std::uintptr_t kLpuart5RxSelect = audio_pins::kIomuxc + 0x540u;
inline constexpr std::uintptr_t kLpuart5TxSelect = audio_pins::kIomuxc + 0x544u;

inline constexpr Pad kTx = {"GPIO_B1_12 LPUART5_TX", audio_pins::mux_of(audio_pins::kB1First + 12), audio_pins::pad_of(audio_pins::kB1First + 12), 1,
                            kLpuart5TxSelect, 1};
inline constexpr Pad kRx = {"GPIO_B1_13 LPUART5_RX", audio_pins::mux_of(audio_pins::kB1First + 13), audio_pins::pad_of(audio_pins::kB1First + 13), 1,
                            kLpuart5RxSelect, 1};
// SW_PAD_CTL. TX: fast slew (bit 0), DSE 6 (bits 5:3), SPEED 2 (bits 7:6). RX: the same drive fields, plus the 22 kOhm pull-up (PKE bit
// 12, PUE bit 13, PUS 3 at bits 15:14) and the hysteresis (HYS bit 16): a floating or noisy input is a stream of false start bits.
inline constexpr std::uint32_t kTxPadWord = (1u << 0) | (6u << 3) | (2u << 6);
inline constexpr std::uint32_t kRxPadWord = (1u << 0) | (6u << 3) | (2u << 6) | (1u << 12) | (1u << 13) | (3u << 14) | (1u << 16);

static_assert(kTx.mux_ctl == 0x401F81ACu && kRx.mux_ctl == 0x401F81B0u, "GPIO_B1_12 and GPIO_B1_13's mux registers (the RM's addresses)");
static_assert(kTx.pad_ctl == 0x401F839Cu && kRx.pad_ctl == 0x401F83A0u, "their pad registers");

}  // namespace o3os::platform::rt10xx::midi_pins
