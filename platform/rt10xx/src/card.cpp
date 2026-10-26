// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rt10xx's card and volume (OS-PORT-RT10XX phase 4, card.h): one uSDHC1 bus, the chip-neutral card logic and FatFs storage of
// platform/mk66 over it. The card is mounted on request only (the console's `sd mount`, later the app's start-up); the read-only storage is
// the hal::storage::Storage the app's loaders take, and the update path's writer (platform/mk66 update/fat_firmware_store.h) writes through
// the same mounted volume.
#include <o3os/platform/rt10xx/storage/card.h>
#include <o3os/platform/rt10xx/storage/usdhc_bus.h>

namespace o3os::platform::rt10xx::storage {
namespace {

namespace mstorage = o3os::platform::mk66::storage;

UsdhcBus g_bus;
mstorage::sd::Card g_card(g_bus);
mstorage::FatStorage g_fat(g_card);
bool g_tried = false;
hal::storage::Status g_last = hal::storage::Status::NotMounted;

}  // namespace

hal::storage::Status mount() noexcept {
  if (g_fat.mounted()) {
    g_fat.release();  // a re-mount: every handle of the old one is invalid from here
  }
  g_last = g_fat.mount();
  g_tried = true;
  return g_last;
}
bool mount_tried() noexcept { return g_tried; }
hal::storage::Status last_mount_status() noexcept { return g_last; }
hal::storage::Storage& storage() noexcept { return g_fat; }
mstorage::FatStorage& fat() noexcept { return g_fat; }
mstorage::sd::Card& card() noexcept { return g_card; }
std::uint32_t root_hz() noexcept { return g_bus.root_hz(); }

}  // namespace o3os::platform::rt10xx::storage
