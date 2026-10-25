// SPDX-License-Identifier: GPL-3.0-or-later
//
// The gate of the write arm of `diag bench disk` (platform/mk66/save/bench_scratch.h; OS-USBDISK U1d, Q14) as a pure function of the facts: any card the Tracker owns passes (no marker, OS-ANYCARD), a computer's card
// (owner Host, or no card) is refused FIRST, a busy, unmounted, unsupported or write-protected card is refused, and the scratch path is none of the
// save guard's: no user folder, nothing the save layer may create, rename to or remove. The file itself on a FAT32 volume is platform/mk66/tests/mk66_bench_scratch_test.cpp.
#include <doctest/doctest.h>

#include <o3os/hal/save_guard.h>
#include <o3os/hal/storage.h>
#include <o3os/platform/mk66/save/bench_scratch.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace hs = o3os::hal::storage;
namespace guard = o3os::hal::storage::save;
namespace st = o3os::platform::mk66::storage;
namespace sv = o3os::platform::mk66::save;
using sv::BenchResult;

namespace {

// A good card for the gate: the Tracker's, idle, mounted, FAT32, writable.
sv::BenchFacts good() {
  sv::BenchFacts f;
  f.owner = st::CardOwner::Tracker;
  f.busy = false;
  f.mounted = true;
  f.fs = hs::FsKind::Fat32;
  f.write_protected = false;
  return f;
}

}  // namespace

TEST_CASE("bench scratch gate: the Tracker's idle FAT card, in order, and nothing looked at under a computer") {
  CHECK(sv::bench_gate(good()) == BenchResult::Ok);  // any such card: no marker, no volume check (OS-ANYCARD)

  {  // owner Host (or no card): refused FIRST, whatever else is wrong with the facts: a computer may be writing the card
    sv::BenchFacts f = good();
    f.owner = st::CardOwner::Host;
    CHECK(sv::bench_gate(f) == BenchResult::NotTracker);
    f.owner = st::CardOwner::NoCard;
    CHECK(sv::bench_gate(f) == BenchResult::NotTracker);
    f.owner = st::CardOwner::Host;
    f.busy = true;
    f.mounted = false;
    f.write_protected = true;
    CHECK(sv::bench_gate(f) == BenchResult::NotTracker);
  }
  {  // the other refusals, in order
    sv::BenchFacts f = good();
    f.busy = true;
    CHECK(sv::bench_gate(f) == BenchResult::Busy);
    f = good();
    f.mounted = false;
    CHECK(sv::bench_gate(f) == BenchResult::NoCard);
    f = good();
    f.fs = hs::FsKind::ExFat;
    CHECK(sv::bench_gate(f) == BenchResult::Unsupported);
    f.fs = hs::FsKind::HostDirectory;
    CHECK(sv::bench_gate(f) == BenchResult::Unsupported);
    f = good();
    f.write_protected = true;
    CHECK(sv::bench_gate(f) == BenchResult::WriteProtected);
    for (const hs::FsKind k : {hs::FsKind::Fat12, hs::FsKind::Fat16, hs::FsKind::Fat32}) {
      f = good();
      f.fs = k;
      CHECK(sv::bench_gate(f) == BenchResult::Ok);
    }
  }
}

TEST_CASE("bench scratch path: not inside a user folder, and not a path the save layer may touch") {
  CHECK(guard::classify(sv::kBenchDir) == guard::Place::None);
  CHECK(guard::classify(sv::kBenchScratchPath) == guard::Place::None);
  CHECK_FALSE(guard::may_make_dir(sv::kBenchDir));
  CHECK_FALSE(guard::may_create(sv::kBenchScratchPath));
  CHECK_FALSE(guard::may_remove(sv::kBenchScratchPath));
  CHECK_FALSE(guard::may_rename(sv::kBenchScratchPath, "Projects/Song/project.o3pj"));
  CHECK_FALSE(guard::may_rename("Projects/Song/_o3save.tmp", sv::kBenchScratchPath));
  // The first component of the path is none of the folders the save layer, the loaders and the firmware writer use.
  const char* const user_folders[] = {"Projects", "Samples", "Settings", "Export", "instruments", "AppExport", "Firmware", "Workspace", "modules", "Recordings"};
  const std::size_t first = std::strchr(sv::kBenchScratchPath, '/') - sv::kBenchScratchPath;
  for (const char* folder : user_folders) {
    INFO("folder: " << folder);
    CHECK_FALSE((std::strlen(folder) == first && std::strncmp(folder, sv::kBenchScratchPath, first) == 0));
  }
  CHECK(std::strcmp(sv::kBenchDir, "o3-bench") == 0);
  CHECK(sv::kBenchScratchBytes == 1024u * 1024u);
}

