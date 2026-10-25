// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "support/memory_io.h"
#include "support/modimp_fixture.h"
#include "support/need.h"
#include "support/rng.h"

#include <o3os/formats/builtin.h>
#include <o3os/formats/mod/import.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace o3os;
using namespace o3os::formats::mod;
using o3os::formats::Progress;
using o3os::test::modimp::Bytes;
using o3os::test::modimp::ModCell;
using o3os::test::modimp::ModSpec;
using o3os::test::modimp::S3mCell;
using o3os::test::modimp::S3mInst;
using o3os::test::modimp::S3mSpec;
using o3os::test::modimp::note;
namespace med = o3os::formats::med;

// The MOD and S3M importer (OS-MODIMP, formats/mod/import.h, docs/formats/mod.md): synthetic modules through the job, the project they make (golden steps worked out by hand from the
// rules in map.h and flow.h), the report, the PCM, the three ways to end for lack of room, hostile input, resumability and the registered formats. The libopenmpt comparison of the same
// rules is tests/modimp_openmpt_oracle.py; the reader is modimp_reader_test.cpp.

#if defined(__arm__)
constexpr unsigned kMutants = 200;
constexpr std::uint32_t kThin = 8;
#else
constexpr unsigned kMutants = 1500;
constexpr std::uint32_t kThin = 1;
#endif

namespace {

model::ModelArena& arena() {
  static model::ModelArena a;  // 1.8 MB: not on the stack
  return a;
}

Importer& importer() {
  static Importer i;  // about 26 KB
  return i;
}

const model::Project& project() { return arena().project; }

// One import to its end. The Importer and the arena are shared statics: a Run is valid until the next one.
struct Run {
  Bytes bytes;
  med::SpanSource src;
  Progress end = Progress::Failed;
  explicit Run(const Bytes& b, ImportOptions opt = ImportOptions(), std::size_t budget = 1u << 20) : bytes(b), src(bytes.data(), static_cast<std::uint32_t>(bytes.size())) {
    importer().begin(src, arena(), opt);
    do {
      end = importer().step(budget);
    } while (end == Progress::Running);
  }
  bool ok() const { return end == Progress::Done; }
  const ImportReport& rep() const { return importer().report(); }
  const model::Project& p() const { return project(); }
};

void check_project_state(bool done) {
  const model::Project& p = project();
  CHECK(p.check_invariants());
  if (!done) {
    CHECK(p.pattern_count() == 0);
    CHECK(p.song_length() == 0);
    CHECK(p.instrument_count() == 0);
  }
}

const model::Step& step(unsigned pattern, unsigned slot, unsigned row) {
  static const model::Step none = model::kEmptyStep;
  const model::Pattern* pt = project().pattern(pattern);
  return pt != nullptr && slot < model::kMaxTracks && row < model::kMaxPatternLength ? pt->steps[slot][row] : none;
}

struct StepWant {
  unsigned pat, track, line;
  unsigned note, inst, vol;
  unsigned t1, v1, t2, v2;
};
constexpr unsigned kU = 255;  // an unset volume

// Every non-empty step of the pattern is in `want` and the other way round.
void check_steps(unsigned pat, std::initializer_list<StepWant> want) {
  const model::Pattern* pt = project().pattern(pat);
  NEED(pt != nullptr);
  unsigned have = 0;
  for (unsigned t = 0; t < model::kMaxTracks; ++t)
    for (unsigned l = 0; l < model::kMaxPatternLength; ++l)
      if (!model::is_empty(pt->steps[t][l])) ++have;
  CHECK(have == want.size());
  for (const StepWant& w : want) {
    CAPTURE(w.track);
    CAPTURE(w.line);
    const model::Step& s = pt->steps[w.track][w.line];
    CHECK(s.note == w.note);
    CHECK(s.instr == w.inst);
    CHECK(s.vol == w.vol);
    CHECK(s.fx[0].type == w.t1);
    CHECK(s.fx[0].value == w.v1);
    CHECK(s.fx[1].type == w.t2);
    CHECK(s.fx[1].value == w.v2);
  }
}

// A digest of everything an import makes, for "the same result however it was driven".
std::uint32_t digest(const model::Project& p, const ImportReport& r) {
  std::uint32_t h = 2166136261u;
  auto mix = [&h](std::uint32_t v) {
    for (int i = 0; i < 4; ++i) h = (h ^ ((v >> (8 * i)) & 0xFFu)) * 16777619u;
  };
  auto text = [&](const char* s) {
    while (*s != '\0') mix(static_cast<std::uint8_t>(*s++));
    mix(0x100);
  };
  text(p.name());
  mix(p.tempo_x100());
  mix(p.active_tracks());
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    const model::TrackSlot* ts = p.track(t);
    mix(static_cast<std::uint32_t>(ts->type) | (static_cast<std::uint32_t>(ts->volume) << 8) | (static_cast<std::uint32_t>(ts->pan) << 16) | (static_cast<std::uint32_t>(ts->flags) << 24));
  }
  mix(p.song_length());
  for (unsigned i = 0; i < p.song_length(); ++i) mix(p.order(i));
  for (unsigned id = 1; id <= model::kMaxPatternId; ++id) {
    const model::Pattern* pt = p.pattern(id);
    if (pt == nullptr) continue;
    mix(id);
    text(pt->name);
    for (unsigned t = 0; t < model::kMaxTracks; ++t) {
      mix(pt->track_length[t]);
      for (unsigned l = 0; l < model::kMaxPatternLength; ++l) {
        const model::Step& s = pt->steps[t][l];
        mix(static_cast<std::uint32_t>(s.note) | (static_cast<std::uint32_t>(s.instr) << 8) | (static_cast<std::uint32_t>(s.vol) << 16));
        mix(static_cast<std::uint32_t>(s.fx[0].type) | (static_cast<std::uint32_t>(s.fx[0].value) << 8) | (static_cast<std::uint32_t>(s.fx[1].type) << 16) |
            (static_cast<std::uint32_t>(s.fx[1].value) << 24));
      }
    }
  }
  for (unsigned i = 0; i < model::kMaxInstruments; ++i) {
    const model::Instrument* in = p.instrument(i);
    if (in == nullptr) continue;
    mix(i);
    text(in->name);
    text(in->sample_path);
    mix(in->sample_rate);
    mix(static_cast<std::uint32_t>(in->engine));
    const model::SamplerParams& sp = in->sampler;
    mix(static_cast<std::uint32_t>(sp.volume) | (static_cast<std::uint32_t>(sp.play_mode) << 8) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(sp.tune)) << 16) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(sp.fine_tune)) << 24));
    mix(static_cast<std::uint32_t>(sp.loop_start) | (static_cast<std::uint32_t>(sp.loop_end) << 16));
  }
  mix(r.med.patterns);
  mix(r.med.order);
  mix(r.med.cells);
  mix(r.med.tracks_used);
  mix(r.med.tracks_dropped);
  mix(r.med.tempo_fx);
  mix(r.med.instruments);
  for (unsigned c = 0; c < 256; ++c) {
    mix(r.med.cmd_dropped[c]);
    mix(r.med.cmd_approx[c]);
  }
  for (unsigned i = 0; i < kFxIds; ++i) {
    mix(r.fx[i].seen);
    mix(r.fx[i].approx);
    mix(r.fx[i].dropped);
  }
  mix(r.periods_off_table);
  mix(r.tick0_slide_cells);
  return h;
}

Bytes sine(std::size_t n) { return test::modimp::sine8(n); }

// ---- the songs the tests start from -------------------------------------------------------------------------------------------------------

ModSpec golden_mod() {
  ModSpec m;
  m.name = "golden mod";
  m.samples[0].data = sine(32);
  m.samples[0].name = "sine";
  m.samples[0].loop_len = 16;                  // the whole sample loops
  m.samples[1].data = sine(64);
  m.samples[1].name = "half";
  m.samples[1].volume = 32;
  m.samples[1].finetune = 3;
  m.samples[2].data = sine(8);
  m.samples[2].name = "tiny";
  m.samples[2].volume = 10;
  m.samples[2].finetune = -5;
  m.orders = {0};
  m.patterns = {{
      {{0, 0}, note(12, 1)},                    // C-2 on sample 1
      {{0, 1}, note(0, 2, 0xC, 0x20)},          // C-1 on sample 2, volume 32
      {{1, 0}, note(24, 0, 0xA, 0x0F)},         // C-3, no sample number, a volume slide
      {{2, 1}, ModCell{0, 0, 0x4, 0x35}},       // a vibrato alone
      {{3, 2}, note(35, 3, 0xE, 0x15)},         // B-3, a fine slide up
      {{4, 3}, ModCell{0, 0, 0xE, 0x9F}},       // a retrigger
      {{6, 0}, ModCell{0, 0, 0xE, 0x00}},       // the filter on
  }};
  return m;
}

S3mSpec golden_s3m() {
  S3mSpec m;
  m.name = "golden s3m";
  m.instruments.resize(3);
  m.instruments[0].data = Bytes(32, 0);
  for (std::size_t i = 0; i < 32; ++i) m.instruments[0].data[i] = static_cast<std::uint8_t>(128 + (i % 8) * 10);
  m.instruments[0].name = "unsigned";
  m.instruments[0].loop = true;
  m.instruments[0].loop_start = 4;
  m.instruments[0].loop_end = 20;
  m.instruments[0].c2spd = 22050;
  m.instruments[0].volume = 48;
  m.instruments[1].type = 2;
  m.instruments[1].name = "adlib";
  m.instruments[2].data = Bytes(16, 0);
  m.instruments[2].sixteen = true;
  m.instruments[2].name = "sixteen";
  m.instruments[2].c2spd = 0;
  m.orders = {0, 1};
  auto cell = [](int n, int inst, int vol, int cmd, int info) {
    S3mCell c;
    c.note = n;
    c.inst = inst;
    c.vol = vol;
    c.cmd = cmd;
    c.info = info;
    return c;
  };
  m.patterns.assign(2, {});
  m.patterns[0][{0, 0}] = cell(0x40, 1, 64, 0, 0);
  m.patterns[0][{1, 0}] = cell(0x43, 3, 255, 0, 0);
  m.patterns[0][{2, 1}] = cell(254, 0, 255, 0, 0);
  m.patterns[0][{3, 2}] = cell(255, 0, 255, 7, 0x10);    // G: a tone portamento
  m.patterns[0][{4, 3}] = cell(255, 0, 255, 4, 0x05);    // D: a volume slide
  m.patterns[0][{5, 0}] = cell(255, 0, 255, 20, 0x90);   // T: 144 BPM
  m.patterns[0][{6, 0}] = cell(255, 0, 255, 1, 0x03);    // A: speed 3
  m.patterns[1][{63, 3}] = cell(0x5C, 3, 20, 0, 0);
  return m;
}

}  // namespace

// ---- a whole song, by hand ---------------------------------------------------------------------------------------------------------------

TEST_CASE("modimp import: the golden MOD, step by step (the engine window)") {
  Run r(test::modimp::build(golden_mod()));
  NEED(r.ok());
  check_project_state(true);
  const model::Project& p = r.p();
  CHECK(std::string(p.name()) == "golden mod");
  CHECK(p.tempo_x100() == 12500);
  CHECK(p.active_tracks() == 4);
  CHECK(p.song_length() == 1);
  CHECK(p.order(0) == 1);
  CHECK(p.pattern_count() == 1);
  CHECK(p.pattern_length(1) == 64);
  CHECK(std::string(p.pattern(1)->name) == "PAT 0");
  // tracks: Paula's L R R L, at full volume
  for (unsigned t = 0; t < 4; ++t) {
    CHECK(p.track(t)->type == model::TrackType::Audio);
    CHECK(p.track(t)->volume == 100);
    CHECK(p.track(t)->pan == ((t == 0 || t == 3) ? 0u : 100u));
    CHECK(p.track(t)->flags == 0);
  }
  // 128 + the MED command, the data as it came; a command that is not repeated is ended by E00 00 on the row after it
  check_steps(1, {
      {1, 0, 0, 61, 1, kU, 0, 0, 0, 0},          // C-2 on sample 1: MED's note + 48 is libopenmpt's, 61 is the sampler's unity
      {1, 1, 0, 49, 2, 64, 0, 0, 0, 0},          // C-1: 49; C20 is 32 of 64: 64 of 127
      {1, 0, 1, 73, 0, kU, 138, 15, 0, 0},       // C-3, A0F: MED 0A
      {1, 0, 2, 0, 0, kU, 128, 0, 0, 0},         // ... ended
      {1, 1, 2, 0, 0, kU, 148, 0x35, 0, 0},      // 4xy: MED 14 (ProTracker's vibrato depth)
      {1, 1, 3, 0, 0, kU, 128, 0, 0, 0},
      {1, 2, 3, 84, 3, kU, 145, 5, 0, 0},        // E15: MED 11 (a fine slide up), 5
      {1, 2, 4, 0, 0, kU, 128, 0, 0, 0},
      {1, 3, 4, 0, 0, kU, 159, 15, 0, 0},        // E9F: MED 1F 0F (retrigger every 15 ticks)
      {1, 3, 5, 0, 0, kU, 128, 0, 0, 0},
      {1, 0, 6, 0, 0, kU, 143, 249, 0, 0},       // E00: the LED filter on: MED 0F F9
      {1, 0, 7, 0, 0, kU, 128, 0, 0, 0},
  });
  // instruments: the Paula rate of period 428, volume from the header, finetune in 12.5 cent steps
  CHECK(p.instrument_count() == 3);
  const model::Instrument* i0 = p.instrument(0);
  NEED(i0 != nullptr);
  CHECK(std::string(i0->name) == "sine");
  CHECK(std::string(i0->sample_path) == "samples/instr01.wav");
  CHECK(i0->sample_rate == 8287);
  CHECK(i0->sampler.volume == 50);  // volume 64: level 50, 0 dB (OS-BUG-PASTE24)
  CHECK(i0->sampler.play_mode == 1);
  CHECK(i0->sampler.loop_start == 0);
  CHECK(i0->sampler.loop_end == 65535);
  CHECK(i0->engine == model::kInstEngineAuto);   // the Paula rate sends it to the Amiga engine (app::SampleBank::engine_for)
  const model::Instrument* i1 = p.instrument(1);
  NEED(i1 != nullptr);
  CHECK(i1->sampler.volume == 37);  // volume 32: -6.2 dB
  CHECK(i1->sampler.fine_tune == 38);            // 3 x 12.5 cents
  CHECK(i1->sampler.play_mode == 0);
  const model::Instrument* i2 = p.instrument(2);
  NEED(i2 != nullptr);
  CHECK(i2->sampler.volume == 16);               // 10 of 64
  CHECK(i2->sampler.fine_tune == -63);           // -5 x 12.5 = -62.5, away from zero
  CHECK(p.instrument(3) == nullptr);
  // the report
  const ImportReport& rep = r.rep();
  CHECK(rep.kind == Kind::Mod);
  CHECK(rep.engine_fx);
  CHECK(!rep.structural);
  CHECK(rep.med.tracks_used == 4);
  CHECK(rep.med.tracks_kept == 4);
  CHECK(rep.med.instruments == 3);
  CHECK(rep.med.pcm_bytes == (32 + 64 + 8) * 2u);
  CHECK(rep.med.flow.visits == 64);
  CHECK(rep.med.flow.loop_found);
  CHECK(rep.fx[10].seen == 1);     // A
  CHECK(rep.fx[4].seen == 1);      // 4
  CHECK(rep.fx[12].seen == 1);     // C
  CHECK(rep.fx[17].seen == 1);     // E1
  CHECK(rep.fx[25].seen == 1);     // E9
  CHECK(rep.fx[16].seen == 1);     // E0
  for (unsigned id = 0; id < kFxIds; ++id) {
    CHECK(rep.fx[id].dropped == 0);
    CHECK(rep.fx[id].approx == 0);
  }
  CHECK(rep.tick0_slide_cells == 1);   // the volume slide (ProTracker's slides do not run on tick 0, the engine's do)
  CHECK(rep.periods_off_table == 0);
}

TEST_CASE("modimp import: the golden S3M, step by step (the OG and core FX, native sample rates)") {
  Run r(test::modimp::build(golden_s3m()));
  NEED(r.ok());
  check_project_state(true);
  const model::Project& p = r.p();
  CHECK(std::string(p.name()) == "golden s3m");
  CHECK(p.tempo_x100() == 12500);
  CHECK(p.song_length() == 2);
  CHECK(p.order(0) == 1);
  CHECK(p.order(1) == 2);
  CHECK(!r.rep().engine_fx);
  CHECK(r.rep().kind == Kind::S3m);
  // the tracks: ST3's default panning (left channels 3 of 15, right 12)
  CHECK(p.track(0)->pan == 20);
  CHECK(p.track(1)->pan == 80);
  CHECK(p.track(2)->pan == 20);
  CHECK(p.track(3)->pan == 80);
  // the first pattern: pieces of a song whose tempo changes are keyed by the state they start in
  const model::Step& s0 = step(1, 0, 0);
  CHECK(s0.note == 61);                 // C-4 is byte 0x40: libopenmpt's 61, the sampler's unity
  CHECK(s0.instr == 1);
  CHECK(s0.vol == 127);                 // volume column 64: twice 64, clamped to 127
  const model::Step& s1 = step(1, 0, 1);
  CHECK(s1.note == 64);
  CHECK(s1.instr == 3);
  CHECK(s1.vol == model::kVolumeUnset);
  CHECK(step(1, 1, 2).note == model::kNoteCut);   // ^^ is the note cut
  CHECK(step(1, 2, 3).fx[0].type == 19);          // G: a Glide (the nearest thing, said so)
  const model::Step& d = step(1, 3, 4);           // D05: the volume slide, the tracker FX 47 since OS-COREFX (x up else y down, here 5 down)
  CHECK(d.fx[0].type == 47);
  CHECK(d.fx[0].value == 0x05);
  const model::Step& t = step(1, 0, 5);           // T90: 144 BPM, the Tempo FX is BPM / 2
  CHECK(t.fx[0].type == 15);
  CHECK(t.fx[0].value == 72);
  CHECK(t.fx[1].type == model::kFxNone);
  const model::Step& a = step(1, 0, 6);           // A03: speed 3 at 144 BPM is 288 BPM of rows: Tempo 144, Speed 3
  CHECK(a.fx[0].type == 15);
  CHECK(a.fx[0].value == 144);
  CHECK(a.fx[1].type == 43);
  CHECK(a.fx[1].value == 3);
  // the second pattern: the semitone of 12 is the next octave's (libopenmpt's reading): 0x5C is C-7 = 85 with ... instrument 3, volume 20
  const model::Step& s2 = step(2, 3, 63);
  CHECK(s2.note == 85);
  CHECK(s2.instr == 3);
  CHECK(s2.vol == 40);
  // instruments: the module's own rate, the Modern engine, volumes of 64
  const model::Instrument* i0 = p.instrument(0);
  NEED(i0 != nullptr);
  CHECK(i0->sample_rate == 22050);
  CHECK(i0->engine == model::kInstEngineModern);
  CHECK(i0->sampler.volume == 45);               // 48 of 64: -2.5 dB
  CHECK(i0->sampler.play_mode == 1);
  CHECK(i0->sampler.loop_start == 8192);         // 4 of 32: 8191.9
  CHECK(i0->sampler.loop_end == 40959);          // 20 of 32: 40959.4
  CHECK(p.instrument(1) == nullptr);             // AdLib: refused
  const model::Instrument* i2 = p.instrument(2);
  NEED(i2 != nullptr);
  CHECK(i2->sample_rate == 8363);                // a C2SPD of 0 is 8,363
  CHECK(i2->engine == model::kInstEngineModern);
  const ImportReport& rep = r.rep();
  CHECK(rep.adlib_instruments == 1);
  CHECK(rep.med.instruments == 2);
  CHECK(rep.med.instruments_refused == 1);
  CHECK(rep.rates_defaulted == 1);
  CHECK(rep.fx[39].seen == 1);                   // G
  CHECK(rep.fx[39].approx == 1);
  CHECK(rep.fx[36].seen == 1);                   // D
  CHECK(rep.fx[36].dropped == 0);                // (mapped: the tracker FX 47)
  CHECK(rep.fx[36].approx == 0);
  CHECK(rep.fx[52].seen == 1);                   // T
  CHECK(rep.fx[33].seen == 1);                   // A
  CHECK(rep.fx[52].dropped == 0);
  CHECK(rep.tick0_slide_cells == 0);             // an S3M is not run by the engine whose slides run on tick 0
}

TEST_CASE("modimp import: an S3M can send its effects to the engine window, a MOD can keep the OG and core FX") {
  ImportOptions on;
  on.engine_fx = EngineFx::On;
  Run r(test::modimp::build(golden_s3m()), on);
  NEED(r.ok());
  CHECK(r.rep().engine_fx);
  CHECK(step(1, 3, 4).fx[0].type == 128 + 0x0A);   // D05: the volume slide, 128 + MED 0A
  CHECK(step(1, 3, 4).fx[0].value == 0x05);
  CHECK(step(1, 2, 3).fx[0].type == 128 + 0x03);   // G: the tone portamento
  CHECK(r.p().instrument(0)->engine == model::kInstEngineAuto);   // its commands are the engine window's: not forced to the Modern engine (AUTO at the S3M's rate)
  ImportOptions off;
  off.engine_fx = EngineFx::Off;
  Run m(test::modimp::build(golden_mod()), off);
  NEED(m.ok());
  CHECK(!m.rep().engine_fx);
  CHECK(step(1, 0, 1).fx[0].type == 47);   // the volume slide: the tracker FX 47 (OS-COREFX), a core FX that an OG export writes as none (docs/formats/og.md)
  CHECK(m.rep().fx[10].dropped == 0);
  CHECK(m.rep().tick0_slide_cells == 0);
  CHECK(m.p().instrument(0)->engine == model::kInstEngineModern);   // OS-BUG-MODENGINE: its commands are the core FX 46..50, which only the Modern engine runs; AUTO at its Paula rate would load on Amiga
  CHECK(m.p().instrument(0)->sample_rate == 8287);
}

TEST_CASE("modimp import: on the Modern engine the commands of ProTracker and Scream Tracker that have a tracker FX become FX 46..50 (OS-COREFX), in the trackers' units") {
  ImportOptions off;
  off.engine_fx = EngineFx::Off;
  ModSpec m = golden_mod();
  m.patterns = {{
      {{0, 0}, note(12, 1)},                        // C-2 on sample 1 (volume 64)
      {{1, 0}, ModCell{0, 0, 0x0, 0x37}},           // 0xy: the arpeggio
      {{2, 0}, ModCell{0, 0, 0xA, 0x40}},           // A40: the volume slide, up 4 a tick
      {{3, 0}, ModCell{0, 0, 0xE, 0x13}},           // E13: fine slide up 3 Paula period units = 12 quarter units
      {{4, 0}, ModCell{0, 0, 0xE, 0x25}},           // E25: fine slide down 5 = 20
      {{5, 0}, ModCell{0, 0, 0xE, 0xA4}},           // EA4: fine volume up 4, on a row without a note: the FX 48 (x = 4)
      {{6, 0}, ModCell{0, 0, 0xE, 0xB2}},           // EB2: fine volume down 2 (y = 2)
      {{7, 0}, ModCell{0, 0, 0x0, 0x00}},           // 000: nothing is written
      {{8, 0}, ModCell{0, 0, 0xE, 0x10}},           // E10, EA0, A00: no effect, nothing is written
      {{9, 0}, ModCell{0, 0, 0xE, 0xA0}},
      {{10, 0}, ModCell{0, 0, 0xA, 0x00}},
      // the volume a bare note keeps: sample 3 has volume 10; A30 (+3 on each of the 5 other ticks of a speed-6 row) is 25; the bare note after it is baked at 25 (2 x 25 = 50 in the step)
      {{12, 0}, note(12, 3)},
      {{13, 0}, ModCell{0, 0, 0xA, 0x30}},
      {{14, 0}, note(14, 0)},
      // a note row's fine volume is baked into the step (exact, as before); the row after, with no note, takes the FX
      {{16, 0}, note(12, 1)},
      {{17, 0}, note(14, 0, 0xE, 0xB8)},
  }};
  Run r(test::modimp::build(m), off);
  NEED(r.ok());
  const model::Step& arp = step(1, 0, 1);
  CHECK(arp.fx[0].type == 46);
  CHECK(arp.fx[0].value == 0x37);
  CHECK(step(1, 0, 2).fx[0].type == 47);
  CHECK(step(1, 0, 2).fx[0].value == 0x40);
  CHECK(step(1, 0, 3).fx[0].type == 49);
  CHECK(step(1, 0, 3).fx[0].value == 12);
  CHECK(step(1, 0, 4).fx[0].type == 50);
  CHECK(step(1, 0, 4).fx[0].value == 20);
  CHECK(step(1, 0, 5).fx[0].type == 48);
  CHECK(step(1, 0, 5).fx[0].value == 0x40);
  CHECK(step(1, 0, 6).fx[0].type == 48);
  CHECK(step(1, 0, 6).fx[0].value == 0x02);
  for (unsigned row : {7u, 8u, 9u, 10u}) {
    CHECK_MESSAGE(model::is_empty(step(1, 0, row)), "row " << row);
  }
  CHECK(step(1, 0, 14).vol == 50);                    // baked: the track ended at 25
  CHECK(step(1, 0, 17).fx[0].type == model::kFxNone);  // a note row: the volume 64 - 8 = 56 is baked, 112 in the step
  CHECK(step(1, 0, 17).vol == 112);
  const ImportReport& rep = r.rep();
  CHECK(rep.fx[0].seen == 1);                          // 0xy (000 is no effect at all)
  CHECK(rep.fx[0].dropped == 0);
  CHECK(rep.fx[10].seen == 3);                         // A40, A30 and A00
  CHECK(rep.fx[10].dropped == 0);
  CHECK(rep.fx[17].dropped == 0);
  CHECK(rep.fx[18].dropped == 0);
  CHECK(rep.fx[26].dropped == 0);
  CHECK(rep.fx[27].dropped == 0);
  CHECK(rep.tick0_slide_cells == 0);                   // not the engine window: nothing runs on tick 0 that the trackers do not

  // the engine window is as it was: the commands stay MED's
  Run e(test::modimp::build(m));
  NEED(e.ok());
  CHECK(step(1, 0, 1).fx[0].type == 128 + 0x00);
  CHECK(step(1, 0, 2).fx[0].type == 128 + 0x0A);
  CHECK(step(1, 0, 3).fx[0].type == 128 + 0x11);
  CHECK(step(1, 0, 5).fx[0].type == 128 + 0x1A);
}

TEST_CASE("modimp import: the S3M effects J, D, E and F on the Modern engine, the extra fine slides exact (OS-COREFX)") {
  S3mSpec m = golden_s3m();
  m.orders = {0};
  m.patterns.assign(1, {});
  auto cell = [](int cmd, int info) {
    S3mCell c;
    c.cmd = cmd;
    c.info = info;
    return c;
  };
  S3mCell first;
  first.note = 0x40;
  first.inst = 1;
  first.vol = 64;
  m.patterns[0][{0, 0}] = first;
  m.patterns[0][{1, 0}] = cell(10, 0x4C);   // J4C
  m.patterns[0][{2, 0}] = cell(4, 0x30);    // D30: up 3
  m.patterns[0][{3, 0}] = cell(4, 0x02);    // D02: down 2
  m.patterns[0][{4, 0}] = cell(4, 0x7F);    // D7F: fine up 7
  m.patterns[0][{5, 0}] = cell(4, 0xF2);    // DF2: fine down 2
  m.patterns[0][{6, 0}] = cell(4, 0x00);    // D00: the volume slide's memory since CF2 (DF2 before it: fine down 2)
  m.patterns[0][{7, 0}] = cell(6, 0xF3);    // FF3: fine slide up 3 period units = 12
  m.patterns[0][{8, 0}] = cell(5, 0xF2);    // EF2: fine slide down 8
  m.patterns[0][{9, 0}] = cell(6, 0xE3);    // FE3: extra fine up 3 quarter units: exact
  m.patterns[0][{10, 0}] = cell(5, 0xE1);   // EE1: extra fine down 1
  m.patterns[0][{11, 0}] = cell(6, 0xE0);   // FE0: nothing
  m.patterns[0][{12, 0}] = cell(6, 0x05);   // F05: a slide, as before (the Slide FX)
  Run r(test::modimp::build(m));
  NEED(r.ok());
  CHECK(!r.rep().engine_fx);
  const struct {
    unsigned row, type, value;
  } want[] = {{1, 46, 0x4C}, {2, 47, 0x30}, {3, 47, 0x02}, {4, 48, 0x70}, {5, 48, 0x02}, {7, 49, 12}, {8, 50, 8}, {9, 49, 3}, {10, 50, 1}, {12, 41, 0x05}};
  for (const auto& w : want) {
    CAPTURE(w.row);
    CHECK(step(1, 0, w.row).fx[0].type == w.type);
    CHECK(step(1, 0, w.row).fx[0].value == w.value);
  }
  CHECK(step(1, 0, 6).fx[0].type == 48);
  CHECK(step(1, 0, 6).fx[0].value == 0x02);
  CHECK(step(1, 0, 11).fx[0].type == model::kFxNone);
  const ImportReport& rep = r.rep();
  CHECK(rep.fx[36].dropped == 0);   // D
  CHECK(rep.fx[36].approx == 0);
  CHECK(rep.fx[37].dropped == 0);   // E: the extra fine slides are exact now
  CHECK(rep.fx[37].approx == 0);
  CHECK(rep.fx[38].dropped == 0);   // F
  CHECK(rep.fx[38].approx == 1);    // only F05, the plain slide: the Slide FX's unit is not confirmed (PAR-079), as before
  CHECK(rep.fx[42].dropped == 0);   // J
}

TEST_CASE("modimp import: CF2 on the Modern engine, the S3M P Q I S1x S3x S4x and the MOD E3x E4x E7x become FX 51..56 (OS-COREFX)") {
  S3mSpec m = golden_s3m();
  m.orders = {0};
  m.patterns.assign(1, {});
  auto cell = [](int cmd, int info) {
    S3mCell c;
    c.cmd = cmd;
    c.info = info;
    return c;
  };
  S3mCell first;
  first.note = 0x40;
  first.inst = 1;
  first.vol = 64;
  m.patterns[0][{0, 0}] = first;
  m.patterns[0][{1, 0}] = cell(16, 0x04);   // P04: pan slide right 4
  m.patterns[0][{2, 0}] = cell(16, 0xF3);   // PF3: fine right 3 (kept as the nibbles)
  m.patterns[0][{3, 0}] = cell(17, 0x23);   // Q23: retrigger every 3, volume code 2
  m.patterns[0][{4, 0}] = cell(17, 0x50);   // Q50: never restarts: nothing is written
  m.patterns[0][{5, 0}] = cell(9, 0x21);    // I21: tremor
  m.patterns[0][{6, 0}] = cell(9, 0x00);    // I00 with no memory: a tremor of 1 on 1 off
  m.patterns[0][{7, 0}] = cell(19, 0x11);   // S11: glissando on
  m.patterns[0][{8, 0}] = cell(19, 0x10);   // S10: glissando off
  m.patterns[0][{9, 0}] = cell(19, 0x32);   // S32: vibrato waveform 2 (square)
  m.patterns[0][{10, 0}] = cell(19, 0x47);  // S47: tremolo waveform 7 (random, no retrigger)
  m.patterns[0][{11, 0}] = cell(16, 0x00);  // P00 with memory: the last was PF3
  Run r(test::modimp::build(m));
  NEED(r.ok());
  const struct {
    unsigned row, type, value;
  } want[] = {{1, 51, 0x04}, {2, 51, 0xF3}, {3, 55, 0x23}, {5, 56, 0x21}, {6, 56, 0x21}, {7, 54, 1}, {8, 54, 0}, {9, 52, 2}, {10, 53, 7}, {11, 51, 0xF3}};
  for (const auto& w : want) {
    CAPTURE(w.row);
    CHECK(step(1, 0, w.row).fx[0].type == w.type);
    CHECK(step(1, 0, w.row).fx[0].value == w.value);
  }
  CHECK(step(1, 0, 4).fx[0].type == model::kFxNone);
  const ImportReport& rep = r.rep();
  CHECK(rep.fx[48].dropped == 0);   // P
  CHECK(rep.fx[49].dropped == 0);   // Q
  CHECK(rep.fx[49].approx == 0);    // (exact now: the volume code is carried)
  CHECK(rep.fx[41].dropped == 0);   // I
  CHECK(rep.fx[65].dropped == 0);   // S1x
  CHECK(rep.fx[67].approx == 1);    // S3x: the sine is mapped to the triangle LFO shape
  CHECK(rep.fx[68].approx == 1);    // S4x
  CHECK(rep.fx[67].dropped == 0);

  // the engine window is as it was: P, I, S1x S3x S4x stay dropped, Q is the MED retrigger
  ImportOptions on;
  on.engine_fx = EngineFx::On;
  Run e(test::modimp::build(m), on);
  NEED(e.ok());
  CHECK(step(1, 0, 1).fx[0].type == model::kFxNone);
  CHECK(step(1, 0, 3).fx[0].type == 128 + 0x1F);
  CHECK(e.rep().fx[48].dropped == 3);   // the three P cells (P04, PF3, and P00 as PF3)
}

TEST_CASE("modimp import: CF3 on the Modern engine, the S3M M N V W Y become FX 58..62, N and W resolve their parameter memory, a value above 64 is ignored as libopenmpt does (OS-COREFX)") {
  S3mSpec m = golden_s3m();
  m.orders = {0};
  m.patterns.assign(1, {});
  auto cell = [](int cmd, int info) {
    S3mCell c;
    c.cmd = cmd;
    c.info = info;
    return c;
  };
  S3mCell first;
  first.note = 0x40;
  first.inst = 1;
  first.vol = 64;
  m.patterns[0][{0, 0}] = first;
  m.patterns[0][{1, 0}] = cell(13, 0x20);   // M20: channel volume 32
  m.patterns[0][{2, 0}] = cell(13, 0x00);   // M00: channel volume 0 (an effect)
  m.patterns[0][{3, 0}] = cell(13, 0x41);   // M41: above 64, ignored
  m.patterns[0][{4, 0}] = cell(14, 0x04);   // N04
  m.patterns[0][{5, 0}] = cell(14, 0x00);   // N00: the memory, 04
  m.patterns[0][{6, 0}] = cell(22, 0x40);   // V40: global volume 64
  m.patterns[0][{7, 0}] = cell(23, 0x30);   // W30
  m.patterns[0][{8, 0}] = cell(23, 0x00);   // W00: the memory, 30 (its own: N's 04 is not it)
  m.patterns[0][{9, 0}] = cell(25, 0x3A);   // Y3A: panbrello speed 3 depth 10
  m.patterns[0][{10, 0}] = cell(25, 0x00);  // Y00: kept as it is, the engine has the nibbles' memory
  m.patterns[0][{11, 0}] = cell(14, 0x00);  // N00 on channel 0 after other classes: still 04
  m.patterns[0][{1, 1}] = cell(14, 0x00);   // N00 on channel 1: no memory, no effect
  m.patterns[0][{2, 1}] = cell(23, 0x00);   // W00 on channel 1: likewise
  m.patterns[0][{12, 0}] = cell(22, 0x41);  // V41: above 64, ignored and counted
  Run r(test::modimp::build(m));
  NEED(r.ok());
  const struct {
    unsigned row, type, value;
  } want[] = {{1, 58, 0x20}, {2, 58, 0}, {4, 59, 0x04}, {5, 59, 0x04}, {6, 60, 0x40}, {7, 61, 0x30}, {8, 61, 0x30}, {9, 62, 0x3A}, {10, 62, 0x00}, {11, 59, 0x04}};
  for (const auto& w : want) {
    CAPTURE(w.row);
    CHECK(step(1, 0, w.row).fx[0].type == w.type);
    CHECK(step(1, 0, w.row).fx[0].value == w.value);
  }
  CHECK(step(1, 0, 3).fx[0].type == model::kFxNone);
  CHECK(step(1, 1, 1).fx[0].type == model::kFxNone);
  CHECK(step(1, 1, 2).fx[0].type == model::kFxNone);
  const ImportReport& rep = r.rep();
  CHECK(rep.fx[45].dropped == 1);   // M41 only
  CHECK(rep.fx[46].dropped == 0);   // N
  CHECK(step(1, 0, 12).fx[0].type == model::kFxNone);
  CHECK(rep.fx[54].dropped == 1);   // V41 only
  CHECK(rep.fx[55].dropped == 0);   // W
  CHECK(rep.fx[57].dropped == 0);   // Y

  // the engine window: the Amiga voices have none of them (counted as dropped)
  ImportOptions on;
  on.engine_fx = EngineFx::On;
  Run e(test::modimp::build(m), on);
  NEED(e.ok());
  CHECK(step(1, 0, 1).fx[0].type == model::kFxNone);
  CHECK(step(1, 0, 9).fx[0].type == model::kFxNone);
  CHECK(e.rep().fx[57].dropped == 2);
  CHECK(e.rep().fx[45].dropped == 3);
}

TEST_CASE("modimp import: the S3M header's global volume becomes a V (FX 60) at row 0 of the first pattern played when it is below 64, that row has a free FX slot and the pattern is played once (OS-COREFX CF3)") {
  auto cell = [](int cmd, int info) {
    S3mCell c;
    c.cmd = cmd;
    c.info = info;
    return c;
  };
  S3mCell first;
  first.note = 0x40;
  first.inst = 1;
  first.vol = 64;
  auto build = [&](int gv, std::vector<int> channels, std::vector<int> orders, bool full_row) {
    S3mSpec m = golden_s3m();
    m.global_volume = gv;
    m.channels = channels;
    m.orders = orders;
    m.patterns.assign(2, {});
    m.patterns[0][{0, 0}] = full_row ? cell(1, 0x03) : first;  // A03 at the row makes the Tempo and the Speed FX: both slots
    m.patterns[0][{3, 0}] = first;
    m.patterns[1][{0, 0}] = first;
    return test::modimp::build(m);
  };
  auto v_count = [](unsigned pat) {
    unsigned n = 0;
    for (unsigned t = 0; t < model::kMaxTracks; ++t)
      for (unsigned l = 0; l < model::kMaxPatternLength; ++l)
        for (const model::StepFx& f : step(pat, t, l).fx)
          if (f.type == 60) ++n;
    return n;
  };
  {  // written: the first free slot of the first track, at row 0 of pattern 1 only
    Run r(build(32, {0, 8, 1, 9}, {0, 1}, false));
    NEED(r.ok());
    CHECK(r.rep().global_volume_written);
    CHECK(v_count(1) == 1);
    CHECK(v_count(2) == 0);
    CHECK(step(1, 0, 0).fx[0].type == 60);
    CHECK(step(1, 0, 0).fx[0].value == 32);
    CHECK(step(1, 0, 0).note == 61);  // the note of the row is kept
  }
  {  // the markers of the order list (254) are not a pattern: the first playable order is the pattern after them
    Run r(build(0, {0, 8, 1, 9}, {254, 1, 0}, false));
    NEED(r.ok());
    CHECK(r.rep().global_volume_written);
    CHECK(v_count(1) == 1);  // (the model numbers the patterns in the order they are played; a global volume of 0 is a V of 0)
    CHECK(v_count(2) == 0);
  }
  {  // 64 is nothing to say
    Run r(build(64, {0, 8, 1, 9}, {0, 1}, false));
    NEED(r.ok());
    CHECK(!r.rep().global_volume_written);
    CHECK(v_count(1) == 0);
  }
  {  // no free slot at row 0 (the one channel's A03 holds both): counted as dropped, as before
    Run r(build(32, {0}, {0, 1}, true));
    NEED(r.ok());
    CHECK(step(1, 0, 0).fx[1].type != model::kFxNone);  // (the premise: both slots are in use)
    CHECK(!r.rep().global_volume_written);
    CHECK(v_count(1) == 0);
    CHECK(r.rep().global_volume == 32);
  }
  {  // the first pattern is played twice: dropped
    Run r(build(32, {0, 8, 1, 9}, {0, 1, 0}, false));
    NEED(r.ok());
    CHECK(!r.rep().global_volume_written);
    CHECK(v_count(1) == 0);
  }
  {  // the engine window has no V: dropped
    ImportOptions on;
    on.engine_fx = EngineFx::On;
    Run r(build(32, {0, 8, 1, 9}, {0, 1}, false), on);
    NEED(r.ok());
    CHECK(!r.rep().global_volume_written);
  }
}

TEST_CASE("modimp import: S3M parameter memory, a zero parameter repeats the channel's last non-zero one of its class (D K L, E F, J, Q, R, S, P, I), per channel (OS-COREFX CF2)") {
  S3mSpec m = golden_s3m();
  m.orders = {0};
  m.patterns.assign(1, {});
  auto cell = [](int cmd, int info) {
    S3mCell c;
    c.cmd = cmd;
    c.info = info;
    return c;
  };
  S3mCell first;
  first.note = 0x40;
  first.inst = 1;
  first.vol = 64;
  m.patterns[0][{0, 0}] = first;
  m.patterns[0][{1, 0}] = cell(10, 0x37);   // J37
  m.patterns[0][{2, 0}] = cell(10, 0x00);   // J00: 37
  m.patterns[0][{3, 0}] = cell(4, 0x04);    // D04
  m.patterns[0][{4, 0}] = cell(4, 0x00);    // D00: 04
  m.patterns[0][{5, 0}] = cell(12, 0x00);   // L00: the volume slide's memory too (04); the FX is the tracker volume slide only for D, so check it through the D after
  m.patterns[0][{6, 0}] = cell(11, 0x30);   // K30 sets the volume memory ...
  m.patterns[0][{7, 0}] = cell(4, 0x00);    // ... which D00 uses: 30
  m.patterns[0][{8, 0}] = cell(6, 0xF3);    // FF3
  m.patterns[0][{9, 0}] = cell(5, 0x00);    // E00: the portamento memory is shared with F: FF3 -> fine DOWN 3 (12)
  m.patterns[0][{10, 0}] = cell(17, 0x23);  // Q23
  m.patterns[0][{11, 0}] = cell(17, 0x00);  // Q00: 23
  m.patterns[0][{12, 0}] = cell(10, 0x00);  // J00 after other classes: still 37 (its own memory)
  // a second channel has its own memory
  S3mCell other;
  other.note = 0x40;
  other.inst = 1;
  other.vol = 64;
  m.patterns[0][{0, 1}] = other;
  m.patterns[0][{1, 1}] = cell(10, 0x00);   // J00 on channel 1: no memory, nothing
  Run r(test::modimp::build(m));
  NEED(r.ok());
  CHECK(step(1, 0, 2).fx[0].type == 46);
  CHECK(step(1, 0, 2).fx[0].value == 0x37);
  CHECK(step(1, 0, 4).fx[0].type == 47);
  CHECK(step(1, 0, 4).fx[0].value == 0x04);
  CHECK(step(1, 0, 7).fx[0].type == 47);
  CHECK(step(1, 0, 7).fx[0].value == 0x30);
  CHECK(step(1, 0, 9).fx[0].type == 50);
  CHECK(step(1, 0, 9).fx[0].value == 12);
  CHECK(step(1, 0, 11).fx[0].type == 55);
  CHECK(step(1, 0, 11).fx[0].value == 0x23);
  CHECK(step(1, 0, 12).fx[0].type == 46);
  CHECK(step(1, 0, 12).fx[0].value == 0x37);
  CHECK(step(1, 1, 1).fx[0].type == model::kFxNone);
}

TEST_CASE("modimp import: the NTSC clock is the Paula rate 8,363 Hz") {
  ImportOptions o;
  o.clock = med::Clock::Ntsc;
  Run r(test::modimp::build(golden_mod()), o);
  NEED(r.ok());
  CHECK(r.p().instrument(0)->sample_rate == 8363);
  CHECK(importer().pcm_rate(0) == 8363);
  Run pal(test::modimp::build(golden_mod()));
  NEED(pal.ok());
  CHECK(pal.p().instrument(0)->sample_rate == 8287);
  CHECK(importer().pcm_rate(0) == 8287);
}

TEST_CASE("modimp import: volume: Cxx is 64 steps, above 64 is 64; the S3M volume column is the same") {
  ModSpec m = golden_mod();
  m.patterns = {{
      {{0, 0}, note(12, 1, 0xC, 0x00)},
      {{1, 0}, note(12, 1, 0xC, 0x10)},
      {{2, 0}, note(12, 1, 0xC, 0x3F)},
      {{3, 0}, note(12, 1, 0xC, 0x40)},
      {{4, 0}, note(12, 1, 0xC, 0x41)},
      {{5, 0}, note(12, 1, 0xC, 0xFF)},
  }};
  Run r(test::modimp::build(m));
  NEED(r.ok());
  CHECK(step(1, 0, 0).vol == 0);
  CHECK(step(1, 0, 1).vol == 32);
  CHECK(step(1, 0, 2).vol == 126);
  CHECK(step(1, 0, 3).vol == 127);   // 64 is 128, clamped to the model's 127
  CHECK(step(1, 0, 4).vol == 127);
  CHECK(step(1, 0, 5).vol == 127);
  CHECK(r.rep().med.volume_clamped == 0);   // ProTracker clamps it itself: nothing to say
  S3mSpec s = golden_s3m();
  s.patterns = {{}};
  s.orders = {0};
  int row = 0;
  for (int v : {0, 1, 32, 63, 64}) {
    S3mCell c;
    c.note = 0x40;
    c.inst = 1;
    c.vol = v;
    s.patterns[0][{row++, 0}] = c;
  }
  S3mCell bad;
  bad.vol = 65;
  s.patterns[0][{10, 1}] = bad;
  Run q(test::modimp::build(s));
  NEED(q.ok());
  CHECK(step(1, 0, 0).vol == 0);
  CHECK(step(1, 0, 1).vol == 2);
  CHECK(step(1, 0, 2).vol == 64);
  CHECK(step(1, 0, 3).vol == 126);
  CHECK(step(1, 0, 4).vol == 127);   // 64 is 128, clamped to the model's 127
  CHECK(q.rep().volume_invalid == 1);
  CHECK(step(1, 1, 10).vol == model::kVolumeUnset);
}

TEST_CASE("modimp import: the rows past a note keep the volume the channel had (the instrument's own is not written)") {
  ModSpec m = golden_mod();
  m.patterns = {{
      {{0, 0}, note(12, 1, 0xC, 0x20)},
      {{1, 0}, note(14, 0)},          // a bare note keeps volume 32
      {{2, 0}, note(14, 2)},          // an instrument number: its own volume (32 for sample 2, the same: not written)
      {{3, 0}, note(14, 1)},          // sample 1: 64
      {{4, 0}, note(14, 0)},
  }};
  Run r(test::modimp::build(m));
  NEED(r.ok());
  CHECK(step(1, 0, 0).vol == 64);
  CHECK(step(1, 0, 1).vol == 64);    // baked: the volume the channel kept
  CHECK(step(1, 0, 2).vol == model::kVolumeUnset);
  CHECK(step(1, 0, 3).vol == model::kVolumeUnset);
  CHECK(step(1, 0, 4).vol == model::kVolumeUnset);
  CHECK(r.rep().med.volume_baked == 1);
}

// ---- the effects ------------------------------------------------------------------------------------------------------------------------

namespace {

struct FxCase {
  int effect, param;
  unsigned type, value;   // the engine window: 128 + MED command; 0 type: nothing is written
};

}  // namespace

TEST_CASE("modimp import: every MOD effect, in the engine window and counted") {
  const FxCase cases[] = {
      {0x0, 0x47, 128 + 0x00, 0x47}, {0x1, 0x12, 128 + 0x01, 0x12}, {0x2, 0x34, 128 + 0x02, 0x34}, {0x3, 0x56, 128 + 0x03, 0x56}, {0x4, 0x78, 128 + 0x14, 0x78},
      {0x5, 0x9A, 128 + 0x05, 0x9A}, {0x6, 0xBC, 128 + 0x06, 0xBC}, {0x7, 0xDE, 128 + 0x07, 0xDE}, {0x9, 0x01, 128 + 0x19, 0x01}, {0xA, 0x0F, 128 + 0x0A, 0x0F},
      {0xE, 0x01, 128 + 0x0F, 0xF8}, {0xE, 0x00, 128 + 0x0F, 0xF9}, {0xE, 0x15, 128 + 0x11, 0x05}, {0xE, 0x2A, 128 + 0x12, 0x0A}, {0xE, 0x50, 128 + 0x15, 0x00},
      {0xE, 0x57, 128 + 0x15, 0x07}, {0xE, 0x58, 128 + 0x15, 0xF8}, {0xE, 0x5F, 128 + 0x15, 0xFF}, {0xE, 0x93, 128 + 0x1F, 0x03}, {0xE, 0xA3, 128 + 0x1A, 0x03},
      {0xE, 0xB5, 128 + 0x1B, 0x05}, {0xE, 0xC2, 128 + 0x18, 0x02}, {0xE, 0xD4, 128 + 0x1F, 0x40},
      // 8xx and E8x are Channel pan (57, 0..100: 8xx / 255, E8x x 17 / 255, rounded) since OS-COREFX; the rest has nothing to map to, or nothing to say: no FX
      {0x8, 0x40, 57, 25}, {0x8, 0x80, 57, 50}, {0x8, 0x00, 57, 0}, {0x8, 0xFF, 57, 100}, {0x8, 0x7F, 57, 50}, {0x8, 0xC0, 57, 75}, {0xE, 0x84, 57, 27}, {0xE, 0x88, 57, 53}, {0xE, 0x8F, 57, 100}, {0xE, 0x80, 57, 0},
      {0xE, 0x30, 0, 0}, {0xE, 0x45, 0, 0}, {0xE, 0x75, 0, 0}, {0xE, 0xF7, 0, 0}, {0xE, 0x90, 0, 0}, {0xE, 0xD0, 0, 0}, {0xF, 0x00, 0, 0},
  };
  for (const FxCase& c : cases) {
    CAPTURE(c.effect);
    CAPTURE(c.param);
    ModSpec m = golden_mod();
    m.patterns = {{{{0, 0}, note(12, 1, c.effect, c.param)}}};
    Run r(test::modimp::build(m));
    NEED(r.ok());
    const model::Step& s = step(1, 0, 0);
    CHECK(s.note == 61);
    CHECK(s.fx[0].type == c.type);
    CHECK(s.fx[0].value == c.value);
    CHECK(s.fx[1].type == model::kFxNone);
  }
}

TEST_CASE("modimp import: which effects are counted as dropped, approximated or flow") {
  ModSpec m = golden_mod();
  m.patterns = {{
      {{0, 0}, ModCell{0, 0, 0x8, 0x40}},    // pan: not ProTracker's
      {{1, 0}, ModCell{0, 0, 0xE, 0x30}},    // glissando
      {{2, 0}, ModCell{0, 0, 0xE, 0x45}},    // vibrato waveform
      {{3, 0}, ModCell{0, 0, 0xE, 0x75}},
      {{4, 0}, ModCell{0, 0, 0xE, 0xF7}},    // invert loop
      {{5, 0}, ModCell{0, 0, 0xE, 0x84}},    // pan
      {{6, 0}, ModCell{0, 0, 0xE, 0x84}},
      {{7, 0}, ModCell{0, 0, 0x1, 0x01}},    // a slide
      {{8, 1}, ModCell{0, 0, 0xB, 0x00}},    // a jump: the flow's, not counted as dropped
  }};
  m.orders = {0, 0};
  Run r(test::modimp::build(m));
  NEED(r.ok());
  const ImportReport& rep = r.rep();
  CHECK(rep.fx[8].seen == 1);
  CHECK(rep.fx[8].dropped == 0);   // OS-COREFX: Channel pan, 0..100
  CHECK(rep.fx[8].approx == 0);
  CHECK(rep.fx[19].dropped == 1);
  CHECK(rep.fx[20].dropped == 1);
  CHECK(rep.fx[23].dropped == 1);
  CHECK(rep.fx[31].dropped == 1);
  CHECK(rep.fx[24].seen == 2);
  CHECK(rep.fx[24].dropped == 0);
  CHECK(rep.fx[24].approx == 0);
  CHECK(rep.fx[1].seen == 1);
  CHECK(rep.fx[1].dropped == 0);
  CHECK(rep.fx[11].seen == 1);
  CHECK(rep.fx[11].dropped == 0);
  CHECK(rep.med.flow.jumps >= 1);
  CHECK(rep.med.flow.loop_found);
}

// ---- time ----------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("modimp import: speed and tempo: BPM mode, four rows to a beat") {
  ModSpec m = golden_mod();
  m.patterns = {{
      {{0, 0}, ModCell{0, 0, 0xF, 0x03}},    // speed 3 at 125 BPM: a row is 2.5 x 3 / 125 s: 250 BPM of rows
      {{4, 0}, ModCell{0, 0, 0xF, 0x7D}},    // 125 BPM at speed 3: still 250
      {{8, 1}, ModCell{0, 0, 0xF, 0x06}},    // speed 6, 125 BPM
      {{12, 2}, ModCell{0, 0, 0xF, 0xC8}},   // 200 BPM at speed 6: 200
      {{16, 0}, ModCell{0, 0, 0xF, 0x06}},   // speed 6 again: the same Tempo FX
      {{20, 0}, ModCell{0, 0, 0xF, 0x00}},   // F00 does nothing
      {{24, 0}, ModCell{0, 0, 0xF, 0xFF}},   // 255 BPM: 255 BPM of rows, Tempo 127 + a correction
  }};
  Run r(test::modimp::build(m));
  NEED(r.ok());
  CHECK(r.p().tempo_x100() == 12500);
  const model::Step& a = step(1, 0, 0);
  CHECK(a.fx[0].type == 15);
  CHECK(a.fx[0].value == 125);
  CHECK(a.fx[1].type == 43);
  CHECK(a.fx[1].value == 3);
  const model::Step& b = step(1, 0, 4);
  CHECK(b.fx[0].type == 15);
  CHECK(b.fx[0].value == 125);
  CHECK(b.fx[1].type == model::kFxNone);   // a tempo alone, at the speed in force
  const model::Step& c = step(1, 1, 8);
  CHECK(c.fx[0].type == 15);
  CHECK(c.fx[0].value == 63);              // Tempo = BPM / 2 = 62.5, rounded to 63 ...
  CHECK(c.fx[1].type == 43);               // ... and the Speed takes the second FX column, so the Tempo fine correction (-0.5 BPM) has no room: said so
  CHECK(c.fx[1].value == 6);
  CHECK(r.rep().fx[15].approx >= 1);
  CHECK(step(1, 2, 12).fx[0].type == 15);
  CHECK(step(1, 0, 20).fx[0].type == model::kFxNone);
  const model::Step& e = step(1, 0, 24);
  CHECK(e.fx[0].type == 15);
  CHECK(r.rep().med.speed_fx >= 3);
  CHECK(r.rep().med.tempo_fx >= 6);
  CHECK(r.rep().med.flow.speed_sets == 3);
  CHECK(r.rep().med.flow.tempo_sets == 3);
  CHECK(r.rep().fx[15].seen == 6);         // F00 is not counted: it does nothing
  CHECK(r.rep().fx[15].dropped == 0);
}

TEST_CASE("modimp import: a tempo the model cannot hold is clamped and said so") {
  ModSpec m = golden_mod();
  m.patterns = {{{{0, 0}, ModCell{0, 0, 0xF, 0x20}}, {{4, 0}, ModCell{0, 0, 0xF, 0x1F}}}};
  Run r(test::modimp::build(m));
  NEED(r.ok());
  CHECK(step(1, 0, 0).fx[0].type == 15);
  CHECK(step(1, 0, 0).fx[0].value == 20);   // 32 BPM of rows is under the model's 40: the minimum
  CHECK(r.rep().med.flow.tempo_clamps >= 1);
  S3mSpec s = golden_s3m();
  s.speed = 200;
  s.tempo = 32;
  s.patterns = {{}};
  s.orders = {0};
  Run q(test::modimp::build(s));
  NEED(q.ok());
  CHECK(q.p().tempo_x100() == 4000);
  CHECK(q.rep().initial_tempo_clamped);
}

TEST_CASE("modimp import: S3M Xxx is 0..0x80 (0x40 the centre), Channel pan 0..100") {
  struct PanCase {
    int info;
    unsigned value;
  };
  const PanCase cases[] = {{0x00, 0}, {0x20, 25}, {0x40, 50}, {0x60, 75}, {0x80, 100}, {0xA4, 100}, {0xFF, 100}};  // X20 is a quarter (33 steps, as the MED 2E)
  S3mSpec s = golden_s3m();
  s.patterns = {{}};
  s.orders = {0};
  unsigned row = 0;
  for (const PanCase& c : cases) {
    S3mCell cell;
    cell.cmd = 24;
    cell.info = c.info;
    s.patterns[0][{static_cast<int>(row++), 0}] = cell;
  }
  Run r(test::modimp::build(s));
  NEED(r.ok());
  row = 0;
  for (const PanCase& c : cases) {
    CAPTURE(c.info);
    CHECK(step(1, 0, row).fx[0].type == 57);
    CHECK(step(1, 0, row).fx[0].value == c.value);
    ++row;
  }
}

TEST_CASE("modimp import: a speed above 32 keeps its time and loses its ticks") {
  S3mSpec s = golden_s3m();
  s.patterns = {{}};
  s.orders = {0};
  S3mCell c;
  c.cmd = 1;
  c.info = 0x40;   // A40: speed 64
  s.patterns[0][{0, 0}] = c;
  Run r(test::modimp::build(s));
  NEED(r.ok());
  CHECK(step(1, 0, 0).fx[0].type == 15);
  CHECK(step(1, 0, 0).fx[0].value == 20);   // 6 x 125 / 64 = 11.7 BPM: below the model's 40 (clamped)
  CHECK(step(1, 0, 0).fx[1].type == 43);
  CHECK(step(1, 0, 0).fx[1].value == 32);
  CHECK(r.rep().speed_clamped);
  CHECK(r.rep().fx[33].approx == 1);
}

// ---- the flow --------------------------------------------------------------------------------------------------------------------------

namespace {

ModSpec flow_mod(std::initializer_list<std::pair<std::pair<int, int>, ModCell>> p0, std::vector<int> orders, int patterns = 3) {
  ModSpec m = golden_mod();
  m.orders = std::move(orders);
  m.patterns.assign(static_cast<std::size_t>(patterns), {});
  for (const auto& e : p0) m.patterns[0][e.first] = e.second;
  return m;
}

std::vector<std::pair<unsigned, unsigned>> rows_played() {
  std::vector<std::pair<unsigned, unsigned>> out;
  const med::LinearSong& l = importer().linear();
  for (unsigned i = 0; i < l.order_count; ++i) {
    const med::PatternDef& d = l.patterns[l.order[i]];
    for (unsigned row = d.first; row <= d.last; ++row) out.emplace_back(d.block, row);
  }
  return out;
}

bool plays(const std::vector<std::pair<unsigned, unsigned>>& v, std::size_t at, unsigned pattern, unsigned row) { return at < v.size() && v[at] == std::make_pair(pattern, row); }

}  // namespace

TEST_CASE("modimp import: a pattern break goes to the next position at the row it names, a jump to a position at row 0") {
  Run r(test::modimp::build(flow_mod({{{2, 0}, ModCell{0, 0, 0xD, 0x12}}}, {0, 1}, 2)));
  NEED(r.ok());
  auto v = rows_played();
  CHECK(plays(v, 2, 0, 2));
  CHECK(plays(v, 3, 1, 12));            // D12 is row 12 (decimal), not 18
  CHECK(r.rep().med.flow.breaks == 1);
  Run j(test::modimp::build(flow_mod({{{2, 0}, ModCell{0, 0, 0xB, 0x02}}}, {0, 1, 2})));
  NEED(j.ok());
  v = rows_played();
  CHECK(plays(v, 3, 2, 0));             // position 2 is pattern 2
  CHECK(j.rep().med.flow.jumps == 1);
  // ProTracker: a jump clears the break row of a channel to its left; a break to the right of it sets it
  Run jd(test::modimp::build(flow_mod({{{2, 0}, ModCell{0, 0, 0xD, 0x04}}, {{2, 1}, ModCell{0, 0, 0xB, 0x02}}}, {0, 1, 2})));
  NEED(jd.ok());
  v = rows_played();
  CHECK(plays(v, 3, 2, 0));
  Run dj(test::modimp::build(flow_mod({{{2, 0}, ModCell{0, 0, 0xB, 0x02}}, {{2, 1}, ModCell{0, 0, 0xD, 0x04}}}, {0, 1, 2})));
  NEED(dj.ok());
  v = rows_played();
  CHECK(plays(v, 3, 2, 4));
  // a row past the end of the pattern is row 0 in ProTracker; a position past the order list restarts the song
  Run big(test::modimp::build(flow_mod({{{2, 0}, ModCell{0, 0, 0xD, 0x65}}}, {0, 1}, 2)));
  NEED(big.ok());
  v = rows_played();
  CHECK(plays(v, 3, 1, 0));
  CHECK(big.rep().med.flow.break_line_clamped == 1);
  Run out(test::modimp::build(flow_mod({{{2, 0}, ModCell{0, 0, 0xB, 0x09}}}, {0, 1}, 2)));
  NEED(out.ok());
  v = rows_played();
  CHECK(v.size() == 3);                 // rows 0 to 2, then back to position 0: the song loops
  CHECK(out.rep().med.flow.jump_out_of_range == 1);
  CHECK(out.rep().med.flow.loop_found);
  CHECK(out.rep().med.flow.loop_pos == 0);
}

TEST_CASE("modimp import: the end of the song is a loop the model cannot store, and is said so") {
  Run r(test::modimp::build(golden_mod()));
  NEED(r.ok());
  CHECK(r.rep().med.flow.loop_found);
  CHECK(r.rep().med.flow.loop_order == 0);
  CHECK(r.rep().med.flow.loop_pos == 0);
  CHECK(r.rep().med.flow.loop_line == 0);
  Run b(test::modimp::build(flow_mod({{{4, 0}, ModCell{0, 0, 0xB, 0x00}}}, {0})));
  NEED(b.ok());
  CHECK(b.rep().med.flow.loop_found);
  CHECK(rows_played().size() == 5);       // rows 0 to 4, then back to the start
  CHECK(b.p().pattern_length(1) == 5);
  CHECK(std::string(b.p().pattern(1)->name) == "PAT 0");
}

TEST_CASE("modimp import: pattern loops: the body is played 1 + x times, per channel, and the counters reset with the position") {
  Run r(test::modimp::build(flow_mod({{{2, 0}, ModCell{0, 0, 0xE, 0x60}}, {{5, 0}, ModCell{0, 0, 0xE, 0x62}}}, {0})));
  NEED(r.ok());
  auto v = rows_played();
  // rows 0-5, then 2-5 twice more, then 6..63
  CHECK(plays(v, 5, 0, 5));
  CHECK(plays(v, 6, 0, 2));
  CHECK(plays(v, 9, 0, 5));
  CHECK(plays(v, 10, 0, 2));
  CHECK(plays(v, 13, 0, 5));
  CHECK(plays(v, 14, 0, 6));
  CHECK(r.rep().med.flow.loops == 2);
  CHECK(v.size() == 64 + 8);
  // without a start mark the loop returns to row 0
  Run n(test::modimp::build(flow_mod({{{3, 0}, ModCell{0, 0, 0xE, 0x62}}}, {0})));
  NEED(n.ok());
  v = rows_played();
  CHECK(plays(v, 4, 0, 0));
  CHECK(v.size() == 64 + 8);
  // a loop that jumps back wins over a break on the same row; the break is taken on the last pass
  Run lb(test::modimp::build(flow_mod({{{1, 0}, ModCell{0, 0, 0xE, 0x60}}, {{4, 0}, ModCell{0, 0, 0xE, 0x61}}, {{4, 1}, ModCell{0, 0, 0xD, 0x03}}}, {0, 1}, 2)));
  NEED(lb.ok());
  v = rows_played();
  CHECK(plays(v, 4, 0, 4));
  CHECK(plays(v, 5, 0, 1));
  CHECK(plays(v, 8, 0, 4));
  CHECK(plays(v, 9, 1, 3));
  // two channels loop at once: the one on the right sets where the row goes, both counters run
  Run two(test::modimp::build(flow_mod({{{1, 0}, ModCell{0, 0, 0xE, 0x60}}, {{2, 1}, ModCell{0, 0, 0xE, 0x60}}, {{4, 0}, ModCell{0, 0, 0xE, 0x61}}, {{4, 1}, ModCell{0, 0, 0xE, 0x61}}}, {0})));
  NEED(two.ok());
  v = rows_played();
  CHECK(plays(v, 4, 0, 4));
  CHECK(plays(v, 5, 0, 2));
}

TEST_CASE("modimp import: a pattern delay plays the row again with its notes cleared, the last one on a row wins") {
  ModSpec m = flow_mod({{{4, 0}, note(12, 1, 0xE, 0xE2)}, {{4, 1}, ModCell{0, 0, 0xE, 0xE3}}}, {0});
  Run r(test::modimp::build(m));
  NEED(r.ok());
  CHECK(r.rep().med.flow.repeats == 3);
  CHECK(r.rep().fx[30].seen == 4);          // the two EEx on the row, and again in the replay pattern, which maps the row's commands once more
  // the repeats are one-row patterns flagged as replays, played after the row
  const med::LinearSong& l = importer().linear();
  unsigned replays = 0;
  for (unsigned i = 0; i < l.order_count; ++i) replays += (l.patterns[l.order[i]].flags & med::kPatReplay) != 0 ? 1u : 0u;
  CHECK(replays == 3);
  // the pattern that repeats has the row's command (it keeps them) but no note
  for (unsigned id = 1; id <= r.p().pattern_count(); ++id) {
    if ((l.patterns[id - 1].flags & med::kPatReplay) == 0) continue;
    CHECK(r.p().pattern_length(id) == 1);
    CHECK(step(id, 0, 0).note == model::kNoteEmpty);
    CHECK(step(id, 0, 0).instr == 0);
  }
  CHECK(r.p().song_length() == 1 + 3 + 1);   // rows 0-4, the three repeats, rows 5-63
}

TEST_CASE("modimp import: a tempo or speed in a piece keys the pattern by the state it starts in") {
  // pattern 0 sets the speed; played twice at different speeds, it is two patterns
  ModSpec m = flow_mod({{{4, 0}, ModCell{0, 0, 0xF, 0x03}}}, {0, 1, 0}, 2);
  m.patterns[1][{0, 0}] = ModCell{0, 0, 0xF, 0x08};
  Run r(test::modimp::build(m));
  NEED(r.ok());
  CHECK(r.rep().med.patterns == 3);          // pattern 0 at speed 6, pattern 1, pattern 0 at speed 8
  CHECK(r.p().song_length() == 3);
  CHECK(r.p().order(0) != r.p().order(2));
  // without a speed command the two plays are the same pattern
  Run same(test::modimp::build(flow_mod({{{4, 0}, note(12, 1)}}, {0, 1, 0}, 2)));
  NEED(same.ok());
  CHECK(same.rep().med.patterns == 2);
  CHECK(same.p().order(0) == same.p().order(2));
}

// ---- tracks ------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("modimp import: more than 16 channels keep the busiest 16, in channel order, and say which were dropped") {
  ModSpec m = golden_mod();
  m.tag = "20CH";
  m.channels = 20;
  m.patterns = {{}};
  // channel c has 10 + c notes, except channels 3 and 11 which have only one each
  for (int c = 0; c < 20; ++c) {
    const int n = (c == 3 || c == 11) ? 1 : 10 + c;
    for (int k = 0; k < n; ++k) m.patterns[0][{k, c}] = note(12, 1);
  }
  Run r(test::modimp::build(m));
  NEED(r.ok());
  const ImportReport& rep = r.rep();
  CHECK(rep.med.tracks_used == 20);
  CHECK(rep.med.tracks_kept == 16);
  CHECK(rep.med.tracks_dropped == 4);
  CHECK(rep.med.track[3].slot == med::kNoSlot);
  CHECK(rep.med.track[11].slot == med::kNoSlot);
  CHECK(rep.med.track[0].slot == med::kNoSlot);    // 10 notes: the least busy of the rest, the lower channel first
  CHECK(rep.med.track[1].slot == med::kNoSlot);    // 11 notes
  CHECK(rep.med.track[2].slot == 0);               // 12 notes: the first track kept, in channel order
  CHECK(rep.med.track[19].slot == 15);
  CHECK(rep.med.track[2].cells == 12);
  CHECK(rep.med.track[2].notes == 12);
  CHECK(r.p().active_tracks() == 16);
  CHECK(r.p().track(0)->pan == 100);               // channel 2: right (Paula's L R R L, repeated)
  CHECK(r.p().track(15)->pan == 0);                // channel 19: 19 & 3 = 3: left
}

TEST_CASE("modimp import: the time of a channel that gets no track is carried onto one that has a track") {
  ModSpec m = golden_mod();
  m.tag = "20CH";
  m.channels = 20;
  m.patterns = {{}};
  for (int c = 0; c < 16; ++c)
    for (int k = 0; k < 16; ++k) m.patterns[0][{k, c}] = note(12, 1);
  m.patterns[0][{4, 19}] = ModCell{0, 0, 0xF, 0x50};
  m.patterns[0][{9, 19}] = ModCell{0, 0, 0xF, 0x04};
  Run r(test::modimp::build(m));
  NEED(r.ok());
  CHECK(r.rep().med.tracks_dropped == 1);
  CHECK(r.rep().med.timing_carried >= 3);       // the tempo, the speed's Tempo and its Speed
  CHECK(r.rep().med.timing_lost == 0);
  // the Tempo FX is on a track that has a slot, at the row the command was on
  bool found = false;
  for (unsigned t = 0; t < 16; ++t) found = found || (step(1, t, 4).fx[0].type == 15 && step(1, t, 4).fx[0].value == 40);
  CHECK(found);
}

// ---- instruments -------------------------------------------------------------------------------------------------------------------------

TEST_CASE("modimp import: S3M instruments: the rate, the engine, 16 bit, stereo, loops, what is refused") {
  S3mSpec s;
  s.orders = {0};
  s.instruments.resize(9);
  s.instruments[0].data = Bytes(16, 128);
  s.instruments[0].c2spd = 44100;
  s.instruments[1].data = Bytes(16, 0);
  s.instruments[1].sixteen = true;
  s.instruments[1].stereo = true;
  s.instruments[1].c2spd = 999999999;
  s.instruments[2].data = Bytes(16, 0);
  s.instruments[2].packed = true;
  s.instruments[3].type = 0;
  s.instruments[4].type = 5;
  s.instruments[5].data = Bytes(64, 128);
  s.instruments[5].loop = true;
  s.instruments[5].loop_start = 1;
  s.instruments[5].loop_end = 63;
  s.instruments[6].data = Bytes(64, 128);
  s.instruments[6].volume = 32;
  s.instruments[7].data = Bytes(4, 0);
  s.instruments[7].c2spd = 8363;
  s.instruments[8].data = Bytes(16, 128);
  s.instruments[8].c2spd = 500;  // under the engine's 1,000 Hz floor (OS-RATEPITCH): held to it and counted
  s.patterns = {{}};
  Run r(test::modimp::build(s));
  NEED(r.ok());
  const model::Project& p = r.p();
  CHECK(p.instrument(0)->sample_rate == 44100);
  CHECK(p.instrument(1)->sample_rate == 192000);
  CHECK(p.instrument(8)->sample_rate == 1000);
  CHECK(r.rep().rates_clamped == 2);
  CHECK(r.rep().stereo_samples == 1);
  CHECK(p.instrument(2) == nullptr);
  CHECK(r.rep().packed_samples == 1);
  CHECK(p.instrument(3) == nullptr);
  CHECK(p.instrument(4) == nullptr);
  CHECK(r.rep().adlib_instruments == 1);
  CHECK(r.rep().med.instruments_refused == 2);
  CHECK(p.instrument(5)->sampler.play_mode == 1);
  CHECK(p.instrument(6)->sampler.volume == 37);
  CHECK(std::string(p.instrument(7)->sample_path) == "samples/instr08.wav");
  for (unsigned i : {0u, 1u, 5u, 6u, 7u}) CHECK(p.instrument(i)->engine == model::kInstEngineModern);
  // the PCM: mono 16-bit, the pieces; the rate of each
  CHECK(importer().pcm_frames(1) == 16 / 2);       // 16 bytes of 16-bit data are 8 frames
  CHECK(importer().pcm_rate(0) == 44100);
  CHECK(importer().pcm_rate(1) == 192000);
  std::int16_t pcm[8];
  NEED(importer().read_pcm(1, 0, 8, pcm).ok());
  CHECK(!importer().read_pcm(3, 0, 1, pcm).ok());   // no PCM for an empty instrument
  CHECK(!importer().read_pcm(1, 0, 9, pcm).ok());   // past the end
  CHECK(!importer().read_pcm(0, 0, 17, pcm).ok());
  std::int16_t one[16];
  NEED(importer().read_pcm(0, 0, 16, one).ok());
  for (unsigned i = 0; i < 16; ++i) CHECK(one[i] == 0);   // unsigned 128 is the middle
}

TEST_CASE("modimp import: S3M instrument numbers above 63 have no slot: the cells that name them lose the number, and it is counted") {
  S3mSpec s;
  s.orders = {0};
  s.instruments.resize(70);
  for (auto& i : s.instruments) i.data = Bytes(4, 128);
  s.patterns = {{}};
  S3mCell c;
  c.note = 0x40;
  c.inst = 64;
  s.patterns[0][{0, 0}] = c;
  c.inst = 63;
  s.patterns[0][{1, 0}] = c;
  Run r(test::modimp::build(s));
  NEED(r.ok());
  CHECK(r.rep().instruments_beyond == 7);
  CHECK(r.rep().instrument_refs_dropped == 1);
  CHECK(step(1, 0, 0).instr == 0);
  CHECK(step(1, 0, 0).note == 61);
  CHECK(step(1, 0, 1).instr == 63);
  CHECK(r.p().instrument(62) != nullptr);
  CHECK(r.p().instrument(63) == nullptr);
  CHECK(r.rep().med.instruments == 63);
}

TEST_CASE("modimp import: S3M channels: muted ones are muted tracks, AdLib channels and unused ones are not imported") {
  S3mSpec s = golden_s3m();
  s.channels = {0, 8, 0x80 | 3, 17, 255, 5};
  s.patterns = {{}};
  s.orders = {0};
  for (int ch = 0; ch < 6; ++ch) {
    S3mCell c;
    c.note = 0x40 + ch;
    c.inst = 1;
    s.patterns[0][{ch, ch}] = c;
  }
  Run r(test::modimp::build(s));
  NEED(r.ok());
  CHECK(r.rep().adlib_cells == 1);
  CHECK(r.rep().unused_cells == 1);
  CHECK(r.rep().med.tracks_used == 4);       // channels 0, 1, 2, 5
  CHECK(r.rep().med.track[3].slot == med::kNoSlot);
  CHECK(r.rep().med.track[4].slot == med::kNoSlot);
  CHECK(r.p().track(2)->flags == model::kTrackMute);   // channel 2 is the third track kept, and it is the muted one
  CHECK(r.p().track(0)->flags == 0);
  CHECK(r.p().track(1)->flags == 0);
  CHECK(r.p().track(3)->flags == 0);
  CHECK(step(1, 3, 5).note == 66);                     // channel 5, the fourth track: byte 0x45 is 4 x 12 + 5 + 13
  CHECK(step(1, 2, 2).note == 63);
}

// ---- the three ways to end for lack of room -------------------------------------------------------------------------------------------------

namespace {

// A song of `n` patterns, each played in full 16 times by a loop (E60 on row 0, E6F on row 1): 16 order entries of a one-pattern... a body of
// two rows, 16 passes, so the linearisation lists 16 two-row passes and the rest of the pattern: 17 order entries a pattern, 255 allow 15 patterns.
ModSpec long_song(int patterns) {
  ModSpec m = golden_mod();
  m.orders.clear();
  m.patterns.assign(static_cast<std::size_t>(patterns), {});
  for (int p = 0; p < patterns; ++p) {
    m.orders.push_back(p);
    m.patterns[static_cast<std::size_t>(p)][{0, 0}] = ModCell{0, 0, 0xE, 0x60};
    m.patterns[static_cast<std::size_t>(p)][{1, 0}] = ModCell{0, 0, 0xE, 0x6F};
    m.patterns[static_cast<std::size_t>(p)][{2, 1}] = note(12 + p % 12, 1);
  }
  return m;
}

}  // namespace

TEST_CASE("modimp import: too much to list is refused by default, with what the UI needs to offer the others") {
  Run r(test::modimp::build(long_song(20)));
  CHECK(!r.ok());
  check_project_state(false);
  CHECK(importer().status().error == med::Error::LimitExceeded);
  CHECK(r.rep().overflow);
  CHECK(r.rep().med.flow.visits > 0);
}

TEST_CASE("modimp import: truncate keeps the first order entries that fit and says so") {
  ImportOptions o;
  o.overflow = med::Overflow::Truncate;
  Run r(test::modimp::build(long_song(20)), o);
  NEED(r.ok());
  check_project_state(true);
  CHECK(r.rep().med.flow.truncated);
  CHECK(r.p().song_length() == 255);
  CHECK(r.p().pattern_count() <= 128);
}

TEST_CASE("modimp import: structural keeps the source patterns and the order list and counts what it dropped") {
  ImportOptions o;
  o.overflow = med::Overflow::Structural;
  Run r(test::modimp::build(long_song(20)), o);
  NEED(r.ok());
  check_project_state(true);
  CHECK(r.rep().structural);
  CHECK(r.p().song_length() == 20);
  CHECK(r.p().pattern_count() == 20);
  CHECK(r.p().pattern_length(1) == 64);
  const med::FlowReport& f = r.rep().med.flow;
  CHECK(f.structural);
  CHECK(f.dropped_loops == 40);       // E60 and E6F in each of the 20 patterns
  // a Break and a Jump become Break Pattern and Order jump, approximately: the target row and the position of the order list the fallback cut are not kept
  ModSpec m = long_song(20);
  m.patterns[0][{3, 2}] = ModCell{0, 0, 0xD, 0x10};
  m.patterns[1][{5, 2}] = ModCell{0, 0, 0xB, 0x02};
  Run s(test::modimp::build(m), o);
  NEED(s.ok());
  CHECK(step(1, 2, 3).fx[0].type == 13);
  CHECK(step(1, 2, 3).fx[0].value == 1);
  CHECK(step(2, 2, 5).fx[0].type == 45);
  CHECK(step(2, 2, 5).fx[0].value == 2);
  CHECK(s.rep().fx[13].approx == 1);   // the D10: the row is dropped
  CHECK(s.rep().fx[11].approx == 1);   // the B02: the position counts the fallback's patterns
  CHECK(s.rep().fx[22].dropped == 40);  // the loops: E60 and E6F in each of the 20 patterns
  CHECK(s.rep().fx[11].seen == 1);
}

// ---- resumability and hostile input ---------------------------------------------------------------------------------------------------------

TEST_CASE("modimp import: the result does not depend on the size of the steps") {
  const Bytes files[] = {test::modimp::build(golden_mod()), test::modimp::build(golden_s3m()), test::modimp::build(long_song(5))};
  for (const Bytes& f : files) {
    Run base(f);
    NEED(base.ok());
    const std::uint32_t want = digest(project(), importer().report());
    for (std::size_t budget : {std::size_t{1}, std::size_t{7}, std::size_t{100}, std::size_t{4096}}) {
      Run r(f, ImportOptions(), budget);
      NEED(r.ok());
      CHECK(digest(project(), importer().report()) == want);
    }
  }
}

TEST_CASE("modimp import: step always makes progress and permille only goes up") {
  const Bytes f = test::modimp::build(golden_mod());
  med::SpanSource src(f.data(), static_cast<std::uint32_t>(f.size()));
  importer().begin(src, arena(), ImportOptions());
  unsigned last = 0, steps = 0;
  while (importer().progress() == Progress::Running && steps < 100000) {
    importer().step(1);
    ++steps;
    const unsigned now = importer().permille();
    CHECK(now >= last);
    last = now;
  }
  CHECK(importer().progress() == Progress::Done);
  CHECK(importer().permille() == 1000);
  CHECK(steps < 100000);
}

TEST_CASE("modimp import: a file cut at any offset is refused cleanly or imported, never worse") {
  const Bytes files[] = {test::modimp::build(golden_mod()), test::modimp::build(golden_s3m())};
  for (const Bytes& full : files) {
    unsigned done = 0, failed = 0;
    for (std::size_t len = 0; len <= full.size(); len += (len < 1200 || len + 64 > full.size()) ? 1 : kThin * 5) {
      const Bytes cut(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(len));
      Run r(cut);
      if (r.ok()) {
        ++done;
        check_project_state(true);
      } else {
        ++failed;
        check_project_state(false);
        NEED(importer().status().error != med::Error::None);
      }
      NEED(r.rep().med.patterns <= 128);
    }
    CHECK(failed > 10);
    CHECK(done >= 1);
  }
}

TEST_CASE("modimp import: mutated modules never break the project or the reader") {
  test::Rng rng(0x5EEDu);
  const Bytes bases[] = {test::modimp::build(golden_mod()), test::modimp::build(golden_s3m()), test::modimp::build(long_song(3))};
  for (const Bytes& base : bases) {
    for (unsigned k = 0; k < kMutants; ++k) {
      Bytes m = base;
      const unsigned flips = 1 + rng.below(6);
      for (unsigned i = 0; i < flips; ++i) {
        const std::uint32_t at = rng.below(3) == 0 ? rng.below(static_cast<std::uint32_t>(m.size())) : rng.below(std::min<std::uint32_t>(static_cast<std::uint32_t>(m.size()), 1200u));
        m[at] = rng.byte();
      }
      ImportOptions opt;
      opt.overflow = (k % 3 == 0) ? med::Overflow::Structural : (k % 3 == 1 ? med::Overflow::Truncate : med::Overflow::Refuse);
      opt.engine_fx = (k % 4 == 0) ? EngineFx::On : ((k % 4 == 1) ? EngineFx::Off : EngineFx::Auto);
      Run r(m, opt, 50 + rng.below(4000));
      if (r.ok()) {
        NEED(project().check_invariants());
        NEED(r.rep().med.patterns == project().pattern_count());
        for (unsigned s = 0; s < caps::kSlots; ++s) {
          if (r.rep().med.instrument[s].result == med::InstResult::Sample) {
            std::int16_t pcm[8];
            const std::uint32_t n = r.rep().med.instrument[s].frames < 8 ? r.rep().med.instrument[s].frames : 8u;
            NEED(importer().read_pcm(s, 0, n, pcm).ok());
          }
        }
      } else {
        NEED(project().pattern_count() == 0);
        NEED(project().song_length() == 0);
        NEED(project().instrument_count() == 0);
        NEED(project().check_invariants());
      }
    }
  }
}

TEST_CASE("modimp import: an empty module imports, junk is refused") {
  ModSpec e;
  e.patterns = {{}};
  Run m(test::modimp::build(e));
  CHECK(m.ok());
  check_project_state(true);
  CHECK(m.p().pattern_count() == 1);
  S3mSpec s;
  s.orders = {};
  s.patterns = {};
  Run q(test::modimp::build(s));
  CHECK(q.ok());
  check_project_state(true);
  CHECK(q.p().song_length() == 0);
  CHECK(q.p().pattern_count() == 0);
  Bytes junk(900, 0x55);
  Run j(junk);
  CHECK(!j.ok());
  CHECK(importer().status().error == med::Error::BadMagic);
  check_project_state(false);
  Run none(Bytes{});
  CHECK(!none.ok());
  check_project_state(false);
}

// ---- the registered formats ---------------------------------------------------------------------------------------------------------------------

namespace {

formats::BuiltinFormats& storage() {
  static formats::BuiltinFormats s;
  return s;
}

formats::FormatRegistry& registry() {
  static formats::FormatRegistry r;
  static const bool registered = formats::register_builtin_formats(r, storage());
  CHECK(registered);
  return r;
}

}  // namespace

TEST_CASE("modimp import: registered as \"mod\" and \"s3m\", found by name and by bytes, one reader for both") {
  formats::FormatRegistry& reg = registry();
  const formats::FormatDesc* m = reg.find_id("mod");
  const formats::FormatDesc* s = reg.find_id("s3m");
  NEED(m != nullptr);
  NEED(s != nullptr);
  CHECK(m->kind == formats::FormatKind::Project);
  CHECK(s->kind == formats::FormatKind::Project);
  CHECK(m->reader == &storage().mod_reader);
  CHECK(s->reader == &storage().mod_reader);
  CHECK(m->writer == nullptr);
  CHECK(s->writer == nullptr);
  CHECK(m->group_name != nullptr);
  for (const char* name : {"song.mod", "SONG.MOD", "a.b.Mod", "dir/x.mod", "C:\\m\\x.MOD"}) {
    CAPTURE(name);
    CHECK(reg.find_name(name) == m);
  }
  for (const char* name : {"song.s3m", "SONG.S3M", "dir/x.s3m"}) {
    CAPTURE(name);
    CHECK(reg.find_name(name) == s);
  }
  for (const char* name : {"song.mods", "mod", "song.mod.bak", "._song.mod", "song.xm", "song.it", "song.s3"}) {
    CAPTURE(name);
    CHECK(reg.find_name(name) != m);
    CHECK(reg.find_name(name) != s);
  }
  const Bytes mod = test::modimp::build(golden_mod());
  const Bytes s3m = test::modimp::build(golden_s3m());
  CHECK(reg.sniff(mod.data(), mod.size(), static_cast<std::uint32_t>(mod.size())) == m);
  CHECK(reg.sniff(s3m.data(), s3m.size(), static_cast<std::uint32_t>(s3m.size())) == s);
  CHECK(reg.sniff(mod.data(), 16, 0) != m);          // a head too short to see the signature at 1,080 is not claimed
  CHECK(reg.identify("song.mod", mod.data(), mod.size(), static_cast<std::uint32_t>(mod.size())) == m);
  CHECK(reg.identify("song.mod", s3m.data(), s3m.size(), static_cast<std::uint32_t>(s3m.size())) == nullptr);   // called .mod, is not
  CHECK(reg.identify("song.s3m", s3m.data(), s3m.size(), static_cast<std::uint32_t>(s3m.size())) == s);
  CHECK(reg.identify("untitled", mod.data(), mod.size(), static_cast<std::uint32_t>(mod.size())) == m);
  CHECK(reg.identify("untitled", s3m.data(), s3m.size(), static_cast<std::uint32_t>(s3m.size())) == s);
  CHECK(reg.identify("song.mod", nullptr, 0, 0) == m);    // no bytes offered: the name
  CHECK(std::string(m->group_name(kDropAdLib)) == "AdLib instruments");
  CHECK(std::string(m->group_name(kDropRestart)) == "restart position");
  CHECK(std::string(m->group_name(kDropGroupCount)) == "");
  for (unsigned i = 0; i < kDropGroupCount; ++i) CHECK(std::string(m->group_name(i)) != "");
}

TEST_CASE("modimp import: the strategy: ReadParams::random, the report, the options") {
  formats::FormatRegistry& reg = registry();
  const formats::FormatDesc* d = reg.find_id("mod");
  NEED(d != nullptr);
  formats::FormatReader& reader = *d->reader;
  // without the random-access source it is refused, and the project is left valid and empty
  {
    const Bytes f = test::modimp::build(golden_mod());
    test::MemorySource ms(f);
    reader.begin(ms, arena(), formats::ReadParams{});
    CHECK(reader.progress() == Progress::Failed);
    CHECK(reader.report().error == formats::FormatError::Internal);
    check_project_state(false);
  }
  // with it, a MOD
  {
    const Bytes f = test::modimp::build(golden_mod());
    med::SpanSource src(f.data(), static_cast<std::uint32_t>(f.size()));
    test::MemorySource ms(f);
    formats::ReadParams params;
    params.random = &src;
    reader.begin(ms, arena(), params);
    CHECK(reader.progress() == Progress::Running);
    Progress p = Progress::Running;
    unsigned guard = 0;
    while (p == Progress::Running && guard++ < 100000) {
      p = reader.step(4096);
      CHECK(reader.permille() <= 1000);
    }
    CHECK(p == Progress::Done);
    CHECK(reader.report().error == formats::FormatError::None);
    CHECK(reader.report().bytes == f.size());
    CHECK(reader.report().patterns == 1);
    CHECK(reader.report().instruments == 3);
    // what a song that loops and runs the engine's slides says: the loop point (the model cannot store it), the slide timing
    CHECK(reader.report().dropped_groups == ((1u << kDropLoopPoint) | (1u << kDropSlides)));
    CHECK(reader.report().sanitised == 0u);
    CHECK(project().pattern_count() == 1);
  }
  // an S3M with an AdLib instrument and a dropped effect
  {
    const Bytes f = test::modimp::build(golden_s3m());
    med::SpanSource src(f.data(), static_cast<std::uint32_t>(f.size()));
    test::MemorySource ms(f);
    formats::ReadParams params;
    params.random = &src;
    reader.begin(ms, arena(), params);
    Progress p = Progress::Running;
    unsigned guard = 0;
    while (p == Progress::Running && guard++ < 100000) p = reader.step(4096);
    CHECK(p == Progress::Done);
    const std::uint32_t g = reader.report().dropped_groups;
    CHECK((g & (1u << kDropAdLib)) != 0);
    CHECK((g & (1u << kDropInstruments)) != 0);
    CHECK((g & (1u << kDropEffects)) == 0);      // (D is the tracker FX 47 since OS-COREFX: nothing in this song is without a counterpart)
    CHECK((g & (1u << kDropApprox)) != 0);       // G
    CHECK((g & (1u << kDropLoopPoint)) != 0);
    CHECK((g & (1u << kDropSlides)) == 0);
    CHECK((g & (1u << kDropStereo)) == 0);
    CHECK(reader.report().sanitised == 0u);
  }
  // a file that is not a module
  {
    const Bytes f(2000, 0x33);
    med::SpanSource src(f.data(), static_cast<std::uint32_t>(f.size()));
    test::MemorySource ms(f);
    formats::ReadParams params;
    params.random = &src;
    reader.begin(ms, arena(), params);
    Progress p = Progress::Running;
    unsigned guard = 0;
    while (p == Progress::Running && guard++ < 100000) p = reader.step(4096);
    CHECK(p == Progress::Failed);
    CHECK(reader.report().error == formats::FormatError::BadMagic);
    check_project_state(false);
  }
  // the options of the object: NTSC
  auto* mine = static_cast<ImportReader*>(&reader);
  mine->options().clock = med::Clock::Ntsc;
  {
    const Bytes f = test::modimp::build(golden_mod());
    med::SpanSource src(f.data(), static_cast<std::uint32_t>(f.size()));
    test::MemorySource ms(f);
    formats::ReadParams params;
    params.random = &src;
    reader.begin(ms, arena(), params);
    Progress p = Progress::Running;
    unsigned guard = 0;
    while (p == Progress::Running && guard++ < 100000) p = reader.step(4096);
    CHECK(p == Progress::Done);
    CHECK(project().instrument(0)->sample_rate == 8363);
  }
  mine->options() = ImportOptions();
}
