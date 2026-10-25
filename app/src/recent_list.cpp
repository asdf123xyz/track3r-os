// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/recent_list.h>

#include <cstring>

namespace o3os::app {

namespace {

namespace guard = hal::storage::save;

constexpr char kMagic[] = "O3RC";

bool same_name(const char* a, const char* b) noexcept {
  for (;; ++a, ++b) {
    if (guard::detail::lower(*a) != guard::detail::lower(*b)) {
      return false;
    }
    if (*a == '\0') {
      return true;
    }
  }
}

}  // namespace

void RecentList::clear() noexcept {
  std::memset(names_, 0, sizeof names_);
  count_ = 0;
}

int RecentList::find(const char* name) const noexcept {
  if (name == nullptr) {
    return -1;
  }
  for (unsigned i = 0; i < count_; ++i) {
    if (same_name(names_[i], name)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void RecentList::erase(unsigned index) noexcept {
  for (unsigned i = index; i + 1u < count_; ++i) {
    std::memcpy(names_[i], names_[i + 1u], kNameBytes);
  }
  --count_;
  std::memset(names_[count_], 0, kNameBytes);
}

bool RecentList::add(const char* name) noexcept {
  if (!guard::valid_name(name)) {
    return false;
  }
  const int at = find(name);
  if (at == 0 && std::strcmp(names_[0], name) == 0) {
    return true;  // already the newest, spelled so: no change
  }
  if (at >= 0) {
    erase(static_cast<unsigned>(at));
  } else if (count_ == kMax) {
    erase(kMax - 1u);  // the oldest leaves
  }
  for (unsigned i = count_; i > 0u; --i) {
    std::memcpy(names_[i], names_[i - 1u], kNameBytes);
  }
  std::memset(names_[0], 0, kNameBytes);
  std::memcpy(names_[0], name, std::strlen(name));
  ++count_;
  ++generation_;
  return true;
}

bool RecentList::remove(const char* name) noexcept {
  if (!drop(name)) {
    return false;
  }
  ++generation_;
  return true;
}

bool RecentList::drop(const char* name) noexcept {
  const int at = find(name);
  if (at < 0) {
    return false;
  }
  erase(static_cast<unsigned>(at));
  return true;
}

unsigned RecentList::parse(const std::uint8_t* data, std::size_t size) noexcept {
  clear();
  if (data == nullptr || size > kMaxFileBytes) {
    return 0;
  }
  std::size_t pos = 0;
  unsigned line_no = 0;
  while (pos < size && count_ < kMax) {
    std::size_t end = pos;
    while (end < size && data[end] != '\n') {
      ++end;
    }
    std::size_t len = end - pos;
    if (len != 0u && data[pos + len - 1u] == '\r') {
      --len;
    }
    const char* line = reinterpret_cast<const char*>(data + pos);
    if (line_no == 0u) {  // "O3RC 1": the magic, one space, the version number and nothing else
      const std::size_t magic = sizeof kMagic - 1u;
      if (len < magic + 2u || std::memcmp(line, kMagic, magic) != 0 || line[magic] != ' ') {
        return 0;
      }
      unsigned version = 0;
      for (std::size_t i = magic + 1u; i < len; ++i) {
        if (line[i] < '0' || line[i] > '9' || version > 99u) {
          return 0;
        }
        version = version * 10u + static_cast<unsigned>(line[i] - '0');
      }
      if (version != kVersion) {
        return 0;  // a file of another version is not read as this one
      }
    } else if (len != 0u && len <= guard::kNameMax) {
      char name[kNameBytes];
      std::memcpy(name, line, len);
      name[len] = '\0';
      if (std::strlen(name) == len && guard::valid_name(name) && find(name) < 0) {  // (an embedded NUL makes the line shorter than it was: not a name)
        std::memcpy(names_[count_], name, len + 1u);
        ++count_;
      }
    }
    ++line_no;
    pos = end + 1u;
  }
  return count_;
}

std::size_t RecentList::write(std::uint8_t* out, std::size_t cap) const noexcept {
  std::size_t need = sizeof kMagic - 1u + 3u;  // "O3RC 1\n"
  for (unsigned i = 0; i < count_; ++i) {
    need += std::strlen(names_[i]) + 1u;
  }
  if (out == nullptr || need > cap) {
    return 0;
  }
  std::size_t n = 0;
  std::memcpy(out, kMagic, sizeof kMagic - 1u);
  n += sizeof kMagic - 1u;
  out[n++] = ' ';
  out[n++] = static_cast<std::uint8_t>('0' + kVersion);
  out[n++] = '\n';
  for (unsigned i = 0; i < count_; ++i) {
    const std::size_t len = std::strlen(names_[i]);
    std::memcpy(out + n, names_[i], len);
    n += len;
    out[n++] = '\n';
  }
  return n;
}

}  // namespace o3os::app
