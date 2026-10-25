// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/format.h>

#include <cstring>

namespace o3os::formats {

namespace {

char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool equal_nocase(const char* a, std::size_t na, const char* b, std::size_t nb) noexcept {
  if (na != nb) {
    return false;
  }
  for (std::size_t i = 0; i < na; ++i) {
    if (lower(a[i]) != lower(b[i])) {
      return false;
    }
  }
  return true;
}

// The last path component of `name` and, within it, the extension (from the last '.', '.' included; empty when
// there is none or when the dot starts the name, as in ".hidden").
void split_name(const char* name, const char*& base, std::size_t& base_len, const char*& ext,
                std::size_t& ext_len) noexcept {
  base = name;
  for (const char* p = name; *p != '\0'; ++p) {
    if (*p == '/' || *p == '\\') {
      base = p + 1;
    }
  }
  base_len = std::strlen(base);
  ext = base + base_len;
  ext_len = 0;
  for (std::size_t i = base_len; i > 1; --i) {
    if (base[i - 1] == '.') {
      ext = base + i - 1;
      ext_len = base_len - (i - 1);
      break;
    }
  }
}

// A macOS AppleDouble file ("._project.mt", the resource fork the Finder drops next to a copied file): never a project,
// whatever its name ends with and whatever its bytes look like.
bool is_apple_double(const char* file_name) noexcept {
  const char* base = file_name;
  for (const char* p = file_name; *p != '\0'; ++p) {
    if (*p == '/' || *p == '\\') {
      base = p + 1;
    }
  }
  return base[0] == '.' && base[1] == '_';
}

// Does the space-separated list `names` own this file (an extension token, or a whole-name token)?
bool owns(const char* names, const char* base, std::size_t base_len, const char* ext, std::size_t ext_len) noexcept {
  const char* p = names;
  while (*p != '\0') {
    while (*p == ' ') {
      ++p;
    }
    const char* start = p;
    while (*p != '\0' && *p != ' ') {
      ++p;
    }
    const std::size_t n = static_cast<std::size_t>(p - start);
    if (n == 0) {
      continue;
    }
    const bool match = start[0] == '.' ? (ext_len == n && equal_nocase(ext, ext_len, start, n))
                                       : equal_nocase(base, base_len, start, n);
    if (match) {
      return true;
    }
  }
  return false;
}

}  // namespace

const char* format_error_name(FormatError e) noexcept {
  switch (e) {
    case FormatError::None: return "none";
    case FormatError::Truncated: return "truncated";
    case FormatError::BadMagic: return "bad magic";
    case FormatError::BadCrc: return "bad CRC";
    case FormatError::UnsupportedVersion: return "unsupported version";
    case FormatError::Unsupported: return "unsupported content";
    case FormatError::LimitExceeded: return "limit exceeded";
    case FormatError::Malformed: return "malformed";
    case FormatError::SourceFailed: return "source failed";
    case FormatError::SinkFailed: return "sink failed";
    case FormatError::Modified: return "modified while writing";
    case FormatError::Internal: return "internal error";
    case FormatError::Cancelled: return "cancelled";
  }
  return "unknown";
}

bool FormatRegistry::add(const FormatDesc& desc) noexcept {
  if (count_ >= kMaxFormats || desc.id == nullptr || desc.id[0] == '\0' ||
      (desc.reader == nullptr && desc.writer == nullptr) || find_id(desc.id) != nullptr) {
    return false;
  }
  entries_[count_++] = desc;
  return true;
}

const FormatDesc* FormatRegistry::find_id(const char* id) const noexcept {
  if (id == nullptr) {
    return nullptr;
  }
  for (std::size_t i = 0; i < count_; ++i) {
    if (std::strcmp(entries_[i].id, id) == 0) {
      return &entries_[i];
    }
  }
  return nullptr;
}

const FormatDesc* FormatRegistry::find_name(const char* file_name) const noexcept {
  if (file_name == nullptr) {
    return nullptr;
  }
  const char* base;
  const char* ext;
  std::size_t base_len;
  std::size_t ext_len;
  split_name(file_name, base, base_len, ext, ext_len);
  if (base_len == 0 || is_apple_double(file_name)) {
    return nullptr;
  }
  for (std::size_t i = 0; i < count_; ++i) {
    if (owns(entries_[i].names, base, base_len, ext, ext_len)) {
      return &entries_[i];
    }
  }
  return nullptr;
}

const FormatDesc* FormatRegistry::sniff(const std::uint8_t* head, std::size_t n, std::uint32_t file_size) const noexcept {
  if (head == nullptr || n == 0) {
    return nullptr;
  }
  const std::size_t have = n < kSniffBytes ? n : kSniffBytes;
  for (std::size_t i = 0; i < count_; ++i) {
    if (entries_[i].sniff != nullptr && entries_[i].sniff(head, have, file_size)) {
      return &entries_[i];
    }
  }
  return nullptr;
}

const FormatDesc* FormatRegistry::identify(const char* file_name, const std::uint8_t* head, std::size_t n,
                                           std::uint32_t file_size) const noexcept {
  if (file_name != nullptr && is_apple_double(file_name)) {  // not even the bytes are asked
    return nullptr;
  }
  if (const FormatDesc* by_name = find_name(file_name)) {
    if (head == nullptr || n == 0 || by_name->sniff == nullptr) {
      return by_name;
    }
    const std::size_t have = n < kSniffBytes ? n : kSniffBytes;
    return by_name->sniff(head, have, file_size) ? by_name : nullptr;
  }
  return sniff(head, n, file_size);
}

}  // namespace o3os::formats
