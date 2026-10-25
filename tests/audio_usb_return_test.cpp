// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-006e: the USB return's own route to the outputs, and the level of the input strip against the master DRY fader (graph.h, "USB return"), on the host and on the Cortex-M4F
// (`qemu_audio_graph`). The graph is driven block by block with a -6 dBFS 1 kHz tone, the level the HIL test sent (usb_return_test.py gen), and the level that comes out is measured:
//   * with the monitor at 0 (the default) a host that plays into the Tracker is heard on the outputs at the level it sent, to 0.5 dB, through the master limiter as the platform installs it;
//   * NO LOOPBACK: it is in no capture: the stems (all ten channels, Master L / R included), RecTap::Source::Master, the REC bus, CUE and the MAIN meters read nothing of it, while the
//     master meters (what is heard) read it;
//   * that level is the return's own: it follows the ParamChange master::UsbReturnLevel (the mixer level law), and the master DRY level, DRY MUTE and the input strip's level do not
//     move it; the strip is the codec alone;
//   * the recorder's USB source takes it over from the route without a step (the route ramps over a block, both ways);
//   * the level the HIL measured 5.6 dB low: `mon on` (70, -12 dB) of a -6 dBFS input reads -18 dBFS, whatever the DRY level (86 in every v7+ project); a track is still scaled by it.
#include <doctest/doctest.h>

#include <o3os/audio/graph.h>
#include <o3os/audio/master_limiter.h>
#include <o3os/audio/meters.h>
#include <o3os/hal/audio.h>
#include <o3os/usb/stream.h>

#include "support/static_slot.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace {

namespace ha = o3os::hal::audio;
using namespace o3os::audio;
namespace gps = o3os::audio::graph_param::strip;
namespace gpm = o3os::audio::graph_param::master;

constexpr std::uint32_t kB = ha::kBlock;
constexpr std::uint32_t kBS = ha::kBlockSamples;
constexpr double kPi = 3.14159265358979323846;
constexpr double kHz = 1000.0;
constexpr double kRate = 44100.0;
constexpr double kSentPeak = 16384.0;  // -6.02 dBFS: the tone usb_return_test.py gen writes (0.5 of full scale)
constexpr double kDbTolerance = 0.5;

std::uint32_t g_cycles = 0;
std::uint32_t fake_cycles() noexcept { return ++g_cycles; }

Graph& make_graph() {
  static o3os::test::StaticSlot<Graph> slot;  // not on the target's stack
  static TapMem taps;
  g_cycles = 0;
  return slot.make(&fake_cycles, taps);
}
MasterLimiter& make_limiter() {
  static o3os::test::StaticSlot<MasterLimiter> slot;
  return slot.make();
}

double tone(std::uint32_t n) { return kSentPeak * std::sin(2.0 * kPi * kHz * static_cast<double>(n) / kRate); }
double db(double ratio) { return 20.0 * std::log10(ratio); }

struct Rig {
  Graph& g;
  alignas(8) std::int16_t in[kBS] = {};
  alignas(8) std::int16_t out[kBS] = {};
  alignas(8) std::int16_t feed_[kTracks][kB] = {};
  ha::BlockReport report = {};
  o3os::usb::Counters counters;
  o3os::usb::ReturnReader reader;
  std::uint32_t sent = 0;  // frames of the host's stream written so far
  explicit Rig(Graph& graph, o3os::usb::ReturnRing& ring) : g(graph), reader(ring, counters), ring_(ring) {}

  void render(std::uint32_t flags = 0) {
    report = ha::BlockReport{};
    const ha::BlockIo io{in, out, flags, &report};
    g.render(io);
  }
  void set(o3os::core::ParamId id, float v) { CHECK(g.params().push(ParamChange{id, v, 1})); }
  void strip(unsigned slot, unsigned field, float v) { set(strip_param(slot, field), v); }
  void master(unsigned field, float v) { set(master_param(field), v); }

  // The host writes `frames` frames of the tone on both channels (right inverted, so a channel mix-up shows), in chunks the ring takes.
  void host_writes(std::uint32_t frames, double gain = 1.0) {
    std::uint32_t chunk[32];
    for (std::uint32_t done = 0; done < frames;) {
      const std::uint32_t n = frames - done < 32u ? frames - done : 32u;
      for (std::uint32_t i = 0; i < n; ++i) {
        const std::int32_t v = static_cast<std::int32_t>(std::lround(tone(sent++) * gain));
        chunk[i] = static_cast<std::uint16_t>(v) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(-v)) << 16);
      }
      ring_.write(chunk, n);
      done += n;
    }
  }
  // The host writes `frames` frames of a constant: left l, right r.
  void host_dc_lr(std::uint32_t frames, std::int32_t l, std::int32_t r) {
    std::uint32_t chunk[32];
    for (std::uint32_t i = 0; i < 32u; ++i) {
      chunk[i] = static_cast<std::uint16_t>(l) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(r)) << 16);
    }
    for (std::uint32_t done = 0; done < frames; done += 32u) {
      ring_.write(chunk, frames - done < 32u ? frames - done : 32u);
    }
  }
  void host_dc(std::uint32_t frames, std::int32_t v) { host_dc_lr(frames, v, -v); }  // left v, right -v
  // The host streams: the ring is primed, then one block is written before each render (what the isochronous stream does, 64 frames a block).
  void start_host() {
    g.attach_return(&reader);
    g.set_return_enabled(true);
    host_writes(288);
  }
  // The codec's RX is the tone too (the line-in of the monitor tests), on the left only.
  void rx_tone(std::uint32_t block) {
    for (std::uint32_t i = 0; i < kB; ++i) {
      in[2u * i] = static_cast<std::int16_t>(std::lround(tone(block * kB + i)));
      in[2u * i + 1u] = 0;
    }
  }
  // Renders `blocks` blocks with the host streaming (when it does) and the RX tone (when asked); the peak of the left and right output samples of the last `tail` blocks.
  struct Peaks {
    std::int32_t left = 0;
    std::int32_t right = 0;
  };
  Peaks run(unsigned blocks, unsigned tail, bool host, bool codec) {
    Peaks p;
    for (unsigned b = 0; b < blocks; ++b) {
      if (host) host_writes(kB);
      if (codec) rx_tone(b);
      render();
      if (b + tail >= blocks) {
        for (std::uint32_t i = 0; i < kB; ++i) {
          const std::int32_t l = out[2u * i] < 0 ? -out[2u * i] : out[2u * i];
          const std::int32_t r = out[2u * i + 1u] < 0 ? -out[2u * i + 1u] : out[2u * i + 1u];
          p.left = l > p.left ? l : p.left;
          p.right = r > p.right ? r : p.right;
        }
      }
    }
    return p;
  }

 private:
  o3os::usb::ReturnRing& ring_;
};

o3os::usb::ReturnRing& fresh_ring() {
  static o3os::usb::ReturnRing ring;
  ring.flush();
  return ring;
}

bool within(double measured_peak, double expected_peak) { return std::fabs(db(measured_peak / expected_peak)) <= kDbTolerance; }

}  // namespace

TEST_CASE("usb return: a host that plays into the Tracker is heard on the outputs at the level it sent, with the monitor at 0 (the default)") {
  Rig rig(make_graph(), fresh_ring());
  MasterLimiter& limiter = make_limiter();
  rig.g.install_master_limiter(limiter);  // as the platform installs it: the chain is active and MAIN goes through the float planes
  rig.start_host();
  const Rig::Peaks p = rig.run(70, 12, true, false);
  CHECK(within(static_cast<double>(p.left), kSentPeak));
  CHECK(within(static_cast<double>(p.right), kSentPeak));
  // the master meters read what is heard; the MAIN meters (as MAIN arrives at the limiter) read nothing: the return is not in MAIN
  MeterSnapshot snap;
  CHECK(rig.g.meters().snapshot(snap));
  CHECK(snap.windows != 0u);
  CHECK(within(static_cast<double>(meter_level(snap, meter_ch::kMasterL).hold) * 32768.0, kSentPeak));
  CHECK(within(static_cast<double>(meter_level(snap, meter_ch::kMasterR).hold) * 32768.0, kSentPeak));
  CHECK(meter_level(snap, meter_ch::kMainL).hold == 0.0f);
  CHECK(meter_level(snap, meter_ch::kMainR).hold == 0.0f);
  CHECK(rig.g.level_gain(kInputStrip) == 0);  // the monitor is at 0: nothing of this went through the input strip
  CHECK(limiter.gain() == 1.0f);              // the limiter had nothing to catch (and does not see the return)
}

TEST_CASE("usb return: no loopback by default: the stems (Master L / R too), REC-Master, the REC bus and CUE have none of it while it is heard") {
  for (const OutMode mode : {OutMode::Stereo, OutMode::MainLCueR}) {
    Rig rig(make_graph(), fresh_ring());
    rig.g.install_master_limiter(make_limiter());
    const bool stereo = mode == OutMode::Stereo;
    rig.master(gpm::OutMode, stereo ? 0.0f : 1.0f);
    rig.g.stems().set_enabled(true);  // P10: T1..T8, Master L / R
    o3os::usb::StemReader stems(rig.g.stems().ring());
    rig.g.attach_return(&rig.reader);
    rig.g.set_return_enabled(true);
    // the host streams a constant (left = right, so the mono sum of MainLCueR is not zero): 8000, 0.24 of full scale
    rig.host_dc_lr(288, 8000, 8000);
    const auto blocks = [&](unsigned n) {
      unsigned stem_blocks = 0;
      for (unsigned b = 0; b < n; ++b) {
        rig.host_dc_lr(kB, 8000, 8000);
        rig.render();
        alignas(4) std::int16_t block[o3os::usb::kStemBlockFrames * 10];
        while (stems.read_interleaved(block, o3os::usb::kStemBlockFrames, 10) == o3os::usb::kStemBlockFrames) {
          ++stem_blocks;
          for (const std::int16_t v : block) {
            CHECK(v == 0);  // every channel of every stem block, the master pair included
          }
        }
      }
      return stem_blocks;
    };
    CHECK(blocks(8) >= 3u);
    alignas(4) static std::int16_t take[kB * 4];
    for (std::int16_t& v : take) {
      v = 0x5555;
    }
    rig.g.rec().set_source(RecTap::Source::Master);
    CHECK(rig.g.rec().arm(take, kB * 4, 1));
    CHECK(blocks(8) >= 3u);
    CHECK(rig.g.rec().state() == RecTap::State::Done);
    for (const std::int16_t v : take) {
      CHECK(v == 0);  // REC-Master is MAIN after the master level: without the return
    }
    // and it is heard: the left jack (the left, or the mono sum of the pair), and the right of MainLCueR is CUE: silent
    CHECK(rig.out[2 * 10] == 8000);
    CHECK(rig.out[2 * 10 + 1] == (stereo ? 8000 : 0));
    MeterSnapshot snap;
    CHECK(rig.g.meters().snapshot(snap));
    for (const unsigned ch : {meter_ch::kMainL, meter_ch::kMainR, meter_ch::kCueL, meter_ch::kCueR, meter_ch::kRecL, meter_ch::kRecR}) {
      CHECK(meter_level(snap, ch).hold == 0.0f);
    }
  }
}

TEST_CASE("usb return: unity is exact (a constant stream comes out as it went in, left and right kept apart), and nothing is added when no host streams") {
  Rig rig(make_graph(), fresh_ring());  // no limiter: the sum goes through the bus' own rounding
  rig.g.attach_return(&rig.reader);
  rig.render();
  CHECK(rig.out[0] == 0);  // not enabled: the ring is not pulled, nothing is added
  rig.g.set_return_enabled(true);
  rig.host_dc(288, 1234);
  for (unsigned b = 0; b < 6; ++b) {
    rig.host_dc(kB, 1234);
    rig.render();
  }
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(rig.out[2u * i] == 1234);
    CHECK(rig.out[2u * i + 1u] == -1234);
  }
  rig.g.set_return_enabled(false);  // the host stops: the route is silent again at once
  rig.render();
  CHECK(rig.out[0] == 0);
  CHECK(rig.out[2u * (kB - 1u) + 1u] == 0);
}

TEST_CASE("usb return: its own level (the mixer law), refused out of range, shut at 0, ramped over a block") {
  Rig rig(make_graph(), fresh_ring());
  rig.start_host();
  (void)rig.run(8, 1, true, false);
  // level 50 on the 1.1.14 law is 10^((50 - 100) / 50) = 0.1: -20 dB
  rig.master(gpm::UsbReturnLevel, 50.0f);
  Rig::Peaks p = rig.run(24, 12, true, false);
  CHECK(within(static_cast<double>(p.left), kSentPeak * 0.1));
  CHECK(within(static_cast<double>(p.right), kSentPeak * 0.1));
  const std::uint32_t rejected = rig.g.params_rejected();
  rig.master(gpm::UsbReturnLevel, 101.0f);
  rig.master(gpm::UsbReturnLevel, -1.0f);
  rig.master(gpm::UsbReturnLevel, std::nanf(""));
  (void)rig.run(2, 1, true, false);
  CHECK(rig.g.params_rejected() == rejected + 3u);
  p = rig.run(24, 12, true, false);  // the refused values changed nothing
  CHECK(within(static_cast<double>(p.left), kSentPeak * 0.1));
  // 0 is silence; the stream is still pulled
  rig.master(gpm::UsbReturnLevel, 0.0f);
  (void)rig.run(4, 1, true, false);
  p = rig.run(8, 4, true, false);
  CHECK(p.left == 0);
  CHECK(p.right == 0);
  // back to unity over a block (the engine's own smoothing, ramp 0): the first block after is between, the next at the level
  CHECK(rig.g.params().push(ParamChange{master_param(gpm::UsbReturnLevel), 100.0f, 0}));
  rig.host_writes(kB);
  rig.render();
  std::int32_t early = 0;
  for (std::uint32_t i = 0; i < kB / 4u; ++i) {
    const std::int32_t l = rig.out[2u * i] < 0 ? -rig.out[2u * i] : rig.out[2u * i];
    early = l > early ? l : early;
  }
  CHECK(early < static_cast<std::int32_t>(kSentPeak / 2.0));  // not yet there at the start of the block
  p = rig.run(16, 8, true, false);
  CHECK(within(static_cast<double>(p.left), kSentPeak));
}

TEST_CASE("usb return: the master DRY level, DRY MUTE and the input strip's level are not in it; it is heard only (no bus, no capture)") {
  Rig rig(make_graph(), fresh_ring());
  rig.start_host();
  rig.master(gpm::Dry, 86.0f);  // the default of every v7+ project: -5.6 dB on a track
  rig.master(gpm::DryMute, 1.0f);
  rig.strip(kInputStrip, gps::Volume, 30.0f);
  const Rig::Peaks p = rig.run(24, 12, true, false);
  CHECK(within(static_cast<double>(p.left), kSentPeak));
  CHECK(within(static_cast<double>(p.right), kSentPeak));
  // Heard only: over three meter windows the master meters read the tone, and MAIN, CUE, REC and the input bus read nothing
  (void)rig.run(70, 1, true, false);
  MeterSnapshot snap;
  CHECK(rig.g.meters().snapshot(snap));
  CHECK(meter_level(snap, meter_ch::kMasterL).hold > 0.4f);
  CHECK(meter_level(snap, meter_ch::kMasterR).hold > 0.4f);
  for (const unsigned ch : {meter_ch::kMainL, meter_ch::kMainR, meter_ch::kCueL, meter_ch::kCueR, meter_ch::kRecL, meter_ch::kRecR, meter_ch::kInL, meter_ch::kInR}) {
    CHECK(meter_level(snap, ch).hold == 0.0f);
  }
}

TEST_CASE("usb return: the input strip is the codec alone; with the recorder's USB source the strip carries the stream and the monitor does not double it") {
  Rig rig(make_graph(), fresh_ring());
  rig.start_host();
  rig.strip(kInputStrip, gps::Volume, 100.0f);  // the monitor at unity, on MAIN
  rig.strip(kInputStrip, gps::Route, static_cast<float>(route::kMain));
  rig.master(gpm::UsbReturnLevel, 0.0f);        // the return's own route shut: what is heard is the strip, the codec's tone on the left alone
  Rig::Peaks p = rig.run(24, 12, true, true);
  CHECK(within(static_cast<double>(p.left), kSentPeak));
  CHECK(p.right == 0);  // (the stream is on both channels: it is not in the strip)
  // The recorder's USB source: the strip is the stream alone (the codec's tone is out of it) and its own route is shut, so MAIN has the stream once.
  rig.master(gpm::UsbReturnLevel, 100.0f);
  rig.g.set_input_feed(InputFeed::ReturnOnly);
  p = rig.run(24, 12, true, true);
  CHECK(within(static_cast<double>(p.left), kSentPeak));
  CHECK(within(static_cast<double>(p.right), kSentPeak));
  // Back to the codec: both are heard (the left is two tones of the same frequency, whose phase depends on the ring: the right is the stream alone).
  rig.g.set_input_feed(InputFeed::Sum);
  p = rig.run(24, 12, true, true);
  CHECK(within(static_cast<double>(p.right), kSentPeak));
  CHECK(p.left > 0);
}

TEST_CASE("usb return: the input strip reads its own level, whatever the DRY fader (`mon on` = 70 of a -6 dBFS input is -18 dBFS), and a track is still scaled by DRY") {
  const double expected = kSentPeak * 0.25118864;  // 10^((70 - 100) / 50): -12 dB
  for (const float dry : {100.0f, 86.0f, 50.0f}) {
    Rig rig(make_graph(), fresh_ring());
    rig.g.install_master_limiter(make_limiter());
    rig.master(gpm::Dry, dry);
    rig.strip(kInputStrip, gps::Volume, 70.0f);  // `mon on`
    const Rig::Peaks p = rig.run(24, 12, false, true);
    CHECK(within(static_cast<double>(p.left), expected));
    CHECK(p.right == 0);
  }
  // a track at 100 through DRY 86 is -5.6 dB: the exemption is the input strip's alone
  Rig rig(make_graph(), fresh_ring());
  rig.master(gpm::Dry, 86.0f);
  rig.strip(0, gps::Source, 3.0f);
  rig.strip(0, gps::Pan, 50.0f);
  rig.g.feed(0, rig.feed_[0]);
  for (std::uint32_t i = 0; i < kB; ++i) {
    rig.feed_[0][i] = 10000;
  }
  for (unsigned b = 0; b < 4; ++b) {
    rig.render();
  }
  const double dry86 = std::pow(10.0, (86.0 - 100.0) / 50.0);
  CHECK(std::fabs(db(static_cast<double>(rig.out[2 * 10]) / (10000.0 * dry86))) < 0.1);
}

TEST_CASE("usb return: the master level scales it, and the heard sum (MAIN + the return) saturates at full scale and bypasses the limiter") {
  Rig rig(make_graph(), fresh_ring());
  rig.g.attach_return(&rig.reader);
  rig.g.set_return_enabled(true);
  rig.master(gpm::Volume, 50.0f);  // -20 dB on the mixer law
  rig.host_dc(288, 20000);
  for (unsigned b = 0; b < 4; ++b) {
    rig.host_dc(kB, 20000);
    rig.render();
  }
  CHECK(std::fabs(db(static_cast<double>(rig.out[20]) / 2000.0)) < 0.1);  // 20000 x 0.1
  CHECK(std::fabs(db(static_cast<double>(rig.out[21]) / -2000.0)) < 0.1);
  // MAIN at 30000 (a track) plus the return at 20000 is past full scale: it saturates once, at the end, with no wrap
  rig.master(gpm::Volume, 100.0f);
  rig.strip(0, gps::Source, 3.0f);
  rig.strip(0, gps::Pan, 50.0f);
  rig.g.feed(0, rig.feed_[0]);
  for (std::uint32_t i = 0; i < kB; ++i) {
    rig.feed_[0][i] = 30000;
  }
  for (unsigned b = 0; b < 4; ++b) {
    rig.host_dc(kB, 20000);
    rig.render();
  }
  CHECK(rig.out[2 * 10] == 32767);
  CHECK(rig.out[2 * 10 + 1] == 10000);  // right: 30000 - 20000 (the stream's right is inverted)
}

TEST_CASE("usb return: the recorder's USB source takes the stream over from the route, and gives it back, without a step") {
  Rig rig(make_graph(), fresh_ring());
  rig.g.attach_return(&rig.reader);
  rig.g.set_return_enabled(true);
  constexpr std::int32_t kDc = 14440;  // the step the abrupt shut made at the HIL
  rig.host_dc(288, kDc);
  // The largest sample-to-sample step on the left of `blocks` blocks, from the last sample before the first (the monitor is at 0, so the strip carries nothing: only the route is heard).
  const auto largest_step = [&](unsigned blocks) {
    std::int32_t last = rig.out[2u * (kB - 1u)];
    std::int32_t worst = 0;
    for (unsigned b = 0; b < blocks; ++b) {
      rig.host_dc(kB, kDc);
      rig.render();
      for (std::uint32_t i = 0; i < kB; ++i) {
        const std::int32_t d = rig.out[2u * i] - last;
        worst = (d < 0 ? -d : d) > worst ? (d < 0 ? -d : d) : worst;
        last = rig.out[2u * i];
      }
    }
    return worst;
  };
  for (unsigned b = 0; b < 4; ++b) {
    rig.host_dc(kB, kDc);
    rig.render();
  }
  CHECK(rig.out[0] == kDc);
  rig.g.set_input_feed(InputFeed::ReturnOnly);  // the strip's level is 0: it carries the stream silently, the route ramps to 0 over the next block
  CHECK(largest_step(1) <= kDc / static_cast<std::int32_t>(kB) + 2);  // the ramp's slope (225 a sample), not 14,440
  CHECK(rig.out[2u * (kB - 1u)] == 0);
  CHECK(largest_step(2) == 0);
  rig.g.set_input_feed(InputFeed::Sum);  // and back: up over a block
  CHECK(largest_step(1) <= kDc / static_cast<std::int32_t>(kB) + 2);
  CHECK(rig.out[2u * (kB - 1u)] > kDc - 2 * kDc / static_cast<std::int32_t>(kB));
  (void)largest_step(2);
  CHECK(rig.out[0] == kDc);
}
