// SPDX-License-Identifier: GPL-3.0-or-later
//
// An export of an unchanged project is byte-identical (docs/specs/OS-WAVEXP.md, "Determinism"; QA-PRF-128 proves "X never reaches an export" with two exports and cmp). The claims, each through
// the whole path of the unit's export (ExportSource over seq::OfflineRender over the Engine, onto an in-memory card), the engine staying alive between the files as it does on the unit:
//   * three exports of the song in a row are the same bytes, with the delay and the reverb sending (the reverb's LFO and the delay's state are the same at the start of every file);
//   * the same with the random LFO shapes on the filter, the pan, the finetune and the amp of the instruments (the generator draws from its seed in every pass), and with Chance and Random note FX
//     in the pattern (the sequencer is built afresh from its seed);
//   * the same after a live Play that stopped with the voices and the effects still ringing;
//   * the same for a Pattern request as for a Song one, and for a project whose instruments use the Amiga engine, also after a live Play that left the song's LED filter on (the Amiga voices, the MED
//     track state and the LED switch start every pass clean).
// Every file is also checked to have sound in it: two silent files are the same bytes too.
// What is NOT in the claim: a module instrument keeps its instance's own memory (the TB-303's oscillator phase), which the module ABI cannot reset (seq/offline.h, A PASS).
#include <doctest/doctest.h>

#include <o3os/app/atomic_save.h>
#include <o3os/app/export_source.h>
#include <o3os/audio/amiga_voices.h>
#include <o3os/audio/demo_kit.h>
#include <o3os/audio/engine.h>
#include <o3os/audio/graph_params.h>
#include <o3os/audio/live_notes.h>
#include <o3os/audio/master_limiter.h>
#include <o3os/audio/send_delay.h>
#include <o3os/audio/send_reverb.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>
#include <o3os/model/demo.h>
#include <o3os/model/project.h>
#include <o3os/seq/engine_link.h>
#include <o3os/seq/offline.h>
#include <o3os/seq/sequencer.h>

#include "support/mem_card.h"
#include "support/need.h"
#include "support/save_source.h"
#include "support/static_slot.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

namespace {

namespace haudio = o3os::hal::audio;
using namespace o3os;

std::uint32_t fake_cycles_counter = 0;
std::uint32_t fake_cycles() noexcept { return fake_cycles_counter += 11u; }

model::ModelArena& arena() {
  static model::ModelArena a;
  return a;
}

void build_project(model::Project& project, bool chance, bool amiga = false) {
  project.reset();
  for (const std::uint32_t id : {1u, 2u}) {
    NEED(project.create_pattern(id, 16) == model::Status::Ok);
    NEED(project.append_order(id) == model::Status::Ok);
  }
  NEED(model::demo::add_instruments(project) == model::Status::Ok);
  NEED(model::demo::fill_pattern(project, 1) == model::Status::Ok);
  NEED(model::demo::fill_pattern(project, 2) == model::Status::Ok);
  if (chance) {
    for (const std::uint32_t id : {1u, 2u}) {
      for (unsigned t = 0; t < 3; ++t) {
        for (unsigned s = 0; s < 16; ++s) {
          model::Step st = project.pattern(id)->steps[t][s];
          if (st.note == model::kNoteEmpty) {
            continue;
          }
          st.fx[0] = {4, 50};  // Chance 50 %
          st.fx[1] = {5, 12};  // Random note
          NEED(project.set_step(id, t, s, st) == model::Status::Ok);
        }
      }
    }
  }
  if (amiga) {
    // MED commands in the FX column (128 + the command) that outlive a pass: a slide up on the kick of pattern 1 that nothing ends, and the LED filter switched on (0F F9) late in pattern 2, after the
    // last pattern's own sound has begun: a pass that started with the LED still on, or with the slide still held, would not be the first pass's.
    model::Step slide = project.pattern(1)->steps[0][0];
    slide.fx[0] = {129, 2};
    NEED(project.set_step(1, 0, 0, slide) == model::Status::Ok);
    model::Step led = project.pattern(2)->steps[0][12];
    led.fx[0] = {143, 0xF9};
    NEED(project.set_step(2, 0, 12, led) == model::Status::Ok);
  }
}

struct Rig {
  Rig() : engine(&fake_cycles, taps), live(arena().project), render(wiring()) { live.attach_order_store(live_order); }
  seq::OfflineRender::Wiring wiring() {
    seq::OfflineRender::Wiring w;
    w.engine = &engine;
    w.project = &arena().project;
    w.live = &live;
    w.events = export_events;
    w.event_cap = audio::kMaxEventsPerBlock;
    w.make_sequencer = &Rig::make_export_sequencer;
    w.make_ctx = this;
    return w;
  }
  static seq::Sequencer* make_export_sequencer(void* ctx) noexcept {
    Rig& r = *static_cast<Rig*>(ctx);
    seq::Sequencer* s = new (r.export_seq_storage) seq::Sequencer(arena().project);
    s->attach_order_store(r.export_order);
    return s;
  }
  audio::TapMem taps;
  audio::Engine engine;
  audio::LiveBox live_box;
  audio::VoicePool pool;
  audio::AmigaVoices amiga;
  seq::Sequencer live;
  seq::OrderStore live_order;    // (E-SONG-07: the sequencers' copies of the song list)
  seq::OrderStore export_order;
  audio::SeqEvent live_events[audio::kMaxEventsPerBlock];
  audio::SeqEvent export_events[audio::kMaxEventsPerBlock];
  alignas(seq::Sequencer) unsigned char export_seq_storage[sizeof(seq::Sequencer)];
  audio::MasterLimiter limiter;
  audio::SendDelay delay;
  audio::SendReverb reverb;
  std::uint32_t delay_mem[audio::SendDelay::kRingFrames];
  std::int16_t reverb_mem[audio::SendReverb::kMemSamples];
  audio::DemoKit kit;
  std::int16_t kit_pcm[40000];
  seq::OfflineRender render;
  alignas(8) std::int16_t in[haudio::kBlockSamples] = {};
  alignas(8) std::int16_t out[haudio::kBlockSamples] = {};
  haudio::BlockReport report;
};

struct Feat {
  bool sends = true;
  int only = 0;  // 1 delay only, 2 reverb only
  bool flt_random = false;
  bool flt_tri = false;
  bool pan_random = false;
  bool tune_random = false;
  bool amp_random = false;
  bool amiga = false;  // the first three sounds (kick, snare, hat) play on the Amiga engine, the bass on the Modern one
};

Rig& make_rig(const Feat& f) {
  static o3os::test::StaticSlot<Rig> slot;
  Rig& r = slot.make();
  CHECK(audio::generate_demo_kit(r.kit_pcm, 40000, r.kit));
  for (unsigned i = 0; i < audio::kDemoKitSounds; ++i) {
    audio::SampleDesc d = r.kit.sample[i];
    d.delay_send = static_cast<std::uint8_t>(f.sends && f.only != 2 ? 45 : 0);
    d.reverb_send = static_cast<std::uint8_t>(f.sends && f.only != 1 ? 60 : 0);
    if (f.flt_random || f.flt_tri) {
      d.filter.enable = true;
      d.filter.wanted = true;
      d.filter.type = audio::kFilterLow;
      d.filter.cutoff = 0.5f;
      d.filter.resonance = 1.5f;
      d.filter.mod.kind = audio::kFmodLfo;
      d.filter.mod.lfo = true;
      d.filter.mod.shape = f.flt_random ? 4 : 2;
      d.filter.mod.rate = 8;
      d.filter.mod.amount = 0.8f;
      d.filter.mod.lfo_amount = 0.8f;
    }
    if (f.pan_random) {
      d.pan_mod.kind = audio::kFmodLfo;
      d.pan_mod.lfo = true;
      d.pan_mod.shape = 4;
      d.pan_mod.rate = 8;
      d.pan_mod.amount = 0.8f;
      d.pan_mod.lfo_amount = 0.8f;
    }
    if (f.tune_random) {
      d.tune_mod.kind = audio::kFmodLfo;
      d.tune_mod.lfo = true;
      d.tune_mod.shape = 4;
      d.tune_mod.rate = 8;
      d.tune_mod.amount = 0.8f;
      d.tune_mod.lfo_amount = 0.8f;
    }
    if (f.amp_random) {
      d.amp_mod.kind = audio::kFmodLfo;
      d.amp_mod.lfo = true;
      d.amp_mod.shape = 4;
      d.amp_mod.rate = 8;
      d.amp_mod.amount = 0.8f;
      d.amp_mod.lfo_amount = 0.8f;
    }
    if (f.amiga && i < 3u) {
      d.engine = audio::kEngineAmiga;
    }
    r.pool.set_sample(i, d);
  }
  if (f.amiga) {
    seq::Sequencer::Config c = r.live.config();
    c.emit_ticks = true;  // the live sequencer's Config is the export's base: the Tick events the MED tick processor runs on
    r.live.configure(c);
    r.engine.attach_amiga(&r.amiga);
  }
  r.engine.attach_voices(&r.pool);
  r.engine.attach_live(&r.live_box);
  r.engine.attach_events(seq::event_source(r.live, r.live_events, audio::kMaxEventsPerBlock));
  r.delay.attach(r.delay_mem, audio::SendDelay::kRingFrames);
  r.reverb.attach(r.reverb_mem, audio::SendReverb::kMemSamples);
  r.engine.graph().install_send_fx(0, &audio::SendDelay::process_fx, &r.delay, &audio::SendDelay::param_fx);
  r.engine.graph().install_send_fx(1, &audio::SendReverb::process_fx, &r.reverb, &audio::SendReverb::param_fx);
  r.engine.graph().set_send_fx_bypass(0, false);
  r.engine.graph().set_send_fx_bypass(1, false);
  r.limiter.reset();
  r.engine.graph().install_master_limiter(r.limiter);
  return r;
}

void export_wav(Rig& r, const audio::OfflineRequest& request, std::vector<std::uint8_t>& result) {
  result.clear();
  test::MemCard card;
  alignas(4) static std::uint8_t ring[app::ExportSource::kRingBytes];
  app::ExportSource source;
  app::ExportSource::Wiring w;
  w.renderer = &r.render;
  w.ring = ring;
  w.ring_bytes = sizeof ring;
  source.attach(w);
  NEED(source.set(request));
  app::AtomicSave job;
  NEED(job.begin(card, card, "Export/T", "T_0001.wav", app::AtomicSave::Mode::NewOnly, source, "T", "EXPORT "));
  NEED(test::run(job) != 0u);
  NEED(job.done());
  const test::Bytes* file = card.get("Export/T/T_0001.wav");
  NEED(file != nullptr);
  result = *file;
}

void idle_blocks(Rig& r, unsigned n) {
  const haudio::BlockCallback cb = r.engine.callback();
  for (unsigned i = 0; i < n; ++i) {
    r.report = haudio::BlockReport{};
    const haudio::BlockIo io{r.in, r.out, 0, &r.report};
    cb.fn(cb.user, io);
  }
}

}  // namespace

namespace {

// The three files of three exports in a row (50 live blocks between them, as the main loop runs), the first from the fresh engine.
void three_exports(const Feat& f, bool chance, std::vector<std::uint8_t>* files, const audio::OfflineRequest& rq = audio::OfflineRequest{audio::OfflineRequest::What::Song, 0}) {
  build_project(arena().project, chance, f.amiga);
  Rig& r = make_rig(f);
  for (int i = 0; i < 3; ++i) {
    export_wav(r, rq, files[i]);
    idle_blocks(r, 50);
  }
}

// The largest sample of the file's PCM (16 bit, after the 44 byte header) and the number of samples that are not zero.
void measure(const std::vector<std::uint8_t>& file, int& peak, std::size_t& nonzero) {
  peak = 0;
  nonzero = 0;
  for (std::size_t i = 44u; i + 1u < file.size(); i += 2u) {
    std::int16_t v;
    std::memcpy(&v, &file[i], sizeof v);
    const int a = v < 0 ? -static_cast<int>(v) : static_cast<int>(v);
    peak = a > peak ? a : peak;
    nonzero += v != 0 ? 1u : 0u;
  }
}

constexpr int kPeakFloor = 2000;  // of 32767

void check_same(const char* name, const std::vector<std::uint8_t>* files) {
  CAPTURE(name);
  NEED(files[0].size() > 44u + 4u * 1000u);
  int peak = 0;
  std::size_t nonzero = 0;
  measure(files[0], peak, nonzero);
  CHECK(peak > kPeakFloor);                              // the sound is there: an all-silent render is the same bytes three times
  CHECK(nonzero > (files[0].size() - 44u) / 2u / 4u);    // and in more than a quarter of the samples
  CHECK(files[0] == files[1]);
  CHECK(files[1] == files[2]);
}

}  // namespace

TEST_CASE("export determinism: three exports of the song are the same bytes, with the delay and the reverb sending") {
  std::vector<std::uint8_t> f[3];
  three_exports(Feat{}, false, f);
  check_same("sends", f);
  Feat delay_only;
  delay_only.only = 1;
  three_exports(delay_only, false, f);
  check_same("delay only", f);
  Feat reverb_only;
  reverb_only.only = 2;
  three_exports(reverb_only, false, f);
  check_same("reverb only", f);
}

TEST_CASE("export determinism: the random LFO shapes (filter, pan, finetune, amp) and Chance / Random note FX give the same file every time") {
  const struct {
    const char* name;
    Feat feat;
    bool chance;
  } cases[] = {
      {"filter LFO, triangle", Feat{true, 0, false, true, false, false, false}, false},
      {"filter LFO, random", Feat{true, 0, true, false, false, false, false}, false},
      {"pan LFO, random", Feat{true, 0, false, false, true, false, false}, false},
      {"finetune LFO, random", Feat{true, 0, false, false, false, true, false}, false},
      {"amp LFO, random", Feat{true, 0, false, false, false, false, true}, false},
      {"all four random LFOs", Feat{true, 0, true, false, true, true, true}, false},
      {"Chance and Random note FX", Feat{}, true},
  };
  for (const auto& c : cases) {
    std::vector<std::uint8_t> f[3];
    three_exports(c.feat, c.chance, f);
    check_same(c.name, f);
  }
}

TEST_CASE("export determinism: a Pattern request is the same file every time, whole and as a selection of rows") {
  const audio::OfflineRequest whole{audio::OfflineRequest::What::Pattern, 1};
  audio::OfflineRequest rows = whole;
  rows.row_first = 2;
  rows.row_last = 9;
  std::vector<std::uint8_t> f[3];
  three_exports(Feat{}, false, f, whole);
  check_same("pattern, sends", f);
  three_exports(Feat{true, 0, true, false, true, true, true}, true, f, whole);
  check_same("pattern, random LFOs and Chance", f);
  three_exports(Feat{true, 0, true, false, true, true, true}, true, f, rows);
  check_same("pattern rows 2..9, random LFOs and Chance", f);
}

TEST_CASE("export determinism: a project on the Amiga engine (MED commands, the LED filter, a held slide) is the same file every time, as a song and as a pattern") {
  Feat amiga;
  amiga.amiga = true;
  std::vector<std::uint8_t> f[3];
  three_exports(amiga, false, f);
  check_same("amiga, song", f);
  three_exports(amiga, false, f, audio::OfflineRequest{audio::OfflineRequest::What::Pattern, 2});
  check_same("amiga, pattern 2 (the LED switch)", f);
  amiga.sends = false;
  three_exports(amiga, false, f);
  check_same("amiga, song, no sends", f);
}

TEST_CASE("export determinism: an Amiga project exports the same pattern from rest and after a live Play that left the song's LED filter on") {
  // Pattern 1 has no LED switch of its own (0F F9 is late in pattern 2), so the filter's state at the start of its pass is the history's: the song's LED switch, the MED track state and the voices are
  // reset with the pass (AmigaVoices::reset_pass), and a live Play of the whole song that ran past the switch must not colour the export.
  Feat amiga;
  amiga.amiga = true;
  build_project(arena().project, false, true);
  const audio::OfflineRequest pattern{audio::OfflineRequest::What::Pattern, 1};
  std::vector<std::uint8_t> rest;
  {
    Rig& r = make_rig(amiga);
    export_wav(r, pattern, rest);
  }
  Rig& r = make_rig(amiga);
  CHECK(r.live.commands().push(seq::Command{seq::Command::Kind::PlaySong, 0, 0, 0}));
  idle_blocks(r, 3500);  // past the end of the song (32 rows), the switch at row 28 included
  CHECK(r.live.commands().push(seq::Command{seq::Command::Kind::Stop, 0, 0, 0}));
  idle_blocks(r, 30);
  CHECK_FALSE(r.live.play_position().read().playing);
  CHECK(r.amiga.stats().ticks > 28u * 6u);
  CHECK(r.amiga.song_led());  // the live Play did switch the LED filter on (0F F9); without it the case would pass whatever reset_pass does
  std::vector<std::uint8_t> after;
  export_wav(r, pattern, after);
  NEED(rest.size() > 44u + 4u * 1000u);
  int peak = 0;
  std::size_t nonzero = 0;
  measure(rest, peak, nonzero);
  CHECK(peak > kPeakFloor);
  CHECK(rest == after);
}

TEST_CASE("export determinism: an export after a live Play that stopped with the voices and the effects ringing is the file of an export from rest") {
  Feat all{true, 0, true, false, true, true, true};
  build_project(arena().project, true);
  std::vector<std::uint8_t> rest;
  {
    Rig& r = make_rig(all);
    export_wav(r, audio::OfflineRequest{audio::OfflineRequest::What::Song, 0}, rest);
  }
  Rig& r = make_rig(all);
  CHECK(r.live.commands().push(seq::Command{seq::Command::Kind::PlaySong, 0, 0, 0}));
  idle_blocks(r, 900);  // (a live block is the main loop's: the audio callback of the Rig)
  CHECK(r.live.commands().push(seq::Command{seq::Command::Kind::Stop, 0, 0, 0}));
  idle_blocks(r, 30);
  CHECK_FALSE(r.live.play_position().read().playing);
  std::vector<std::uint8_t> after;
  export_wav(r, audio::OfflineRequest{audio::OfflineRequest::What::Song, 0}, after);
  NEED(rest.size() > 44u + 4u * 1000u);
  int peak = 0;
  std::size_t nonzero = 0;
  measure(rest, peak, nonzero);
  CHECK(peak > kPeakFloor);
  CHECK(rest == after);
}
