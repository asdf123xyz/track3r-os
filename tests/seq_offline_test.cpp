// SPDX-License-Identifier: GPL-3.0-or-later
//
// seq::OfflineRender (OS-WAVEXP): the song or the pattern rendered from the main loop, block by block, through the same Engine path a live Play uses. The claims:
//   * the blocks it hands over are, bit for bit, the blocks a live Play of the same project makes from the same quiet state (a pattern, a song, with the send effects and the limiter
//     installed and the instruments sending to them, so the tails are in the comparison);
//   * the engine's state before the export does not leak into it: after a live Play (the effects ringing, voices sounding) the export is still the same blocks;
//   * a second pass over the same request is the same blocks as the first (the export measures in one pass and writes in the next);
//   * the live sequencer is untouched, the engine goes back to the live render with its clock and its source, and the voices are silent;
//   * the arrangement's end is flagged inside the block it ends in, the song stops at its last pattern and the pattern after one pass, the progress rises to 999 and stays;
//   * the refusals: the recorder armed, the transport running, an empty song, no such pattern;
//   * OS-SONGSHARE S5, the slots of the song: order_first..order_last plays those slots from the first and stops at the end of the last, to the sample, with the progress counted from the first; the whole
//     range, spelled any way, is the whole song, and the full-song WAV is byte for byte the one made before the range existed (a pinned hash).
// Host only (the project is 1.9 MB, the effects' memory 0.8 MB, a run a few thousand blocks).
#include <doctest/doctest.h>

#include <o3os/app/atomic_save.h>
#include <o3os/app/export_source.h>
#include <o3os/audio/demo_kit.h>
#include <o3os/audio/engine.h>
#include <o3os/audio/graph_params.h>
#include <o3os/audio/live_notes.h>
#include <o3os/audio/master_limiter.h>
#include <o3os/audio/send_delay.h>
#include <o3os/audio/send_reverb.h>
#include <o3os/audio/voices.h>
#include <o3os/core/notify.h>
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

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <new>
#include <string>
#include <vector>

namespace {

namespace haudio = o3os::hal::audio;
using namespace o3os;

constexpr std::uint32_t kBlk = haudio::kBlock;

std::uint32_t fake_cycles_counter = 0;
std::uint32_t fake_cycles() noexcept { return fake_cycles_counter += 11u; }

model::ModelArena& arena() {
  static model::ModelArena a;
  return a;
}

// Two patterns of 16 steps (one bar at 120 BPM: 2 s, 1,378 blocks), the demo's four sounds on tracks 1 to 4, and a song of the two in order.
void build_project(model::Project& project) {
  project.reset();
  for (const std::uint32_t id : {1u, 2u}) {
    NEED(project.create_pattern(id, 16) == model::Status::Ok);
    NEED(project.append_order(id) == model::Status::Ok);
  }
  NEED(model::demo::add_instruments(project) == model::Status::Ok);
  NEED(model::demo::fill_pattern(project, 1) == model::Status::Ok);
  NEED(model::demo::fill_pattern(project, 2) == model::Status::Ok);
}

// The engine of the MK66's composition root, minus the hardware: the demo kit's voices, a sequencer, the send effects and the limiter. Built in place for every run.
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

Rig& make_rig(bool sends = true, unsigned delay_send = 45, unsigned reverb_send = 60) {
  static o3os::test::StaticSlot<Rig> slot;
  Rig& r = slot.make();
  CHECK(audio::generate_demo_kit(r.kit_pcm, 40000, r.kit));
  for (unsigned i = 0; i < audio::kDemoKitSounds; ++i) {
    audio::SampleDesc d = r.kit.sample[i];
    d.delay_send = static_cast<std::uint8_t>(sends ? delay_send : 0u);
    d.reverb_send = static_cast<std::uint8_t>(sends ? reverb_send : 0u);
    r.pool.set_sample(i, d);
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

seq::Sequencer::Config reference_config(const seq::Sequencer& s) {
  seq::Sequencer::Config c = s.config();
  c.song_loop = false;
  c.pattern_once = true;
  c.emit_clock = false;
  c.start_lead = 0;
  return c;
}

void live_block(Rig& r, std::vector<std::int16_t>* into) {
  r.report = haudio::BlockReport{};
  const haudio::BlockIo io{r.in, r.out, 0, &r.report};
  const haudio::BlockCallback cb = r.engine.callback();  // the live render line's entry (the export's request is looked at there)
  cb.fn(cb.user, io);
  if (into != nullptr) {
    into->insert(into->end(), r.out, r.out + haudio::kBlockSamples);
  }
}

// What a live Play of a pattern or the song makes from a fresh rig: `blocks` blocks, the Play command taken at the first block's start.
std::vector<std::int16_t> live_play(Rig& r, bool song, std::uint32_t blocks) {
  r.live.configure(reference_config(r.live));
  CHECK(r.live.commands().push(song ? seq::Command{seq::Command::Kind::PlaySong, 0, 0, 0} : seq::Command{seq::Command::Kind::PlayPattern, 1, 0, 0}));
  std::vector<std::int16_t> v;
  for (std::uint32_t b = 0; b < blocks; ++b) {
    live_block(r, &v);
  }
  return v;
}

struct Pass {
  std::vector<std::int16_t> pcm;      // the blocks handed over
  std::uint32_t tail_at = 0;          // the block (0-based) after which in_tail() was first true
  bool saw_tail = false;
  unsigned progress_max = 0;
  bool progress_monotonic = true;
  std::uint32_t waits = 0;
};

// Runs `render` until `blocks` blocks were handed over. The caller has open()ed it.
Pass run_pass(audio::OfflineRenderer& render, std::uint32_t blocks) {
  Pass p;
  std::int16_t out[haudio::kBlockSamples];
  unsigned last = 0;
  std::uint32_t spins = 0;
  while (p.pcm.size() < static_cast<std::size_t>(blocks) * haudio::kBlockSamples) {
    const audio::OfflineRenderer::Step s = render.next(out);
    if (s == audio::OfflineRenderer::Step::Failed) {
      CHECK_MESSAGE(false, "the render failed: " << render.failure());
      break;
    }
    if (s == audio::OfflineRenderer::Step::Wait) {
      ++p.waits;
      if (++spins >= 100000u) {
        CHECK_MESSAGE(false, "the render never handed a block over");
        break;
      }
      continue;
    }
    p.pcm.insert(p.pcm.end(), out, out + haudio::kBlockSamples);
    const unsigned pr = render.progress();
    p.progress_monotonic = p.progress_monotonic && pr >= last;
    last = pr;
    p.progress_max = pr > p.progress_max ? pr : p.progress_max;
    if (render.in_tail() && !p.saw_tail) {
      p.saw_tail = true;
      p.tail_at = static_cast<std::uint32_t>(p.pcm.size() / haudio::kBlockSamples) - 1u;
    }
  }
  return p;
}

std::size_t first_difference(const std::vector<std::int16_t>& a, const std::vector<std::int16_t>& b) {
  const std::size_t n = a.size() < b.size() ? a.size() : b.size();
  for (std::size_t i = 0; i < n; ++i) {
    if (a[i] != b[i]) {
      return i;
    }
  }
  return a.size() == b.size() ? ~std::size_t{0} : n;
}

std::int32_t peak_of(const std::vector<std::int16_t>& v, std::size_t from, std::size_t to) {
  std::int32_t peak = 0;
  for (std::size_t i = from; i < to && i < v.size(); ++i) {
    const std::int32_t a = v[i] < 0 ? -static_cast<std::int32_t>(v[i]) : static_cast<std::int32_t>(v[i]);
    peak = a > peak ? a : peak;
  }
  return peak;
}

constexpr audio::OfflineRequest kPattern1{audio::OfflineRequest::What::Pattern, 1};
constexpr audio::OfflineRequest kSong{audio::OfflineRequest::What::Song, 0};

}  // namespace

TEST_CASE("offline: a pattern rendered offline is, bit for bit, the blocks a live Play makes from the same quiet state (effects and limiter in, tail included)") {
  build_project(arena().project);
  constexpr std::uint32_t kBlocks = 2400;  // the bar is 1,378 blocks; the rest is the end of the pattern's tail
  std::vector<std::int16_t> reference;
  {
    Rig& r = make_rig();
    reference = live_play(r, false, kBlocks);
  }
  Rig& r = make_rig();
  CHECK(r.render.open(kPattern1));
  const Pass p = run_pass(r.render, kBlocks);
  CHECK(first_difference(reference, p.pcm) == ~std::size_t{0});
  CHECK(peak_of(reference, 0, reference.size()) > 1000);  // the comparison is of sound, not of silence
  CHECK(peak_of(reference, 0, reference.size()) < 32767);
  CHECK(r.render.sequencer().stats().queue_overflow == 0u);
  r.render.close();
}

TEST_CASE("offline: the whole song, and the song's end is flagged inside the block it ends in") {
  build_project(arena().project);
  constexpr std::uint32_t kBlocks = 3800;  // two bars (2,756 blocks) and a tail
  std::vector<std::int16_t> reference;
  {
    Rig& r = make_rig();
    reference = live_play(r, true, kBlocks);
  }
  Rig& r = make_rig();
  CHECK(r.render.open(kSong));
  const Pass p = run_pass(r.render, kBlocks);
  CHECK(first_difference(reference, p.pcm) == ~std::size_t{0});
  NEED(p.saw_tail);
  // Two bars of 16 steps at 120 BPM: 8 beats of 0.5 s end at 4 s of the engine's clock, 176,470.6 samples at 44,117.65 Hz (block 2,757, counted from 0; audio_demo_test: bar 3 starts in
  // block 2757 at sample 23) and 176,400 at 44,100 Hz (block 2,756).
  CHECK(p.tail_at == static_cast<std::uint32_t>(4.0 * haudio::sample_rate_mhz() / 1000.0 / kBlk));
  CHECK(r.render.in_tail());
  CHECK(r.render.progress() == 999u);
  CHECK(p.progress_monotonic);
  CHECK(p.progress_max == 999u);
  r.render.close();
}

TEST_CASE("offline: what the engine held before does not leak: after a live Play (voices and effects ringing) the export is the same blocks") {
  build_project(arena().project);
  constexpr std::uint32_t kBlocks = 2400;
  std::vector<std::int16_t> reference;
  {
    Rig& r = make_rig();
    reference = live_play(r, false, kBlocks);
  }
  Rig& r = make_rig();
  // A live Play of pattern 2 for 700 blocks, stopped in the middle of it, and the blocks right after (the stop's release, the delay and the reverb still ringing).
  CHECK(r.live.commands().push(seq::Command{seq::Command::Kind::PlayPattern, 2, 0, 0}));
  for (std::uint32_t b = 0; b < 700; ++b) {
    live_block(r, nullptr);
  }
  CHECK(r.live.commands().push(seq::Command{seq::Command::Kind::Stop, 0, 0, 0}));
  for (std::uint32_t b = 0; b < 30; ++b) {
    live_block(r, nullptr);
  }
  CHECK_FALSE(r.live.play_position().read().playing);
  CHECK(r.pool.voices_in_use() <= 1u);
  const std::uint64_t clock_before = r.engine.sample_clock();
  // The live sequencer's own cursor (it keeps it for a resume) is not what the export reads.
  CHECK(r.render.open(kPattern1));
  const Pass p = run_pass(r.render, kBlocks);
  CHECK(first_difference(reference, p.pcm) == ~std::size_t{0});
  r.render.close();
  // The live engine got its sample clock back, its source, and quiet voices; the live sequencer stayed stopped.
  CHECK(r.engine.sample_clock() == clock_before);
  CHECK_FALSE(r.engine.offline_wanted());
  CHECK_FALSE(r.engine.offline_entered());
  CHECK(r.pool.voices_in_use() == 0u);
  CHECK_FALSE(r.live.play_position().read().playing);
  live_block(r, nullptr);
  CHECK(r.engine.sample_clock() == clock_before + kBlk);  // the live render runs again, on its own source
}

TEST_CASE("offline: a second pass over the same request is the same blocks as the first, the engine staying with the export between them") {
  build_project(arena().project);
  constexpr std::uint32_t kBlocks = 2400;
  Rig& r = make_rig();
  CHECK(r.render.open(kPattern1));
  const Pass first = run_pass(r.render, kBlocks);
  const std::uint32_t settle_1 = first.waits;
  CHECK(r.engine.offline_entered());
  CHECK(r.render.open(kPattern1));  // again, without a close(): the engine stays
  CHECK(r.engine.offline_entered());
  const Pass second = run_pass(r.render, kBlocks);
  CHECK(first_difference(first.pcm, second.pcm) == ~std::size_t{0});
  CHECK(second.waits == seq::OfflineRender::kSettleBlocks);  // the settle of the second pass
  CHECK(settle_1 == seq::OfflineRender::kSettleBlocks + 1u);  // ... and the first one's, after the one call that took the engine (the audio is not running: nothing to wait for)
  r.render.close();
  CHECK_FALSE(r.engine.offline_entered());
}

TEST_CASE("offline: close() is idempotent and safe before anything ran; the live render is parked between request and enter, and takes the engine back") {
  build_project(arena().project);
  Rig& r = make_rig(false);
  r.render.close();  // nothing was opened
  CHECK_FALSE(r.engine.offline_wanted());
  CHECK(r.render.open(kPattern1));
  CHECK(r.engine.offline_wanted());
  CHECK_FALSE(r.engine.offline_entered());
  // A live block while the request stands: digital silence, nothing rendered (the clock does not move).
  const std::uint64_t clock = r.engine.sample_clock();
  std::memset(r.out, 0x55, sizeof r.out);
  live_block(r, nullptr);
  for (std::uint32_t i = 0; i < haudio::kBlockSamples; ++i) {
    CHECK(r.out[i] == 0);
  }
  CHECK(r.engine.sample_clock() == clock);
  CHECK(r.engine.offline_ready(true));  // the render has seen the request
  r.render.close();
  r.render.close();
  CHECK_FALSE(r.engine.offline_wanted());
  live_block(r, nullptr);
  CHECK(r.engine.sample_clock() == clock + kBlk);
}

TEST_CASE("offline: the refusals - the recorder armed, the transport running, an empty song, a pattern that cannot be") {
  build_project(arena().project);
  Rig& r = make_rig(false);
  CHECK(r.render.open(audio::OfflineRequest{audio::OfflineRequest::What::Pattern, 0}) == false);
  CHECK(std::strcmp(r.render.failure(), "NO SUCH PATTERN") == 0);
  CHECK(r.render.open(audio::OfflineRequest{audio::OfflineRequest::What::Pattern, 256}) == false);
  CHECK_FALSE(r.engine.offline_wanted());

  // the transport is running
  CHECK(r.live.commands().push(seq::Command{seq::Command::Kind::PlayPattern, 1, 0, 0}));
  live_block(r, nullptr);
  live_block(r, nullptr);
  CHECK(r.live.play_position().read().playing);
  CHECK_FALSE(r.render.open(kPattern1));
  CHECK(std::strcmp(r.render.failure(), "STOP THE TRANSPORT FIRST") == 0);
  CHECK_FALSE(r.engine.offline_wanted());
  CHECK(r.live.commands().push(seq::Command{seq::Command::Kind::Stop, 0, 0, 0}));
  live_block(r, nullptr);

  // the recorder is armed
  static std::int16_t take[4096];
  CHECK(r.engine.graph().rec().arm(take, 4096, 2));
  CHECK_FALSE(r.render.open(kPattern1));
  CHECK(std::strcmp(r.render.failure(), "THE RECORDER IS ARMED") == 0);
  r.engine.graph().rec().disarm();
  live_block(r, nullptr);
  r.engine.graph().rec().release();

  // an empty song
  build_project(arena().project);
  CHECK(arena().project.clear_order() == model::Status::Ok);
  CHECK_FALSE(r.render.open(kSong));
  CHECK(std::strcmp(r.render.failure(), "THE SONG IS EMPTY") == 0);
  CHECK_FALSE(r.engine.offline_wanted());
}

TEST_CASE("offline: a pattern plays once and stops, a song stops at its last pattern; the live sequencer and its resume point are never touched") {
  build_project(arena().project);
  Rig& r = make_rig(false);
  CHECK(r.render.open(kPattern1));
  const Pass p = run_pass(r.render, 1500);
  NEED(p.saw_tail);
  CHECK(p.tail_at == static_cast<std::uint32_t>(2.0 * haudio::sample_rate_mhz() / 1000.0 / kBlk));  // 16 steps of a sixteenth at 120 BPM: 2 s, block 1,378 at either rate
  CHECK_FALSE(r.render.sequencer().playing());
  CHECK(r.live.stats().steps == 0u);  // the live sequencer was never stepped by the export
  CHECK(r.live.stats().commands == 0u);
  r.render.close();
}

// ---- OS-COREFX and the live pads after an export ---------------------------------------------------------------------------------------------------

// The tracker FX of OS-COREFX on the demo pattern: a channel volume (58) and a channel pan (57) on the first steps, an arpeggio (46) on the bass.
void add_tracker_fx(model::Project& project) {
  const auto put = [&](unsigned track, unsigned step, unsigned slot, std::uint8_t type, std::uint8_t value) {
    NEED(project.pattern(1) != nullptr);
    model::Step s = project.pattern(1)->steps[track][step];
    s.fx[slot] = model::StepFx{type, value};
    NEED(project.set_step(1, track, step, s) == model::Status::Ok);
  };
  put(0, 0, 0, 58, 24);    // channel volume 24/64 on the kick's track, held
  put(2, 0, 1, 57, 10);    // channel pan 10 on the hat's track, held
  put(3, 3, 0, 46, 0x37);  // an arpeggio on the bass's second note
  put(1, 4, 0, 47, 0x04);  // a volume slide down on the snare
}

// A live pad note after two idle blocks (a cut sampler voice fades out within the first block after an export: that residual is gone by the second).
std::vector<std::int16_t> live_pad_note(Rig& r, std::uint32_t blocks) {
  live_block(r, nullptr);
  live_block(r, nullptr);
  CHECK(r.live_box.queue.push(audio::LiveEvent{audio::LiveKind::On, audio::kLiveSourcePad, 0, 60, 1, 100, 0, 0}));
  std::vector<std::int16_t> v;
  for (std::uint32_t b = 0; b < blocks; ++b) {
    live_block(r, &v);
  }
  return v;
}

TEST_CASE("offline: the tracker FX of OS-COREFX (channel volume and pan, an arpeggio, a volume slide) render offline as a live Play does, bit for bit") {
  build_project(arena().project);
  add_tracker_fx(arena().project);
  constexpr std::uint32_t kBlocks = 2000;
  std::vector<std::int16_t> reference;
  {
    Rig& r = make_rig(false);
    reference = live_play(r, false, kBlocks);
  }
  Rig& r = make_rig(false);
  CHECK(r.render.open(kPattern1));
  const Pass p = run_pass(r.render, kBlocks);
  CHECK(first_difference(reference, p.pcm) == ~std::size_t{0});
  r.render.close();
  // and the FX did something: the same pattern without them is another sound
  build_project(arena().project);
  Rig& plain = make_rig(false);
  const std::vector<std::int16_t> without = live_play(plain, false, kBlocks);
  CHECK(first_difference(reference, without) != ~std::size_t{0});
}

TEST_CASE("offline: an export cancelled in the middle of a pattern leaves the engine as it was: a live pad note is the same as without any export (with and without the tracker FX in force)") {
  for (const bool with_fx : {false, true}) {
    CAPTURE(with_fx);
    build_project(arena().project);
    if (with_fx) {
      add_tracker_fx(arena().project);  // channel volume 24/64 and a channel pan are in force when the cancel comes
    }
    std::vector<std::int16_t> fresh;
    {
      Rig& r = make_rig(false);
      fresh = live_pad_note(r, 60);
    }
    Rig& r = make_rig(false);
    CHECK(r.render.open(kPattern1));
    (void)run_pass(r.render, 300);
    r.render.close();  // a cancel: no Stopped event, no end of the pattern
    const std::vector<std::int16_t> after = live_pad_note(r, 60);
    CHECK(peak_of(fresh, 0, fresh.size()) > 1000);
    CHECK(first_difference(fresh, after) == ~std::size_t{0});
  }
}

// ---- OS-EXPNOTE: a held live note is stopped at the start of the export, not a reason to refuse it ------------------------------------------------

namespace {

audio::LiveEvent midi_on(std::uint8_t note, std::uint8_t channel) {
  return audio::LiveEvent{audio::LiveKind::On, audio::kLiveSourceMidi, note, note, 1, 100, 0, channel};
}
audio::LiveEvent pad_on(std::uint8_t pad, std::uint8_t note) {
  return audio::LiveEvent{audio::LiveKind::On, audio::kLiveSourcePad, pad, note, 1, 100, 0, 0};
}

// The blocks of an export of pattern 1 (`blocks` of them) with the live events `held` sounding when it is opened ("" in `text` when none was stopped).
std::vector<std::int16_t> export_with_held(std::initializer_list<audio::LiveEvent> held, std::string* text, bool* ended_live_idle = nullptr) {
  build_project(arena().project);
  Rig& r = make_rig(false);
  for (const audio::LiveEvent& e : held) {
    CHECK(r.live_box.queue.push(e));
  }
  live_block(r, nullptr);
  live_block(r, nullptr);
  CHECK(r.engine.live_held() == held.size());
  CHECK(r.render.open(kPattern1));  // no refusal: the held note is no reason
  const Pass p = run_pass(r.render, 300);
  *text = r.render.stopped();
  CHECK(r.engine.live_held() == 0u);  // the table forgot the notes
  r.render.close();
  if (ended_live_idle != nullptr) {
    *ended_live_idle = r.engine.live_idle();
  }
  return p.pcm;
}

}  // namespace

TEST_CASE("offline: a MIDI note-on that never got its note-off (channel 10) does not block the export: it is stopped, named, and the render is the one of a quiet engine") {
  std::string none;
  const std::vector<std::int16_t> reference = export_with_held({}, &none);
  CHECK(none.empty());
  std::string text;
  bool idle = false;
  const std::vector<std::int16_t> held = export_with_held({midi_on(48, 10)}, &text, &idle);
  CHECK(text == "STOPPED MIDI CH10 C-4");
  CHECK(text.size() <= o3os::core::kNotifyTextChars);
  CHECK(idle);
  CHECK(first_difference(reference, held) == ~std::size_t{0});  // the note does not leak into the file
  CHECK(peak_of(held, 0, held.size()) > 1000);
}

TEST_CASE("offline: a held pad is named as the pad's note; several held notes name the oldest and count the others; a sharp, a one-digit channel and the tenth octave fit") {
  std::string text;
  (void)export_with_held({pad_on(3, 60)}, &text);
  CHECK(text == "STOPPED PAD C-5");
  (void)export_with_held({pad_on(3, 61), midi_on(50, 2), midi_on(51, 16)}, &text);
  CHECK(text == "STOPPED PAD C#5 +2");
  (void)export_with_held({midi_on(127, 16)}, &text);
  CHECK(text == "STOPPED MIDI CH16 G10");
  (void)export_with_held({midi_on(121, 1)}, &text);
  CHECK(text == "STOPPED MIDI CH1 c10");
  (void)export_with_held({midi_on(0, 0)}, &text);  // a channel the unit did not give: the note alone
  CHECK(text == "STOPPED MIDI C-0");
  std::string longest;
  (void)export_with_held({midi_on(121, 16), midi_on(1, 1), midi_on(2, 1), midi_on(3, 1), midi_on(4, 1), midi_on(5, 1), midi_on(6, 1), midi_on(7, 1), midi_on(8, 1), midi_on(9, 1), midi_on(10, 1), midi_on(11, 1)}, &longest);
  CHECK(longest == "STOPPED MIDI CH16 c10 +11");
  CHECK(longest.size() <= o3os::core::kNotifyTextChars);
}

TEST_CASE("offline: a note on that arrives during the export is not in the file; a late note-off for a stopped note does nothing, before or after the engine is back") {
  build_project(arena().project);
  std::vector<std::int16_t> reference;
  {
    Rig& r = make_rig(false);
    CHECK(r.render.open(kPattern1));
    reference = run_pass(r.render, 300).pcm;
    r.render.close();
  }
  Rig& r = make_rig(false);
  CHECK(r.live_box.queue.push(midi_on(48, 10)));
  live_block(r, nullptr);
  CHECK(r.render.open(kPattern1));
  std::vector<std::int16_t> got;
  std::int16_t out[haudio::kBlockSamples];
  bool pushed = false;
  while (got.size() < 300u * haudio::kBlockSamples) {
    const audio::OfflineRenderer::Step s = r.render.next(out);
    NEED(s != audio::OfflineRenderer::Step::Failed);
    if (s == audio::OfflineRenderer::Step::Wait) {
      continue;
    }
    got.insert(got.end(), out, out + haudio::kBlockSamples);
    if (!pushed && got.size() > 100u * haudio::kBlockSamples) {  // mid-export: the stopped note's off arrives (the platform drops a new note on while the export runs: here none is pushed)
      pushed = true;
      CHECK(r.live_box.queue.push(audio::LiveEvent{audio::LiveKind::Off, audio::kLiveSourceMidi, 48, 0, 0, 0, 0, 0}));
    }
  }
  CHECK(pushed);
  CHECK(std::string(r.render.stopped()) == "STOPPED MIDI CH10 C-4");
  CHECK(first_difference(reference, got) == ~std::size_t{0});
  r.render.close();
  // The engine is back: the off queued during the export is played now and finds no note; so do the late ones, and the unit is as quiet as without any of it.
  CHECK(r.engine.live_held() == 0u);
  CHECK(r.live_box.queue.push(audio::LiveEvent{audio::LiveKind::Off, audio::kLiveSourceMidi, 48, 0, 0, 0, 0, 0}));
  CHECK(r.live_box.queue.push(audio::LiveEvent{audio::LiveKind::Off, audio::kLiveSourcePad, 5, 0, 0, 0, 0, 0}));
  CHECK(r.live_box.queue.push(audio::LiveEvent{audio::LiveKind::AllOff, audio::kLiveAllSources, 0, 0, 0, 0, 0, 0}));
  std::vector<std::int16_t> quiet;
  live_block(r, nullptr);  // (the cut voices' residual fades within two blocks)
  live_block(r, nullptr);
  for (int b = 0; b < 4; ++b) {
    live_block(r, &quiet);
  }
  CHECK(peak_of(quiet, 0, quiet.size()) == 0);
  CHECK(r.engine.live_held() == 0u);
  CHECK(r.engine.live_idle());
  // and a new note works as ever
  const std::vector<std::int16_t> note = live_pad_note(r, 60);
  CHECK(peak_of(note, 0, note.size()) > 1000);
}

TEST_CASE("offline: a note on that was pushed just before the export, and not yet taken by the render, is stopped and named, and does not start after the export") {
  build_project(arena().project);
  Rig& r = make_rig(false);
  CHECK(r.live_box.queue.push(pad_on(2, 61)));  // never taken: no live block runs before the request
  CHECK(r.render.open(kPattern1));
  (void)run_pass(r.render, 20);
  CHECK(std::string(r.render.stopped()) == "STOPPED PAD C#5");
  r.render.close();
  CHECK(r.live_box.queue.empty());
  std::vector<std::int16_t> quiet;
  live_block(r, nullptr);
  live_block(r, nullptr);
  for (int b = 0; b < 4; ++b) {
    live_block(r, &quiet);
  }
  CHECK(peak_of(quiet, 0, quiet.size()) == 0);
  CHECK(r.engine.live_held() == 0u);
}

// ---- End to end: the file against a live Play --------------------------------------------------------------------------------------------------

TEST_CASE("export end to end: the WAV of a pattern and of the song is what a live Play of the same project renders, through the rate converter, and its tempo is the live one") {
  for (const bool song : {false, true}) {
    CAPTURE(song);
    build_project(arena().project);
    constexpr std::uint32_t kLiveBlocks = 8200;  // the song is 2,757 blocks, then the 5.1 s of silence that ends the tail (3,500 blocks), and room
    std::vector<std::int16_t> live;
    {
      Rig& r = make_rig(false);  // (no sends: the tail is the voices' own, so that the test stays short)
      live = live_play(r, song, kLiveBlocks);
    }
    Rig& r = make_rig(false);
    test::MemCard card;
    alignas(4) static std::uint8_t ring[app::ExportSource::kRingBytes];
    app::ExportSource source;
    app::ExportSource::Wiring w;
    w.renderer = &r.render;
    w.ring = ring;
    w.ring_bytes = sizeof ring;
    source.attach(w);
    audio::OfflineRequest request = song ? kSong : kPattern1;
    NEED(source.set(request));
    app::AtomicSave job;
    NEED(job.begin(card, card, "Export/T", "T_0001.wav", app::AtomicSave::Mode::NewOnly, source, "T", "EXPORT "));
    NEED(test::run(job) != 0u);
    NEED(job.done());
    CHECK(card.violations == 0);
    const test::Bytes* file = card.get("Export/T/T_0001.wav");
    NEED(file != nullptr);
    NEED(file->size() > 44u + 4u * 1000u);
    const std::uint32_t frames = static_cast<std::uint32_t>((file->size() - 44u) / 4u);

    // The reference: the live blocks through the same converter, frame by frame (the file is the first `frames` of it).
    app::RateConverter conv;
    conv.reset();
    std::vector<std::int16_t> want;
    std::int16_t out[app::RateConverter::kMaxOutFrames * 2u];
    for (std::uint32_t b = 0; b < kLiveBlocks && want.size() < static_cast<std::size_t>(frames) * 2u; ++b) {
      const std::uint32_t made = conv.feed(live.data() + static_cast<std::size_t>(b) * haudio::kBlockSamples, kBlk, out);
      want.insert(want.end(), out, out + made * 2u);
    }
    NEED(want.size() >= static_cast<std::size_t>(frames) * 2u);
    std::size_t bad = 0;
    std::size_t first_bad = ~std::size_t{0};
    for (std::size_t i = 0; i < static_cast<std::size_t>(frames) * 2u; ++i) {
      const std::int16_t got = static_cast<std::int16_t>((*file)[44 + 2 * i] | ((*file)[44 + 2 * i + 1] << 8));
      if (got != want[i]) {
        ++bad;
        first_bad = first_bad == ~std::size_t{0} ? i : first_bad;
      }
    }
    CHECK_MESSAGE(bad == 0u, "the first sample that differs: " << first_bad << " of " << frames * 2u);
    CHECK(peak_of(want, 0, static_cast<std::size_t>(frames) * 2u) > 1000);

    // Tempo and pitch are the live ones: the arrangement ends where the live sequencer says (2 s for the pattern, 4 s for the song at 120 BPM) and the file runs on to the last sound.
    const double seconds = static_cast<double>(frames) / 44100.0;
    CHECK(seconds > (song ? 4.0 : 2.0) - 0.01);
    CHECK(seconds < (song ? 4.0 : 2.0) + 1.5);
    // the first kick: its onset in the file is the live onset, scaled by the rates, to a frame
    const auto onset = [](const std::vector<std::int16_t>& v) {
      for (std::size_t i = 0; i + 1 < v.size(); i += 2) {
        if (std::abs(static_cast<int>(v[i])) > 500) {
          return i / 2;
        }
      }
      return std::size_t{0};
    };
    std::vector<std::int16_t> file_pcm(static_cast<std::size_t>(frames) * 2u);
    std::memcpy(file_pcm.data(), file->data() + 44, file_pcm.size() * sizeof(std::int16_t));
    const double live_onset_s = static_cast<double>(onset(live)) / (haudio::sample_rate_mhz() / 1000.0);
    const double file_onset_s = static_cast<double>(onset(file_pcm)) / 44100.0;
    CHECK(std::fabs(live_onset_s - file_onset_s) < 3.0 / 44100.0);
    CHECK_FALSE(r.engine.offline_wanted());  // the engine was given back
  }
}

// ---- OS-WAVEXP slice B: stems and the selection ----------------------------------------------------------------------------------------------------

namespace {

namespace gp = o3os::audio::graph_param;

void push_param(Rig& r, core::ParamId id, float v) { CHECK(r.engine.params().push(audio::ParamChange{id, v, 1u})); }

// One request, open to close (or left open when `close` is false: the engine stays with the export for the next pass).
std::vector<std::int16_t> offline(Rig& r, const audio::OfflineRequest& rq, std::uint32_t blocks, bool close = true, Pass* pass = nullptr) {
  CHECK(r.render.open(rq));
  Pass p = run_pass(r.render, blocks);
  if (close) {
    r.render.close();
  }
  if (pass != nullptr) {
    *pass = p;
  }
  return p.pcm;
}

audio::OfflineRequest track_stem(unsigned track) {
  audio::OfflineRequest rq = kPattern1;
  rq.track_mask = static_cast<std::uint16_t>(1u << track);
  rq.dry_sends = true;
  return rq;
}

audio::OfflineRequest bus_stem(unsigned buses) {
  audio::OfflineRequest rq = kPattern1;
  rq.bus_only = true;
  rq.buses = static_cast<std::uint8_t>(buses);
  return rq;
}

}  // namespace

TEST_CASE("stems: a track stem is, bit for bit, the live render of that track soloed with the sends shut (the same chain, the limiter in)") {
  build_project(arena().project);
  constexpr std::uint32_t kBlocks = 2400;
  for (const unsigned track : {0u, 1u, 2u, 3u}) {
    CAPTURE(track);
    std::vector<std::int16_t> reference;
    {
      Rig& r = make_rig();  // (delay and reverb sends on: the stem must not carry them)
      push_param(r, audio::strip_param(track, gp::strip::Solo), 1.0f);
      push_param(r, audio::master_param(gp::master::DelayMute), 1.0f);
      push_param(r, audio::master_param(gp::master::ReverbMute), 1.0f);
      reference = live_play(r, false, kBlocks);
    }
    Rig& r = make_rig();
    const std::vector<std::int16_t> stem = offline(r, track_stem(track), kBlocks);
    CHECK(first_difference(reference, stem) == ~std::size_t{0});
    CHECK(peak_of(reference, 0, reference.size()) > 500);  // the comparison is of sound, not of silence
  }
}

TEST_CASE("stems: the stem set is deterministic - the same stem twice, with the plain MIX between them and the engine staying with the export, is the same blocks, and the MIX is the plain MIX") {
  build_project(arena().project);
  constexpr std::uint32_t kBlocks = 2400;
  std::vector<std::int16_t> mix_reference;
  {
    Rig& r = make_rig();
    mix_reference = live_play(r, false, kBlocks);
  }
  Rig& r = make_rig();
  const std::vector<std::int16_t> first = offline(r, track_stem(2), kBlocks, false);
  CHECK(r.engine.offline_entered());
  const std::vector<std::int16_t> mix = offline(r, kPattern1, kBlocks, false);
  CHECK(first_difference(mix_reference, mix) == ~std::size_t{0});  // the mixer is the project's again for a plain pass after a stem
  const std::vector<std::int16_t> bus = offline(r, bus_stem(1), kBlocks, false);
  const std::vector<std::int16_t> again = offline(r, track_stem(2), kBlocks, true);
  CHECK(first_difference(first, again) == ~std::size_t{0});
  CHECK(first_difference(first, mix) != ~std::size_t{0});
  CHECK(first_difference(bus, mix) != ~std::size_t{0});
  CHECK_FALSE(r.engine.offline_entered());
  // and a whole set again from the top, as a second export would (a different engine run, the same project)
  Rig& r2 = make_rig();
  CHECK(first_difference(first, offline(r2, track_stem(2), kBlocks)) == ~std::size_t{0});
}

TEST_CASE("stems: the project's mixer is the graph's again when the render is closed - a user mute and a solo survive a stem export, a cancelled one and a failed one") {
  build_project(arena().project);
  constexpr std::uint32_t kBlocks = 700;
  model::TrackSlot muted = *arena().project.track(3);
  muted.flags = model::kTrackMute;
  NEED(arena().project.set_track(3, muted) == model::Status::Ok);
  std::vector<std::int16_t> reference;
  {
    Rig& r = make_rig(false);  // (no sends: the send effects clear a slice a block after an export, their returns silent meanwhile - that is not what this is about)
    push_param(r, audio::strip_param(3, gp::strip::Mute), 1.0f);  // (what the app told the graph when the user muted it)
    live_block(r, nullptr);  // (two idle blocks first, as after an export: a cut sampler voice fades out within the first block after one)
    live_block(r, nullptr);
    reference = live_play(r, false, kBlocks);
  }
  CHECK(peak_of(reference, 0, reference.size()) > 500);
  for (const unsigned mode : {0u, 1u, 2u}) {  // 0 finished, 1 cancelled in the middle, 2 the stem of the muted track itself
    CAPTURE(mode);
    Rig& r = make_rig(false);  // (no sends: the send effects clear a slice a block after an export, their returns silent meanwhile - that is not what this is about)
    push_param(r, audio::strip_param(3, gp::strip::Mute), 1.0f);
    CHECK(r.render.open(mode == 2u ? track_stem(3) : bus_stem(2)));
    const Pass p = run_pass(r.render, mode == 1u ? 200u : 900u);
    if (mode == 2u) {
      CHECK(peak_of(p.pcm, 0, p.pcm.size()) > 500);  // a track stem is the track alone: the project's mute does not silence it
    }
    r.render.close();
    live_block(r, nullptr);
    live_block(r, nullptr);
    CHECK(first_difference(reference, live_play(r, false, kBlocks)) == ~std::size_t{0});
  }
}

TEST_CASE("stems: the bus stems carry the returns only - silent without sends, and a sound that is not in the dry tracks with them") {
  build_project(arena().project);
  constexpr std::uint32_t kBlocks = 2400;
  {
    Rig& r = make_rig(false);  // no instrument sends
    for (const unsigned buses : {1u, 2u}) {
      const std::vector<std::int16_t> v = offline(r, bus_stem(buses), kBlocks);
      CHECK(peak_of(v, 0, v.size()) <= 2);
    }
  }
  Rig& r = make_rig();
  const std::vector<std::int16_t> delay = offline(r, bus_stem(1), kBlocks);
  const std::vector<std::int16_t> reverb = offline(r, bus_stem(2), kBlocks);
  const std::vector<std::int16_t> mix = offline(r, kPattern1, kBlocks);
  CHECK(peak_of(delay, 0, delay.size()) > 200);
  CHECK(peak_of(reverb, 0, reverb.size()) > 200);
  // no dry sound: the first step's kick is in the mix's first blocks and in neither return (the delay's repeats come later, the reverb's pre-delay and build-up too)
  CHECK(peak_of(mix, 0, 20 * haudio::kBlockSamples) > 2000);
  CHECK(peak_of(delay, 0, 20 * haudio::kBlockSamples) < peak_of(mix, 0, 20 * haudio::kBlockSamples) / 4);
  CHECK(peak_of(reverb, 0, 20 * haudio::kBlockSamples) < peak_of(mix, 0, 20 * haudio::kBlockSamples) / 4);
}

TEST_CASE("stems: the stems add up to the MIX within a few LSB while the limiter is idle (the sends are linear)") {
  build_project(arena().project);
  constexpr std::uint32_t kBlocks = 2400;
  Rig& r = make_rig();
  const std::vector<std::int16_t> mix = offline(r, kPattern1, kBlocks);
  std::vector<std::int32_t> sum(mix.size(), 0);
  const auto add = [&](const std::vector<std::int16_t>& v) {
    for (std::size_t i = 0; i < v.size() && i < sum.size(); ++i) {
      sum[i] += v[i];
    }
  };
  for (unsigned t = 0; t < 4u; ++t) {
    add(offline(r, track_stem(t), kBlocks));
  }
  add(offline(r, bus_stem(1), kBlocks));
  add(offline(r, bus_stem(2), kBlocks));
  std::int32_t worst = 0;
  for (std::size_t i = 0; i < mix.size(); ++i) {
    const std::int32_t d = std::abs(sum[i] - mix[i]);
    worst = d > worst ? d : worst;
  }
  const std::int32_t peak = peak_of(mix, 0, mix.size());
  CHECK(peak > 2000);
  CHECK(peak < 30000);  // the limiter's ceiling is 30,200: below it the base limiter does not act
  // Measured: 0 LSB (the sums are exact in the accumulators, the effects are linear). Two LSB is allowed for the six stems rounding on their own; anything near a percent of the peak would be a mixing error.
  CHECK_MESSAGE(worst <= 2, "worst sum-of-stems error: " << worst << " LSB at a peak of " << peak);
}

TEST_CASE("selection: the rows play from the first to the last, the start is sharp, the transport stops at the end of the last row to the sample, nothing after it starts") {
  build_project(arena().project);
  constexpr std::uint32_t kBlocks = 1000;
  audio::OfflineRequest rq = kPattern1;
  rq.row_first = 4;
  rq.row_last = 7;
  rq.track_mask = 0x000F;
  Pass p;
  std::vector<std::int16_t> sel;
  {
    Rig& r = make_rig();
    sel = offline(r, rq, kBlocks, true, &p);
  }
  NEED(p.saw_tail);
  // four steps of a sixteenth at 120 BPM: half a second
  CHECK(p.tail_at == static_cast<std::uint32_t>(0.5 * haudio::sample_rate_mhz() / 1000.0 / kBlk));
  CHECK(p.progress_monotonic);
  CHECK(p.progress_max == 999u);
  // up to the block the last row ends in, it is the live Play of the same pattern from step 4, bit for bit (the start is at the sample, with no earlier row's sound)
  std::vector<std::int16_t> live;
  {
    Rig& r = make_rig();
    r.live.configure(reference_config(r.live));
    CHECK(r.live.commands().push(seq::Command{seq::Command::Kind::PlayPattern, 1, 0, 4}));
    for (std::uint32_t b = 0; b < kBlocks; ++b) {
      live_block(r, &live);
    }
  }
  const std::size_t cut = static_cast<std::size_t>(p.tail_at) * haudio::kBlockSamples;
  CHECK(first_difference(std::vector<std::int16_t>(live.begin(), live.begin() + static_cast<std::ptrdiff_t>(cut)), std::vector<std::int16_t>(sel.begin(), sel.begin() + static_cast<std::ptrdiff_t>(cut))) == ~std::size_t{0});
  CHECK(peak_of(sel, 0, 4 * haudio::kBlockSamples) > 2000);  // the first row's kick and snare sound at once
  // the live Play goes on with step 8's kick, snare-less hat and bass; the export has only the rows' own tails: far quieter after the cut
  const std::size_t after = (static_cast<std::size_t>(p.tail_at) + 60u) * haudio::kBlockSamples;
  CHECK(peak_of(live, after, live.size()) > 2000);
  CHECK(peak_of(sel, after, sel.size()) * 3 < peak_of(live, after, live.size()));
}

TEST_CASE("selection: the edges - one row, the last row, a last row past the pattern, a first row past it, a last row before the first") {
  build_project(arena().project);
  constexpr std::uint32_t kBlocks = 1500;
  const auto sel = [](unsigned first, unsigned last) {
    audio::OfflineRequest rq = kPattern1;
    rq.row_first = static_cast<std::uint16_t>(first);
    rq.row_last = static_cast<std::uint16_t>(last);
    return rq;
  };
  const auto tail_of = [&](Rig& r, const audio::OfflineRequest& rq, std::vector<std::int16_t>* pcm = nullptr) {
    Pass p;
    std::vector<std::int16_t> v = offline(r, rq, kBlocks, true, &p);
    CHECK(p.saw_tail);
    if (pcm != nullptr) {
      *pcm = v;
    }
    return p.tail_at;
  };
  const auto rows = [](double steps) { return static_cast<std::uint32_t>(steps * 0.125 * haudio::sample_rate_mhz() / 1000.0 / kBlk); };
  Rig& r = make_rig(false);
  CHECK(tail_of(r, sel(5, 5)) == rows(1));      // a single row
  CHECK(tail_of(r, sel(0, 0)) == rows(1));      // the first row alone
  CHECK(tail_of(r, sel(15, 15)) == rows(1));    // the last row of the pattern alone
  CHECK(tail_of(r, sel(0, 15)) == rows(16));    // all of it: the pattern, as without rows
  CHECK(tail_of(r, kPattern1) == rows(16));
  // a last row past the end is the end of the pattern; the same blocks as asking for it by name
  std::vector<std::int16_t> past;
  std::vector<std::int16_t> exact;
  CHECK(tail_of(r, sel(8, 999), &past) == rows(8));
  CHECK(tail_of(r, sel(8, 15), &exact) == rows(8));
  CHECK(first_difference(past, exact) == ~std::size_t{0});
  CHECK(peak_of(exact, 0, exact.size()) > 1000);
  // refusals: a first row past the pattern, a last row before the first; nothing is requested of the engine
  CHECK_FALSE(r.render.open(sel(16, 20)));
  CHECK(std::strcmp(r.render.failure(), "NO SUCH ROWS") == 0);
  CHECK_FALSE(r.render.open(sel(7, 3)));
  CHECK(std::strcmp(r.render.failure(), "NO SUCH ROWS") == 0);
  CHECK_FALSE(r.engine.offline_wanted());
}

TEST_CASE("selection: a masked request that cannot set the mixer (the ParamChange ring is full) fails and the engine is given back") {
  build_project(arena().project);
  Rig& r = make_rig(false);
  audio::OfflineRequest rq = track_stem(1);
  // The ring is full of changes nobody drains (the render is not running): the 35 changes of the mixer do not fit.
  std::uint32_t pushed = 0;
  while (r.engine.params().push(audio::ParamChange{audio::strip_param(5, gp::strip::Volume), 50.0f, 1u})) {
    ++pushed;
  }
  CHECK(pushed > 100u);
  CHECK(r.render.open(rq));
  std::int16_t out[haudio::kBlockSamples];
  audio::OfflineRenderer::Step s = audio::OfflineRenderer::Step::Wait;
  for (unsigned i = 0; i < 8 && s == audio::OfflineRenderer::Step::Wait; ++i) {
    s = r.render.next(out);
  }
  CHECK(s == audio::OfflineRenderer::Step::Failed);
  CHECK(std::strcmp(r.render.failure(), "THE MIXER IS BUSY") == 0);
  r.render.close();
  CHECK_FALSE(r.engine.offline_entered());
}

TEST_CASE("stems: a ParamChange ring too full for the whole mixer on close() takes none of it, the project's mixer stays owed and service() pushes it when there is room - it is never lost or half told") {
  build_project(arena().project);
  model::TrackSlot muted = *arena().project.track(3);
  muted.flags = model::kTrackMute;
  NEED(arena().project.set_track(3, muted) == model::Status::Ok);
  constexpr std::uint32_t kBlocks = 700;
  std::vector<std::int16_t> reference;
  {
    Rig& r = make_rig(false);
    push_param(r, audio::strip_param(3, gp::strip::Mute), 1.0f);
    live_block(r, nullptr);
    live_block(r, nullptr);
    reference = live_play(r, false, kBlocks);
  }
  Rig& r = make_rig(false);
  push_param(r, audio::strip_param(3, gp::strip::Mute), 1.0f);
  CHECK(r.render.open(track_stem(0)));
  (void)run_pass(r.render, 100);
  // the ring fills up (nobody drains it while the export has the engine): 20 changes of room are left, 35 are needed
  while (r.engine.params().capacity() - r.engine.params().size() > 20u) {
    CHECK(r.engine.params().push(audio::ParamChange{audio::strip_param(5, gp::strip::Volume), 50.0f, 1u}));
  }
  const std::uint32_t before = r.engine.params().size();
  r.render.close();
  CHECK_FALSE(r.engine.offline_entered());
  CHECK(r.engine.params().size() <= before + 3u);  // (the three clears, perhaps) and none of the 35 mixer changes
  CHECK(r.render.restore_owed());
  r.render.service();
  CHECK(r.render.restore_owed());  // still no room
  // the live render drains the ring a few changes a block; the restore goes in as soon as there is room for all of it
  unsigned blocks = 0;
  while (r.render.restore_owed() && blocks < 400u) {
    live_block(r, nullptr);
    r.render.service();
    ++blocks;
  }
  CHECK_FALSE(r.render.restore_owed());
  CHECK(blocks > 0u);
  for (unsigned i = 0; i < 20; ++i) {
    live_block(r, nullptr);
  }
  CHECK(first_difference(reference, live_play(r, false, kBlocks)) == ~std::size_t{0});
  r.render.service();  // nothing owed: nothing happens
  CHECK_FALSE(r.render.restore_owed());
}

TEST_CASE("stems: a project solo and the master DRY, DELAY and REVERB mutes are the graph's again after a set") {
  struct Case {
    const char* name;
    unsigned solo_track;      // 0: none
    std::uint8_t master_flags;
    bool sends;
  };
  const Case cases[] = {
      {"a solo on track 2", 2, 0, false},
      {"DRY MUTE", 0, model::kMasterDryMute, false},
      {"DELAY MUTE and REVERB MUTE with the sends on", 0, static_cast<std::uint8_t>(model::kMasterDelayMute | model::kMasterReverbMute), true},
  };
  for (const Case& c : cases) {
    CAPTURE(c.name);
    build_project(arena().project);
    if (c.solo_track != 0u) {
      model::TrackSlot t = *arena().project.track(c.solo_track - 1u);
      t.flags = model::kTrackSolo;
      NEED(arena().project.set_track(c.solo_track - 1u, t) == model::Status::Ok);
    }
    model::MasterSettings m = arena().project.master();
    m.flags = c.master_flags;
    NEED(arena().project.set_master(m) == model::Status::Ok);
    const auto tell = [&](Rig& r) {  // what the app told the graph for the project's mixer
      if (c.solo_track != 0u) {
        push_param(r, audio::strip_param(c.solo_track - 1u, gp::strip::Solo), 1.0f);
      }
      push_param(r, audio::master_param(gp::master::DryMute), (c.master_flags & model::kMasterDryMute) != 0u ? 1.0f : 0.0f);
      push_param(r, audio::master_param(gp::master::DelayMute), (c.master_flags & model::kMasterDelayMute) != 0u ? 1.0f : 0.0f);
      push_param(r, audio::master_param(gp::master::ReverbMute), (c.master_flags & model::kMasterReverbMute) != 0u ? 1.0f : 0.0f);
    };
    constexpr std::uint32_t kBlocks = 700;
    std::vector<std::int16_t> reference;
    {
      Rig& r = make_rig(c.sends);
      tell(r);
      live_block(r, nullptr);
      live_block(r, nullptr);
      reference = live_play(r, false, kBlocks);
    }
    Rig& r = make_rig(c.sends);
    tell(r);
    // a set as the App makes it: the MIX, a track stem, a bus stem and the MIX, an engine hand-over for each
    (void)offline(r, kPattern1, 200);
    (void)offline(r, track_stem(0), 200);
    (void)offline(r, bus_stem(1), 200);
    (void)offline(r, kPattern1, 200);
    live_block(r, nullptr);
    live_block(r, nullptr);
    const std::vector<std::int16_t> after = live_play(r, false, kBlocks);
    CHECK(first_difference(reference, after) == ~std::size_t{0});
    if ((c.master_flags & model::kMasterDryMute) != 0u) {
      CHECK(peak_of(after, 0, after.size()) <= 2);  // DRY MUTE is back on: silence (the control that the comparison is of the right thing)
    } else {
      CHECK(peak_of(after, 0, after.size()) > 500);
    }
  }
}

// ---- OS-SONGSHARE S5: the slots of the song ----------------------------------------------------------------------------------------------------

namespace {

// The WAV a Song export writes for `request`, byte for byte, through the export source and the atomic save onto an in-memory card (the path the Export folder's file takes).
std::vector<std::uint8_t> wav_of(Rig& r, const audio::OfflineRequest& request) {
  test::MemCard card;
  alignas(4) static std::uint8_t ring[app::ExportSource::kRingBytes];
  app::ExportSource source;
  app::ExportSource::Wiring w;
  w.renderer = &r.render;
  w.ring = ring;
  w.ring_bytes = sizeof ring;
  source.attach(w);
  app::AtomicSave job;
  if (!source.set(request) || !job.begin(card, card, "Export/T", "T_0001.wav", app::AtomicSave::Mode::NewOnly, source, "T", "EXPORT ") || test::run(job) == 0u || !job.done()) {
    CHECK_MESSAGE(false, "the export did not run to its end");
    return {};
  }
  CHECK(card.violations == 0);
  const test::Bytes* file = card.get("Export/T/T_0001.wav");
  CHECK(file != nullptr);
  return file != nullptr ? *file : std::vector<std::uint8_t>{};
}

std::uint64_t fnv1a(const std::vector<std::uint8_t>& bytes) {
  std::uint64_t h = 0xCBF29CE484222325ull;
  for (const std::uint8_t b : bytes) {
    h = (h ^ b) * 0x100000001B3ull;
  }
  return h;
}

}  // namespace

namespace {

// Four slots of different lengths, so that a render of the wrong slots, or one that stops in the wrong place, shows in the timing: patterns 1 to 4 of 16, 8, 12 and 4 steps (2.0, 1.0, 1.5 and 0.5 s at
// 120 BPM), the song in that order.
constexpr unsigned kSlotSteps[4] = {16, 8, 12, 4};

void build_slots_project(model::Project& project) {
  project.reset();
  for (unsigned i = 0; i < 4u; ++i) {
    NEED(project.create_pattern(i + 1u, kSlotSteps[i]) == model::Status::Ok);
    NEED(project.append_order(i + 1u) == model::Status::Ok);
  }
  NEED(model::demo::add_instruments(project) == model::Status::Ok);
  for (unsigned id = 1; id <= 4u; ++id) {
    NEED(model::demo::fill_pattern(project, id) == model::Status::Ok);
  }
}

// The engine's block in which `steps` sixteenths end (120 BPM; the same block at either clock rate for the lengths used here).
std::uint32_t block_of(double steps) { return static_cast<std::uint32_t>(steps * 0.125 * haudio::sample_rate_mhz() / 1000.0 / kBlk); }

audio::OfflineRequest slots(unsigned first, unsigned last) {
  audio::OfflineRequest rq = kSong;
  rq.order_first = static_cast<std::uint16_t>(first);
  rq.order_last = static_cast<std::uint16_t>(last);
  return rq;
}

// What a live Play of the song makes from slot `order`, from a fresh rig.
std::vector<std::int16_t> live_song_from(Rig& r, unsigned order, std::uint32_t blocks) {
  r.live.configure(reference_config(r.live));
  CHECK(r.live.commands().push(seq::Command{seq::Command::Kind::PlaySong, 0, static_cast<std::uint16_t>(order), 0}));
  std::vector<std::int16_t> v;
  for (std::uint32_t b = 0; b < blocks; ++b) {
    live_block(r, &v);
  }
  return v;
}

}  // namespace

TEST_CASE("slots: order_first..order_last plays those slots from the first to the end of the last, the transport stops there to the sample, and the progress counts from the first") {
  build_slots_project(arena().project);
  constexpr std::uint32_t kBlocks = 4200;  // the longest range is 36 steps (4.5 s, 3,100 blocks) and a tail
  struct Case {
    unsigned first, last;
    double steps;
  };
  // Ranges that stop inside the list (the order stop is armed) and ranges that run to its end (the list's own end).
  const Case cases[] = {{1, 2, 20}, {0, 0, 16}, {1, 1, 8}, {0, 2, 36}, {2, 3, 16}, {3, 3, 4}, {0, 3, 40}};
  for (const Case& c : cases) {
    CAPTURE(c.first);
    CAPTURE(c.last);
    Pass p;
    std::vector<std::int16_t> got;
    {
      Rig& r = make_rig();
      got = offline(r, slots(c.first, c.last), kBlocks, true, &p);
    }
    NEED(p.saw_tail);
    CHECK(p.tail_at == block_of(c.steps));
    CHECK(p.progress_monotonic);
    CHECK(p.progress_max == 999u);
    std::vector<std::int16_t> live;
    {
      Rig& r = make_rig();
      live = live_song_from(r, c.first, kBlocks);
    }
    // up to the block the last slot ends in it is, bit for bit, the live Play of the song from the first slot
    const std::size_t cut = static_cast<std::size_t>(p.tail_at) * haudio::kBlockSamples;
    CHECK(first_difference(std::vector<std::int16_t>(live.begin(), live.begin() + static_cast<std::ptrdiff_t>(cut)), std::vector<std::int16_t>(got.begin(), got.begin() + static_cast<std::ptrdiff_t>(cut))) == ~std::size_t{0});
    CHECK(peak_of(got, 0, cut) > 1000);
    if (c.last + 1u < 4u) {
      // the live Play goes on into the next slot; the export has only the tail of the last one: far quieter a little after the stop
      const std::size_t after = (static_cast<std::size_t>(p.tail_at) + 120u) * haudio::kBlockSamples;
      CHECK(peak_of(live, after, after + 400u * haudio::kBlockSamples) > 2000);
      CHECK(peak_of(got, after, after + 400u * haudio::kBlockSamples) * 3 < peak_of(live, after, after + 400u * haudio::kBlockSamples));
    }
  }
}

TEST_CASE("slots: the whole range, however it is spelled, is the whole song, and the order stop is armed only inside the list") {
  build_slots_project(arena().project);
  constexpr std::uint32_t kBlocks = 3300;
  const auto render = [&](const audio::OfflineRequest& rq, unsigned expected_stop) -> std::vector<std::int16_t> {
    Rig& r = make_rig();
    if (!r.render.open(rq)) {
      CHECK_MESSAGE(false, "open() refused: " << r.render.failure());
      return {};
    }
    const Pass p = run_pass(r.render, kBlocks);
    CHECK(r.render.sequencer().config().order_stop == expected_stop);
    CHECK(r.render.sequencer().config().song_loop == false);
    r.render.close();
    return p.pcm;
  };
  const std::vector<std::int16_t> whole = render(kSong, 0);
  CHECK(peak_of(whole, 0, whole.size()) > 1000);
  CHECK(first_difference(whole, render(slots(0, 3), 0)) == ~std::size_t{0});                             // explicit
  CHECK(first_difference(whole, render(slots(0, 999), 0)) == ~std::size_t{0});                           // a last slot past the list is its end
  CHECK(first_difference(whole, render(slots(0, audio::OfflineRequest::kToEnd), 0)) == ~std::size_t{0});  // the default spelled out
  // armed: the position after the last slot; and a last slot past the list that starts later is the same blocks as naming the end
  const std::vector<std::int16_t> tail_range = render(slots(2, 3), 0);
  CHECK(first_difference(tail_range, render(slots(2, 999), 0)) == ~std::size_t{0});
  (void)render(slots(1, 2), 3);
  (void)render(slots(0, 0), 1);
}

TEST_CASE("slots: the refusals - a first slot past the list, a first slot after the last; nothing is requested of the engine") {
  build_slots_project(arena().project);
  Rig& r = make_rig(false);
  CHECK_FALSE(r.render.open(slots(4, 4)));
  CHECK(std::strcmp(r.render.failure(), "NO SUCH SLOTS") == 0);
  CHECK_FALSE(r.render.open(slots(4, 9)));
  CHECK(std::strcmp(r.render.failure(), "NO SUCH SLOTS") == 0);
  CHECK_FALSE(r.render.open(slots(3, 1)));
  CHECK(std::strcmp(r.render.failure(), "NO SUCH SLOTS") == 0);
  CHECK_FALSE(r.engine.offline_wanted());
  // a pattern request ignores the slots
  audio::OfflineRequest rq = kPattern1;
  rq.order_first = 9;
  rq.order_last = 2;
  CHECK(r.render.open(rq));
  r.render.close();
}

TEST_CASE("slots: the progress counts the steps of the range, not of the song") {
  build_slots_project(arena().project);
  Rig& r = make_rig(false);
  // slots 2 and 3 are 12 + 4 steps; after the first of them (the 12 steps) the render is three quarters through. Counted from slot 0 it would be (16 + 8 + 12) / 40, nine tenths.
  NEED(r.render.open(slots(2, 3)));
  const Pass p = run_pass(r.render, block_of(12) + 10u);
  CHECK(r.render.progress() >= 740u);
  CHECK(r.render.progress() <= 800u);
  CHECK(p.progress_monotonic);
  r.render.close();
}

TEST_CASE("slots: the tracks of the rectangle (track_mask) apply to a range of slots, and the project's mixer is the graph's again after it") {
  build_slots_project(arena().project);
  constexpr std::uint32_t kBlocks = 1500;
  audio::OfflineRequest kick = slots(0, 0);
  kick.track_mask = 0x0001;
  std::vector<std::int16_t> one;
  std::vector<std::int16_t> all;
  std::vector<std::int16_t> before;
  std::vector<std::int16_t> after;
  {
    Rig& r = make_rig(false);
    live_block(r, nullptr);
    live_block(r, nullptr);
    before = live_song_from(r, 0, 400);
  }
  {
    Rig& r = make_rig(false);
    one = offline(r, kick, kBlocks);
    all = offline(r, slots(0, 0), kBlocks);
    live_block(r, nullptr);
    live_block(r, nullptr);
    after = live_song_from(r, 0, 400);
  }
  const auto energy = [](const std::vector<std::int16_t>& v) {
    double e = 0;
    for (const std::int16_t s : v) {
      e += static_cast<double>(s) * static_cast<double>(s);
    }
    return e;
  };
  CHECK(energy(one) > 0.0);
  CHECK(energy(one) * 1.5 < energy(all));  // the kick alone against the four tracks
  CHECK(first_difference(before, after) == ~std::size_t{0});
}

TEST_CASE("slots: a mask of all 16 tracks is a choice, not 'no choice' - a muted track in it sounds (D8), while kAllTracks keeps the project's mute") {
  build_slots_project(arena().project);
  constexpr std::uint32_t kBlocks = 1500;
  static_assert(audio::OfflineRequest::kAllTracks != 0xFFFF, "all 16 tracks picked must differ from the 'no override' sentinel");
  audio::OfflineRequest picked = slots(0, 0);
  picked.track_mask = 0xFFFF;
  CHECK(picked.masked());
  CHECK_FALSE(slots(0, 0).masked());
  const auto energy = [](const std::vector<std::int16_t>& v) {
    double e = 0;
    for (const std::int16_t s : v) {
      e += static_cast<double>(s) * static_cast<double>(s);
    }
    return e;
  };
  const auto render = [&](const audio::OfflineRequest& rq, bool muted_in_graph) {
    Rig& r = make_rig(false);
    if (muted_in_graph) {
      push_param(r, audio::strip_param(0, gp::strip::Mute), 1.0f);  // (what the app told the graph when the user muted the kick: a plain render plays the graph as it is)
    }
    return offline(r, rq, kBlocks);
  };
  const std::vector<std::int16_t> unmuted = render(slots(0, 0), false);  // the project as it is, no mute
  model::TrackSlot muted = *arena().project.track(0);                     // the kick
  muted.flags = model::kTrackMute;
  NEED(arena().project.set_track(0, muted) == model::Status::Ok);
  const std::vector<std::int16_t> kept = render(slots(0, 0), true);  // no choice of tracks: the project's mute applies
  const std::vector<std::int16_t> chosen = render(picked, true);     // all 16 picked: the muted kick sounds
  CHECK(energy(kept) > 0.0);
  CHECK(energy(kept) < energy(unmuted));
  CHECK(first_difference(chosen, unmuted) == ~std::size_t{0});
  CHECK(first_difference(chosen, kept) != ~std::size_t{0});
}

TEST_CASE("selection: a block over all 16 tracks (mask 0xFFFF, rows cut) is a choice of tracks - a muted track in it sounds, while kAllTracks keeps the project's mute") {
  build_slots_project(arena().project);
  constexpr std::uint32_t kBlocks = 700;
  const auto block = [](std::uint16_t mask) {  // what App export_selection asks for: the block's rows of pattern 1, the tail trimmed
    audio::OfflineRequest rq = kPattern1;
    rq.row_first = 0;
    rq.row_last = 7;
    rq.track_mask = mask;
    rq.trim_tail = true;
    return rq;
  };
  CHECK(block(0xFFFF).masked());
  CHECK_FALSE(block(audio::OfflineRequest::kAllTracks).masked());
  const auto energy = [](const std::vector<std::int16_t>& v) {
    double e = 0;
    for (const std::int16_t s : v) {
      e += static_cast<double>(s) * static_cast<double>(s);
    }
    return e;
  };
  const auto render = [&](const audio::OfflineRequest& rq, bool muted_in_graph) {
    Rig& r = make_rig(false);
    if (muted_in_graph) {
      push_param(r, audio::strip_param(0, gp::strip::Mute), 1.0f);  // (the kick muted in the live graph, as the user left it)
    }
    return offline(r, rq, kBlocks);
  };
  const std::vector<std::int16_t> unmuted = render(block(audio::OfflineRequest::kAllTracks), false);
  model::TrackSlot muted = *arena().project.track(0);  // the kick
  muted.flags = model::kTrackMute;
  NEED(arena().project.set_track(0, muted) == model::Status::Ok);
  const std::vector<std::int16_t> kept = render(block(audio::OfflineRequest::kAllTracks), true);
  const std::vector<std::int16_t> chosen = render(block(0xFFFF), true);
  CHECK(energy(kept) > 0.0);
  CHECK(energy(kept) < energy(unmuted));
  CHECK(first_difference(chosen, unmuted) == ~std::size_t{0});
  CHECK(first_difference(chosen, kept) != ~std::size_t{0});
}

TEST_CASE("slots: the full-song WAV is byte for byte the file the export made before the slot range existed (pinned), and the whole range is that file") {
  build_project(arena().project);
  const auto wav = [](const audio::OfflineRequest& rq) {
    Rig& r = make_rig(true);  // a fresh engine for each file, with the sends: the delay's and the reverb's tails are in it
    return wav_of(r, rq);
  };
  const std::vector<std::uint8_t> file = wav(kSong);
  // The size and the FNV-1a hash of the 1,417,200 bytes that dev (ccf24a7) wrote for this project, taken before OfflineRequest had a slot range. This pin also fails on ANY change to the DSP that reaches
  // this render (a voice, a filter, the effects, the limiter, the converter): an INTENTIONAL change re-pins it - retake the hash from a render of the changed tree and say why in the commit. Re-pinned once for OS-WAVEXP section 8: the writing pass now starts the reverb's LFOs from their beginning (as the measuring pass did), the size is the same. The explicit
  // ranges below are compared with the file itself, so they need no hash.
  // The pin is per clock (core/audio_clock.h): the pre-1.0 clock (4080 cycles a sample) renders the 1,417,200 bytes above through the rate converter; at 44,100 Hz the engine's rate
  // is the file's, there is nothing to convert, and the same project is 1,417,260 bytes (OS-RATE; the hash taken from this tree's render at that clock).
  constexpr bool kPreClock = core::kRefCyclesPerSample.num == 4080u && core::kRefCyclesPerSample.den == 1u;
  CHECK(file.size() == (kPreClock ? 1417200u : 1417260u));
  CHECK(fnv1a(file) == (kPreClock ? 0x6DC22CA0DFCACBF0ull : 0xF57BCB659017EE86ull));
  CHECK(wav(slots(0, 1)) == file);                                   // build_project's two slots, spelled out
  CHECK(wav(slots(0, audio::OfflineRequest::kToEnd)) == file);       // the default, spelled out
}

TEST_CASE("slots: a range's WAV keeps the tail, and its first frames are the song's") {
  build_slots_project(arena().project);
  const auto wav = [](const audio::OfflineRequest& rq) {
    Rig& r = make_rig(false);
    return wav_of(r, rq);
  };
  const std::vector<std::uint8_t> song = wav(kSong);
  const std::vector<std::uint8_t> range = wav(slots(0, 0));
  NEED(range.size() > 44u + 4u * 1000u);
  const double seconds = static_cast<double>((range.size() - 44u) / 4u) / 44100.0;
  CHECK(seconds > 2.0 - 0.01);  // the slot is 2 s; the file runs on to the last sound
  CHECK(range.size() < song.size());
  // the slot's first 1.9 s are the song's: the same blocks through the same converter
  const std::size_t compare = static_cast<std::size_t>(1.9 * 44100.0) * 4u;
  NEED(song.size() > 44u + compare);
  CHECK(std::memcmp(song.data() + 44, range.data() + 44, compare) == 0);
}
