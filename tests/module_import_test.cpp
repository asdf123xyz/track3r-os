// SPDX-License-Identifier: GPL-3.0-or-later
// OS-FILES part A: the module import loader (app/module_import.h) over a card that holds generated MOD, S3M and OctaMED files (FakeStorage with bytes): the project the importer makes,
// the samples into the arena through the PCM feed, the engine option, a file cut short, a file that is not a module, a card without the file, an arena without room, steps of any size,
// and the texts of the strip and the report page. The importers' own rules are modimp_import_test.cpp and med_import_test.cpp.
#include <doctest/doctest.h>

#include "support/mmd_fixture.h"
#include "support/modimp_fixture.h"
#include "support/need.h"
#include "support/static_slot.h"

#include <o3os/app/module_import.h>
#include <o3os/app/sample_bank.h>
#include <o3os/audio/voices.h>
#include <o3os/platform/host/fake_storage.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using app::ImportEngine;
using app::ModuleImport;
using app::ModuleSummary;
using app::OgSampleLoader;
using Bytes = std::vector<std::uint8_t>;

namespace {

Bytes mod_bytes() {
  test::modimp::ModSpec m;
  m.name = "kc test";
  m.samples[0].name = "kick";
  for (unsigned i = 0; i < 64; ++i) {
    m.samples[0].data.push_back(static_cast<std::uint8_t>((i * 3u) & 0x7Fu));  // 0..126, positive 8-bit
  }
  m.samples[0].volume = 64;
  m.patterns[0][{0, 0}] = test::modimp::note(12, 1);       // period 428, sample 1
  m.patterns[0][{4, 1}] = test::modimp::note(14, 1, 0xC, 0x20);
  m.patterns[0][{8, 2}] = test::modimp::note(16, 1, 0x1, 0x04);
  return test::modimp::build(m);
}

Bytes s3m_bytes() {
  test::modimp::S3mSpec m;
  m.name = "s3m test";
  m.instruments.resize(1);
  m.instruments[0].data = Bytes(48, 0);
  for (std::size_t i = 0; i < 48; ++i) {
    m.instruments[0].data[i] = static_cast<std::uint8_t>(128 + (i % 8) * 10);  // unsigned 8-bit
  }
  m.instruments[0].name = "unsigned";
  m.instruments[0].c2spd = 22050;
  test::modimp::S3mCell c;
  c.note = 0x40;
  c.inst = 1;
  c.vol = 64;
  m.patterns[0][{0, 0}] = c;
  test::modimp::S3mCell d;
  d.cmd = 1;  // A: speed
  d.info = 5;
  m.patterns[0][{2, 0}] = d;
  return test::modimp::build(m);
}

Bytes med_bytes() { return test::mmd::build(test::mmd::sample_module(1)); }

struct Rig {
  platform::host::FakeStorage card;
  std::unique_ptr<model::ModelArena> arena = std::make_unique<model::ModelArena>();
  std::unique_ptr<ModuleImport> import = std::make_unique<ModuleImport>();
  std::unique_ptr<OgSampleLoader> loader = std::make_unique<OgSampleLoader>();
  std::vector<std::int16_t> pcm = std::vector<std::int16_t>(200000, std::int16_t{-1});
  audio::VoicePool* pool = nullptr;

  Rig() {
    static test::StaticSlot<audio::VoicePool> slot;
    pool = &slot.make();
    arena->project.reset();
  }

  // The project phase to its end in steps of `budget` bytes of the platform's budget; the state it ended in.
  ModuleImport::State run(const std::string& path, ImportEngine engine = ImportEngine::Auto, std::size_t budget = 16384) {
    if (import->begin(card, path.c_str(), *arena, engine) != ModuleImport::Start::Started) {
      return ModuleImport::State::Idle;
    }
    for (unsigned i = 0; i < 2000000 && import->state() == ModuleImport::State::Running; ++i) {
      (void)import->step(budget);
    }
    return import->state();
  }
  // The samples phase, as the platform runs it after the project; false when it could not start.
  bool samples(std::size_t budget = 16384, std::uint32_t arena_samples = 0) {
    if (!loader->begin_feed(*import, arena->project, *pool, pcm.data(), arena_samples != 0u ? arena_samples : static_cast<std::uint32_t>(pcm.size()))) {
      return false;
    }
    for (unsigned i = 0; i < 2000000 && loader->state() == OgSampleLoader::State::Running; ++i) {
      (void)loader->step(budget);
    }
    import->samples_done(loader->report());
    return loader->state() == OgSampleLoader::State::Done;
  }
  // The samples phase with the sample bank the platform gives the loader (OS-BUG-MODENGINE: the engine of each slot reaches the voices at the bind).
  bool samples_with_bank() {
    mem = std::make_unique<app::SampleBankMem>();
    bank = std::make_unique<app::SampleBank>();  // (made here: a bank that was never init()ed cannot be destroyed, close_wav() reaches its memory)
    bank->init(*pool, *arena, *mem, pcm.data(), static_cast<std::uint32_t>(pcm.size()));
    if (!loader->begin_feed(*import, arena->project, *pool, pcm.data(), static_cast<std::uint32_t>(pcm.size()), audio::LevelLaw::Og12, bank.get())) {
      return false;
    }
    for (unsigned i = 0; i < 2000000 && loader->state() == OgSampleLoader::State::Running; ++i) {
      (void)loader->step(16384);
    }
    import->samples_done(loader->report());
    return loader->state() == OgSampleLoader::State::Done;
  }
  std::unique_ptr<app::SampleBankMem> mem;  // (declared before the bank: destroyed after it)
  std::unique_ptr<app::SampleBank> bank;
};

}  // namespace

TEST_CASE("module import: a MOD becomes a project, its sample goes to the arena at the Paula rate, the commands go to the Amiga engine") {
  Rig r;
  r.card.add_file("MOD/kc_test.mod", mod_bytes());
  NEED(r.run("MOD/kc_test.mod") == ModuleImport::State::Done);
  const ModuleSummary& s = r.import->summary();
  CHECK(s.valid);
  CHECK(s.ok);
  CHECK(s.format == ModuleSummary::Format::Mod);
  CHECK(s.channels == 4);
  CHECK(s.amiga_commands);  // AUTO: a MOD plays through the Amiga engine window
  CHECK(s.patterns >= 1);
  CHECK(s.instruments == 1);
  CHECK(std::string(s.name) == "KC_TEST.MOD");
  CHECK(r.arena->project.song_length() >= 1);
  const model::Instrument* in = r.arena->project.instrument(0);
  NEED(in != nullptr);
  CHECK(in->kind == model::InstrumentKind::Sample);

  NEED(r.samples());
  CHECK(r.loader->report().loaded == 1);
  CHECK(r.loader->report().no_room == 0);
  const audio::SampleDesc& d = r.pool->sample(0);
  CHECK(d.length != 0);
  CHECK(d.data == r.pcm.data());
  for (unsigned i = 0; i < 64; ++i) {
    CHECK(r.pcm[i] == static_cast<std::int16_t>(static_cast<int>((i * 3u) & 0x7Fu) * 256));  // 8-bit signed times 256
  }
  CHECK(r.import->summary().samples_loaded == 1);
  CHECK(r.import->summary().samples_kb == 0);  // (64 samples: under half a KB)
}

TEST_CASE("module import: an S3M keeps its own rate and the commands go to the OG / core FX; the F7 choice sends them elsewhere") {
  Rig r;
  r.card.add_file("S3M/test.s3m", s3m_bytes());
  NEED(r.run("S3M/test.s3m") == ModuleImport::State::Done);
  CHECK(r.import->summary().format == ModuleSummary::Format::S3m);
  CHECK_FALSE(r.import->summary().amiga_commands);  // AUTO: an S3M is on the Modern engine
  NEED(r.samples());
  const audio::SampleDesc& d = r.pool->sample(0);
  CHECK(d.length != 0);
  CHECK(r.pcm[0] == 0);  // unsigned 128 is silence
  CHECK(r.pcm[1] == 10 * 256);
  const model::Instrument* in = r.arena->project.instrument(0);
  NEED(in != nullptr);
  CHECK(in->sample_rate == 22050);

  Rig a;
  a.card.add_file("S3M/test.s3m", s3m_bytes());
  NEED(a.run("S3M/test.s3m", ImportEngine::Amiga) == ModuleImport::State::Done);
  CHECK(a.import->summary().amiga_commands);  // AMIGA forces the engine window
  Rig m;
  m.card.add_file("S3M/test.s3m", s3m_bytes());
  NEED(m.run("S3M/test.s3m", ImportEngine::Modern) == ModuleImport::State::Done);
  CHECK_FALSE(m.import->summary().amiga_commands);
}

// OS-BUG-MODENGINE: the engine of an imported instrument is the voices' at once, not after a rebind. A MOD or an OctaMED sample is AUTO at a Paula rate: the Amiga engine. An S3M
// imported for the Modern engine writes MODERN explicitly (QA-FIL-006), so it stays Modern even at a Paula rate (the AUTO rule would send it to Amiga).
TEST_CASE("module import: a MOD plays on the Amiga engine right after the import, an S3M imported for the Modern engine stays Modern (OS-BUG-MODENGINE)") {
  Rig r;
  r.card.add_file("MOD/kc_test.mod", mod_bytes());
  NEED(r.run("MOD/kc_test.mod") == ModuleImport::State::Done);
  NEED(r.samples_with_bank());
  const model::Instrument* in = r.arena->project.instrument(0);
  NEED(in != nullptr);
  CHECK(in->engine == model::kInstEngineAuto);
  CHECK((in->sample_rate == 8287u || in->sample_rate == 8363u));
  CHECK(r.pool->sample(0).length != 0u);
  CHECK(r.pool->sample(0).engine == audio::kEngineAmiga);
  CHECK(r.pool->sample(0).engine == r.bank->engine_for(0, *in));

  // an OctaMED sample is AUTO at the Paula rate too
  Rig q;
  q.card.add_file("MED/song.med", med_bytes());
  NEED(q.run("MED/song.med") == ModuleImport::State::Done);
  NEED(q.samples_with_bank());
  const model::Instrument* mi = q.arena->project.instrument(0);
  NEED(mi != nullptr);
  CHECK(q.pool->sample(0).length != 0u);
  CHECK(q.pool->sample(0).engine == q.bank->engine_for(0, *mi));

  // An S3M sample at 8,363 Hz: imported for the Modern engine (Auto), MODERN is written and it stays Modern. Imported for the Amiga engine it is AUTO, and AUTO at 8,363 Hz is Amiga.
  const auto s3m_at_paula_rate = []() {
    test::modimp::S3mSpec m;
    m.name = "s3m paula";
    m.instruments.resize(1);
    m.instruments[0].data = Bytes(48, 0);
    for (std::size_t i = 0; i < 48; ++i) {
      m.instruments[0].data[i] = static_cast<std::uint8_t>(128 + (i % 8) * 10);
    }
    m.instruments[0].name = "paula";
    m.instruments[0].c2spd = 8363;
    test::modimp::S3mCell c;
    c.note = 0x40;
    c.inst = 1;
    c.vol = 64;
    m.patterns[0][{0, 0}] = c;
    return test::modimp::build(m);
  };
  Rig m;
  m.card.add_file("S3M/paula.s3m", s3m_at_paula_rate());
  NEED(m.run("S3M/paula.s3m") == ModuleImport::State::Done);
  NEED(m.samples_with_bank());
  const model::Instrument* si = m.arena->project.instrument(0);
  NEED(si != nullptr);
  CHECK(si->sample_rate == 8363u);
  CHECK(si->engine == model::kInstEngineModern);
  CHECK(m.pool->sample(0).length != 0u);
  CHECK(m.pool->sample(0).engine == audio::kEngineModern);

  Rig a;
  a.card.add_file("S3M/paula.s3m", s3m_at_paula_rate());
  NEED(a.run("S3M/paula.s3m", ImportEngine::Amiga) == ModuleImport::State::Done);
  NEED(a.samples_with_bank());
  const model::Instrument* ai = a.arena->project.instrument(0);
  NEED(ai != nullptr);
  CHECK(ai->engine == model::kInstEngineAuto);
  CHECK(a.pool->sample(0).engine == audio::kEngineAmiga);
}

// OS-BUG-MODENGINE (review fold): "Modern" in the import dialog is a promise that the song plays on the sampler with the OG / core commands (a MOD's are the tracker FX 46..50, which the Amiga
// voices do not run). The instruments are at the Paula rate, so AUTO would send them to Amiga at the bind: the importers write MODERN when the commands are not the engine window's.
TEST_CASE("module import: a MOD or an OctaMED module imported for the Modern engine plays on the Modern engine, at a Paula rate too (OS-BUG-MODENGINE)") {
  Rig r;
  r.card.add_file("MOD/kc_test.mod", mod_bytes());
  NEED(r.run("MOD/kc_test.mod", ImportEngine::Modern) == ModuleImport::State::Done);
  NEED(r.samples_with_bank());
  const model::Instrument* in = r.arena->project.instrument(0);
  NEED(in != nullptr);
  CHECK((in->sample_rate == 8287u || in->sample_rate == 8363u));
  CHECK(in->engine == model::kInstEngineModern);
  CHECK(r.pool->sample(0).length != 0u);
  CHECK(r.pool->sample(0).engine == audio::kEngineModern);
  CHECK(r.bank->engine_for(0, *in) == audio::kEngineModern);

  Rig q;
  q.card.add_file("MED/song.med", med_bytes());
  NEED(q.run("MED/song.med", ImportEngine::Modern) == ModuleImport::State::Done);
  NEED(q.samples_with_bank());
  const model::Instrument* mi = q.arena->project.instrument(0);
  NEED(mi != nullptr);
  CHECK((mi->sample_rate == 8287u || mi->sample_rate == 8363u));
  CHECK(mi->engine == model::kInstEngineModern);
  CHECK(q.pool->sample(0).length != 0u);
  CHECK(q.pool->sample(0).engine == audio::kEngineModern);

  // for the Amiga engine (the default) both stay AUTO and play on Amiga
  Rig a;
  a.card.add_file("MOD/kc_test.mod", mod_bytes());
  NEED(a.run("MOD/kc_test.mod", ImportEngine::Amiga) == ModuleImport::State::Done);
  NEED(a.samples_with_bank());
  CHECK(a.arena->project.instrument(0)->engine == model::kInstEngineAuto);
  CHECK(a.pool->sample(0).engine == audio::kEngineAmiga);
  Rig b;
  b.card.add_file("MED/song.med", med_bytes());
  NEED(b.run("MED/song.med", ImportEngine::Amiga) == ModuleImport::State::Done);
  NEED(b.samples_with_bank());
  CHECK(b.arena->project.instrument(0)->engine == model::kInstEngineAuto);
  CHECK(b.pool->sample(0).engine == audio::kEngineAmiga);
}

TEST_CASE("module import: the engine option for a MOD and for an OctaMED module") {
  for (const ImportEngine e : {ImportEngine::Auto, ImportEngine::Amiga, ImportEngine::Modern}) {
    Rig r;
    r.card.add_file("MOD/kc_test.mod", mod_bytes());
    NEED(r.run("MOD/kc_test.mod", e) == ModuleImport::State::Done);
    CHECK(r.import->summary().amiga_commands == (e != ImportEngine::Modern));
    Rig q;
    q.card.add_file("MED/song.med", med_bytes());
    NEED(q.run("MED/song.med", e) == ModuleImport::State::Done);
    CHECK(q.import->summary().format == ModuleSummary::Format::Med);
    CHECK(q.import->summary().amiga_commands == (e != ImportEngine::Modern));
  }
}

TEST_CASE("module import: an OctaMED module imports, its samples are fed, and .mmd1 names are taken") {
  Rig r;
  r.card.add_file("MED/song.MMD1", med_bytes());
  NEED(r.run("MED/song.MMD1") == ModuleImport::State::Done);
  const ModuleSummary& s = r.import->summary();
  CHECK(s.ok);
  CHECK(s.format == ModuleSummary::Format::Med);
  CHECK(s.patterns >= 1);
  CHECK(s.instruments >= 1);
  NEED(r.samples());
  CHECK(r.loader->report().loaded >= 1);
  CHECK(r.import->summary().samples_loaded == r.loader->report().loaded);
}

TEST_CASE("module import: the steps' size changes nothing") {
  Rig big;
  big.card.add_file("MOD/kc_test.mod", mod_bytes());
  NEED(big.run("MOD/kc_test.mod", ImportEngine::Auto, 1u << 20) == ModuleImport::State::Done);
  NEED(big.samples(1u << 20));
  Rig tiny;
  tiny.card.add_file("MOD/kc_test.mod", mod_bytes());
  NEED(tiny.run("MOD/kc_test.mod", ImportEngine::Auto, 1) == ModuleImport::State::Done);
  NEED(tiny.samples(1));
  const ModuleSummary& a = big.import->summary();
  const ModuleSummary& b = tiny.import->summary();
  CHECK(a.patterns == b.patterns);
  CHECK(a.order == b.order);
  CHECK(a.cmd_approx == b.cmd_approx);
  CHECK(a.cmd_dropped == b.cmd_dropped);
  CHECK(a.dropped_groups == b.dropped_groups);
  CHECK(a.samples_loaded == b.samples_loaded);
  for (unsigned i = 0; i < 64; ++i) {
    CHECK(big.pcm[i] == tiny.pcm[i]);
  }
}

TEST_CASE("module import: a file cut short fails with its reason and the strip says so") {
  const Bytes whole = mod_bytes();
  Rig r;
  Bytes cut(whole.begin(), whole.begin() + 1500);  // inside the first pattern
  r.card.add_file("MOD/cut.mod", cut);
  CHECK(r.run("MOD/cut.mod") == ModuleImport::State::Failed);
  CHECK_FALSE(r.import->summary().ok);
  CHECK(r.import->summary().valid);
  CHECK(std::string(r.import->summary().error) == "TRUNCATED");
  app::ModuleReportText failed;
  app::module_report_text(r.import->summary(), failed);
  NEED(failed.count >= 2);
  CHECK(std::string(failed.line[1]) == "FAILED    TRUNCATED");
  char line[64];
  app::module_strip_text(r.import->summary(), line, sizeof line);
  CHECK(std::string(line) == "IMPORT CUT.MOD FAILED: TRUNCATED");
  // the file is let go: a second import works and nothing stays open
  r.card.add_file("MOD/kc_test.mod", whole);
  CHECK(r.run("MOD/kc_test.mod") == ModuleImport::State::Done);
  CHECK(r.import->summary().ok);

  Rig s;
  const Bytes s3m = s3m_bytes();
  s.card.add_file("S3M/cut.s3m", Bytes(s3m.begin(), s3m.begin() + 60));
  CHECK(s.run("S3M/cut.s3m") == ModuleImport::State::Failed);
  CHECK_FALSE(std::string(s.import->summary().error).empty());
  Rig q;
  const Bytes med = med_bytes();
  q.card.add_file("MED/cut.med", Bytes(med.begin(), med.begin() + 40));
  CHECK(q.run("MED/cut.med") == ModuleImport::State::Failed);
  CHECK_FALSE(std::string(q.import->summary().error).empty());
}

TEST_CASE("module import: bytes that are no module, a missing file and a wrong extension") {
  Rig r;
  Bytes noise(5000);
  for (std::size_t i = 0; i < noise.size(); ++i) {
    noise[i] = static_cast<std::uint8_t>(i * 131u + 7u);
  }
  r.card.add_file("MOD/noise.mod", noise);
  CHECK(r.run("MOD/noise.mod") == ModuleImport::State::Failed);
  CHECK(std::string(r.import->summary().error) == "UNKNOWN FILE TYPE");

  // a card without the file: the document is not touched
  Rig m;
  (void)m.arena->project.create_pattern(1, 32);
  (void)m.arena->project.append_order(1);
  CHECK(m.run("MOD/absent.mod") == ModuleImport::State::Failed);
  CHECK(std::string(m.import->summary().error) == "FILE NOT FOUND");
  CHECK(m.arena->project.song_length() == 1);

  Rig w;
  w.card.add_file("MOD/readme.txt", Bytes(100, 1));
  CHECK(w.run("MOD/readme.txt") == ModuleImport::State::Idle);  // not a module name: nothing started
  CHECK(w.import->begin(w.card, "MOD/readme.txt", *w.arena, ImportEngine::Auto) == ModuleImport::Start::BadPath);
  CHECK(w.import->begin(w.card, "../x.mod", *w.arena, ImportEngine::Auto) == ModuleImport::Start::BadPath);

  // a refused start keeps the last import's report
  Rig k;
  k.card.add_file("MOD/kc_test.mod", mod_bytes());
  NEED(k.run("MOD/kc_test.mod") == ModuleImport::State::Done);
  CHECK(k.import->begin(k.card, "MOD/readme.txt", *k.arena, ImportEngine::Auto) == ModuleImport::Start::BadPath);
  CHECK(k.import->summary().ok);
  CHECK(std::string(k.import->summary().name) == "KC_TEST.MOD");
}

TEST_CASE("module import: begin() writes nothing to the document; the importer's reset waits for the first step (after the platform's Settle), and a failure from there leaves it empty") {
  for (unsigned kind = 0; kind < 3; ++kind) {
    Rig r;
    const char* path = kind == 0 ? "MOD/kc_test.mod" : (kind == 1 ? "S3M/test.s3m" : "MED/song.med");
    r.card.add_file(path, kind == 0 ? mod_bytes() : (kind == 1 ? s3m_bytes() : med_bytes()));
    model::Project& project = r.arena->project;
    NEED(project.create_pattern(1, 32) == model::Status::Ok);
    NEED(project.append_order(1) == model::Status::Ok);
    const std::uint32_t generation = project.generation();
    NEED(r.import->begin(r.card, path, *r.arena, ImportEngine::Auto) == ModuleImport::Start::Started);
    // This is the whole of what the platform does between begin() and the end of Settle: the render still reads the document.
    CHECK(r.import->state() == ModuleImport::State::Running);
    CHECK_FALSE(r.import->document_touched());
    CHECK(project.generation() == generation);
    CHECK(project.song_length() == 1);
    CHECK(project.pattern(1) != nullptr);
    (void)r.import->step(1);  // the first step: the reader begins, the project is reset
    CHECK(r.import->document_touched());
    CHECK(project.generation() != generation);
    for (unsigned i = 0; i < 2000000 && r.import->state() == ModuleImport::State::Running; ++i) {
      (void)r.import->step(16384);
    }
    CHECK(r.import->state() == ModuleImport::State::Done);
  }

  // A file that cannot be opened: Failed at begin(), the reader never starts, the document is as it was.
  Rig m;
  NEED(m.arena->project.create_pattern(1, 32) == model::Status::Ok);
  NEED(m.arena->project.append_order(1) == model::Status::Ok);
  const std::uint32_t before = m.arena->project.generation();
  CHECK(m.import->begin(m.card, "MOD/absent.mod", *m.arena, ImportEngine::Auto) == ModuleImport::Start::Started);
  CHECK(m.import->state() == ModuleImport::State::Failed);
  CHECK_FALSE(m.import->document_touched());
  CHECK(m.import->step(16384) == ModuleImport::State::Failed);
  CHECK_FALSE(m.import->document_touched());
  CHECK(m.arena->project.generation() == before);
  CHECK(m.arena->project.song_length() == 1);

  // A bad signature is found by the reader, after the reset: the document is empty (and the platform empties the voices' table and the bank).
  Rig n;
  n.card.add_file("MOD/noise.mod", Bytes(5000, 0x5A));
  NEED(n.arena->project.create_pattern(1, 32) == model::Status::Ok);
  NEED(n.arena->project.append_order(1) == model::Status::Ok);
  CHECK(n.run("MOD/noise.mod") == ModuleImport::State::Failed);
  CHECK(n.import->document_touched());
  CHECK(n.arena->project.song_length() == 0);
}

namespace {
// A feed that claims a frame count near 2^32 for every slot (a hostile module): the rounding to an even count must not wrap to 0.
struct HugeFeed final : app::PcmFeed {
  std::uint32_t frames(unsigned) const noexcept override { return 0xFFFFFFFFu; }
  std::uint32_t rate(unsigned) const noexcept override { return 8363; }
  bool read(unsigned, std::uint32_t, std::uint32_t, std::int16_t*) noexcept override { return true; }
};
}  // namespace

TEST_CASE("module import: a feed that claims more frames than the arena holds is no room, never a wrapped count") {
  Rig r;
  r.card.add_file("MOD/kc_test.mod", mod_bytes());
  NEED(r.run("MOD/kc_test.mod") == ModuleImport::State::Done);
  HugeFeed feed;
  NEED(r.loader->begin_feed(feed, r.arena->project, *r.pool, r.pcm.data(), static_cast<std::uint32_t>(r.pcm.size())));
  for (unsigned i = 0; i < 100000 && r.loader->state() == OgSampleLoader::State::Running; ++i) {
    (void)r.loader->step(16384);
  }
  CHECK(r.loader->state() == OgSampleLoader::State::Done);
  CHECK(r.loader->report().loaded == 0);
  CHECK(r.loader->report().no_room == 1);
  CHECK(r.pool->sample(0).length == 0);
}

TEST_CASE("module import: the report names the tracks cut on a line of its own, within the page's width") {
  ModuleSummary s;
  s.valid = true;
  s.ok = true;
  s.format = ModuleSummary::Format::Med;
  s.patterns = 65535;
  s.order = 65535;
  s.tracks_kept = 65535;
  s.tracks_dropped = 12;
  app::ModuleReportText text;
  app::module_report_text(s, text);
  bool cut = false;
  for (unsigned i = 0; i < text.count; ++i) {
    CHECK(std::strlen(text.line[i]) <= app::ModuleReportText::kChars);
    cut = cut || std::string(text.line[i]) == "          12 TRACKS CUT";
  }
  CHECK(cut);
  s.tracks_dropped = 0;
  app::module_report_text(s, text);
  for (unsigned i = 0; i < text.count; ++i) {
    CHECK(std::string(text.line[i]).find("CUT") == std::string::npos);
  }
}

TEST_CASE("module import: an arena without room leaves the instrument silent and the report counts it") {
  Rig r;
  r.card.add_file("MOD/kc_test.mod", mod_bytes());
  NEED(r.run("MOD/kc_test.mod") == ModuleImport::State::Done);
  NEED(r.samples(16384, 32));  // 32 samples of arena for 64 frames
  CHECK(r.loader->report().loaded == 0);
  CHECK(r.loader->report().no_room == 1);
  CHECK(r.pool->sample(0).length == 0);
  const ModuleSummary& s = r.import->summary();
  CHECK(s.samples_no_room == 1);
  char line[64];
  app::module_strip_text(s, line, sizeof line);
  CHECK(std::string(line).find("1 NO ROOM") != std::string::npos);
  CHECK(std::string(line).find("F8 REPORT") != std::string::npos);
}

TEST_CASE("module import: the strip line cuts the name, never the reason, and the report page lists what happened") {
  Rig r;
  r.card.add_file("MOD/a_very_long_module_name_that_goes_on_and_on.mod", mod_bytes());
  NEED(r.run("MOD/a_very_long_module_name_that_goes_on_and_on.mod") == ModuleImport::State::Done);
  NEED(r.samples());
  char line[49];
  app::module_strip_text(r.import->summary(), line, sizeof line);
  CHECK(std::strlen(line) <= 48);
  CHECK(std::string(line).rfind("IMPORTED A_VERY_LONG", 0) == 0);

  app::ModuleReportText text;
  app::module_report_text(r.import->summary(), text);
  NEED(text.count >= 6);
  CHECK(std::string(text.line[0]).rfind("A_VERY_LONG", 0) == 0);
  CHECK(std::string(text.line[1]) == "FORMAT    MOD, 4 CHANNELS");
  CHECK(std::string(text.line[2]) == "COMMANDS  AMIGA ENGINE (FX 128+)");
  CHECK(std::string(text.line[3]).rfind("SONG      ", 0) == 0);
  CHECK(std::string(text.line[4]).rfind("SAMPLES   1 MADE, 1 LOADED", 0) == 0);
  for (unsigned i = 0; i < text.count; ++i) {
    CHECK(std::strlen(text.line[i]) <= app::ModuleReportText::kChars);
  }
  ModuleSummary none;
  app::module_report_text(none, text);
  CHECK(text.count == 1);
  CHECK(std::string(text.line[0]) == "NO IMPORT YET");
}

TEST_CASE("module import: the drop groups are named in the report, upper case") {
  ModuleSummary s;
  s.valid = true;
  s.ok = true;
  s.format = ModuleSummary::Format::Mod;
  s.dropped_groups = 0x1FFFu;  // all thirteen groups of the MOD importer: more lines than the page holds
  app::ModuleReportText text;
  app::module_report_text(s, text);
  CHECK(text.count == app::ModuleReportText::kLines);  // capped, not overrun
  bool header = false;
  for (unsigned i = 0; i < text.count; ++i) {
    header = header || std::string(text.line[i]).rfind("NOT KEPT", 0) == 0;
  }
  CHECK(header);
  CHECK(std::string(s.group_name(8)) == "slide timing");
}

// The maintainer's test set (the SD card's /MOD /S3M /MED; PC copy build-sdtest/O3TEST, git-ignored, never committed): O3_SDTEST_DIR names that folder. Every file goes through the loader and the PCM feed with
// a roomy arena; each either imports (and every instrument the importer made has its samples bound) or says why not. Skipped without the folder.
TEST_CASE("module import: the maintainer's test files import, every sample instrument is bound") {
  const char* env = std::getenv("O3_SDTEST_DIR");
  if (env == nullptr || !std::filesystem::is_directory(env)) {
    MESSAGE("O3_SDTEST_DIR is not set: the maintainer's test set was not run");
    return;
  }
  unsigned imported = 0;
  for (const char* sub : {"MOD", "S3M", "MED"}) {
    const std::filesystem::path dir = std::filesystem::path(env) / sub;
    if (!std::filesystem::is_directory(dir)) {
      continue;
    }
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
      std::ifstream in(e.path(), std::ios::binary);
      const Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      Rig r;
      r.pcm.assign(4u << 20, std::int16_t{0});
      const std::string path = std::string(sub) + "/" + e.path().filename().string();
      r.card.add_file(path, bytes);
      const ModuleImport::State st = r.run(path);
      const ModuleSummary& s = r.import->summary();
      if (st != ModuleImport::State::Done) {
        MESSAGE(path << ": " << s.error << " (" << std::string(s.detail) << ")");
        CHECK_FALSE(std::string(s.error).empty());
        continue;
      }
      NEED(r.samples());
      ++imported;
      MESSAGE(path << ": " << s.patterns << " patterns, " << s.instruments << " instruments, " << r.loader->report().loaded << " loaded, " << r.loader->report().missing << " silent, "
                   << r.loader->report().no_room << " no room, groups " << s.dropped_groups);
      CHECK(r.loader->report().loaded + r.loader->report().missing + r.loader->report().no_room >= s.instruments - s.instruments_refused);
      CHECK(r.loader->report().no_room == 0);
    }
  }
  CHECK(imported > 0);
}
