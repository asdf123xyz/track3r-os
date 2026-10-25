// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "support/memory_io.h"
#include "support/mmd_fixture.h"
#include "support/need.h"

#include <o3os/app/import_job.h>
#include <o3os/core/job.h>
#include <o3os/formats/builtin.h>
#include <o3os/formats/med/import.h>
#include <o3os/formats/med/map.h>
#include <o3os/formats/med/mmd.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using namespace o3os::formats::med;
using o3os::formats::Progress;
using o3os::test::mmd::BlockSpec;
using o3os::test::mmd::Bytes;
using o3os::test::mmd::InstrSpec;
using o3os::test::mmd::ModuleSpec;

// The importer's cases that need megabytes of module (host only, like med_oversize_test): loops that 16-bit positions cannot
// tell to the frame, the sample arena filling up, an instrument that alone does not fit, a block built to be slow. The small
// cases are in med_import_test.cpp and med_map_test.cpp, which also run on the Cortex-M4F under QEMU.

namespace {

struct Run {
  std::unique_ptr<model::ModelArena> arena = std::make_unique<model::ModelArena>();
  std::unique_ptr<Importer> importer = std::make_unique<Importer>();
  Bytes bytes;
  std::unique_ptr<SpanSource> src;
  Progress end = Progress::Failed;
  explicit Run(const ModuleSpec& m, ImportOptions opt = ImportOptions()) : bytes(test::mmd::build(m)) {
    src = std::make_unique<SpanSource>(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
    importer->begin(*src, *arena, opt);
    do {
      end = importer->step(1u << 20);
    } while (end == Progress::Running);
  }
  bool ok() const { return end == Progress::Done; }
  const ImportReport& rep() const { return importer->report(); }
  const model::Project& p() const { return arena->project; }
};

ModuleSpec song_with(std::vector<InstrSpec> instruments) {
  ModuleSpec m;
  m.version = 1;
  BlockSpec b;
  b.tracks = 2;
  b.lines = 2;
  m.blocks = {b};
  m.playseq = {0};
  m.instruments = std::move(instruments);
  return m;
}

InstrSpec silence(std::size_t frames) {
  InstrSpec s;
  s.data.assign(frames, 0x11);
  return s;
}

}  // namespace

TEST_CASE("med import big: loops that 65,536 positions cannot tell to the frame are flagged, and tiny ones are dropped") {
  // 70,000 frames: a loop of 25,060 + 10,000 starts between two positions (23,462 comes back as 25,061)
  InstrSpec s = silence(70000);
  s.rep = 12530;
  s.replen = 5000;
  {
    Run r(song_with({s}));
    NEED(r.ok());
    const InstReport& i = r.rep().instrument[0];
    CHECK((i.flags & kInstLoopQuantised) != 0);
    const model::Instrument* in = r.p().instrument(0);
    NEED(in != nullptr);
    CHECK(in->sampler.play_mode == 1);
    CHECK(in->sampler.loop_start == 23462);
    CHECK(in->sampler.loop_end == 32824);
  }
  // 20,000 + 4,000 survives the round trip: 18,724 and 22,469 are exactly those frames
  s.rep = 10000;
  s.replen = 2000;
  {
    Run r(song_with({s}));
    NEED(r.ok());
    CHECK((r.rep().instrument[0].flags & kInstLoopQuantised) == 0);
    NEED(r.p().instrument(0) != nullptr);
    CHECK(r.p().instrument(0)->sampler.loop_start == 18724);
    CHECK(r.p().instrument(0)->sampler.loop_end == 22469);
  }
  // 4 frames in a million fall on one position: no loop, said so
  InstrSpec tiny = silence(1000000);
  tiny.rep = 50000;
  tiny.replen = 2;
  Run r(song_with({tiny}));
  NEED(r.ok());
  CHECK((r.rep().instrument[0].flags & kInstLoopQuantised) != 0);
  NEED(r.p().instrument(0) != nullptr);
  CHECK(r.p().instrument(0)->sampler.play_mode == 0);
}

TEST_CASE("med import big: the sample arena fills up, and what does not fit is refused with its number, never cut") {
  // 4 Mi frames of 16-bit PCM are 8 MiB: the first fits the 10,816 KiB arena, the second finds 2,816 KiB left, the third (small) fits
  Run r(song_with({silence(4u << 20), silence(4u << 20), silence(1000)}));
  NEED(r.ok());
  const ImportReport& rep = r.rep();
  CHECK(rep.instrument[0].result == InstResult::Sample);
  CHECK(rep.instrument[1].result == InstResult::ArenaFull);
  CHECK(rep.instrument[2].result == InstResult::Sample);
  CHECK(rep.instruments == 2);
  CHECK(rep.instruments_refused == 1);
  CHECK(rep.pcm_bytes == (8ull << 20) + 2000);
  CHECK(r.p().instrument(0) != nullptr);
  CHECK(r.p().instrument(1) == nullptr);   // the slot stays free: the step that names it plays nothing
  CHECK(r.p().instrument(2) != nullptr);
  CHECK(r.p().check_invariants());
  // one instrument alone beyond the arena: 5.5 Mi frames are 11 MiB
  Run big(song_with({silence(5632u << 10)}));
  NEED(big.ok());
  CHECK(big.rep().instrument[0].result == InstResult::Oversize);
  CHECK(big.rep().instruments_refused == 1);
  CHECK(big.p().instrument_count() == 0);
}

TEST_CASE("med import big: what the PCM needs, not what the file holds, decides: a stereo sample is mixed down to half") {
  // 3 Mi frames on two channels are 12 MiB of 16-bit PCM in the reader's count, 6 MiB once mixed down: it fits
  InstrSpec st;
  st.type = 0x20;
  st.data.assign(2u * (3u << 20), 0x22);
  Run r(song_with({st}));
  NEED(r.ok());
  CHECK(r.rep().instrument[0].result == InstResult::Sample);
  CHECK(r.rep().instrument[0].frames == (3u << 20));
  CHECK(r.rep().pcm_bytes == (6ull << 20));
  std::vector<std::int16_t> pcm(4);
  CHECK(r.importer->read_pcm(0, (3u << 20) - 4, 4, pcm.data()).ok());
  for (std::int16_t v : pcm) CHECK(v == 0x2200);
}

TEST_CASE("med import big: a block built to be slow is refused by the work cap, not walked") {
  // 64 tracks and 16 shared command pages: 1,088 cells a line, so the cap of 2^21 cells ends it after 1,928 lines
  ModuleSpec m = song_with({silence(8)});
  BlockSpec b;
  b.tracks = 64;
  b.lines = 1930;
  b.share_pages = true;
  for (unsigned p = 0; p < 16; ++p) b.page_cell(p, 0, 0, 0x01, 0x10);
  m.blocks = {b};
  {
    Run r(m);
    CHECK(!r.ok());
    CHECK(r.importer->status().error == Error::LimitExceeded);
    CHECK(r.rep().overflow);
    CHECK(r.p().pattern_count() == 0);
    CHECK(r.p().check_invariants());
  }
  ImportOptions opt;
  opt.overflow = Overflow::Structural;
  Run s(m, opt);
  CHECK(!s.ok());   // the fallback counts the same lines: it cannot do better
  CHECK(s.importer->status().error == Error::LimitExceeded);
  CHECK(s.p().check_invariants());
}

TEST_CASE("med import big: 64 tracks of a block that fits: the busiest 16 are kept") {
  ModuleSpec m = song_with({silence(8)});
  BlockSpec b;
  b.tracks = 64;
  b.lines = 100;
  for (unsigned t = 0; t < 64; ++t) {
    b.set(0, t, 13, 1, 0, 0);
    if (t >= 40) b.set(1, t, 14, 1, 0, 0);   // the last 24 carry two notes
  }
  m.blocks = {b};
  Run r(m);
  NEED(r.ok());
  CHECK(r.rep().tracks_used == 64);
  CHECK(r.rep().tracks_kept == 16);
  CHECK(r.rep().tracks_dropped == 48);
  for (unsigned t = 40; t < 56; ++t) CHECK(r.rep().track[t].slot == t - 40);   // 24 busy tracks, the 16 lowest of them
  for (unsigned t = 56; t < 64; ++t) CHECK(r.rep().track[t].slot == kNoSlot);
  for (unsigned t = 0; t < 40; ++t) CHECK(r.rep().track[t].slot == kNoSlot);
  CHECK(r.p().active_tracks() == 16);
  CHECK(r.p().check_invariants());
}

TEST_CASE("med import big: through the app's import job, as the file browser will run it") {
  auto storage = std::make_unique<formats::BuiltinFormats>();
  formats::FormatRegistry registry;
  NEED(formats::register_builtin_formats(registry, *storage));
  const formats::FormatDesc* d = registry.find_name("Songs/Mine.MED");
  NEED(d != nullptr);
  CHECK(std::string(d->id) == "med");
  auto arena = std::make_unique<model::ModelArena>();
  ModuleSpec m = test::mmd::sample_module(1);
  const Bytes bytes = test::mmd::build(m);
  SpanSource random(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  test::MemorySource unused(nullptr, 0);
  formats::ReadParams params;
  params.random = &random;
  params.size_hint = static_cast<std::uint32_t>(bytes.size());
  app::ImportJob job;
  job.begin(*d->reader, unused, *arena, "MINE.MED", params);
  CHECK(!job.cancellable());
  unsigned steps = 0;
  unsigned last = 0;
  while (job.step(200) == core::JobStatus::Running) {
    CHECK(job.progress() >= last);
    last = job.progress();
    NEED(++steps < 100000);
  }
  CHECK(job.progress() == core::kJobProgressMax);
  CHECK(std::string(job.detail()).empty());
  NEED(job.report() != nullptr);
  CHECK(job.report()->patterns >= 1);
  CHECK(arena->project.pattern_count() == job.report()->patterns);
  CHECK(arena->project.check_invariants());
  // a refusal reads as "too big for memory", and the project is empty
  ModuleSpec too_big = m;
  too_big.blocks.clear();
  std::vector<std::uint8_t> seq;
  for (unsigned i = 0; i < 130; ++i) {
    BlockSpec b;
    b.tracks = 1;
    b.lines = 1;
    b.set(0, 0, 13, 1, 0, 0);
    too_big.blocks.push_back(b);
    seq.push_back(static_cast<std::uint8_t>(i));
  }
  too_big.playseq = seq;
  const Bytes big_bytes = test::mmd::build(too_big);
  SpanSource random2(big_bytes.data(), static_cast<std::uint32_t>(big_bytes.size()));
  params.random = &random2;
  app::ImportJob job2;
  job2.begin(*d->reader, unused, *arena, "BIG.MED", params);
  while (job2.step(100000) == core::JobStatus::Running) {
  }
  CHECK(std::string(job2.detail()) == "TOO BIG FOR MEMORY");
  CHECK(arena->project.pattern_count() == 0);
}
