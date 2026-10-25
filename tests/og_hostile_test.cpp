// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/og_fixture.h"
#include "support/rng.h"

#include <o3os/formats/og.h>
#include <o3os/model/project.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace o3os;
using namespace o3os::test::og;
using formats::FormatError;
using formats::Progress;
namespace og_ns = o3os::formats::og;

// Hostile input in the spirit of PAR-011: any bytes whatever. Deterministic mutations of valid files of every kind,
// half of them with the CRC repaired so the checks behind it run, driven with random budgets and read sizes. Under
// ASan and UBSan the run proves that nothing reads or writes out of bounds; the assertions prove the rest: the
// reader ends, says what was wrong, leaves the project as it found it, and what it accepts is a valid model that
// Export can write and read back. O3OS_FUZZ_SCALE=<n> multiplies the run counts (default 1).

namespace {

#if defined(__arm__)
constexpr std::uint32_t kThin = 40;  // the target runs at a few MIPS
#elif defined(__SANITIZE_THREAD__)
constexpr std::uint32_t kThin = 20;
#else
constexpr std::uint32_t kThin = 1;
#endif

std::uint32_t runs(std::uint32_t base) {
  const char* e = std::getenv("O3OS_FUZZ_SCALE");
  const long v = e != nullptr ? std::strtol(e, nullptr, 10) : 1;
  const std::uint32_t scale = v < 1 ? 1u : (v > 1000 ? 1000u : static_cast<std::uint32_t>(v));
  const std::uint32_t n = base * scale / kThin;
  return n < 8 ? 8 : n;
}

// One to four mutations of a copy of `base`. `crc_at` locates the CRC for the "repair" half (the CRC covers
// [0, crc_at) and sits there); a negative value counts from the end of the mutated file.
Bytes mutate(const Bytes& base, o3os::test::Rng& rng, int crc_at, bool repair) {
  Bytes b = base;
  const unsigned n = 1 + rng.below(4);
  for (unsigned i = 0; i < n; ++i) {
    switch (rng.below(8)) {
      case 0: if (!b.empty()) b[rng.below(static_cast<std::uint32_t>(b.size()))] ^= static_cast<std::uint8_t>(1u << rng.below(8)); break;
      case 1: if (!b.empty()) b[rng.below(static_cast<std::uint32_t>(b.size()))] = 0; break;
      case 2: if (!b.empty()) b[rng.below(static_cast<std::uint32_t>(b.size()))] = 0xFF; break;
      case 3: if (b.size() > 1) b.resize(rng.below(static_cast<std::uint32_t>(b.size()))); break;
      case 4: for (unsigned k = rng.below(40); k > 0; --k) b.push_back(rng.byte()); break;
      case 5:  // a short run set to one value, in the first 64 bytes more often than not
        if (!b.empty()) {
          const std::uint32_t lim = rng.below(3) != 0 ? std::min<std::uint32_t>(64, static_cast<std::uint32_t>(b.size())) : static_cast<std::uint32_t>(b.size());
          const std::uint32_t at = rng.below(lim);
          const std::uint8_t v = rng.byte();
          for (std::uint32_t k = at; k < lim && k < at + 1 + rng.below(8); ++k) b[k] = v;
        }
        break;
      case 6:  // a slice copied over another
        if (b.size() > 8) {
          const std::uint32_t len = 1 + rng.below(std::min<std::uint32_t>(300, static_cast<std::uint32_t>(b.size()) / 2));
          const std::uint32_t from = rng.below(static_cast<std::uint32_t>(b.size()) - len);
          const std::uint32_t to = rng.below(static_cast<std::uint32_t>(b.size()) - len);
          std::memmove(&b[to], &b[from], len);
        }
        break;
      default: if (!b.empty()) b[rng.below(static_cast<std::uint32_t>(std::min<std::size_t>(b.size(), 32)))] = rng.byte(); break;
    }
  }
  if (repair && b.size() > 8) {
    const std::size_t at = crc_at >= 0 ? static_cast<std::size_t>(crc_at) : b.size() - 4;
    if (at + 4 <= b.size()) p32(b, at, formats::crc32(b.data(), at));
  }
  return b;
}

struct Tally {
  unsigned runs = 0, done = 0, failed = 0;
};

// The whole project in a few numbers: enough to notice a reader that leaves something behind after a failure.
struct Snapshot {
  std::uint32_t patterns, instruments, song, generation_of_9;
  bool operator==(const Snapshot& o) const {
    return patterns == o.patterns && instruments == o.instruments && song == o.song && generation_of_9 == o.generation_of_9;
  }
};
Snapshot snap(const model::Project& p) {
  return {p.pattern_count(), p.instrument_count(), p.song_length(), p.pattern_generation(9)};
}

std::size_t random_budget(o3os::test::Rng& rng) {
  static const std::size_t b[] = {1, 3, 64, 769, 4096, 100000};
  return b[rng.below(6)];
}

// Runs the reader to the end over `bytes`. False when it never ends.
template <class R>
bool drive(R& r, const Bytes& bytes, model::ModelArena& a, formats::ReadParams params, o3os::test::Rng& rng,
           Progress& result) {
  o3os::test::MemorySource src(bytes);
  src.max_per_read = rng.below(3) == 0 ? 7 : 100000;
  src.block_every = rng.below(4) == 0 ? 3 : 0;
  r.begin(src, a, params);
  const std::size_t budget = random_budget(rng);
  for (unsigned guard = 0; guard < 2'000'000; ++guard) {
    result = r.step(budget);
    if (result != Progress::Running) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("og hostile: mutated patterns") {
  static og_ns::PatternReader reader;
  static og_ns::PatternWriter writer;
  const Bytes corpus[] = {pattern_bytes(rich_pattern(12, 5, 91)), pattern_bytes(rich_pattern(8, 4, 92)),
                          pattern_bytes(rich_pattern(8, 1, 93))};
  o3os::test::Rng rng(0xA11CE);
  Tally t;
  for (std::uint32_t i = 0; i < runs(3000); ++i) {
    model::Project& p = arena(0).project;
    p.reset();
    NEED(p.create_pattern(9, 5) == model::Status::Ok);
    const Snapshot before = snap(p);
    const Bytes in = mutate(corpus[rng.below(3)], rng, -1, i % 2 == 0);
    formats::ReadParams params;
    params.index = static_cast<std::uint16_t>(rng.below(4) == 0 ? rng.below(300) : 7);
    Progress result = Progress::Running;
    NEED(drive(reader, in, arena(0), params, rng, result));
    ++t.runs;
    if (result == Progress::Failed) {
      ++t.failed;
      NEED(reader.report().error != FormatError::None);
      NEED(std::strlen(reader.report().detail) > 0);
      NEED(snap(p) == before);
    } else {
      ++t.done;
      NEED(reader.report().error == FormatError::None);
      NEED(p.pattern(params.index) != nullptr);
      NEED(p.check_invariants());
      Bytes out;
      formats::WriteParams wp;
      wp.index = params.index;
      NEED(run_writer(writer, p, out, wp) == Progress::Done);
      NEED(out.size() == 9260);
      // what Export wrote reads back as the same pattern
      p.reset();
      NEED(run_reader(reader, out, arena(0), params) == Progress::Done);
    }
  }
  CHECK(t.done > t.runs / 20);    // the mutations are not all fatal: the accepting paths ran
  CHECK(t.failed > t.runs / 20);  // and not all harmless
}

TEST_CASE("og hostile: mutated instruments") {
  static og_ns::InstrumentReader mti(false);
  static og_ns::InstrumentReader pti(true);
  static og_ns::InstrumentWriter writer;
  InstrumentSpec s9;
  s9.pti = false;
  InstrumentSpec s3 = s9;
  s3.version = 3;
  s3.canonical = false;
  InstrumentSpec s6 = s9;
  s6.version = 6;
  s6.canonical = false;
  InstrumentSpec p9;
  p9.frames = 40;
  const Bytes corpus[] = {instrument_bytes(s9), instrument_bytes(s3), instrument_bytes(s6), instrument_bytes(p9)};
  o3os::test::Rng rng(0xBEEF);
  Tally t;
  for (std::uint32_t i = 0; i < runs(6000); ++i) {
    model::Project& p = arena(0).project;
    p.reset();
    const Snapshot before = snap(p);
    const unsigned which = rng.below(4);
    const Bytes in = mutate(corpus[which], rng, which == 1 ? 384 : 388, i % 2 == 0);
    formats::ReadParams params;
    params.index = static_cast<std::uint16_t>(rng.below(5) == 0 ? rng.below(70) : 11);
    params.size_hint = rng.below(4) == 0 ? static_cast<std::uint32_t>(in.size() + rng.below(100)) : 0;
    og_ns::InstrumentReader& r = (which == 3) == (rng.below(8) != 0) ? pti : mti;
    Progress result = Progress::Running;
    NEED(drive(r, in, arena(0), params, rng, result));
    ++t.runs;
    if (result == Progress::Failed) {
      ++t.failed;
      NEED(r.report().error != FormatError::None);
      NEED(snap(p) == before);
    } else {
      ++t.done;
      NEED(p.check_invariants());
      if (r.import_report().instruments == 1) {
        NEED(p.instrument(params.index) != nullptr);
        NEED(p.instrument_count() == 1);
        Bytes out;
        formats::WriteParams wp;
        wp.index = params.index;
        NEED(run_writer(writer, p, out, wp) == Progress::Done);
        NEED(out.size() == 392);
      } else {
        NEED(p.instrument_count() == 0);
      }
    }
  }
  CHECK(t.done > t.runs / 20);
  CHECK(t.failed > t.runs / 20);
}

TEST_CASE("og hostile: mutated projects") {
  static og_ns::ProjectReader reader;
  static og_ns::ProjectWriter writer;
  ProjectSpec v16;
  ProjectSpec v6;
  v6.version = 6;
  v6.fill = ProjectSpec::Fill::Groups;
  ProjectSpec v2;
  v2.version = 2;
  v2.fill = ProjectSpec::Fill::Groups;
  ProjectSpec v14;
  v14.version = 14;
  v14.fill = ProjectSpec::Fill::Groups;
  ProjectSpec v17;  // v17, the layout assumed for 1.9.2 (OS-004c), with a non-zero byte at 0x80C: the OGPM 1.1 chunk
  v17.version = 17;
  v17.fill = ProjectSpec::Fill::Groups;
  const Bytes corpus[] = {project_bytes(v16), project_bytes(v6), project_bytes(v2), project_bytes(v14), project_bytes(v17)};
  o3os::test::Rng rng(0xC0FFEE);
  Tally t;
  for (std::uint32_t i = 0; i < runs(5000); ++i) {
    model::Project& p = arena(0).project;
    p.reset();
    NEED(p.create_pattern(3, 8) == model::Status::Ok);
    const Bytes in = mutate(corpus[rng.below(5)], rng, -1, i % 2 == 0);
    Progress result = Progress::Running;
    NEED(drive(reader, in, arena(0), {}, rng, result));
    ++t.runs;
    if (result == Progress::Failed) {
      ++t.failed;
      NEED(reader.report().error != FormatError::None);
      NEED(p.pattern_count() == 0 && p.instrument_count() == 0 && p.song_length() == 0);  // reset: valid and empty
      NEED(p.check_invariants());
    } else {
      ++t.done;
      NEED(p.check_invariants());
      NEED(p.instrument_count() == 16);
      std::uint16_t groups = 0;
      for (std::uint32_t k = 0; k < p.unknown_count(); ++k) {  // the settings kept are a valid chunk for the writer
        NEED(p.unknown(k)->id == og_ns::kChunkSettings);
        NEED(og_ns::inspect_settings(p.unknown_data(k), p.unknown(k)->length, groups));
      }
      Bytes out;
      formats::WriteParams wp;
      wp.target = static_cast<std::uint8_t>(rng.below(2));  // 1.9.0 (v16) or 1.9.2 (v17): both must read back
      NEED(run_writer(writer, p, out, wp) == Progress::Done);
      NEED(out.size() == (wp.target == og_ns::kTargetOg192 ? 2324u : 2320u));
      NEED(writer.export_report().drops.settings_ignored == 0);
      NEED(run_reader(reader, out, arena(1)) == Progress::Done);
      NEED(!reader.report().unverified_version);
      NEED(reader.import_report().version == (wp.target == og_ns::kTargetOg192 ? 17 : 16));
    }
  }
  CHECK(t.done > t.runs / 20);
  CHECK(t.failed > t.runs / 20);
}

TEST_CASE("og hostile: mutated pattern names") {
  static og_ns::PamdReader reader;
  std::vector<std::string> names(256);
  names[0] = "One";
  names[1] = "Two";
  names[200] = "Two hundred and one";
  const Bytes corpus[] = {pamd_bytes(names), pamd_bytes(names, 3)};
  o3os::test::Rng rng(0xFADE);
  Tally t;
  for (std::uint32_t i = 0; i < runs(2000); ++i) {
    model::Project& p = arena(0).project;
    p.reset();
    for (unsigned id : {1u, 2u, 201u}) NEED(p.create_pattern(id, 4) == model::Status::Ok);
    const Bytes in = mutate(corpus[rng.below(2)], rng, -1, false);
    Progress result = Progress::Running;
    NEED(drive(reader, in, arena(0), {}, rng, result));
    ++t.runs;
    NEED(p.check_invariants());
    NEED(p.pattern_count() == 3);
    if (result == Progress::Failed) {
      ++t.failed;
      NEED(reader.report().error != FormatError::None);
      // the file is checked whole before anything is applied: a failure changes nothing
      NEED(p.unknown_count() == 0);
      for (unsigned id : {1u, 2u, 201u}) NEED(p.pattern(id)->name[0] == '\0');
    } else {
      ++t.done;
      std::uint32_t file_length = 0;
      for (std::uint32_t k = 0; k < p.unknown_count(); ++k) {  // what is kept is valid for the writer
        NEED(p.unknown(k)->id == og_ns::kChunkNames);
        NEED(og_ns::inspect_names(p.unknown_data(k), p.unknown(k)->length, file_length));
      }
      og_ns::PamdWriter w;
      Bytes out;
      NEED(run_writer(w, p, out) == Progress::Done);
      NEED(out.size() <= 12816);
    }
  }
  CHECK(t.done > t.runs / 20);
  CHECK(t.failed > t.runs / 20);
}

TEST_CASE("og hostile: every single byte of a header, set to a few values") {
  static og_ns::PatternReader pattern;
  static og_ns::InstrumentReader pti(true);
  static og_ns::ProjectReader project;
  const Bytes p = pattern_bytes(rich_pattern(12, 5, 99));
  const Bytes i = instrument_bytes({});
  const Bytes j = project_bytes({});
  ProjectSpec s17;
  s17.version = 17;
  const Bytes j17 = project_bytes(s17);
  o3os::test::Rng rng(1);
  for (std::size_t at = 0; at < 32; ++at) {
    for (unsigned value : {0x00u, 0x01u, 0x7Fu, 0x80u, 0xFFu}) {
      const std::uint8_t v = static_cast<std::uint8_t>(value);
      Progress result = Progress::Running;
      model::Project& proj = arena(0).project;
      proj.reset();
      Bytes b = p;
      b[at] = v;
      formats::ReadParams params;
      params.index = 1;
      NEED(drive(pattern, b, arena(0), params, rng, result));
      NEED(proj.check_invariants());
      proj.reset();
      b = i;
      b[at] = v;
      NEED(drive(pti, b, arena(0), params, rng, result));
      NEED(proj.check_invariants());
      b = j;
      b[at] = v;
      NEED(drive(project, b, arena(0), {}, rng, result));
      NEED(proj.check_invariants());
      b = j17;
      b[at] = v;
      NEED(drive(project, b, arena(0), {}, rng, result));
      NEED(proj.check_invariants());
    }
  }
}
