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

model::Project& fresh() {
  model::Project& p = arena(0).project;
  p.reset();
  return p;
}

og_ns::PamdReader& reader() {
  static og_ns::PamdReader r;
  return r;
}

Progress import_names(const Bytes& file, og_ns::ImportReport* rep = nullptr, std::size_t budget = 4096) {
  const Progress p = run_reader(reader(), file, arena(0), {}, budget);
  if (rep != nullptr) *rep = reader().import_report();
  return p;
}

// A project with the given patterns, nothing else.
model::Project& with_patterns(std::initializer_list<unsigned> ids) {
  model::Project& proj = fresh();
  for (unsigned id : ids) CHECK(proj.create_pattern(id, 8) == model::Status::Ok);
  return proj;
}

// The OGPN chunk of a project: its payload, or nullptr.
const std::uint8_t* names_chunk(const model::Project& p, std::uint32_t* length = nullptr) {
  for (std::uint32_t i = 0; i < p.unknown_count(); ++i) {
    if (p.unknown(i)->id == og_ns::kChunkNames) {
      if (length != nullptr) *length = p.unknown(i)->length;
      return p.unknown_data(i);
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE("og pamd: names go to the patterns that exist, record i names pattern i + 1") {
  model::Project& proj = with_patterns({1, 2, 15, 255});
  std::vector<std::string> names(256);
  names[0] = "Intro";                   // pattern 1
  names[1] = "A name of thirty chars long";  // pattern 2
  names[2] = "Nobody home";             // pattern 3 does not exist
  names[14] = std::string("\xC3\xA9t\xC3\xA9 \x01\x7F", 8);  // verbatim bytes
  names[254] = "Last";                  // pattern 255
  names[255] = "No pattern 256";
  og_ns::ImportReport rep;
  NEED(import_names(pamd_bytes(names), &rep) == Progress::Done);
  CHECK(rep.error == FormatError::None);
  CHECK(rep.version == 1);
  CHECK(rep.names_applied == 4);
  CHECK(rep.names_skipped == 1);
  CHECK(!rep.partial_record);
  CHECK(!rep.unverified_version);
  CHECK(rep.names_kept);
  CHECK(std::string(proj.pattern(1)->name) == "Intro");
  CHECK(std::string(proj.pattern(2)->name) == "A name of thirty chars long");
  CHECK(std::string(proj.pattern(15)->name) == names[14]);
  CHECK(std::string(proj.pattern(255)->name) == "Last");
  CHECK(proj.pattern_count() == 4);
  CHECK(proj.check_invariants());
  // the file is kept as it is: OGPN, valid, and no longer than the file's last non-zero byte
  std::uint32_t len = 0;
  const std::uint8_t* chunk = names_chunk(proj, &len);
  NEED(chunk != nullptr);
  std::uint32_t file_len = 0;
  CHECK(og_ns::inspect_names(chunk, len, file_len));
  CHECK(file_len == 12816);
  CHECK(len == 6 + 16 + 50 * 255 + 14);  // record 255 ends in "No pattern 256" (14 characters)
}

TEST_CASE("og pamd: lengths of names, short files, partial records") {
  // 31 characters with no NUL: byte 30 is forced to the end, so 30 are the name
  model::Project& proj = with_patterns({1, 2});
  Bytes f = pamd_bytes({}, 2);
  for (unsigned i = 0; i < 31; ++i) f[16 + i] = 'x';
  og_ns::ImportReport rep;
  NEED(import_names(f, &rep) == Progress::Done);
  CHECK(std::string(proj.pattern(1)->name) == std::string(30, 'x'));
  // a header only: no names, and a chunk that says so
  with_patterns({1});
  CHECK(import_names(pamd_bytes({}, 0), &rep) == Progress::Done);
  CHECK(rep.names_applied == 0);
  CHECK(names_chunk(proj) != nullptr);
  // 16 + 50 + 7 bytes: the partial record is ignored
  Bytes partial = pamd_bytes({"One", "Two"}, 2);
  partial.resize(16 + 50 + 7);
  with_patterns({1, 2});
  CHECK(import_names(partial, &rep) == Progress::Done);
  CHECK(rep.partial_record);
  CHECK(std::string(proj.pattern(1)->name) == "One");
  CHECK(std::string(proj.pattern(2)->name).empty());
  // the first record's other 19 bytes, the reserved bytes, the flags and a total that is not the length: kept
  Bytes junk = pamd_bytes({"One"}, 1);
  for (unsigned i = 31; i < 50; ++i) junk[16 + i] = 0xEE;
  junk[6] = junk[7] = 0x99;
  p32(junk, 12, 0xFFFFFFFFu);
  p32(junk, 8, 7);
  with_patterns({1});
  CHECK(import_names(junk, &rep) == Progress::Done);
  CHECK(std::string(proj.pattern(1)->name) == "One");
  CHECK(rep.names_kept);
}

TEST_CASE("og pamd: bad files, and a failure changes nothing") {
  og_ns::ImportReport rep;
  model::Project& proj = with_patterns({1, 2});
  const Bytes good = pamd_bytes({"One", "Two"}, 256);
  auto unchanged = [&proj]() {
    return std::string(proj.pattern(1)->name).empty() && std::string(proj.pattern(2)->name).empty() &&
           proj.unknown_count() == 0 && proj.pattern_count() == 2;
  };
  for (std::size_t n = 0; n < 16; ++n) {  // every cut inside the header
    CHECK(import_names(Bytes(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n)), &rep) == Progress::Failed);
    CHECK(rep.error == FormatError::Truncated);
    CHECK(unchanged());
  }
  Bytes f = good;
  f[0] = 'X';
  CHECK(import_names(f, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::BadMagic);
  CHECK(unchanged());
  CHECK(import_names(pamd_bytes({"One"}, 256, 0), &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::UnsupportedVersion);
  CHECK(unchanged());
  // version 2: a record layout nobody has seen, no names are read and nothing is kept
  for (unsigned v : {2u, 3u, 255u, 65535u}) {
    CHECK(import_names(pamd_bytes({"One"}, 256, v), &rep) == Progress::Done);
    CHECK(rep.unverified_version);
    CHECK(rep.names_applied == 0);
    CHECK(!rep.names_kept);
    CHECK(unchanged());
  }
  // more than 256 records: refused whole, the names before the excess are not applied
  f = pamd_bytes({"One", "Two"}, 257);
  CHECK(import_names(f, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::LimitExceeded);
  CHECK(unchanged());
  f = pamd_bytes({"One", "Two"}, 256);
  f.push_back(0);  // exactly 256 records and one byte more
  CHECK(import_names(f, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::LimitExceeded);
  CHECK(unchanged());
  // the platform knows it is too long: nothing is read
  {
    MemorySource src(good);
    formats::ReadParams params;
    params.size_hint = 12817;
    reader().begin(src, arena(0), params);
    CHECK(reader().step(4096) == Progress::Failed);
    CHECK(reader().report().error == FormatError::LimitExceeded);
    CHECK(src.position() == 0);
    CHECK(unchanged());
  }
  // a source that fails after the names were seen: still nothing applied
  for (std::size_t at : {std::size_t{20}, std::size_t{200}, std::size_t{6000}, std::size_t{12000}}) {
    MemorySource src(good);
    src.fail_at = at;
    reader().begin(src, arena(0), {});
    while (reader().step(4096) == Progress::Running) {}
    CHECK(reader().report().error == FormatError::SourceFailed);
    CHECK(unchanged());
  }
  for (MemorySource::Lie lie : {MemorySource::Lie::TooMany, MemorySource::Lie::OkZero}) {
    MemorySource src(good);
    src.lie = lie;
    reader().begin(src, arena(0), {});
    while (reader().step(4096) == Progress::Running) {}
    CHECK(reader().report().error == FormatError::SourceFailed);
    CHECK(unchanged());
  }
  // tiny reads, WouldBlock, budget 1: the same result
  CHECK(run_reader(reader(), good, arena(0), {}, 1, 7, 3) == Progress::Done);
  CHECK(std::string(proj.pattern(1)->name) == "One");
  // a second file into the same project is refused, not merged: the project already holds one
  CHECK(import_names(pamd_bytes({"Other"}), &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::Malformed);
  CHECK(std::string(proj.pattern(1)->name) == "One");
  CHECK(proj.unknown_count() == 1);
}

TEST_CASE("og pamd: a full pool still applies the names, and says it kept nothing") {
  model::Project& proj = with_patterns({1});
  NEED(proj.unknown_begin(0x41414141u, 16000) == model::Status::Ok);  // leaves 384 bytes
  Bytes f = pamd_bytes({"One"});
  f[16 + 50 * 255 + 3] = 1;  // a non-zero byte at the very end: the whole 12,816 bytes are kept
  og_ns::ImportReport rep;
  NEED(import_names(f, &rep) == Progress::Done);
  CHECK(std::string(proj.pattern(1)->name) == "One");
  CHECK(rep.names_applied == 1);
  CHECK(!rep.names_kept);
  CHECK(names_chunk(proj) == nullptr);
  CHECK(proj.unknown_dropped_chunks() == 1);
  // the writer then has no file to give back: an empty one, and the name is counted as not written
  og_ns::PamdWriter w;
  Bytes out;
  NEED(run_writer(w, proj, out) == Progress::Done);
  CHECK(out.size() == 12816);
  CHECK(w.export_report().drops.pattern_names == 1);
}

TEST_CASE("og pamd: OG -> model -> OG gives the file back byte for byte, whatever is in it") {
  model::Project& proj = with_patterns({1, 2, 15});
  std::vector<std::string> names(256);
  names[0] = "One";
  names[1] = "Two";
  names[14] = "Fifteen";
  names[99] = "Pattern 100 is not here";      // the file names a pattern the project does not have
  Bytes in = pamd_bytes(names);
  in[6] = in[7] = 0x99;                       // the reserved bytes of the one real file
  p32(in, 12, 0xCAFEF00Du);                   // flags
  p32(in, 8, 12345);                          // a total size that is not the length
  for (unsigned i = 0; i < 256; i += 3) {     // the 19 bytes behind some names
    for (unsigned k = 31; k < 50; ++k) in[16 + 50u * i + k] = static_cast<std::uint8_t>(0xE0 + k + i);
  }
  NEED(import_names(in) == Progress::Done);
  og_ns::PamdWriter w;
  Bytes out;
  NEED(run_writer(w, proj, out) == Progress::Done);
  CHECK(out == in);
  CHECK(w.export_report().drops.pattern_names == 0);
  CHECK(w.export_report().drops.settings_ignored == 0);
  Bytes slow;
  NEED(run_writer(w, proj, slow, {}, 1) == Progress::Done);
  CHECK(slow == in);
  // shorter files come back as short as they were, and a file with a name in its very last record in full
  for (unsigned records : {0u, 1u, 3u, 256u}) {
    Bytes f = pamd_bytes(names, records);
    if (records == 256) f[f.size() - 1] = 0x42;
    with_patterns({1, 2});
    NEED(import_names(f) == Progress::Done);
    NEED(run_writer(w, proj, out) == Progress::Done);
    CHECK_MESSAGE(out == f, records << " records");
  }
  // a file cut in the middle of a record is kept as it is too
  Bytes cut = pamd_bytes(names, 3);
  cut.resize(16 + 50 * 2 + 20);
  cut[cut.size() - 1] = 7;
  with_patterns({1});
  NEED(import_names(cut) == Progress::Done);
  NEED(run_writer(w, proj, out) == Progress::Done);
  CHECK(out == cut);
}

TEST_CASE("og pamd: names given in the OS are not written, they are counted") {
  model::Project& proj = with_patterns({1, 2, 3});
  og_ns::PamdWriter w;
  Bytes out;
  // a project that has no PAMD file: header and 256 empty records
  NEED(proj.set_pattern_name(1, "Mine", 4) == model::Status::Ok);
  NEED(proj.set_pattern_name(3, "Also mine", 9) == model::Status::Ok);
  NEED(run_writer(w, proj, out) == Progress::Done);
  const std::uint8_t head[16] = {'P', 'A', 'M', 'D', 1, 0, 0, 0, 0x10, 0x32, 0, 0, 0, 0, 0, 0};
  NEED(out.size() == 12816);
  CHECK(std::memcmp(out.data(), head, 16) == 0);
  CHECK(std::all_of(out.begin() + 16, out.end(), [](std::uint8_t b) { return b == 0; }));
  CHECK(w.export_report().drops.pattern_names == 2);
  CHECK(w.export_report().drops.any_lost());
  // a project that has one: the file as it was, and the names changed since then counted
  with_patterns({1, 2, 3, 200});
  const Bytes in = pamd_bytes({"One", "Two", "Three"});
  NEED(import_names(in) == Progress::Done);
  NEED(run_writer(w, proj, out) == Progress::Done);
  CHECK(w.export_report().drops.pattern_names == 0);
  NEED(proj.set_pattern_name(2, "Renamed", 7) == model::Status::Ok);
  NEED(proj.set_pattern_name(3, "Thre", 4) == model::Status::Ok);        // a prefix of the file's name is another name
  NEED(proj.set_pattern_name(1, "One", 3) == model::Status::Ok);         // the same name again: nothing to count
  NEED(run_writer(w, proj, out) == Progress::Done);
  CHECK(out == in);
  CHECK(w.export_report().drops.pattern_names == 2);
  // a name where the file has none, in a part of the file that was cut off as zeros
  NEED(proj.set_pattern_name(200, "New", 3) == model::Status::Ok);
  NEED(run_writer(w, proj, out) == Progress::Done);
  CHECK(w.export_report().drops.pattern_names == 3);
  CHECK(out == in);
  // a pattern created (or deleted) after the import: the names would land on other patterns, so the file is not given back (OS-FILES-C H15): the empty file, and every named pattern counted
  CHECK_FALSE(proj.names_stale());
  NEED(proj.create_pattern(201, 4) == model::Status::Ok);
  CHECK(proj.names_stale());
  NEED(run_writer(w, proj, out) == Progress::Done);
  CHECK(w.export_report().drops.pattern_names == 4);  // One (the same name again), Renamed, Thre, New: the empty file has none of them
  CHECK(out.size() == 12816);
  CHECK(out != in);
  CHECK(std::all_of(out.begin() + 16, out.end(), [](std::uint8_t b) { return b == 0; }));
}

TEST_CASE("og pamd: a damaged OGPN chunk is ignored and counted") {
  model::Project& proj = with_patterns({1});
  const Bytes in = pamd_bytes({"One"});
  NEED(import_names(in) == Progress::Done);
  std::uint32_t len = 0;
  const std::uint8_t* good = names_chunk(proj, &len);
  NEED(good != nullptr);
  const Bytes keep(good, good + len);
  og_ns::PamdWriter w;
  Bytes out;
  auto try_chunk = [&](const Bytes& payload) {
    model::Project& p = fresh();
    CHECK(p.create_pattern(1, 4) == model::Status::Ok);
    CHECK(p.unknown_begin(og_ns::kChunkNames, static_cast<std::uint32_t>(payload.size())) == model::Status::Ok);
    CHECK(p.unknown_append(payload.data(), static_cast<std::uint32_t>(payload.size())) == model::Status::Ok);
    CHECK(run_writer(w, p, out) == Progress::Done);
    return w.export_report().drops.settings_ignored;
  };
  CHECK(try_chunk(keep) == 0);
  CHECK(out == in);
  Bytes bad = keep;
  bad[0] = 2;                       // a newer major version
  CHECK(try_chunk(bad) == 1);
  CHECK(out.size() == 12816);       // an empty file instead
  bad = keep;
  p32(bad, 2, 99999);               // a file longer than any PAMD (the u32 is at bytes 2-5: this sets 2..5)
  bad[2] = 0; bad[3] = 1; bad[4] = 0xFF; bad[5] = 0xFF;
  CHECK(try_chunk(bad) == 1);
  bad = keep;
  bad[6] = 'X';                     // not a PAMD
  CHECK(try_chunk(bad) == 1);
  bad = keep;
  bad.resize(5);                    // too short to hold anything
  CHECK(try_chunk(bad) == 1);
  bad = keep;
  bad[2] = 0; bad[3] = 0; bad[4] = 0; bad[5] = 8;  // the kept bytes are longer than the file they are part of
  CHECK(try_chunk(bad) == 1);
  std::uint32_t file_length = 0;
  CHECK(!og_ns::inspect_names(nullptr, 100, file_length));
  CHECK(og_ns::inspect_names(keep.data(), static_cast<std::uint32_t>(keep.size()), file_length));
  CHECK(file_length == 12816);
}

TEST_CASE("og files: names in an OG folder") {
  using og_ns::FileClass;
  struct Row { const char* name; FileClass cls; unsigned index; };
  const Row rows[] = {
      {"project.mt", FileClass::Project, 0},
      {"Project.MT", FileClass::Project, 0},
      {"Projects/Song/project.mt", FileClass::Project, 0},
      {"C:\\card\\Song\\PROJECT.mt", FileClass::Project, 0},
      {"project.mt.bak", FileClass::None, 0},
      {"projects.mt", FileClass::None, 0},
      {"pattern_01.mtp", FileClass::Pattern, 1},
      {"pattern_255.mtp", FileClass::Pattern, 255},
      {"PATTERN_7.MTP", FileClass::Pattern, 7},
      {"pattern_001.mtp", FileClass::Pattern, 1},
      {"pattern_00.mtp", FileClass::None, 0},
      {"pattern_256.mtp", FileClass::None, 0},
      {"pattern_1000.mtp", FileClass::None, 0},
      {"pattern_.mtp", FileClass::None, 0},
      {"pattern_01.mtp.bak", FileClass::None, 0},
      {"pattern_01x.mtp", FileClass::None, 0},
      {"instrument_01.mti", FileClass::Mti, 0},
      {"instrument_48.mti", FileClass::Mti, 47},
      {"instrument_49.mti", FileClass::None, 0},
      {"instrument_00.mti", FileClass::None, 0},
      {"instrument_001.mti", FileClass::None, 0},
      {"instrument_7.mti", FileClass::Mti, 6},
      {"instrument_16.pti", FileClass::Pti, 15},   // the Workspace of firmware 1.5+
      {"INSTRUMENT_01.PTI", FileClass::Pti, 0},
      {"instrument_49.pti", FileClass::None, 0},
      {"instrument_01.pt", FileClass::None, 0},
      {"instrument_01.mtp", FileClass::None, 0},
      {"instrument_01.ptx", FileClass::None, 0},
      {"01 Kick.pti", FileClass::Pti, 0},
      {"1 x.PTI", FileClass::Pti, 0},
      {"48 a.pti", FileClass::Pti, 47},
      {"49 a.pti", FileClass::None, 0},
      {"00 a.pti", FileClass::None, 0},
      {"01a.pti", FileClass::None, 0},
      {"01  .pti", FileClass::Pti, 0},
      {"01 .pti", FileClass::Pti, 0},
      {"001 x.pti", FileClass::Pti, 0},
      {"1000 x.pti", FileClass::None, 0},
      {"._01 kick.pti", FileClass::None, 0},
      {"01 kick.pti.bak", FileClass::None, 0},
      {"01 kick.pt", FileClass::None, 0},
      {"01.pti", FileClass::None, 0},
      {"patternsMetadata", FileClass::PatternNames, 0},
      {"Projects/Song/patterns/PatternsMetadata", FileClass::PatternNames, 0},
      {"patternsMetadata2", FileClass::None, 0},
      {"", FileClass::None, 0},
      {"/", FileClass::None, 0},
  };
  for (const Row& r : rows) {
    const og_ns::FileName n = og_ns::classify_file_name(r.name);
    CHECK_MESSAGE(n.file_class == r.cls, "'" << r.name << "'");
    CHECK_MESSAGE(n.index == r.index, "'" << r.name << "'");
  }
  CHECK(og_ns::classify_file_name(nullptr).file_class == FileClass::None);
  // a long name is no problem
  CHECK(og_ns::classify_file_name((std::string("01 ") + std::string(300, 'a') + ".pti").c_str()).file_class ==
        FileClass::Pti);
}

TEST_CASE("og files: a .pti name built from any instrument name stays in its folder and reads back") {
  char buf[64];
  CHECK(og_ns::format_pti_name(buf, sizeof buf, 0, "Kick") == 11);
  CHECK(std::string(buf) == "01 Kick.pti");
  CHECK(og_ns::format_pti_name(buf, sizeof buf, 47, "") == 7);
  CHECK(std::string(buf) == "48 .pti");
  CHECK(og_ns::format_pti_name(buf, sizeof buf, 3, nullptr) == 7);
  for (const char* evil : {"../../etc/passwd", "..\\..\\x", "/abs", "C:\\x", "a\"b*c?d<e>f|g", "tab\there", "\x7F"}) {
    CHECK(og_ns::format_pti_name(buf, sizeof buf, 9, evil) != 0);
    CHECK_MESSAGE(std::strpbrk(buf, "/\\:\"*?<>|") == nullptr, buf);
    for (const char* c = buf; *c != 0; ++c) CHECK(static_cast<unsigned char>(*c) >= 0x20);
    const og_ns::FileName n = og_ns::classify_file_name(buf);
    CHECK(n.file_class == og_ns::FileClass::Pti);
    CHECK(n.index == 9);
  }
  CHECK(std::string(buf) == "10 _.pti");  // the last of the names above: one DEL
  og_ns::format_pti_name(buf, sizeof buf, 9, "../x");
  CHECK(std::string(buf) == "10 .._x.pti");
  // too small, or a slot an OG folder does not have
  CHECK(og_ns::format_pti_name(buf, 7, 0, "x") == 0);
  CHECK(og_ns::format_pti_name(buf, 8, 0, "x") == 7);
  CHECK(std::string(buf) == "01 .pti");
  CHECK(og_ns::format_pti_name(buf, 10, 0, "abcdefgh") == 9);
  CHECK(std::string(buf) == "01 ab.pti");
  CHECK(og_ns::format_pti_name(buf, sizeof buf, 48, "x") == 0);
  CHECK(og_ns::format_pti_name(nullptr, 64, 0, "x") == 0);
  // every slot reads back
  for (unsigned slot = 0; slot < 48; ++slot) {
    og_ns::format_pti_name(buf, sizeof buf, slot, "name");
    const og_ns::FileName n = og_ns::classify_file_name(buf);
    CHECK(n.file_class == og_ns::FileClass::Pti);
    CHECK(n.index == slot);
  }
}
