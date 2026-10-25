// SPDX-License-Identifier: GPL-3.0-or-later
//
// The write arm of `diag bench disk` (save/bench_scratch.h; OS-USBDISK U1d, Q14): the file itself on a FAT32 volume built in the test (the gate over a MemCard and the path against the save guard
// are tests/bench_scratch_guard_test.cpp: a platform test may not include tests/support): made and filled once, kept, rewritten INSIDE its size only (the FAT and the file size do not move, a write past the end is refused before FatFs), a fill cut short
// goes on, and `clean` removes the file and nothing else. Runs on the host and on QEMU's Cortex-M4F (the volume is sparse).
#include <doctest/doctest.h>

#include "mk66_storage_need.h"

#include "mk66_storage_fakes.h"

#include <o3os/hal/storage.h>
#include <o3os/hal/storage_write.h>
#include <o3os/platform/mk66/save/bench_scratch.h>
#include <o3os/platform/mk66/storage/fat_storage.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hs = o3os::hal::storage;
namespace st = o3os::platform::mk66::storage;
namespace sv = o3os::platform::mk66::save;
using fakes::Fat32Builder;
using fakes::Medium;
using sv::BenchResult;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes pattern(std::size_t n, unsigned salt) {
  Bytes v(n);
  for (std::size_t i = 0; i < n; ++i) {
    v[i] = static_cast<std::uint8_t>((i * 131u + (i >> 9) * 17u + salt) & 0xFFu);
  }
  return v;
}

constexpr std::uint32_t kPatternBytes = 32u * 512u;

// prepare()'s notice before the fill: counts its calls and sees the file as it is at that moment (the fill has not begun).
struct NoticeProbe {
  int calls = 0;
  const Medium* medium = nullptr;
  std::uint64_t written_at_call = 0;
};
void probe_notice(void* ctx) noexcept {
  auto* p = static_cast<NoticeProbe*>(ctx);
  ++p->calls;
  p->written_at_call = p->medium->blocks_written;
}

struct Card {
  Medium medium{200'000};
  Card() {
    Fat32Builder fat(medium);
    fat.add_file(Fat32Builder::kRootCluster, "README.TXT", pattern(40, 1));
    const std::uint32_t projects = fat.make_dir(Fat32Builder::kRootCluster, "Projects");
    const std::uint32_t song = fat.make_dir(projects, "Song");
    fat.add_file(song, "project.mt", pattern(2320, 10));
    const std::uint32_t samples = fat.make_dir(Fat32Builder::kRootCluster, "Samples");
    fat.add_file(samples, "A.WAV", pattern(700, 12));
  }
};

Bytes read_file(hs::Storage& s, const char* path) {
  Bytes out;
  hs::FileHandle f;
  if (s.open(path, f) != hs::Status::Ok) {
    return out;
  }
  std::uint8_t buf[512];
  std::size_t got = 0;
  while (s.read(f, buf, sizeof buf, got) == hs::Status::Ok && got != 0u) {
    out.insert(out.end(), buf, buf + got);
  }
  s.close(f);
  return out;
}

bool exists(hs::Storage& s, const char* path) { return s.exists(path, nullptr) == hs::Status::Ok; }

// FNV-1a over the FAT copy 1 (Fat32Builder: 32 reserved sectors, then ((65'600 + 2) * 4 + 511) / 512 sectors of FAT).
std::uint64_t fat_hash(const Medium& m) {
  std::uint64_t h = 1469598103934665603ull;
  std::uint8_t b[512];
  const std::uint32_t sectors = ((65'600u + 2u) * 4u + 511u) / 512u;
  for (std::uint32_t i = 0; i < sectors; ++i) {
    m.read_sector(Fat32Builder::kReserved + i, b);
    for (std::uint8_t x : b) {
      h = (h ^ x) * 1099511628211ull;
    }
  }
  return h;
}

}  // namespace

TEST_CASE("bench scratch file: made and filled once, kept, rewritten inside its size only") {
  Card card;
  st::FatStorage fat(card.medium);
  NEED(fat.mount() == hs::Status::Ok);
  const Bytes pat = pattern(kPatternBytes, 5);
  sv::BenchScratch sc(fat, card.medium);

  CHECK_FALSE(exists(fat, sv::kBenchScratchPath));
  NEED(sc.prepare(st::CardOwner::Tracker, false, pat.data(), kPatternBytes) == BenchResult::Ok);
  CHECK(sc.is_open());
  CHECK(sc.blocks() == sv::kBenchScratchBlocks);
  CHECK(sc.filled_bytes() == sv::kBenchScratchBytes);
  CHECK(exists(fat, sv::kBenchDir));
  hs::FileHandle fh;
  std::uint32_t size = 0;
  NEED(fat.open(sv::kBenchScratchPath, fh) == hs::Status::Ok);
  CHECK(fat.size(fh, size) == hs::Status::Ok);
  fat.close(fh);
  CHECK(size == sv::kBenchScratchBytes);

  // From here the FAT must not move: every write is inside the file.
  const std::uint64_t fat_before = fat_hash(card.medium);
  const Bytes one = pattern(512, 21), eight = pattern(8 * 512, 22), many = pattern(32 * 512, 23);
  CHECK(sc.rewrite(0, one.data(), 1) == BenchResult::Ok);
  CHECK(sc.rewrite(1, eight.data(), 8) == BenchResult::Ok);  // the next block: no seek
  CHECK(sc.rewrite(100, many.data(), 32) == BenchResult::Ok);  // a seek forward
  CHECK(sc.rewrite(3, one.data(), 1) == BenchResult::Ok);      // a seek back
  CHECK(sc.rewrite(sv::kBenchScratchBlocks - 32u, many.data(), 32) == BenchResult::Ok);  // the last 32 blocks: exactly the end
  CHECK(sc.finish() == BenchResult::Ok);
  CHECK_FALSE(sc.is_open());
  CHECK(fat_hash(card.medium) == fat_before);

  const Bytes f = read_file(fat, sv::kBenchScratchPath);
  NEED(f.size() == sv::kBenchScratchBytes);  // the size did not move
  CHECK(Bytes(f.begin(), f.begin() + 512) == one);
  CHECK(Bytes(f.begin() + 512, f.begin() + 512 + 3 * 512) != Bytes(eight.begin(), eight.begin() + 3 * 512));  // block 3 was rewritten by the later single write
  CHECK(Bytes(f.begin() + 512, f.begin() + 512 + 2 * 512) == Bytes(eight.begin(), eight.begin() + 2 * 512));
  CHECK(Bytes(f.begin() + 4 * 512, f.begin() + 9 * 512) == Bytes(eight.begin() + 3 * 512, eight.end()));
  CHECK(Bytes(f.begin() + 100 * 512, f.begin() + 132 * 512) == many);
  CHECK(Bytes(f.end() - 32 * 512, f.end()) == many);
  CHECK(Bytes(f.begin() + 3 * 512, f.begin() + 4 * 512) == one);

  // The user's files are as they were.
  CHECK(read_file(fat, "README.TXT") == pattern(40, 1));
  CHECK(read_file(fat, "Projects/Song/project.mt") == pattern(2320, 10));
  CHECK(read_file(fat, "Samples/A.WAV") == pattern(700, 12));

  // A second run finds the file: nothing to fill, the content is what was written, and it is kept.
  const std::uint64_t fat_kept = fat_hash(card.medium);
  NEED(sc.prepare(st::CardOwner::Tracker, false, pat.data(), kPatternBytes) == BenchResult::Ok);
  CHECK(sc.filled_bytes() == 0u);
  CHECK(sc.blocks() == sv::kBenchScratchBlocks);
  CHECK(sc.finish() == BenchResult::Ok);
  CHECK(fat_hash(card.medium) == fat_kept);
  CHECK(read_file(fat, sv::kBenchScratchPath) == f);
}

TEST_CASE("bench scratch file: a write past the end is refused before FatFs, nothing reaches the card") {
  Card card;
  st::FatStorage fat(card.medium);
  NEED(fat.mount() == hs::Status::Ok);
  const Bytes pat = pattern(kPatternBytes, 5);
  sv::BenchScratch sc(fat, card.medium);
  const Bytes blk = pattern(33 * 512, 9);
  CHECK(sc.rewrite(0, blk.data(), 1) == BenchResult::NotOpen);  // before prepare()
  NEED(sc.prepare(st::CardOwner::Tracker, false, pat.data(), kPatternBytes) == BenchResult::Ok);
  const std::uint64_t written = card.medium.blocks_written;
  CHECK(sc.rewrite(sv::kBenchScratchBlocks, blk.data(), 1) == BenchResult::Range);
  CHECK(sc.rewrite(sv::kBenchScratchBlocks - 1u, blk.data(), 2) == BenchResult::Range);
  CHECK(sc.rewrite(sv::kBenchScratchBlocks - 31u, blk.data(), 32) == BenchResult::Range);
  CHECK(sc.rewrite(0, blk.data(), sv::kBenchScratchBlocks + 1u) == BenchResult::Range);
  CHECK(sc.rewrite(0xFFFFFFF0u, blk.data(), 32) == BenchResult::Range);  // no wrap in the arithmetic
  CHECK(sc.rewrite(0, blk.data(), 0) == BenchResult::Range);
  CHECK(sc.rewrite(0, nullptr, 1) == BenchResult::Range);
  CHECK(card.medium.blocks_written == written);
  // A bad pattern is refused before the card is touched.
  CHECK(sc.finish() == BenchResult::Ok);
  const std::uint64_t before = card.medium.blocks_written;
  CHECK(sc.prepare(st::CardOwner::Tracker, false, nullptr, kPatternBytes) == BenchResult::Range);
  CHECK(sc.prepare(st::CardOwner::Tracker, false, pat.data(), 100) == BenchResult::Range);
  CHECK(card.medium.blocks_written == before);
}

TEST_CASE("bench scratch file: a computer's card, a busy one and a write protected one are refused and not one block is written") {
  {
    Card card;  // no marker is looked for (OS-ANYCARD): the refusals below are the owner, the busy state and the write protect
    st::FatStorage fat(card.medium);
    NEED(fat.mount() == hs::Status::Ok);
    const Bytes pat = pattern(kPatternBytes, 5);
    sv::BenchScratch sc(fat, card.medium);
    const std::uint64_t written = card.medium.blocks_written;
    CHECK(sc.prepare(st::CardOwner::Host, false, pat.data(), kPatternBytes) == BenchResult::NotTracker);
    CHECK(sc.prepare(st::CardOwner::NoCard, false, pat.data(), kPatternBytes) == BenchResult::NotTracker);
    CHECK(sc.prepare(st::CardOwner::Tracker, true, pat.data(), kPatternBytes) == BenchResult::Busy);
    CHECK(sc.clean(st::CardOwner::Host, false) == BenchResult::NotTracker);
    CHECK(card.medium.blocks_written == written);
    CHECK_FALSE(exists(fat, sv::kBenchDir));
    card.medium.protect = true;
    CHECK(sc.prepare(st::CardOwner::Tracker, false, pat.data(), kPatternBytes) == BenchResult::WriteProtected);
    CHECK(card.medium.blocks_written == written);
  }
}

TEST_CASE("bench scratch file: a fill cut short goes on from where it stopped") {
  Medium medium{200'000};
  const Bytes part = pattern(100 * 1024 + 100, 77);  // a cut inside a sector
  {
    Fat32Builder b(medium);
    const std::uint32_t dir = b.make_dir(Fat32Builder::kRootCluster, "o3-bench");
    b.add_file(dir, "disk-scratch.bin", part);
  }
  st::FatStorage fat(medium);
  NEED(fat.mount() == hs::Status::Ok);
  const Bytes pat = pattern(kPatternBytes, 5);
  sv::BenchScratch sc(fat, medium);
  NEED(sc.prepare(st::CardOwner::Tracker, false, pat.data(), kPatternBytes) == BenchResult::Ok);
  CHECK(sc.filled_bytes() == sv::kBenchScratchBytes - part.size());
  CHECK(sc.blocks() == sv::kBenchScratchBlocks);
  CHECK(sc.finish() == BenchResult::Ok);
  const Bytes f = read_file(fat, sv::kBenchScratchPath);
  NEED(f.size() == sv::kBenchScratchBytes);
  CHECK(Bytes(f.begin(), f.begin() + part.size()) == part);  // what was there is untouched
}

TEST_CASE("bench scratch clean: removes the file, keeps the folder and everything else, and is Ok when it is not there") {
  Card card;
  st::FatStorage fat(card.medium);
  NEED(fat.mount() == hs::Status::Ok);
  const Bytes pat = pattern(kPatternBytes, 5);
  sv::BenchScratch sc(fat, card.medium);
  CHECK(sc.clean(st::CardOwner::Tracker, false) == BenchResult::Ok);  // nothing there yet (and the folder is not made by it)
  CHECK_FALSE(exists(fat, sv::kBenchDir));
  NEED(sc.prepare(st::CardOwner::Tracker, false, pat.data(), kPatternBytes) == BenchResult::Ok);
  CHECK(sc.finish() == BenchResult::Ok);
  CHECK(exists(fat, sv::kBenchScratchPath));
  CHECK(sc.clean(st::CardOwner::Host, false) == BenchResult::NotTracker);  // a computer's card: not removed
  CHECK(exists(fat, sv::kBenchScratchPath));
  CHECK(sc.clean(st::CardOwner::Tracker, false) == BenchResult::Ok);
  CHECK_FALSE(exists(fat, sv::kBenchScratchPath));
  CHECK(exists(fat, sv::kBenchDir));  // the save contract never removes a folder
  CHECK(read_file(fat, "README.TXT") == pattern(40, 1));
  CHECK(read_file(fat, "Projects/Song/project.mt") == pattern(2320, 10));
  CHECK(read_file(fat, "Samples/A.WAV") == pattern(700, 12));
  CHECK(sc.clean(st::CardOwner::Tracker, false) == BenchResult::Ok);  // twice
  // ... and it can be made again.
  NEED(sc.prepare(st::CardOwner::Tracker, false, pat.data(), kPatternBytes) == BenchResult::Ok);
  CHECK(sc.filled_bytes() == sv::kBenchScratchBytes);
  CHECK(sc.finish() == BenchResult::Ok);
}

TEST_CASE("bench scratch file: a card taken out and put back ends the run") {
  Card card;
  st::FatStorage fat(card.medium);
  NEED(fat.mount() == hs::Status::Ok);
  const Bytes pat = pattern(kPatternBytes, 5);
  sv::BenchScratch sc(fat, card.medium);
  NEED(sc.prepare(st::CardOwner::Tracker, false, pat.data(), kPatternBytes) == BenchResult::Ok);
  const Bytes blk = pattern(512, 9);
  CHECK(sc.rewrite(0, blk.data(), 1) == BenchResult::Ok);
  fat.release();  // OS-SDSWAP: the volume is gone
  const std::uint64_t written = card.medium.blocks_written;
  CHECK(sc.rewrite(1, blk.data(), 1) == BenchResult::NoCard);
  CHECK(card.medium.blocks_written == written);
  NEED(fat.mount() == hs::Status::Ok);  // the same card back: another mount, the old handle is not used
  CHECK(sc.rewrite(1, blk.data(), 1) == BenchResult::NoCard);
  CHECK(card.medium.blocks_written == written);
  // finish() on that stale handle makes no FatFs call either (no sync, no close): it forgets the handle and says the card changed.
  CHECK(sc.is_open());
  CHECK(sc.finish() == BenchResult::NoCard);
  CHECK_FALSE(sc.is_open());
  CHECK(card.medium.blocks_written == written);
  CHECK(sc.finish() == BenchResult::Ok);  // nothing open any more
  // The object is usable again on the new mount.
  NEED(sc.prepare(st::CardOwner::Tracker, false, pat.data(), kPatternBytes) == BenchResult::Ok);
  CHECK(sc.filled_bytes() == 0u);
  CHECK(sc.rewrite(1, blk.data(), 1) == BenchResult::Ok);
  CHECK(sc.finish() == BenchResult::Ok);
}

TEST_CASE("bench scratch file: finish() does not write to a card that became write protected") {
  Card card;
  st::FatStorage fat(card.medium);
  NEED(fat.mount() == hs::Status::Ok);
  const Bytes pat = pattern(kPatternBytes, 5);
  sv::BenchScratch sc(fat, card.medium);
  NEED(sc.prepare(st::CardOwner::Tracker, false, pat.data(), kPatternBytes) == BenchResult::Ok);
  const Bytes blk = pattern(512, 9);
  CHECK(sc.rewrite(0, blk.data(), 1) == BenchResult::Ok);
  card.medium.protect = true;
  const std::uint64_t written = card.medium.blocks_written;
  CHECK(sc.rewrite(1, blk.data(), 1) == BenchResult::NoCard);
  CHECK(sc.finish() == BenchResult::NoCard);
  CHECK_FALSE(sc.is_open());
  CHECK(card.medium.blocks_written == written);
}

TEST_CASE("bench scratch file: the fill notice comes once, before the first fill byte, and not when the file is there or the gate refuses") {
  {
    Card card;
    st::FatStorage fat(card.medium);
    NEED(fat.mount() == hs::Status::Ok);
    const Bytes pat = pattern(kPatternBytes, 5);
    sv::BenchScratch sc(fat, card.medium);
    NoticeProbe probe;
    probe.medium = &card.medium;
    const std::uint64_t before = card.medium.blocks_written;
    NEED(sc.prepare(st::CardOwner::Tracker, false, pat.data(), kPatternBytes, probe_notice, &probe) == BenchResult::Ok);
    CHECK(probe.calls == 1);
    CHECK(sc.filled_bytes() == sv::kBenchScratchBytes);
    CHECK(probe.written_at_call - before < sv::kBenchScratchBlocks / 8u);  // the folder and the file entry only: the 1 MiB came after
    CHECK(card.medium.blocks_written - before >= sv::kBenchScratchBlocks);
    CHECK(sc.finish() == BenchResult::Ok);
    NEED(sc.prepare(st::CardOwner::Tracker, false, pat.data(), kPatternBytes, probe_notice, &probe) == BenchResult::Ok);  // kept: no fill, no notice
    CHECK(probe.calls == 1);
    CHECK(sc.finish() == BenchResult::Ok);
  }
}

TEST_CASE("bench scratch file: a folder at the scratch path is Denied, not 'the card is full'") {
  Medium medium{200'000};
  {
    Fat32Builder b(medium);
    const std::uint32_t dir = b.make_dir(Fat32Builder::kRootCluster, "o3-bench");
    b.make_dir(dir, "disk-scratch.bin");
  }
  st::FatStorage fat(medium);
  NEED(fat.mount() == hs::Status::Ok);
  const Bytes pat = pattern(kPatternBytes, 5);
  sv::BenchScratch sc(fat, medium);
  NoticeProbe probe;
  probe.medium = &medium;
  CHECK(sc.prepare(st::CardOwner::Tracker, false, pat.data(), kPatternBytes, probe_notice, &probe) == BenchResult::Denied);
  CHECK(sc.last_fatfs_error() == static_cast<int>(FR_DENIED));
  CHECK_FALSE(sc.is_open());
  CHECK(probe.calls == 0);
  CHECK(std::string(sv::bench_result_name(BenchResult::Denied)).find("full") == std::string::npos);
}
