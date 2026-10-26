// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Who owns what of the rt10xx's shared hardware (OS-PORT-RT10XX phase 3): the eDMA channels, the PIT channels, the render line. One table
// each, checked at compile time against the interrupt map, as platform/mk66 resources.h does for the MK66.

#include <o3os/platform/rt10xx/irq_list.h>
#include <o3os/platform/rt10xx/irq_map.h>

#include <cstdint>

namespace o3os::platform::rt10xx::resources {

enum class DmaOwner : std::uint8_t { Free, AudioTx, AudioRx, DisplayTx, DisplayRx, Card };

struct DmaRow {
  std::uint8_t channel;
  DmaOwner owner;
  std::uint8_t arbitration;  // DCHPRI: a permutation of 0..15 within the group, the audio channels highest
};

// The first group (channels 0..15); the second group is unused. The audio transmit channel is 0 (its line DMA0_DMA16 carries the
// AudioDma role in irq_map.h, as the official firmware's audio-out channel does: FINDINGS 12), the receive channel 1; the display's
// pair and the card's channel are phase 4's. The priorities: audio first, then the display, the rest in order.
inline constexpr DmaRow kDma[16] = {
    {0, DmaOwner::AudioTx, 15}, {1, DmaOwner::AudioRx, 14}, {2, DmaOwner::DisplayTx, 13}, {3, DmaOwner::DisplayRx, 12},
    {4, DmaOwner::Card, 11},    {5, DmaOwner::Free, 10},    {6, DmaOwner::Free, 9},        {7, DmaOwner::Free, 8},
    {8, DmaOwner::Free, 7},     {9, DmaOwner::Free, 6},     {10, DmaOwner::Free, 5},       {11, DmaOwner::Free, 4},
    {12, DmaOwner::Free, 3},    {13, DmaOwner::Free, 2},    {14, DmaOwner::Free, 1},       {15, DmaOwner::Free, 0},
};
inline constexpr unsigned kDmaChannelCount = sizeof(kDma) / sizeof(kDma[0]);
inline constexpr std::uint8_t kDmaTxChannel = 0;
inline constexpr std::uint8_t kDmaRxChannel = 1;

constexpr Irq dma_irq(std::uint8_t channel) noexcept { return static_cast<Irq>(channel % 16u); }  // DMAn_DMA16+n

constexpr bool dma_table_valid() noexcept {
  for (unsigned i = 0; i < kDmaChannelCount; ++i) {
    if (kDma[i].channel != i) {
      return false;
    }
    for (unsigned j = i + 1; j < kDmaChannelCount; ++j) {
      if (kDma[i].arbitration == kDma[j].arbitration) {
        return false;
      }
    }
  }
  return true;
}
static_assert(dma_table_valid(), "the DMA table lists channels 0..15 with distinct priorities");
static_assert(kDma[kDmaTxChannel].owner == DmaOwner::AudioTx && kDma[kDmaRxChannel].owner == DmaOwner::AudioRx, "the audio channels");
static_assert(kDma[kDmaTxChannel].arbitration == 15u && kDma[kDmaRxChannel].arbitration == 14u, "audio outranks everything else");
static_assert(role_of(dma_irq(kDmaTxChannel)) == hal::irq::Role::AudioDma, "the transmit channel's line is the slot-boundary interrupt");

// The PIT's four channels share one line (irq_map.h: MidiOutTimer); channel 0 is the MIDI-out emitter's.
enum class PitOwner : std::uint8_t { Free, MidiOutEmitter };
inline constexpr PitOwner kPit[4] = {PitOwner::MidiOutEmitter, PitOwner::Free, PitOwner::Free, PitOwner::Free};
inline constexpr unsigned kMidiPitChannel = 0;
static_assert(kPit[kMidiPitChannel] == PitOwner::MidiOutEmitter, "the emitter's PIT channel");
static_assert(role_of(Irq::PIT) == hal::irq::Role::MidiOutTimer, "the PIT line carries the MIDI-out row");

// The software-pended render line (irq_map.h).
inline constexpr Irq kRenderLineOfMap = o3os::platform::rt10xx::kRenderLine;

}  // namespace o3os::platform::rt10xx::resources
