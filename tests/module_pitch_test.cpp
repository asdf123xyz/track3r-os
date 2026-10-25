// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-MODPITCH (docs/specs/OS-MODPITCH.md): the pitch-FX path of the module instruments. Step 1 (this file's first case): what the sequencer's Arp 21 and Chord 14 do on an instrument that is an SD
// module, end to end through the Sequencer, the Engine and ModuleVoices, with a recording module. Host only (the sequencer's project is 1.9 MB).
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"

#include <o3os/audio/engine.h>
#include <o3os/audio/module_voices.h>
#include <o3os/audio/taps.h>
#include <o3os/audio/voices.h>
#include <o3os/seq/engine_link.h>
#include <o3os/seq/fx.h>
#include <o3os/seq/sequencer.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using namespace o3os;
using audio::EventKind;
using audio::ModuleVoices;
namespace haudio = o3os::hal::audio;
namespace fx = o3os::seq::fx;

// ---- a module that writes down every call the host makes into it ----------------------------------------------------------------------
struct Call {
  char kind;  // 'N' note_on, 'F' note_off
  std::uint32_t block;
  std::uint32_t note;
  std::uint32_t flags;
};
std::vector<Call> g_log;
std::uint32_t g_block = 0;

struct alignas(16) RecState {
  std::uint32_t gate;
};

int rec_init(void* s, const o3m_host*) {
  static_cast<RecState*>(s)->gate = 0;
  return 0;
}
std::uint32_t rec_render(void* s, const float*, float* out, std::uint32_t n) {
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = 0.0f;
  }
  return static_cast<RecState*>(s)->gate;
}
void rec_set_param(void*, std::uint32_t, float) {}
void rec_note_on(void* s, std::uint32_t note, std::uint32_t, std::uint32_t flags) {
  static_cast<RecState*>(s)->gate = 1;
  g_log.push_back(Call{'N', g_block, note, flags});
}
void rec_note_off(void* s, std::uint32_t note) {
  static_cast<RecState*>(s)->gate = 0;
  g_log.push_back(Call{'F', g_block, note, 0});
}
void rec_shutdown(void*) {}

const o3m_module kRec = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, sizeof(o3m_module), "rec", sizeof(RecState), 0, nullptr,
                         rec_init, rec_render, rec_set_param, rec_note_on, rec_note_off, rec_shutdown, nullptr, 0};

constexpr std::uint8_t kModInstr = 2;  // the module is instrument slot 1, which a step names as 2

// A sequencer over a project of one module instrument (slot 1) and sample instruments, driving the Engine that has the recording module bound.
struct SeqRig {
  alignas(16) std::uint8_t scratch[ModuleVoices::kScratchBytes];
  alignas(16) RecState rec;
  audio::Engine* engine = nullptr;
  audio::VoicePool* pool = nullptr;
  ModuleVoices* mv = nullptr;
  seq::Sequencer* seq = nullptr;
  haudio::BlockReport report{};
  alignas(8) std::int16_t in[haudio::kBlockSamples] = {};
  alignas(8) std::int16_t out[haudio::kBlockSamples] = {};

  static std::uint32_t cycles() noexcept { return 0u; }

  explicit SeqRig(void (*build)()) {
    static test::StaticSlot<audio::Engine> engine_slot;
    static test::StaticSlot<audio::VoicePool> pool_slot;
    static test::StaticSlot<ModuleVoices> mv_slot;
    static test::StaticSlot<seq::Sequencer> seq_slot;
    static audio::TapMem taps;
    static audio::SeqEvent events[audio::kMaxEventsPerBlock];
    test::fresh_project(4, 1);
    model::Project& p = test::seq_project();
    CHECK(p.add_module("rec", 2, 1, 0) == model::Status::Ok);
    model::Instrument m;
    model::init_instrument(m, model::InstrumentKind::Module);
    m.module_index = 0;
    CHECK(p.set_instrument(kModInstr - 1u, m) == model::Status::Ok);
    CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
    CHECK(p.create_pattern(1, 16) == model::Status::Ok);
    build();
    seq = &seq_slot.make(p);
    engine = &engine_slot.make(&SeqRig::cycles, taps);
    pool = &pool_slot.make();
    mv = &mv_slot.make();
    std::memset(scratch, 0, sizeof scratch);
    std::memset(&rec, 0, sizeof rec);
    CHECK(mv->attach_scratch(scratch, sizeof scratch));
    engine->attach_voices(pool);
    engine->attach_modules(mv);
    engine->attach_events(seq::event_source(*seq, events, audio::kMaxEventsPerBlock));
    CHECK(mv->bind(kModInstr - 1u, kRec, 1, &rec, nullptr, 0) == ModuleVoices::Bind::Ok);
    g_log.clear();
    g_block = 0;
  }
  void block() {
    report = haudio::BlockReport{};
    const haudio::BlockIo io{in, out, 0, &report};
    engine->render_block(io);
    ++g_block;
  }
  void play_blocks(std::uint32_t n) {
    for (std::uint32_t i = 0; i < n; ++i) {
      block();
    }
  }
  ~SeqRig() {
    mv->retire_all();
    block();
    block();
    mv->release_all();
  }
};

constexpr std::uint32_t kStepBlocks = 87;  // one step at 120 BPM is 5512.5 samples, 86.1 blocks of 64

std::vector<Call> ons() {
  std::vector<Call> v;
  for (const Call& c : g_log) {
    if (c.kind == 'N') {
      v.push_back(c);
    }
  }
  return v;
}

}  // namespace

// OS-MODPITCH step 1 (spec "Arp 21 may already work on modules"): the sequencer's Arp makes ordinary retriggering NoteOn events on any instrument, so a module slot gets the arpeggio's notes. The
// finding: Arp needs a Chord FX in the other column (otherwise fx_ignored counts it and nothing happens), and a Chord alone is the root only (one voice a track). Hence QA-MOD-099 and the manual are
// corrected and Arp needs no pitch path.
TEST_CASE("module pitch: an Arp with a Chord reaches a module instrument as retriggered notes, a Chord alone is its root, an Arp alone does nothing (event stream, OS-MODPITCH step 1)") {
  {  // Chord Min (0 3 7) + Arp up at 24 microsteps (two notes a step) on step 0; the plain note of step 2 ends the arpeggio
    SeqRig r([] {
      model::Project& p = test::seq_project();
      CHECK(p.set_step(1, 0, 0, test::make_step(60, kModInstr, fx::kChord, 4, fx::kArp, 7)) == model::Status::Ok);
      CHECK(p.set_step(1, 0, 2, test::make_step(50, kModInstr)) == model::Status::Ok);
    });
    r.seq->play_pattern(1);
    r.play_blocks(2 * kStepBlocks + 4);
    const std::vector<Call> on = ons();
    const std::uint32_t want[4] = {60, 63, 67, 60};  // 60 at 0, then every half step; the fifth would fall on step 2 itself, where the plain note 50 takes over
    NEED(on.size() == 5u);
    for (std::size_t i = 0; i < 4; ++i) {
      CAPTURE(i);
      CHECK(on[i].note == want[i]);
      CHECK(on[i].flags == 0u);  // a pattern's note: no O3M_NOTE_LEGATO (no Glide / Slide FX here), and no ramp flag exists before 1.8
    }
    CHECK(on[4].note == 50u);
    CHECK(on[0].block == 0u);
    for (std::size_t i = 1; i < 4; ++i) {
      CHECK(on[i].block - on[i - 1].block >= 42u);  // a half step is 43 blocks of 64 samples
      CHECK(on[i].block - on[i - 1].block <= 44u);
    }
    // each note is released before the next starts (a sequencer note never slides): one note-off per retrigger, in front of it (the plain note 50 included)
    std::uint32_t offs = 0;
    for (const Call& c : g_log) {
      offs += c.kind == 'F' ? 1u : 0u;
    }
    CHECK(offs == 4u);
    CHECK(r.seq->stats().fx_ignored == 0u);
  }
  {  // a Chord alone: the root, once (a module is one voice a track; the other tones are counted as ignored)
    SeqRig r([] { CHECK(test::seq_project().set_step(1, 0, 0, test::make_step(62, kModInstr, fx::kChord, 4)) == model::Status::Ok); });
    r.seq->play_pattern(1);
    r.play_blocks(kStepBlocks);
    const std::vector<Call> on = ons();
    NEED(on.size() == 1u);
    CHECK(on[0].note == 62u);
    CHECK(r.seq->stats().fx_ignored == 1u);
  }
  {  // an Arp alone: nothing to arpeggiate, the note plays once, counted as ignored
    SeqRig r([] { CHECK(test::seq_project().set_step(1, 0, 0, test::make_step(64, kModInstr, fx::kArp, 7)) == model::Status::Ok); });
    r.seq->play_pattern(1);
    r.play_blocks(kStepBlocks);
    const std::vector<Call> on = ons();
    NEED(on.size() == 1u);
    CHECK(on[0].note == 64u);
    CHECK(r.seq->stats().fx_ignored == 1u);
  }
}

// =================================================================================================================================================================================
// OS-MODPITCH step 3: the host side. A module of ABI 1.8 that exports `pitch` is driven by the Slide (41 / 42) and Glide (19) FX of the pattern: ModuleVoices::fx_event holds them, the pitch offset
// is a ramp (audio/pitch_ramp.h, the sampler voices' law) and the module is told with pitch(semitones, per_frame). The events are scripted (a block and a sample each) so that every figure is exact.
// =================================================================================================================================================================================

#include <o3os/audio/pitch_ramp.h>

#include <cmath>

namespace {

using audio::SeqEvent;

// A module that writes down every call and FOLLOWS the pitch calls as the ABI says a module may: from the next frame the offset is `semitones` and it moves by `per_frame` each frame.
struct alignas(16) FState {
  std::uint32_t gate;
  float off;    // the offset of the next frame, semitones
  float slope;  // per frame
  bool bad;     // a pitch call with a number the ABI forbids
};
struct PCall {
  char kind;  // 'N' note_on, 'F' note_off, 'P' pitch
  std::uint32_t block;
  std::uint32_t note;   // N, F
  std::uint32_t flags;  // N
  float semis, per_frame;  // P
};
std::vector<PCall> g_pc;
std::uint32_t g_pblock = 0;
bool g_old_pitch_called = false;

int f_init(void* s, const o3m_host*) {
  std::memset(s, 0, sizeof(FState));
  return 0;
}
std::uint32_t f_render(void* s, const float*, float* out, std::uint32_t n) {
  FState* f = static_cast<FState*>(s);
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = f->gate != 0u ? 0.001f * (1.0f + f->off) : 0.0f;
  }
  f->off += f->slope * static_cast<float>(n);
  return f->gate;
}
void f_set_param(void*, std::uint32_t, float) {}
void f_note_on(void* s, std::uint32_t note, std::uint32_t, std::uint32_t flags) {
  FState* f = static_cast<FState*>(s);
  f->gate = 1;
  f->off = 0.0f;  // a note_on puts the offset back to 0 (the ABI)
  f->slope = 0.0f;
  g_pc.push_back(PCall{'N', g_pblock, note, flags, 0.0f, 0.0f});
}
void f_note_off(void* s, std::uint32_t note) {
  static_cast<FState*>(s)->gate = 0;
  g_pc.push_back(PCall{'F', g_pblock, note, 0, 0.0f, 0.0f});
}
void f_shutdown(void*) {}
void f_pitch(void* s, float semitones, float per_frame) {
  FState* f = static_cast<FState*>(s);
  f->bad = f->bad || !(std::isfinite(semitones) && std::isfinite(per_frame) && std::fabs(semitones) <= 128.0f && std::fabs(per_frame) <= 128.0f);
  f->off = semitones;
  f->slope = per_frame;
  g_pc.push_back(PCall{'P', g_pblock, 0, 0, semitones, per_frame});
}
void f_pitch_must_not_run(void*, float, float) { g_old_pitch_called = true; }

o3m_module follower(std::uint32_t minor, void (*pitch)(void*, float, float)) {
  o3m_module m = {O3M_ABI_MAJOR, minor, O3M_KIND_INSTRUMENT, static_cast<std::uint32_t>(O3M_MODULE_SIZE(minor)), "follow", sizeof(FState), 0, nullptr,
                  f_init, f_render, f_set_param, f_note_on, f_note_off, f_shutdown, nullptr, 0};
  m.pitch = pitch;
  return m;
}
const o3m_module kFollow = follower(8, &f_pitch);                       // ABI 1.8 with pitch
const o3m_module kNoPitch = follower(8, nullptr);                       // ABI 1.8 that leaves it NULL
const o3m_module kOld = follower(7, &f_pitch_must_not_run);             // ABI 1.7: the object has the member (in C++ it is zero by default), here set to prove the host never reads it

// ---- scripted events ----
struct PCue {
  std::uint32_t block;
  SeqEvent ev;
};
std::vector<PCue> g_pcues;
std::uint32_t g_ptempo = 12000;

std::uint32_t p_step(void*, std::uint64_t block_start, std::uint32_t n, SeqEvent* out, std::uint32_t cap) noexcept {
  const std::uint32_t block = static_cast<std::uint32_t>(block_start / n);
  std::uint32_t count = 0;
  for (const PCue& c : g_pcues) {
    if (c.block == block && count < cap) {
      out[count++] = c.ev;
    }
  }
  return count;
}
std::uint32_t p_tempo(void*) noexcept { return g_ptempo; }
std::uint32_t p_cycles() noexcept { return 0u; }

SeqEvent mk(EventKind kind, unsigned offset, unsigned track, unsigned a = 0, unsigned b = 0, unsigned c = 0, unsigned flags = 0) {
  SeqEvent e{};
  e.offset = static_cast<std::uint16_t>(offset);
  e.kind = kind;
  e.track = static_cast<std::uint8_t>(track);
  e.a = static_cast<std::uint8_t>(a);
  e.b = static_cast<std::uint8_t>(b);
  e.c = static_cast<std::uint8_t>(c);
  e.flags = static_cast<std::uint8_t>(flags);
  return e;
}
constexpr unsigned kSlot = 3;
void cue_note(std::uint32_t block, unsigned offset, unsigned track, unsigned note) { g_pcues.push_back({block, mk(EventKind::NoteOn, offset, track, note, kSlot, 100)}); }
void cue_off(std::uint32_t block, unsigned track) { g_pcues.push_back({block, mk(EventKind::NoteOff, 0, track)}); }
void cue_slide(std::uint32_t block, unsigned offset, unsigned track, bool up, unsigned value) {
  g_pcues.push_back({block, mk(EventKind::ModSet, offset, track, up ? audio::kFxSlideUp : audio::kFxSlideDown, value, audio::kLayerFx1, audio::kModGroupSlide)});
}
void cue_slide_end(std::uint32_t block, unsigned track) { g_pcues.push_back({block, mk(EventKind::ModEnd, 0, track, 0, 0, 0, audio::kModGroupSlide)}); }
void cue_glide(std::uint32_t block, unsigned track, unsigned value) {
  g_pcues.push_back({block, mk(EventKind::ModSet, 0, track, audio::kFxGlide, value, audio::kLayerFx1, audio::kModGroupGlide)});
}
void cue_glide_end(std::uint32_t block, unsigned track) { g_pcues.push_back({block, mk(EventKind::ModEnd, 0, track, 0, 0, 0, audio::kModGroupGlide)}); }
void cue_reset(std::uint32_t block) { g_pcues.push_back({block, mk(EventKind::FxReset, 0, audio::kNoTrack)}); }

struct PRig {
  alignas(16) std::uint8_t scratch[ModuleVoices::kScratchBytes];
  alignas(16) FState fs;
  audio::Engine* engine = nullptr;
  audio::VoicePool* pool = nullptr;
  ModuleVoices* mv = nullptr;
  haudio::BlockReport report{};
  alignas(8) std::int16_t in[haudio::kBlockSamples] = {};
  alignas(8) std::int16_t out[haudio::kBlockSamples] = {};
  std::vector<float> off_end;                   // the follower's offset at the end of each block
  std::vector<std::vector<std::int16_t>> feed;  // the track-0 feed of each block (empty: none)

  explicit PRig(const o3m_module& mod) {
    static test::StaticSlot<audio::Engine> engine_slot;
    static test::StaticSlot<audio::VoicePool> pool_slot;
    static test::StaticSlot<ModuleVoices> mv_slot;
    static audio::TapMem taps;
    static SeqEvent events[audio::kMaxEventsPerBlock];
    engine = &engine_slot.make(&p_cycles, taps);
    pool = &pool_slot.make();
    mv = &mv_slot.make();
    std::memset(scratch, 0, sizeof scratch);
    std::memset(&fs, 0, sizeof fs);
    CHECK(mv->attach_scratch(scratch, sizeof scratch));
    engine->attach_voices(pool);
    engine->attach_modules(mv);
    audio::EventSource src;
    src.step = &p_step;
    src.tempo = &p_tempo;
    src.buffer = events;
    src.capacity = audio::kMaxEventsPerBlock;
    engine->attach_events(src);
    CHECK(mv->bind(kSlot, mod, 1, &fs, nullptr, 0) == ModuleVoices::Bind::Ok);
    g_pc.clear();
    g_pblock = 0;
    g_ptempo = 12000;
    g_old_pitch_called = false;
  }
  ~PRig() {
    mv->retire_all();
    block();
    block();
    mv->release_all();
    g_pcues.clear();
  }
  void block() {
    report = haudio::BlockReport{};
    const haudio::BlockIo io{in, out, 0, &report};
    engine->render_block(io);
    off_end.push_back(fs.off);
    const std::int16_t* f = mv->feed(0);
    feed.push_back(f != nullptr ? std::vector<std::int16_t>(f, f + haudio::kBlock) : std::vector<std::int16_t>());
    ++g_pblock;
  }
  void run(std::uint32_t blocks) {
    for (std::uint32_t i = 0; i < blocks; ++i) {
      block();
    }
  }
};

// The calls of block `b` of kind `k` (k 0: any).
std::vector<PCall> in_block(std::uint32_t b, char k = 0) {
  std::vector<PCall> v;
  for (const PCall& c : g_pc) {
    if (c.block == b && (k == 0 || c.kind == k)) {
      v.push_back(c);
    }
  }
  return v;
}
std::uint32_t count_kind(char k) {
  std::uint32_t n = 0;
  for (const PCall& c : g_pc) {
    n += c.kind == k ? 1u : 0u;
  }
  return n;
}

}  // namespace

TEST_CASE("module pitch: the ramp helper is the sampler's old arithmetic, step for step (the law is shared, OS-MODPITCH)") {
  // the formula VoicePool::mod_block had before the helper (kept here as the reference)
  const auto old_step = [](std::int32_t& q16, std::int32_t tgt, std::uint32_t& left, std::uint32_t ahead) {
    if (left == 0u) {
      return;
    }
    if (left <= ahead) {
      q16 = tgt;
      left = 0;
    } else {
      q16 += (tgt - q16) * static_cast<std::int32_t>(ahead) / static_cast<std::int32_t>(left);
      left -= ahead;
    }
  };
  const std::int32_t qs[] = {0, 65536, -7 * 65536, 127 * 65536, -127 * 65536, 4096 * 255, -4096 * 255, 12345, -54321};
  const std::int32_t ts[] = {0, 4096 * 16, -4096 * 255, 65536, 5 * 65536};
  const std::uint32_t lefts[] = {1, 2, 63, 64, 65, 882, 5514, 8820, 661500};
  const std::uint32_t aheads[] = {1, 20, 44, 63, 64};
  std::uint32_t checked = 0;
  for (std::int32_t q0 : qs) {
    for (std::int32_t t : ts) {
      for (std::uint32_t l0 : lefts) {
        for (std::uint32_t a : aheads) {
          std::int32_t q = q0, r_q;
          std::uint32_t l = l0;
          audio::PitchRamp r{q0, t, l0};
          for (int k = 0; k < 6; ++k) {  // a few evaluations, the first with `a` samples ahead and the rest with whole blocks
            const std::uint32_t ah = k == 0 ? a : 64u;
            old_step(q, t, l, ah);
            audio::ramp_advance(r, ah);
            r_q = r.q16;
            CHECK(q == r_q);
            CHECK(l == r.left);
            ++checked;
          }
        }
      }
    }
  }
  CHECK(checked > 5000u);
  // and the constants: a Slide step at 120 BPM, a Glide time, the tick rate
  CHECK(audio::slide_target_q16(16) == 65536);
  CHECK(audio::slide_target_q16(-255) == -255 * 4096);
  CHECK(audio::glide_samples(200) == 8820u);
  CHECK(audio::glide_samples(0) == 1u);
  const std::uint32_t step = audio::slide_step_samples(audio::tick_rate_q24(12000));
  CHECK((step > 5500u && step < 5530u));  // 0.125 s at 44.1 kHz
  CHECK(audio::slide_step_samples(audio::tick_rate_q24(6000)) > 2u * step - 4u);  // half the tempo, twice the time
  CHECK(audio::kTicksPerStep == 576u);
}

TEST_CASE("module pitch: a Slide of 16 reaches +1 semitone after one step at 120 BPM, within a block, one pitch() call a block, and holds (module of ABI 1.8)") {
  PRig r(kFollow);
  cue_note(0, 0, 0, 60);
  cue_slide(5, 0, 0, true, 16);
  r.run(100);
  NEED(g_pc.size() > 80u);
  CHECK(!r.fs.bad);
  // the note: plain, flags 0 (no FX yet), no pitch call before the Slide
  CHECK(in_block(0, 'N').size() == 1u);
  CHECK(in_block(0, 'N')[0].flags == 0u);
  for (std::uint32_t b = 0; b < 5; ++b) {
    CHECK(in_block(b, 'P').empty());
  }
  // the Slide on the sounding note (a row without a note): from where it is (0) to +1, starting in block 5; no new note, no retrigger
  CHECK(count_kind('N') == 1u);
  CHECK(count_kind('F') == 0u);
  const std::vector<PCall> first = in_block(5, 'P');
  NEED(first.size() == 1u);
  CHECK(first[0].semis == 0.0f);
  CHECK((first[0].per_frame > 0.0f && first[0].per_frame < 0.001f));  // 1 semitone over 5514 samples, a stretch of 64
  // monotone, never past the target; one call a block while it moves; it has landed within 87 blocks
  std::uint32_t landed = 0;
  float prev = 0.0f;
  for (std::uint32_t b = 5; b < 100; ++b) {
    CHECK(in_block(b).size() <= 1u);
    CHECK(r.off_end[b] >= prev - 1e-6f);
    CHECK(r.off_end[b] <= 1.0f + 1e-5f);
    prev = r.off_end[b];
    if (landed == 0u && std::fabs(r.off_end[b] - 1.0f) < 1e-4f) {
      landed = b - 5u + 1u;
    }
  }
  CHECK_MESSAGE((landed >= 86u && landed <= 87u), "landed after " << landed << " blocks (5514 samples is 86.2 blocks)");
  // settled: the block after it lands tells the module the target with no slope, once; then nothing at all while it holds
  const std::uint32_t settle = 5u + landed;
  CHECK(in_block(settle, 'P').size() == 1u);
  CHECK(in_block(settle, 'P')[0].semis == 1.0f);
  CHECK(in_block(settle, 'P')[0].per_frame == 0.0f);
  for (std::uint32_t b = settle + 1; b < 100; ++b) {
    CHECK(in_block(b).empty());
    CHECK(r.off_end[b] == 1.0f);
  }
}

TEST_CASE("module pitch: Glide 60 to 67 with value 10 starts at -7.0 and lands on 0 after 200 ms, as a legato note: no note-off, LEGATO | RAMP (module of ABI 1.8)") {
  PRig r(kFollow);
  cue_note(0, 0, 0, 60);   // the first note: a Glide FX is not in force yet
  cue_glide(4, 0, 10);
  cue_note(4, 0, 0, 67);   // the FX comes with the note, before it
  r.run(160);
  NEED(count_kind('N') == 2u);
  CHECK(!r.fs.bad);
  CHECK(count_kind('F') == 0u);  // no release: the envelope is not restarted
  CHECK(in_block(0, 'N')[0].flags == 0u);
  const std::vector<PCall> n = in_block(4, 'N');
  NEED(n.size() == 1u);
  CHECK(n[0].note == 67u);
  CHECK(n[0].flags == (O3M_NOTE_LEGATO | O3M_NOTE_RAMP));
  const std::vector<PCall> p = in_block(4, 'P');
  NEED(p.size() == 1u);
  CHECK(p[0].semis == -7.0f);
  CHECK(p[0].per_frame > 0.0f);
  // the order within the block: note_on, then pitch()
  std::size_t at_n = 0, at_p = 0;
  for (std::size_t i = 0; i < g_pc.size(); ++i) {
    if (g_pc[i].block == 4u) {
      (g_pc[i].kind == 'N' ? at_n : at_p) = i;
    }
  }
  CHECK(at_n < at_p);
  // 200 ms is 8820 samples: 137.8 blocks. Halfway it is at about -3.5; it lands in block 4 + 137; the offset never overshoots 0
  CHECK_MESSAGE(std::fabs(r.off_end[4 + 68] - (-7.0f * (1.0f - 69.0f * 64.0f / 8820.0f))) < 0.02f, "after 69 blocks: " << r.off_end[4 + 68]);
  std::uint32_t landed = 0;
  for (std::uint32_t b = 4; b < 160; ++b) {
    CHECK(r.off_end[b] <= 1e-5f);
    CHECK(in_block(b, 'P').size() <= 1u);
    if (landed == 0u && std::fabs(r.off_end[b]) < 1e-4f) {
      landed = b - 4u + 1u;
    }
  }
  CHECK_MESSAGE(landed == 138u, "landed after " << landed << " blocks");
  CHECK(in_block(4 + landed, 'P').size() == 1u);  // the settle call: 0 with no slope
  CHECK(in_block(4 + landed, 'P')[0].semis == 0.0f);
  CHECK(in_block(4 + landed + 1u).empty());
}

TEST_CASE("module pitch: a Slide beats a Glide on the same track; a Slide of 0 is no slide; the Glide FX ends with its ModEnd (module of ABI 1.8)") {
  {
    PRig r(kFollow);
    cue_note(0, 0, 0, 60);
    cue_glide(3, 0, 10);
    cue_slide(3, 0, 0, true, 32);  // the Slide (+2 semitones) and the Glide (from 60) are both in force for the next note
    cue_note(3, 0, 0, 67);
    r.run(120);
    NEED(count_kind('N') == 2u);
    const std::vector<PCall> p = in_block(3, 'P');
    NEED(p.size() >= 1u);
    CHECK(p.back().semis == 0.0f);  // the Slide starts at the note's own pitch, not at -7 (the Glide's start)
    CHECK(in_block(3, 'N')[0].flags == (O3M_NOTE_LEGATO | O3M_NOTE_RAMP));
    CHECK(std::fabs(r.off_end[119] - 2.0f) < 1e-5f);  // and goes to +2 (32 sixteenths)
    CHECK(!r.fs.bad);
  }
  {
    PRig r(kFollow);  // a Slide of 0 changes nothing: the note is an ordinary, retriggering one
    cue_note(0, 0, 0, 60);
    cue_slide(2, 0, 0, true, 0);
    cue_note(3, 0, 0, 62);
    r.run(20);
    CHECK(count_kind('P') == 0u);
    CHECK(count_kind('F') == 1u);  // the sequencer's note releases the held one
    CHECK(in_block(3, 'N')[0].flags == 0u);
  }
  {
    PRig r(kFollow);  // the Glide FX ended before the note: an ordinary note
    cue_note(0, 0, 0, 60);
    cue_glide(1, 0, 10);
    cue_glide_end(2, 0);
    cue_note(3, 0, 0, 67);
    r.run(20);
    CHECK(count_kind('P') == 0u);
    CHECK(in_block(3, 'N')[0].flags == 0u);
    CHECK(count_kind('F') == 1u);
  }
  {
    PRig r(kFollow);  // a Glide of value 0 is no glide either
    cue_note(0, 0, 0, 60);
    cue_glide(1, 0, 0);
    cue_note(3, 0, 0, 67);
    r.run(20);
    CHECK(count_kind('P') == 0u);
    CHECK(in_block(3, 'N')[0].flags == 0u);
  }
}

TEST_CASE("module pitch: the end of the Slide (ModEnd) and a reset (FxReset) put the note's own pitch back with one call, walking or landed; and nothing is called when nothing is held") {
  {  // the FX ends while the ramp walks
    PRig r(kFollow);
    cue_note(0, 0, 0, 60);
    cue_slide(2, 0, 0, true, 64);  // +4 over 86 blocks
    cue_slide_end(20, 0);
    r.run(60);
    CHECK(r.off_end[19] > 0.5f);
    const std::vector<PCall> end = in_block(20, 'P');
    NEED(end.size() == 1u);  // exactly one call in the block of the ModEnd (the ramp's own stretch is dropped with the ramp: the ModEnd is dispatched before the block's render)
    CHECK(end[0].semis == 0.0f);
    CHECK(end[0].per_frame == 0.0f);
    CHECK(r.off_end[20] == 0.0f);
    for (std::uint32_t b = 21; b < 60; ++b) {
      CHECK(in_block(b).empty());
    }
  }
  {  // landed and held at +1, then the FX ends
    PRig r(kFollow);
    cue_note(0, 0, 0, 60);
    cue_slide(1, 0, 0, true, 16);
    cue_slide_end(100, 0);
    r.run(110);
    CHECK(r.off_end[99] == 1.0f);
    NEED(in_block(100, 'P').size() == 1u);
    CHECK(in_block(100, 'P')[0].semis == 0.0f);
    CHECK(r.off_end[100] == 0.0f);
    CHECK(in_block(101).empty());
  }
  {  // a second ModEnd with the offset already at 0: no call
    PRig r(kFollow);
    cue_note(0, 0, 0, 60);
    cue_slide(1, 0, 0, true, 16);
    cue_slide_end(100, 0);
    cue_slide_end(102, 0);
    r.run(110);
    CHECK(in_block(102).empty());
  }
  {  // FxReset (the transport starts) ends every FX: a ramp in progress snaps, and the next note is an ordinary one
    PRig r(kFollow);
    cue_note(0, 0, 0, 60);
    cue_slide(2, 0, 0, true, 64);
    cue_reset(10);
    cue_note(12, 0, 0, 62);
    r.run(30);
    NEED(in_block(10, 'P').size() == 1u);
    CHECK(in_block(10, 'P')[0].semis == 0.0f);
    CHECK(r.off_end[10] == 0.0f);
    CHECK(in_block(12, 'N')[0].flags == 0u);  // no Slide in force any more
    CHECK(in_block(12, 'P').empty());
    CHECK(in_block(11).empty());
  }
  {  // nothing held: the Slide FX of a track whose note has ended makes no call, and its end makes none
    PRig r(kFollow);
    cue_note(0, 0, 0, 60);
    cue_off(1, 0);
    cue_slide(3, 0, 0, true, 32);
    cue_slide_end(6, 0);
    r.run(10);
    CHECK(count_kind('P') == 0u);
  }
  {  // the note ends in the middle of a ramp: the calls stop with it
    PRig r(kFollow);
    cue_note(0, 0, 0, 60);
    cue_slide(1, 0, 0, true, 64);
    cue_off(10, 0);
    r.run(40);
    CHECK(in_block(9, 'P').size() == 1u);
    for (std::uint32_t b = 10; b < 40; ++b) {
      CHECK(in_block(b, 'P').empty());
    }
  }
}

TEST_CASE("module pitch: a Slide started at 60 BPM lasts twice as long as at 120, and the length of a ramp already running does not change with the tempo (module of ABI 1.8)") {
  const auto landed_after = [](std::uint32_t tempo_at_start, std::uint32_t tempo_later) {
    PRig r(kFollow);
    g_ptempo = tempo_at_start;
    cue_note(0, 0, 0, 60);
    cue_slide(2, 0, 0, true, 16);
    r.run(3);
    g_ptempo = tempo_later;  // the engine reads the tempo from the source once a block
    r.run(297);
    std::uint32_t landed = 0;
    for (std::uint32_t b = 2; b < 300; ++b) {
      if (landed == 0u && std::fabs(r.off_end[b] - 1.0f) < 1e-4f) {
        landed = b - 2u + 1u;
      }
    }
    CHECK(!r.fs.bad);
    return landed;
  };
  const std::uint32_t at120 = landed_after(12000, 12000);
  const std::uint32_t at60 = landed_after(6000, 6000);
  CHECK((at120 >= 86u && at120 <= 87u));
  CHECK((at60 >= 172u && at60 <= 174u));
  CHECK(landed_after(12000, 6000) == at120);  // the tempo changed after the ramp began: it keeps its length
}

TEST_CASE("module pitch: a Slide or Glide note is legato (no note-off, no retrigger), a note on a silent instance starts with RAMP only, a tie does nothing, a plain note after it releases first (module of ABI 1.8)") {
  PRig r(kFollow);
  cue_note(0, 0, 0, 60);              // 1: no FX in force: an ordinary note (flags 0)
  cue_slide(10, 0, 0, true, 16);      // the Slide (FX in force from now)
  cue_note(10, 0, 0, 62);             // 2: a Slide note over the sounding one: LEGATO | RAMP, no note-off
  cue_note(130, 0, 0, 62);            // 3: the same note again under the Slide: a tie, nothing at all
  cue_slide_end(200, 0);              // the FX ends ...
  cue_note(200, 0, 0, 64);            // 4: ... a plain note: the held note is released first, flags 0
  cue_off(210, 0);                    // 5: the note is released
  cue_slide(220, 0, 0, false, 32);    // a Slide down is in force again
  cue_note(221, 0, 0, 65);            // 6: nothing sounds: RAMP only, and the ramp runs (-2 semitones over a step)
  r.run(400);
  NEED(count_kind('N') == 4u);
  const std::vector<PCall> notes = [&] {
    std::vector<PCall> v;
    for (const PCall& c : g_pc) {
      if (c.kind == 'N' || c.kind == 'F') {
        v.push_back(c);
      }
    }
    return v;
  }();
  // N60(0) N62(L|R) [tie: nothing] F62 N64(0) F64 N65(R)
  NEED(notes.size() == 6u);
  CHECK((notes[0].kind == 'N' && notes[0].note == 60u && notes[0].flags == 0u));
  CHECK((notes[1].kind == 'N' && notes[1].note == 62u && notes[1].flags == (O3M_NOTE_LEGATO | O3M_NOTE_RAMP) && notes[1].block == 10u));
  CHECK((notes[2].kind == 'F' && notes[2].note == 62u && notes[2].block == 200u));
  CHECK((notes[3].kind == 'N' && notes[3].note == 64u && notes[3].flags == 0u && notes[3].block == 200u));
  CHECK((notes[4].kind == 'F' && notes[4].note == 64u && notes[4].block == 210u));
  CHECK((notes[5].kind == 'N' && notes[5].note == 65u && notes[5].flags == O3M_NOTE_RAMP && notes[5].block == 221u));
  // the tie (block 130): nothing was called, and the Slide stayed where it was (landed at +1 since block 97)
  CHECK(in_block(130).empty());
  CHECK(r.off_end[130] == 1.0f);
  // the plain note: its ModEnd put the offset back (one call in block 200: the ModEnd's pitch(0, 0)), and the new note started from 0
  CHECK(r.off_end[200] == 0.0f);
  // the note from silence: the ramp runs, down 2 semitones
  const std::vector<PCall> p = in_block(221, 'P');
  NEED(p.size() == 1u);
  CHECK(p[0].semis == 0.0f);
  CHECK(p[0].per_frame < 0.0f);
  CHECK(std::fabs(r.off_end[399] - (-2.0f)) < 1e-4f);
  CHECK(!r.fs.bad);
}

TEST_CASE("module pitch: a module of ABI 1.7, and a 1.8 module without pitch, never see O3M_NOTE_RAMP, a Slide-made legato or a pitch call, and play exactly as without the FX") {
  const auto song = [](bool with_fx) {
    g_pcues.clear();
    cue_note(0, 0, 0, 60);
    if (with_fx) {
      cue_slide(5, 0, 0, true, 32);
      cue_glide(5, 0, 20);
    }
    cue_note(6, 0, 0, 62);
    cue_note(100, 20, 0, 62);
    if (with_fx) {
      cue_slide_end(150, 0);
      cue_reset(160);
    }
    cue_note(170, 0, 0, 55);
    cue_off(180, 0);
  };
  for (const o3m_module* mod : {&kOld, &kNoPitch}) {
    std::vector<PCall> log[2];
    std::vector<std::vector<std::int16_t>> audio_out[2];
    for (int fx_on = 0; fx_on < 2; ++fx_on) {
      PRig r(*mod);
      song(fx_on != 0);
      r.run(200);
      log[fx_on] = g_pc;
      audio_out[fx_on] = r.feed;
      CHECK(count_kind('P') == 0u);
      CHECK(!g_old_pitch_called);
      for (const PCall& c : g_pc) {
        CHECK(((c.flags & O3M_NOTE_RAMP) == 0u && (c.flags & O3M_NOTE_LEGATO) == 0u));  // a pattern's note: neither flag
      }
    }
    NEED(log[0].size() == log[1].size());
    for (std::size_t i = 0; i < log[0].size(); ++i) {
      CHECK((log[0][i].kind == log[1][i].kind && log[0][i].block == log[1][i].block && log[0][i].note == log[1][i].note && log[0][i].flags == log[1][i].flags));
    }
    NEED(audio_out[0].size() == audio_out[1].size());
    bool same = true;
    bool sounded = false;
    for (std::size_t b = 0; b < audio_out[0].size(); ++b) {
      same = same && audio_out[0][b] == audio_out[1][b];  // bit for bit, block by block
      sounded = sounded || !audio_out[0][b].empty();
    }
    CHECK(same);
    CHECK(sounded);
  }
}

TEST_CASE("module pitch: whatever the song does, every pitch() call is finite and within +-128, and a steady ramp costs at most one call a block (random events)") {
  std::uint32_t seed = 0x1234567u;
  const auto rnd = [&seed](std::uint32_t n) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed % n;
  };
  for (int round = 0; round < 6; ++round) {
    PRig r(kFollow);
    g_ptempo = 3000u + rnd(30000u);
    for (std::uint32_t b = 0; b < 400; ++b) {
      if (rnd(7) == 0) {
        cue_note(b, rnd(64), rnd(3), rnd(2) == 0 ? rnd(128) : 60 + rnd(24));
      }
      if (rnd(11) == 0) {
        cue_slide(b, rnd(64), rnd(3), rnd(2) == 0, rnd(256));
      }
      if (rnd(13) == 0) {
        cue_glide(b, rnd(3), rnd(102));
      }
      if (rnd(17) == 0) {
        cue_slide_end(b, rnd(3));
      }
      if (rnd(19) == 0) {
        cue_glide_end(b, rnd(3));
      }
      if (rnd(23) == 0) {
        cue_off(b, rnd(3));
      }
      if (rnd(97) == 0) {
        cue_reset(b);
      }
    }
    r.run(400);
    CHECK(!r.fs.bad);
    for (const PCall& c : g_pc) {
      if (c.kind == 'P') {
        CHECK((std::isfinite(c.semis) && std::isfinite(c.per_frame) && std::fabs(c.semis) <= 128.0f && std::fabs(c.per_frame) <= 128.0f));
      }
    }
    for (float o : r.off_end) {
      CHECK((std::isfinite(o) && std::fabs(o) <= 130.0f));
    }
  }
}

TEST_CASE("module pitch: a Slide on a note that follows an earlier ramp starts from the note's own pitch, not from where the old ramp stopped (module of ABI 1.8)") {
  PRig r(kFollow);
  cue_note(0, 0, 0, 60);
  cue_slide(1, 0, 0, true, 64);  // +4 over a step ...
  cue_off(10, 0);                // ... the note ends in the middle of it (the module sits at about +0.5)
  cue_slide_end(11, 0);
  cue_note(12, 0, 0, 62);        // an ordinary note (the FX ended)
  cue_slide(14, 0, 0, true, 16); // a Slide on the sounding note
  r.run(40);
  CHECK(r.off_end[9] > 0.4f);
  const std::vector<PCall> p = in_block(14, 'P');
  NEED(p.size() == 1u);
  CHECK(p[0].semis == 0.0f);  // from the note's pitch
  CHECK(p[0].per_frame > 0.0f);
}

TEST_CASE("module pitch: a ramping note that starts at the very end of a block (offset 63, 64) calls pitch() with finite numbers, no division by zero samples (review: ahead 0)") {
  const unsigned offsets[] = {62, 63, 64, 65, 500};
  for (unsigned off : offsets) {
    CAPTURE(off);
    PRig r(kFollow);
    cue_note(0, 0, 0, 60);
    cue_glide(1, 0, 50);  // 50: a long glide
    cue_note(3, off, 0, 67);
    cue_slide(6, off, 0, true, 64);
    r.run(20);
    CHECK(!r.fs.bad);  // every number the module was given was finite and within +-128
    CHECK(count_kind('N') == 2u);
    for (const PCall& c : g_pc) {
      if (c.kind == 'P') {
        CHECK(std::isfinite(c.semis));
        CHECK(std::isfinite(c.per_frame));
      }
    }
    CHECK(!in_block(3, 'P').empty());
  }
}

TEST_CASE("module pitch: a Slide ModSet and a ramping note in the same block make two pitch() calls and the last one wins; the module ends where the ramp goes (module of ABI 1.8)") {
  PRig r(kFollow);
  cue_note(0, 0, 0, 60);
  cue_slide(2, 0, 0, true, 32);  // the Slide row arrives ...
  cue_note(2, 0, 0, 64);         // ... with a note in the same step: a legato ramping note over the sounding one
  r.run(10);
  CHECK(!r.fs.bad);
  const std::vector<PCall> p = in_block(2, 'P');
  NEED(p.size() == 2u);  // the ModSet's ramp, then the note's (documented: OS-MODPITCH.md Risks)
  const std::vector<PCall> n = in_block(2, 'N');
  NEED(n.size() == 1u);
  CHECK((n[0].flags & O3M_NOTE_RAMP) != 0u);
  CHECK(r.off_end[2] == doctest::Approx(p[1].semis + p[1].per_frame * 64.0f).epsilon(1e-4));  // the last call's line, nothing of the first one
}

// ---- the whole chain: a pattern's Slide, Glide, Arp and Chord steps, through the Sequencer, the Engine and ModuleVoices, to a module of ABI 1.8 ----------------------------------------

namespace {

struct SeqPRig {
  alignas(16) std::uint8_t scratch[ModuleVoices::kScratchBytes];
  alignas(16) FState fs;
  audio::Engine* engine = nullptr;
  audio::VoicePool* pool = nullptr;
  ModuleVoices* mv = nullptr;
  seq::Sequencer* seq = nullptr;
  haudio::BlockReport report{};
  alignas(8) std::int16_t in[haudio::kBlockSamples] = {};
  alignas(8) std::int16_t out[haudio::kBlockSamples] = {};
  std::vector<float> off_end;

  SeqPRig(const o3m_module& mod, void (*build)()) {
    static test::StaticSlot<audio::Engine> engine_slot;
    static test::StaticSlot<audio::VoicePool> pool_slot;
    static test::StaticSlot<ModuleVoices> mv_slot;
    static test::StaticSlot<seq::Sequencer> seq_slot;
    static audio::TapMem taps;
    static SeqEvent events[audio::kMaxEventsPerBlock];
    test::fresh_project(4, 1);
    model::Project& p = test::seq_project();
    CHECK(p.add_module("follow", 6, 1, 8) == model::Status::Ok);
    model::Instrument m;
    model::init_instrument(m, model::InstrumentKind::Module);
    m.module_index = 0;
    CHECK(p.set_instrument(kModInstr - 1u, m) == model::Status::Ok);
    CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
    CHECK(p.create_pattern(1, 16) == model::Status::Ok);
    build();
    seq = &seq_slot.make(p);
    engine = &engine_slot.make(&p_cycles, taps);
    pool = &pool_slot.make();
    mv = &mv_slot.make();
    std::memset(scratch, 0, sizeof scratch);
    std::memset(&fs, 0, sizeof fs);
    CHECK(mv->attach_scratch(scratch, sizeof scratch));
    engine->attach_voices(pool);
    engine->attach_modules(mv);
    engine->attach_events(seq::event_source(*seq, events, audio::kMaxEventsPerBlock));
    CHECK(mv->bind(kModInstr - 1u, mod, 1, &fs, nullptr, 0) == ModuleVoices::Bind::Ok);
    g_pc.clear();
    g_pblock = 0;
    g_old_pitch_called = false;
  }
  ~SeqPRig() {
    mv->retire_all();
    block();
    block();
    mv->release_all();
  }
  void block() {
    report = haudio::BlockReport{};
    const haudio::BlockIo io{in, out, 0, &report};
    engine->render_block(io);
    off_end.push_back(fs.off);
    ++g_pblock;
  }
  void run(std::uint32_t blocks) {
    for (std::uint32_t i = 0; i < blocks; ++i) {
      block();
    }
  }
};

void song_slide_glide_arp() {
  model::Project& p = test::seq_project();
  const auto set = [&p](std::uint32_t row, const model::Step& s) { CHECK(p.set_step(1, 0, row, s) == model::Status::Ok); };
  set(0, test::make_step(60, kModInstr));                                            // an ordinary note
  set(1, test::make_step(67, kModInstr, fx::kGlide, 10));                            // Glide: legato from 60, -7.0 -> 0 in 200 ms
  set(2, test::make_step(67, kModInstr, fx::kGlide, 10));                            // the same note under the Glide: a tie
  set(3, test::make_step(62, kModInstr));                                            // plain: the Glide ends, the held note is released first
  set(4, test::make_step(64, kModInstr, fx::kSlideUp, 16));                          // Slide: legato, 0 -> +1 in a step
  set(6, test::make_step(60, kModInstr, fx::kChord, 4, fx::kArp, 7));                // Chord Min + Arp: retriggered notes 60 63 67 60 ...
  set(8, test::make_step(50, kModInstr));                                            // plain: the arpeggio ends
}

}  // namespace

TEST_CASE("module pitch: a pattern's Glide, Slide, Chord and Arp steps reach a module of ABI 1.8 as legato ramping notes, a tie, and retriggered notes (event stream)") {
  SeqPRig r(kFollow, &song_slide_glide_arp);
  r.seq->play_pattern(1);
  r.run(9 * 87 + 10);
  struct Want {
    char kind;
    unsigned note, flags, row;
  };
  const unsigned L = O3M_NOTE_LEGATO | O3M_NOTE_RAMP;
  const Want want[] = {{'N', 60, 0, 0},  {'N', 67, L, 1},  /* row 2: a tie */ {'F', 67, 0, 3}, {'N', 62, 0, 3}, {'N', 64, L, 4},
                       {'F', 64, 0, 6},  {'N', 60, 0, 6},  /* the arp: */ {'F', 60, 0, 6}, {'N', 63, 0, 6}, {'F', 63, 0, 7}, {'N', 67, 0, 7}, {'F', 67, 0, 7}, {'N', 60, 0, 7},
                       {'F', 60, 0, 8},  {'N', 50, 0, 8}};
  std::vector<PCall> got;
  for (const PCall& c : g_pc) {
    if (c.kind != 'P') {
      got.push_back(c);
    }
  }
  NEED(got.size() == sizeof want / sizeof want[0]);
  for (std::size_t i = 0; i < got.size(); ++i) {
    CAPTURE(i);
    CHECK(got[i].kind == want[i].kind);
    CHECK(got[i].note == want[i].note);
    if (got[i].kind == 'N') {
      CHECK(got[i].flags == want[i].flags);
    }
    // the row it fell in: a step at 120 BPM is 86.13 blocks, the arp's notes are half a step apart
    const double row = static_cast<double>(got[i].block) / 86.13;
    CHECK_MESSAGE((row >= want[i].row - 0.02 && row < want[i].row + 1.0), "event " << i << " in block " << got[i].block << " (row " << row << ")");
  }
  // the Glide: right after the legato note, pitch(-7.0) with a slope; the Slide: from 0
  bool glide_start = false, slide_start = false;
  for (const PCall& c : g_pc) {
    glide_start = glide_start || (c.kind == 'P' && c.semis == -7.0f && c.block >= 86u && c.block <= 88u);
    slide_start = slide_start || (c.kind == 'P' && c.semis == 0.0f && c.per_frame > 0.0f && c.block >= 344u && c.block <= 346u);
  }
  CHECK(glide_start);
  CHECK(slide_start);
  CHECK(!r.fs.bad);
  // and nothing else asked the module to ramp: the plain notes and the arpeggio are ordinary
  for (const PCall& c : g_pc) {
    if (c.kind == 'P') {
      CHECK(c.block < 530u);
    }
  }
}
