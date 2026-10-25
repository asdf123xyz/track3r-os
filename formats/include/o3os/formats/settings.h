// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/formats/native.h>
#include <o3os/model/settings.h>

#include <cstddef>
#include <cstdint>

namespace o3os::formats::settings {

// The device settings file (OS-002d, docs/formats/o3-settings.md): an IFF-style container like the native project (`FORM` +
// `O3ST`, chunks `SETS` and `LRNB`, `CRC `), holding the settings of scope device (model/params.h, kind Device) and the device's
// MIDI Learn bindings. It is at most kMaxFileBytes (1,024 bytes; today 94 with no bindings and 362 with all 32), so unlike the project it is read and written whole from
// and to a buffer: the platform reads the file into 1 KiB, calls read(), and calls write() into 1 KiB to save. No file I/O, no heap,
// no state. A missing, truncated, damaged or newer-major file is never an error for the device: read() leaves the settings at their
// defaults, valid, and the caller notifies (ADR 0009: the SD, and a file on it, only adds or overrides, a bad one falls back).
//
// What an old reader does with a newer file (the version rules of docs/formats/o3-project.md section 3): a newer major is refused
// (NewerMajor), a newer minor is read as far as this version knows it (newer_minor), an entry for a parameter this version lacks
// is skipped and counted (skipped_entries), a chunk of another kind is ignored (unknown_chunks); what a file does not say stays
// at its default. Unlike the project file, unknown chunks are not kept: saving over a newer file drops them.

inline constexpr std::uint32_t kTypeO3st = native::fourcc('O', '3', 'S', 'T');
inline constexpr std::size_t kMaxFileBytes = 1024;

enum class Error : std::uint8_t {
  None,
  Truncated,   // fewer bytes than the FORM says (or than a FORM header)
  BadMagic,    // not an O3ST FORM
  BadCrc,      // the CRC chunk does not match
  NewerMajor,  // a chunk of a newer major version
  Malformed,   // a length that does not fit, a bad pad byte, a chunk twice, a missing or misplaced CRC chunk, a SETS or LRNB of the wrong shape
  TooBig,      // the FORM is longer than kMaxFileBytes
};

const char* error_name(Error e) noexcept;

struct Report {
  Error error = Error::None;
  const char* detail = "";             // static text: what was wrong, "" when nothing was
  std::uint32_t offset = 0;            // where in the file it was found
  std::uint32_t chunk = 0;             // the fourcc of the chunk being read
  std::uint32_t skipped_entries = 0;   // SETS and LRNB entries not taken (the default stays)
  std::uint32_t unknown_chunks = 0;    // chunks that are neither SETS, LRNB nor CRC: ignored
  bool newer_minor = false;            // a chunk of a newer minor version: fields it appends were not read
};

// Parses `size` bytes into `out`, which is first made the defaults of scope device (Settings::init) and stays so on any error.
Report read(const std::uint8_t* data, std::size_t size, model::Settings& out) noexcept;

// Writes the block (kind Device, valid) into `out` and returns the size in bytes; 0 when it is of another kind or invalid or does not fit
// `cap` (kMaxFileBytes always suffices). Deterministic: the same block gives the same bytes. SETS always, LRNB when a binding is in use.
std::size_t write(const model::Settings& s, std::uint8_t* out, std::size_t cap) noexcept;

// True when the first bytes look like this format (FORM and O3ST).
bool sniff(const std::uint8_t* head, std::size_t n) noexcept;

}  // namespace o3os::formats::settings
