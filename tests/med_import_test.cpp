// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "support/memory_io.h"
#include "support/mmd_fixture.h"
#include "support/need.h"
#include "support/rng.h"

#include <o3os/formats/builtin.h>
#include <o3os/formats/med/import.h>
#include <o3os/formats/med/map.h>
#include <o3os/formats/med/mmd.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace o3os;
using namespace o3os::formats::med;
using o3os::formats::Progress;
using o3os::test::mmd::BlockSpec;
using o3os::test::mmd::Bytes;
using o3os::test::mmd::InstrSpec;
using o3os::test::mmd::ModuleSpec;

// The importer of slice M2 (OS-008b, docs/formats/med.md section 3): synthetic modules of every version through the job, the
// project they make (golden steps worked out by hand from the rules in map.h and flow.h), the report, the PCM, the three ways
// to end for lack of room, hostile input (every truncation, mutations), resumability (the result does not depend on the step
// budget) and the registered format. The pure arithmetic is med_map_test.cpp.

#if defined(__arm__)
constexpr std::uint32_t kThin = 8;   // the target runs at a few MIPS
constexpr unsigned kMutants = 300;
#else
constexpr std::uint32_t kThin = 1;
constexpr unsigned kMutants = 3000;
#endif

namespace {

model::ModelArena& arena() {
  static model::ModelArena a;  // 1.8 MB: not on the stack
  return a;
}

Importer& importer() {
  static Importer i;  // about 13 KB
  return i;
}

const model::Project& project() { return arena().project; }

// One import to its end. The Importer and the arena are shared statics: a Run is valid until the next one.
struct Run {
  Bytes bytes;
  SpanSource src;
  Progress end = Progress::Failed;
  explicit Run(const Bytes& b, ImportOptions opt = ImportOptions(), std::size_t budget = 1u << 20)
      : bytes(b), src(bytes.data(), static_cast<std::uint32_t>(bytes.size())) {
    importer().begin(src, arena(), opt);
    do {
      end = importer().step(budget);
    } while (end == Progress::Running);
  }
  explicit Run(const ModuleSpec& m, ImportOptions opt = ImportOptions(), std::size_t budget = 1u << 20)
      : Run(test::mmd::build(m), opt, budget) {}
  bool ok() const { return end == Progress::Done; }
  const ImportReport& rep() const { return importer().report(); }
  const model::Project& p() const { return project(); }
};

// A failed import leaves the project valid and empty; a finished one is valid.
void check_project_state(bool done) {
  const model::Project& p = project();
  CHECK(p.check_invariants());
  if (!done) {
    CHECK(p.pattern_count() == 0);
    CHECK(p.song_length() == 0);
    CHECK(p.instrument_count() == 0);
  }
}

BlockSpec block(unsigned tracks, unsigned lines, const char* name = "") {
  BlockSpec b;
  b.tracks = tracks;
  b.lines = lines;
  b.name = name;
  return b;
}

InstrSpec sample8(unsigned frames, unsigned mul = 1) {
  InstrSpec s;
  for (unsigned i = 0; i < frames; ++i) s.data.push_back(static_cast<std::uint8_t>(i * mul));
  return s;
}

ModuleSpec song_of(std::vector<BlockSpec> blocks, std::vector<std::uint8_t> playseq, unsigned version = 1) {
  ModuleSpec m;
  m.version = version;
  m.deftempo = 33;
  m.tempo2 = 6;
  m.blocks = std::move(blocks);
  m.playseq = std::move(playseq);
  if (version >= 2) {
    test::mmd::PlaySeqSpec ps;
    for (std::uint8_t b : m.playseq) ps.entries.push_back(b);
    m.pseqs = {ps};
    m.sections = {0};
    m.playseq.clear();
  }
  return m;
}

struct StepWant {
  unsigned pat, track, line;
  unsigned note, inst, vol;
  unsigned t1, v1, t2, v2;
};

// Every non-empty step of the pattern is in `want` and the other way round.
void check_steps(const model::Project& p, unsigned pat, std::initializer_list<StepWant> want) {
  const model::Pattern* pt = p.pattern(pat);
  NEED(pt != nullptr);
  unsigned have = 0;
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    for (unsigned l = 0; l < model::kMaxPatternLength; ++l) {
      if (!model::is_empty(pt->steps[t][l])) ++have;
    }
  }
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

constexpr unsigned kU = 255;  // an unset volume

// A digest of everything an import makes, for "the same result however it was driven".
std::uint32_t digest(const model::Project& p, const ImportReport& r) {
  std::uint32_t h = 2166136261u;
  auto mix = [&h](std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      h = (h ^ ((v >> (8 * i)) & 0xFFu)) * 16777619u;
    }
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
    mix(static_cast<std::uint32_t>(ts->type) | (static_cast<std::uint32_t>(ts->volume) << 8) | (static_cast<std::uint32_t>(ts->pan) << 16));
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
        mix(static_cast<std::uint32_t>(s.fx[0].type) | (static_cast<std::uint32_t>(s.fx[0].value) << 8) |
            (static_cast<std::uint32_t>(s.fx[1].type) << 16) | (static_cast<std::uint32_t>(s.fx[1].value) << 24));
      }
    }
  }
  for (unsigned i = 0; i < model::kMaxInstruments; ++i) {
    const model::Instrument* in = p.instrument(i);
    if (in == nullptr) continue;
    mix(i);
    mix(static_cast<std::uint32_t>(in->kind));
    text(in->name);
    text(in->sample_path);
    mix(in->sample_rate);
    mix(static_cast<std::uint32_t>(in->midi_channel));
    const model::SamplerParams& sp = in->sampler;
    mix(static_cast<std::uint32_t>(sp.volume) | (static_cast<std::uint32_t>(sp.play_mode) << 8) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(sp.tune)) << 16) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(sp.fine_tune)) << 24));
    mix(static_cast<std::uint32_t>(sp.loop_start) | (static_cast<std::uint32_t>(sp.loop_end) << 16));
  }
  mix(r.patterns);
  mix(r.order);
  mix(r.cells);
  mix(r.tracks_used);
  mix(r.tracks_dropped);
  mix(r.fx_overflow);
  mix(r.tempo_fx);
  mix(r.instruments);
  for (unsigned c = 0; c < 256; ++c) {
    mix(r.cmd_dropped[c]);
    mix(r.cmd_approx[c]);
  }
  for (unsigned i = 0; i < caps::kMaxSamples; ++i) {
    mix(r.instrument[i].flags);
    mix(r.instrument[i].frames);
  }
  return h;
}

// The song every golden test below starts from (MMD1): four tracks, one block of four lines, three instruments.
ModuleSpec golden() {
  ModuleSpec m = song_of({block(4, 4, "intro")}, {0});
  BlockSpec& a = m.blocks[0];
  a.set(0, 0, 13, 1, 0x0C, 0x40);   // C-2 on instrument 1, volume 40 (decimal-coded)
  a.set(0, 1, 25, 2, 0, 0);          // C-3 on the MIDI instrument
  a.set(0, 2, 1, 3, 0x03, 0x10);     // C-1 on instrument 3, a tone portamento
  a.set(1, 0, 15, 0, 0, 0);          // D-2, no instrument number
  a.set(1, 3, 0, 0, 0x15, 0x03);     // a finetune alone
  a.set(2, 0, 0, 0, 0x0F, 0x21);     // tempo 33
  a.set(2, 2, 13, 1, 0x0F, 0xF3);    // C-2, three times per line
  a.set(3, 1, 0x80, 0, 0, 0);        // the exact byte 0x80: a note cut
  a.page_cell(0, 1, 2, 0x01, 0x20);  // a second command on line 1 of track 2: a slide up
  InstrSpec s1 = sample8(64, 4);
  s1.svol = 64;
  s1.name = "one";
  InstrSpec s2;
  s2.midich = 5;
  s2.svol = 32;
  s2.name = "midi";
  InstrSpec s3 = sample8(40, 2);
  s3.strans = -3;
  s3.name = "three";
  m.instruments = {s1, s2, s3};
  m.exp = true;
  m.i_ext_entrsz = 40;
  m.songname = "Golden";
  return m;
}

}  // namespace

// ---- a whole song, by hand ---------------------------------------------------------------------------------------------

TEST_CASE("med import: the golden song, step by step") {
  Run r(golden());
  NEED(r.ok());
  const model::Project& p = r.p();
  CHECK(p.check_invariants());
  CHECK(std::string(p.name()) == "Golden");
  CHECK(p.tempo_x100() == 12338);            // tempo 33, speed 6, PAL: 123.38 BPM
  CHECK(p.active_tracks() == 4);
  CHECK(p.pattern_count() == 1);
  NEED(p.song_length() == 1);
  CHECK(p.order(0) == 1);
  NEED(p.pattern(1) != nullptr);
  CHECK(std::string(p.pattern(1)->name) == "intro");
  CHECK(p.pattern_length(1) == 4);
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    CHECK(p.pattern(1)->track_length[t] == 4);
  }
  // note = MED note + 48; the glide of 03 10 is 14 (271 ms of 20 ms steps), the finetune of 3 is 136, the tempo 33 is Tempo 62 and
  // a half BPM down, the roll 3 is value 3, the page command 01 20 is slide up 32
  check_steps(p, 1, {
      {1, 0, 0, 61, 1, 80, 0, 0, 0, 0},
      {1, 1, 0, 73, 2, kU, 0, 0, 0, 0},
      {1, 2, 0, 49, 3, kU, 19, 14, 0, 0},
      {1, 0, 1, 63, 0, 80, 0, 0, 0, 0},     // a bare note after 0C 40: the track keeps its volume, so the step says 80
      {1, 3, 1, 0, 0, kU, 34, 136, 0, 0},
      {1, 2, 1, 0, 0, kU, 41, 32, 0, 0},
      {1, 0, 2, 0, 0, kU, 15, 62, 44, 126},
      {1, 2, 2, 61, 1, kU, 3, 3, 0, 0},
      {1, 1, 3, model::kNoteCut, 0, kU, 0, 0, 0, 0},
  });
  // the instruments: a sample, a MIDI instrument, a sample tuned by strans
  const model::Instrument* i1 = p.instrument(0);
  NEED(i1 != nullptr);
  CHECK(std::string(i1->name) == "one");
  CHECK(i1->kind == model::InstrumentKind::Sample);
  CHECK(std::string(i1->sample_path) == "samples/instr01.wav");
  CHECK(i1->sample_rate == 8287);
  CHECK(i1->sampler.volume == 50);  // svol 64: level 50, 0 dB (OS-BUG-PASTE24)
  const model::Instrument* i2 = p.instrument(1);
  NEED(i2 != nullptr);
  CHECK(i2->kind == model::InstrumentKind::Midi);
  CHECK(i2->midi_channel == 5);
  const model::Instrument* i3 = p.instrument(2);
  NEED(i3 != nullptr);
  CHECK(i3->sampler.tune == -3);
  CHECK(std::string(i3->sample_path) == "samples/instr03.wav");
  CHECK(p.instrument_count() == 3);
  // tracks: the hardware panning L R R L, volume 64 of 64
  for (unsigned t = 0; t < 4; ++t) {
    CHECK(p.track(t)->volume == 100);
    CHECK(p.track(t)->pan == ((t == 0 || t == 3) ? 0u : 100u));
    CHECK(p.track(t)->type == model::TrackType::Audio);
  }
  // the report
  const ImportReport& rep = r.rep();
  CHECK(rep.version == 1);
  CHECK(rep.patterns == 1);
  CHECK(rep.order == 1);
  CHECK(rep.cells == 32);  // 4 lines of 4 tracks on 2 pages
  CHECK(rep.tracks_used == 4);
  CHECK(rep.tracks_kept == 4);
  CHECK(rep.tracks_dropped == 0);
  CHECK(rep.track[0].cells == 3);
  CHECK(rep.track[0].notes == 2);
  CHECK(rep.track[1].cells == 2);
  CHECK(rep.track[1].notes == 2);   // the cut counts as a note
  CHECK(rep.track[2].cells == 3);
  CHECK(rep.track[2].notes == 2);
  CHECK(rep.track[3].cells == 1);
  CHECK(rep.track[3].notes == 0);
  CHECK(rep.note_cuts == 1);
  CHECK(rep.volume_baked == 1);
  CHECK(rep.timing_carried == 0);
  CHECK(rep.timing_lost == 0);
  CHECK(rep.notes_out_of_range == 0);
  CHECK(rep.fx_overflow == 0);
  CHECK(rep.tempo_fx == 1);
  CHECK(rep.tempo_fx_fine == 1);
  CHECK(rep.cmd_approx[0x03] == 1);
  CHECK(rep.cmd_approx[0x15] == 1);
  CHECK(rep.cmd_approx[0x01] == 1);
  CHECK(rep.cmd_approx[0x0F] == 1);
  unsigned dropped = 0;
  for (unsigned c = 0; c < 256; ++c) dropped += rep.cmd_dropped[c];
  CHECK(dropped == 0);
  CHECK(rep.instruments == 3);
  CHECK(rep.instruments_refused == 0);
  CHECK(rep.pcm_bytes == 2u * (64 + 40));
  CHECK(rep.instrument[1].result == InstResult::Midi);
  CHECK(rep.flow.loop_found);   // the end of the play list returns to the start
  CHECK(rep.flow.runs == 1);
  CHECK(!rep.structural);
  CHECK(!rep.overflow);
}

TEST_CASE("med import: the options: NTSC clock and the engine window") {
  ImportOptions opt;
  opt.clock = Clock::Ntsc;
  {
    Run r(golden(), opt);
    NEED(r.ok());
    CHECK(r.p().tempo_x100() == 12452);          // 124.52 BPM (flow M1: 124.519)
    NEED(r.p().instrument(0) != nullptr);
    CHECK(r.p().instrument(0)->sample_rate == 8363);
    CHECK(importer().pcm_rate() == 8363);
  }
  opt = ImportOptions();
  opt.engine_fx = true;
  Run r(golden(), opt);
  NEED(r.ok());
  // 03 10 -> E03, 15 03 -> E15, 01 20 -> E01, 0F F3 -> E0F; the tempo has no entry and keeps its OG target; after E0F and E15 the row below ends the command (E00 00)
  check_steps(r.p(), 1, {
      {1, 0, 0, 61, 1, 80, 0, 0, 0, 0},
      {1, 1, 0, 73, 2, kU, 0, 0, 0, 0},
      {1, 2, 0, 49, 3, kU, 131, 16, 0, 0},
      {1, 0, 1, 63, 0, 80, 0, 0, 0, 0},
      {1, 3, 1, 0, 0, kU, 149, 3, 0, 0},
      {1, 2, 1, 0, 0, kU, 129, 32, 0, 0},
      {1, 0, 2, 0, 0, kU, 15, 62, 44, 126},
      {1, 2, 2, 61, 1, kU, 143, 243, 0, 0},
      {1, 1, 3, model::kNoteCut, 0, kU, 0, 0, 0, 0},
      {1, 2, 3, 0, 0, kU, 128, 0, 0, 0},  // the line after a command with no note and a free column: E00 00, "no effect" (the engine FX hold until their column changes)
      {1, 3, 2, 0, 0, kU, 128, 0, 0, 0},
  });
  CHECK(r.rep().cmd_approx[0x03] == 0);
  CHECK(r.rep().cmd_approx[0x0F] == 1);   // only the tempo
}

TEST_CASE("med import: every version: the note offset, MMD3 two octaves lower, playtransp") {
  struct Row { unsigned version; int transp; unsigned want; };
  const Row rows[] = {{0, 0, 61}, {1, 0, 61}, {2, 0, 61}, {3, 0, 37}, {1, 5, 66}, {3, -12, 25}};
  for (const Row& row : rows) {
    CAPTURE(row.version);
    ModuleSpec m = song_of({block(4, 2)}, {0}, row.version);
    m.blocks[0].set(0, 0, 13, 1, 0, 0);
    m.playtransp = static_cast<std::int8_t>(row.transp);
    m.instruments = {sample8(8)};
    Run r(m);
    NEED(r.ok());
    check_steps(r.p(), 1, {{1, 0, 0, row.want, 1, kU, 0, 0, 0, 0}});
    CHECK(r.rep().version == row.version);
  }
}

TEST_CASE("med import: volume commands, decimal-coded and 7 bit") {
  ModuleSpec m = song_of({block(2, 4)}, {0});
  m.blocks[0].set(0, 0, 13, 1, 0x0C, 0x32).set(1, 0, 13, 1, 0x0C, 0x64).set(2, 0, 13, 1, 0x0C, 0x65).set(3, 0, 0, 0, 0x0C, 0x00);
  m.instruments = {sample8(8)};
  Run r(m);
  NEED(r.ok());
  check_steps(r.p(), 1, {{1, 0, 0, 61, 1, 64, 0, 0, 0, 0},
                         {1, 0, 1, 61, 1, 127, 0, 0, 0, 0},
                         {1, 0, 2, 61, 1, 127, 0, 0, 0, 0},
                         {1, 0, 3, 0, 0, 0, 0, 0, 0, 0}});
  CHECK(r.rep().volume_clamped == 1);
  m.flags = kFlagVolHex;
  Run h(m);
  NEED(h.ok());
  check_steps(h.p(), 1, {{1, 0, 0, 61, 1, 0x32, 0, 0, 0, 0},
                         {1, 0, 1, 61, 1, 0x64, 0, 0, 0, 0},
                         {1, 0, 2, 61, 1, 0x65, 0, 0, 0, 0},
                         {1, 0, 3, 0, 0, 0, 0, 0, 0, 0}});
}

// ---- tracks, volumes, pans -------------------------------------------------------------------------------------------

TEST_CASE("med import: a block of 20 tracks keeps the busiest 16 and says which were dropped") {
  ModuleSpec m = song_of({block(20, 2)}, {0});
  for (unsigned t = 0; t < 20; ++t) {
    m.blocks[0].set(0, t, 13 + t % 12, 1, 0, 0);
  }
  m.blocks[0].set(1, 18, 20, 1, 0, 0);  // track 18 carries two notes: it is the busiest
  m.instruments = {sample8(8)};
  Run r(m);
  NEED(r.ok());
  const ImportReport& rep = r.rep();
  CHECK(rep.tracks_used == 20);
  CHECK(rep.tracks_kept == 16);
  CHECK(rep.tracks_dropped == 4);
  for (unsigned t = 0; t < 15; ++t) CHECK(rep.track[t].slot == t);
  CHECK(rep.track[15].slot == kNoSlot);   // the equal tracks: the higher numbers lose
  CHECK(rep.track[16].slot == kNoSlot);
  CHECK(rep.track[17].slot == kNoSlot);
  CHECK(rep.track[18].slot == 15);
  CHECK(rep.track[19].slot == kNoSlot);
  CHECK(rep.track[19].cells == 1);        // what was lost is in the report
  CHECK(rep.track[19].notes == 1);
  CHECK(r.p().active_tracks() == 16);
  const model::Pattern* pt = r.p().pattern(1);
  NEED(pt != nullptr);
  CHECK(pt->steps[15][0].note == 13 + 6 + 48);   // track 18: 13 + 18 % 12
  CHECK(pt->steps[15][1].note == 20 + 48);
  CHECK(pt->steps[14][0].note == 13 + 2 + 48);   // track 14
}

TEST_CASE("med import: the commands of a track that got no slot are not counted twice") {
  ModuleSpec m = song_of({block(17, 1)}, {0});
  for (unsigned t = 0; t < 16; ++t) m.blocks[0].set(0, t, 13, 1, 0x01, 0x10);
  m.blocks[0].set(0, 16, 13, 1, 0x02, 0x10);   // the 17th track, equal to the others: the higher number loses
  m.instruments = {sample8(8)};
  Run r(m);
  NEED(r.ok());
  CHECK(r.rep().tracks_dropped == 1);
  CHECK(r.rep().track[16].slot == kNoSlot);
  CHECK(r.rep().track[16].cells == 1);
  CHECK(r.rep().cmd_approx[0x01] == 16);
  CHECK(r.rep().cmd_approx[0x02] == 0);
  CHECK(r.rep().cmd_dropped[0x02] == 0);
}

TEST_CASE("med import: tracks that carry nothing take no slot") {
  ModuleSpec m = song_of({block(12, 2)}, {0});
  m.blocks[0].set(0, 3, 13, 1, 0, 0).set(1, 9, 14, 1, 0, 0).set(1, 4, 0, 0, 0x0C, 0x20);
  m.instruments = {sample8(8)};
  Run r(m);
  NEED(r.ok());
  CHECK(r.rep().tracks_used == 3);
  CHECK(r.rep().track[3].slot == 0);
  CHECK(r.rep().track[4].slot == 1);
  CHECK(r.rep().track[9].slot == 2);
  CHECK(r.p().active_tracks() == 4);      // at least 4
  check_steps(r.p(), 1, {{1, 0, 0, 61, 1, kU, 0, 0, 0, 0}, {1, 2, 1, 62, 1, kU, 0, 0, 0, 0}, {1, 1, 1, 0, 0, 40, 0, 0, 0, 0}});
}

TEST_CASE("med import: track volumes and pans: the hardware's, then the file's") {
  ModuleSpec m = song_of({block(4, 1)}, {0});
  for (unsigned t = 0; t < 4; ++t) m.blocks[0].set(0, t, 13, 1, 0, 0);
  m.instruments = {sample8(8)};
  m.trkvol[1] = 32;
  m.trkvol[2] = 0;
  Run r(m);
  NEED(r.ok());
  CHECK(r.p().track(0)->volume == 100);
  CHECK(r.p().track(1)->volume == 50);
  CHECK(r.p().track(2)->volume == 0);
  CHECK(r.p().track(0)->pan == 0);
  CHECK(r.p().track(1)->pan == 100);
  CHECK(r.p().track(2)->pan == 100);
  CHECK(r.p().track(3)->pan == 0);
  // MMD2: the file's track volumes and pans (-16..16)
  ModuleSpec v2 = song_of({block(4, 1)}, {0}, 2);
  for (unsigned t = 0; t < 4; ++t) v2.blocks[0].set(0, t, 13, 1, 0, 0);
  v2.instruments = {sample8(8)};
  v2.trackvols = {64, 48, 16, 0};
  v2.trackpans = {-16, 0, 8, 16};
  Run r2(v2);
  NEED(r2.ok());
  const unsigned vol[4] = {100, 75, 25, 0};
  const unsigned pan[4] = {0, 50, 75, 100};
  for (unsigned t = 0; t < 4; ++t) {
    CAPTURE(t);
    CHECK(r2.p().track(t)->volume == vol[t]);
    CHECK(r2.p().track(t)->pan == pan[t]);
  }
  // an MMD2 song without pans: Paula's, or the centre for a mixing song
  v2.trackpans.clear();
  Run r3(v2);
  NEED(r3.ok());
  CHECK(r3.p().track(1)->pan == 100);
  v2.flags2 = kFlag2Mix;
  Run r4(v2);
  NEED(r4.ok());
  CHECK(r4.p().track(1)->pan == 50);
}

// ---- blocks into patterns -----------------------------------------------------------------------------------------------

TEST_CASE("med import: a long block is cut at a multiple of the lines per beat, and named") {
  ModuleSpec m = song_of({block(2, 200, "long")}, {0});
  m.blocks[0].set(0, 0, 13, 1, 0, 0).set(150, 1, 14, 1, 0, 0);
  m.instruments = {sample8(8)};
  {
    Run r(m);
    NEED(r.ok());
    CHECK(r.p().pattern_count() == 2);
    NEED(r.p().song_length() == 2);
    CHECK(r.p().order(0) == 1);
    CHECK(r.p().order(1) == 2);
    CHECK(r.p().pattern_length(1) == 128);
    CHECK(r.p().pattern_length(2) == 72);
    CHECK(std::string(r.p().pattern(1)->name) == "long");
    CHECK(std::string(r.p().pattern(2)->name) == "long @128");
    check_steps(r.p(), 1, {{1, 0, 0, 61, 1, kU, 0, 0, 0, 0}});
    check_steps(r.p(), 2, {{2, 1, 22, 62, 1, kU, 0, 0, 0, 0}});   // line 150 - 128
  }
  m.flags2 = kFlag2Bpm | 4;   // five lines to the beat: pieces of 125
  Run r(m);
  NEED(r.ok());
  CHECK(r.p().pattern_length(1) == 125);
  CHECK(r.p().pattern_length(2) == 75);
  CHECK(std::string(r.p().pattern(2)->name) == "long @125");
  check_steps(r.p(), 2, {{2, 1, 25, 62, 1, kU, 0, 0, 0, 0}});
}

TEST_CASE("med import: a long block name is cut to leave room for the piece") {
  const char* name = "abcdefghijklmnopqrstuvwxyz0123456789";   // the reader keeps 30
  ModuleSpec m = song_of({block(1, 130, name)}, {0});
  m.instruments = {sample8(8)};
  m.blocks[0].set(0, 0, 13, 1, 0, 0);
  Run r(m);
  NEED(r.ok());
  CHECK(std::string(r.p().pattern(1)->name) == "abcdefghijklmnopqrstuvwxyz0123");
  CHECK(std::string(r.p().pattern(2)->name) == "abcdefghijklmnopqrstuvwxy @128");
  CHECK(std::string(r.p().pattern(2)->name).size() == 30);
}

TEST_CASE("med import: what the flow executes becomes patterns and an order, repeats keep their commands and lose their notes") {
  // block of 6 lines: line 2 repeats twice (1E 02) with a note and a volume, line 4 breaks (0F 00)
  ModuleSpec m = song_of({block(2, 6)}, {0});
  m.blocks[0].set(0, 0, 13, 1, 0, 0);
  m.blocks[0].set(2, 0, 14, 1, 0x1E, 0x02).set(2, 1, 25, 1, 0x0C, 0x20);
  m.blocks[0].set(3, 0, 15, 1, 0, 0);
  m.blocks[0].set(4, 1, 16, 1, 0x0F, 0x00);
  m.instruments = {sample8(8)};
  Run r(m);
  NEED(r.ok());
  const model::Project& p = r.p();
  CHECK(p.pattern_count() == 3);   // lines 0..2, the repeated line 2, lines 3..4
  NEED(p.song_length() == 4);
  CHECK(p.order(0) == 1);
  CHECK(p.order(1) == 2);
  CHECK(p.order(2) == 2);
  CHECK(p.order(3) == 3);
  CHECK(p.pattern_length(1) == 3);
  CHECK(p.pattern_length(2) == 1);
  CHECK(p.pattern_length(3) == 2);
  check_steps(p, 1, {{1, 0, 0, 61, 1, kU, 0, 0, 0, 0}, {1, 0, 2, 62, 1, kU, 0, 0, 0, 0}, {1, 1, 2, 73, 1, 40, 0, 0, 0, 0}});
  check_steps(p, 2, {{2, 1, 0, 0, 0, 40, 0, 0, 0, 0}});      // the volume stays, the notes and the instruments go
  check_steps(p, 3, {{3, 0, 0, 63, 1, kU, 0, 0, 0, 0}, {3, 1, 1, 64, 1, kU, 0, 0, 0, 0}});
  CHECK(r.rep().flow.repeats == 2);
  CHECK(r.rep().flow.breaks == 1);
  CHECK(r.rep().patterns == 3);
  CHECK(r.rep().order == 4);
}

TEST_CASE("med import: tempo and speed commands follow the line, 18 uses the speed in force") {
  ModuleSpec m = song_of({block(1, 4)}, {0});
  m.blocks[0].set(0, 0, 0, 0, 0x09, 0x03);   // speed 3 at tempo 33: 246.77 BPM, Tempo 123 and Speed 3
  m.blocks[0].set(1, 0, 0, 0, 0x0F, 0x1E);   // tempo 30 at speed 3: 224.33 BPM, Tempo 112 and a quarter-BPM correction
  m.blocks[0].set(2, 0, 0, 0, 0x18, 0x01);   // cut after 1 of 3 ticks: 33
  m.instruments = {sample8(8)};
  Run r(m);
  NEED(r.ok());
  CHECK(r.p().tempo_x100() == 12338);        // the song's own start
  check_steps(r.p(), 1, {{1, 0, 0, 0, 0, kU, 15, 123, 43, 3}, {1, 0, 1, 0, 0, kU, 15, 112, 44, 129}, {1, 0, 2, 0, 0, kU, 20, 33, 0, 0}});
  CHECK(r.rep().speed_fx == 1);
  CHECK(r.rep().tempo_fx == 2);
  CHECK(r.rep().cmd_approx[0x18] == 1);
}

TEST_CASE("med import: MMD2 sections and play sequences become one order") {
  ModuleSpec m = song_of({block(2, 2), block(2, 2)}, {0}, 2);
  m.blocks[0].set(0, 0, 13, 1, 0, 0);
  m.blocks[1].set(0, 1, 14, 1, 0, 0);
  test::mmd::PlaySeqSpec second;
  second.entries = {1};
  m.pseqs.push_back(second);
  m.sections = {0, 1};
  m.instruments = {sample8(8)};
  Run r(m);
  NEED(r.ok());
  CHECK(r.p().pattern_count() == 2);
  NEED(r.p().song_length() == 2);
  CHECK(r.p().order(0) == 1);
  CHECK(r.p().order(1) == 2);
  check_steps(r.p(), 2, {{2, 1, 0, 62, 1, kU, 0, 0, 0, 0}});
}

TEST_CASE("med import: a song with nothing to play still brings its instruments") {
  ModuleSpec m = song_of({block(2, 2)}, {});
  m.instruments = {sample8(8)};
  Run r(m);
  NEED(r.ok());
  CHECK(r.p().pattern_count() == 0);
  CHECK(r.p().song_length() == 0);
  CHECK(r.p().instrument_count() == 1);
  CHECK(r.rep().flow.no_playable);
  CHECK(r.p().active_tracks() == 4);
  CHECK(r.p().check_invariants());
}

// ---- running out of room: refuse, structural, truncate -----------------------------------------------------------------

namespace {

// 130 one-line blocks, each played once: 130 patterns, two more than the model holds.
ModuleSpec too_many_patterns() {
  std::vector<BlockSpec> blocks;
  std::vector<std::uint8_t> seq;
  for (unsigned i = 0; i < 130; ++i) {
    BlockSpec b = block(1, 1);
    b.set(0, 0, 13, 1, 0, 0);
    blocks.push_back(b);
    seq.push_back(static_cast<std::uint8_t>(i));
  }
  ModuleSpec m = song_of(blocks, seq);
  m.instruments = {sample8(8)};
  return m;
}

// A loop of two lines that repeats 255 times: 256 plays of its body, one order entry too many; its commands cannot be kept
ModuleSpec endless_loop() {
  ModuleSpec m = song_of({block(1, 2)}, {0});
  m.blocks[0].set(0, 0, 13, 1, 0x16, 0x00).set(1, 0, 14, 1, 0x16, 0xFF);
  m.instruments = {sample8(8)};
  return m;
}

}  // namespace

TEST_CASE("med import: too many patterns is refused by default, with everything the UI needs to offer the others") {
  Run r(too_many_patterns());
  CHECK(!r.ok());
  CHECK(importer().status().error == Error::LimitExceeded);
  CHECK(r.rep().overflow);
  CHECK(!r.rep().structural);
  CHECK(r.rep().flow.runs == 129);   // how far it got
  check_project_state(false);
}

TEST_CASE("med import: truncate keeps the first order entries that fit and says so") {
  ImportOptions opt;
  opt.overflow = Overflow::Truncate;
  Run r(too_many_patterns(), opt);
  NEED(r.ok());
  CHECK(r.p().pattern_count() == 128);
  CHECK(r.p().song_length() == 128);
  CHECK(r.rep().flow.truncated);
  CHECK(!r.rep().overflow);
  CHECK(r.p().check_invariants());
  for (unsigned i = 0; i < 128; ++i) CHECK(r.p().order(i) == i + 1);
}

TEST_CASE("med import: structural keeps the blocks and the play list when the flow does not fit, and counts what it dropped") {
  {
    Run refused(endless_loop());
    CHECK(!refused.ok());
    CHECK(importer().status().error == Error::LimitExceeded);
    CHECK(refused.rep().overflow);
  }
  ImportOptions opt;
  opt.overflow = Overflow::Structural;
  Run r(endless_loop(), opt);
  NEED(r.ok());
  CHECK(r.rep().structural);
  CHECK(!r.rep().overflow);
  CHECK(r.p().pattern_count() == 1);
  CHECK(r.p().song_length() == 1);
  check_steps(r.p(), 1, {{1, 0, 0, 61, 1, kU, 0, 0, 0, 0}, {1, 0, 1, 62, 1, kU, 0, 0, 0, 0}});
  CHECK(r.rep().cmd_dropped[0x16] == 2);
  CHECK(r.rep().flow.dropped_loops == 2);
  CHECK(r.rep().flow.structural);
  CHECK(r.p().check_invariants());
  // a song that fits is not touched by the option
  Run fits(golden(), opt);
  NEED(fits.ok());
  CHECK(!fits.rep().structural);
}

// ---- time that moves on a track without a slot ------------------------------------------------------------------------

TEST_CASE("med import: tempo and speed of a track without a slot are carried onto one that has") {
  // sixteen busy tracks and a 17th that only moves time: it gets no slot, and its 0F 32 and 09 05 must not vanish
  ModuleSpec m = song_of({block(20, 4)}, {0});
  for (unsigned t = 0; t < 16; ++t) {
    for (unsigned l = 0; l < 4; ++l) m.blocks[0].set(l, t, 13, 1, 0, 0);
  }
  m.blocks[0].set(1, 19, 0, 0, 0x0F, 0x32);   // tempo 50 at speed 6: 186.94 BPM, Tempo 93 and a correction of one BPM
  m.blocks[0].set(2, 19, 0, 0, 0x09, 0x05);   // speed 5 at tempo 50: 224.33 BPM, Tempo 112 and Speed 5
  m.instruments = {sample8(8)};
  Run r(m);
  NEED(r.ok());
  const ImportReport& rep = r.rep();
  CHECK(rep.tracks_used == 17);
  CHECK(rep.tracks_dropped == 1);
  CHECK(rep.track[19].slot == kNoSlot);
  CHECK(rep.track[19].cells == 2);
  CHECK(rep.track[19].notes == 0);
  const model::Pattern* pt = r.p().pattern(1);
  NEED(pt != nullptr);
  const model::Step& a = pt->steps[0][1];
  CHECK(a.note == 61);
  CHECK(a.fx[0].type == 15);
  CHECK(a.fx[0].value == 93);
  CHECK(a.fx[1].type == 44);
  CHECK(a.fx[1].value == 132);
  const model::Step& b = pt->steps[0][2];
  CHECK(b.fx[0].type == 15);
  CHECK(b.fx[0].value == 112);
  CHECK(b.fx[1].type == 43);
  CHECK(b.fx[1].value == 5);
  for (unsigned t = 1; t < 16; ++t) {   // nothing was copied onto the others
    CHECK(pt->steps[t][1].fx[0].type == 0);
    CHECK(pt->steps[t][2].fx[0].type == 0);
  }
  CHECK(rep.timing_carried == 4);
  CHECK(rep.timing_lost == 0);
  CHECK(rep.tempo_fx == 2);
  CHECK(rep.tempo_fx_fine == 1);
  CHECK(rep.speed_fx == 1);
  CHECK(rep.cmd_approx[0x0F] == 1);
  CHECK(rep.cmd_approx[0x09] == 1);
}

TEST_CASE("med import: time that finds no free FX column anywhere is counted as lost") {
  ModuleSpec m = song_of({block(20, 4)}, {0});
  for (unsigned t = 0; t < 16; ++t) {
    for (unsigned l = 0; l < 4; ++l) m.blocks[0].set(l, t, 13, 1, 0, 0);
    m.blocks[0].set(1, t, 13, 1, 0x01, 0x10);      // both FX columns of every kept track taken at line 1
    m.blocks[0].page_cell(0, 1, t, 0x02, 0x10);
  }
  m.blocks[0].set(1, 19, 0, 0, 0x0F, 0x32);        // the 17th track's Tempo and its correction have nowhere to go
  m.instruments = {sample8(8)};
  Run r(m);
  NEED(r.ok());
  CHECK(r.rep().track[19].slot == kNoSlot);
  CHECK(r.rep().timing_carried == 0);
  CHECK(r.rep().timing_lost == 2);
  const model::Pattern* pt = r.p().pattern(1);
  NEED(pt != nullptr);
  for (unsigned t = 0; t < 16; ++t) {
    CHECK(pt->steps[t][1].fx[0].type == 41);
    CHECK(pt->steps[t][1].fx[1].type == 42);
  }
  // the structural fallback: a break and a jump of such a track are carried too
  ModuleSpec e = endless_loop();
  e.blocks.push_back(block(20, 1));
  for (unsigned t = 0; t < 16; ++t) e.blocks[1].set(0, t, 13, 1, 0, 0);
  e.blocks[1].set(0, 19, 0, 0, 0x0B, 0x00);
  e.playseq = {0, 1};
  ImportOptions opt;
  opt.overflow = Overflow::Structural;
  Run s(e, opt);
  NEED(s.ok());
  CHECK(s.rep().structural);
  CHECK(s.rep().track[19].slot == kNoSlot);
  CHECK(s.rep().timing_carried == 1);
  const model::Pattern* sp = s.p().pattern(2);
  NEED(sp != nullptr);
  CHECK(sp->steps[0][0].fx[0].type == 45);
  CHECK(sp->steps[0][0].fx[0].value == 0);
  CHECK(s.rep().cmd_approx[0x0B] == 1);
}

TEST_CASE("med import: structural mode writes a jump of the play sequence as Order jump, approximately") {
  ModuleSpec m = endless_loop();                      // the first block cannot be linearised
  BlockSpec y = block(1, 1);
  y.set(0, 0, 13, 1, 0x0B, 0x00);
  m.blocks.push_back(y);
  m.playseq = {0, 1};
  ImportOptions opt;
  opt.overflow = Overflow::Structural;
  Run r(m, opt);
  NEED(r.ok());
  NEED(r.p().song_length() == 2);
  check_steps(r.p(), 2, {{2, 0, 0, 61, 1, kU, 45, 0, 0, 0}});
  CHECK(r.rep().cmd_approx[0x0B] == 1);
  CHECK(r.rep().cmd_dropped[0x0B] == 0);
}

// ---- the report's groups and counters, each pinned -----------------------------------------------------------------------

namespace {

struct Strategy {
  std::uint32_t groups = 0;
  std::uint32_t sanitised = 0;
  bool done = false;
  formats::FormatError error = formats::FormatError::None;
};

// The module through the registered strategy, the way the file browser runs it.
Strategy through_strategy(const ModuleSpec& m, ImportOptions opt = ImportOptions()) {
  static ImportReader reader;   // 13 KB
  reader.options() = opt;
  const Bytes bytes = test::mmd::build(m);
  SpanSource src(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  test::MemorySource dummy(nullptr, 0);
  formats::ReadParams params;
  params.random = &src;
  reader.begin(dummy, arena(), params);
  while (reader.step(1u << 20) == Progress::Running) {
  }
  Strategy out;
  out.groups = reader.report().dropped_groups;
  out.sanitised = reader.report().sanitised;
  out.done = reader.progress() == Progress::Done;
  out.error = reader.report().error;
  return out;
}

constexpr std::uint32_t bit(unsigned group) { return 1u << group; }
constexpr std::uint32_t kLoopBit = 1u << kDropLoopPoint;   // every song that does not end loops back to its start

ModuleSpec tiny_song() {
  ModuleSpec m = song_of({block(2, 2)}, {0});
  m.blocks[0].set(0, 0, 13, 1, 0, 0);
  m.instruments = {sample8(8)};
  return m;
}

}  // namespace

TEST_CASE("med import: each group of what could not be kept is raised by what it names, and by nothing else") {
  {
    Strategy g = through_strategy(tiny_song());
    CHECK(g.done);
    CHECK(g.groups == kLoopBit);   // a plain song: only the loop point
    CHECK(g.sanitised == 0);
  }
  {
    ModuleSpec m = song_of({block(17, 2)}, {0});
    for (unsigned t = 0; t < 17; ++t) m.blocks[0].set(0, t, 13, 1, 0, 0);
    m.instruments = {sample8(8)};
    CHECK(through_strategy(m).groups == (bit(kDropTracks) | kLoopBit));
  }
  {
    ModuleSpec m = tiny_song();
    m.blocks[0].set(1, 1, 0, 0, 0x0A, 0x05);   // a volume slide: no target
    CHECK(through_strategy(m).groups == (bit(kDropCommands) | kLoopBit));
  }
  {
    ModuleSpec m = tiny_song();
    InstrSpec packed = sample8(8);
    packed.type = 0x80;                       // packed data: refused
    m.instruments.push_back(packed);
    CHECK(through_strategy(m).groups == (bit(kDropInstruments) | kLoopBit));
  }
  {
    Strategy g = through_strategy(endless_loop(), [] { ImportOptions o; o.overflow = Overflow::Structural; return o; }());
    CHECK(g.done);
    CHECK(g.groups == (bit(kDropFlow) | bit(kDropCommands)));   // structural: the loops were dropped; no loop point is found
  }
  {
    Strategy g = through_strategy(too_many_patterns(), [] { ImportOptions o; o.overflow = Overflow::Truncate; return o; }());
    CHECK(g.done);
    CHECK(g.groups == bit(kDropFlow));
  }
  {
    ModuleSpec m = tiny_song();
    m.instruments[0].ext = {2, 3, 0, 0, 0, 0};   // hold 2, decay 3
    m.s_ext_entrsz = 6;
    CHECK(through_strategy(m).groups == (bit(kDropHoldDecay) | kLoopBit));
    m.instruments[0].ext = {0, 0, 0, 0, 25, 0};  // a default pitch
    CHECK(through_strategy(m).groups == (bit(kDropDefaultPitch) | kLoopBit));
  }
  {
    ModuleSpec m = tiny_song();
    m.instruments[0].midich = 3;                 // MIDI with a program
    m.instruments[0].midipreset = 9;
    CHECK(through_strategy(m).groups == (bit(kDropMidiProgram) | kLoopBit));
  }
  {
    ModuleSpec m = tiny_song();
    m.mastervol = 32;
    CHECK(through_strategy(m).groups == (bit(kDropMasterVolume) | kLoopBit));
    m.mastervol = 64;                            // the default, and 0 (unset), are nothing to report
    CHECK(through_strategy(m).groups == kLoopBit);
    m.mastervol = 0;
    CHECK(through_strategy(m).groups == kLoopBit);
  }
  {
    ModuleSpec m = tiny_song();
    InstrSpec syn;
    syn.has_synth = true;
    syn.synth.wforms = 1;
    syn.synth.waves = {Bytes(8, 0x10)};
    m.instruments.push_back(syn);
    CHECK(through_strategy(m).groups == (bit(kDropSynthTables) | kLoopBit));
  }
  {
    ModuleSpec m = tiny_song();                  // a song that ends does not loop back: no loop point to report
    m.blocks[0].set(1, 0, 0, 0, 0x0F, 0xFE);
    CHECK(through_strategy(m).groups == 0);
  }
}

TEST_CASE("med import: what is clamped to the model is counted, and sanitised is their sum") {
  // start at tempo 240, speed 3 (far above 800 BPM), a note beyond the model, a volume above full, a tempo above 800 BPM and a
  // transposition beyond the tune range of 24: six values the model could not hold as they were
  ModuleSpec m = song_of({block(1, 3)}, {0});
  m.deftempo = 240;
  m.tempo2 = 3;
  m.blocks[0].set(0, 0, 100, 1, 0, 0);
  m.blocks[0].set(1, 0, 13, 1, 0x0C, 0x65);
  m.blocks[0].set(2, 0, 0, 0, 0x0F, 0x7D);
  InstrSpec s = sample8(8);
  s.strans = 40;
  m.instruments = {s};
  Run r(m);
  NEED(r.ok());
  CHECK(r.rep().notes_out_of_range == 1);
  CHECK(r.rep().volume_clamped == 1);
  CHECK(r.rep().initial_tempo_clamped);
  CHECK(r.rep().flow.tempo_clamps == 1);
  CHECK(r.rep().tempo_fx_clamped == 1);
  CHECK((r.rep().instrument[0].flags & kInstTuneClamped) != 0);
  CHECK(r.p().tempo_x100() == 80000);
  const Strategy g = through_strategy(m);
  CHECK(g.done);
  CHECK(g.sanitised == 6);
  CHECK(g.groups == kLoopBit);
}

// ---- the scan is weighted by how often a block is played, and both passes have a cap --------------------------------------

TEST_CASE("med import: which tracks are kept counts the plays: a track in a block played five times beats one in a block played once") {
  ModuleSpec m = song_of({block(17, 12), block(17, 1)}, {0, 1, 1, 1, 1, 1});
  for (unsigned t = 0; t < 15; ++t) {
    for (unsigned l = 0; l < 12; ++l) m.blocks[0].set(l, t, 13, 1, 0, 0);   // 15 tracks busier than either of the two
  }
  for (unsigned l = 0; l < 3; ++l) m.blocks[0].set(l, 15, 14, 1, 0, 0);       // track 15: 3 cells, played once
  m.blocks[1].set(0, 16, 15, 1, 0, 0);                                        // track 16: 1 cell, played five times
  m.instruments = {sample8(8)};
  Run r(m);
  NEED(r.ok());
  CHECK(r.rep().tracks_used == 17);
  CHECK(r.rep().track[15].cells == 3);
  CHECK(r.rep().track[15].notes == 3);
  CHECK(r.rep().track[16].cells == 5);
  CHECK(r.rep().track[16].notes == 5);
  CHECK(r.rep().track[15].slot == kNoSlot);   // unweighted, 16 (1 cell) would lose to 15 (3 cells)
  CHECK(r.rep().track[16].slot == 15);
  CHECK(r.rep().tracks_dropped == 1);
}

TEST_CASE("med import: the scan and the mapping pass each stop at their cell cap, to the cell") {
  // the golden song has 4 lines of 4 tracks on 2 pages: 32 cells in each pass
  const Bytes bytes = test::mmd::build(golden());
  {
    ImportOptions opt;
    opt.max_scan_cells = 32;
    opt.max_map_cells = 32;
    Run r(bytes, opt);
    CHECK(r.ok());
  }
  {
    ImportOptions opt;
    opt.max_scan_cells = 31;
    Run r(bytes, opt);
    CHECK(!r.ok());
    CHECK(importer().status().error == Error::LimitExceeded);
    CHECK(std::string(importer().status().detail).find("scanned") != std::string::npos);
    check_project_state(false);
  }
  {
    ImportOptions opt;
    opt.max_map_cells = 31;
    Run r(bytes, opt);
    CHECK(!r.ok());
    CHECK(importer().status().error == Error::LimitExceeded);
    CHECK(std::string(importer().status().detail).find("mapped") != std::string::npos);
    check_project_state(false);
  }
  {
    ImportOptions opt;
    opt.max_scan_cells = 0;
    opt.max_map_cells = 0;
    Run r(bytes, opt);
    CHECK(!r.ok());
  }
}

// ---- validate in pieces ---------------------------------------------------------------------------------------------------

namespace {

bool same_summary(const Summary& a, const Summary& b) {
  return a.songs == b.songs && a.blocks_referenced == b.blocks_referenced && a.empty_blocks == b.empty_blocks &&
         a.bad_play_entries == b.bad_play_entries && a.skip_play_entries == b.skip_play_entries && a.bad_sections == b.bad_sections &&
         a.play_commands == b.play_commands && a.max_tracks == b.max_tracks && a.max_lines == b.max_lines &&
         a.pages_max == b.pages_max && a.cells == b.cells && a.instruments == b.instruments && a.synth == b.synth &&
         a.unsupported_mask == b.unsupported_mask && a.oversize_mask == b.oversize_mask && a.pcm_bytes == b.pcm_bytes &&
         a.ignored == b.ignored;
}

}  // namespace

TEST_CASE("med import: validate in steps of any size is validate, on whole files and on every kind of cut") {
  std::vector<Bytes> files;
  for (unsigned v = 0; v <= 3; ++v) files.push_back(test::mmd::build(test::mmd::sample_module(v)));
  files.push_back(test::mmd::build(golden()));
  files.push_back(test::mmd::build(too_many_patterns()));
  unsigned compared = 0, refused = 0;
  for (const Bytes& full : files) {
    for (std::size_t len = 0; len <= full.size(); len += (len < 64 || len + 64 > full.size()) ? 1 : 3 * kThin) {
      const Bytes cut(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(len));
      SpanSource src(cut.data(), static_cast<std::uint32_t>(cut.size()));
      Reader whole;
      if (!whole.open(src).ok()) continue;
      Summary want;
      const Status ws = whole.validate(want);
      for (std::size_t budget : {std::size_t{1}, std::size_t{2}, std::size_t{5}, std::size_t{64}, ~std::size_t{0}}) {
        Reader piece;
        NEED(piece.open(src).ok());
        Summary got;
        ValidateState st;
        Status gs;
        unsigned guard = 0;
        while (st.phase != ValidateState::Phase::Done && gs.ok()) {
          gs = piece.validate_step(st, got, budget);
          NEED(++guard < 100000);
        }
        CHECK(gs.error == ws.error);
        CHECK(gs.offset == ws.offset);
        CHECK(std::string(gs.detail) == std::string(ws.detail));
        if (ws.ok()) {
          CHECK(st.phase == ValidateState::Phase::Done);
          CHECK(same_summary(got, want));
        }
      }
      ++compared;
      refused += ws.ok() ? 0u : 1u;
    }
  }
  CHECK(compared > 100);
  CHECK(refused > 50);
}

// ---- the job: the budget changes nothing --------------------------------------------------------------------------------

TEST_CASE("med import: the result does not depend on the size of the steps") {
  for (unsigned v = 0; v <= 3; ++v) {
    CAPTURE(v);
    const Bytes bytes = test::mmd::build(test::mmd::sample_module(v));
    Run whole(bytes);
    NEED(whole.ok());
    const std::uint32_t want = digest(whole.p(), whole.rep());
    for (std::size_t budget : {std::size_t{1}, std::size_t{7}, std::size_t{100}, std::size_t{3000}}) {
      CAPTURE(budget);
      Run piece(bytes, ImportOptions(), budget);
      NEED(piece.ok());
      CHECK(digest(piece.p(), piece.rep()) == want);
    }
  }
}

TEST_CASE("med import: step always makes progress and permille only goes up") {
  const Bytes bytes = test::mmd::build(golden());
  SpanSource src(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  importer().begin(src, arena(), ImportOptions());
  CHECK(importer().progress() == Progress::Running);
  unsigned last = 0, steps = 0;
  while (importer().step(1) == Progress::Running) {
    const unsigned pm = importer().permille();
    CHECK(pm >= last);
    CHECK(pm <= 1000);
    last = pm;
    NEED(++steps < 100000);
  }
  CHECK(importer().progress() == Progress::Done);
  CHECK(importer().permille() == 1000);
  CHECK(steps > 10);
  // a finished job stays finished
  CHECK(importer().step(1000) == Progress::Done);
}

// ---- the PCM ------------------------------------------------------------------------------------------------------------

namespace {

std::vector<std::int16_t> pcm_of(unsigned slot) {
  std::vector<std::int16_t> v(importer().pcm_frames(slot));
  if (!v.empty()) {
    CHECK(importer().read_pcm(slot, 0, static_cast<std::uint32_t>(v.size()), v.data()).ok());
  }
  return v;
}

ModuleSpec pcm_module() {
  ModuleSpec m = song_of({block(1, 1)}, {0});
  InstrSpec a;                       // 1: 8-bit, 600 frames (more than one 512-byte piece)
  for (unsigned i = 0; i < 600; ++i) a.data.push_back(static_cast<std::uint8_t>(i * 7));
  InstrSpec b;                       // 2: 16-bit, 700 frames (two pieces of 256)
  b.type = 0x10;
  for (unsigned i = 0; i < 700; ++i) {
    const std::uint16_t v = static_cast<std::uint16_t>(i * 251u - 30000u);
    b.data.push_back(static_cast<std::uint8_t>(v >> 8));
    b.data.push_back(static_cast<std::uint8_t>(v));
  }
  InstrSpec c;                       // 3: stereo 8-bit, 100 frames, left then right
  c.type = 0x20;
  for (unsigned i = 0; i < 100; ++i) c.data.push_back(static_cast<std::uint8_t>(i * 3));
  for (unsigned i = 0; i < 100; ++i) c.data.push_back(static_cast<std::uint8_t>(250 - i * 5));
  InstrSpec d;                       // 4: three octaves of 5 frames, each filled with its own value range
  d.type = 2;
  for (unsigned j = 0; j < 3; ++j) {
    for (unsigned i = 0; i < (5u << j); ++i) d.data.push_back(static_cast<std::uint8_t>(0x10 * (j + 1) + i));
  }
  InstrSpec e;                       // 5: a synth instrument
  e.has_synth = true;
  e.synth.wforms = 2;
  e.synth.waves = {Bytes(8, 0x10), Bytes(16, 0x20)};
  InstrSpec f;                       // 6: a hybrid, whose first waveform is a sample
  f.has_synth = true;
  f.synth.hybrid = true;
  f.synth.wforms = 2;
  f.synth.waves = {Bytes(12, 0x30), Bytes(8, 0x40)};
  InstrSpec g;                       // 7: stereo 16-bit, odd sums (the mix truncates toward zero)
  g.type = 0x30;
  for (unsigned i = 0; i < 20; ++i) {
    const std::int16_t v = static_cast<std::int16_t>(-1000 - 3 * static_cast<int>(i));
    g.data.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(v) >> 8));
    g.data.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(v)));
  }
  for (unsigned i = 0; i < 20; ++i) {
    const std::int16_t v = static_cast<std::int16_t>(500 + 7 * static_cast<int>(i));
    g.data.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(v) >> 8));
    g.data.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(v)));
  }
  m.instruments = {a, b, c, d, e, f, g};
  return m;
}

}  // namespace

TEST_CASE("med import: the PCM of every instrument kind") {
  Run r(pcm_module());
  NEED(r.ok());
  // 8-bit: times 256
  std::vector<std::int16_t> v = pcm_of(0);
  NEED(v.size() == 600);
  for (unsigned i = 0; i < 600; ++i) {
    NEED(v[i] == static_cast<std::int16_t>(static_cast<std::int8_t>(static_cast<std::uint8_t>(i * 7)) * 256));
  }
  // 16-bit: big-endian as it is
  v = pcm_of(1);
  NEED(v.size() == 700);
  for (unsigned i = 0; i < 700; ++i) {
    NEED(v[i] == static_cast<std::int16_t>(static_cast<std::uint16_t>(i * 251u - 30000u)));
  }
  // stereo: (L + R) / 2 of the 8-bit values, times 256
  v = pcm_of(2);
  NEED(v.size() == 100);
  for (unsigned i = 0; i < 100; ++i) {
    const int l = static_cast<std::int8_t>(static_cast<std::uint8_t>(i * 3)) * 256;
    const int rr = static_cast<std::int8_t>(static_cast<std::uint8_t>(250 - i * 5)) * 256;
    NEED(v[i] == (l + rr) / 2);
  }
  // the longest octave only: 20 frames of the third octave
  v = pcm_of(3);
  NEED(v.size() == 20);
  for (unsigned i = 0; i < 20; ++i) NEED(v[i] == static_cast<std::int16_t>((0x30 + i) * 256));
  // a synth instrument: its first waveform
  v = pcm_of(4);
  NEED(v.size() == 8);
  for (std::int16_t s : v) CHECK(s == 0x1000);
  // a hybrid: its sample
  v = pcm_of(5);
  NEED(v.size() == 12);
  for (std::int16_t s : v) CHECK(s == 0x3000);
  // stereo 16-bit with odd sums: truncation toward zero
  v = pcm_of(6);
  NEED(v.size() == 20);
  for (unsigned i = 0; i < 20; ++i) {
    const int l = -1000 - 3 * static_cast<int>(i), rr = 500 + 7 * static_cast<int>(i);
    NEED(v[i] == (l + rr) / 2);
  }
  CHECK(r.rep().pcm_bytes == 2u * (600 + 700 + 100 + 20 + 8 + 12 + 20));
}

TEST_CASE("med import: PCM can be read in any pieces, and nothing past the end") {
  Run r(pcm_module());
  NEED(r.ok());
  const std::vector<std::int16_t> whole = pcm_of(0);
  NEED(whole.size() == 600);
  const std::uint32_t starts[] = {0, 1, 100, 509, 511, 512, 513, 599, 600};
  for (std::uint32_t first : starts) {
    for (std::uint32_t n : {0u, 1u, 2u, 3u, 100u, 511u, 512u, 513u, 600u}) {
      if (first + n > 600) continue;
      CAPTURE(first);
      CAPTURE(n);
      std::vector<std::int16_t> out(n + 1, 12345);
      CHECK(importer().read_pcm(0, first, n, out.data()).ok());
      for (std::uint32_t i = 0; i < n; ++i) NEED(out[i] == whole[first + i]);
      CHECK(out[n] == 12345);   // one past is untouched
    }
  }
  std::int16_t one = 0;
  CHECK(!importer().read_pcm(0, 0, 601, &one).ok());     // past the end
  CHECK(!importer().read_pcm(0, 601, 0, &one).ok());
  CHECK(!importer().read_pcm(0, 600, 1, &one).ok());
  CHECK(!importer().read_pcm(0, 0xFFFFFFFFu, 1, &one).ok());
  CHECK(!importer().read_pcm(0, 0, 1, nullptr).ok());     // nowhere to put it
  CHECK(importer().read_pcm(0, 0, 0, nullptr).ok());      // nothing to put is fine
  CHECK(!importer().read_pcm(63, 0, 1, &one).ok());       // no such instrument
  CHECK(!importer().read_pcm(40, 0, 1, &one).ok());       // an empty slot
  // 16-bit pieces across the 256-frame chunk
  const std::vector<std::int16_t> wide = pcm_of(1);
  std::vector<std::int16_t> out(300);
  CHECK(importer().read_pcm(1, 200, 300, out.data()).ok());
  for (unsigned i = 0; i < 300; ++i) NEED(out[i] == wide[200 + i]);
}

TEST_CASE("med import: no PCM for a MIDI instrument, a refused one, or a job that has not finished") {
  Run r(golden());
  NEED(r.ok());
  std::int16_t one = 0;
  CHECK(importer().pcm_frames(1) == 0);                 // the MIDI instrument
  CHECK(!importer().read_pcm(1, 0, 1, &one).ok());
  CHECK(importer().pcm_frames(0) == 64);
  CHECK(importer().read_pcm(0, 0, 1, &one).ok());
  const Bytes bytes = test::mmd::build(golden());
  SpanSource src(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  importer().begin(src, arena(), ImportOptions());
  CHECK(!importer().read_pcm(0, 0, 1, &one).ok());      // not finished
}

// ---- hostile input ------------------------------------------------------------------------------------------------------

TEST_CASE("med import: a file cut at any offset is refused cleanly or imported, never worse") {
  for (unsigned v = 0; v <= 3; ++v) {
    CAPTURE(v);
    const Bytes full = test::mmd::build(test::mmd::sample_module(v));
    unsigned done = 0, failed = 0;
    for (std::size_t len = 0; len <= full.size(); len += (len + kThin > full.size() - 64 || len < 64) ? 1 : kThin) {
      const Bytes cut(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(len));
      Run r(cut);
      if (r.ok()) {
        ++done;
        check_project_state(true);
      } else {
        ++failed;
        check_project_state(false);
        NEED(importer().status().error != Error::None);
      }
      NEED(r.rep().patterns <= 128);
    }
    CHECK(failed > 10);
    CHECK(done >= 1);  // the whole file, at least
  }
}

TEST_CASE("med import: mutated modules never break the project or the reader") {
  test::Rng rng(0x5EEDu);
  for (unsigned v = 0; v <= 3; ++v) {
    const Bytes base = test::mmd::build(test::mmd::sample_module(v));
    for (unsigned k = 0; k < kMutants; ++k) {
      Bytes m = base;
      const unsigned flips = 1 + rng.below(6);
      for (unsigned i = 0; i < flips; ++i) {
        m[rng.below(static_cast<std::uint32_t>(m.size()))] = rng.byte();
      }
      ImportOptions opt;
      opt.overflow = (k % 3 == 0) ? Overflow::Structural : (k % 3 == 1 ? Overflow::Truncate : Overflow::Refuse);
      opt.engine_fx = (k % 5) == 0;
      Run r(m, opt, 50 + rng.below(4000));
      if (r.ok()) {
        NEED(project().check_invariants());
        NEED(r.rep().patterns == project().pattern_count());
        for (unsigned s = 0; s < caps::kMaxSamples; ++s) {
          if (r.rep().instrument[s].result == InstResult::Sample) {
            std::int16_t pcm[8];
            NEED(importer().read_pcm(s, 0, r.rep().instrument[s].frames < 8 ? r.rep().instrument[s].frames : 8u, pcm).ok());
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

TEST_CASE("med import: an empty module imports, junk is refused") {
  // no instruments and no blocks at all
  ModuleSpec empty;
  empty.version = 1;
  Run e(empty);
  CHECK(e.ok());
  check_project_state(true);
  // not an MMD file
  Bytes junk(900, 0x55);
  Run j(junk);
  CHECK(!j.ok());
  CHECK(importer().status().error == Error::BadMagic);
  check_project_state(false);
}

// ---- the registry ---------------------------------------------------------------------------------------------------------

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

TEST_CASE("med import: registered as \"med\", found by name and by bytes") {
  formats::FormatRegistry& reg = registry();
  const formats::FormatDesc* d = reg.find_id("med");
  NEED(d != nullptr);
  CHECK(d->kind == formats::FormatKind::Project);
  CHECK(d->reader == &storage().med_reader);
  CHECK(d->writer == nullptr);
  CHECK(d->sniff != nullptr);
  CHECK(d->group_name != nullptr);
  for (const char* name : {"song.med", "SONG.MED", "a.b.mmd", "x.mmd0", "x.MMD1", "x.mmd2", "dir/x.mmd3", "C:\\m\\x.Med"}) {
    CAPTURE(name);
    CHECK(reg.find_name(name) == d);
  }
  for (const char* name : {"song.mmd4", "song.mod", "med", "song.med.bak", "._song.med"}) {
    CAPTURE(name);
    CHECK(reg.find_name(name) != d);
  }
  const std::uint8_t mmd1[16] = {'M', 'M', 'D', '1', 0, 0, 0, 0};
  CHECK(reg.sniff(mmd1, 16, 0) == d);
  CHECK(reg.sniff(mmd1, 16, 5000) == d);
  CHECK(reg.sniff(mmd1, 16, 100) != d);       // too small to hold a header and a song record
  CHECK(reg.sniff(mmd1, 3, 0) == d);          // not enough to tell the version: still a candidate
  const std::uint8_t mmd0[4] = {'M', 'M', 'D', '0'}, mmd3[4] = {'M', 'M', 'D', '3'}, mmd4[4] = {'M', 'M', 'D', '4'};
  CHECK(reg.sniff(mmd0, 4, 0) == d);
  CHECK(reg.sniff(mmd3, 4, 0) == d);
  CHECK(reg.sniff(mmd4, 4, 0) != d);
  const std::uint8_t mcn[4] = {'M', 'C', 'N', '1'};
  CHECK(reg.sniff(mcn, 4, 0) != d);           // a chained song header is never the start of a file
  // the name decides when it is known, and the bytes must agree
  CHECK(reg.identify("song.med", mmd1, 16, 5000) == d);
  CHECK(reg.identify("song.med", mmd4, 4, 0) == nullptr);
  CHECK(reg.identify("untitled", mmd1, 16, 5000) == d);
  CHECK(std::string(d->group_name(kDropEcho)) == "echo");
  CHECK(std::string(d->group_name(kDropTracks)) == "tracks without a slot");
  CHECK(std::string(d->group_name(kDropGroupCount)) == "");
  for (unsigned i = 0; i < kDropGroupCount; ++i) CHECK(std::string(d->group_name(i)) != "");
}

TEST_CASE("med import: the strategy: ReadParams::random, the report, the options") {
  formats::FormatRegistry& reg = registry();
  const formats::FormatDesc* d = reg.find_id("med");
  NEED(d != nullptr);
  formats::FormatReader& reader = *d->reader;
  const Bytes bytes = test::mmd::build(golden());
  SpanSource src(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  test::MemorySource dummy(nullptr, 0);
  formats::ReadParams params;
  // without random access it is refused as a bug of the caller, and the project is reset
  arena().project.reset();
  CHECK(arena().project.set_name("keep", 4) == model::Status::Ok);
  reader.begin(dummy, arena(), params);
  CHECK(reader.progress() == Progress::Failed);
  CHECK(reader.report().error == formats::FormatError::Internal);
  CHECK(std::string(arena().project.name()).empty());
  CHECK(reader.step(1000) == Progress::Failed);
  // with it
  params.random = &src;
  reader.begin(dummy, arena(), params);
  CHECK(reader.progress() == Progress::Running);
  unsigned guard = 0;
  while (reader.step(64) == Progress::Running) NEED(++guard < 100000);
  CHECK(reader.progress() == Progress::Done);
  const formats::FormatReport& fr = reader.report();
  CHECK(fr.error == formats::FormatError::None);
  CHECK(fr.patterns == 1);
  CHECK(fr.instruments == 3);
  CHECK(fr.bytes == bytes.size());
  CHECK(reader.permille() == 1000);
  CHECK(fr.dropped_groups == (1u << kDropLoopPoint));   // only: the song returns to its start
  CHECK(fr.sanitised == 0);
  CHECK(std::string(arena().project.name()) == "Golden");
  {  // the registered reader writes the engine window by default (OS-008c): 03 10 on track 2 is E03, and the line below says "no effect" where a free column allows
    const model::Pattern* pt = arena().project.pattern(1);
    NEED(pt != nullptr);
    CHECK(pt->steps[2][0].fx[0].type == 131);
    CHECK(pt->steps[2][3].fx[0].type == 128);
  }
  // a refused file: the error, the detail, a reset project
  const Bytes junk(900, 0x55);
  SpanSource jsrc(junk.data(), static_cast<std::uint32_t>(junk.size()));
  params.random = &jsrc;
  reader.begin(dummy, arena(), params);
  while (reader.step(1000) == Progress::Running) {}
  CHECK(reader.progress() == Progress::Failed);
  CHECK(reader.report().error == formats::FormatError::BadMagic);
  CHECK(std::string(reader.report().detail) != "");
  CHECK(arena().project.pattern_count() == 0);
  // the options live on the object
  auto* med = static_cast<ImportReader*>(d->reader);
  med->options().clock = Clock::Ntsc;
  params.random = &src;
  reader.begin(dummy, arena(), params);
  while (reader.step(100000) == Progress::Running) {}
  CHECK(reader.progress() == Progress::Done);
  CHECK(arena().project.tempo_x100() == 12452);
  med->options() = ImportOptions();
  // the instrument PCM through the registered object
  reader.begin(dummy, arena(), params);
  while (reader.step(100000) == Progress::Running) {}
  CHECK(med->importer().pcm_frames(0) == 64);
}

TEST_CASE("med import: dropped groups name what the model could not keep") {
  ModuleSpec m = test::mmd::sample_module(2);   // echo, volume adjust, stereo separation, synth, hybrid, unsupported-free
  Run r(m);
  NEED(r.ok());
  const std::uint32_t ig = r.rep().ignored;
  CHECK((ig & kIgnoreEcho) != 0);
  CHECK((ig & kIgnoreVolAdjust) != 0);
  CHECK((ig & kIgnoreStereoSep) != 0);
  formats::FormatRegistry& reg = registry();
  const formats::FormatDesc* d = reg.find_id("med");
  NEED(d != nullptr);
  const Bytes bytes = test::mmd::build(m);
  SpanSource src(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  test::MemorySource dummy(nullptr, 0);
  formats::ReadParams params;
  params.random = &src;
  d->reader->begin(dummy, arena(), params);
  while (d->reader->step(100000) == Progress::Running) {}
  NEED(d->reader->progress() == Progress::Done);
  const std::uint32_t g = d->reader->report().dropped_groups;
  CHECK((g & (1u << kDropEcho)) != 0);
  CHECK((g & (1u << kDropVolAdjust)) != 0);
  CHECK((g & (1u << kDropStereoSep)) != 0);
  CHECK((g & (1u << kDropSynthTables)) != 0);
}

// ---- the maintainer's own modules, when they are there ---------------------------------------------------------------------

#if !defined(__arm__)
namespace {

bool has_med_extension(const std::string& name) {
  std::string l;
  for (char c : name) l.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
  for (const char* e : {".med", ".mmd", ".mmd0", ".mmd1", ".mmd2", ".mmd3"}) {
    const std::string x = e;
    if (l.size() > x.size() && l.compare(l.size() - x.size(), x.size(), x) == 0) return true;
  }
  return l.rfind("mmd", 0) == 0 && l.size() > 4 && l[3] >= '0' && l[3] <= '3' && l[4] == '.';
}

}  // namespace

// The directory O3_MED_DIR (every file in it with a MED name, not recursive). Counts only are printed, never a name or a note.
TEST_CASE("med import: the fixture modules, when present, import without a crash") {
  const char* dir = std::getenv("O3_MED_DIR");
  if (dir == nullptr || dir[0] == '\0') {
    std::printf("med import survey: O3_MED_DIR not set, skipped\n");
    return;
  }
  std::string cmd = std::string("ls -1 '") + dir + "' 2>/dev/null";
  std::FILE* ls = popen(cmd.c_str(), "r");
  if (ls == nullptr) {
    std::printf("med import survey: cannot list the directory, skipped\n");
    return;
  }
  std::vector<std::string> names;
  char line[512];
  while (std::fgets(line, sizeof line, ls) != nullptr) {
    std::string n(line);
    while (!n.empty() && (n.back() == '\n' || n.back() == '\r')) n.pop_back();
    if (has_med_extension(n)) names.push_back(n);
  }
  pclose(ls);
  if (names.empty()) {
    std::printf("med import survey: no module in the directory, skipped\n");
    return;
  }
  unsigned ok = 0, structural = 0, refused = 0, patterns = 0, dropped_tracks = 0, refused_instruments = 0;
  for (const std::string& n : names) {
    std::FILE* f = std::fopen((std::string(dir) + "/" + n).c_str(), "rb");
    if (f == nullptr) continue;
    Bytes bytes;
    std::uint8_t buf[4096];
    std::size_t got;
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + got);
    std::fclose(f);
    ImportOptions opt;
    {
      Run r(bytes, opt);
      if (!r.ok() && importer().status().error == Error::LimitExceeded) {
        opt.overflow = Overflow::Structural;
        Run s(bytes, opt);
        if (s.ok()) ++structural;
        else ++refused;
        check_project_state(s.ok());
        continue;
      }
      if (r.ok()) {
        ++ok;
        patterns += r.rep().patterns;
        dropped_tracks += r.rep().tracks_dropped;
        refused_instruments += r.rep().instruments_refused;
        check_project_state(true);
      } else {
        ++refused;
        check_project_state(false);
      }
    }
  }
  std::printf("med import survey: %u modules: %u imported, %u by the structural fallback, %u refused; %u patterns, %u tracks dropped, "
              "%u instruments refused\n",
              static_cast<unsigned>(names.size()), ok, structural, refused, patterns, dropped_tracks, refused_instruments);
  CHECK(ok + structural + refused <= names.size());
}
#endif
