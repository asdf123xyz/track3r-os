// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/og_fixture.h"

#include <o3os/formats/native.h>
#include <o3os/formats/og.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace o3os;
using namespace o3os::test::og;
using formats::Progress;
using o3os::test::MemorySource;
namespace og_ns = o3os::formats::og;

// The settings of an OG project that the model has no field for (delay, limiter, reverb, inputs, performance, the
// 1.3 and 1.7 blocks, MIDI velocity) travel in an OGPM chunk of the project, through the native format, and come
// back byte for byte on Export (OS-004b, ADR 0006/0007). Session state, the pattern bitmask and uninitialised bytes
// are not carried.

namespace {

og_ns::ProjectReader& reader() {
  static og_ns::ProjectReader r;
  return r;
}
og_ns::ProjectWriter& writer() {
  static og_ns::ProjectWriter w;
  return w;
}

constexpr std::uint32_t kAllSettings = og_ns::kDropDelay | og_ns::kDropLimiter | og_ns::kDropReverb |
                                       og_ns::kDropInputs | og_ns::kDropPerformance | og_ns::kDropMidiVelocity |
                                       og_ns::kDropExt13 | og_ns::kDropExt19;

Progress import_project(const Bytes& f, int a, og_ns::ImportReport* rep = nullptr) {
  const Progress p = run_reader(reader(), f, arena(a));
  if (rep != nullptr) *rep = reader().import_report();
  return p;
}

// Tracks 9-16 as an import of the OG's files makes them (9-12 audio, 13-16 MIDI), so Export has nothing to count about track types.
void og_tracks(model::Project& proj) {
  for (std::uint32_t slot = 8; slot < model::kMaxTracks; ++slot) {
    model::TrackSlot t = *proj.track(slot);
    t.type = slot < 12 ? model::TrackType::Audio : model::TrackType::Midi;  // what Import makes of them (OS-TRK912)
    CHECK(proj.set_track(slot, t) == model::Status::Ok);
  }
}

Bytes export_project(const model::Project& p) {
  Bytes out;
  CHECK(run_writer(writer(), p, out) == Progress::Done);
  return out;
}

// The bytes Export rebuilds or defaults rather than carries (project.mt, v16): header, session state, padding, the
// uninitialised bytes behind the name, the CRC.
bool normalised(std::size_t i) {
  return (i >= 4 && i < 16) || (i >= 0x10F && i <= 0x119) || i == 0x11B || i == 0x11F || i == 0x122 || i == 0x123 ||
         i == 0x12B || i == 0x13A || i == 0x13B || i == 0x140 || i == 0x141 || i == 0x1BE || i == 0x1BF ||
         (i >= 0x1C6 && i < 0x2F5) || (i >= 0x82C && i < 0x90C) || i >= 0x90C;
}

const std::uint8_t* chunk_of(const model::Project& p, std::uint32_t id, std::uint32_t* length = nullptr) {
  for (std::uint32_t i = 0; i < p.unknown_count(); ++i) {
    if (p.unknown(i)->id == id) {
      if (length != nullptr) *length = p.unknown(i)->length;
      return p.unknown_data(i);
    }
  }
  return nullptr;
}

Bytes native_bytes(const model::Project& p) {
  formats::native::Writer w;
  test::VectorSink sink;
  w.begin(p, sink);
  while (w.step(4096) == Progress::Running) {}
  return sink.bytes;
}

}  // namespace

TEST_CASE("og settings: non-default values in every group come back byte for byte, through the native format too") {
  ProjectSpec spec;
  spec.fill = ProjectSpec::Fill::Groups;
  const Bytes in = project_bytes(spec);
  og_ns::ImportReport rep;
  NEED(import_project(in, 0, &rep) == Progress::Done);
  CHECK(rep.kept_groups == (kAllSettings & ~og_ns::kDropMidiVelocity));  // version 16 has no version-7 velocities
  CHECK(rep.dropped_groups == (og_ns::kDropSession | og_ns::kDropPatternBitmask));
  model::Project& proj = arena(0).project;
  std::uint32_t len = 0;
  const std::uint8_t* chunk = chunk_of(proj, og_ns::kChunkSettings, &len);
  NEED(chunk != nullptr);
  std::uint16_t groups = 0;
  CHECK(og_ns::inspect_settings(chunk, len, groups));
  CHECK(groups == rep.kept_groups);
  CHECK(len == 4 + 4 + 9 + 16 + 10 + 144 + 306 + 460);
  CHECK(chunk[0] == 0x01);
  CHECK(chunk[1] == 0x00);
  og_tracks(proj);

  const Bytes out = export_project(proj);
  NEED(out.size() == in.size());
  // every byte of every group is the input's
  for (const GroupRange& g : group_ranges(16)) {
    for (std::size_t i = g.begin; i < g.end; ++i) {
      if (out[i] != in[i]) CHECK_MESSAGE(false, "group byte " << i);
    }
  }
  // and the whole file differs from the input only where Export rebuilds or defaults
  unsigned stray = 0;
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] != out[i] && !normalised(i)) ++stray;
  }
  CHECK(stray == 0);
  CHECK(writer().export_report().drops.defaulted_groups == (og_ns::kDropSession | og_ns::kDropMidiVelocity));  // v16 has no version-7 velocities; all else was carried
  CHECK(!writer().export_report().drops.any_lost());

  // through the native format: save, load into the other arena, export: the same file
  const Bytes nat = native_bytes(proj);
  formats::native::Reader nr;
  MemorySource src(nat);
  nr.begin(src, arena(1));
  while (nr.step(4096) == Progress::Running) {}
  NEED(nr.progress() == Progress::Done);
  CHECK(nr.report().unknown_kept == 1);
  CHECK(nr.report().unknown_dropped == 0);
  CHECK(export_project(arena(1).project) == out);
  // and reading the export again gives a model that saves to the same native bytes
  NEED(import_project(out, 0) == Progress::Done);
  CHECK(native_bytes(arena(0).project) == nat);
}

TEST_CASE("og settings: one group differs, one group is kept; none differs, none is") {
  ProjectSpec spec;
  Bytes f = project_bytes(spec);
  og_ns::ImportReport rep;
  model::Project& proj = arena(0).project;
  NEED(import_project(f, 0, &rep) == Progress::Done);
  CHECK(rep.kept_groups == 0);
  CHECK(proj.unknown_count() == 0);  // nothing to keep: no chunk at all
  f[0x11A] = 34;                     // delay feedback
  seal(f);
  NEED(import_project(f, 0, &rep) == Progress::Done);
  CHECK(rep.kept_groups == og_ns::kDropDelay);
  std::uint32_t len = 0;
  const std::uint8_t* chunk = chunk_of(proj, og_ns::kChunkSettings, &len);
  NEED(chunk != nullptr);
  CHECK(len == 4 + 4);
  CHECK(chunk[3] == 0x02);  // bit 1 of the big-endian group mask
  CHECK(chunk[4] == 34);
  og_tracks(proj);
  const Bytes out = export_project(proj);
  CHECK(out[0x11A] == 34);
  CHECK(out[0x11C] == 0xF4);  // the other delay bytes are the defaults (500 ms, parameters 0xC3)
  CHECK(out[0x11E] == 0xC3);
  CHECK(writer().export_report().drops.defaulted_groups == (og_ns::kDropSession | (kAllSettings & ~og_ns::kDropDelay)));
  // a padding byte inside the span of a group is not carried
  Bytes g = project_bytes(spec);
  g[0x122] = 0xAA;  // limiter padding
  g[0x11B] = 0xBB;  // delay padding
  seal(g);
  NEED(import_project(g, 0, &rep) == Progress::Done);
  CHECK(rep.kept_groups == 0);
}

TEST_CASE("og settings: a project of 1.3 or 1.4 has its relocated value moved to 0x309, where the later versions keep it") {
  for (unsigned v : {11u, 12u, 13u, 14u}) {
    ProjectSpec spec;
    spec.version = v;
    spec.fill = ProjectSpec::Fill::Groups;
    const Bytes in = project_bytes(spec);
    og_ns::ImportReport rep;
    NEED(import_project(in, 0, &rep) == Progress::Done);
    CHECK(rep.kept_groups == (og_ns::kDropDelay | og_ns::kDropLimiter | og_ns::kDropReverb | og_ns::kDropInputs |
                              og_ns::kDropPerformance | og_ns::kDropExt13));
    og_tracks(arena(0).project);
    const Bytes out = export_project(arena(0).project);
    if (v <= 12) {  // 0x4F7 held the value; later versions keep it at 0x309
      CHECK(in[0x4F7] == 77);
      CHECK(out[0x4F7] == 0);
      CHECK(out[0x309] == 77);
      for (unsigned i = 1; i < 16; ++i) CHECK(out[0x309 + i] == 0);
    } else {
      for (unsigned i = 0; i < 16; ++i) CHECK(out[0x309 + i] == in[0x309 + i]);
      CHECK(out[0x4F7] == in[0x4F7]);
    }
    for (std::size_t i = 0x4DE; i < 0x600; ++i) {
      if (i == 0x4F7) continue;
      if (in[i] != out[i]) CHECK_MESSAGE(false, "version " << v << " byte " << i);
    }
    for (const GroupRange& g : group_ranges(16)) {  // the groups the version has: the same bytes (the 1.7 block is zero)
      if (g.begin == 0x4DE || g.begin == 0x309 || g.begin == 0x600 || g.begin == 0x643) continue;
      for (std::size_t i = g.begin; i < g.end; ++i) {
        if (in[i] != out[i]) CHECK_MESSAGE(false, "version " << v << " group byte " << i);
      }
    }
    CHECK(writer().export_report().drops.defaulted_groups ==
          (og_ns::kDropSession | og_ns::kDropMidiVelocity | og_ns::kDropExt19));
  }
}

TEST_CASE("og settings: the MIDI velocity of the old versions goes to the channel records") {
  for (unsigned v : {2u, 6u, 7u}) {
    ProjectSpec spec;
    spec.version = v;
    spec.fill = ProjectSpec::Fill::Groups;
    const Bytes in = project_bytes(spec);
    og_ns::ImportReport rep;
    NEED(import_project(in, 0, &rep) == Progress::Done);
    CHECK((rep.kept_groups & og_ns::kDropMidiVelocity) != 0);
    og_tracks(arena(0).project);
    const Bytes out = export_project(arena(0).project);
    for (unsigned ch = 0; ch < 16; ++ch) {
      CHECK_MESSAGE(out[0x4FD + 16 * ch + 1] == in[0x309 + ch], "version " << v << " channel " << ch);
      CHECK(out[0x4FD + 16 * ch] == ch);          // the rest of the record is the default
      CHECK(out[0x4FD + 16 * ch + 2] == 0x01);
      CHECK(out[0x4FD + 16 * ch + 7] == 0x80);
    }
    CHECK(out[0x309] == 86);                      // 0x309 keeps its v16 meaning
    CHECK(writer().export_report().drops.defaulted_groups ==
          (og_ns::kDropSession | (v == 2 ? static_cast<std::uint32_t>(og_ns::kDropReverb) : 0u) | og_ns::kDropExt13 | og_ns::kDropExt19));
  }
  // 127, the default, is not worth keeping
  ProjectSpec d;
  d.version = 7;
  og_ns::ImportReport rep;
  NEED(import_project(project_bytes(d), 0, &rep) == Progress::Done);
  CHECK(rep.kept_groups == 0);
}

TEST_CASE("og settings: a chunk that is not valid is ignored and counted, the first valid one wins") {
  model::Project& proj = arena(0).project;
  auto put = [&proj](std::uint32_t id, const Bytes& payload) {
    CHECK(proj.unknown_begin(id, static_cast<std::uint32_t>(payload.size())) == model::Status::Ok);
    CHECK(proj.unknown_append(payload.data(), static_cast<std::uint32_t>(payload.size())) == model::Status::Ok);
  };
  // a valid chunk with the delay group: version 1.0, groups 0x0002, 4 bytes
  const Bytes delay = {0x01, 0x00, 0x00, 0x02, 77, 0x10, 0x20, 0x30};
  auto result = [&](std::initializer_list<Bytes> chunks, Bytes* out_file = nullptr) {
    proj.reset();
    og_tracks(proj);
    for (const Bytes& c : chunks) put(og_ns::kChunkSettings, c);
    Bytes out = export_project(proj);
    if (out_file != nullptr) *out_file = out;
    return writer().export_report().drops;
  };
  Bytes out;
  og_ns::ExportDrops d = result({delay}, &out);
  CHECK(d.settings_ignored == 0);
  CHECK(out[0x11A] == 77);
  CHECK(out[0x11C] == 0x10);
  CHECK(out[0x11D] == 0x20);
  CHECK(out[0x11E] == 0x30);
  // too long, too short
  Bytes longer = delay;
  longer.push_back(0);
  d = result({longer}, &out);
  CHECK(d.settings_ignored == 1);
  CHECK(out[0x11A] == 50);  // the default
  Bytes shorter = delay;
  shorter.pop_back();
  CHECK(result({shorter}).settings_ignored == 1);
  CHECK(result({Bytes{0x01, 0x00, 0x00}}).settings_ignored == 1);
  CHECK(result({Bytes{}}).settings_ignored == 1);
  // a newer major version, a group that does not exist, the bitmask and the session bits, which are never carried
  Bytes major2 = delay;
  major2[0] = 0x02;
  CHECK(result({major2}).settings_ignored == 1);
  for (unsigned bits : {0x01u, 0x80u}) {  // groups 0x0001 (session) and 0x0080 (bitmask): never carried
    Bytes b = delay;
    b[3] = static_cast<std::uint8_t>(bits);
    CHECK(result({b}).settings_ignored == 1);
  }
  Bytes high = delay;
  high[2] = 0x04;  // group 10: nobody's
  CHECK(result({high}).settings_ignored == 1);
  // a newer minor version only appends: read, the rest skipped
  Bytes minor = delay;
  minor[1] = 0x02;
  minor.push_back(0xEE);
  minor.push_back(0xEF);
  d = result({minor}, &out);
  CHECK(d.settings_ignored == 0);
  CHECK(out[0x11A] == 77);
  Bytes short_minor = delay;  // ... but a newer minor is not shorter than 1.0
  short_minor[1] = 0x02;
  short_minor.pop_back();
  CHECK(result({short_minor}).settings_ignored == 1);
  // the first valid chunk is the one used; a second one, valid or not, is counted and ignored
  Bytes other = delay;
  other[4] = 99;
  d = result({shorter, delay, other}, &out);
  CHECK(d.settings_ignored == 2);
  CHECK(out[0x11A] == 77);
  // two groups, in bit order
  Bytes two = {0x01, 0x00, 0x00, 0x0A, 1, 2, 3, 4};  // groups 1 (delay) and 3 (reverb)
  for (unsigned i = 0; i < 16; ++i) two.push_back(static_cast<std::uint8_t>(0x40 + i));
  d = result({two}, &out);
  CHECK(d.settings_ignored == 0);
  CHECK(out[0x11A] == 1);
  CHECK(out[0x418] == 0x40);
  CHECK(out[0x427] == 0x4F);
  CHECK(d.defaulted_groups == (og_ns::kDropSession | (kAllSettings & ~(og_ns::kDropDelay | og_ns::kDropReverb))));
  // the inspector agrees
  std::uint16_t groups = 0;
  CHECK(og_ns::inspect_settings(two.data(), static_cast<std::uint32_t>(two.size()), groups));
  CHECK(groups == 0x0A);
  CHECK(!og_ns::inspect_settings(longer.data(), static_cast<std::uint32_t>(longer.size()), groups));
  CHECK(groups == 0);
  CHECK(!og_ns::inspect_settings(nullptr, 8, groups));
}

TEST_CASE("og settings: defaulted groups are always reported, and add up across files") {
  model::Project& proj = arena(0).project;
  proj.reset();
  og_tracks(proj);
  export_project(proj);
  const og_ns::ExportDrops fresh = writer().export_report().drops;
  CHECK(fresh.defaulted_groups == (og_ns::kDropSession | kAllSettings));  // nothing is carried, so everything is
  CHECK(!fresh.any_lost());                                               // and none of it is a loss
  og_ns::ExportDrops a;
  a.defaulted_groups = og_ns::kDropDelay;
  og_ns::ExportDrops b;
  b.defaulted_groups = og_ns::kDropLimiter;
  b.pattern_names = 2;
  b.settings_ignored = 1;
  a.add(b);
  CHECK(a.defaulted_groups == (og_ns::kDropDelay | og_ns::kDropLimiter));
  CHECK(a.pattern_names == 2);
  CHECK(a.settings_ignored == 1);
  CHECK(a.any_lost());
}

TEST_CASE("og settings: a project read again does not keep the chunk of the last one") {
  ProjectSpec g;
  g.fill = ProjectSpec::Fill::Groups;
  NEED(import_project(project_bytes(g), 0) == Progress::Done);
  CHECK(arena(0).project.unknown_count() == 1);
  NEED(import_project(project_bytes({}), 0) == Progress::Done);
  CHECK(arena(0).project.unknown_count() == 0);
  // a failed read leaves none either
  NEED(import_project(project_bytes(g), 0) == Progress::Done);
  Bytes bad = project_bytes(g);
  bad.resize(500);
  CHECK(import_project(bad, 0) == Progress::Failed);
  CHECK(arena(0).project.unknown_count() == 0);
}
