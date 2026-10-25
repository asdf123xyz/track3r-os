// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/og_fixture.h"

#include <o3os/formats/og.h>
#include <o3os/model/project.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

using namespace o3os;
using namespace o3os::test::og;
using formats::FormatError;
using formats::Progress;
using o3os::test::MemorySource;
using o3os::test::VectorSink;

namespace {

// The expected model step for six OG bytes, written the long way round (no shared code with the reader): OG
// note i8, -1 empty, -2 fade, -3 cut, -4 off, the other negatives kept in 129..252; instrument 0 on a note-less
// step names none, 255 reads as none, anything else is slot + 1.
model::Step expected_step(const std::uint8_t* s) {
  const int n = static_cast<std::int8_t>(s[0]);
  model::Step e{};
  if (n == -1) e.note = 0;
  else if (n >= 0) e.note = static_cast<std::uint8_t>(n + 1);
  else if (n == -2) e.note = 253;
  else if (n == -3) e.note = 254;
  else if (n == -4) e.note = 255;
  else e.note = static_cast<std::uint8_t>(257 + n);
  const bool special = e.note == 0 || e.note >= 253;
  if ((s[1] == 0 && special) || s[1] == 255) e.instr = 0;
  else e.instr = static_cast<std::uint8_t>(s[1] + 1);
  e.vol = 255;
  e.fx[0] = {s[2], s[3]};
  e.fx[1] = {s[4], s[5]};
  return e;
}

bool same_step(const model::Step& a, const model::Step& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

model::Project& fresh(int i = 0) {
  model::Project& p = arena(i).project;
  p.reset();
  return p;
}

formats::og::PatternReader& reader() {
  static formats::og::PatternReader r;
  return r;
}

Progress import_pattern(const Bytes& file, unsigned id, formats::og::ImportReport* rep = nullptr, int arena_index = 0,
                        std::size_t budget = 4096) {
  formats::ReadParams params;
  params.index = static_cast<std::uint16_t>(id);
  const Progress p = run_reader(reader(), file, arena(arena_index), params, budget);
  if (rep != nullptr) *rep = reader().import_report();
  return p;
}

// Every step of the pattern is the OG step the spec says, and every track has its length.
void check_pattern(const model::Project& proj, unsigned id, const PatternSpec& spec) {
  const model::Pattern* p = proj.pattern(id);
  NEED(p != nullptr);
  for (unsigned t = 0; t < spec.tracks; ++t) {
    CHECK_MESSAGE(p->track_length[t] == spec.length[t] + 1, "track " << t);
    unsigned bad = 0;
    for (unsigned s = 0; s < 128; ++s) {
      if (!same_step(p->steps[t][s], expected_step(spec.step[t][s]))) ++bad;
    }
    CHECK_MESSAGE(bad == 0, "track " << t << ": " << bad << " steps differ");
  }
  for (unsigned t = spec.tracks; t < model::kMaxTracks; ++t) {  // tracks the file does not hold: empty, first track's length
    CHECK(p->track_length[t] == spec.length[0] + 1);
    for (unsigned s = 0; s < 128; ++s) CHECK(model::is_empty(p->steps[t][s]));
  }
}

}  // namespace

TEST_CASE("og pattern: a 12-track v5 file maps every step, every length") {
  const PatternSpec spec = rich_pattern(12, 5, 1);
  formats::og::ImportReport rep;
  model::Project& proj = fresh();
  NEED(import_pattern(pattern_bytes(spec), 7, &rep) == Progress::Done);
  CHECK(rep.error == FormatError::None);
  CHECK(rep.version == 5);
  CHECK(rep.tracks == 12);
  CHECK(rep.patterns == 1);
  CHECK(!rep.unverified_version);
  CHECK(!rep.crc_mismatch);
  CHECK(proj.pattern_count() == 1);
  CHECK(proj.check_invariants());
  check_pattern(proj, 7, spec);
}

TEST_CASE("og pattern: 8-track files of versions 1 to 4 (6,184 bytes)") {
  for (unsigned v = 1; v <= 4; ++v) {
    const PatternSpec spec = rich_pattern(8, v, 10 + v);
    formats::og::ImportReport rep;
    model::Project& proj = fresh();
    NEED(import_pattern(pattern_bytes(spec), 3, &rep) == Progress::Done);
    CHECK(rep.tracks == 8);
    CHECK(rep.version == v);
    CHECK(!rep.unverified_version);
    check_pattern(proj, 3, spec);
  }
}

TEST_CASE("og pattern: the note byte maps all 256 values one to one, special notes keep no instrument") {
  PatternSpec spec = empty_pattern(8, 4);
  for (unsigned i = 0; i < 256; ++i) {
    spec.step[0][i % 128][0] = static_cast<std::uint8_t>(i);
    spec.step[1 + i / 128][i % 128][0] = static_cast<std::uint8_t>(i);
  }
  model::Project& proj = fresh();
  NEED(import_pattern(pattern_bytes(spec), 1) == Progress::Done);
  const model::Pattern* p = proj.pattern(1);
  NEED(p != nullptr);
  bool seen[256] = {};
  for (unsigned t = 1; t <= 2; ++t) {
    for (unsigned s = 0; s < 128; ++s) {
      seen[p->steps[t][s].note] = true;
    }
  }
  unsigned distinct = 0;
  for (bool b : seen) distinct += b ? 1u : 0u;
  CHECK(distinct == 256);
  CHECK(p->steps[1][0].note == 1);       // note 0 -> 1 (C-0)
  CHECK(p->steps[1][127].note == 128);   // note 127
  CHECK(p->steps[2][0].note == 129);     // byte 128 (-128), the first kept negative
  CHECK(p->steps[2][123].note == 252);   // byte 251 (-5), the last
  CHECK(p->steps[2][124].note == 255);   // byte 252: off
  CHECK(p->steps[2][125].note == 254);   // byte 253: cut
  CHECK(p->steps[2][126].note == 253);   // byte 254: fade
  CHECK(p->steps[2][127].note == 0);     // byte 255: empty

  // instruments: a note names slot + 1; empty, off, cut and fade with byte 0 name none
  PatternSpec ins = empty_pattern(8, 4);
  const std::uint8_t cases[][2] = {{5, 0}, {5, 47}, {5, 63}, {5, 255}, {0xFF, 0}, {0xFF, 9}, {0xFC, 0},
                                   {0xFC, 3}, {0xFD, 0}, {0xFE, 0}, {0xFE, 63}};
  for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
    ins.step[0][i][0] = cases[i][0];
    ins.step[0][i][1] = cases[i][1];
  }
  fresh();
  NEED(import_pattern(pattern_bytes(ins), 1) == Progress::Done);
  const model::Pattern* q = proj.pattern(1);
  NEED(q != nullptr);
  const std::uint8_t want[] = {1, 48, 64, 0, 0, 10, 0, 4, 0, 0, 64};
  for (unsigned i = 0; i < sizeof want; ++i) CHECK_MESSAGE(q->steps[0][i].instr == want[i], "case " << i);
}

TEST_CASE("og pattern: FX bytes and leftovers are kept verbatim, the volume column stays unset") {
  PatternSpec spec = empty_pattern(8, 3);
  spec.step[2][5][0] = 60;
  spec.step[2][5][2] = 18; spec.step[2][5][3] = 77;
  spec.step[2][5][4] = 200; spec.step[2][5][5] = 255;          // a type nobody defines
  spec.step[2][6][3] = 99;                                    // a value on an FX-less column
  model::Project& proj = fresh();
  NEED(import_pattern(pattern_bytes(spec), 2) == Progress::Done);
  const model::Pattern* p = proj.pattern(2);
  NEED(p != nullptr);
  CHECK(p->steps[2][5].fx[0].type == 18);
  CHECK(p->steps[2][5].fx[0].value == 77);
  CHECK(p->steps[2][5].fx[1].type == 200);
  CHECK(p->steps[2][5].fx[1].value == 255);
  CHECK(p->steps[2][5].vol == model::kVolumeUnset);
  CHECK(p->steps[2][6].fx[0].type == 0);
  CHECK(p->steps[2][6].fx[0].value == 99);
}

TEST_CASE("og pattern: the magic PM is accepted, the type field and header bytes are not judged") {
  PatternSpec spec = rich_pattern(8, 4, 2);
  spec.magic[0] = 'P';
  spec.magic[1] = 'M';
  Bytes f = pattern_bytes(spec);
  f[2] = 9;                    // a type that is not 2
  f[14] = f[15] = 0xA5;        // junk in the padding
  f[4] = 77;                   // another firmware
  for (std::size_t i = 16; i < 28; ++i) f[i] = 0x66;
  seal(f);
  model::Project& proj = fresh();
  CHECK(import_pattern(f, 4) == Progress::Done);
  check_pattern(proj, 4, spec);
}

TEST_CASE("og pattern: versions 0 is refused, 6 and up read as unverified") {
  formats::og::ImportReport rep;
  PatternSpec spec = rich_pattern(12, 5, 3);
  Bytes f = pattern_bytes(spec);
  for (unsigned i = 8; i < 12; ++i) f[i] = 0;
  seal(f);
  model::Project& proj = fresh();
  CHECK(import_pattern(f, 1, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::UnsupportedVersion);
  CHECK(proj.pattern_count() == 0);

  for (unsigned v : {6u, 7u, 100u, 255u}) {
    spec.version = static_cast<std::uint8_t>(v);
    fresh();
    CHECK(import_pattern(pattern_bytes(spec), 1, &rep) == Progress::Done);
    CHECK_MESSAGE(rep.unverified_version, "version " << v);
    CHECK(rep.version == v);
    check_pattern(proj, 1, spec);  // same layout: read as far as it is known
  }
  spec.version = 5;
  fresh();
  CHECK(import_pattern(pattern_bytes(spec), 1, &rep) == Progress::Done);
  CHECK(!rep.unverified_version);
}

TEST_CASE("og pattern: not a pattern file") {
  formats::og::ImportReport rep;
  Bytes f = pattern_bytes(rich_pattern(8, 4, 4));
  f[0] = 'X';
  model::Project& proj = fresh();
  CHECK(import_pattern(f, 1, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::BadMagic);
  CHECK(proj.pattern_count() == 0);
  CHECK(import_pattern(Bytes{}, 1, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::Truncated);
}

TEST_CASE("og pattern: a bad CRC refuses the file and leaves the project as it was") {
  for (unsigned tracks : {8u, 12u}) {
    const Bytes good = pattern_bytes(rich_pattern(tracks, 5, 5));
    model::Project& proj = fresh();
    CHECK(proj.create_pattern(9, 16) == model::Status::Ok);  // something already there
    const std::uint32_t generation_before = proj.pattern_generation(9);
    for (std::size_t at : {std::size_t{16}, std::size_t{28}, std::size_t{1000}, good.size() - 5, good.size() - 4,
                           good.size() - 1}) {
      Bytes bad = good;
      bad[at] ^= 0x10;
      formats::og::ImportReport rep;
      CHECK_MESSAGE(import_pattern(bad, 2, &rep) == Progress::Failed, "tracks " << tracks << " at " << at);
      CHECK(rep.error == FormatError::BadCrc);
      CHECK(proj.pattern(2) == nullptr);
      CHECK(proj.pattern_count() == 1);
      CHECK(proj.pattern_generation(9) == generation_before);
    }
  }
}

TEST_CASE("og pattern: a track length byte above 127 is refused") {
  PatternSpec spec = rich_pattern(8, 4, 6);
  spec.length[3] = 128;
  formats::og::ImportReport rep;
  model::Project& proj = fresh();
  CHECK(import_pattern(pattern_bytes(spec), 1, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::Malformed);
  CHECK(proj.pattern_count() == 0);
  spec.length[3] = 127;
  CHECK(import_pattern(pattern_bytes(spec), 1) == Progress::Done);
  CHECK(proj.pattern(1)->track_length[3] == 128);
}

TEST_CASE("og pattern: truncated at every offset") {
  for (unsigned tracks : {8u, 12u}) {
    const Bytes good = pattern_bytes(rich_pattern(tracks, 5, 7));
    model::Project& proj = fresh();
    const std::size_t stride =
#if defined(__arm__)
        37;
#else
        1;
#endif
    for (std::size_t n = 0; n < good.size(); n += (n < 64 || n + 64 > good.size()) ? 1 : stride) {
      const Bytes cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
      formats::og::ImportReport rep;
      const Progress p = import_pattern(cut, 1, &rep);
      const bool failed = p == Progress::Failed;
      CHECK_MESSAGE(failed, "tracks " << tracks << " cut at " << n);
      if (n != 28 + 769 * 8 + 4) {  // (the exception: that length ends an 8-track file, whose CRC the four bytes then fail)
        CHECK_MESSAGE(rep.error == FormatError::Truncated, "tracks " << tracks << " cut at " << n);
      }
      CHECK(proj.pattern_count() == 0);
    }
  }
}

TEST_CASE("og pattern: bytes after the end are refused") {
  formats::og::ImportReport rep;
  model::Project& proj = fresh();
  Bytes f = pattern_bytes(rich_pattern(12, 5, 8));
  f.push_back(0);
  CHECK(import_pattern(f, 1, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::Malformed);
  CHECK(proj.pattern_count() == 0);
  f = pattern_bytes(rich_pattern(8, 4, 8));
  f.push_back(0);  // five bytes after the eighth row look like a ninth row: it is then cut short
  CHECK(import_pattern(f, 1, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::Truncated);
  f = pattern_bytes(rich_pattern(12, 5, 8));
  f.resize(f.size() + 4000, 0);
  CHECK(import_pattern(f, 1, &rep) == Progress::Failed);
  CHECK(proj.pattern_count() == 0);
}

TEST_CASE("og pattern: any step budget and any source behaviour gives the same pattern") {
  const PatternSpec spec = rich_pattern(12, 5, 9);
  const Bytes file = pattern_bytes(spec);
  model::Project& ref = fresh(1);
  NEED(import_pattern(file, 5, nullptr, 1) == Progress::Done);
  const model::Pattern* want = ref.pattern(5);
  NEED(want != nullptr);
  for (std::size_t budget : {std::size_t{1}, std::size_t{7}, std::size_t{769}, std::size_t{4096}}) {
    fresh(0);
    formats::ReadParams params;
    params.index = 5;
    CHECK(run_reader(reader(), file, arena(0), params, budget, 3, 5) == Progress::Done);  // 3-byte reads, a WouldBlock every 5th
    const model::Pattern* got = arena(0).project.pattern(5);
    NEED(got != nullptr);
    CHECK(std::memcmp(got, want, sizeof *got) == 0);
  }
}

TEST_CASE("og pattern: id out of range, duplicate id, full pool") {
  formats::og::ImportReport rep;
  const Bytes f = pattern_bytes(rich_pattern(8, 4, 10));
  model::Project& proj = fresh();
  CHECK(import_pattern(f, 0, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::Malformed);
  CHECK(import_pattern(f, 256, &rep) == Progress::Failed);
  CHECK(import_pattern(f, 3) == Progress::Done);
  const model::Pattern* first = proj.pattern(3);
  NEED(first != nullptr);
  const model::Pattern copy = *first;
  CHECK(import_pattern(pattern_bytes(rich_pattern(8, 4, 11)), 3, &rep) == Progress::Failed);  // not replaced
  CHECK(rep.error == FormatError::Malformed);
  CHECK(std::memcmp(proj.pattern(3), &copy, sizeof copy) == 0);
  for (unsigned id = 1; proj.pattern_count() < model::kMaxPatterns; ++id) {
    if (id != 3) NEED(proj.create_pattern(id, 8) == model::Status::Ok);
  }
  CHECK(import_pattern(f, 200, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::LimitExceeded);
  CHECK(proj.pattern_count() == model::kMaxPatterns);
}

TEST_CASE("og pattern: a failing or lying source") {
  const Bytes f = pattern_bytes(rich_pattern(12, 5, 12));
  formats::og::PatternReader& r = reader();
  model::Project& proj = fresh();
  formats::ReadParams params;
  params.index = 1;
  {
    MemorySource src(f);
    src.fail_at = 3000;
    r.begin(src, arena(0), params);
    while (r.step(4096) == Progress::Running) {}
    CHECK(r.report().error == FormatError::SourceFailed);
    CHECK(proj.pattern_count() == 0);
  }
  for (MemorySource::Lie lie : {MemorySource::Lie::TooMany, MemorySource::Lie::OkZero}) {
    MemorySource src(f);
    src.lie = lie;
    r.begin(src, arena(0), params);
    while (r.step(4096) == Progress::Running) {}
    CHECK(r.report().error == FormatError::SourceFailed);
    CHECK(proj.pattern_count() == 0);
  }
}

// ---- export -------------------------------------------------------------------------------------------------

namespace {
formats::og::PatternWriter& writer() {
  static formats::og::PatternWriter w;
  return w;
}
Progress export_pattern(const model::Project& p, unsigned id, Bytes& out, std::size_t budget = 4096) {
  formats::WriteParams params;
  params.index = static_cast<std::uint16_t>(id);
  return run_writer(writer(), p, out, params, budget);
}
}  // namespace

TEST_CASE("og pattern: OG -> model -> OG is byte-identical for a 12-track v5 file") {
  for (std::uint32_t seed = 1; seed <= 4; ++seed) {
    const Bytes in = pattern_bytes(rich_pattern(12, 5, seed));
    model::Project& proj = fresh();
    NEED(import_pattern(in, 9) == Progress::Done);
    Bytes out;
    NEED(export_pattern(proj, 9, out) == Progress::Done);
    CHECK_MESSAGE(out == in, "seed " << seed);
    CHECK(writer().export_report().drops.tracks_beyond_12 == 0);
    CHECK(!writer().export_report().drops.any_lost());
    Bytes slow;  // one byte per step gives the same file
    NEED(export_pattern(proj, 9, slow, 1) == Progress::Done);
    CHECK(slow == in);
  }
}

TEST_CASE("og pattern: the instrument bytes 48 and 63 (MIDI channels 1 and 16 in this OS's design) import as instruments 49 and 64 and export the same bytes (OS-INSTRMIDI)") {
  PatternSpec spec = empty_pattern(12, 5);
  spec.step[0][0][0] = 60;
  spec.step[0][0][1] = 48;
  spec.step[0][1][0] = 60;
  spec.step[0][1][1] = 63;
  const Bytes in = pattern_bytes(spec);
  model::Project& proj = fresh();
  NEED(import_pattern(in, 3) == Progress::Done);
  const model::Pattern* p = proj.pattern(3);
  NEED(p != nullptr);
  CHECK(p->steps[0][0].instr == 49);
  CHECK(p->steps[0][1].instr == 64);
  Bytes out;
  NEED(export_pattern(proj, 3, out) == Progress::Done);
  CHECK(out == in);
}

TEST_CASE("og pattern: an 8-track file is exported as 12 tracks, the four new ones empty") {
  const PatternSpec spec = rich_pattern(8, 4, 5);
  model::Project& proj = fresh();
  NEED(import_pattern(pattern_bytes(spec), 1) == Progress::Done);
  Bytes out;
  NEED(export_pattern(proj, 1, out) == Progress::Done);
  PatternSpec want = spec;  // the same eight tracks, then four empty ones of the first track's length
  want.tracks = 12;
  for (unsigned t = 8; t < 12; ++t) {
    want.length[t] = spec.length[0];
    for (unsigned s = 0; s < 128; ++s) {
      std::memset(want.step[t][s], 0, 6);
      want.step[t][s][0] = 0xFF;
    }
  }
  want.version = 5;
  CHECK(out == pattern_bytes(want));
}

TEST_CASE("og pattern: the header Export writes") {
  model::Project& proj = fresh();
  NEED(proj.create_pattern(1, 32) == model::Status::Ok);
  Bytes out;
  NEED(export_pattern(proj, 1, out) == Progress::Done);
  NEED(out.size() == 9260);
  const std::uint8_t head[28] = {'K', 'S', 2, 0, 1, 9, 0, 1, 5, 5, 5, 5, 0x2C, 0x24, 0, 0,
                                 0, 0, 0xC8, 0x42, 0, 0, 0, 0, 0, 0, 0, 0};
  CHECK(std::memcmp(out.data(), head, 28) == 0);
  CHECK(out[28] == 31);                                  // 32 steps: the index of the last
  CHECK(std::memcmp(&out[29], "\xFF\x00\x00\x00\x00\x00", 6) == 0);  // an empty step
  CHECK(r32(out, 9256) == formats::crc32(out.data(), 9256));
}

TEST_CASE("og pattern: the volume column goes to a free FX slot as FX 18, and what cannot is counted") {
  model::Project& proj = fresh();
  NEED(proj.create_pattern(1, 32) == model::Status::Ok);
  auto step = [](std::uint8_t note, std::uint8_t vol, std::uint8_t t0, std::uint8_t v0, std::uint8_t t1,
                 std::uint8_t v1) {
    model::Step s{};
    s.note = note;
    s.instr = 3;
    s.vol = vol;
    s.fx[0] = {t0, v0};
    s.fx[1] = {t1, v1};
    return s;
  };
  NEED(proj.set_step(1, 0, 0, step(10, 127, 0, 0, 0, 0)) == model::Status::Ok);    // free slot 1 -> FX 18 = 100
  NEED(proj.set_step(1, 0, 1, step(10, 0, 0, 55, 0, 0)) == model::Status::Ok);     // a leftover value is overwritten
  NEED(proj.set_step(1, 0, 2, step(10, 64, 4, 50, 0, 0)) == model::Status::Ok);    // slot 2 free
  NEED(proj.set_step(1, 0, 3, step(10, 100, 4, 50, 5, 60)) == model::Status::Ok);  // both taken: dropped
  NEED(proj.set_step(1, 0, 4, step(10, 50, 18, 33, 0, 0)) == model::Status::Ok);   // FX 18 already there: dropped
  NEED(proj.set_step(1, 0, 5, step(10, 1, 200, 7, 0, 0)) == model::Status::Ok);    // engine FX cleared, slot freed
  NEED(proj.set_step(1, 0, 6, step(10, model::kVolumeUnset, 4, 9, 0, 0)) == model::Status::Ok);  // no volume
  NEED(proj.set_step(1, 0, 7, step(10, 200, 0, 0, 0, 0)) == model::Status::Ok);    // out of range: full
  NEED(proj.set_step(1, 0, 8, step(10, 127, 130, 1, 140, 2)) == model::Status::Ok);  // both engine: cleared, then FX 18
  Bytes out;
  NEED(export_pattern(proj, 1, out) == Progress::Done);
  auto at = [&out](unsigned s) { return &out[29 + 6 * s]; };
  auto eq = [](const std::uint8_t* p, std::initializer_list<std::uint8_t> v) {
    return std::equal(v.begin(), v.end(), p);
  };
  CHECK(eq(at(0), {9, 2, 18, 100, 0, 0}));
  CHECK(eq(at(1), {9, 2, 18, 0, 0, 0}));
  CHECK(eq(at(2), {9, 2, 4, 50, 18, 50}));
  CHECK(eq(at(3), {9, 2, 4, 50, 5, 60}));
  CHECK(eq(at(4), {9, 2, 18, 33, 0, 0}));
  CHECK(eq(at(5), {9, 2, 18, 1, 0, 0}));
  CHECK(eq(at(6), {9, 2, 4, 9, 0, 0}));
  CHECK(eq(at(7), {9, 2, 18, 100, 0, 0}));
  CHECK(eq(at(8), {9, 2, 18, 100, 0, 0}));
  const formats::og::ExportDrops& d = writer().export_report().drops;
  CHECK(d.volume_as_fx18 == 6);
  CHECK(d.volume_dropped == 2);
  CHECK(d.engine_fx == 3);
  CHECK(d.any_lost());
}

TEST_CASE("og pattern: core FX 43..127 are written as none and counted, every other byte is as it was (ADR 0004 amendment 1)") {
  model::Project& proj = fresh();
  NEED(proj.create_pattern(1, 16) == model::Status::Ok);
  auto step = [](std::uint8_t t0, std::uint8_t v0, std::uint8_t t1, std::uint8_t v1) {
    model::Step s{};
    s.note = 10;
    s.instr = 3;
    s.vol = model::kVolumeUnset;
    s.fx[0] = {t0, v0};
    s.fx[1] = {t1, v1};
    return s;
  };
  // the last OG type and the first core one; the tracker FX 46..50, 51, Channel pan 57 and CF3's Channel volume 58 and Panbrello 62; the last reserved type and the first engine one
  NEED(proj.set_step(1, 0, 0, step(42, 7, 43, 8)) == model::Status::Ok);      // 1 dropped
  NEED(proj.set_step(1, 0, 1, step(46, 0x37, 50, 0x12)) == model::Status::Ok);  // 2
  NEED(proj.set_step(1, 0, 2, step(127, 255, 128, 9)) == model::Status::Ok);  // 1 core, 1 engine
  NEED(proj.set_step(1, 0, 3, step(4, 50, 5, 60)) == model::Status::Ok);      // OG types: kept
  NEED(proj.set_step(1, 0, 4, step(45, 1, 0, 77)) == model::Status::Ok);      // the leftover value of the free column stays
  NEED(proj.set_step(1, 0, 5, step(51, 0x44, 57, 75)) == model::Status::Ok);  // 2: Pan slide and Channel pan (57) are dropped, not turned into the Panning FX (31)
  NEED(proj.set_step(1, 0, 6, step(58, 32, 62, 0x3A)) == model::Status::Ok);  // 2: CF3's Channel volume and Panbrello are dropped likewise (OG export drops 43..127)
  Bytes out;
  NEED(export_pattern(proj, 1, out) == Progress::Done);
  auto at = [&out](unsigned s) { return &out[29 + 6 * s]; };
  auto eq = [](const std::uint8_t* p, std::initializer_list<std::uint8_t> v) { return std::equal(v.begin(), v.end(), p); };
  CHECK(eq(at(0), {9, 2, 42, 7, 0, 0}));
  CHECK(eq(at(1), {9, 2, 0, 0, 0, 0}));
  CHECK(eq(at(2), {9, 2, 0, 0, 0, 0}));
  CHECK(eq(at(3), {9, 2, 4, 50, 5, 60}));
  CHECK(eq(at(4), {9, 2, 0, 0, 0, 77}));
  CHECK(eq(at(5), {9, 2, 0, 0, 0, 0}));
  CHECK(eq(at(6), {9, 2, 0, 0, 0, 0}));
  const formats::og::ExportDrops& d = writer().export_report().drops;
  CHECK(d.core_fx == 9);
  CHECK(d.engine_fx == 1);
  CHECK(d.volume_as_fx18 == 0);
  CHECK(d.any_lost());
  // every other byte of the file is what a file of the same pattern with those cells emptied would hold
  Bytes expect;
  {
    model::Project& p2 = fresh(1);
    NEED(p2.create_pattern(1, 16) == model::Status::Ok);
    NEED(p2.set_step(1, 0, 0, step(42, 7, 0, 0)) == model::Status::Ok);
    NEED(p2.set_step(1, 0, 1, step(0, 0, 0, 0)) == model::Status::Ok);
    NEED(p2.set_step(1, 0, 2, step(0, 0, 0, 0)) == model::Status::Ok);
    NEED(p2.set_step(1, 0, 3, step(4, 50, 5, 60)) == model::Status::Ok);
    NEED(p2.set_step(1, 0, 4, step(0, 0, 0, 77)) == model::Status::Ok);
    NEED(p2.set_step(1, 0, 5, step(0, 0, 0, 0)) == model::Status::Ok);
    NEED(p2.set_step(1, 0, 6, step(0, 0, 0, 0)) == model::Status::Ok);
    NEED(export_pattern(p2, 1, expect) == Progress::Done);
  }
  CHECK(out == expect);
  CHECK(writer().export_report().drops.core_fx == 0);  // the clean pattern drops none
  // a native copy of the pattern still holds the FX
  CHECK(proj.pattern(1)->steps[0][1].fx[0].type == 46);
}

TEST_CASE("og pattern: tracks 13-16 are left out and counted, reserved notes survive") {
  model::Project& proj = fresh();
  NEED(proj.create_pattern(2, 16) == model::Status::Ok);
  model::Step s{};
  s.note = 200;  // a reserved model note: an i8 of -57 in the file
  s.instr = 1;
  s.vol = model::kVolumeUnset;
  NEED(proj.set_step(2, 3, 0, s) == model::Status::Ok);
  s.note = 5;
  NEED(proj.set_step(2, 12, 0, s) == model::Status::Ok);
  NEED(proj.set_step(2, 15, 127, s) == model::Status::Ok);
  NEED(proj.set_step(2, 15, 126, s) == model::Status::Ok);
  NEED(proj.set_track_length(2, 5, 128) == model::Status::Ok);
  Bytes out;
  NEED(export_pattern(proj, 2, out) == Progress::Done);
  CHECK(out[28 + 769 * 3 + 1] == 199);                      // 200 - 1 = 0xC7 = -57
  CHECK(out[28 + 769 * 5] == 127);                          // 128 steps: the index of the last is 127
  CHECK(out[28 + 769 * 0] == 15);
  CHECK(writer().export_report().drops.tracks_beyond_12 == 3);
  // and it reads back as the same pattern
  NEED(import_pattern(out, 4) == Progress::Done);
  const model::Pattern* a = proj.pattern(2);
  const model::Pattern* b = proj.pattern(4);
  NEED(a != nullptr && b != nullptr);
  for (unsigned t = 0; t < 12; ++t) {
    CHECK(a->track_length[t] == b->track_length[t]);
    CHECK(std::memcmp(a->steps[t], b->steps[t], sizeof a->steps[t]) == 0);
  }
}

TEST_CASE("og pattern: writer refusals and failures") {
  model::Project& proj = fresh();
  NEED(proj.create_pattern(1, 8) == model::Status::Ok);
  Bytes out;
  CHECK(export_pattern(proj, 2, out) == Progress::Failed);  // no such pattern
  CHECK(writer().report().error == FormatError::Malformed);
  CHECK(export_pattern(proj, 0, out) == Progress::Failed);
  {  // the sink fails
    VectorSink sink;
    sink.fail_after = 5000;
    formats::WriteParams params;
    params.index = 1;
    writer().begin(proj, sink, params);
    while (writer().step(4096) == Progress::Running) {}
    CHECK(writer().report().error == FormatError::SinkFailed);
  }
  for (VectorSink::Lie lie : {VectorSink::Lie::TooMany, VectorSink::Lie::OkZero, VectorSink::Lie::End}) {
    VectorSink sink;
    sink.lie = lie;
    formats::WriteParams params;
    params.index = 1;
    writer().begin(proj, sink, params);
    while (writer().step(4096) == Progress::Running) {}
    CHECK(writer().report().error == FormatError::SinkFailed);
  }
  {  // the project changes between two steps
    VectorSink sink;
    formats::WriteParams params;
    params.index = 1;
    writer().begin(proj, sink, params);
    CHECK(writer().step(1000) == Progress::Running);
    NEED(proj.set_pattern_name(1, "x", 1) == model::Status::Ok);
    CHECK(writer().step(1000) == Progress::Failed);
    CHECK(writer().report().error == FormatError::Modified);
  }
  {  // a sink that blocks now and then
    VectorSink sink;
    sink.block_every = 3;
    sink.max_per_write = 100;
    formats::WriteParams params;
    params.index = 1;
    writer().begin(proj, sink, params);
    unsigned steps = 0;
    while (writer().step(4096) == Progress::Running && ++steps < 100000) {}
    CHECK(writer().progress() == Progress::Done);
    CHECK(sink.bytes.size() == 9260);
  }
}
