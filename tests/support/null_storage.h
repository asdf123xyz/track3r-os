// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A card that is not there: every call that needs one says so (what hal::storage::Storage answers with no card in the slot). For the tests of what the OS does with no SD
// (OS-012d: the built-in modules). No heap, no std: it also runs on the target suites.
#include <o3os/hal/storage.h>

namespace o3os::test {

class NullStorage final : public hal::storage::Storage {
 public:
  NullStorage() = default;
  hal::storage::Status mount() noexcept override { return hal::storage::Status::NoCard; }
  bool mounted() const noexcept override { return false; }
  bool card_present() const noexcept override { return false; }
  hal::storage::VolumeInfo volume() const noexcept override { return {}; }
  hal::storage::Status exists(const char*, bool*) noexcept override { return hal::storage::Status::NotMounted; }
  hal::storage::Status open(const char*, hal::storage::FileHandle&) noexcept override { return hal::storage::Status::NotMounted; }
  hal::storage::Status read(hal::storage::FileHandle, void*, std::size_t, std::size_t&) noexcept override { return hal::storage::Status::BadHandle; }
  hal::storage::Status seek(hal::storage::FileHandle, std::uint32_t) noexcept override { return hal::storage::Status::BadHandle; }
  hal::storage::Status size(hal::storage::FileHandle, std::uint32_t&) const noexcept override { return hal::storage::Status::BadHandle; }
  void close(hal::storage::FileHandle&) noexcept override {}
  hal::storage::Status opendir(const char*, hal::storage::DirHandle&) noexcept override { return hal::storage::Status::NotMounted; }
  hal::storage::Status readdir(hal::storage::DirHandle, hal::storage::DirEntry&) noexcept override { return hal::storage::Status::BadHandle; }
  void closedir(hal::storage::DirHandle&) noexcept override {}
};

}  // namespace o3os::test
