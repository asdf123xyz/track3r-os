// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/og_fixture.h"

#include <o3os/formats/og.h>
#include <o3os/model/project.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace o3os;
using namespace o3os::test::og;
using formats::FormatError;
using formats::Progress;
using o3os::test::MemorySource;
using o3os::test::VectorSink;
namespace og_ns = o3os::formats::og;

namespace {

model::Project& fresh(int i = 0) {
  model::Project& p = arena(i).project;
  p.reset();
  return p;
}

og_ns::ProjectReader& reader() {
  static og_ns::ProjectReader r;
  return r;
}

Progress import_project(const Bytes& file, og_ns::ImportReport* rep = nullptr, std::size_t budget = 4096,
                        int arena_index = 0) {
  const Progress p = run_reader(reader(), file, arena(arena_index), {}, budget);
  if (rep != nullptr) *rep = reader().import_report();
  return p;
}

constexpr std::uint32_t kBase = og_ns::kDropSession | og_ns::kDropDelay | og_ns::kDropLimiter | og_ns::kDropInputs |
                                og_ns::kDropPerformance | og_ns::kDropPatternBitmask;

// The groups a version's layout carries, and the ones of them that are kept in the OGPM chunk.
std::uint32_t layout_groups(unsigned v) {
  if (v == 2) return kBase | og_ns::kDropMidiVelocity;
  if (v <= 7) return kBase | og_ns::kDropReverb | og_ns::kDropMidiVelocity;
  if (v <= 14) return kBase | og_ns::kDropReverb | og_ns::kDropExt13;
  return kBase | og_ns::kDropReverb | og_ns::kDropExt13 | og_ns::kDropExt19;
}
constexpr std::uint32_t kNotKept = og_ns::kDropSession | og_ns::kDropPatternBitmask;
std::uint32_t kept_groups(unsigned v) { return layout_groups(v) & ~kNotKept; }

// What the model should hold after a read of a project of version v built from `s` (the default spec).
void check_model(const model::Project& p, const ProjectSpec& s, unsigned v) {
  CHECK(p.name() == s.name);
  CHECK(p.tempo_x100() == 13350);
  CHECK(p.default_pattern_length() == s.pattern_length);
  CHECK(p.swing() == model::kDefaultSwing);
  CHECK(p.active_tracks() == (v >= 16 ? 12u : 8u));
  NEED(p.song_length() == s.playlist.size());
  for (unsigned i = 0; i < s.playlist.size(); ++i) CHECK(p.order(i) == s.playlist[i]);
  for (unsigned slot = 0; slot < 16; ++slot) {
    const model::TrackSlot* t = p.track(slot);
    NEED(t != nullptr);
    // OS-TRK912: tracks 9-12 are ordinary tracks here (new Community OS behaviour), 13-16 stay MIDI
    CHECK_MESSAGE(t->type == (slot < 12 ? model::TrackType::Audio : model::TrackType::Midi), "slot " << slot);
    CHECK(t->pan == 50);
    CHECK(t->bus == 0);
    if (slot < 8) {
      const std::string name = v >= 4 ? s.names[slot] : "Track " + std::to_string(slot + 1);
      CHECK_MESSAGE(std::string(t->name) == name, "slot " << slot);
      CHECK(t->volume == (v >= 5 ? s.volume[slot] : 100));
      CHECK(t->flags == (s.mute[slot] != 0 ? model::kTrackMute : 0));
    } else {
      // "Midi 9" and "Midi 10" are the default names an OG project file holds for slots 9 and 10, read as "Track 9", "Track 10"; "M11" is the user's, "" stays empty
      std::string name = v >= 16 ? s.names[slot] : "";
      if (slot < 12 && name == "Midi " + std::to_string(slot + 1)) name = "Track " + std::to_string(slot + 1);
      CHECK_MESSAGE(std::string(t->name) == name, "slot " << slot);
      CHECK(t->volume == model::kDefaultVolume);
      CHECK(t->flags == 0);
    }
  }
  const model::MasterSettings& m = p.master();
  CHECK(m.volume == model::kDefaultVolume);
  CHECK(m.reverb_volume == (v >= 5 ? s.reverb_volume : 100));
  CHECK(m.delay_volume == (v >= 5 ? s.delay_volume : 100));
  CHECK(m.dry_volume == (v >= 6 ? s.dry_volume : 100));
  const std::uint8_t flags = static_cast<std::uint8_t>(model::kMasterLimiter |
                                                       (v >= 6 && s.reverb_mute ? model::kMasterReverbMute : 0) |
                                                       (v >= 6 && s.delay_mute ? model::kMasterDelayMute : 0) |
                                                       (v >= 6 && s.dry_mute ? model::kMasterDryMute : 0));
  CHECK(m.flags == flags);
  // the 16 MIDI instruments, which have no file
  CHECK(p.instrument_count() == 16);
  for (unsigned ch = 0; ch < 16; ++ch) {
    const model::Instrument* in = p.instrument(48 + ch);
    NEED(in != nullptr);
    CHECK(in->kind == model::InstrumentKind::Midi);
    CHECK(in->midi_channel == ch + 1);
    CHECK(std::string(in->name) == "MIDI " + std::to_string(ch + 1));
  }
  CHECK(p.pattern_count() == 0);
  CHECK(p.check_invariants());
}

}  // namespace

TEST_CASE("og project: v16 maps every field the model has") {
  const ProjectSpec spec;
  og_ns::ImportReport rep;
  model::Project& proj = fresh();
  NEED(import_project(project_bytes(spec), &rep) == Progress::Done);
  CHECK(rep.error == FormatError::None);
  CHECK(rep.version == 16);
  CHECK(!rep.unverified_version);
  CHECK(!rep.crc_mismatch);
  CHECK(rep.sanitised == 0);
  CHECK(rep.dropped_groups == kNotKept);  // the session state and the bitmask; the rest is at the defaults
  CHECK(rep.kept_groups == 0);            // so nothing needs keeping
  CHECK(proj.unknown_count() == 0);
  check_model(proj, spec, 16);
  CHECK(std::string(og_ns::drop_group_name(0)) == "session");
  CHECK(std::string(og_ns::drop_group_name(og_ns::kDropGroupCount)) == "");
}

TEST_CASE("og project: every version it reads, from 2 to 17") {
  for (unsigned v : {2u, 3u, 4u, 5u, 6u, 7u, 11u, 12u, 13u, 14u, 16u, 17u}) {
    ProjectSpec spec;
    spec.version = v;
    spec.fill = ProjectSpec::Fill::Groups;  // junk in the session state, a non-default value in every setting
    og_ns::ImportReport rep;
    model::Project& proj = fresh();
    const Bytes f = project_bytes(spec);
    CHECK(f.size() == project_length(v));
    NEED(import_project(f, &rep) == Progress::Done);
    CHECK_MESSAGE(rep.version == v, "version " << v);
    CHECK_MESSAGE(!rep.unverified_version, "version " << v);
    CHECK_MESSAGE(rep.observed_only == (v == 17), "version " << v);  // v17: no file of it (OS-004c)
    CHECK(rep.sanitised == 0);
    CHECK_MESSAGE(rep.dropped_groups == kNotKept, "version " << v);
    CHECK_MESSAGE(rep.kept_groups == kept_groups(v), "version " << v);
    CHECK(proj.unknown_count() == 1);
    INFO("version " << v);
    check_model(proj, spec, v);
  }
}

TEST_CASE("og project: version 0 and 1 are refused") {
  for (unsigned v : {0u, 1u}) {
    Bytes f = project_bytes({});
    for (unsigned i = 8; i < 12; ++i) f[i] = static_cast<std::uint8_t>(v);
    seal(f);
    og_ns::ImportReport rep;
    model::Project& proj = fresh();
    CHECK(import_project(f, &rep) == Progress::Failed);
    CHECK(rep.error == FormatError::UnsupportedVersion);
    CHECK(proj.instrument_count() == 0);  // reset, valid and empty
    CHECK(proj.check_invariants());
  }
}

TEST_CASE("og project: versions with no evidence read the part every version shares, as unverified") {
  const ProjectSpec spec;
  // the gaps (8-10, 15) on the layouts either side of them, and 18+ on the 1.9.0 layout and on the 1.9.2 one's 2,324 bytes
  // (17 is no longer here: it is known, see og_v17_test.cpp)
  struct Case { unsigned version; unsigned like; unsigned extra; };
  for (const Case c : {Case{8, 7, 0}, Case{9, 7, 0}, Case{10, 7, 0}, Case{15, 14, 0}, Case{18, 16, 0}, Case{18, 16, 4},
                       Case{19, 16, 0}, Case{255, 16, 0}}) {
    ProjectSpec s = spec;
    s.version = c.like;
    Bytes f = project_bytes(s);
    f.resize(f.size() + c.extra, 0xAB);
    if (c.extra != 0) {  // 4 bytes inside, before the name, as Tracker+/Mini files have: the CRC moves to the end
      for (std::size_t i = f.size() - 1; i >= 0x810 + 4; --i) f[i] = f[i - 4];
    }
    for (unsigned i = 8; i < 12; ++i) f[i] = static_cast<std::uint8_t>(c.version);
    seal(f);
    og_ns::ImportReport rep;
    model::Project& proj = fresh();
    NEED(import_project(f, &rep) == Progress::Done);
    CHECK_MESSAGE(rep.unverified_version, "version " << c.version);
    CHECK(!rep.observed_only);
    CHECK(rep.version == c.version);
    CHECK(std::string(proj.name()).empty());          // where the name sits is not known: left to the folder
    CHECK(proj.tempo_x100() == 13350);
    CHECK(proj.song_length() == spec.playlist.size());
    CHECK(std::string(proj.track(1)->name) == "Snare");
    CHECK(proj.track(1)->flags == model::kTrackMute);
    CHECK(proj.track(2)->volume == 70);
    CHECK(std::string(proj.track(9)->name).empty());  // nor tracks 9-16
    CHECK(proj.master().reverb_volume == 60);
    CHECK(proj.active_tracks() == (c.version >= 16 ? 12u : 8u));
    CHECK(proj.check_invariants());
    // nothing vouches for the layout but the CRC
    Bytes bad = f;
    bad[0x300] ^= 1;
    CHECK(import_project(bad, &rep) == Progress::Failed);
    CHECK(rep.error == FormatError::BadCrc);
    CHECK(proj.instrument_count() == 0);
  }
  // the settings of the common part are kept, the blocks whose layout is not known are dropped
  {
    ProjectSpec g;
    g.fill = ProjectSpec::Fill::Groups;
    Bytes f = project_bytes(g);
    for (unsigned i = 8; i < 12; ++i) f[i] = 18;
    seal(f);
    og_ns::ImportReport rep;
    model::Project& proj = fresh();
    NEED(import_project(f, &rep) == Progress::Done);
    CHECK(rep.kept_groups == (og_ns::kDropDelay | og_ns::kDropLimiter | og_ns::kDropReverb | og_ns::kDropInputs |
                              og_ns::kDropPerformance));
    CHECK(rep.dropped_groups == (kNotKept | og_ns::kDropExt13 | og_ns::kDropExt19));
    CHECK(proj.unknown_count() == 1);
  }
  // too short for any layout
  Bytes tiny(0x4E0 + 3, 0);
  tiny[0] = 'M'; tiny[1] = 'T'; tiny[8] = 20;
  og_ns::ImportReport rep;
  CHECK(import_project(tiny, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::Truncated);
}

TEST_CASE("og project: a length that does not fit the version, a bad magic, a bad CRC") {
  og_ns::ImportReport rep;
  model::Project& proj = fresh();
  for (unsigned v : {2u, 5u, 6u, 11u, 16u, 17u}) {
    ProjectSpec s;
    s.version = v;
    Bytes f = project_bytes(s);
    f.pop_back();
    CHECK(import_project(f, &rep) == Progress::Failed);
    CHECK_MESSAGE(rep.error == FormatError::Truncated, "version " << v);
    f.resize(f.size() + 2, 0);
    CHECK(import_project(f, &rep) == Progress::Failed);
    CHECK_MESSAGE(rep.error == FormatError::Malformed, "version " << v);
  }
  Bytes f = project_bytes({});
  f[1] = 'X';
  CHECK(import_project(f, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::BadMagic);
  f = project_bytes({});
  f[2] = 1;  // type 1 is an instrument
  CHECK(import_project(f, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::BadMagic);
  // a project's CRC is only warned about (our rule, docs/formats/og.md): the data is used
  f = project_bytes({});
  f[0x1C0] ^= 0;  // unchanged
  f[0x12C + 2] = 1;
  CHECK(import_project(f, &rep) == Progress::Done);
  CHECK(rep.crc_mismatch);
  CHECK(proj.track(2)->flags == model::kTrackMute);
}

TEST_CASE("og project: values the model cannot hold") {
  og_ns::ImportReport rep;
  model::Project& proj = fresh();
  auto with = [](auto edit) {
    Bytes f = project_bytes({});
    edit(f);
    seal(f);
    return f;
  };
  // tempo
  for (float t : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 0.0f, -5.0f, 9.0f,
                  1001.0f}) {
    CHECK(import_project(with([t](Bytes& f) { pf(f, 0x1C0, t); }), &rep) == Progress::Done);
    CHECK(proj.tempo_x100() == model::kDefaultTempoX100);
    CHECK(rep.sanitised == 1);
  }
  CHECK(import_project(with([](Bytes& f) { pf(f, 0x1C0, 25.0f); }), &rep) == Progress::Done);  // allowed
  CHECK(proj.tempo_x100() == model::kMinTempoX100);
  CHECK(rep.sanitised == 1);
  CHECK(import_project(with([](Bytes& f) { pf(f, 0x1C0, 900.0f); }), &rep) == Progress::Done);
  CHECK(proj.tempo_x100() == model::kMaxTempoX100);
  CHECK(import_project(with([](Bytes& f) { pf(f, 0x1C0, 120.25f); }), &rep) == Progress::Done);
  CHECK(proj.tempo_x100() == 12025);
  CHECK(rep.sanitised == 0);
  // pattern length
  for (unsigned n : {0u, 129u, 65535u}) {
    CHECK(import_project(with([n](Bytes& f) { p16(f, 0x1C4, n); }), &rep) == Progress::Done);
    CHECK(proj.default_pattern_length() == model::kDefaultPatternLength);
    CHECK(rep.sanitised == 1);
  }
  CHECK(import_project(with([](Bytes& f) { p16(f, 0x1C4, 128); }), &rep) == Progress::Done);
  CHECK(proj.default_pattern_length() == 128);
  // levels above 100
  CHECK(import_project(with([](Bytes& f) { f[0x4D0 + 3] = 101; f[0x4D8] = 255; f[0x4D9] = 200; f[0x4DD] = 150; }),
                       &rep) == Progress::Done);
  CHECK(proj.track(3)->volume == 100);
  CHECK(proj.master().reverb_volume == 100);
  CHECK(proj.master().delay_volume == 100);
  CHECK(proj.master().dry_volume == 100);
  CHECK(rep.sanitised == 4);
  // a playlist with entries behind a zero: only the head is the song
  CHECK(import_project(with([](Bytes& f) { f[0x10 + 2] = 0; f[0x10 + 9] = 4; }), &rep) == Progress::Done);
  CHECK(proj.song_length() == 2);
  CHECK(rep.sanitised == 1);
  // a full playlist, names without a terminator, any bytes in names
  CHECK(import_project(with([](Bytes& f) {
                         for (unsigned i = 0; i < 255; ++i) f[0x10 + i] = static_cast<std::uint8_t>(1 + i % 255);
                         for (unsigned i = 0; i < 21; ++i) f[0x428 + i] = 'a';          // no NUL in the field
                         for (unsigned i = 0; i < 32; ++i) f[0x80C + i] = 'p';
                         for (unsigned i = 0; i < 8; ++i) f[0x603 + i] = 'q';
                       }),
                       &rep) == Progress::Done);
  CHECK(proj.song_length() == 255);
  CHECK(std::string(proj.track(0)->name) == std::string(20, 'a'));
  CHECK(std::string(proj.name()) == std::string(31, 'p'));
  CHECK(std::string(proj.track(8)->name) == std::string(7, 'q'));
  CHECK(proj.check_invariants());
}

TEST_CASE("og project: truncated at every offset, never a half-loaded project") {
  for (unsigned v : {6u, 16u, 17u}) {
    ProjectSpec s;
    s.version = v;
    const Bytes good = project_bytes(s);
    model::Project& proj = fresh();
    const std::size_t stride =
#if defined(__arm__)
        29;
#else
        1;
#endif
    for (std::size_t n = 0; n < good.size(); n += (n < 40 || n + 40 > good.size()) ? 1 : stride) {
      const Bytes cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
      og_ns::ImportReport rep;
      CHECK_MESSAGE(import_project(cut, &rep) == Progress::Failed, "version " << v << " cut at " << n);
      CHECK(rep.error == FormatError::Truncated);
      CHECK(proj.instrument_count() == 0);
      CHECK(proj.song_length() == 0);
      CHECK(proj.check_invariants());
    }
  }
}

#if !defined(__arm__)
TEST_CASE("og project: a source that never ends is refused after the longest layout") {
  class Zeros final : public core::ByteSource {
   public:
    core::IoResult read(void* dst, std::size_t max) noexcept override {
      std::memset(dst, 'M', max);
      return {max, core::IoStatus::Ok};
    }
  } src;
  model::Project& proj = fresh();
  og_ns::ProjectReader& r = reader();
  r.begin(src, arena(0), {});
  std::size_t steps = 0;
  while (r.step(4096) == Progress::Running) ++steps;
  CHECK(r.report().error == FormatError::LimitExceeded);
  CHECK(steps < 10);
  CHECK(proj.check_invariants());
  CHECK(proj.instrument_count() == 0);
}
#endif

TEST_CASE("og project: any budget and any source behaviour give the same project") {
  const ProjectSpec spec;
  const Bytes f = project_bytes(spec);
  for (std::size_t budget : {std::size_t{1}, std::size_t{13}, std::size_t{4096}}) {
    model::Project& proj = fresh();
    CHECK(run_reader(reader(), f, arena(0), {}, budget, 5, 3) == Progress::Done);
    check_model(proj, spec, 16);
  }
  model::Project& proj = fresh();
  {
    MemorySource src(f);
    src.fail_at = 1000;
    reader().begin(src, arena(0), {});
    while (reader().step(4096) == Progress::Running) {}
    CHECK(reader().report().error == FormatError::SourceFailed);
    CHECK(proj.instrument_count() == 0);
  }
  for (MemorySource::Lie lie : {MemorySource::Lie::TooMany, MemorySource::Lie::OkZero}) {
    MemorySource src(f);
    src.lie = lie;
    reader().begin(src, arena(0), {});
    while (reader().step(4096) == Progress::Running) {}
    CHECK(reader().report().error == FormatError::SourceFailed);
  }
}

TEST_CASE("og project: reading resets the project it reads into") {
  model::Project& proj = fresh();
  NEED(proj.create_pattern(5, 16) == model::Status::Ok);
  NEED(proj.append_order(5) == model::Status::Ok);
  NEED(import_project(project_bytes({})) == Progress::Done);
  CHECK(proj.pattern_count() == 0);
  CHECK(proj.song_length() == 5);
}

// ---- export -------------------------------------------------------------------------------------------------

namespace {
og_ns::ProjectWriter& writer() {
  static og_ns::ProjectWriter w;
  return w;
}
Progress export_project(const model::Project& p, Bytes& out, std::size_t budget = 4096) {
  return run_writer(writer(), p, out, {}, budget);
}

// Tracks 9-16 as MIDI tracks, the form Export writes into an OG project (a fresh OS project has twelve audio ones, which
// Export counts as a type change on tracks 9-12).
void og_tracks(model::Project& proj) {
  for (std::uint32_t slot = 8; slot < model::kMaxTracks; ++slot) {
    model::TrackSlot t = *proj.track(slot);
    t.type = model::TrackType::Midi;
    CHECK(proj.set_track(slot, t) == model::Status::Ok);
  }
}
}  // namespace

TEST_CASE("og project: the 2,320 bytes of an empty project, field by field") {
  model::Project& proj = fresh();
  og_tracks(proj);
  NEED(proj.set_name("Empty", 5) == model::Status::Ok);
  Bytes out;
  NEED(export_project(proj, out) == Progress::Done);
  NEED(out.size() == 2320);
  const std::uint8_t head[16] = {'M', 'T', 0, 0, 1, 9, 0, 255, 16, 16, 16, 16, 0x10, 0x09, 0, 0};
  CHECK(std::memcmp(out.data(), head, 16) == 0);
  CHECK(out[0x10] == 1);                      // the default song of an empty project: pattern 1 (this OS's choice)
  CHECK(out[0x11] == 0);
  CHECK(out[0x114] == 1);
  CHECK(out[0x116] == 11);
  CHECK(out[0x117] == 24);
  CHECK(out[0x118] == 8);
  CHECK(out[0x11A] == 50);
  CHECK(r16(out, 0x11C) == 500);
  CHECK(out[0x11E] == 0xC3);
  CHECK(r16(out, 0x120) == 100);
  CHECK(r32(out, 0x124) == fbits(0.512f));
  CHECK(r16(out, 0x128) == 30200);
  CHECK(out[0x12A] == 16);
  CHECK(out[0x12B] == 1);
  CHECK(r32(out, 0x13C) == fbits(87.5f));
  CHECK(r16(out, 0x142) == 1);
  CHECK(out[0x152] == 1);
  CHECK(out[0x15D] == 12);
  CHECK(r32(out, 0x1C0) == fbits(130.0f));
  CHECK(r16(out, 0x1C4) == 32);
  CHECK(out[0x309] == 86);
  CHECK(out[0x30A] == 0);
  CHECK(r32(out, 0x418) == fbits(0.5f));
  CHECK(r32(out, 0x424) == fbits(0.68f));
  CHECK(out[0x4D0] == 86);                    // the default track volume
  CHECK(out[0x4D8] == 86);
  CHECK(out[0x4DD] == 86);
  CHECK(out[0x4F8] == 3);
  CHECK(out[0x4F9] == 1);
  CHECK(out[0x4FC] == 1);
  CHECK(out[0x4FD] == 0);
  CHECK(out[0x4FD + 16 * 15] == 15);
  CHECK(out[0x4FE + 16 * 15] == 0x7F);
  CHECK(out[0x504 + 16 * 15] == 0x80);
  CHECK(r16(out, 0x5FE) == 20000);
  CHECK(std::string(reinterpret_cast<const char*>(&out[0x80C])) == "Empty");
  CHECK(r32(out, 0x90C) == formats::crc32(out.data(), 0x90C));
  CHECK(!writer().export_report().drops.any_lost());
  // every group is at its default, and says so: the session state and all the settings
  const formats::og::ExportDrops& d = writer().export_report().drops;
  CHECK(d.defaulted_groups == (og_ns::kDropSession | og_ns::kDropDelay | og_ns::kDropLimiter | og_ns::kDropReverb |
                               og_ns::kDropInputs | og_ns::kDropPerformance | og_ns::kDropMidiVelocity |
                               og_ns::kDropExt13 | og_ns::kDropExt19));
  // the same 2,320 bytes with a one-byte budget
  Bytes slow;
  NEED(export_project(proj, slow, 1) == Progress::Done);
  CHECK(slow == out);
}

TEST_CASE("og project: the pattern-usage bitmask is rebuilt from the patterns by a rule that agrees with the fixture files") {
  model::Project& proj = fresh();
  NEED(proj.create_pattern(1, 16) == model::Status::Ok);
  NEED(proj.create_pattern(7, 4) == model::Status::Ok);
  NEED(proj.create_pattern(255, 128) == model::Status::Ok);
  model::Step note{};
  note.note = 40;
  note.vol = model::kVolumeUnset;
  model::Step off = note;
  off.note = model::kNoteOff;
  NEED(proj.set_step(1, 0, 0, note) == model::Status::Ok);    // track 1 -> bit 0
  NEED(proj.set_step(1, 2, 14, note) == model::Status::Ok);   // track 3, the last step that counts (length 16: steps 0..14)
  NEED(proj.set_step(1, 3, 15, note) == model::Status::Ok);   // the last step of the track is not looked at
  NEED(proj.set_step(1, 4, 3, off) == model::Status::Ok);     // off is not a note
  NEED(proj.set_step(1, 8, 3, note) == model::Status::Ok);    // track 9: only eight tracks have a bit
  NEED(proj.set_step(7, 7, 2, note) == model::Status::Ok);    // length 4: steps 0..2 count; track 8 -> bit 7
  NEED(proj.set_step(255, 5, 126, note) == model::Status::Ok);
  NEED(proj.set_step(255, 6, 127, note) == model::Status::Ok);
  Bytes out;
  NEED(export_project(proj, out) == Progress::Done);
  CHECK(out[0x319 + 0] == 0x05);
  CHECK(out[0x319 + 6] == 0x80);
  CHECK(out[0x319 + 254] == 0x20);
  CHECK(out[0x319 + 1] == 0);
  CHECK(out[0x319 + 2] == 0);
  // any budget finds the same bits
  Bytes slow;
  NEED(export_project(proj, slow, 100) == Progress::Done);
  CHECK(slow == out);
}

TEST_CASE("og project: the song, names, volumes, mutes and master section go where the OG keeps them") {
  model::Project& proj = fresh();
  NEED(proj.set_name("ABCDEFGHIJKLMNOPQRSTUVWXYZ012345", 32) == model::Status::Ok);
  NEED(proj.set_tempo_x100(12025) == model::Status::Ok);
  NEED(proj.set_default_pattern_length(48) == model::Status::Ok);
  for (std::uint32_t id : {3u, 1u, 255u}) NEED(proj.append_order(id) == model::Status::Ok);
  model::TrackSlot t{};
  t.type = model::TrackType::Audio;
  t.volume = 33;
  t.pan = 50;
  t.flags = model::kTrackMute;
  std::memcpy(t.name, "Twenty chars exactly", 20);
  NEED(proj.set_track(2, t) == model::Status::Ok);
  t.type = model::TrackType::Midi;
  t.flags = 0;
  std::memset(t.name, 0, sizeof t.name);
  std::memcpy(t.name, "Longer than seven", 17);
  NEED(proj.set_track(9, t) == model::Status::Ok);
  model::MasterSettings m{70, 61, 62, 63, model::kMasterReverbMute | model::kMasterDelayMute};
  NEED(proj.set_master(m) == model::Status::Ok);
  Bytes out;
  NEED(export_project(proj, out) == Progress::Done);
  CHECK(out[0x10] == 3);
  CHECK(out[0x11] == 1);
  CHECK(out[0x12] == 255);
  CHECK(out[0x13] == 0);
  CHECK(out[0x114] == 3);  // the current pattern: the first of the song
  CHECK(r32(out, 0x1C0) == fbits(120.25f));
  CHECK(r16(out, 0x1C4) == 48);
  CHECK(out[0x12C + 2] == 1);
  CHECK(out[0x12C + 1] == 0);
  CHECK(out[0x4D0 + 2] == 33);
  CHECK(std::string(reinterpret_cast<const char*>(&out[0x428 + 2 * 21])) == "Twenty chars exactly");
  CHECK(std::string(reinterpret_cast<const char*>(&out[0x603 + 8])) == "Longer ");
  CHECK(out[0x4D8] == 62);
  CHECK(out[0x4D9] == 63);
  CHECK(out[0x4DA] == 1);
  CHECK(out[0x4DB] == 1);
  CHECK(out[0x4DC] == 0);
  CHECK(out[0x4DD] == 61);
  CHECK(std::string(reinterpret_cast<const char*>(&out[0x80C])) == "ABCDEFGHIJKLMNOPQRSTUVWXYZ01234");
  const formats::og::ExportDrops& d = writer().export_report().drops;
  CHECK(d.track_names_cut == 1);
  CHECK(d.project_settings == 1);  // the name was cut
}

TEST_CASE("og project: what Export counts as dropped") {
  model::Project& proj = fresh();
  og_tracks(proj);
  NEED(proj.set_swing(60) == model::Status::Ok);
  model::TrackSlot t{};
  t.volume = 86;
  t.pan = 20;
  NEED(proj.set_track(0, t) == model::Status::Ok);
  t.pan = 50;
  t.flags = model::kTrackSolo;
  NEED(proj.set_track(1, t) == model::Status::Ok);
  t.flags = 0;
  t.bus = 3;
  NEED(proj.set_track(2, t) == model::Status::Ok);
  t.bus = 0;
  t.type = model::TrackType::ExternalAudio;
  NEED(proj.set_track(3, t) == model::Status::Ok);
  NEED(proj.set_track(14, [] { model::TrackSlot x{}; x.pan = 0; x.volume = 1; return x; }()) == model::Status::Ok);  // track 15: nowhere to go
  NEED(proj.unknown_begin(0x5A5A5A5Au, 4) == model::Status::Ok);
  NEED(proj.unknown_append(reinterpret_cast<const std::uint8_t*>("abcd"), 4) == model::Status::Ok);
  model::Instrument s;
  model::init_instrument(s, model::InstrumentKind::Sample);
  NEED(proj.set_instrument(60, s) == model::Status::Ok);
  Bytes out;
  NEED(export_project(proj, out) == Progress::Done);
  const formats::og::ExportDrops& d = writer().export_report().drops;
  CHECK(d.track_settings == 4);
  CHECK(d.project_settings == 1);
  CHECK(d.unknown_chunks == 1);
  CHECK(d.sample_slots_beyond_48 == 1);
  CHECK(d.any_lost());
  formats::og::ExportDrops total;
  total.add(d);
  total.add(d);
  CHECK(total.track_settings == 8);
}

TEST_CASE("og project: track settings that an OG track cannot hold are counted") {
  model::Project& proj = fresh();
  og_tracks(proj);
  Bytes out;
  NEED(export_project(proj, out) == Progress::Done);
  CHECK(writer().export_report().drops.track_settings == 0);   // tracks 1-8 audio, 9-16 MIDI, all at the defaults
  auto edit = [&proj](std::uint32_t slot, auto change) {
    model::TrackSlot t = *proj.track(slot);
    change(t);
    CHECK(proj.set_track(slot, t) == model::Status::Ok);
  };
  auto count = [&]() {
    Bytes o;
    CHECK(export_project(proj, o) == Progress::Done);
    return writer().export_report().drops.track_settings;
  };
  // tracks 1-8 have a mute and a volume in the file: not counted. Pan, solo, bus and a MIDI type are.
  edit(2, [](model::TrackSlot& t) { t.flags = model::kTrackMute; t.volume = 40; });
  CHECK(count() == 0);
  edit(0, [](model::TrackSlot& t) { t.type = model::TrackType::Midi; });
  CHECK(count() == 1);
  // tracks 9-12 have neither: a mute, a volume that is not the default, and an audio type are counted
  edit(8, [](model::TrackSlot& t) { t.flags = model::kTrackMute; });
  CHECK(count() == 2);
  edit(9, [](model::TrackSlot& t) { t.volume = 60; });
  CHECK(count() == 3);
  edit(10, [](model::TrackSlot& t) { t.type = model::TrackType::ExternalAudio; });
  CHECK(count() == 4);
  edit(11, [](model::TrackSlot& t) { t.pan = 10; });
  CHECK(count() == 5);
  // tracks 13-16 are not written at all: their settings are not this counter's business
  edit(13, [](model::TrackSlot& t) { t.flags = model::kTrackMute; t.volume = 3; t.type = model::TrackType::Audio; });
  CHECK(count() == 5);
  // and the way back: restoring a track restores the count
  edit(8, [](model::TrackSlot& t) { t.flags = 0; });
  CHECK(count() == 4);
}

// OS-TRK912: tracks 9-12 of an OG project are ordinary (audio) tracks here; the default names the file holds go with them, a name the
// user typed stays, and Export writes them back into slots 9-12 of the file under "Midi N".
TEST_CASE("og project: tracks 9-12 import as audio tracks named Track N, custom names kept") {
  ProjectSpec spec;
  spec.names[8] = "Midi 9";    // the default name in an OG project file
  spec.names[9] = "Midi 10";   // two digits: still 7 characters in the file
  spec.names[10] = "Drums";    // typed by the user
  spec.names[11] = "Midi 11";  // the default of ANOTHER slot is a custom name
  spec.names[12] = "Midi 13";  // beyond slot 12: untouched, and still MIDI
  model::Project& proj = fresh();
  NEED(import_project(project_bytes(spec)) == Progress::Done);
  for (std::uint32_t slot = 8; slot < 12; ++slot) CHECK_MESSAGE(proj.track(slot)->type == model::TrackType::Audio, "slot " << slot);
  for (std::uint32_t slot = 12; slot < 16; ++slot) CHECK_MESSAGE(proj.track(slot)->type == model::TrackType::Midi, "slot " << slot);
  CHECK(std::string(proj.track(8)->name) == "Track 9");
  CHECK(std::string(proj.track(9)->name) == "Track 10");
  CHECK(std::string(proj.track(10)->name) == "Drums");
  CHECK(std::string(proj.track(11)->name) == "Midi 11");
  CHECK(std::string(proj.track(12)->name) == "Midi 13");
  CHECK(proj.track(8)->volume == model::kDefaultVolume);  // nothing else of the slot changes
  CHECK(proj.track(8)->flags == 0);
  CHECK(proj.track(8)->pan == model::kDefaultPan);

  // Export: slots 9-12 of the file again, "Track 10" as "Midi 10" (8 characters would not fit the file's 7) and not cut
  Bytes out;
  NEED(export_project(proj, out) == Progress::Done);
  NEED(out.size() == 2320);
  auto name_at = [&out](std::uint32_t slot) { return std::string(reinterpret_cast<const char*>(&out[0x603 + (slot - 8) * 8])); };
  CHECK(name_at(8) == "Midi 9");
  CHECK(name_at(9) == "Midi 10");
  CHECK(name_at(10) == "Drums");
  CHECK(name_at(11) == "Midi 11");
  CHECK(name_at(12) == "Midi 13");
  const og_ns::ExportDrops& d = writer().export_report().drops;
  CHECK(d.audio_on_midi_tracks == 4);   // reported as that ...
  CHECK(d.track_settings == 0);         // ... and not as a lost setting
  CHECK(d.track_names_cut == 0);
  CHECK(!d.any_lost());                 // a note is not a loss
  og_ns::ExportDrops total;
  total.add(d);
  total.add(d);
  CHECK(total.audio_on_midi_tracks == 8);
}

TEST_CASE("og project: the way back of Track 9-12 only touches the slot's own number, and a real lost setting still counts") {
  model::Project& proj = fresh();
  og_tracks(proj);  // MIDI on 9-16, as Export writes them
  Bytes out;
  auto rename = [&proj](std::uint32_t slot, const char* name, model::TrackType type) {
    model::TrackSlot t = *proj.track(slot);
    std::memset(t.name, 0, sizeof t.name);
    std::memcpy(t.name, name, std::strlen(name));
    t.type = type;
    CHECK(proj.set_track(slot, t) == model::Status::Ok);
  };
  rename(8, "Track 10", model::TrackType::Audio);  // another slot's number: a name of the user's, cut to 7 and counted
  rename(9, "Track 10", model::TrackType::Audio);  // its own: the default name
  rename(12, "Track 13", model::TrackType::Midi);  // beyond slot 12: not mapped, cut like any long name
  rename(10, "Track 11", model::TrackType::Audio);
  NEED(export_project(proj, out) == Progress::Done);
  auto name_at = [&out](std::uint32_t slot) { return std::string(reinterpret_cast<const char*>(&out[0x603 + (slot - 8) * 8])); };
  CHECK(name_at(8) == "Track 1");
  CHECK(name_at(9) == "Midi 10");
  CHECK(name_at(10) == "Midi 11");
  CHECK(name_at(12) == "Track 1");
  CHECK(writer().export_report().drops.track_names_cut == 2);
  CHECK(writer().export_report().drops.audio_on_midi_tracks == 3);
  // a pan on an audio track 9-12 is lost whatever its type
  model::TrackSlot t = *proj.track(9);
  t.pan = 10;
  NEED(proj.set_track(9, t) == model::Status::Ok);
  NEED(export_project(proj, out) == Progress::Done);
  CHECK(writer().export_report().drops.track_settings == 1);
  CHECK(writer().export_report().drops.any_lost());
}

TEST_CASE("og project: the chunks of the OG formats are not dropped chunks, other unknown ones are") {
  model::Project& proj = fresh();
  og_tracks(proj);
  const std::uint8_t chunk[4] = {0x01, 0x00, 0x00, 0x00};  // a valid OGPM with no groups
  NEED(proj.unknown_begin(og_ns::kChunkSettings, 4) == model::Status::Ok);
  NEED(proj.unknown_append(chunk, 4) == model::Status::Ok);
  NEED(proj.unknown_begin(og_ns::kChunkNames, 3) == model::Status::Ok);  // (an OGPN too: counted by its own writer)
  NEED(proj.unknown_append(chunk, 3) == model::Status::Ok);
  Bytes out;
  NEED(export_project(proj, out) == Progress::Done);
  CHECK(writer().export_report().drops.unknown_chunks == 0);
  CHECK(writer().export_report().drops.settings_ignored == 0);
  NEED(proj.unknown_begin(0x58585858u, 1) == model::Status::Ok);  // "XXXX"
  NEED(proj.unknown_append(chunk, 1) == model::Status::Ok);
  NEED(export_project(proj, out) == Progress::Done);
  CHECK(writer().export_report().drops.unknown_chunks == 1);
}

TEST_CASE("og project: writer failures") {
  model::Project& proj = fresh();
  Bytes out;
  {
    VectorSink sink;
    sink.fail_after = 1000;
    writer().begin(proj, sink, {});
    while (writer().step(4096) == Progress::Running) {}
    CHECK(writer().report().error == FormatError::SinkFailed);
  }
  for (VectorSink::Lie lie : {VectorSink::Lie::TooMany, VectorSink::Lie::OkZero, VectorSink::Lie::End}) {
    VectorSink sink;
    sink.lie = lie;
    writer().begin(proj, sink, {});
    while (writer().step(4096) == Progress::Running) {}
    CHECK(writer().report().error == FormatError::SinkFailed);
  }
  {  // changed during the pattern scan, and during the write
    NEED(proj.create_pattern(1, 8) == model::Status::Ok);
    VectorSink sink;
    writer().begin(proj, sink, {});
    CHECK(writer().step(1) == Progress::Running);
    NEED(proj.set_tempo_x100(9000) == model::Status::Ok);
    CHECK(writer().step(1) == Progress::Failed);
    CHECK(writer().report().error == FormatError::Modified);
  }
  {
    VectorSink sink;
    sink.block_every = 2;
    sink.max_per_write = 77;
    writer().begin(proj, sink, {});
    unsigned steps = 0;
    while (writer().step(4096) == Progress::Running && ++steps < 100000) {}
    CHECK(writer().progress() == Progress::Done);
    CHECK(sink.bytes.size() == 2320);
  }
}
