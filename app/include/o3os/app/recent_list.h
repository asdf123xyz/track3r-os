// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The RECENT projects (OS-FILES part B, docs/specs/OS-FILES-B.md section 4): the last five project folders opened or saved, newest first, kept in Settings/recent.o3rc so that the File
// screen's PROJECTS page can list them above the A-Z list. This is the list and its file; the card, the debounce of the write and the File screen are the save service's and the screen's.
//
// THE FILE. A new format, versioned from its first byte (nothing older exists, so there is nothing to convert), text so that a person can read and mend it on a computer:
//
//   line 1   "O3RC 1"            the magic and the version (LF, or CR LF, ends a line)
//   line 2.. one project folder name each, newest first, at most kRecentMax; a name is hal/save_guard.h valid_name() (what the save layer writes)
//
// The file is at most kMaxFileBytes (8 + 5 * 33, with CR LF line ends): a longer one is damaged. THE LOADER never fails and never touches a project: a missing file, a file that is empty, binary, longer than
// that, whose first line is not exactly "O3RC <version>" or whose version is not 1 (a newer file is not read as this one: an empty list), gives an EMPTY list; a line that is not a valid name,
// an empty line, a name seen already (FAT names are equal in any case) or a line past the fifth is skipped. The only thing a damaged file costs is the list. The version rule when the format
// changes: a number bump, a loader for the old one kept, and a round-trip test (tests/recent_file_test.cpp).
//
// A name on the list that is no longer on the card is not removed here: the File screen shows the rows it finds and the write after the next open, save or delete leaves the others out
// (drop() does not ask for a write by itself).
//
// Pure data: no heap, no I/O, no globals. Main loop only.

#include <o3os/hal/save_guard.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

class RecentList {
 public:
  static constexpr unsigned kMax = 5;
  static constexpr unsigned kVersion = 1;
  static constexpr std::size_t kNameBytes = hal::storage::save::kNameMax + 1;  // a name with its NUL
  static constexpr std::size_t kMaxFileBytes = 8 + kMax * (hal::storage::save::kNameMax + 2);  // "O3RC 1" and five names, each with CR LF

  void clear() noexcept;
  unsigned count() const noexcept { return count_; }
  // The i-th project, 0 the newest; "" outside the list.
  const char* name(unsigned i) const noexcept { return i < count_ ? names_[i] : ""; }
  bool contains(const char* name) const noexcept { return find(name) >= 0; }
  // Moves `name` to the front (or puts it there; the oldest leaves a full list). False, nothing changed, for a name the save layer would not write.
  bool add(const char* name) noexcept;
  // Takes `name` out. True when it was there.
  bool remove(const char* name) noexcept;
  // Takes `name` out without counting it as a change that wants a write (the file is rewritten by the next change).
  bool drop(const char* name) noexcept;
  // Moves on with every change that asks for a write (add, remove); the owner writes the file when it differs from the last one it wrote.
  std::uint32_t generation() const noexcept { return generation_; }

  // The list from the bytes of a file (see THE FILE): replaces the list, whatever it held. Returns how many names it took. Never reads past `size`.
  unsigned parse(const std::uint8_t* data, std::size_t size) noexcept;
  // The file for the list into `out` (`cap` bytes): its size, 0 when it does not fit (kMaxFileBytes is always enough).
  std::size_t write(std::uint8_t* out, std::size_t cap) const noexcept;

 private:
  int find(const char* name) const noexcept;
  void erase(unsigned index) noexcept;

  char names_[kMax][kNameBytes] = {};
  std::uint8_t count_ = 0;
  std::uint32_t generation_ = 0;
};

}  // namespace o3os::app
