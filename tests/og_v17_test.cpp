// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/og_fixture.h"

#include <o3os/formats/native.h>
#include <o3os/formats/og.h>
#include <o3os/model/project.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace o3os;
using namespace o3os::test::og;
using formats::FormatError;
using formats::Progress;
using o3os::test::MemorySource;
using o3os::test::VectorSink;
namespace og_ns = o3os::formats::og;

// OS-004c: project.mt version 17, what official OG 1.9.2 is assumed to write (open research question, unverified: docs/formats/og.md):
// 2,324 bytes, the values block four bytes longer (a new one-byte field at 0x80C), the name at 0x810, the CRC at 0x910. The
// reader takes it as an open research question (no fixture); the byte at 0x80C is carried in OGPM 1.1; Export -> OG has a target, 1.9.0
// (v16, the default) or 1.9.2 (v17). Every file here is synthetic, written from the table of docs/formats/og.md section 3 by support/og_fixture.h.

namespace {

og_ns::ProjectReader& reader() {
  static og_ns::ProjectReader r;
  return r;
}
og_ns::ProjectWriter& writer() {
  static og_ns::ProjectWriter w;
  return w;
}

Progress import_project(const Bytes& f, int a, og_ns::ImportReport* rep = nullptr) {
  const Progress p = run_reader(reader(), f, arena(a));
  if (rep != nullptr) *rep = reader().import_report();
  return p;
}

// Tracks 9-16 as an import of the OG's files makes them (9-12 audio, 13-16 MIDI), so that Export has nothing to count about track types.
void og_tracks(model::Project& proj) {
  for (std::uint32_t slot = 8; slot < model::kMaxTracks; ++slot) {
    model::TrackSlot t = *proj.track(slot);
    t.type = slot < 12 ? model::TrackType::Audio : model::TrackType::Midi;  // what Import makes of them (OS-TRK912)
    CHECK(proj.set_track(slot, t) == model::Status::Ok);
  }
}

Bytes export_project(const model::Project& p, std::uint8_t target, std::size_t budget = 4096) {
  Bytes out;
  formats::WriteParams params;
  params.target = target;
  CHECK(run_writer(writer(), p, out, params, budget) == Progress::Done);
  return out;
}

ProjectSpec spec17(ProjectSpec::Fill fill = ProjectSpec::Fill::Defaults, std::uint8_t byte_80c = 0) {
  ProjectSpec s;
  s.version = 17;
  s.fill = fill;
  s.byte_80c = byte_80c;
  return s;
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
  VectorSink sink;
  w.begin(p, sink);
  while (w.step(4096) == Progress::Running) {}
  return sink.bytes;
}

// The bytes Export rebuilds or defaults rather than carries in a v17 file: header, session state, the three padding bytes after
// the new field, the name's NUL with the second string and its spare byte (uninitialised memory), the CRC.
bool normalised(std::size_t i) {
  return (i >= 4 && i < 16) || (i >= 0x10F && i <= 0x119) || i == 0x11B || i == 0x11F || i == 0x122 || i == 0x123 ||
         i == 0x12B || i == 0x13A || i == 0x13B || i == 0x140 || i == 0x141 || i == 0x1BE || i == 0x1BF ||
         (i >= 0x1C6 && i < 0x2F5) || (i >= 0x80D && i < 0x810) || (i >= 0x830 && i < 0x910) || i >= 0x910;
}

constexpr std::uint32_t kNotKept = og_ns::kDropSession | og_ns::kDropPatternBitmask;
constexpr std::size_t kOgpm17Len = 4 + 4 + 9 + 16 + 10 + 144 + 306 + 460 + 1;  // every group of a v17 project, and the new byte

}  // namespace

TEST_CASE("og v17: the layout is the v16 one with four bytes inserted, the name at 0x810") {
  const ProjectSpec s = spec17();
  const Bytes f = project_bytes(s);
  CHECK(f.size() == 2324);
  CHECK(f[8] == 17);
  CHECK(r16(f, 12) == 2324);
  CHECK(r32(f, 0x910) == formats::crc32(f.data(), 0x910));  // the CRC is over [0, 2320), at 0x910

  og_ns::ImportReport rep;
  model::Project& proj = arena(0).project;
  NEED(import_project(f, 0, &rep) == Progress::Done);
  CHECK(rep.version == 17);
  CHECK(!rep.unverified_version);  // a layout assumed for 1.9.2 (open research question): not "unverified"...
  CHECK(rep.observed_only);       // ... but its own class: no file of it has been seen
  CHECK(!rep.crc_mismatch);
  CHECK(rep.sanitised == 0);
  CHECK(rep.dropped_groups == kNotKept);
  CHECK(rep.kept_groups == 0);
  CHECK(proj.unknown_count() == 0);
  CHECK(std::string(proj.name()) == s.name);
  CHECK(proj.active_tracks() == 12);
  CHECK(std::string(proj.track(8)->name) == "Track 9");  // tracks 9-16 are at 0x603, which did not move (the file's default "Midi 9" becomes "Track 9", OS-TRK912)
  CHECK(std::string(proj.track(15)->name) == "Midi 16");

  // the same values in a v16 file are the same model, byte for byte through the native format
  NEED(import_project(project_bytes(ProjectSpec{}), 1) == Progress::Done);
  CHECK(native_bytes(arena(1).project) == native_bytes(proj));
  CHECK(!reader().import_report().observed_only);
}

TEST_CASE("og v17: nothing of the four inserted bytes or of the second string is read as a name") {
  Bytes f = project_bytes(spec17());
  f[0x80C] = 'P';  // the new field (it is not a name: in a v16 file this would be the name's first character)
  f[0x80D] = f[0x80E] = f[0x80F] = 'Q';
  for (unsigned i = 0; i < 221; ++i) f[0x831 + i] = 'S';  // the second string
  seal(f);
  model::Project& proj = arena(0).project;
  NEED(import_project(f, 0) == Progress::Done);
  CHECK(std::string(proj.name()) == "Synthetic Project");
  // a name that fills its 32 bytes (no NUL, the NUL is at 0x830): 31 characters, as for every version
  f = project_bytes(spec17());
  for (unsigned i = 0; i < 32; ++i) f[0x810 + i] = 'p';
  f[0x830] = 'Z';
  seal(f);
  NEED(import_project(f, 0) == Progress::Done);
  CHECK(std::string(proj.name()) == std::string(31, 'p'));
  // the old position holds no name any more: a v16 reading of the same bytes would see "PQQQ..." there; v17 sees none of it
  f = project_bytes(spec17());
  for (unsigned i = 0; i < 4; ++i) f[0x80C + i] = 'x';
  for (unsigned i = 0; i < 32; ++i) f[0x810 + i] = 0;
  seal(f);
  NEED(import_project(f, 0) == Progress::Done);
  CHECK(std::string(proj.name()).empty());
}

TEST_CASE("og v17: a zero byte at 0x80C needs no chunk; a non-zero one is carried in OGPM 1.1 and written back") {
  og_ns::ImportReport rep;
  model::Project& proj = arena(0).project;
  // zero: the default of a file that never touched the feature
  NEED(import_project(project_bytes(spec17()), 0, &rep) == Progress::Done);
  CHECK(proj.unknown_count() == 0);

  for (unsigned v : {1u, 2u, 0x55u, 0x7Fu, 0x80u, 0xFEu, 0xFFu}) {
    const std::uint8_t value = static_cast<std::uint8_t>(v);
    const Bytes in = project_bytes(spec17(ProjectSpec::Fill::Defaults, value));
    NEED(import_project(in, 0, &rep) == Progress::Done);
    CHECK(rep.kept_groups == og_ns::kDropExt19);
    CHECK(rep.dropped_groups == kNotKept);
    NEED(proj.unknown_count() == 1);
    std::uint32_t len = 0;
    const std::uint8_t* chunk = chunk_of(proj, og_ns::kChunkSettings, &len);
    NEED(chunk != nullptr);
    NEED(len == 4 + 460 + 1);
    CHECK(chunk[0] == 0x01);  // OGPM 1.1
    CHECK(chunk[1] == 0x01);
    CHECK(chunk[2] == 0x02);  // group 9
    CHECK(chunk[3] == 0x00);
    std::uint16_t groups = 0;
    std::uint8_t carried = 0;
    CHECK(og_ns::inspect_settings(chunk, len, groups, &carried));
    CHECK(groups == og_ns::kDropExt19);
    CHECK(carried == value);
    CHECK(chunk[len - 1] == value);
    for (std::uint32_t i = 4; i + 1 < len; ++i) CHECK(chunk[i] == 0);  // the 460 bytes of the block are the defaults
    og_tracks(proj);
    // 1.9.2: the file comes back byte for byte
    const Bytes out = export_project(proj, og_ns::kTargetOg192);
    CHECK(out == in);
    CHECK(out[0x80C] == value);
    CHECK(!writer().export_report().drops.any_lost());
    // 1.9.0: there is no such field in a v16 file (0x80C is the name): counted, and 0x80C holds the name
    const Bytes old = export_project(proj, og_ns::kTargetOg190);
    CHECK(old.size() == 2320);
    CHECK(writer().export_report().drops.project_settings == 1);
    CHECK(writer().export_report().drops.any_lost());
    CHECK(old[0x80C] == 'S');  // "Synthetic Project"
    // through the native format and back
    const Bytes nat = native_bytes(proj);
    formats::native::Reader nr;
    MemorySource src(nat);
    nr.begin(src, arena(1));
    while (nr.step(4096) == Progress::Running) {}
    NEED(nr.progress() == Progress::Done);
    CHECK(nr.report().unknown_kept == 1);
    CHECK(export_project(arena(1).project, og_ns::kTargetOg192) == in);
  }
}

TEST_CASE("og v17: the groups and the new byte together; only the block's other bytes: OGPM stays 1.0") {
  model::Project& proj = arena(0).project;
  og_ns::ImportReport rep;
  // a distinct non-default value in every byte of every group, the new one too
  {
    const Bytes in = project_bytes(spec17(ProjectSpec::Fill::Groups));
    CHECK(in[0x80C] != 0);
    NEED(import_project(in, 0, &rep) == Progress::Done);
    CHECK(rep.kept_groups == (og_ns::kDropDelay | og_ns::kDropLimiter | og_ns::kDropReverb | og_ns::kDropInputs |
                              og_ns::kDropPerformance | og_ns::kDropExt13 | og_ns::kDropExt19));
    std::uint32_t len = 0;
    const std::uint8_t* chunk = chunk_of(proj, og_ns::kChunkSettings, &len);
    NEED(chunk != nullptr);
    CHECK(len == kOgpm17Len);
    CHECK(chunk[1] == 0x01);
    CHECK(chunk[len - 1] == in[0x80C]);
    og_tracks(proj);
    const Bytes out = export_project(proj, og_ns::kTargetOg192);
    NEED(out.size() == in.size());
    CHECK(out[0x80C] == in[0x80C]);
    unsigned stray = 0;  // every byte but the ones Export rebuilds or defaults is the input's, group bytes and 0x80C included
    for (std::size_t i = 0; i < in.size(); ++i) {
      if (in[i] != out[i] && !normalised(i)) ++stray;
    }
    CHECK(stray == 0);
    for (const GroupRange& g : group_ranges(17)) {
      for (std::size_t i = g.begin; i < g.end; ++i) {
        if (out[i] != in[i]) CHECK_MESSAGE(false, "group byte " << i);
      }
    }
    CHECK(writer().export_report().drops.defaulted_groups == (og_ns::kDropSession | og_ns::kDropMidiVelocity));
    CHECK(!writer().export_report().drops.any_lost());
    // read again: the same model, through the native format as well
    NEED(import_project(out, 1) == Progress::Done);
    CHECK(native_bytes(arena(1).project) == native_bytes(proj));
  }
  // the same, with a zero at 0x80C: the block is carried as in v16, OGPM 1.0, 460 bytes
  {
    ProjectSpec s = spec17(ProjectSpec::Fill::Groups, 0);
    s.own_80c = true;
    const Bytes in = project_bytes(s);
    CHECK(in[0x80C] == 0);
    NEED(import_project(in, 0, &rep) == Progress::Done);
    std::uint32_t len = 0;
    const std::uint8_t* chunk = chunk_of(proj, og_ns::kChunkSettings, &len);
    NEED(chunk != nullptr);
    CHECK(len == kOgpm17Len - 1);
    CHECK(chunk[0] == 0x01);
    CHECK(chunk[1] == 0x00);
  }
  // only the 460 bytes differ and 0x80C is zero: still 1.0, and a v16 reader of OGPM could read it
  {
    Bytes in = project_bytes(spec17());
    in[0x700] = 9;
    seal(in);
    NEED(import_project(in, 0, &rep) == Progress::Done);
    CHECK(rep.kept_groups == og_ns::kDropExt19);
    std::uint32_t len = 0;
    const std::uint8_t* chunk = chunk_of(proj, og_ns::kChunkSettings, &len);
    NEED(chunk != nullptr);
    CHECK(len == 4 + 460);
    CHECK(chunk[1] == 0x00);
  }
  // a v16 project never makes a 1.1 chunk: its 0x80C is the first character of the name
  {
    ProjectSpec s;
    s.name = "Zebra";
    s.fill = ProjectSpec::Fill::Groups;
    NEED(import_project(project_bytes(s), 0, &rep) == Progress::Done);
    std::uint32_t len = 0;
    const std::uint8_t* chunk = chunk_of(proj, og_ns::kChunkSettings, &len);
    NEED(chunk != nullptr);
    CHECK(len == kOgpm17Len - 1);
    CHECK(chunk[1] == 0x00);
    s.fill = ProjectSpec::Fill::Defaults;
    NEED(import_project(project_bytes(s), 0, &rep) == Progress::Done);
    CHECK(proj.unknown_count() == 0);
  }
}

TEST_CASE("og v17: the bytes Export normalises in a 1.9.2 project are exactly the ones nobody reads") {
  // junk in the session state, the padding after the new byte, the second string and the spare bytes: what comes out is the
  // canonical file (the new byte is carried, so it is the same in both)
  const Bytes want = project_bytes(spec17(ProjectSpec::Fill::Defaults, 0x6B));
  ProjectSpec j = spec17(ProjectSpec::Fill::Junk, 0x6B);
  j.junk = 4242;
  const Bytes in = project_bytes(j);
  CHECK(in != want);
  CHECK(in[0x80D] != 0);  // the junk is there
  CHECK(in[0x831] != 0);
  model::Project& proj = arena(0).project;
  NEED(import_project(in, 0) == Progress::Done);
  og_tracks(proj);
  CHECK(export_project(proj, og_ns::kTargetOg192) == want);
  CHECK(!writer().export_report().drops.any_lost());
}

TEST_CASE("og v17: v16 to v17 conversion, and back") {
  ProjectSpec s;
  s.fill = ProjectSpec::Fill::Groups;
  s.name = "Converted Project";
  NEED(import_project(project_bytes(s), 0) == Progress::Done);
  model::Project& proj = arena(0).project;
  og_tracks(proj);
  const Bytes v16 = export_project(proj, og_ns::kTargetOg190);
  const Bytes v17 = export_project(proj, og_ns::kTargetOg192);
  NEED(v16.size() == 2320);
  NEED(v17.size() == 2324);
  CHECK(!writer().export_report().drops.any_lost());
  // the header: firmware 1.9.2.255, version 17 four times, the size field 2,324
  const std::uint8_t head[16] = {'M', 'T', 0, 0, 1, 9, 2, 255, 17, 17, 17, 17, 0x14, 0x09, 0, 0};
  CHECK(std::memcmp(v17.data(), head, 16) == 0);
  // [0x10, 0x80C) did not move
  CHECK(std::equal(v16.begin() + 0x10, v16.begin() + 0x80C, v17.begin() + 0x10));
  // the new field and its padding are zero (0 is the new field's default; in a v16 file 0x80C is the name's
  // first character)
  CHECK(v16[0x80C] == 'C');
  for (std::size_t i = 0x80C; i < 0x810; ++i) CHECK_MESSAGE(v17[i] == 0, "byte " << i);
  // the name, its NUL and the second string moved up by four; the second string is empty in both
  CHECK(std::equal(v16.begin() + 0x80C, v16.begin() + 0x82D, v17.begin() + 0x810));
  CHECK(std::string(reinterpret_cast<const char*>(&v17[0x810])) == "Converted Project");
  for (std::size_t i = 0x831; i < 0x910; ++i) CHECK_MESSAGE(v17[i] == 0, "byte " << i);
  for (std::size_t i = 0x82D; i < 0x90C; ++i) CHECK_MESSAGE(v16[i] == 0, "byte " << i);
  // the CRC moved with the end of the file
  CHECK(r32(v17, 0x910) == formats::crc32(v17.data(), 0x910));
  CHECK(r32(v16, 0x90C) == formats::crc32(v16.data(), 0x90C));
  // both read back as the model they were written from
  const Bytes nat = native_bytes(proj);
  og_ns::ImportReport rep;
  NEED(import_project(v17, 1, &rep) == Progress::Done);
  CHECK(rep.version == 17);
  CHECK(rep.observed_only);
  CHECK(!rep.crc_mismatch);
  CHECK(native_bytes(arena(1).project) == nat);
  NEED(import_project(v16, 1, &rep) == Progress::Done);
  CHECK(rep.version == 16);
  CHECK(native_bytes(arena(1).project) == nat);
  // a v17 project with a zero new byte exports to 1.9.0 as exactly this v16 file
  NEED(import_project(v17, 1) == Progress::Done);
  CHECK(export_project(arena(1).project, og_ns::kTargetOg190) == v16);
  CHECK(writer().export_report().drops.project_settings == 0);
}

TEST_CASE("og v17: the CRC is judged as for every known layout, the length is exact") {
  og_ns::ImportReport rep;
  model::Project& proj = arena(0).project;
  // a CRC that does not match is only warned about (our rule, docs/formats/og.md), and the data is used
  Bytes f = project_bytes(spec17());
  f[0x12C + 2] = 1;
  CHECK(import_project(f, 0, &rep) == Progress::Done);
  CHECK(rep.crc_mismatch);
  CHECK(!rep.unverified_version);
  CHECK(proj.track(2)->flags == model::kTrackMute);
  f = project_bytes(spec17());
  f[0x910] ^= 0x80;  // the CRC itself
  CHECK(import_project(f, 0, &rep) == Progress::Done);
  CHECK(rep.crc_mismatch);
  // the CRC is at 0x910 over 2,320 bytes: one computed the v16 way, over [0, 2316) at 0x90C, does not match
  f = project_bytes(spec17());
  p32(f, 0x910, 0);
  p32(f, 0x90C, formats::crc32(f.data(), 0x90C));
  CHECK(import_project(f, 0, &rep) == Progress::Done);
  CHECK(rep.crc_mismatch);
  // for contrast: a version nobody has seen needs a CRC that matches
  f = project_bytes(spec17());
  f[8] = f[9] = f[10] = f[11] = 18;
  f[0x300] ^= 1;
  CHECK(import_project(f, 0, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::BadCrc);
  CHECK(proj.instrument_count() == 0);
  // the length is the version's own: a v17 header on the 2,320 bytes of v16 is short, a v16 header on 2,324 is long
  f = project_bytes(ProjectSpec{});
  f[8] = f[9] = f[10] = f[11] = 17;
  seal(f);
  CHECK(import_project(f, 0, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::Truncated);
  f = project_bytes(spec17());
  f[8] = f[9] = f[10] = f[11] = 16;
  seal(f);
  CHECK(import_project(f, 0, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::Malformed);
  f = project_bytes(spec17());
  f.push_back(0);
  CHECK(import_project(f, 0, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::Malformed);
  CHECK(proj.instrument_count() == 0);  // reset, valid and empty
  CHECK(proj.check_invariants());
}

TEST_CASE("og v17: Export targets") {
  model::Project& proj = arena(0).project;
  proj.reset();
  og_tracks(proj);
  NEED(proj.set_name("ABCDEFGHIJKLMNOPQRSTUVWXYZ012345", 32) == model::Status::Ok);
  NEED(proj.set_tempo_x100(12025) == model::Status::Ok);
  for (std::uint32_t id : {3u, 1u, 255u}) NEED(proj.append_order(id) == model::Status::Ok);
  // no target given: 1.9.0, v16, as before this slice
  Bytes out;
  NEED(run_writer(writer(), proj, out) == Progress::Done);
  CHECK(out.size() == 2320);
  const std::uint8_t head16[16] = {'M', 'T', 0, 0, 1, 9, 0, 255, 16, 16, 16, 16, 0x10, 0x09, 0, 0};
  CHECK(std::memcmp(out.data(), head16, 16) == 0);
  CHECK(std::string(reinterpret_cast<const char*>(&out[0x80C])) == "ABCDEFGHIJKLMNOPQRSTUVWXYZ01234");
  CHECK(og_ns::kTargetOg190 == 0);
  const Bytes v16 = export_project(proj, og_ns::kTargetOg190);
  CHECK(v16 == out);
  const og_ns::ExportDrops cut16 = writer().export_report().drops;
  // 1.9.2: v17, the name at 0x810 cut to 31 characters, counted as for 1.9.0
  const Bytes v17 = export_project(proj, og_ns::kTargetOg192);
  NEED(v17.size() == 2324);
  const std::uint8_t head17[16] = {'M', 'T', 0, 0, 1, 9, 2, 255, 17, 17, 17, 17, 0x14, 0x09, 0, 0};
  CHECK(std::memcmp(v17.data(), head17, 16) == 0);
  CHECK(std::string(reinterpret_cast<const char*>(&v17[0x810])) == "ABCDEFGHIJKLMNOPQRSTUVWXYZ01234");
  CHECK(v17[0x80C] == 0);
  CHECK(v17[0x82F] == 0);
  CHECK(r32(v17, 0x910) == formats::crc32(v17.data(), 0x910));
  CHECK(writer().export_report().drops.project_settings == cut16.project_settings);
  CHECK(writer().export_report().drops.project_settings == 1);  // the name was cut
  CHECK(std::equal(v16.begin() + 0x10, v16.begin() + 0x80C, v17.begin() + 0x10));
  // any other target is refused before a byte is written
  for (unsigned t : {2u, 3u, 127u, 255u}) {
    VectorSink sink;
    formats::WriteParams params;
    params.target = static_cast<std::uint8_t>(t);
    writer().begin(proj, sink, params);
    CHECK(writer().progress() == Progress::Failed);
    CHECK(writer().step(4096) == Progress::Failed);
    CHECK(writer().report().error == FormatError::Malformed);
    CHECK(sink.bytes.empty());
  }
  // the writer is reusable, in either order, and a slow sink and a one-byte budget give the same files
  CHECK(export_project(proj, og_ns::kTargetOg192) == v17);
  CHECK(export_project(proj, og_ns::kTargetOg190) == v16);
  CHECK(export_project(proj, og_ns::kTargetOg192, 1) == v17);
  {
    VectorSink sink;
    sink.block_every = 2;
    sink.max_per_write = 77;
    formats::WriteParams params;
    params.target = og_ns::kTargetOg192;
    writer().begin(proj, sink, params);
    unsigned steps = 0;
    while (writer().step(4096) == Progress::Running && ++steps < 100000) {}
    CHECK(writer().progress() == Progress::Done);
    CHECK(sink.bytes == v17);
  }
  // the project changing under the writer is caught for the new target too
  {
    VectorSink sink;
    formats::WriteParams params;
    params.target = og_ns::kTargetOg192;
    writer().begin(proj, sink, params);
    NEED(proj.set_tempo_x100(9000) == model::Status::Ok);
    CHECK(writer().step(4096) == Progress::Failed);
    CHECK(writer().report().error == FormatError::Modified);
  }
}

TEST_CASE("og v17: an OGPM chunk is valid by its version, and the new byte is only written where it fits") {
  struct Case {
    const char* what;
    std::uint16_t version, groups;
    std::size_t body;     // bytes after the four of the header
    std::uint8_t last;    // the last byte of the payload
    bool valid;
    std::uint8_t tail;    // what inspect_settings reports as the carried byte
  };
  const Case cases[] = {
      {"1.1, group 9 and its byte", 0x0101, 0x0200, 461, 0x77, true, 0x77},
      {"1.1, group 9 and a zero byte", 0x0101, 0x0200, 461, 0x00, true, 0},
      {"1.1, group 9 without its byte", 0x0101, 0x0200, 460, 0x77, false, 0},
      {"1.0, group 9 with a byte too many", 0x0100, 0x0200, 461, 0x77, false, 0},
      {"1.0, group 9", 0x0100, 0x0200, 460, 0x77, true, 0},
      {"1.1, no group 9", 0x0101, 0x0002, 4, 0x77, true, 0},
      {"1.1, no group 9, a byte appended", 0x0101, 0x0002, 5, 0x77, true, 0},
      {"1.2, group 9, the byte and more", 0x0102, 0x0200, 463, 0x77, true, 0x99},
      {"1.1, delay and group 9", 0x0101, 0x0202, 465, 0x55, true, 0x55},
      {"2.0, group 9 and its byte", 0x0201, 0x0200, 461, 0x77, false, 0},
  };
  model::Project& proj = arena(0).project;
  for (const Case& c : cases) {
    Bytes payload(4 + c.body, 0);
    payload[0] = static_cast<std::uint8_t>(c.version >> 8);
    payload[1] = static_cast<std::uint8_t>(c.version);
    payload[2] = static_cast<std::uint8_t>(c.groups >> 8);
    payload[3] = static_cast<std::uint8_t>(c.groups);
    payload.back() = c.last;
    if (c.version == 0x0102 && c.valid) payload[4 + 460] = 0x99;  // the byte of 1.1 is the first one after group 9
    std::uint16_t groups = 0;
    std::uint8_t tail = 0xEE;
    CHECK_MESSAGE(og_ns::inspect_settings(payload.data(), static_cast<std::uint32_t>(payload.size()), groups, &tail) == c.valid,
                  c.what);
    CHECK_MESSAGE(tail == c.tail, c.what);
    CHECK_MESSAGE(groups == (c.valid ? c.groups : 0), c.what);
    // Export, with the chunk in the project
    proj.reset();
    og_tracks(proj);
    NEED(proj.set_name("Zed", 3) == model::Status::Ok);
    NEED(proj.unknown_begin(og_ns::kChunkSettings, static_cast<std::uint32_t>(payload.size())) == model::Status::Ok);
    NEED(proj.unknown_append(payload.data(), static_cast<std::uint32_t>(payload.size())) == model::Status::Ok);
    const Bytes v17 = export_project(proj, og_ns::kTargetOg192);
    CHECK_MESSAGE(v17[0x80C] == (c.valid ? c.tail : 0), c.what);
    CHECK_MESSAGE(writer().export_report().drops.settings_ignored == (c.valid ? 0u : 1u), c.what);
    const Bytes v16 = export_project(proj, og_ns::kTargetOg190);
    CHECK_MESSAGE(v16[0x80C] == 'Z', c.what);  // the name, whatever the chunk holds
    CHECK_MESSAGE(writer().export_report().drops.project_settings == (c.valid && c.tail != 0 ? 1u : 0u), c.what);
    CHECK_MESSAGE(writer().export_report().drops.any_lost() == (!c.valid || c.tail != 0), c.what);
  }
  std::uint16_t groups = 0;
  std::uint8_t tail = 0xEE;
  CHECK(!og_ns::inspect_settings(nullptr, 8, groups, &tail));
  CHECK(tail == 0);
  CHECK(!og_ns::inspect_settings(nullptr, 8, groups));  // the byte is optional to ask for
}
