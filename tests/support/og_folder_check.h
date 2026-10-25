// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Helpers of the OG folder loader tests (OS-007e), shared by the host-directory run (og_folder_test.cpp) and the FAT32 run
// (og_folder_fat_test.cpp): drive the loader to its end, read the same files straight through the formats readers, and check that two projects
// write to the same OG files.

#include <doctest/doctest.h>

#include "og_folder_fixture.h"

#include <o3os/app/og_folder.h>
#include <o3os/formats/og.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace o3os::test::og {

namespace formats = ::o3os::formats;

inline o3os::app::OgFolderLoader::State run_loader(o3os::app::OgFolderLoader& l, std::size_t budget, unsigned* steps = nullptr) {
  unsigned n = 0;
  o3os::app::OgFolderLoader::State s = o3os::app::OgFolderLoader::State::Running;
  while (s == o3os::app::OgFolderLoader::State::Running && n < 200000) {
    s = l.step(budget);
    ++n;
  }
  if (steps != nullptr) {
    *steps = n;
  }
  return s;
}

inline o3os::app::OgFolderLoader& loader() {
  static o3os::app::OgFolderLoader l;  // about 8 KB: not for a stack
  return l;
}

// The same files straight through the formats readers, in memory: the project, every pattern file the name classifier takes, the instruments with
// the .mti before the .pti of a slot (so the .pti is what is left, as the loader's precedence says).
inline void load_direct(const std::vector<FolderFile>& files, o3os::model::ModelArena& arena) {
  static formats::og::ProjectReader project;
  static formats::og::PatternReader pattern;
  static formats::og::InstrumentReader mti(false);
  static formats::og::InstrumentReader pti(true);
  for (const FolderFile& f : files) {
    if (f.path == "project.mt") {
      CHECK(run_reader(project, f.bytes, arena) == formats::Progress::Done);
    }
  }
  for (const FolderFile& f : files) {
    const std::string name = f.path.substr(f.path.rfind('/') == std::string::npos ? 0 : f.path.rfind('/') + 1);
    const bool in_patterns = f.path.rfind("patterns/", 0) == 0 && f.path.find('/', 9) == std::string::npos;
    const formats::og::FileName fn = formats::og::classify_file_name(name.c_str());
    if (in_patterns && fn.file_class == formats::og::FileClass::Pattern) {
      formats::ReadParams p;
      p.index = fn.index;
      (void)run_reader(pattern, f.bytes, arena, p);  // pattern_07 fails and leaves nothing behind
    }
  }
  for (const bool want_pti : {false, true}) {
    for (const FolderFile& f : files) {
      if (f.path.rfind("instruments/", 0) != 0) {
        continue;
      }
      const formats::og::FileName fn = formats::og::classify_file_name(f.path.c_str() + 12);
      const bool is_pti = fn.file_class == formats::og::FileClass::Pti;
      if ((fn.file_class != formats::og::FileClass::Mti && !is_pti) || is_pti != want_pti) {
        continue;
      }
      formats::ReadParams p;
      p.index = fn.index;
      p.size_hint = static_cast<std::uint32_t>(f.bytes.size());
      (void)run_reader(is_pti ? pti : mti, f.bytes, arena, p);
    }
  }
}

// The two projects write to the same OG files: the project file (which carries the pattern bitmask and the song), every pattern and every instrument slot.
inline void check_same_model(const o3os::model::Project& a, const o3os::model::Project& b) {
  Bytes pa, pb;
  formats::og::ProjectWriter w1, w2;
  CHECK(run_writer(w1, a, pa) == formats::Progress::Done);
  CHECK(run_writer(w2, b, pb) == formats::Progress::Done);
  CHECK(pa == pb);
  for (unsigned id = 1; id <= 255; ++id) {
    CHECK((a.pattern(id) != nullptr) == (b.pattern(id) != nullptr));
    if (a.pattern(id) != nullptr && b.pattern(id) != nullptr) {
      formats::og::PatternWriter pw1, pw2;
      formats::WriteParams wp;
      wp.index = static_cast<std::uint16_t>(id);
      Bytes x, y;
      CHECK(run_writer(pw1, a, x, wp) == formats::Progress::Done);
      CHECK(run_writer(pw2, b, y, wp) == formats::Progress::Done);
      CHECK(x == y);
    }
  }
  for (unsigned slot = 0; slot < 48; ++slot) {
    CHECK((a.instrument(slot) != nullptr) == (b.instrument(slot) != nullptr));
    if (a.instrument(slot) != nullptr && b.instrument(slot) != nullptr) {
      formats::og::InstrumentWriter iw1, iw2;  // the head of a .pti: every field the model holds, without the struct's padding
      formats::WriteParams wp;
      wp.index = static_cast<std::uint16_t>(slot);
      Bytes x, y;
      CHECK(run_writer(iw1, a, x, wp) == formats::Progress::Done);
      CHECK(run_writer(iw2, b, y, wp) == formats::Progress::Done);
      CHECK(x == y);
    }
  }
}

}  // namespace o3os::test::og
