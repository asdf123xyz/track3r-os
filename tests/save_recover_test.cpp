// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-007o: what a power cut inside a replacing save leaves is not only "whole in some name" (atomic_save_test.cpp) but REACHABLE: the File list shows the folder, OPEN loads it
// (the backup when the project file is missing) and the next SAVE heals the name; the device settings load from device.o3st.bak when device.o3st is missing. Every cut point of
// the save job's power-loss harness, over real native project bytes and real settings bytes, ends with a project (and settings) that loads.
#include <doctest/doctest.h>

#include "support/mem_card.h"
#include "support/need.h"
#include "support/project_fixture.h"
#include "support/save_source.h"

#include <o3os/app/atomic_save.h>
#include <o3os/app/dir_scan.h>
#include <o3os/app/native_load.h>
#include <o3os/app/save_service.h>
#include <o3os/formats/settings.h>
#include <o3os/model/params.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace app = o3os::app;
namespace hs = o3os::hal::storage;
using o3os::test::MemCard;
using o3os::test::VecSource;
using o3os::test::run;
using Bytes = MemCard::Bytes;
using Mode = app::AtomicSave::Mode;

// Like NEED, for a function that returns a value: checks the condition and runs `action` (a return) when it is false.
#define NEED_OR(cond, action)                           \
  do {                                                  \
    const bool o3_need_ok = static_cast<bool>(cond);    \
    CHECK_MESSAGE(o3_need_ok, #cond);                   \
    if (!o3_need_ok) {                                  \
      action;                                           \
    }                                                   \
  } while (0)

namespace {

constexpr const char* kFile = "Projects/Track 01/project.o3pj";
constexpr const char* kBak = "Projects/Track 01/project.o3pj.bak";

// Native project bytes of the two arenas the sweep tells apart by the project's name.
Bytes project_bytes(unsigned arena_index, const char* name) {
  o3os::model::ModelArena& a = o3os::test::arena(arena_index);
  o3os::test::fill_small(a.project);
  (void)a.project.set_name(name, std::strlen(name));
  Bytes out;
  (void)o3os::test::save(a.project, out);
  return out;
}

Bytes saved_project(const o3os::model::Project& p) {
  Bytes out;
  (void)o3os::test::save(p, out);
  return out;
}

// What the File list and OPEN do with the card: one scan of Projects/, then the load the entry's tag asks for. The bytes the loaded project saves as ("" when nothing loaded) and the tag.
struct Opened {
  std::size_t entries = 0;
  app::EntryTag tag = app::EntryTag::None;
  bool loaded = false;
  Bytes bytes;
  std::string folder;
};

Opened scan_and_open(MemCard& card, o3os::model::ModelArena& target) {
  Opened out;
  app::DirScan scan;
  NEED_OR(scan.begin(card, "Projects", app::kProjectFilter) == app::DirScan::Begin::Started, return out);
  for (unsigned i = 0; scan.step(2000) == o3os::core::JobStatus::Running; ++i) {
    NEED_OR(i < 10000u, return out);
  }
  out.entries = scan.index().count();
  if (out.entries != 1u) {
    return out;
  }
  out.tag = scan.index().tag(0);
  out.folder = std::string("Projects/") + scan.index().name(0);
  target.project.reset();
  app::NativeFolderLoader loader;
  NEED_OR(loader.begin(card, out.folder.c_str(), target, out.tag == app::EntryTag::Recover), return out);
  for (unsigned i = 0; loader.step(4096) == app::NativeFolderLoader::State::Running; ++i) {
    NEED_OR(i < 10000u, return out);
  }
  out.loaded = loader.state() == app::NativeFolderLoader::State::Done;
  if (out.loaded) {
    out.bytes = saved_project(target.project);
  }
  return out;
}

}  // namespace

TEST_CASE("recover: a power cut at every step of a replacing project save leaves a folder the File list shows, OPEN loads and SAVE heals") {
  const Bytes old_file = project_bytes(0, "old version");
  const Bytes old_bak = project_bytes(0, "older version");
  const Bytes fresh = project_bytes(0, "fresh version");
  const Bytes later = project_bytes(0, "later version");
  NEED(old_file != fresh);
  NEED(old_file != old_bak);

  const auto setup = [&](MemCard& card, bool with_bak) {
    card.make_dir_now("Projects");
    card.make_dir_now("Projects/Track 01");
    card.put(kFile, old_file);
    if (with_bak) {
      card.put(kBak, old_bak);
    }
  };
  for (int with_bak = 0; with_bak < 2; ++with_bak) {
    std::size_t total = 0;
    {
      MemCard card;
      setup(card, with_bak != 0);
      app::AtomicSave job;
      VecSource source(fresh);
      NEED(job.begin(card, card, "Projects/Track 01", "project.o3pj", Mode::Replace, source, "TRACK 01"));
      run(job);
      NEED(job.done());
      total = card.ops;
    }
    unsigned recover_cuts = 0;
    for (std::size_t cut = 0; cut <= total; ++cut) {
      INFO("with older backup: " << with_bak << ", power cut at mutating call " << cut << " of " << total);
      MemCard card;
      setup(card, with_bak != 0);
      card.cut_at = cut;
      app::AtomicSave job;
      VecSource source(fresh);
      NEED(job.begin(card, card, "Projects/Track 01", "project.o3pj", Mode::Replace, source, "TRACK 01"));
      run(job);
      card.revive();  // the next boot

      Opened first = scan_and_open(card, o3os::test::arena(1));
      CHECK(first.entries == 1u);  // the folder is in the list, whatever the cut left
      CHECK((first.tag == app::EntryTag::Native || first.tag == app::EntryTag::Recover));
      CHECK(first.loaded);
      // The project that opens is the old version or the new one, whole - never anything else.
      CHECK((first.bytes == old_file || first.bytes == fresh));
      if (first.tag == app::EntryTag::Recover) {
        ++recover_cuts;
        CHECK_FALSE(card.has(kFile));
        CHECK(first.bytes == old_file);  // the backup is the old version
      }
      CHECK(card.violations == 0);

      // SAVE from what OPEN loaded: a Replace into the same folder. It heals the name and goes on.
      VecSource again(later);
      NEED(job.begin(card, card, "Projects/Track 01", "project.o3pj", Mode::Replace, again, "TRACK 01"));
      run(job);
      CHECK(job.done());
      CHECK(job.healed() == (first.tag == app::EntryTag::Recover));
      CHECK(card.same(kFile, later));
      CHECK(card.has(kBak));
      CHECK_FALSE(card.has("Projects/Track 01/_o3save.tmp"));
      Opened second = scan_and_open(card, o3os::test::arena(1));
      CHECK(second.tag == app::EntryTag::Native);
      CHECK(second.bytes == later);
      if (first.tag == app::EntryTag::Recover) {
        CHECK(card.same(kBak, old_file));  // the recovered version is the new file's backup
      }
      CHECK(card.violations == 0);
    }
    CHECK(recover_cuts >= 1u);  // the window between the two renames exists in the harness: RECOVER was exercised
  }
}

TEST_CASE("recover: the device settings load from device.o3st.bak when device.o3st is missing, at every cut of a settings save") {
  const auto make = [](float theme) {
    o3os::model::Settings s;
    s.init(o3os::core::ParamKind::Device);
    CHECK(s.set(o3os::model::device_setting::Theme, theme) == o3os::model::Status::Ok);
    std::uint8_t buffer[o3os::formats::settings::kMaxFileBytes];
    const std::size_t n = o3os::formats::settings::write(s, buffer, sizeof buffer);
    NEED_OR(n != 0u, return Bytes{});
    return Bytes(buffer, buffer + n);
  };
  const Bytes old_file = make(1.0f);
  const Bytes fresh = make(2.0f);
  NEED(old_file != fresh && !old_file.empty());

  const auto setup = [&](MemCard& card) {
    card.make_dir_now("Settings");
    card.put("Settings/device.o3st", old_file);
  };
  std::size_t total = 0;
  {
    MemCard card;
    setup(card);
    app::AtomicSave job;
    VecSource source(fresh);
    NEED(job.begin(card, card, "Settings", "device.o3st", Mode::Replace, source, "SETTINGS"));
    run(job);
    NEED(job.done());
    total = card.ops;
  }
  unsigned from_backup = 0;
  for (std::size_t cut = 0; cut <= total; ++cut) {
    INFO("power cut at mutating call " << cut << " of " << total);
    MemCard card;
    setup(card);
    card.cut_at = cut;
    app::AtomicSave job;
    VecSource source(fresh);
    NEED(job.begin(card, card, "Settings", "device.o3st", Mode::Replace, source, "SETTINGS"));
    run(job);
    card.revive();
    o3os::model::Settings loaded;
    CHECK(app::load_device_settings(card, loaded));  // the user's settings are there, whatever the cut left
    const float theme = loaded.get(o3os::model::device_setting::Theme);
    CHECK((theme == 1.0f || theme == 2.0f));
    if (!card.has("Settings/device.o3st")) {
      ++from_backup;
      CHECK(theme == 1.0f);  // the backup is the old file
    }
    // The next save of the settings heals the name.
    VecSource again(old_file);
    NEED(job.begin(card, card, "Settings", "device.o3st", Mode::Replace, again, "SETTINGS"));
    run(job);
    CHECK(job.done());
    CHECK(card.same("Settings/device.o3st", old_file));
  }
  CHECK(from_backup >= 1u);
  // The plain rules stay: the file wins over the backup, a damaged file is not replaced by the backup, no file and no backup is the defaults.
  MemCard card;
  card.make_dir_now("Settings");
  card.put("Settings/device.o3st", fresh);
  card.put("Settings/device.o3st.bak", old_file);
  o3os::model::Settings loaded;
  CHECK(app::load_device_settings(card, loaded));
  CHECK(loaded.get(o3os::model::device_setting::Theme) == 2.0f);
  Bytes damaged = fresh;
  damaged[10] ^= 0xFF;
  card.put("Settings/device.o3st", damaged);
  CHECK_FALSE(app::load_device_settings(card, loaded));
  CHECK(loaded.is_default());
  card.files.erase("settings/device.o3st");
  CHECK(app::load_device_settings(card, loaded));  // (the backup alone loads)
  CHECK(loaded.get(o3os::model::device_setting::Theme) == 1.0f);
  card.files.erase("settings/device.o3st.bak");
  CHECK_FALSE(app::load_device_settings(card, loaded));
  CHECK(loaded.is_default());
}
