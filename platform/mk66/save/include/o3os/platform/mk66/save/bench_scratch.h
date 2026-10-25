// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The scratch file of `diag bench disk` (OS-USBDISK U1d, Q14; only bench images call it, but the class is pure so the host tests run it against a software volume).
//
// Q14 (maintainer, 2026-10-17): the bench may write the card, but ONLY into one scratch file that is allocated beforehand. Never a raw LBA. Any SD card the Tracker owns will do: no marker, no volume check (maintainer, 2026-10-17, OS-ANYCARD).
// So this is the one writer of the bench. It does four things, all through FatFs (disk_write -> BlockDevice::write stays the FatFs glue's call, tools/check_layers.py rule 9):
//   * gate: refuse unless the card is the Tracker's, idle, mounted FAT12/16/32 and not write protected. A pure function of the facts: under a computer (owner Host) nothing is read at all.
//   * prepare(): make the folder `o3-bench` (not a folder of the save guard: hal/save_guard.h classifies the whole path as None, pinned by static_assert in the .cpp and by the tests) and open or
//     create `o3-bench/disk-scratch.bin`, then pre-allocate it ONCE by a write-once fill to kBenchScratchBytes (FF_USE_EXPAND is off: no f_expand, and the release image gains nothing).
//   * rewrite() (not write(): tools/check_layers.py rule 9 flags a three-argument member call of that name outside the FatFs glue): blocks INSIDE the file. A range past the end of the file is refused (Range) before FatFs is called, so a timed write never allocates a cluster, never changes the file size
//     and never touches the FAT (the tests compare the FAT sectors before and after). Before its FatFs call it re-checks that the card is the mount prepare() passed on (mounted, same generation, not write protected), as finish() does.
//   * clean(): the gate again, then remove the scratch file (the folder stays: the save contract never removes a folder).
// Where the gate runs: the whole gate (owner, idle, FAT, write protect) before the first write-class call of prepare() and of clean(); the same-mount check before the one of rewrite() and of finish(). Only bench.cpp includes this header (tools/check_layers.py rule 10).
// Main loop only (it blocks for the card's time; the fill of 1 MiB is a few seconds, once). Memory: one FIL (about 560 bytes) inside the object, which the bench places in the unused ui_cache SDRAM region.

#include <o3os/hal/storage.h>
#include <o3os/platform/mk66/storage/block_device.h>
#include <o3os/platform/mk66/storage/card_swap.h>
#include <o3os/platform/mk66/storage/fat_storage.h>

#include <cstdint>

#include <ff.h>

namespace o3os::platform::mk66::save {

inline constexpr const char* kBenchDir = "o3-bench";  // (pointers, not arrays: the static_asserts of the .cpp run them through the constexpr save guard)
inline constexpr const char* kBenchScratchPath = "o3-bench/disk-scratch.bin";
inline constexpr std::uint32_t kBenchScratchBlocks = 2048;  // 1 MiB: 63 timed calls of 32 blocks plus the warm-up call
inline constexpr std::uint32_t kBenchScratchBytes = kBenchScratchBlocks * 512u;

enum class BenchResult : std::uint8_t {
  Ok = 0,
  NotTracker,   // the card is a computer's (or there is none): nothing is touched
  Busy,         // a save, load or export holds the card
  NoCard,       // not mounted, or the card was taken out or changed during the run
  Unsupported,  // not FAT12/16/32
  WriteProtected,
  NoSpace,      // the volume cannot hold the scratch file (a short write of the fill)
  Denied,       // FatFs refused (FR_DENIED): no room for the folder or file entry, or the scratch path is a read-only file or a folder
  Range,        // a write past the end of the scratch file (refused, nothing sent)
  NotOpen,
  Io,
};

constexpr const char* bench_result_name(BenchResult r) noexcept {
  switch (r) {
    case BenchResult::Ok: return "ok";
    case BenchResult::NotTracker: return "the card is not the Tracker's (a computer has it, or no card)";
    case BenchResult::Busy: return "the card is busy (a save, load or export holds it)";
    case BenchResult::NoCard: return "no card mounted, or it changed";
    case BenchResult::Unsupported: return "file system not FAT12/16/32";
    case BenchResult::WriteProtected: return "the card is write protected";
    case BenchResult::NoSpace: return "the card is full";
    case BenchResult::Denied: return "refused: no room for the entry, or the scratch file is read-only or a folder";
    case BenchResult::Range: return "write outside the scratch file";
    case BenchResult::NotOpen: return "scratch file not open";
    case BenchResult::Io: return "i/o error";
  }
  return "?";
}

// What the gate looks at.
struct BenchFacts {
  storage::CardOwner owner = storage::CardOwner::NoCard;
  bool busy = false;
  bool mounted = false;
  hal::storage::FsKind fs = hal::storage::FsKind::None;
  bool write_protected = false;
};

// The gate as a pure function of the facts: it reads nothing, so under a computer (owner Host) not one call reaches the card.
BenchResult bench_gate(const BenchFacts& facts) noexcept;

class BenchScratch {
 public:
  BenchScratch(storage::FatStorage& fat, storage::BlockDevice& device) noexcept : fat_(fat), device_(device) {}
  ~BenchScratch() = default;
  BenchScratch(const BenchScratch&) = delete;
  BenchScratch& operator=(const BenchScratch&) = delete;

  // Called once, only when prepare() is about to write the fill (the file is new or short), before its first byte: the caller says that the main loop is about to wait.
  using FillNotice = void (*)(void* ctx) noexcept;

  // The gate, the folder, the file, the one-time fill (from `pattern`, `pattern_bytes` a multiple of 512, reused as often as needed). Leaves the file open for rewrite().
  BenchResult prepare(storage::CardOwner owner, bool busy, const std::uint8_t* pattern, std::uint32_t pattern_bytes, FillNotice before_fill = nullptr, void* notice_ctx = nullptr) noexcept;
  // `count` blocks from block `block` of the file, taken from `src`. Inside the file or Range.
  BenchResult rewrite(std::uint32_t block, const void* src, std::uint32_t count) noexcept;
  // Flushes the directory entry and closes. Always leaves the file closed; on a card that is no longer the mount of prepare() it only forgets the handle (NoCard, no FatFs call).
  BenchResult finish() noexcept;
  // The gate again, then removes the scratch file (Ok when it is not there).
  BenchResult clean(storage::CardOwner owner, bool busy) noexcept;

  bool is_open() const noexcept { return open_; }
  std::uint32_t blocks() const noexcept { return blocks_; }        // the size of the scratch file in blocks (kBenchScratchBlocks once it is filled)
  std::uint32_t filled_bytes() const noexcept { return filled_; }  // bytes the last prepare() had to write (0: the file was there)
  int last_fatfs_error() const noexcept { return last_fresult_; }

 private:
  BenchFacts facts(storage::CardOwner owner, bool busy) const noexcept;
  BenchResult fail(FRESULT r) noexcept;
  bool same_mount() const noexcept;  // mounted, still the mount (generation) the file was opened on, not write protected

  storage::FatStorage& fat_;
  storage::BlockDevice& device_;
  FIL fil_{};
  bool open_ = false;
  std::uint32_t generation_ = 0;  // the mount the file was opened on (FatStorage::generation())
  std::uint32_t blocks_ = 0;
  std::uint32_t next_block_ = 0;  // where the file position is now, to seek only when a write is not the next block
  std::uint32_t filled_ = 0;
  int last_fresult_ = 0;
};

}  // namespace o3os::platform::mk66::save
