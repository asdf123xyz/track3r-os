// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The rt10xx's SD card and its volume (OS-PORT-RT10XX phase 4): the uSDHC1 bus (usdhc_bus.h), the chip-neutral card logic and the FatFs
// storage of platform/mk66 (sd_card.h, fat_storage.h), one of each, static. Nothing is touched before mount(): the pads are muxed and the
// controller clocked by its first call (`sd mount` on the console). Implemented in the image only (src/card.cpp). Main loop only.

#include <o3os/hal/storage.h>
#include <o3os/platform/mk66/storage/fat_storage.h>
#include <o3os/platform/mk66/storage/sd_card.h>

#include <cstdint>

namespace o3os::platform::rt10xx::storage {

hal::storage::Status mount() noexcept;
bool mount_tried() noexcept;
hal::storage::Status last_mount_status() noexcept;
hal::storage::Storage& storage() noexcept;
o3os::platform::mk66::storage::FatStorage& fat() noexcept;
o3os::platform::mk66::storage::sd::Card& card() noexcept;
std::uint32_t root_hz() noexcept;

}  // namespace o3os::platform::rt10xx::storage
