// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/platform/host/slow_card.h>

#include <cstring>
#include <string>

namespace o3os::platform::host {

namespace st = hal::storage;

namespace {
constexpr std::size_t kSector = 512;
constexpr unsigned kSlotsPerSector = 16;  // 32-byte directory slots in a 512-byte sector
constexpr unsigned kCharsPerLongSlot = 13;  // UTF-16 units one long-name slot holds

// The directory slots one entry takes: its long-name slots plus the short entry (FatFs writes a long name for any name that is not plain 8.3, so this counts every entry as long: the large card's names are).
unsigned slots_of(const char* name) noexcept {
  return static_cast<unsigned>((std::strlen(name) + kCharsPerLongSlot - 1u) / kCharsPerLongSlot) + 1u;
}

bool same_name(const char* a, const char* b) noexcept {  // FAT compares names without case (ASCII here)
  for (;; ++a, ++b) {
    const char x = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
    const char y = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + 32) : *b;
    if (x != y) {
      return false;
    }
    if (x == '\0') {
      return true;
    }
  }
}
}  // namespace

std::uint64_t SlowCard::cost_ns(std::uint32_t latency_us, std::size_t bytes, std::uint32_t bytes_per_ms) noexcept {
  const std::uint64_t rate = bytes_per_ms == 0u ? 1u : bytes_per_ms;
  return static_cast<std::uint64_t>(latency_us) * 1000u + (static_cast<std::uint64_t>(bytes) * 1000000u + rate - 1u) / rate;
}

void SlowCard::charge_ns(std::uint64_t ns) noexcept {
  carry_ns_ += ns;
  const std::uint64_t whole = carry_ns_ / 1000u;
  carry_ns_ -= whole * 1000u;
  // The clock takes 32 bits at a time (a call longer than 71 minutes is not a call).
  std::uint64_t left = whole;
  while (left != 0u) {
    const std::uint32_t part = left > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<std::uint32_t>(left);
    clock_.card_time(part);
    left -= part;
  }
  charged_us_ += whole;
}

void SlowCard::charge_read(std::size_t bytes, unsigned latencies) noexcept {
  if (!timing_.on) {
    return;
  }
  charge_ns(static_cast<std::uint64_t>(latencies) * timing_.latency_us * 1000u + cost_ns(0u, bytes, timing_.read_bytes_per_ms));
}

void SlowCard::charge_write(std::size_t bytes, unsigned latencies) noexcept {
  if (!timing_.on) {
    return;
  }
  charge_ns(static_cast<std::uint64_t>(latencies) * timing_.latency_us * 1000u + cost_ns(0u, bytes, timing_.write_bytes_per_ms));
}

// The entries of the folder `dir` in the inner card's order, each with the slot at which it ends: read through the inner card (free) on a handle of its own, kept until the card is written.
// nullptr when the folder cannot be listed (a missing folder, or both handles of the card are in use by the app: the caller charges a sector then).
const SlowCard::Listing* SlowCard::listing_of(const std::string& dir) noexcept {
  const auto known = listings_.find(dir);
  if (known != listings_.end()) {
    return &known->second;
  }
  st::DirHandle handle;
  if (inner_.opendir(dir.c_str(), handle) != st::Status::Ok) {
    return nullptr;
  }
  Listing list;
  st::DirEntry entry;
  while (inner_.readdir(handle, entry) == st::Status::Ok) {
    list.slots += slots_of(entry.name);
    list.entries.emplace_back(entry.name, list.slots);
  }
  inner_.closedir(handle);
  return &listings_.emplace(dir, std::move(list)).first->second;
}

// The sectors a walk down `path` reads: FatFs searches each folder from its first slot, one sector at a time, up to the sector that holds the name (a name that is not there reads the whole folder, and the
// walk ends). A folder that cannot be listed costs one sector.
unsigned SlowCard::walk_sectors(const char* path) noexcept {
  std::string dir;  // the folder being searched, as the inner card takes it (the root is "/")
  unsigned sectors = 0;
  const char* p = path;
  while (p != nullptr && *p != '\0') {
    while (*p == '/') {
      ++p;
    }
    if (*p == '\0') {
      break;
    }
    const char* e = p;
    while (*e != '\0' && *e != '/') {
      ++e;
    }
    const std::string name(p, static_cast<std::size_t>(e - p));
    const Listing* list = listing_of(dir.empty() ? std::string("/") : dir);
    if (list == nullptr) {
      sectors += 1u;
    } else {
      bool found = false;
      for (const auto& entry : list->entries) {
        if (same_name(entry.first.c_str(), name.c_str())) {
          sectors += (entry.second - 1u) / kSlotsPerSector + 1u;
          found = true;
          break;
        }
      }
      if (!found) {
        sectors += list->slots == 0u ? 1u : (list->slots + kSlotsPerSector - 1u) / kSlotsPerSector;
        break;
      }
    }
    dir += dir.empty() ? name : "/" + name;
    p = e;
  }
  return sectors == 0u ? 1u : sectors;  // the root itself is one sector
}

void SlowCard::charge_walk(const char* path) noexcept {
  if (!timing_.on) {
    return;
  }
  const unsigned n = walk_sectors(path);
  charge_read(n * kSector, n);
}

st::Status SlowCard::exists(const char* path, bool* is_dir) noexcept {
  charge_walk(path);
  return inner_.exists(path, is_dir);
}

st::Status SlowCard::stat(const char* path, st::FileStamp& out) noexcept {
  charge_walk(path);
  return inner_.stat(path, out);
}

st::Status SlowCard::open(const char* path, st::FileHandle& out) noexcept {
  charge_walk(path);
  return inner_.open(path, out);
}

st::Status SlowCard::read(st::FileHandle file, void* dst, std::size_t max, std::size_t& got) noexcept {
  const st::Status s = inner_.read(file, dst, max, got);
  if (s == st::Status::Ok) {
    charge_read(got, got == 0u ? 0u : 1u);
  }
  return s;
}

st::Status SlowCard::seek(st::FileHandle file, std::uint32_t offset) noexcept {
  charge_read(kSector, 1);
  return inner_.seek(file, offset);
}

st::Status SlowCard::opendir(const char* path, st::DirHandle& out) noexcept {
  charge_walk(path);
  const st::Status s = inner_.opendir(path, out);
  if (s == st::Status::Ok && out.slot < st::kMaxOpenDirs) {
    dirs_[out.slot] = OpenDir{};
  }
  return s;
}

st::Status SlowCard::readdir(st::DirHandle dir, st::DirEntry& out) noexcept {
  const st::Status s = inner_.readdir(dir, out);
  if (s == st::Status::Ok && dir.slot < st::kMaxOpenDirs) {
    // An entry takes ceil(len/13)+1 slots; the listing reads each sector of the folder once, when its first slot is reached.
    OpenDir& d = dirs_[dir.slot];
    const unsigned slots = slots_of(out.name);
    const unsigned sectors_to_here = (d.slots + slots - 1u) / kSlotsPerSector + 1u;
    if (sectors_to_here > d.sectors) {
      const unsigned fresh = sectors_to_here - d.sectors;
      charge_read(fresh * kSector, fresh);
      d.sectors = sectors_to_here;
    }
    d.slots += slots;
  }
  return s;
}

st::WriteStatus SlowCard::make_dir(const char* path) noexcept {
  if (timing_.full) {
    return st::WriteStatus::NoSpace;
  }
  listings_.clear();
  charge_write(kSector, 1);
  return inner_.make_dir(path);
}

st::WriteStatus SlowCard::create(const char* path) noexcept {
  if (timing_.full) {
    return st::WriteStatus::NoSpace;
  }
  listings_.clear();
  charge_write(kSector, 1);
  return inner_.create(path);
}

st::WriteStatus SlowCard::write(const void* src, std::size_t n) noexcept {
  if (timing_.full && n != 0u) {  // (a file open for writing only grows: create refuses a name that is there)
    return st::WriteStatus::NoSpace;
  }
  const st::WriteStatus s = inner_.write(src, n);
  if (s == st::WriteStatus::Ok) {
    charge_write(n, 0);
  }
  return s;
}

st::WriteStatus SlowCard::finish() noexcept {
  listings_.clear();
  charge_write(kSector, 1);
  return inner_.finish();
}

st::WriteStatus SlowCard::rename(const char* from, const char* to) noexcept {
  listings_.clear();
  charge_write(kSector, 1);
  return inner_.rename(from, to);
}

st::WriteStatus SlowCard::create_marker(const char* path) noexcept {
  if (timing_.full) {
    return st::WriteStatus::NoSpace;
  }
  listings_.clear();
  charge_write(kSector, 1);
  return inner_.create_marker(path);
}

st::WriteStatus SlowCard::remove(const char* path) noexcept {
  listings_.clear();
  charge_write(kSector, 1);
  return inner_.remove(path);
}

}  // namespace o3os::platform::host
