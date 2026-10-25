// SPDX-License-Identifier: GPL-3.0-or-later
//
// The scratch file of `diag bench disk` (save/bench_scratch.h; OS-USBDISK U1d, Q14). After fat_storage_writer.cpp and the firmware store, the third source that names a FatFs write-class call
// (tools/check_layers.py rules 6, 8 and 10 list it): it opens ONE path and nothing else. prepare() and clean() run the whole gate (bench_gate: owner, idle, FAT, write protect) before their
// first write-class call. rewrite() and finish() re-check only that the card is still the mount the gate passed on (mounted, the same FatStorage::generation(), not write protected) and make
// no FatFs call otherwise.
#include <o3os/platform/mk66/save/bench_scratch.h>

#include <o3os/hal/save_guard.h>

#include <cstddef>

namespace o3os::platform::mk66::save {
namespace {

namespace guard = hal::storage::save;

// The bench's folder and file are not in the save guard's world: no user folder (Projects, Samples, Settings, Export, instruments, AppExport, Firmware ...), no save target, no Temp, no Backup,
// no delete marker, and nothing the guard lets anybody create, rename or remove. The bench's own writes are therefore never confusable with a save, and a save can never touch the bench's file.
static_assert(guard::classify(kBenchDir) == guard::Place::None, "the bench folder is not a folder of the save guard");
static_assert(guard::classify(kBenchScratchPath) == guard::Place::None, "the scratch file is not a path of the save guard");
static_assert(!guard::may_make_dir(kBenchDir) && !guard::may_create(kBenchScratchPath) && !guard::may_remove(kBenchScratchPath), "the save layer cannot touch the scratch file");
static_assert(kBenchScratchBytes % 512u == 0u, "the scratch file is whole blocks");

}  // namespace

BenchResult bench_gate(const BenchFacts& f) noexcept {
  if (f.owner != storage::CardOwner::Tracker) {
    return BenchResult::NotTracker;  // first, and nothing below touches the card: a computer may be writing it
  }
  if (f.busy) {
    return BenchResult::Busy;
  }
  if (!f.mounted) {
    return BenchResult::NoCard;
  }
  if (f.fs != hal::storage::FsKind::Fat12 && f.fs != hal::storage::FsKind::Fat16 && f.fs != hal::storage::FsKind::Fat32) {
    return BenchResult::Unsupported;
  }
  if (f.write_protected) {
    return BenchResult::WriteProtected;
  }
  return BenchResult::Ok;
}

BenchFacts BenchScratch::facts(storage::CardOwner owner, bool busy) const noexcept {
  BenchFacts f;
  f.owner = owner;
  f.busy = busy;
  f.mounted = fat_.mounted();
  f.fs = f.mounted ? fat_.volume().kind : hal::storage::FsKind::None;
  f.write_protected = device_.write_protected();
  return f;
}

BenchResult BenchScratch::fail(FRESULT r) noexcept {
  if (r != FR_OK) {
    last_fresult_ = static_cast<int>(r);
  }
  switch (r) {
    case FR_OK: return BenchResult::Ok;
    case FR_DENIED: return BenchResult::Denied;  // not only a full volume or folder: also a read-only file or a folder at the scratch path (f_open FA_OPEN_ALWAYS), so it has its own message
    case FR_WRITE_PROTECTED: return BenchResult::WriteProtected;
    case FR_NOT_ENABLED:
    case FR_NO_FILESYSTEM: return BenchResult::NoCard;
    default: return BenchResult::Io;
  }
}

bool BenchScratch::same_mount() const noexcept {
  return fat_.mounted() && fat_.generation() == generation_ && !device_.write_protected();
}

BenchResult BenchScratch::prepare(storage::CardOwner owner, bool busy, const std::uint8_t* pattern, std::uint32_t pattern_bytes, FillNotice before_fill, void* notice_ctx) noexcept {
  (void)finish();  // (a file left open by a run that ended early)
  filled_ = 0;
  blocks_ = 0;
  next_block_ = 0;
  if (pattern == nullptr || pattern_bytes == 0u || pattern_bytes % 512u != 0u) {
    return BenchResult::Range;
  }
  const BenchResult g = bench_gate(facts(owner, busy));
  if (g != BenchResult::Ok) {
    return g;
  }
  FRESULT r = f_mkdir(kBenchDir);
  if (r != FR_OK && r != FR_EXIST) {
    return fail(r);
  }
  r = f_open(&fil_, kBenchScratchPath, FA_READ | FA_WRITE | FA_OPEN_ALWAYS);  // an existing file is kept as it is: never truncated
  if (r != FR_OK) {
    return fail(r);
  }
  open_ = true;
  generation_ = fat_.generation();
  std::uint32_t size = static_cast<std::uint32_t>(f_size(&fil_));
  if (size < kBenchScratchBytes) {  // the write-once fill: from the end of what is there (a fill cut short earlier goes on) to the full size
    if (before_fill != nullptr) {
      before_fill(notice_ctx);  // (the fill blocks the main loop for seconds on a slow card: the caller says so first)
    }
    r = f_lseek(&fil_, size);
    if (r != FR_OK) {
      (void)finish();
      return fail(r);
    }
    while (size < kBenchScratchBytes) {
      const std::uint32_t left = kBenchScratchBytes - size;
      const UINT n = static_cast<UINT>(left < pattern_bytes ? left : pattern_bytes);
      UINT written = 0;
      r = f_write(&fil_, pattern, n, &written);
      if (r != FR_OK || written != n) {
        const BenchResult why = r != FR_OK ? fail(r) : BenchResult::NoSpace;
        (void)finish();
        return why;
      }
      size += n;
      filled_ += n;
    }
    r = f_sync(&fil_);
    if (r != FR_OK) {
      (void)finish();
      return fail(r);
    }
  }
  blocks_ = size / 512u < kBenchScratchBlocks ? size / 512u : kBenchScratchBlocks;
  r = f_lseek(&fil_, 0);  // (a seek inside the size: no cluster is made)
  if (r != FR_OK) {
    (void)finish();
    return fail(r);
  }
  return BenchResult::Ok;
}

BenchResult BenchScratch::rewrite(std::uint32_t block, const void* src, std::uint32_t count) noexcept {
  if (!open_) {
    return BenchResult::NotOpen;
  }
  if (!same_mount()) {
    return BenchResult::NoCard;  // the card was pulled or changed since prepare(): the file handle is of another mount
  }
  if (src == nullptr || count == 0u || block > blocks_ || count > blocks_ - block) {
    return BenchResult::Range;  // before FatFs: a write past the end would extend the file
  }
  if (block != next_block_) {
    const FRESULT s = f_lseek(&fil_, block * 512u);
    if (s != FR_OK) {
      return fail(s);
    }
  }
  const UINT n = count * 512u;
  UINT written = 0;
  const FRESULT r = f_write(&fil_, src, n, &written);
  if (r != FR_OK) {
    return fail(r);
  }
  if (written != n) {
    return BenchResult::Io;
  }
  next_block_ = block + count;
  return BenchResult::Ok;
}

BenchResult BenchScratch::finish() noexcept {
  if (!open_) {
    return BenchResult::Ok;
  }
  open_ = false;  // whatever happens next, no file is open afterwards
  if (!same_mount()) {
    return BenchResult::NoCard;  // the handle is of a mount that is gone (or the card is protected now): no write-class call, FF_FS_LOCK is 0 so nothing is left held
  }
  const FRESULT synced = f_sync(&fil_);
  const FRESULT closed = f_close(&fil_);
  return fail(synced != FR_OK ? synced : closed);
}

BenchResult BenchScratch::clean(storage::CardOwner owner, bool busy) noexcept {
  (void)finish();
  const BenchResult g = bench_gate(facts(owner, busy));
  if (g != BenchResult::Ok) {
    return g;
  }
  const FRESULT r = f_unlink(kBenchScratchPath);
  if (r == FR_NO_FILE || r == FR_NO_PATH) {
    return BenchResult::Ok;
  }
  return fail(r);
}

}  // namespace o3os::platform::mk66::save
