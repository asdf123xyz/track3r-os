// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-SENDFX: the send effects INSIDE the graph and the engine (audio/graph.h, audio/engine.h; the effects themselves are audio_send_fx_test.cpp). What a person turns reaches what they hear:
// a track's SEND A / SEND B, the Routing page's SND A / SND B bits, the master DELAY / REVERB return levels and mutes, the project's SENDS pre/post setting, the effects' own parameters,
// the tempo, and an instrument's DELAY SEND / REVERB SEND at the note's start (and live), each through the ParamChange path and the real effects, to the TX block.
#include <doctest/doctest.h>

#include <o3os/audio/engine.h>
#include <o3os/audio/event_source.h>
#include <o3os/audio/graph.h>
#include <o3os/audio/send_delay.h>
#include <o3os/audio/send_reverb.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include "support/static_slot.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

namespace ha = o3os::hal::audio;
using namespace o3os::audio;
namespace gps = o3os::audio::graph_param::strip;
namespace gpm = o3os::audio::graph_param::master;
namespace sx = o3os::audio::sendfx;

constexpr std::uint32_t kB = ha::kBlock;
constexpr std::uint32_t kBS = ha::kBlockSamples;

std::uint32_t g_cycles = 0;
std::uint32_t fake_cycles() noexcept { return g_cycles += 3u; }

// The two effects of the image, over memory of the test's own (the delay's ring is 20 ms short of 0.5 s: the times the cases use are tens of milliseconds).
constexpr std::uint32_t kRingFrames = 32768;
SendDelay& delay_fx() {
  static o3os::test::StaticSlot<SendDelay> slot;
  return slot.make();
}
SendReverb& reverb_fx() {
  static o3os::test::StaticSlot<SendReverb> slot;
  return slot.make();
}
std::vector<std::uint32_t>& delay_ring() {
  static std::vector<std::uint32_t> ring;
  ring.assign(kRingFrames, 0u);
  return ring;
}
std::vector<std::int16_t>& reverb_mem() {
  static std::vector<std::int16_t> mem;
  mem.assign(SendReverb::kMemSamples, 0);
  return mem;
}

// A graph with both effects installed the way the platform installs them (un-bypassed), and one track fed from a block the test fills.
struct Rig {
  explicit Rig(bool with_fx = true) : taps(), g(graph_slot.make(&fake_cycles, taps)), d(delay_fx()), r(reverb_fx()) {
    g_cycles = 0;
    if (with_fx) {
      d.attach(delay_ring().data(), kRingFrames);
      r.attach(reverb_mem().data(), SendReverb::kMemSamples);
      g.install_send_fx(0, &SendDelay::process_fx, &d, &SendDelay::param_fx);
      g.set_send_fx_bypass(0, false);
      g.install_send_fx(1, &SendReverb::process_fx, &r, &SendReverb::param_fx);
      g.set_send_fx_bypass(1, false);
    }
    strip(0, gps::Source, 3.0f);  // the feed
    g.feed(0, feed);
  }
  void set(o3os::core::ParamId id, float v) { CHECK(g.params().push(ParamChange{id, v, 1u})); }
  void strip(unsigned slot, unsigned field, float v) { set(strip_param(slot, field), v); }
  void master(unsigned field, float v) { set(master_param(field), v); }
  // One block: feed `in` (kB samples, or silence), returns the interleaved TX block in `out`.
  void render(const std::int16_t* in = nullptr) {
    for (std::uint32_t i = 0; i < kB; ++i) feed[i] = in != nullptr ? in[i] : std::int16_t{0};
    const ha::BlockIo io{rx, out, 0, &report};
    g.render(io);
    for (std::uint32_t i = 0; i < kB; ++i) {
      l.push_back(out[2u * i]);
      rr.push_back(out[2u * i + 1u]);
    }
  }
  // An impulse of kA at frame `at` of the first block, then `blocks - 1` of silence.
  void impulse(unsigned blocks, std::uint32_t at = 5, std::int16_t a = 20000) {
    std::int16_t first[kB] = {};
    first[at] = a;
    render(first);
    for (unsigned b = 1; b < blocks; ++b) render();
  }
  std::int64_t sum_abs(std::size_t from, std::size_t to) const {
    std::int64_t s = 0;
    for (std::size_t i = from; i < to && i < l.size(); ++i) s += std::abs(l[i]) + std::abs(rr[i]);
    return s;
  }
  static o3os::test::StaticSlot<Graph> graph_slot;
  TapMem taps;
  Graph& g;
  SendDelay& d;
  SendReverb& r;
  alignas(8) std::int16_t rx[kBS] = {};
  alignas(8) std::int16_t out[kBS] = {};
  alignas(8) std::int16_t feed[kB] = {};
  ha::BlockReport report = {};
  std::vector<std::int16_t> l, rr;
};
o3os::test::StaticSlot<Graph> Rig::graph_slot;

// round(ms x fs / 1000) frames, as the delay's own arithmetic is (exact integers).
std::uint32_t frames_of_ms(unsigned ms) {
  const std::uint64_t num = static_cast<std::uint64_t>(ms) * ha::sample_rate_mhz();
  return static_cast<std::uint32_t>((2u * num + 1000000u) / 2000000u);
}

}  // namespace

TEST_CASE("send fx in the graph: a track's send reaches the delay's return after the exact time; the routing bit, the send level, the return level and the mutes each close it") {
  const std::uint32_t d = frames_of_ms(20);  // 882
  struct Case {
    const char* name;
    bool heard;
    unsigned route;
    float send;
    float ret_level;
    bool delay_mute;
    bool track_mute;
  };
  const Case cases[] = {
      {"open", true, route::kSndA, 100.0f, 100.0f, false, false},
      {"SND A bit clear", false, route::kRec, 100.0f, 100.0f, false, false},
      {"send level 0", false, route::kSndA, 0.0f, 100.0f, false, false},
      {"DELAY return level 0", false, route::kSndA, 100.0f, 0.0f, false, false},
      {"DELAY MUTE", false, route::kSndA, 100.0f, 100.0f, true, false},
      {"track muted (this OS's choice: a muted track's sends are silent)", false, route::kSndA, 100.0f, 100.0f, false, true},
  };
  for (const Case& c : cases) {
    Rig rig;
    rig.strip(0, gps::Route, static_cast<float>(c.route));  // the dry is not in it: only the send
    rig.strip(0, gps::SendA, c.send);
    rig.master(gpm::Delay, c.ret_level);
    rig.master(gpm::DelayMute, c.delay_mute ? 1.0f : 0.0f);
    rig.strip(0, gps::Mute, c.track_mute ? 1.0f : 0.0f);
    rig.master(gpm::DelayTime, 20.0f);
    rig.master(gpm::DelayFeedback, 0.0f);
    rig.impulse(20);  // 1,280 frames: the repeat is at 887
    INFO(c.name);
    CHECK(rig.g.params_rejected() == 0u);
    if (c.heard) {
      CHECK(std::abs(rig.l[5 + d] - 20000) <= 4);  // the send is at unity, the return at unity, MAIN at unity
      CHECK(std::abs(rig.rr[5 + d] - 20000) <= 4);
      CHECK(rig.l[5 + d - 1] == 0);
      CHECK(rig.l[5 + d + 1] == 0);
      CHECK(rig.sum_abs(0, 5 + d) == 0);
    } else {
      CHECK(rig.sum_abs(0, rig.l.size()) == 0);
    }
  }
}

TEST_CASE("send fx in the graph: the delay's own parameters, the sync rate and the tempo, arrive through the ParamChange ring") {
  Rig rig;
  rig.strip(0, gps::Route, static_cast<float>(route::kSndA));
  rig.strip(0, gps::SendA, 100.0f);
  rig.master(gpm::DelayFeedback, 0.0f);
  rig.master(gpm::DelaySync, 1.0f);
  rig.master(gpm::DelayRate, 11.0f);  // 24/96 of a step: 1/4 step = 0.0625 beats
  rig.g.set_send_fx_tempo(14000);     // 140 BPM, as the engine tells it
  rig.impulse(3);
  const std::uint64_t num = static_cast<std::uint64_t>(sx::kDelayRates96[11]) * 1500u * ha::sample_rate_mhz();
  const std::uint64_t den = 96000u * 14000u;
  const std::uint32_t want = static_cast<std::uint32_t>((2u * num + den) / (2u * den));  // 1182 frames
  CHECK(rig.g.params_rejected() == 0u);
  CHECK(rig.d.delay_left() == want);
  for (unsigned b = 0; b < 30; ++b) rig.render();
  CHECK(std::abs(rig.l[5 + want] - 20000) <= 4);
  // a refused value is counted, and changes nothing
  const std::uint32_t before = rig.g.params_rejected();
  rig.master(gpm::DelayFeedback, 150.0f);
  rig.master(gpm::DelayRate, 19.0f);
  rig.render();
  CHECK(rig.g.params_rejected() == before + 2u);
  CHECK(rig.d.delay_left() == want);
}

TEST_CASE("send fx in the graph: a parameter of an effect nobody installed is refused; the reverb takes its four controls and its quality") {
  {
    Rig bare(false);
    bare.master(gpm::ReverbSize, 70.0f);
    bare.master(gpm::DelayTime, 70.0f);
    bare.render();
    CHECK(bare.g.params_rejected() == 2u);
  }
  Rig rig;
  for (const unsigned f : {gpm::ReverbSize, gpm::ReverbDamp, gpm::ReverbPredelay, gpm::ReverbDiffusion}) rig.master(f, 30.0f);
  rig.master(gpm::ReverbQuality, 1.0f);
  rig.render();
  CHECK(rig.g.params_rejected() == 0u);
  rig.render();  // the quality is taken at the next block (the reverb applies it when it runs)
  rig.impulse(2);
  CHECK_FALSE(rig.r.half_rate());
  rig.master(gpm::ReverbSize, 101.0f);
  rig.render();
  CHECK(rig.g.params_rejected() == 1u);
}

TEST_CASE("send fx in the graph: a track's send reaches the reverb's return; the routing bit, the return level, the mutes and a solo elsewhere close it") {
  struct Case {
    const char* name;
    bool heard;
    unsigned route;
    float ret_level;
    bool reverb_mute;
    bool solo_other;
  };
  const Case cases[] = {
      {"open", true, route::kSndB, 100.0f, false, false},
      {"SND B bit clear", false, route::kRec, 100.0f, false, false},
      {"REVERB return level 0", false, route::kSndB, 0.0f, false, false},
      {"REVERB MUTE", false, route::kSndB, 100.0f, true, false},
      {"another track soloed (a live solo mutes the sends too: our choice)", false, route::kSndB, 100.0f, false, true},
  };
  for (const Case& c : cases) {
    Rig rig;
    rig.strip(0, gps::Route, static_cast<float>(c.route));
    rig.strip(0, gps::SendB, 100.0f);
    rig.master(gpm::Reverb, c.ret_level);
    rig.master(gpm::ReverbMute, c.reverb_mute ? 1.0f : 0.0f);
    if (c.solo_other) rig.strip(1, gps::Solo, 1.0f);
    rig.impulse(120);  // 0.17 s
    INFO(c.name);
    CHECK(rig.g.params_rejected() == 0u);
    if (c.heard) {
      std::int64_t left = 0, right = 0;
      for (std::size_t i = 0; i < rig.l.size(); ++i) {
        left += std::abs(rig.l[i]);
        right += std::abs(rig.rr[i]);
      }
      CHECK(left > 20000);   // a plate's response: spread over time, both sides, and different
      CHECK(right > 20000);
      CHECK(rig.l != rig.rr);
    } else {
      CHECK(rig.sum_abs(0, rig.l.size()) == 0);
    }
  }
}

TEST_CASE("send fx in the graph: the project's SENDS setting takes the send after the fader; the strip's volume then scales the return, and before it does not") {
  const std::uint32_t d = frames_of_ms(20);
  std::int32_t echo[2] = {0, 0};
  std::int32_t gain = 0;
  for (unsigned post = 0; post < 2; ++post) {
    Rig rig;
    rig.strip(0, gps::Route, static_cast<float>(route::kSndA));
    rig.strip(0, gps::SendA, 100.0f);
    rig.strip(0, gps::Volume, 50.0f);
    rig.master(gpm::DelayTime, 20.0f);
    rig.master(gpm::DelayFeedback, 0.0f);
    rig.set(o3os::core::param_id(o3os::core::ParamKind::Project, 0, graph_param::project::SendsMode), static_cast<float>(post));
    rig.impulse(20);
    CHECK(rig.g.params_rejected() == 0u);
    CHECK(rig.g.sends_post_fader() == (post != 0u));
    echo[post] = rig.l[5 + d];
    gain = rig.g.level_curve().q16(50.0f);  // the fader's gain, Q16
  }
  CHECK(std::abs(echo[0] - 20000) <= 4);  // pre-fader (the default): the fader at 50 does not touch the send
  const std::int32_t want = static_cast<std::int32_t>((static_cast<std::int64_t>(20000) * gain) >> 16);
  CHECK_MESSAGE(std::abs(echo[1] - want) <= 8, "echo " << echo[1] << " want " << want << " gain " << gain);
  CHECK(echo[1] < echo[0] - 1000);
  // a setting the graph does not know is refused, the sequencer's hook is not asked for this one
  Rig rig;
  rig.set(o3os::core::param_id(o3os::core::ParamKind::Project, 0, graph_param::project::SendsMode), 2.0f);
  rig.render();
  CHECK(rig.g.params_rejected() == 1u);
}

TEST_CASE("send fx in the graph: the instrument's sends are the track's sends from the note's start, and a live edit reaches the playing note (the engine)") {
  static std::int16_t dc[3000];
  for (std::int16_t& v : dc) v = 16384;
  static o3os::test::StaticSlot<Engine> engine_slot;
  static o3os::audio::TapMem taps;
  static o3os::test::StaticSlot<VoicePool> pool_slot;
  static SeqEvent events[o3os::audio::kMaxEventsPerBlock];
  struct Script {
    static std::uint32_t step(void*, std::uint64_t block_start, std::uint32_t n, SeqEvent* out, std::uint32_t cap) noexcept {
      if (block_start / n == 1u && cap > 0u) {  // block 1: a note on the instrument in slot 0, at sample 0
        SeqEvent e{};
        e.offset = 0;
        e.kind = EventKind::NoteOn;
        e.track = 0;
        e.a = 60;
        e.b = 0;
        e.c = 127;
        out[0] = e;
        return 1;
      }
      return 0;
    }
  };
  for (unsigned mode = 0; mode < 3; ++mode) {  // 0: the instrument has no send; 1: DELAY SEND 100 from the start; 2: 0 at the start, a live edit to 100 two blocks in
    Engine& e = engine_slot.make(&fake_cycles, taps);
    VoicePool& p = pool_slot.make();
    SampleDesc desc{dc, 3000, 0, 0, 65536};
    desc.delay_send = static_cast<std::uint8_t>(mode == 1u ? 100 : 0);
    p.set_sample(0, desc);
    e.attach_voices(&p);
    EventSource src;
    src.step = &Script::step;
    src.buffer = events;
    src.capacity = o3os::audio::kMaxEventsPerBlock;
    e.attach_events(src);
    SendDelay& dly = delay_fx();
    dly.attach(delay_ring().data(), kRingFrames);
    e.graph().install_send_fx(0, &SendDelay::process_fx, &dly, &SendDelay::param_fx);
    e.graph().set_send_fx_bypass(0, false);
    // the route is the send alone: SND A, no MAIN (so what is heard is the delay's return); the delay's repeat is not wanted
    CHECK(e.params().push(ParamChange{strip_param(0, gps::Route), static_cast<float>(route::kSndA), 1u}));
    CHECK(e.params().push(ParamChange{master_param(gpm::DelayTime), 10.0f, 1u}));
    CHECK(e.params().push(ParamChange{master_param(gpm::DelayFeedback), 0.0f, 1u}));
    alignas(8) std::int16_t in[kBS] = {};
    alignas(8) std::int16_t out[kBS] = {};
    ha::BlockReport rep = {};
    std::int64_t heard = 0;
    for (unsigned b = 0; b < 14; ++b) {
      if (mode == 2u && b == 3u) {
        CHECK(e.params().push(ParamChange{o3os::core::param_id(o3os::core::ParamKind::Instrument, 0, inst_param::DelaySend), 100.0f, 1u}));
      }
      rep = ha::BlockReport{};
      const ha::BlockIo io{in, out, 0, &rep};
      e.render_block(io);
      for (std::uint32_t i = 0; i < kBS; ++i) heard += std::abs(out[i]);
    }
    INFO("mode " << mode);
    CHECK(e.graph().params_rejected() == 0u);
    if (mode == 0u) {
      CHECK(heard == 0);
    } else {
      CHECK(heard > 100000);  // the note's body, delayed, is in the return (a DC of 16384 at the strip's gain for several blocks)
    }
  }
}

TEST_CASE("send fx in the graph: an instrument's send sounds on the DEFAULT route (no SND bit), as an OG project needs; the mutes close it, the strip's own send stays gated by the bit") {
  const std::uint32_t d = frames_of_ms(20);
  struct Case {
    const char* name;
    unsigned delay_send;
    bool delay_mute;
    bool track_mute;
    float own_send;  // the strip's own SendA, which the default route's missing bit keeps shut
    bool heard;
  };
  const Case cases[] = {
      {"instrument send 100", 100, false, false, 0.0f, true},
      {"instrument send 0", 0, false, false, 0.0f, false},
      {"DELAY MUTE", 100, true, false, 0.0f, false},
      {"track muted", 100, false, true, 0.0f, false},
      {"the strip's own send, no bit", 0, false, false, 100.0f, false},
  };
  for (const Case& c : cases) {
    Rig rig;  // the route stays the default, MAIN | USB
    rig.g.set_voice_sends(0, c.delay_send, 0);
    rig.strip(0, gps::SendA, c.own_send);
    rig.master(gpm::DelayMute, c.delay_mute ? 1.0f : 0.0f);
    rig.strip(0, gps::Mute, c.track_mute ? 1.0f : 0.0f);
    rig.master(gpm::DelayTime, 20.0f);
    rig.master(gpm::DelayFeedback, 0.0f);
    rig.impulse(20);
    INFO(c.name);
    CHECK(rig.g.params_rejected() == 0u);
    if (c.heard) {
      CHECK(std::abs(rig.l[5] - 20000) <= 4);  // the dry, MAIN
      CHECK(std::abs(rig.l[5 + d] - 20000) <= 4);  // and the delay's repeat on top of silence
      CHECK(std::abs(rig.rr[5 + d] - 20000) <= 4);
    } else {
      CHECK(rig.l[5 + d] == 0);
      CHECK(rig.rr[5 + d] == 0);
    }
  }
  // the reverb the same way: instrument send 100 on the default route fills the return
  Rig rig;
  rig.g.set_voice_sends(0, 0, 100);
  rig.impulse(120);
  std::int64_t after_dry = 0;
  for (std::size_t i = 64; i < rig.l.size(); ++i) after_dry += std::abs(rig.l[i]);
  CHECK(after_dry > 20000);
}

TEST_CASE("send fx in the graph: a project load forgets the instrument sends and the effects' memory (ParamChanges VoiceSendsReset, DelayClear, ReverbClear)") {
  const std::uint32_t d = frames_of_ms(20);
  // the sends of the last notes: heard before, gone after a reset alone (the route is the send's alone, no MAIN, so only the effect's return is heard)
  {
    Rig rig;
    rig.strip(0, gps::Route, static_cast<float>(route::kRec));  // no SND bit: the instrument's send does not need one
    rig.g.set_voice_sends(0, 100, 0);
    rig.master(gpm::DelayTime, 20.0f);
    rig.master(gpm::DelayFeedback, 0.0f);
    rig.impulse(20);
    CHECK(std::abs(rig.l[5 + d] - 20000) <= 4);  // it sounds before the load
    rig.master(gpm::VoiceSendsReset, 2.0f);        // not a reset: refused
    rig.render();
    CHECK(rig.g.params_rejected() == 1u);
    rig.master(gpm::VoiceSendsReset, 1.0f);
    rig.render();
    const std::size_t from = rig.l.size();
    rig.impulse(20);
    CHECK(rig.sum_abs(from, rig.l.size()) == 0);  // nothing is sent now
    CHECK(rig.g.params_rejected() == 1u);
  }
  // the effects' memory: a tail that is ringing is cut, the return is silent while the slices are cleared, and the effects work again after
  {
    Rig rig;
    rig.strip(0, gps::Route, static_cast<float>(route::kRec));
    rig.g.set_voice_sends(0, 100, 100);
    rig.master(gpm::DelayTime, 20.0f);
    rig.master(gpm::DelayFeedback, 70.0f);
    rig.impulse(100);  // the delay's repeats (every 20 ms) and the reverb's tail are ringing
    CHECK(rig.sum_abs(0, rig.l.size()) > 100000);
    rig.master(gpm::VoiceSendsReset, 1.0f);
    rig.master(gpm::DelayClear, 1.0f);
    rig.master(gpm::ReverbClear, 1.0f);
    rig.render();
    CHECK(rig.d.clearing());
    CHECK(rig.r.clearing());
    const std::size_t from = rig.l.size();
    for (unsigned b = 0; b < 60; ++b) rig.render();  // the delay's ring is 32,768 frames here: 16 slices; the reverb's 14
    CHECK_FALSE(rig.d.clearing());
    CHECK_FALSE(rig.r.clearing());
    CHECK(rig.sum_abs(from, rig.l.size()) == 0);  // none of the old tail, and no send to make a new one
    CHECK(rig.d.idle());
    CHECK(rig.r.idle());
  }
}

// An effect that only keeps what it was given, for the ramp of a live send.
struct RecFx {
  alignas(8) std::int16_t last[kB] = {};
};
void rec_fx(void* state, const std::int16_t* in, std::int16_t* out_l, std::int16_t* out_r, std::uint32_t n) noexcept {
  RecFx& f = *static_cast<RecFx*>(state);
  for (std::uint32_t i = 0; i < n; ++i) {
    f.last[i] = in[i];
    out_l[i] = 0;
    out_r[i] = 0;
  }
}

TEST_CASE("send fx in the graph: a note's send is instant, a live edit of an instrument's send is smoothed over the block like the other knobs") {
  for (const std::uint32_t ramp : {1u, 0u}) {
    Rig rig(false);
    RecFx rec;
    rig.g.install_send_fx(0, &rec_fx, &rec);
    rig.g.set_send_fx_bypass(0, false);
    std::int16_t dc[kB];
    for (std::int16_t& v : dc) v = 10000;
    rig.render(dc);  // settled at no send
    rig.g.set_voice_sends(0, 100, 0, ramp);
    rig.render(dc);
    INFO("ramp " << ramp);
    if (ramp == 1u) {
      CHECK(std::abs(rec.last[0] - 10000) <= 2);  // from the first sample
      CHECK(std::abs(rec.last[kB - 1] - 10000) <= 2);
    } else {
      CHECK(rec.last[0] < 1000);  // a ramp over the block
      CHECK(rec.last[kB - 1] > 9000);
      CHECK(rec.last[kB / 2] > 3000);
      CHECK(rec.last[kB / 2] < 7000);
    }
  }
}
