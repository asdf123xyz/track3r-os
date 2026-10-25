// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-007e: the OG project folder of og_folder_test.cpp, now on a FAT32 volume (long names, one-sector clusters) read through FatFs, and then
// through the whole card stack (the SD logic on a software card under FatFs): a project loaded the way the image loads it. The files of the
// volume are listed in the order they were written, so unlike the host directory the order of the entries is known, and the .mti that sits beside a
// .pti is read first. Host only (the loader's tests are; the storage stack alone also runs on QEMU, in platform/mk66/tests).
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/og_folder_check.h"

#include "../platform/mk66/tests/mk66_storage_fakes.h"

#include <o3os/app/og_folder.h>
#include <o3os/hal/storage.h>
#include <o3os/platform/mk66/storage/fat_storage.h>
#include <o3os/platform/mk66/storage/sd_card.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace hs = o3os::hal::storage;
namespace st = o3os::platform::mk66::storage;
using o3os::app::OgFolderLoader;
using namespace o3os::test::og;

namespace {

// Writes `files` under `folder` (each parent directory made once, in the order they first appear).
void write_tree(fakes::Fat32Builder& fat, const std::string& folder, const std::vector<FolderFile>& files) {
  std::map<std::string, std::uint32_t> dirs;
  dirs[""] = fakes::Fat32Builder::kRootCluster;
  const auto dir_of = [&](const std::string& path) {  // the cluster of a directory path, created on first use
    std::string so_far;
    std::uint32_t cluster = fakes::Fat32Builder::kRootCluster;
    std::size_t at = 0;
    while (at < path.size()) {
      std::size_t slash = path.find('/', at);
      if (slash == std::string::npos) {
        slash = path.size();
      }
      so_far += (so_far.empty() ? "" : "/") + path.substr(at, slash - at);
      const auto it = dirs.find(so_far);
      if (it == dirs.end()) {
        cluster = fat.make_dir(cluster, path.substr(at, slash - at));
        dirs[so_far] = cluster;
      } else {
        cluster = it->second;
      }
      at = slash + 1;
    }
    return cluster;
  };
  for (const FolderFile& f : files) {
    const std::string full = folder + "/" + f.path;
    const std::size_t slash = full.rfind('/');
    fat.add_file(dir_of(full.substr(0, slash)), full.substr(slash + 1), f.bytes);
  }
}

}  // namespace

TEST_CASE("og folder on FAT32: the same folder loads through FatFs to the same model") {
  fakes::Medium medium(200'000);
  const std::vector<FolderFile> files = synthetic_folder();
  {
    fakes::Fat32Builder fat(medium);
    write_tree(fat, "Workspace", files);
  }
  const std::uint64_t before = medium.fingerprint();
  st::FatStorage card(medium);
  NEED(card.mount() == hs::Status::Ok);
  OgFolderLoader& l = loader();
  o3os::model::ModelArena& arena = o3os::test::og::arena(0);
  arena.project.reset();
  NEED(l.begin(card, "Workspace", arena));
  unsigned steps = 0;
  NEED(run_loader(l, 600, &steps) == OgFolderLoader::State::Done);
  CHECK(steps > 10u);

  const o3os::app::OgLoadReport& rep = l.report();
  CHECK(rep.project_ok);
  CHECK(rep.version == 16);
  CHECK(rep.patterns == kFolderPatternsGood);
  CHECK(rep.patterns_failed == kFolderPatternsBad);
  CHECK(rep.instruments_failed == kFolderInstrumentsBad);
  // The volume lists instrument_05.mti before "05 Clap.pti": both are read, the .pti last, nothing is skipped.
  CHECK(rep.instruments == kFolderPtiFiles + kFolderMtiFiles);
  CHECK(rep.instruments_superseded == 0u);
  CHECK(rep.pcm_frames == kFolderPcmFrames);
  CHECK(rep.files_ignored == kFolderIgnored);
  CHECK(rep.storage_error == hs::Status::Ok);
  CHECK(card.open_files() == 0u);
  CHECK(card.open_dirs() == 0u);
  char line[100];
  const std::size_t n = o3os::app::describe_load(l, line, sizeof line);
  CHECK(std::string(line, n) == "Workspace: v16, 4 patterns, 5 instruments, 2 failed");
  CHECK(n == std::strlen(line));
  char tiny[12];
  CHECK(o3os::app::describe_load(l, tiny, sizeof tiny) == 11u);  // cut to fit, terminated
  CHECK(std::string(tiny) == "Workspace: ");
  NEED(arena.project.instrument(4) != nullptr);
  CHECK(std::string(arena.project.instrument(4)->name) == "New Clap");

  o3os::model::ModelArena& direct = o3os::test::og::arena(1);
  load_direct(files, direct);
  check_same_model(arena.project, direct.project);
  CHECK(medium.fingerprint() == before);  // no sector of the volume changed
}

TEST_CASE("og folder on FAT32: a slice of any size ends in the same model") {
  fakes::Medium medium(200'000);
  const std::vector<FolderFile> files = synthetic_folder();
  {
    fakes::Fat32Builder fat(medium);
    write_tree(fat, "Projects/My Song", files);
  }
  st::FatStorage card(medium);
  NEED(card.mount() == hs::Status::Ok);
  OgFolderLoader& l = loader();
  o3os::model::ModelArena& arena = o3os::test::og::arena(0);
  o3os::model::ModelArena& direct = o3os::test::og::arena(1);
  load_direct(files, direct);
  for (const std::size_t budget : {std::size_t{1}, std::size_t{512}, std::size_t{4096}, std::size_t{65536}, std::size_t{1} << 30}) {
    INFO(budget);
    arena.project.reset();
    NEED(l.begin(card, "Projects/My Song", arena));
    NEED(run_loader(l, budget) == OgFolderLoader::State::Done);
    CHECK(l.report().patterns == kFolderPatternsGood);
    check_same_model(arena.project, direct.project);
  }
}

TEST_CASE("og folder on the card stack: SD logic, FatFs, loader; every command a read, the card's sectors untouched") {
  fakes::Medium medium(200'000);
  const std::vector<FolderFile> files = synthetic_folder();
  {
    fakes::Fat32Builder fat(medium);
    write_tree(fat, "Workspace", files);
  }
  const std::uint64_t before = medium.fingerprint();
  fakes::FakeCardSpec spec;
  fakes::FakeSdBus bus(medium, spec);
  st::sd::Card sd_card(bus);
  st::FatStorage storage(sd_card);
  NEED(storage.mount() == hs::Status::Ok);
  CHECK(storage.volume().kind == hs::FsKind::Fat32);

  OgFolderLoader& l = loader();
  o3os::model::ModelArena& arena = o3os::test::og::arena(0);
  arena.project.reset();
  NEED(l.begin(storage, "Workspace", arena));
  NEED(run_loader(l, 2048) == OgFolderLoader::State::Done);
  CHECK(l.report().project_ok);
  CHECK(l.report().patterns == kFolderPatternsGood);
  CHECK(l.report().instruments == kFolderPtiFiles + kFolderMtiFiles);

  o3os::model::ModelArena& direct = o3os::test::og::arena(1);
  load_direct(files, direct);
  check_same_model(arena.project, direct.project);

  // The card saw reads and nothing else.
  CHECK(sd_card.counters().failures == 0u);
  CHECK(sd_card.counters().blocks > 100u);
  CHECK(bus.reads_single + bus.reads_multi > 0u);
  for (const fakes::FakeSdBus::Seen& s : bus.log) {
    CHECK(st::sd::known(s.cmd));
    const unsigned index = st::sd::wire_index(s.cmd);
    CHECK((st::sd::is_app(s.cmd) || (index != 24u && index != 25u && index != 27u && index != 42u && index != 56u)));
  }
  CHECK(medium.fingerprint() == before);
}
