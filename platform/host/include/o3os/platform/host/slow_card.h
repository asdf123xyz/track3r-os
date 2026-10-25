// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A slow card (OS-QANET D2, docs/specs/OS-QANET.md section 7): a wrapper over any HostCard (a DirCard or a FatFsStorage) that charges the time the unit's card would take to the simulated clock, in the
// pass where the call happens. The unit's card calls are synchronous (the main loop waits for the sector), so a call of the app stretches the pass that makes it; the host's own are instant, so a latency
// that depends on the card (the Sample load preview, a listing, a project load) could never show in a run. With the model off (CardTiming::on false) every call goes straight through and nothing is charged.
//
// The model, per call: latency_us + bytes / rate.
//   * reads:  `read` charges latency_us plus the bytes it returned at read_bytes_per_ms. `exists`, `stat`, `open` and `opendir` walk the path: for each component FatFs searches the folder from its first
//     slot, one sector a call (latency_us plus 512 bytes), up to the sector that holds the name, so a name late in a big folder costs many sectors (a name that is not there reads the whole folder). A
//     folder entry takes ceil(len/13)+1 32-byte slots (the long-name slots and the short entry; every name is counted as long), 16 slots a sector. `seek` charges one sector (the FAT chain). `readdir`
//     charges each sector of the folder once, when the entry that starts or reaches it is returned. `size`, `close` and `closedir` are free (the handle holds the size).
//   * writes: `write` charges its bytes at write_bytes_per_ms (no latency: the unit's writer fills a sector and then writes it); `create`, `make_dir`, `finish`, `rename`, `remove` and `create_marker` charge
//     latency_us plus one sector at the write rate (a directory entry).
//   * `mount`, `gate`, `begin_save` and the slot's switch are not charged (the platform's mount is its own wait).
// The full switch (CardTiming::full, OS-QANET B8, independent of `on`): a full card refuses every write that would grow it (`make_dir`, `create`, `create_marker`, and a `write` of any byte: a file open for
// writing only grows) with WriteStatus::NoSpace, the status the host card gives for ENOSPC. Reads, `rename`, `remove` and `finish` still work, so the old files stay readable and deletable.
// Time below a microsecond is carried to the next call, so a long run of small calls adds up exactly. Charging is the only effect: the answers and the data are the inner card's.
//
// The defaults are PROVISIONAL until OS-QANET D4 measures them on the unit (platform/mk66/tools/hil/qanet_latency.json). They come from the bench of the card's block driver on the unit (v0.6.10-bench, one
// card, docs/specs/OS-USBDISK.md 4a): one block a call 1.8 MB/s (about 286 us a call) and 32 blocks a call 13.9 MB/s (about 1.18 ms a call). A line through the two points is 257 us a call plus 17.7 bytes
// per microsecond (it gives back both: 257 + 512/17.7 = 286 us, 257 + 16384/17.7 = 1183 us). The write side is the bench's disk_wr1, about 432 KiB/s. These are the block driver's numbers, not a file's:
// the FatFs layer over it (cluster chains, the directory walk) is what the sector-a-call walk above stands in for (a folder's own cluster chain is not charged: PROVISIONAL, as the rest).
//
// Host only (platform/host); main thread only.

#include <o3os/platform/host/host_card.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace o3os::platform::host {

// The model's numbers and its switch. Plain data: the simulator owns one and every SlowCard of the run reads it.
struct CardTiming {
  static constexpr std::uint32_t kDefaultReadBytesPerMs = 17700;  // PROVISIONAL (see above): the fit to the unit's block bench
  static constexpr std::uint32_t kDefaultLatencyUs = 257;         // PROVISIONAL: the fit to the unit's block bench
  static constexpr std::uint32_t kDefaultWriteBytesPerMs = 442;   // PROVISIONAL: the unit's disk_wr1, 432 KiB/s

  bool on = false;
  bool full = false;  // OS-QANET B8: the card is full (see above); not part of the timing model, so `on` does not gate it
  std::uint32_t read_bytes_per_ms = kDefaultReadBytesPerMs;  // > 0
  std::uint32_t latency_us = kDefaultLatencyUs;
  std::uint32_t write_bytes_per_ms = kDefaultWriteBytesPerMs;  // > 0
};

// Where the time goes: the simulator's clock (and the audio that the stretched pass would not stop).
class CardClock {
 public:
  virtual ~CardClock() = default;
  virtual void card_time(std::uint32_t us) noexcept = 0;
};

class SlowCard final : public HostCard {
 public:
  SlowCard(HostCard& inner, const CardTiming& timing, CardClock& clock) : inner_(inner), timing_(timing), clock_(clock) {}

  // Everything the model charged since this card was made, in microseconds (a test reads it against the clock).
  std::uint64_t charged_us() const { return charged_us_; }

  // ---- HostCard
  void set_present(bool present) override {
    listings_.clear();
    inner_.set_present(present);
  }
  unsigned violations() const override { return inner_.violations(); }

  // ---- hal::storage::Storage
  hal::storage::Status mount() noexcept override {
    listings_.clear();
    return inner_.mount();
  }
  bool mounted() const noexcept override { return inner_.mounted(); }
  bool card_present() const noexcept override { return inner_.card_present(); }
  hal::storage::VolumeInfo volume() const noexcept override { return inner_.volume(); }
  hal::storage::Status exists(const char* path, bool* is_dir) noexcept override;
  hal::storage::Status stat(const char* path, hal::storage::FileStamp& out) noexcept override;
  hal::storage::Status open(const char* path, hal::storage::FileHandle& out) noexcept override;
  hal::storage::Status read(hal::storage::FileHandle file, void* dst, std::size_t max, std::size_t& got) noexcept override;
  hal::storage::Status seek(hal::storage::FileHandle file, std::uint32_t offset) noexcept override;
  hal::storage::Status size(hal::storage::FileHandle file, std::uint32_t& out) const noexcept override { return inner_.size(file, out); }
  void close(hal::storage::FileHandle& file) noexcept override { inner_.close(file); }
  hal::storage::Status opendir(const char* path, hal::storage::DirHandle& out) noexcept override;
  hal::storage::Status readdir(hal::storage::DirHandle dir, hal::storage::DirEntry& out) noexcept override;
  void closedir(hal::storage::DirHandle& dir) noexcept override { inner_.closedir(dir); }

  // ---- hal::storage::StorageWriter
  hal::storage::WriteStatus gate() noexcept override { return inner_.gate(); }
  hal::storage::WriteStatus make_dir(const char* path) noexcept override;
  hal::storage::WriteStatus create(const char* path) noexcept override;
  hal::storage::WriteStatus write(const void* src, std::size_t n) noexcept override;
  hal::storage::WriteStatus finish() noexcept override;
  void abandon() noexcept override { inner_.abandon(); }
  bool writing() const noexcept override { return inner_.writing(); }
  hal::storage::WriteStatus rename(const char* from, const char* to) noexcept override;
  hal::storage::WriteStatus create_marker(const char* path) noexcept override;
  hal::storage::WriteStatus remove(const char* path) noexcept override;
  void begin_save() noexcept override { inner_.begin_save(); }
  void end_save() noexcept override { inner_.end_save(); }
  std::uint32_t card_id() const noexcept override { return inner_.card_id(); }

  // The model's arithmetic, public so a test can state it: the microseconds of `bytes` at `bytes_per_ms` (rounded up to a whole microsecond) plus `latency_us`.
  static std::uint64_t cost_ns(std::uint32_t latency_us, std::size_t bytes, std::uint32_t bytes_per_ms) noexcept;

 private:
  void charge_read(std::size_t bytes, unsigned sectors_of_latency) noexcept;  // `bytes` at the read rate and `latency_us` times the count
  void charge_write(std::size_t bytes, unsigned latencies) noexcept;
  void charge_ns(std::uint64_t ns) noexcept;
  // A folder as the walk sees it: its entries in the inner card's order, each with the slot at which it ends, and the slots in all. Read through the inner card (not charged), kept until the card is written.
  struct Listing {
    std::vector<std::pair<std::string, unsigned>> entries;
    unsigned slots = 0;
  };
  struct OpenDir {
    unsigned slots = 0;    // slots of the entries returned so far
    unsigned sectors = 0;  // sectors of the folder read so far
  };
  const Listing* listing_of(const std::string& dir) noexcept;
  unsigned walk_sectors(const char* path) noexcept;
  void charge_walk(const char* path) noexcept;

  HostCard& inner_;
  const CardTiming& timing_;
  CardClock& clock_;
  std::uint64_t charged_us_ = 0;
  std::uint64_t carry_ns_ = 0;  // under a microsecond, not charged yet
  OpenDir dirs_[hal::storage::kMaxOpenDirs];
  std::map<std::string, Listing> listings_;
};

}  // namespace o3os::platform::host
