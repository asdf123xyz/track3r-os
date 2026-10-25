// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-COREFX CF1 (audio/tracker_fx.h, ADR 0004 amendment 2): the tracker FX of the Modern engine's sampler voices. The arithmetic (the period table, the arpeggio's semitones, the slide's step, the
// logarithm of a period ratio) against double precision; the processor driven by hand-made TrkFx and Tick events on a voice pool, heard in the voice's block (the pitch from the zero crossings of a
// sine, the level from its peak): arpeggio, volume slides, fine slides, fine portamento, the row scope (an effect that ends), a new note starting the state over, a track with no voice, the two level
// laws (the note's velocity and the instrument's own volume) and a retriggered note. The sequencer's side is seq_corefx_test.cpp; the audio against libopenmpt is corefx_openmpt_oracle.py.
// Host and Cortex-M4F (`qemu_audio`).
#include <doctest/doctest.h>

#include <o3os/audio/amiga_voices.h>
#include <o3os/audio/engine.h>
#include <o3os/audio/event_source.h>
#include <o3os/audio/graph.h>
#include <o3os/audio/smoothing.h>
#include <o3os/audio/tap_mem.h>
#include <o3os/audio/tracker_fx.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include "support/need.h"
#include "support/static_slot.h"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace {

namespace haudio = o3os::hal::audio;
using o3os::audio::EventKind;
using o3os::audio::Graph;
using o3os::audio::SampleDesc;
using o3os::audio::SeqEvent;
using o3os::audio::TrackerFx;
using o3os::audio::VoicePool;
namespace trkfx = o3os::audio::trkfx;

constexpr std::uint32_t kBlk = haudio::kBlock;
constexpr std::uint32_t kTickBlocks = 8;  // 512 samples a "tick" of these tests (the real tick is 882 at 125 BPM and speed 6; the processor does not care)

VoicePool& pool() {
  static o3os::test::StaticSlot<VoicePool> slot;
  return slot.make();
}
TrackerFx& tfx() {
  static TrackerFx t;
  t.attach(nullptr);
  t.reset(pool());
  return t;
}
std::uint32_t g_cycles = 0;
std::uint32_t fake_cycles() noexcept { return ++g_cycles; }
// A graph for the pan tests (the strips' pan is all they look at).
Graph& make_graph() {
  static o3os::test::StaticSlot<Graph> slot;
  static o3os::audio::TapMem taps;
  return slot.make(&fake_cycles, taps);
}

// 40 samples a period, amplitude 20000, looped over the whole sample: a note at the voice's root (60) plays it at 40, an octave up at 20.
std::int16_t g_sine[4000];
void make_sine40() {
  for (std::uint32_t i = 0; i < 4000; ++i) {
    g_sine[i] = static_cast<std::int16_t>(std::lround(20000.0 * std::sin(2.0 * 3.14159265358979 * static_cast<double>(i) / 40.0)));
  }
}
SampleDesc looped_desc() {
  make_sine40();
  SampleDesc d;
  d.data = g_sine;
  d.length = 4000;
  d.loop_start = 0;
  d.loop_end = 4000;
  return d;
}

SeqEvent trk(unsigned track, unsigned type, unsigned value, unsigned col = 0) {
  SeqEvent e{};
  e.kind = EventKind::TrkFx;
  e.track = static_cast<std::uint8_t>(track);
  e.a = static_cast<std::uint8_t>(type);
  e.b = static_cast<std::uint8_t>(value);
  e.flags = static_cast<std::uint8_t>(col);
  return e;
}
SeqEvent tick(unsigned index, unsigned of = 6) {
  SeqEvent e{};
  e.kind = EventKind::Tick;
  e.track = o3os::audio::kNoTrack;
  e.a = static_cast<std::uint8_t>(index);
  e.b = static_cast<std::uint8_t>(of);
  return e;
}

// What a tick sounded like: the mean period of the sine in samples (zero crossings, linear interpolation) and the peak, over the last half of kTickBlocks blocks (the first half lets a gain ramp land).
struct Heard {
  double period = 0.0;
  double peak = 0.0;
};
Heard run_blocks(VoicePool& p, unsigned blocks = kTickBlocks) {
  static std::int16_t out[kTickBlocks * kBlk];
  for (unsigned b = 0; b < blocks; ++b) {
    p.begin_block();
    p.render(0, kBlk);
    p.end_block();
    const std::int16_t* f = p.feed(0);
    for (std::uint32_t i = 0; i < kBlk; ++i) {
      out[b * kBlk + i] = f != nullptr ? f[i] : std::int16_t{0};
    }
  }
  Heard h;
  const unsigned from = blocks / 2u * kBlk;
  double first = -1.0, last = 0.0;
  unsigned crossings = 0;
  for (unsigned i = from + 1u; i < blocks * kBlk; ++i) {
    const double a = out[i - 1], b = out[i];
    if (std::fabs(b) > h.peak) {
      h.peak = std::fabs(b);
    }
    if (a < 0 && b >= 0) {
      const double t = static_cast<double>(i - 1) + (-a) / (b - a);
      if (first < 0) {
        first = t;
      }
      last = t;
      ++crossings;
    }
  }
  h.period = crossings >= 2u ? (last - first) / static_cast<double>(crossings - 1u) : 0.0;
  return h;
}

// Runs the 6 ticks of one row (events first, then the blocks), returning what each sounded like.
void row(VoicePool& p, TrackerFx& t, unsigned ticks, Heard* heard) {
  for (unsigned i = 0; i < ticks; ++i) {
    t.event(tick(i, ticks), p);
    heard[i] = run_blocks(p);
  }
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

}  // namespace

TEST_CASE("tracker fx: the period of a note, the arpeggio's semitones, the slide's step, the log of a ratio") {
  CHECK(trkfx::period_of_note(60) == 1712);  // the sampler's unity: S3M C-4, ProTracker C-2 x 4
  CHECK(trkfx::period_of_note(72) == 856);   // an octave up
  CHECK(trkfx::period_of_note(48) == 3424);
  CHECK(trkfx::period_of_note(36) == 6848);
  CHECK(trkfx::period_of_note(0) == 1712 * 32);
  CHECK(trkfx::period_of_note(127) == (1140 >> 5));  // 127 = 60 + 67: five octaves up, seven semitones (G: 1140)
  for (unsigned i = 0; i < 12; ++i) {
    CHECK(trkfx::period_of_note(60 + i) == trkfx::kPeriod12[i]);
  }
  CHECK(trkfx::period_of_note(200) == trkfx::period_of_note(127));  // past the table: held
  // the table is ProTracker's x 4 (C-2 428 ... B-2 226 -> 904 against Scream Tracker's 907) and falls with the note
  for (unsigned n = 1; n <= 127; ++n) {
    CHECK(trkfx::period_of_note(n) <= trkfx::period_of_note(n - 1));
  }
  // arpeggio: tick % 3 = 0 the note, 1 +x, 2 +y
  for (unsigned v = 0; v < 256; ++v) {
    const std::uint8_t b = static_cast<std::uint8_t>(v);
    CHECK(trkfx::arp_semitones(b, 0) == 0u);
    CHECK(trkfx::arp_semitones(b, 1) == (v >> 4));
    CHECK(trkfx::arp_semitones(b, 2) == (v & 15u));
    CHECK(trkfx::arp_semitones(b, 3) == 0u);
    CHECK(trkfx::arp_semitones(b, 31) == (v >> 4));  // 31 % 3 = 1
  }
  // slide: x up, else y down
  CHECK(trkfx::slide_step(0x40) == 4);
  CHECK(trkfx::slide_step(0x04) == -4);
  CHECK(trkfx::slide_step(0x53) == 5);  // both: x wins (ProTracker's Axy)
  CHECK(trkfx::slide_step(0x00) == 0);
  CHECK(trkfx::clamp_volume(-3) == 0);
  CHECK(trkfx::clamp_volume(70) == 64);
  CHECK(trkfx::clamp_period(-5) == trkfx::kPeriodMin);
  CHECK(trkfx::clamp_period(std::int64_t{1} << 40) == trkfx::kPeriodMax);
  // the offset gives the frequency ratio under the voice's law (a fraction of the way from one semitone's increment to the next), against double
  CHECK(trkfx::offset_q16(1712, 1712) == 0);
  const double r = std::pow(2.0, 1.0 / 12.0);
  for (const std::int32_t period : {1708, 1696, 1652, 1772, 2000, 856, 3424, 100, 40000, 1711, 1713, 1}) {
    const std::int32_t q = trkfx::offset_q16(1712, period);
    const std::int32_t w = (q >= 0 ? q : q - 65535) / 65536;  // floor
    const double g = static_cast<double>(q - w * 65536) / 65536.0;
    const double ratio = std::pow(2.0, w / 12.0) * (1.0 + g * (r - 1.0));
    CHECK_MESSAGE(std::fabs(ratio / (1712.0 / period) - 1.0) < 2e-5, "period " << period << ": ratio " << ratio << " want " << 1712.0 / period);
  }
  CHECK(std::abs(trkfx::offset_q16(1712, 856) - 12 * 65536) <= 1);  // an octave
  CHECK(std::abs(trkfx::offset_q16(1712, 3424) + 12 * 65536) <= 1);
}

TEST_CASE("tracker fx: arpeggio plays the note, then +x, then +y semitones on the ticks of its row and the note again after it") {
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  p.set_sample(0, looped_desc());
  CHECK(p.note_on(0, 60, 0, 127));
  const Heard base = run_blocks(p);
  NEED(base.period > 0.0);
  CHECK(near(base.period, 40.0, 0.05));
  t.event(trk(0, o3os::audio::kFxTrkArp, 0x37), p);  // +3 and +7
  Heard h[6];
  row(p, t, 6, h);
  for (unsigned i = 0; i < 6; ++i) {
    const double semis = i % 3u == 0u ? 0.0 : (i % 3u == 1u ? 3.0 : 7.0);
    CHECK_MESSAGE(near(h[i].period, 40.0 / std::pow(2.0, semis / 12.0), 0.06), "tick " << i << ": " << h[i].period);
  }
  t.event(trk(0, 0, 0), p);  // the row after: no effect
  row(p, t, 6, h);
  for (unsigned i = 0; i < 6; ++i) {
    CHECK_MESSAGE(near(h[i].period, 40.0, 0.06), "after, tick " << i << ": " << h[i].period);  // it ended on a +7 tick (5 % 3 = 2 -> +7): the first tick of the next row takes it away
  }
  // an arpeggio of 0 does nothing, and a row that ends on a raised tick leaves the note raised only until the next tick
  t.event(trk(0, o3os::audio::kFxTrkArp, 0x00), p);
  row(p, t, 6, h);
  for (unsigned i = 0; i < 6; ++i) {
    CHECK(near(h[i].period, 40.0, 0.06));
  }
}

TEST_CASE("tracker fx: the volume slide moves the level each tick from the second, by the note's velocity law, and holds it") {
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  p.set_sample(0, looped_desc());
  CHECK(p.note_on(0, 60, 0, 127));  // velocity 127: tracker volume 64
  Heard h[6];
  const Heard base = run_blocks(p);
  CHECK(near(base.peak, 20000.0, 200.0));
  t.event(trk(0, o3os::audio::kFxTrkVolSlide, 0x04), p);  // down 4 a tick
  row(p, t, 6, h);
  for (unsigned i = 0; i < 6; ++i) {
    const int vol = 64 - 4 * static_cast<int>(i);  // tick 0 does not slide: 64, 60, 56, 52, 48, 44
    const unsigned vel = static_cast<unsigned>(vol * 2 > 127 ? 127 : vol * 2);
    CHECK_MESSAGE(near(h[i].peak, 20000.0 * vel / 127.0, 350.0), "tick " << i << ": " << h[i].peak << " for volume " << vol);
  }
  CHECK(t.volume(0) == 44u);
  t.event(trk(0, 0, 0), p);  // the slide ended with its row: the volume stays
  row(p, t, 6, h);
  for (unsigned i = 0; i < 6; ++i) {
    CHECK_MESSAGE(near(h[i].peak, 20000.0 * 88.0 / 127.0, 350.0), "held, tick " << i << ": " << h[i].peak);
  }
  t.event(trk(0, o3os::audio::kFxTrkVolSlide, 0x30), p);  // up 3 a tick, held at 64
  row(p, t, 6, h);
  CHECK(t.volume(0) == 44u + 15u);
  t.event(trk(0, o3os::audio::kFxTrkVolSlide, 0xF0), p);
  row(p, t, 6, h);
  CHECK(t.volume(0) == 64u);
  CHECK(near(h[5].peak, 20000.0, 200.0));
  t.event(trk(0, o3os::audio::kFxTrkVolSlide, 0x0F), p);  // down 15 a tick: silent in five
  row(p, t, 6, h);
  CHECK(t.volume(0) == 0u);
  CHECK(h[5].peak < 1.0);
}

TEST_CASE("tracker fx: the fine volume slide acts once, on tick 0; x wins over y") {
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  p.set_sample(0, looped_desc());
  CHECK(p.note_on(0, 60, 0, 60));  // velocity 60: volume 30
  Heard h[6];
  run_blocks(p);
  t.event(trk(0, o3os::audio::kFxTrkFineVol, 0x05), p);  // down 5, once
  row(p, t, 6, h);
  CHECK(t.volume(0) == 25u);
  for (unsigned i = 0; i < 6; ++i) {
    CHECK_MESSAGE(near(h[i].peak, 20000.0 * 50.0 / 127.0, 350.0), "tick " << i << ": " << h[i].peak);
  }
  t.event(trk(0, o3os::audio::kFxTrkFineVol, 0x20), p);  // up 2, once
  row(p, t, 6, h);
  CHECK(t.volume(0) == 27u);
  t.event(trk(0, o3os::audio::kFxTrkFineVol, 0x33), p);  // both: x
  row(p, t, 6, h);
  CHECK(t.volume(0) == 30u);
  CHECK(near(h[5].peak, 20000.0 * 60.0 / 127.0, 350.0));
}

TEST_CASE("tracker fx: the instrument's own volume is where a slide starts, and a tracker volume is vol / 64 of unity (a note that names no velocity; OS-BUG-PASTE24)") {
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  SampleDesc d = looped_desc();
  d.amiga_vol = 32;  // tracker volume 32 ...
  d.gain = 32768;    // ... which is half of unity (-6 dB), the Amiga reading of that gain: round(64 x 0.5)
  p.set_sample(0, d);
  p.set_law(o3os::audio::LevelLaw::Og12);
  CHECK(p.note_on(0, 60, 0, o3os::audio::kVelocityDefault));
  VoicePool::TrackerView view;
  NEED(p.tracker_view(0, view));
  CHECK(view.volume == 32);
  CHECK(view.note == 60);
  Heard h[6];
  CHECK(near(run_blocks(p).peak, 10000.0, 100.0));
  t.event(trk(0, o3os::audio::kFxTrkFineVol, 0x08), p);  // down 8 at tick 0: volume 24, 24 / 64 of unity
  row(p, t, 6, h);
  CHECK(t.volume(0) == 24u);
  const double want = 20000.0 * 24.0 / 64.0;
  CHECK_MESSAGE(near(h[5].peak, want, want * 0.02 + 20.0), "peak " << h[5].peak << ", 24 / 64 of unity is " << want);
}

namespace {

// A note that names no velocity on an instrument of tracker volume 32 (half of unity), and the peak a tracker volume gives: vol / 64 of unity (OS-BUG-PASTE24; it was level 100 through the law).
void law_note(VoicePool& p) {
  SampleDesc d = looped_desc();
  d.amiga_vol = 32;
  d.gain = 32768;
  p.set_sample(0, d);
  p.set_law(o3os::audio::LevelLaw::Og12);
  CHECK(p.note_on(0, 60, 0, o3os::audio::kVelocityDefault));
}
double law_peak(unsigned vol) {
  return 20000.0 * vol / 64.0;
}

}  // namespace

TEST_CASE("tracker fx CF2: a plain volume slide over several ticks on a note with no velocity keeps vol / 64 of unity on every tick (it does not turn into the velocity law after the first)") {
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  law_note(p);
  Heard h[6];
  CHECK(near(run_blocks(p).peak, 10000.0, 100.0));
  t.event(trk(0, o3os::audio::kFxTrkVolSlide, 0x04), p);  // down 4 a tick from tick 1: 32, 28, 24, 20, 16, 12
  row(p, t, 6, h);
  for (unsigned i = 0; i < 6; ++i) {
    const unsigned vol = 32u - 4u * i;
    const double want = law_peak(vol);
    CHECK_MESSAGE(near(h[i].peak, want, want * 0.02 + 20.0), "tick " << i << ": " << h[i].peak << " for volume " << vol << " (law " << want << ")");
  }
  CHECK(t.volume(0) == 12u);
  VoicePool::TrackerView view;
  NEED(p.tracker_view(0, view));
  CHECK(view.volume == 12u);  // what the processor set is what the voice reports, not a value read back through the other law
}

TEST_CASE("tracker fx CF2: an FxReset in the middle of a note with no velocity starts the processor over from the volume the note has, on the same scale") {
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  law_note(p);
  run_blocks(p);
  Heard h[6];
  t.event(trk(0, o3os::audio::kFxTrkVolSlide, 0x04), p);
  row(p, t, 6, h);  // volume 12
  CHECK(t.volume(0) == 12u);
  SeqEvent reset{};
  reset.kind = EventKind::FxReset;
  reset.track = o3os::audio::kNoTrack;
  t.event(reset, p);
  CHECK(t.volume(0) == 0u);  // the state is gone ...
  t.event(trk(0, o3os::audio::kFxTrkVolSlide, 0x04), p);  // ... and the next row follows the same note: it starts from 12, not from a level decoded through the linear law
  row(p, t, 6, h);
  CHECK(t.volume(0) == 0u);  // 12 down 4 a tick: 12, 8, 4, 0, 0, 0
  for (unsigned i = 0; i < 4; ++i) {
    const unsigned vol = 12u - 4u * i;
    const double want = law_peak(vol);
    CHECK_MESSAGE(near(h[i].peak, want, want * 0.02 + 20.0), "tick " << i << ": " << h[i].peak << " for volume " << vol << " (law " << want << ")");
  }
  // the same with a fine slide, which acts on tick 0 only: one step, by the law
  law_note(p);
  run_blocks(p);
  t.event(trk(0, o3os::audio::kFxTrkFineVol, 0x08), p);  // 32 - 8 = 24
  row(p, t, 6, h);
  t.event(reset, p);
  t.event(trk(0, o3os::audio::kFxTrkFineVol, 0x08), p);  // 24 - 8 = 16, from the voice's own 24
  row(p, t, 6, h);
  CHECK(t.volume(0) == 16u);
  CHECK_MESSAGE(near(h[5].peak, law_peak(16), law_peak(16) * 0.02 + 20.0), "peak " << h[5].peak << ", law " << law_peak(16));
}

TEST_CASE("tracker fx: fine portamento moves the period once, by the value in quarter Paula units, and for good (until a note)") {
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  p.set_sample(0, looped_desc());
  CHECK(p.note_on(0, 60, 0, 127));
  Heard h[6];
  run_blocks(p);
  t.event(trk(0, o3os::audio::kFxTrkFinePortaUp, 60), p);  // S3M FFF: 60 units from 1712
  row(p, t, 6, h);
  CHECK(t.period(0) == 1652);
  for (unsigned i = 0; i < 6; ++i) {
    CHECK_MESSAGE(near(h[i].period, 40.0 * 1652.0 / 1712.0, 0.05), "tick " << i << ": " << h[i].period);
  }
  t.event(trk(0, 0, 0), p);  // the row after: the pitch stays where the fine slide put it
  row(p, t, 6, h);
  CHECK(t.period(0) == 1652);
  CHECK(near(h[5].period, 40.0 * 1652.0 / 1712.0, 0.05));
  t.event(trk(0, o3os::audio::kFxTrkFinePortaDown, 120), p);  // 120 units down: 1772
  row(p, t, 6, h);
  CHECK(t.period(0) == 1772);
  CHECK(near(h[5].period, 40.0 * 1772.0 / 1712.0, 0.05));
  // extra fine (1 unit): a cent
  t.event(trk(0, o3os::audio::kFxTrkFinePortaUp, 1), p);
  row(p, t, 2, h);
  CHECK(t.period(0) == 1771);
  // a new note starts the state over at its own period
  CHECK(p.note_on(0, 72, 0, 127));
  t.event(trk(0, o3os::audio::kFxTrkFinePortaUp, 0), p);  // an effect of no distance: the processor looks at the new note
  row(p, t, 6, h);
  CHECK(t.period(0) == 856);
  CHECK(near(h[5].period, 20.0, 0.05));
}

// OS-BUG-MODPLAY F4: Scream Tracker's period of a note is the table's scaled by 8363 / the sample's C2SPD, so one fine slide is a bigger step on a sample of a higher rate.
TEST_CASE("tracker fx: the period of a note at the sample's rate: 8363 / rate, to the nearest; the Paula rates, no rate and 44,100 Hz keep the table; the rate and the period are held to a range (OS-BUG-MODPLAY F4)") {
  using o3os::audio::rate_factor_q28;
  using o3os::audio::rate_hz_of_q28;
  for (unsigned n = 0; n < 128; ++n) {  // the rates that keep the table
    for (const std::uint32_t hz : {0u, 8287u, 8363u}) {
      CHECK(trkfx::note_period_at(n, hz) == trkfx::period_of_note(n));
    }
  }
  CHECK(trkfx::note_period_at(60, 11025) == 1299);  // 1712 x 8363 / 11025 = 1298.7
  CHECK(trkfx::note_period_at(72, 11025) == 649);   // an octave up: 856 x 8363 / 11025 = 649.3
  CHECK(trkfx::note_period_at(60, 22050) == 649);   // 1712 x 8363 / 22050 = 649.3
  CHECK(trkfx::note_period_at(60, 16726) == 856);   // twice the NTSC rate: half the period
  CHECK(trkfx::note_period_at(60, 48000) == 298);   // 298.3
  CHECK(trkfx::note_period_at(60, 8000) == 1790);   // a lower rate: a longer period (1789.7)
  // the rate is held to 1,000..96,000 Hz, the period to its range
  CHECK(trkfx::note_period_at(60, 500) == trkfx::note_period_at(60, 1000));
  CHECK(trkfx::note_period_at(60, 1u) == trkfx::note_period_at(60, 1000));
  CHECK(trkfx::note_period_at(60, 384000) == trkfx::note_period_at(60, 96000));
  CHECK(trkfx::note_period_at(60, 0xFFFFFFFFu) == trkfx::note_period_at(60, 96000));
  CHECK(trkfx::note_period_at(0, 1000) == 458159);                   // the longest period the table has, at the lowest rate: 54,784 x 8363 / 1000 (under kPeriodMax)
  CHECK(trkfx::note_period_at(127, 96000) == trkfx::kPeriodMin);     // 35 x 8363 / 96000 = 3: held to the range
  for (unsigned n = 0; n < 128; ++n) {
    for (const std::uint32_t hz : {1u, 1000u, 4000u, 11025u, 22050u, 44101u, 48000u, 96000u, 1000000u}) {
      const std::int32_t p = trkfx::note_period_at(n, hz);
      CHECK(p >= trkfx::kPeriodMin);
      CHECK(p <= trkfx::kPeriodMax);
    }
  }
  // the pitch a rate gives back: the Q28 factor of a rate in whole Hz returns it, and 0 and 44,100 Hz are "none"
  CHECK(rate_hz_of_q28(0) == 0u);
  CHECK(rate_hz_of_q28(rate_factor_q28(44100)) == 0u);
  for (std::uint32_t hz = 1000; hz <= 384000; hz += 13) {
    if (hz != 44100u) {
      CHECK_MESSAGE(rate_hz_of_q28(rate_factor_q28(hz)) == hz, "rate " << hz);
    }
  }
  CHECK(rate_hz_of_q28(rate_factor_q28(384000)) == 384000u);
}

namespace {
// 10 samples a period at the sample rate `hz`: at the note's root it sounds 10 x 44,100 / hz samples a period (40 at 11,025 Hz), so a block of the render has several cycles of it to measure a pitch from.
std::int16_t g_sine10[4000];
SampleDesc rated_desc(std::uint32_t hz) {
  for (std::uint32_t i = 0; i < 4000; ++i) {
    g_sine10[i] = static_cast<std::int16_t>(std::lround(20000.0 * std::sin(2.0 * 3.14159265358979 * static_cast<double>(i) / 10.0)));
  }
  SampleDesc d;
  d.data = g_sine10;
  d.length = 4000;
  d.loop_start = 0;
  d.loop_end = 4000;
  d.rate_q28 = o3os::audio::rate_factor_q28(hz);
  return d;
}
// The period of the voice on track 0, in output samples, over `blocks` blocks of the render (the first half is left out): about 25 cycles, so the interpolation of the crossings is a hundredth of a sample.
double measured_period(VoicePool& p, unsigned blocks = 32) {
  static std::int16_t out[32 * haudio::kBlock];
  for (unsigned b = 0; b < blocks; ++b) {
    p.begin_block();
    p.render(0, kBlk);
    p.end_block();
    const std::int16_t* f = p.feed(0);
    for (std::uint32_t i = 0; i < kBlk; ++i) {
      out[b * kBlk + i] = f != nullptr ? f[i] : std::int16_t{0};
    }
  }
  double first = -1.0, last = 0.0;
  unsigned crossings = 0;
  for (unsigned i = blocks / 2u * kBlk + 1u; i < blocks * kBlk; ++i) {
    const double a = out[i - 1], b = out[i];
    if (a < 0 && b >= 0) {
      const double t = static_cast<double>(i - 1) + (-a) / (b - a);
      if (first < 0) {
        first = t;
      }
      last = t;
      ++crossings;
    }
  }
  return crossings >= 2u ? (last - first) / static_cast<double>(crossings - 1u) : 0.0;
}
double cents_between(double from, double to) { return 1200.0 * std::log2(from / to); }  // how far up the pitch of the period `to` is from the one of `from`
}  // namespace

TEST_CASE("tracker fx: a fine portamento is Scream Tracker's step at the sample's rate: S3M FF4 at C2SPD 11,025 moves 21.4 cents, at 8,363 and 8,287 Hz 16.2 as before (OS-BUG-MODPLAY F4)") {
  struct Rate {
    std::uint32_t hz;
    std::int32_t base;  // the period of C-4 the processor holds for a sample at that rate
    double cents;       // 1200 log2(base / (base - 16)): the step of 16 units (FF4 is 4 x 4)
  };
  const Rate rates[] = {
      {11025, 1299, 21.46},  // libopenmpt and Scream Tracker: 21.5 (1298.6 before the rounding)
      {8363, 1712, 16.25},   // as before
      {8287, 1712, 16.25},   // the PAL Paula rate: as before (D1)
      {22050, 649, 43.22},   // twice as far as at 11,025 Hz, nearly
  };
  for (const Rate& r : rates) {
    VoicePool& p = pool();
    TrackerFx& t = tfx();
    p.set_sample(0, rated_desc(r.hz));
    CHECK(p.note_on(0, 60, 0, 127));
    run_blocks(p);
    const double want_root = 10.0 * 44100.0 / static_cast<double>(r.hz);  // the note's own period, in output samples
    const double root = measured_period(p);
    CHECK_MESSAGE(near(root, want_root, 0.02), "rate " << r.hz << ": the note sounds a period of " << root << " samples, want " << want_root);
    t.event(trk(0, o3os::audio::kFxTrkFinePortaUp, 16), p);  // FF4
    t.event(tick(0), p);
    CHECK(t.note_period(0) == r.base);
    CHECK(t.period(0) == r.base - 16);
    run_blocks(p);  // (the slide's pitch lands)
    const double moved = cents_between(root, measured_period(p));
    CHECK_MESSAGE(near(moved, r.cents, 0.5), "rate " << r.hz << ": the fine slide moved " << moved << " cents, want " << r.cents);
  }
}

TEST_CASE("tracker fx: at a rate the fine portamento and the arpeggio add, a new note takes its own rate's period, and a sample with no recorded rate keeps the table (OS-BUG-MODPLAY F4)") {
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  p.set_sample(0, rated_desc(11025));
  p.set_sample(1, looped_desc());  // no recorded rate: the OG's 44,100 Hz
  CHECK(p.note_on(0, 60, 0, 127));
  Heard h[6];
  run_blocks(p);
  t.event(trk(0, o3os::audio::kFxTrkFinePortaUp, 4), p);
  t.event(trk(0, o3os::audio::kFxTrkArp, 0x0C, 1), p);  // second column: +0 and +12
  row(p, t, 6, h);
  CHECK(t.note_period(0) == 1299);
  CHECK(t.period(0) == 1295);
  CHECK(near(h[0].period, 40.0 * 1295.0 / 1299.0, 0.05));
  CHECK(near(h[2].period, 20.0 * 1295.0 / 1299.0, 0.05));  // y = 12: an octave up from the slid pitch
  // a note an octave up on the same track: the state starts over at its own period, at the same rate
  CHECK(p.note_on(0, 72, 0, 127));
  t.event(trk(0, o3os::audio::kFxTrkFinePortaUp, 0), p);
  row(p, t, 1, h);
  CHECK(t.note_period(0) == 649);  // 856 x 8363 / 11025 = 649.3
  // a note of the sample with no recorded rate: the table, as before
  CHECK(p.note_on(0, 60, 1, 127));
  t.event(trk(0, o3os::audio::kFxTrkFinePortaUp, 60), p);
  row(p, t, 6, h);
  CHECK(t.note_period(0) == 1712);
  CHECK(t.period(0) == 1652);
}

TEST_CASE("tracker fx: arpeggio and fine portamento add (the arpeggio's steps are on the note's slid period); the porta's period is held to a range") {
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  p.set_sample(0, looped_desc());
  CHECK(p.note_on(0, 60, 0, 127));
  Heard h[6];
  run_blocks(p);
  t.event(trk(0, o3os::audio::kFxTrkFinePortaUp, 4), p);
  t.event(trk(0, o3os::audio::kFxTrkArp, 0x0C, 1), p);  // second column: +0 and +12
  row(p, t, 6, h);
  CHECK(near(h[0].period, 40.0 * 1708.0 / 1712.0, 0.05));
  CHECK(near(h[1].period, 40.0 * 1708.0 / 1712.0, 0.05));  // x = 0: the same note
  CHECK(near(h[2].period, 20.0 * 1708.0 / 1712.0, 0.05));  // y = 12: an octave up from the slid pitch
  t.event(trk(0, o3os::audio::kFxTrkFinePortaUp, 255), p);
  for (int i = 0; i < 100; ++i) {  // many rows of the same fine slide: the period stays in its range, the voice's pitch in the table's
    row(p, t, 1, h);
  }
  CHECK(t.period(0) >= trkfx::kPeriodMin);
  CHECK(h[0].period > 0.0);
  t.event(trk(0, o3os::audio::kFxTrkFinePortaDown, 255), p);
  for (int i = 0; i < 100; ++i) {
    row(p, t, 1, h);
  }
  CHECK(t.period(0) <= trkfx::kPeriodMax);
}

TEST_CASE("tracker fx: a track with no sampler voice, a bad track and a bad type are left alone; FxReset ends every effect") {
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  p.set_sample(0, looped_desc());
  t.event(trk(3, o3os::audio::kFxTrkVolSlide, 0x04), p);  // no voice on track 3
  t.event(tick(0), p);
  t.event(tick(1), p);
  CHECK(t.volume(3) == 0u);
  t.event(trk(200, o3os::audio::kFxTrkArp, 1), p);        // outside the 16 tracks
  t.event(trk(0, 99, 1), p);                              // not a tracker FX
  t.event(trk(0, 45, 1), p);                              // the Order jump is not one either
  CHECK(p.note_on(0, 60, 0, 127));
  Heard h[6];
  run_blocks(p);
  row(p, t, 6, h);
  for (unsigned i = 0; i < 6; ++i) {
    CHECK(near(h[i].peak, 20000.0, 200.0));
    CHECK(near(h[i].period, 40.0, 0.06));
  }
  t.event(trk(0, o3os::audio::kFxTrkVolSlide, 0x0F), p);
  SeqEvent reset{};
  reset.kind = EventKind::FxReset;
  reset.track = o3os::audio::kNoTrack;
  t.event(reset, p);
  row(p, t, 6, h);
  CHECK(near(h[5].peak, 20000.0, 200.0));  // the reset ended the slide before it ran
}

TEST_CASE("tracker fx: a released voice keeps its level (regain's rule) but follows the pitch; a retriggered note starts the state over") {
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  p.set_sample(0, looped_desc());
  CHECK(p.note_on(0, 60, 0, 127));
  Heard h[6];
  run_blocks(p);
  t.event(trk(0, o3os::audio::kFxTrkVolSlide, 0x10), p);
  row(p, t, 3, h);
  CHECK(t.volume(0) == 64u);
  CHECK(p.note_on(0, 60, 0, 40));  // retrigger at velocity 40 (volume 20)
  t.event(trk(0, o3os::audio::kFxTrkVolSlide, 0x00), p);
  row(p, t, 6, h);
  CHECK(t.volume(0) == 20u);
  CHECK(near(h[5].peak, 20000.0 * 40.0 / 127.0, 300.0));
}

// ---- CF2: retrigger, tremor, pan slide, the Panning FX, waveforms, glissando ------------------------------------------------------------------------------------------------------------

namespace {

// A one shot that decays: 40 samples a period, the amplitude 20000 falling to 2000 over 20000 samples (a tick of these tests is 512), so a restart shows as the peak jumping back up.
std::int16_t g_dec[20000];
SampleDesc decay_desc() {
  for (std::uint32_t i = 0; i < 20000; ++i) {
    g_dec[i] = static_cast<std::int16_t>(std::lround((20000.0 - 18000.0 * i / 20000.0) * std::sin(2.0 * 3.14159265358979 * static_cast<double>(i) / 40.0)));
  }
  SampleDesc d;
  d.data = g_dec;
  d.length = 20000;
  return d;
}

}  // namespace

TEST_CASE("tracker fx CF2: the volume code of a retrigger, the pan slide's step") {
  using trkfx::retrig_volume;
  CHECK(retrig_volume(30, 0) == 30);
  CHECK(retrig_volume(30, 8) == 30);
  CHECK(retrig_volume(30, 1) == 29);
  CHECK(retrig_volume(30, 2) == 28);
  CHECK(retrig_volume(30, 3) == 26);
  CHECK(retrig_volume(30, 4) == 22);
  CHECK(retrig_volume(30, 5) == 14);
  CHECK(retrig_volume(3, 5) == 0);   // held at 0
  CHECK(retrig_volume(64, 6) == 42);  // x 2/3, integer
  CHECK(retrig_volume(64, 7) == 32);
  CHECK(retrig_volume(30, 9) == 31);
  CHECK(retrig_volume(30, 10) == 32);
  CHECK(retrig_volume(30, 11) == 34);
  CHECK(retrig_volume(30, 12) == 38);
  CHECK(retrig_volume(60, 13) == 64);  // held at 64
  CHECK(retrig_volume(30, 14) == 45);
  CHECK(retrig_volume(30, 15) == 60);
  CHECK(retrig_volume(40, 15) == 64);
  // pan slide: y right else x left, 4 a tick from tick 1; Fy / xF once on tick 0 (libopenmpt's PanningSlide)
  CHECK(trkfx::pan_step(0x04, 0) == 0);
  CHECK(trkfx::pan_step(0x04, 1) == 16);
  CHECK(trkfx::pan_step(0x40, 3) == -16);
  CHECK(trkfx::pan_step(0x44, 1) == 16);   // both: right
  CHECK(trkfx::pan_step(0x0F, 1) == 60);   // x = 0: not a fine form, a plain slide right by 15
  CHECK(trkfx::pan_step(0x0F, 0) == 0);
  CHECK(trkfx::pan_step(0xF0, 1) == -60);
  CHECK(trkfx::pan_step(0x2F, 0) == -8);   // x F...: y = F, x != 0: fine left by x
  CHECK(trkfx::pan_step(0x2F, 1) == 0);
  CHECK(trkfx::pan_step(0xF3, 0) == 12);   // x = F, y != 0: fine right by y
  CHECK(trkfx::pan_step(0xFF, 0) == -60);  // both F: the left fine form first
  CHECK(trkfx::pan_step(0x00, 1) == 0);
}

TEST_CASE("tracker fx CF2: retrigger restarts the sample every y ticks, the count running across the rows and held by a row without the effect, a new note starting it over, and changes the volume first") {
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  p.set_sample(0, decay_desc());
  CHECK(p.note_on(0, 60, 0, 127));
  run_blocks(p);
  // Q03: a run of two rows. The count starts at 0 on the first tick; restarts on ticks 3 of row 1, then 3 ticks after: ticks 0 and 3 of row 2.
  t.event(trk(0, o3os::audio::kFxTrkRetrig, 0x03), p);
  Heard h[6], g[6];
  row(p, t, 6, h);
  t.event(trk(0, o3os::audio::kFxTrkRetrig, 0x03), p);  // the next row carries it too: the count goes on
  row(p, t, 6, g);
  // the sample decays by about 460 a tick of these tests: a restart raises the peak
  const bool up_h[6] = {false, false, false, true, false, false};
  const bool up_g[6] = {true, false, false, true, false, false};
  for (unsigned i = 1; i < 6; ++i) {
    CHECK_MESSAGE((h[i].peak > h[i - 1].peak + 500.0) == up_h[i], "row 1 tick " << i << ": " << h[i - 1].peak << " -> " << h[i].peak);
  }
  CHECK_MESSAGE((g[0].peak > h[5].peak + 500.0) == up_g[0], "row 2 tick 0: " << h[5].peak << " -> " << g[0].peak);
  for (unsigned i = 1; i < 6; ++i) {
    CHECK_MESSAGE((g[i].peak > g[i - 1].peak + 500.0) == up_g[i], "row 2 tick " << i << ": " << g[i - 1].peak << " -> " << g[i].peak);
  }
  // a row without it: nothing restarts, and the count is held: it was 3 ticks into the cycle after the restart on row 2's tick 3 (ticks 4, 5 counted: 3), so the next row that carries it restarts on its tick 0
  t.event(trk(0, 0, 0), p);
  Heard n[6];
  row(p, t, 6, n);
  for (unsigned i = 1; i < 6; ++i) {
    CHECK_MESSAGE(n[i].peak < n[i - 1].peak + 1.0, "no effect, tick " << i);
  }
  t.event(trk(0, o3os::audio::kFxTrkRetrig, 0x03), p);
  Heard m[6];
  row(p, t, 6, m);
  CHECK_MESSAGE(m[0].peak > n[5].peak + 500.0, "the held count restarts on the first tick: " << n[5].peak << " -> " << m[0].peak);
  for (unsigned i = 1; i < 6; ++i) {
    CHECK_MESSAGE((m[i].peak > m[i - 1].peak + 500.0) == (i == 3u), "after the gap, tick " << i);
  }
  // a new note starts the count over
  CHECK(p.note_on(0, 60, 0, 127));
  t.event(trk(0, o3os::audio::kFxTrkRetrig, 0x03), p);
  row(p, t, 6, m);
  for (unsigned i = 1; i < 6; ++i) {
    CHECK_MESSAGE((m[i].peak > m[i - 1].peak + 500.0) == (i == 3u), "new note, tick " << i);
  }
  // y = 0 never restarts
  t.event(trk(0, o3os::audio::kFxTrkRetrig, 0x40), p);
  row(p, t, 6, n);
  for (unsigned i = 1; i < 6; ++i) {
    CHECK_MESSAGE(n[i].peak < n[i - 1].peak + 1.0, "Q40, tick " << i);
  }
}

TEST_CASE("tracker fx CF2: a retrigger's volume code is applied before the restart and kept (Q71 halves the volume at each restart)") {
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  p.set_sample(0, looped_desc());
  CHECK(p.note_on(0, 60, 0, 127));
  run_blocks(p);
  t.event(trk(0, o3os::audio::kFxTrkRetrig, 0x71), p);  // x 1/2, every tick
  Heard h[6];
  row(p, t, 6, h);
  CHECK(t.volume(0) == 2u);  // 64 -> (tick 0 only starts the run) 32, 16, 8, 4, 2
  CHECK(near(h[0].peak, 20000.0, 200.0));
  CHECK(near(h[1].peak, 20000.0 * 64.0 / 127.0, 350.0));  // velocity 2 x 32 = 64
  CHECK(near(h[2].peak, 20000.0 * 32.0 / 127.0, 350.0));
  t.event(trk(0, o3os::audio::kFxTrkRetrig, 0x91), p);  // + 1 every tick
  row(p, t, 6, h);
  CHECK(t.volume(0) == 8u);  // a code change in the middle of a run: the run goes on
}

TEST_CASE("tracker fx CF2: tremor gates the volume x + 1 ticks on and y + 1 off, the position kept across rows without it, and gives the channel's volume back when it ends") {
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  p.set_sample(0, looped_desc());
  CHECK(p.note_on(0, 60, 0, 127));
  run_blocks(p);
  t.event(trk(0, o3os::audio::kFxTrkTremor, 0x21), p);  // 3 on, 2 off
  Heard h[6];
  row(p, t, 6, h);
  const bool on[6] = {true, true, true, false, false, true};
  for (unsigned i = 0; i < 6; ++i) {
    CHECK_MESSAGE((h[i].peak > 10000.0) == on[i], "tick " << i << ": " << h[i].peak);
  }
  CHECK(t.volume(0) == 64u);  // the channel's volume is not touched by the gate
  t.event(trk(0, 0, 0), p);   // a row without: the volume is back at once
  row(p, t, 2, h);
  CHECK(near(h[0].peak, 20000.0, 200.0));
  CHECK(near(h[1].peak, 20000.0, 200.0));
  t.event(trk(0, o3os::audio::kFxTrkTremor, 0x21), p);  // the position went on from where the first row left it (6 ticks: 1 into the second cycle -> ticks continue: on, on, off, off, on, on)
  row(p, t, 6, h);
  const bool on2[6] = {true, true, false, false, true, true};
  for (unsigned i = 0; i < 6; ++i) {
    CHECK_MESSAGE((h[i].peak > 10000.0) == on2[i], "second run, tick " << i << ": " << h[i].peak);
  }
  t.event(trk(0, o3os::audio::kFxTrkTremor, 0x00), p);  // 1 on 1 off
  row(p, t, 4, h);
  CHECK((h[0].peak > 10000.0) != (h[1].peak > 10000.0));
  CHECK((h[0].peak > 10000.0) == (h[2].peak > 10000.0));
}

TEST_CASE("tracker fx CF2: tremor and a volume slide on a track whose volume the tremor gates; a voice that ends under a tremor leaves no gate behind") {
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  p.set_sample(0, looped_desc());
  CHECK(p.note_on(0, 60, 0, 127));
  run_blocks(p);
  t.event(trk(0, o3os::audio::kFxTrkTremor, 0x01), p);  // 2 on 2 off
  t.event(trk(0, o3os::audio::kFxTrkFineVol, 0x04, 1), p);
  Heard h[6];
  row(p, t, 6, h);
  CHECK(t.volume(0) == 60u);  // the fine slide acts on the channel's volume, gated or not
  p.cut(0);
  run_blocks(p);
  t.event(trk(0, o3os::audio::kFxTrkTremor, 0x01), p);
  row(p, t, 6, h);  // no voice: nothing to give, nothing fails
  CHECK(p.note_on(0, 60, 0, 127));  // a new note: the state starts over, the gate with it
  t.event(trk(0, 0, 0), p);
  t.event(trk(0, 0, 0, 1), p);
  row(p, t, 2, h);
  CHECK(near(h[1].peak, 20000.0, 200.0));
}

TEST_CASE("tracker fx CF2: a transport start (FxReset) gives back what the processor holds for one tick: the tremor's gate and the arpeggio's pitch") {
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  p.set_sample(0, looped_desc());
  CHECK(p.note_on(0, 60, 0, 127));
  run_blocks(p);
  SeqEvent reset{};
  reset.kind = EventKind::FxReset;
  reset.track = o3os::audio::kNoTrack;
  Heard h[6];
  t.event(trk(0, o3os::audio::kFxTrkTremor, 0x00), p);  // 1 on, 1 off
  t.event(tick(0), p);
  h[0] = run_blocks(p);
  t.event(tick(1), p);
  h[1] = run_blocks(p);
  CHECK(h[1].peak < 100.0);  // gated
  t.event(reset, p);
  h[2] = run_blocks(p);
  CHECK(near(h[2].peak, 20000.0, 200.0));  // not left silent
  CHECK(p.note_on(0, 60, 0, 127));
  t.event(trk(0, o3os::audio::kFxTrkArp, 0x0C), p);  // +12 on tick 2
  t.event(tick(1), p);
  t.event(tick(2), p);
  h[3] = run_blocks(p);
  CHECK(near(h[3].period, 20.0, 0.1));
  t.event(reset, p);
  h[4] = run_blocks(p);
  CHECK(near(h[4].period, 40.0, 0.1));  // not left an octave up
}

TEST_CASE("tracker fx CF2: pan slide moves the strip's pan from where it is, absolute, clamped to the strip's range, and holds it after the row") {
  Graph& g = make_graph();
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  t.attach(&g);
  CHECK(g.track_pan(0) == doctest::Approx(50.0f));
  t.event(trk(0, o3os::audio::kFxTrkPanSlide, 0x04), p);  // right 16/256 a tick from tick 1
  for (unsigned i = 0; i < 6; ++i) {
    t.event(tick(i), p);
  }
  CHECK(t.pan_owned(0));
  CHECK(t.pan(0) == 128 + 5 * 16);
  CHECK(g.track_pan(0) == doctest::Approx((128.0f + 80.0f) * 100.0f / 256.0f));
  t.event(trk(0, 0, 0), p);  // the row after: the pan stays
  for (unsigned i = 0; i < 6; ++i) {
    t.event(tick(i), p);
  }
  CHECK(g.track_pan(0) == doctest::Approx(208.0f * 100.0f / 256.0f));
  t.event(trk(0, o3os::audio::kFxTrkPanSlide, 0xF0), p);  // left 60 a tick: held at 0
  for (unsigned i = 0; i < 6; ++i) {
    t.event(tick(i), p);
  }
  CHECK(t.pan(0) == 0);
  CHECK(g.track_pan(0) == doctest::Approx(0.0f));
  t.event(trk(0, o3os::audio::kFxTrkPanSlide, 0xF4), p);  // fine right 16, once
  for (unsigned i = 0; i < 6; ++i) {
    t.event(tick(i), p);
  }
  CHECK(t.pan(0) == 16);
  t.event(trk(0, o3os::audio::kFxTrkPanSlide, 0xFF), p);  // fine left 60: held at 0
  for (unsigned i = 0; i < 6; ++i) {
    t.event(tick(i), p);
  }
  CHECK(t.pan(0) == 0);
  t.event(trk(0, o3os::audio::kFxTrkPanSlide, 0x0F), p);  // right 60 a tick for five: held at 256
  t.event(trk(0, 0, 0), p);
  t.event(trk(0, o3os::audio::kFxTrkPanSlide, 0x0F), p);
  for (unsigned i = 0; i < 6; ++i) {
    t.event(tick(i), p);
  }
  CHECK(t.pan(0) == 256);
  CHECK(g.track_pan(0) == doctest::Approx(100.0f));
  // another track keeps the strip's own pan; a track with a slide that is not owned starts from the strip's
  CHECK(g.track_pan(1) == doctest::Approx(50.0f));
  CHECK_FALSE(t.pan_owned(1));
  t.attach(nullptr);
}

namespace {

SeqEvent panning(unsigned track, bool set, unsigned value = 0, unsigned layer = o3os::audio::kLayerFx1) {
  SeqEvent e{};
  e.kind = set ? EventKind::ModSet : EventKind::ModEnd;
  e.track = static_cast<std::uint8_t>(track);
  e.a = set ? o3os::audio::kFxPanning : std::uint8_t{0};
  e.b = static_cast<std::uint8_t>(value);
  e.c = static_cast<std::uint8_t>(layer);
  e.flags = o3os::audio::kModGroupPanning;
  return e;
}
SeqEvent fx_reset() {
  SeqEvent e{};
  e.kind = EventKind::FxReset;
  e.track = o3os::audio::kNoTrack;
  return e;
}

}  // namespace

TEST_CASE("tracker fx CF2: the Panning FX (31), our rule: the track's absolute pan while it is held, again at each note, and the ModEnd gives the instrument's pan back") {
  Graph& g = make_graph();
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  t.attach(&g);
  g.set_voice_pan(2, 20);       // a note of an instrument of pan 20 ...
  t.note_started(2, 20);        // ... (the Engine tells the processor)
  CHECK(g.track_pan(2) == doctest::Approx(20.0f));
  CHECK(TrackerFx::takes(panning(2, true, 80)));
  CHECK(TrackerFx::takes(panning(2, false)));
  t.event(panning(2, true, 80), p);
  CHECK(t.pan_fx_held(2));
  CHECK_FALSE(t.pan_owned(2));  // not a tracker's pan: it is not the processor's to keep
  CHECK(g.track_pan(2) == doctest::Approx(80.0f));  // applied at once, absolute
  g.set_voice_pan(2, 35);       // the next note, while the column still holds the FX (the control table sent no ModEnd) ...
  CHECK(g.track_pan(2) == doctest::Approx(35.0f));
  t.note_started(2, 35);
  CHECK(g.track_pan(2) == doctest::Approx(80.0f));  // ... is heard at the FX's pan
  t.event(panning(2, true, 10), p);                 // another value of the FX
  CHECK(g.track_pan(2) == doctest::Approx(10.0f));
  t.event(panning(2, false), p);                    // FX Off, a note step without it, another FX: the ModEnd
  CHECK_FALSE(t.pan_fx_held(2));
  CHECK(g.track_pan(2) == doctest::Approx(35.0f));  // the instrument's own again
  g.set_voice_pan(2, 60);
  t.note_started(2, 60);
  CHECK(g.track_pan(2) == doctest::Approx(60.0f));  // and the next note plays at its instrument's pan
  t.event(panning(2, false), p);                    // a ModEnd of nothing held changes nothing
  CHECK(g.track_pan(2) == doctest::Approx(60.0f));
  // the Panning FX acts whichever layer won (OS-PERFFX P2: under the performance layer it is the pan the layer's value is added to; the layer's own tests are below); a bad value is held to 100; a bad track is nothing
  t.event(panning(3, true, 70, o3os::audio::kLayerPerf), p);
  CHECK(t.pan_fx_held(3));
  CHECK(g.track_pan(3) == doctest::Approx(70.0f));
  t.event(panning(3, false), p);
  t.event(panning(4, true, 200), p);
  CHECK(g.track_pan(4) == doctest::Approx(100.0f));
  t.event(panning(40, true, 70), p);
  t.event(panning(40, false), p);
  // a pan slide replaces the FX and goes on from its pan
  t.event(trk(2, o3os::audio::kFxTrkPanSlide, 0x40), p);  // left 16/256 a tick
  t.event(panning(2, true, 80), p);
  for (unsigned i = 0; i < 6; ++i) {
    t.event(tick(i), p);
  }
  CHECK_FALSE(t.pan_fx_held(2));
  CHECK(t.pan_owned(2));
  CHECK(t.pan(2) == (80 * 256 + 50) / 100 - 80);
  // FxReset ends it (the strip keeps the pan it has; the instrument's returns at its next note)
  t.event(panning(4, true, 30), p);
  t.event(fx_reset(), p);
  CHECK_FALSE(t.pan_fx_held(4));
  CHECK_FALSE(t.pan_owned(2));
  t.attach(nullptr);
}

namespace {

// A ModSet / ModEnd of the Panning or Volume group as the sequencer sends it with the performance layer in play: `layer` the winner, a / b the step FX underneath (0 when none), `value` the layer's.
SeqEvent layer_ev(unsigned group, unsigned track, bool set, unsigned a, unsigned b, unsigned layer, int value) {
  SeqEvent e{};
  e.kind = set ? EventKind::ModSet : EventKind::ModEnd;
  e.track = static_cast<std::uint8_t>(track);
  e.a = static_cast<std::uint8_t>(a);
  e.b = static_cast<std::uint8_t>(b);
  e.c = static_cast<std::uint8_t>(layer);
  e.flags = static_cast<std::uint8_t>(group);
  e.value = value;
  return e;
}

}  // namespace

TEST_CASE("tracker fx P2: the performance layer's PANNING is the strip's own perf_pan, and the Panning FX under it keeps acting") {
  using o3os::audio::kLayerFx1;
  using o3os::audio::kLayerPerf;
  using o3os::audio::kModGroupPanning;
  Graph& g = make_graph();
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  t.attach(&g);
  g.set_voice_pan(5, 20);
  t.note_started(5, 20);
  // the layer alone: the strip has its value, the tracker's pan is where it was
  t.event(layer_ev(kModGroupPanning, 5, true, 0, 0, kLayerPerf, 30), p);
  CHECK(g.perf_pan(5) == 30);
  CHECK_FALSE(t.pan_fx_held(5));
  CHECK(g.track_pan(5) == doctest::Approx(20.0f));
  // a new value, and a bad one held to the range
  t.event(layer_ev(kModGroupPanning, 5, true, 0, 0, kLayerPerf, -75), p);
  CHECK(g.perf_pan(5) == -75);
  t.event(layer_ev(kModGroupPanning, 5, true, 0, 0, kLayerPerf, 5000), p);
  CHECK(g.perf_pan(5) == 100);
  t.event(layer_ev(kModGroupPanning, 5, true, 0, 0, kLayerPerf, -5000), p);
  CHECK(g.perf_pan(5) == -100);
  // the step FX arrives under the layer (the control table sends the layer as the winner with the FX in a and b): the FX is the track's pan, the layer's value is not touched
  t.event(layer_ev(kModGroupPanning, 5, true, o3os::audio::kFxPanning, 70, kLayerPerf, 40), p);
  CHECK(t.pan_fx_held(5));
  CHECK(g.track_pan(5) == doctest::Approx(70.0f));
  CHECK(g.perf_pan(5) == 40);
  // FxReset ends the step FX and the tracker's offsets, never the layer's value; the same for the transport stopping
  t.event(fx_reset(), p);
  CHECK_FALSE(t.pan_fx_held(5));
  CHECK(g.perf_pan(5) == 40);
  g.set_voice_pan(5, 20);  // (the next note of the instrument)
  t.note_started(5, 20);
  t.event(layer_ev(kModGroupPanning, 5, true, o3os::audio::kFxPanning, 70, kLayerPerf, 40), p);
  SeqEvent stopped{};
  stopped.kind = EventKind::Stopped;
  stopped.track = o3os::audio::kNoTrack;
  t.event(stopped, p);
  CHECK(g.perf_pan(5) == 40);
  // the step FX ends under the layer (the winner is sent again with nothing underneath): the instrument's pan is back, the layer stays
  t.event(layer_ev(kModGroupPanning, 5, true, 0, 0, kLayerPerf, 40), p);
  CHECK_FALSE(t.pan_fx_held(5));
  CHECK(g.track_pan(5) == doctest::Approx(20.0f));
  CHECK(g.perf_pan(5) == 40);
  // the layer ends while the step FX stays: the winner is the step's, and the value goes; the FX is applied as it was
  t.event(layer_ev(kModGroupPanning, 5, true, o3os::audio::kFxPanning, 70, kLayerPerf, 40), p);
  t.event(layer_ev(kModGroupPanning, 5, true, o3os::audio::kFxPanning, 70, kLayerFx1, 0), p);
  CHECK(g.perf_pan(5) == 0);
  CHECK(t.pan_fx_held(5));
  CHECK(g.track_pan(5) == doctest::Approx(70.0f));
  // the layer over the FX again, then nothing is left: the ModEnd takes both off
  t.event(layer_ev(kModGroupPanning, 5, true, o3os::audio::kFxPanning, 70, kLayerPerf, -10), p);
  CHECK(g.perf_pan(5) == -10);
  t.event(layer_ev(kModGroupPanning, 5, false, 0, 0, kLayerFx1, 0), p);
  CHECK(g.perf_pan(5) == 0);
  CHECK_FALSE(t.pan_fx_held(5));
  CHECK(g.track_pan(5) == doctest::Approx(20.0f));
  // a ModEnd of nothing held changes nothing; a track that is not one is nothing; another track is untouched
  t.event(layer_ev(kModGroupPanning, 5, false, 0, 0, kLayerFx1, 0), p);
  t.event(layer_ev(kModGroupPanning, 40, true, 0, 0, kLayerPerf, 20), p);
  CHECK(g.perf_pan(6) == 0);
  CHECK(g.perf_pan(40) == 0);
  t.attach(nullptr);
}

TEST_CASE("tracker fx P2: the performance layer's VOLUME is the strip's own perf_gain: only the layer's ModSet sets it, FxReset and Stopped leave it, the ModEnd and a step layer take it off") {
  using o3os::audio::kLayerFx1;
  using o3os::audio::kLayerPerf;
  using o3os::audio::kModGroupVolume;
  Graph& g = make_graph();
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  t.attach(&g);
  CHECK(TrackerFx::takes(layer_ev(kModGroupVolume, 2, true, 0, 0, kLayerPerf, -20)));
  CHECK(TrackerFx::takes(layer_ev(kModGroupVolume, 2, false, 0, 0, kLayerFx1, 0)));
  t.event(layer_ev(kModGroupVolume, 2, true, 0, 0, kLayerPerf, -40), p);
  CHECK(g.perf_gain(2) == -40);
  CHECK(g.perf_pan(2) == 0);  // the other field is not this group's
  t.event(layer_ev(kModGroupVolume, 2, true, 0, 0, kLayerPerf, 70), p);
  CHECK(g.perf_gain(2) == 70);  // (a positive value is held; recompute_strip makes it 0 dB)
  t.event(layer_ev(kModGroupVolume, 2, true, 0, 0, kLayerPerf, -5000), p);
  CHECK(g.perf_gain(2) == -100);
  t.event(layer_ev(kModGroupVolume, 2, true, 0, 0, kLayerPerf, 5000), p);
  CHECK(g.perf_gain(2) == 100);
  t.event(layer_ev(kModGroupVolume, 2, true, 0, 0, kLayerPerf, -40), p);
  // the tracker's own gain and FxReset: the channel volume and the global volume return to full, the layer stays
  t.event(trk(2, o3os::audio::kFxTrkChanVol, 32), p);
  CHECK(g.track_gain(2) == doctest::Approx(0.5f));
  t.event(fx_reset(), p);
  CHECK(g.track_gain(2) == 1.0f);
  CHECK(g.perf_gain(2) == -40);
  SeqEvent stopped{};
  stopped.kind = EventKind::Stopped;
  stopped.track = o3os::audio::kNoTrack;
  t.event(stopped, p);
  CHECK(g.perf_gain(2) == -40);
  // a step layer as the winner (Velocity or Random velocity under a layer that has ended): no layer value
  t.event(layer_ev(kModGroupVolume, 2, true, 18, 90, kLayerFx1, 0), p);
  CHECK(g.perf_gain(2) == 0);
  t.event(layer_ev(kModGroupVolume, 2, true, 0, 0, kLayerPerf, -60), p);
  CHECK(g.perf_gain(2) == -60);
  t.event(layer_ev(kModGroupVolume, 2, false, 0, 0, kLayerFx1, 0), p);
  CHECK(g.perf_gain(2) == 0);
  t.event(layer_ev(kModGroupVolume, 99, true, 0, 0, kLayerPerf, -60), p);  // not a track
  t.event(layer_ev(kModGroupVolume, 99, false, 0, 0, kLayerFx1, 0), p);
  CHECK(g.perf_gain(99) == 0);
  t.attach(nullptr);
}

TEST_CASE("tracker fx CF2: Channel pan (57) is the track's absolute pan, held over notes, the Panning FX and its end, until another or the transport start") {
  Graph& g = make_graph();
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  t.attach(&g);
  CHECK(TrackerFx::takes(trk(5, o3os::audio::kFxTrkChanPan, 30)));
  t.event(trk(5, o3os::audio::kFxTrkChanPan, 30), p);
  CHECK(t.pan_owned(5));
  CHECK(g.track_pan(5) == doctest::Approx(30.0f));  // exact: 0..100 as given
  t.event(trk(5, 0, 0), p);                         // the end of the row's effects ends nothing: a tracker's pan outlives the step
  g.set_voice_pan(5, 90);                           // a note of an instrument of pan 90 ...
  CHECK(g.track_pan(5) == doctest::Approx(90.0f));
  t.note_started(5, 90);
  CHECK(g.track_pan(5) == doctest::Approx(30.0f));  // ... plays at the channel's pan
  for (unsigned i = 0; i < 6; ++i) {
    t.event(tick(i), p);
  }
  CHECK(g.track_pan(5) == doctest::Approx(30.0f));  // a tick without a slide moves nothing
  // the Panning FX in front of it, and its end: back to the channel's pan, not the instrument's
  t.event(panning(5, true, 75), p);
  CHECK(g.track_pan(5) == doctest::Approx(75.0f));
  t.note_started(5, 90);
  CHECK(g.track_pan(5) == doctest::Approx(75.0f));
  t.event(panning(5, false), p);
  CHECK(g.track_pan(5) == doctest::Approx(30.0f));
  // another Channel pan replaces it, also over a held Panning FX; the values are held to 0..100
  t.event(trk(5, o3os::audio::kFxTrkChanPan, 0), p);
  CHECK(g.track_pan(5) == doctest::Approx(0.0f));
  t.event(panning(5, true, 75), p);
  t.event(trk(5, o3os::audio::kFxTrkChanPan, 200), p);
  CHECK_FALSE(t.pan_fx_held(5));
  CHECK(g.track_pan(5) == doctest::Approx(100.0f));
  // a pan slide goes on from it
  t.event(trk(5, o3os::audio::kFxTrkChanPan, 50), p);
  t.event(trk(5, o3os::audio::kFxTrkPanSlide, 0x04), p);
  for (unsigned i = 0; i < 6; ++i) {
    t.event(tick(i), p);
  }
  CHECK(t.pan(5) == 128 + 5 * 16);
  g.set_voice_pan(5, 10);
  t.note_started(5, 10);
  CHECK(g.track_pan(5) == doctest::Approx((128.0f + 80.0f) * 100.0f / 256.0f));
  // FxReset releases it: the next note plays at its instrument's pan
  t.event(fx_reset(), p);
  CHECK_FALSE(t.pan_owned(5));
  g.set_voice_pan(5, 10);
  t.note_started(5, 10);
  CHECK(g.track_pan(5) == doctest::Approx(10.0f));
  t.attach(nullptr);
}

// ---- the pan FX through Engine::dispatch (the events the sequencer sends, one list a block) -----------------------------------------------------------------------------------------

namespace {

constexpr unsigned kScriptBlocks = 12;
std::vector<SeqEvent> g_script[kScriptBlocks];
std::uint32_t script_step(void*, std::uint64_t block_start, std::uint32_t n, SeqEvent* out, std::uint32_t cap) noexcept {
  const std::uint64_t b = block_start / n;
  std::uint32_t c = 0;
  if (b < kScriptBlocks) {
    for (const SeqEvent& e : g_script[b]) {
      if (c < cap) {
        out[c++] = e;
      }
    }
  }
  return c;
}
std::uint32_t g_engine_cycles = 0;
std::uint32_t engine_cycles() noexcept { return g_engine_cycles += 7u; }

SeqEvent note_on(unsigned track, unsigned slot) {
  SeqEvent e{};
  e.kind = EventKind::NoteOn;
  e.track = static_cast<std::uint8_t>(track);
  e.a = 60;
  e.b = static_cast<std::uint8_t>(slot);
  e.c = 127;
  return e;
}

}  // namespace

TEST_CASE("tracker fx CF2: the pan FX through the Engine: the Panning FX ends at the next plain note and on FX Off, Channel pan is held over notes (sampler and Amiga instruments) until FxReset") {
  static o3os::test::StaticSlot<o3os::audio::Engine> engine_slot;
  static o3os::audio::TapMem taps;
  static o3os::test::StaticSlot<o3os::audio::AmigaVoices> amiga_slot;
  static SeqEvent events[o3os::audio::kMaxEventsPerBlock];
  o3os::audio::Engine& e = engine_slot.make(&engine_cycles, taps);
  VoicePool& p = pool();
  o3os::audio::AmigaVoices& a = amiga_slot.make();
  SampleDesc d = looped_desc();
  d.pan = 20;  // slot 0, the sampler: pan 20
  p.set_sample(0, d);
  d.pan = 80;  // slot 1: pan 80
  p.set_sample(1, d);
  d.pan = 70;  // slot 2: an Amiga instrument, pan 70
  d.engine = o3os::audio::kEngineAmiga;
  p.set_sample(2, d);
  e.attach_voices(&p);
  e.attach_amiga(&a);
  o3os::audio::EventSource src;
  src.step = &script_step;
  src.buffer = events;
  src.capacity = o3os::audio::kMaxEventsPerBlock;
  e.attach_events(src);
  const unsigned kTrack = 2;
  // What the sequencer sends for  P75 + note | note | note + P90 | FX Off | CP30 + note | note | P90 + note | Off | Amiga note | FxReset + note:
  // a note step whose column holds no FX sends the ModEnd before its NoteOn (seq/control.h), a step that holds the FX again sends the ModSet before it.
  for (std::vector<SeqEvent>& v : g_script) {
    v.clear();
  }
  g_script[0] = {panning(kTrack, true, 75), note_on(kTrack, 0)};
  g_script[1] = {panning(kTrack, false), note_on(kTrack, 1)};
  g_script[2] = {panning(kTrack, true, 90), note_on(kTrack, 0)};
  g_script[3] = {panning(kTrack, false)};
  g_script[4] = {trk(kTrack, o3os::audio::kFxTrkChanPan, 30), note_on(kTrack, 1)};
  g_script[5] = {note_on(kTrack, 0)};
  g_script[6] = {panning(kTrack, true, 90), note_on(kTrack, 1)};
  g_script[7] = {panning(kTrack, false)};
  g_script[8] = {note_on(kTrack, 2)};
  g_script[9] = {fx_reset(), note_on(kTrack, 1)};
  alignas(8) std::int16_t in[haudio::kBlockSamples] = {};
  alignas(8) std::int16_t out[haudio::kBlockSamples] = {};
  haudio::BlockReport report{};
  const haudio::BlockIo io{in, out, 0, &report};
  const Graph& g = e.graph();
  const float want[10] = {
      75.0f,  // P75, then the note: the FX's pan, not the instrument's 20
      80.0f,  // FX Off with the next note: that note plays at its instrument's pan
      90.0f,  // the FX held again
      20.0f,  // FX Off alone: the instrument's pan comes back (the last note's, 20)
      30.0f,  // Channel pan, then a note of an instrument of 80
      30.0f,  // the next note (instrument 20) is still at the channel's pan
      90.0f,  // the Panning FX over it ...
      30.0f,  // ... and its end gives the channel's pan back
      30.0f,  // an Amiga instrument's note (pan 70) too
      80.0f,  // the transport start releases it: the note plays at its instrument's pan
  };
  for (unsigned b = 0; b < 10; ++b) {
    e.render_block(io);
    CHECK_MESSAGE(g.track_pan(kTrack) == doctest::Approx(want[b]), "block " << b << ": " << g.track_pan(kTrack));
  }
}

TEST_CASE("tracker fx CF2: without a graph a pan effect keeps its state and applies nothing") {
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  t.event(trk(0, o3os::audio::kFxTrkPanSlide, 0x04), p);
  for (unsigned i = 0; i < 6; ++i) {
    t.event(tick(i), p);
  }
  CHECK(t.pan(0) == 128 + 80);  // (starts at the centre: there is no strip to read)
  t.event(trk(0, o3os::audio::kFxTrkChanPan, 25), p);
  t.event(panning(0, true, 60), p);
  t.event(panning(0, false), p);
  CHECK(t.pan_owned(0));
  t.note_started(0, 50);        // harmless
}

// ---- CF2: the waveforms and the glissando, through the voice pool ------------------------------------------------------------------------------------------------------------------------

namespace {

using o3os::audio::FilterMod;

FilterMod lfo_mod(std::uint8_t shape, std::uint8_t rate, float amount) {
  FilterMod m;
  m.kind = o3os::audio::kFmodLfo;
  m.shape = shape;
  m.rate = rate;
  m.amount = amount;
  return m;
}

constexpr std::uint32_t kLen = 6000;
std::int16_t g_dc[kLen];    // a constant: what the tremolo does to it is the multiplier itself
std::int16_t g_rampv[kLen]; // 5 a frame: the slope of the output is the pitch increment

void init_pcm() {
  for (std::uint32_t i = 0; i < kLen; ++i) {
    g_dc[i] = 16000;
    g_rampv[i] = static_cast<std::int16_t>(5u * i);
  }
}

// The output of 40 blocks of a voice on track 0 whose instrument has the LFO `m` on `target` (0: the finetune LFO over the ramp, 1: the amp LFO over the constant), with the tracker FX's waveform `wave`
// (-1: none) set before the note (when `before`) or after it (when not).
std::vector<std::int16_t> lfo_run(unsigned target, std::uint8_t inst_shape, int wave, bool before, bool reset_after = false) {
  init_pcm();
  VoicePool& p = pool();
  SampleDesc d{target == 0u ? g_rampv : g_dc, kLen, 0, target == 0u ? 0u : kLen, 65536};
  (target == 0u ? d.tune_mod : d.amp_mod) = lfo_mod(inst_shape, 12, 0.8f);
  p.set_sample(0, d);
  p.restart_clock();
  p.on_step(0);
  if (wave >= 0 && before) {
    p.tracker_wave(0, target, static_cast<unsigned>(wave));
  }
  p.note_on(0, 60, 0, 127);
  if (wave >= 0 && !before) {
    p.tracker_wave(0, target, static_cast<unsigned>(wave));
  }
  if (reset_after) {
    p.tracker_reset();
  }
  std::vector<std::int16_t> out;
  for (unsigned b = 0; b < 40; ++b) {
    p.begin_block();
    p.render(0, kBlk);
    p.end_block();
    const std::int16_t* f = p.feed(0);
    for (std::uint32_t i = 0; i < kBlk; ++i) {
      out.push_back(f != nullptr ? f[i] : std::int16_t{0});
    }
  }
  return out;
}

}  // namespace

TEST_CASE("tracker fx CF2: the waveform FX sets the shape of the vibrato's and the tremolo's LFO, for the sounding voice and for the track's next notes, whatever the instrument says") {
  // tracker 0 sine, 1 ramp down, 2 square, 3 random -> the LFO shapes triangle 2, reverse saw 0, square 3, random 4; bit 2 (no retrigger) changes nothing
  const unsigned want[4] = {2, 0, 3, 4};
  for (unsigned target = 0; target < 2; ++target) {
    for (unsigned wave = 0; wave < 8; ++wave) {
      CAPTURE(target);
      CAPTURE(wave);
      const std::vector<std::int16_t> ref = lfo_run(target, static_cast<std::uint8_t>(want[wave & 3u]), -1, true);  // the instrument's own shape
      const std::vector<std::int16_t> live = lfo_run(target, 1, static_cast<int>(wave), false);                     // a saw instrument, the FX after the note
      const std::vector<std::int16_t> early = lfo_run(target, 1, static_cast<int>(wave), true);                     // the FX before the note
      CHECK(ref == live);
      CHECK(ref == early);
      const std::vector<std::int16_t> saw = lfo_run(target, 1, -1, true);
      CHECK(saw != ref);  // the shapes differ: the comparison above is not vacuous
    }
  }
  // the transport start ends it: the next note has the instrument's own shape again
  const std::vector<std::int16_t> saw = lfo_run(1, 1, -1, true);
  CHECK(lfo_run(1, 1, 2, true, true) == saw);
  CHECK(lfo_run(0, 1, 2, true, true) == lfo_run(0, 1, -1, true));
}

TEST_CASE("tracker fx CF2: the TrkFx events of the waveforms and the glissando act at once and end nothing; a bad wave is read modulo 8") {
  const std::vector<std::int16_t> ref = lfo_run(1, 3, -1, true);  // a square instrument (lfo_run makes the pool afresh: take `p` after it)
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  init_pcm();
  SampleDesc d{g_dc, kLen, 0, kLen, 65536};
  d.amp_mod = lfo_mod(1, 12, 0.8f);
  p.set_sample(0, d);
  p.restart_clock();
  p.on_step(0);
  t.event(trk(0, o3os::audio::kFxTrkTremWave, 2), p);  // square
  t.event(trk(0, 0, 0), p);                            // its row ends: nothing to take away
  p.note_on(0, 60, 0, 127);
  t.event(tick(0), p);                                 // no effect in force: the pass is idle
  std::vector<std::int16_t> out;
  for (unsigned b = 0; b < 40; ++b) {
    p.begin_block();
    p.render(0, kBlk);
    p.end_block();
    const std::int16_t* f = p.feed(0);
    for (std::uint32_t i = 0; i < kBlk; ++i) {
      out.push_back(f != nullptr ? f[i] : std::int16_t{0});
    }
  }
  CHECK(out == ref);
  t.event(trk(0, o3os::audio::kFxTrkVibWave, 0xFE), p);  // 0xFE & 3 = 2: no crash
  t.event(trk(9, o3os::audio::kFxTrkGliss, 1), p);
  t.event(trk(200, o3os::audio::kFxTrkGliss, 1), p);     // a track outside the 16: ignored
}

TEST_CASE("tracker fx CF2: glissando makes a slide ramp heard in whole semitones, and only a ramp") {
  init_pcm();
  const auto slopes = [](bool gliss) {
    VoicePool& p = pool();
    SampleDesc d{g_rampv, kLen, 0, 0u, 65536};
    p.set_sample(0, d);
    p.set_tempo_x100(30000);  // a step of about 2,200 samples: the slide below takes 34 blocks
    p.restart_clock();
    p.on_step(0);
    p.tracker_reset();
    p.tracker_gliss(0, gliss);
    p.note_on(0, 60, 0, 127);
    p.fx_event(0, o3os::audio::kModGroupSlide, o3os::audio::kFxSlideUp, 64);  // up 64 sixteenths = 4 semitones over a step
    std::vector<double> out;
    for (unsigned b = 0; b < 30; ++b) {
      p.begin_block();
      p.render(0, kBlk);
      p.end_block();
      const std::int16_t* f = p.feed(0);
      if (b >= 2u) {
        out.push_back((static_cast<double>(f[kBlk - 1u]) - static_cast<double>(f[1])) / (5.0 * static_cast<double>(kBlk - 2u)));  // the increment, 1 at the note's own pitch
      }
    }
    return out;
  };
  const std::vector<double> on = slopes(true), off = slopes(false);
  unsigned whole_on = 0, whole_off = 0;
  std::vector<int> seen;
  for (const double s : on) {
    const double semis = 12.0 * std::log2(s);
    const double k = std::round(semis);
    if (std::fabs(semis - k) < 0.1) {
      ++whole_on;
      if (seen.empty() || seen.back() != static_cast<int>(k)) {
        seen.push_back(static_cast<int>(k));
      }
    }
  }
  for (const double s : off) {
    const double semis = 12.0 * std::log2(s);
    whole_off += std::fabs(semis - std::round(semis)) < 0.1 ? 1u : 0u;
  }
  CHECK_MESSAGE(whole_on == on.size(), "every block of the glissando is at a whole semitone");
  CHECK(seen.size() >= 4u);  // 0, 1, 2, 3 (and 4) in order
  for (std::size_t i = 1; i < seen.size(); ++i) {
    CHECK(seen[i] > seen[i - 1]);
  }
  CHECK_MESSAGE(whole_off < off.size() / 2u, "without it the ramp passes between the semitones (" << whole_off << " of " << off.size() << " blocks are near one)");
  // a fine slide (the tracker FX's own pitch offset) is not a ramp: it is not rounded
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  SampleDesc d{g_rampv, kLen, 0, 0u, 65536};
  p.set_sample(0, d);
  p.tracker_gliss(0, true);
  p.note_on(0, 60, 0, 127);
  t.event(trk(0, o3os::audio::kFxTrkFinePortaUp, 120), p);  // 120 quarter units: 1.26 semitones, not a whole one
  t.event(tick(0), p);
  p.begin_block();
  p.render(0, kBlk);
  p.end_block();
  p.begin_block();
  p.render(0, kBlk);
  p.end_block();
  const std::int16_t* f = p.feed(0);
  const double inc = (static_cast<double>(f[kBlk - 1u]) - static_cast<double>(f[1])) / (5.0 * static_cast<double>(kBlk - 2u));
  const double semis = 12.0 * std::log2(inc);
  CHECK(semis > 1.0);
  CHECK(std::fabs(semis - std::round(semis)) > 0.15);
}

// ---- CF3: channel volume, global volume, panbrello ------------------------------------------------------------------------------------------------------------------------------------------

namespace {

SeqEvent stopped() {
  SeqEvent e{};
  e.kind = EventKind::Stopped;
  e.track = o3os::audio::kNoTrack;
  return e;
}
void run_row(TrackerFx& t, VoicePool& p, unsigned ticks = 6) {
  for (unsigned i = 0; i < ticks; ++i) {
    t.event(tick(i, ticks), p);
  }
}

}  // namespace

TEST_CASE("tracker fx CF3: the slides' steps are libopenmpt's (the channel's plain slide goes down first, the global one up first; xF fine up, Fy fine down, once on tick 0), the panbrello's sine is its table") {
  // channel volume slide N
  CHECK(trkfx::chan_vol_step(0x04, 0) == 0);
  CHECK(trkfx::chan_vol_step(0x04, 1) == -4);
  CHECK(trkfx::chan_vol_step(0x30, 1) == 3);
  CHECK(trkfx::chan_vol_step(0x30, 0) == 0);
  CHECK(trkfx::chan_vol_step(0x32, 1) == -2);  // both nibbles: y first
  CHECK(trkfx::chan_vol_step(0x3F, 0) == 3);   // xF: fine up x, once
  CHECK(trkfx::chan_vol_step(0x3F, 1) == 0);
  CHECK(trkfx::chan_vol_step(0xF2, 0) == -2);  // Fy: fine down y
  CHECK(trkfx::chan_vol_step(0xF2, 4) == 0);
  CHECK(trkfx::chan_vol_step(0xFF, 0) == 15);  // FF: the first form
  CHECK(trkfx::chan_vol_step(0x0F, 1) == -15);  // 0F: not a fine form (x = 0): a plain slide down by 15
  CHECK(trkfx::chan_vol_step(0x0F, 0) == 0);
  CHECK(trkfx::chan_vol_step(0xF0, 1) == 15);  // F0: a plain slide up by 15
  CHECK(trkfx::chan_vol_step(0xF0, 0) == 0);
  CHECK(trkfx::chan_vol_step(0x00, 3) == 0);
  // global volume slide W: x first
  CHECK(trkfx::global_vol_step(0x32, 1) == 3);
  CHECK(trkfx::global_vol_step(0x04, 1) == -4);
  CHECK(trkfx::global_vol_step(0x04, 0) == 0);
  CHECK(trkfx::global_vol_step(0x3F, 0) == 3);
  CHECK(trkfx::global_vol_step(0xF2, 0) == -2);
  CHECK(trkfx::global_vol_step(0xF2, 1) == 0);
  CHECK(trkfx::global_vol_step(0xF0, 1) == 15);
  CHECK(trkfx::global_vol_step(0x0F, 1) == -15);
  // the panbrello: round(127 sin(2 pi i / 64)) for the 64 points, and the delta by hand
  for (unsigned i = 0; i < 64; ++i) {
    CHECK_MESSAGE(trkfx::panbrello_sine(i) == std::lround(127.0 * std::sin(2.0 * 3.14159265358979 * static_cast<double>(i) / 64.0)), "point " << i);
  }
  CHECK(trkfx::panbrello_sine(16) == 127);
  CHECK(trkfx::panbrello_sine(48) == -127);
  CHECK(trkfx::panbrello_delta(0, 10) == (49 * 10 + 2) / 8);    // (0 + 16) >> 2 = 4: the table's 49
  CHECK(trkfx::panbrello_delta(0, 0) == 0);
  CHECK(trkfx::panbrello_delta(48, 15) == 238);    // (48 + 16) >> 2 = 16: the crest, (127 x 15 + 2) / 8
  CHECK(trkfx::panbrello_delta(176, 15) == -237);  // (176 + 16) >> 2 = 48: the trough, truncated toward zero as the C++ division of libopenmpt's
  CHECK(trkfx::panbrello_delta(10, 7) == trkfx::panbrello_delta(10u + 256u, 7));  // the position wraps at 256
}

TEST_CASE("tracker fx CF3: Channel volume (58) and Global volume (60) set the strip's gain, held over rows, reset by FxReset and Stopped; the slides move them, fine and plain") {
  Graph& g = make_graph();
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  t.attach(&g);
  CHECK(TrackerFx::takes(trk(3, o3os::audio::kFxTrkChanVol, 32)));
  CHECK(TrackerFx::takes(stopped()));
  CHECK(t.chan_volume(3) == 64u);
  CHECK(t.global_volume() == 64u);
  t.event(trk(3, o3os::audio::kFxTrkChanVol, 32), p);
  CHECK(t.chan_volume(3) == 32u);
  CHECK(g.track_gain(3) == doctest::Approx(0.5f));
  CHECK(g.track_gain(2) == doctest::Approx(1.0f));
  t.event(trk(3, 0, 0), p);  // the end of the row's effects ends nothing: a tracker's channel volume outlives the row
  run_row(t, p);
  CHECK(g.track_gain(3) == doctest::Approx(0.5f));
  t.event(trk(3, o3os::audio::kFxTrkChanVol, 200), p);  // held to 0..64
  CHECK(t.chan_volume(3) == 64u);
  CHECK(g.track_gain(3) == doctest::Approx(1.0f));
  // global volume: every track, the product with the channel's
  t.event(trk(3, o3os::audio::kFxTrkChanVol, 32), p);
  t.event(trk(0, o3os::audio::kFxTrkGlobalVol, 32), p);
  CHECK(t.global_volume() == 32u);
  CHECK(g.track_gain(0) == doctest::Approx(0.5f));
  CHECK(g.track_gain(3) == doctest::Approx(0.25f));
  CHECK(g.track_gain(15) == doctest::Approx(0.5f));
  t.event(trk(3, o3os::audio::kFxTrkChanVol, 0), p);  // a volume of 0 is an effect
  CHECK(g.track_gain(3) == 0.0f);
  // channel volume slide: down 4 a tick from tick 1 (y first), and held after the row
  t.event(trk(3, o3os::audio::kFxTrkChanVol, 40), p);
  t.event(trk(3, o3os::audio::kFxTrkChanVolSlide, 0x04), p);
  run_row(t, p);
  CHECK(t.chan_volume(3) == 40u - 5u * 4u);
  CHECK(g.track_gain(3) == doctest::Approx(20.0f / 64.0f * 0.5f));
  t.event(trk(3, 0, 0), p);
  run_row(t, p);
  CHECK(t.chan_volume(3) == 20u);  // the row after: held
  // fine up once on tick 0 (3F), then a plain slide up held at 64
  t.event(trk(3, o3os::audio::kFxTrkChanVolSlide, 0x3F), p);
  run_row(t, p);
  CHECK(t.chan_volume(3) == 23u);
  t.event(trk(3, o3os::audio::kFxTrkChanVolSlide, 0xF0), p);
  run_row(t, p);
  CHECK(t.chan_volume(3) == 64u);
  CHECK(g.track_gain(3) == doctest::Approx(0.5f));
  // the global slide: x up first, fine forms once; the gain of every track follows
  t.event(trk(3, o3os::audio::kFxTrkChanVol, 64), p);
  t.event(trk(0, o3os::audio::kFxTrkGlobalVol, 10), p);
  t.event(trk(7, o3os::audio::kFxTrkGlobalVolSlide, 0x31), p);  // x up first: +3 on ticks 1..5
  run_row(t, p);
  CHECK(t.global_volume() == 25u);
  CHECK(g.track_gain(0) == doctest::Approx(25.0f / 64.0f));
  CHECK(g.track_gain(11) == doctest::Approx(25.0f / 64.0f));
  t.event(trk(7, o3os::audio::kFxTrkGlobalVolSlide, 0xF4), p);  // fine down 4, once
  run_row(t, p);
  CHECK(t.global_volume() == 21u);
  t.event(trk(7, o3os::audio::kFxTrkGlobalVolSlide, 0x07), p);  // down 7 a tick from tick 1, held at 0
  run_row(t, p);
  CHECK(t.global_volume() == 0u);
  CHECK(g.track_gain(0) == 0.0f);
  // Stopped (the song ended at volume 0): the states are full again, but the strips keep their gain (the release tails must not swell back to full level) until the track's next note: the pads sound
  t.event(stopped(), p);
  CHECK(t.global_volume() == 64u);
  CHECK(t.chan_volume(3) == 64u);
  CHECK(g.track_gain(0) == 0.0f);
  CHECK(g.track_gain(3) == 0.0f);
  t.note_started(3, 50);  // a pad on track 3: that strip is at full, the others wait for their note
  CHECK(g.track_gain(3) == 1.0f);
  CHECK(g.track_gain(0) == 0.0f);
  t.note_started(3, 50);
  CHECK(g.track_gain(3) == 1.0f);
  t.event(fx_reset(), p);  // the transport start gives every strip back at once
  CHECK(g.track_gain(0) == 1.0f);
  // a stop at channel volume 8: the tail rings at 1/8, not full; a stop with every gain full leaves nothing stale
  t.event(trk(5, o3os::audio::kFxTrkChanVol, 8), p);
  t.event(stopped(), p);
  CHECK(g.track_gain(5) == doctest::Approx(0.125f));
  t.event(trk(5, o3os::audio::kFxTrkChanVol, 64), p);  // (a song would FxReset first; this one is held full by hand)
  t.event(fx_reset(), p);
  t.event(stopped(), p);
  t.note_started(5, 50);
  CHECK(g.track_gain(5) == 1.0f);
  // FxReset does the same
  t.event(trk(3, o3os::audio::kFxTrkChanVol, 10), p);
  t.event(trk(0, o3os::audio::kFxTrkGlobalVol, 10), p);
  t.event(fx_reset(), p);
  CHECK(t.chan_volume(3) == 64u);
  CHECK(t.global_volume() == 64u);
  CHECK(g.track_gain(3) == 1.0f);
  // without a graph the state is kept and nothing is applied
  t.attach(nullptr);
  t.event(trk(3, o3os::audio::kFxTrkChanVol, 10), p);
  CHECK(t.chan_volume(3) == 10u);
  t.event(trk(3, o3os::audio::kFxTrkChanVolSlide, 0x04), p);
  run_row(t, p);
  CHECK(t.chan_volume(3) == 0u);
  t.event(stopped(), p);
}

TEST_CASE("tracker fx CF3: Panbrello offsets the pan in force by libopenmpt's sine, speed and depth, keeps its position across rows and notes, remembers the nibbles and ends with its row") {
  Graph& g = make_graph();
  VoicePool& p = pool();
  TrackerFx& t = tfx();
  t.attach(&g);
  const float k = 100.0f / 256.0f;
  g.set_voice_pan(1, 80);  // a note's pan: the offset rides on whatever is in force and changes none of it
  CHECK(g.track_pan(1) == doctest::Approx(80.0f));
  t.event(trk(1, o3os::audio::kFxTrkPanbrello, 0x3A), p);  // speed 3, depth 10
  t.event(tick(0), p);
  CHECK(g.track_pan_mod(1) == doctest::Approx(61.0f * k));  // pos 0: table[(0 + 16) >> 2 = 4] = 49: (490 + 2) / 8
  t.event(tick(1), p);
  CHECK(g.track_pan_mod(1) == doctest::Approx(61.0f * k));  // pos 3: (19 >> 2) = 4
  t.event(tick(2), p);
  CHECK(g.track_pan_mod(1) == doctest::Approx(75.0f * k));  // pos 6: 5 -> 60: (600 + 2) / 8
  t.event(tick(3), p);
  CHECK(g.track_pan_mod(1) == doctest::Approx(89.0f * k));  // pos 9: 6 -> 71
  CHECK(g.track_pan(1) == doctest::Approx(80.0f));          // the pan in force is untouched
  // a row that carries a zero parameter goes on with the same speed and depth, from the kept position (12)
  t.event(trk(1, o3os::audio::kFxTrkPanbrello, 0x00), p);
  t.event(tick(0), p);
  CHECK(g.track_pan_mod(1) == doctest::Approx(static_cast<float>(trkfx::panbrello_delta(12, 10)) * k));
  // a zero nibble keeps the other: depth 1 with the speed 3 (pos 15)
  t.event(trk(1, o3os::audio::kFxTrkPanbrello, 0x01), p);
  t.event(tick(0), p);
  CHECK(g.track_pan_mod(1) == doctest::Approx(static_cast<float>(trkfx::panbrello_delta(15, 1)) * k));
  // the row after, without it: the offset is taken off
  t.event(trk(1, 0, 0), p);
  t.event(tick(0), p);
  CHECK(g.track_pan_mod(1) == 0.0f);
  // a new note does not start the position again (libopenmpt's S3M: measured): pos 18, depth 1
  t.note_started(1, 80);
  t.event(trk(1, o3os::audio::kFxTrkPanbrello, 0x01), p);
  t.event(tick(0), p);
  CHECK(g.track_pan_mod(1) == doctest::Approx(static_cast<float>(trkfx::panbrello_delta(18, 1)) * k));
  // another track has its own position, speed and depth (nothing set: no movement), and Channel pan is not changed by it
  t.event(trk(2, o3os::audio::kFxTrkChanPan, 20), p);
  t.event(trk(2, o3os::audio::kFxTrkPanbrello, 0x00), p);
  t.event(tick(0), p);
  CHECK(g.track_pan_mod(2) == 0.0f);
  CHECK(g.track_pan(2) == doctest::Approx(20.0f));
  // Stopped takes the offset off a row that was running
  t.event(trk(1, o3os::audio::kFxTrkPanbrello, 0x4F), p);
  t.event(tick(0), p);
  CHECK(g.track_pan_mod(1) != 0.0f);
  t.event(stopped(), p);
  CHECK(g.track_pan_mod(1) == 0.0f);
  // the full depth swings the pan across nearly the whole range, and the pan law holds the sum
  t.event(fx_reset(), p);
  t.event(trk(1, o3os::audio::kFxTrkPanbrello, 0x4F), p);
  float lo = 0.0f, hi = 0.0f;
  for (unsigned i = 0; i < 64; ++i) {
    t.event(tick(i % 6), p);
    lo = std::fmin(lo, g.track_pan_mod(1));
    hi = std::fmax(hi, g.track_pan_mod(1));
  }
  CHECK(hi == doctest::Approx(static_cast<float>((127 * 15 + 2) / 8) * k));
  CHECK(lo == doctest::Approx(static_cast<float>((-127 * 15 + 2) / 8) * k));
  t.attach(nullptr);
}
