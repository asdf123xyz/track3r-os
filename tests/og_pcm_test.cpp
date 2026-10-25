// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-FILES part C, slice C1a (docs/specs/OS-FILES-C.md; app/og_pcm.h): the pure parts of an OG export, unwired. The file name of a .pti inside an OG folder through the real card guard and the OG
// loader's own classifier; the plan of a sample's PCM (length, step, the 5,898,240-frame limit, the Amiga source rate); the points; and the resampler's edges (an edge-extended one-shot, a wrapped loop,
// a full-scale square that rings past full scale and is saturated, never wrapped around).
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/app/amiga_convert.h>
#include <o3os/app/og_pcm.h>
#include <o3os/core/byte_io.h>
#include <o3os/formats/og.h>
#include <o3os/hal/save_guard.h>
#include <o3os/model/instrument.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;

namespace {

namespace guard = hal::storage::save;

const app::AmigaKernel& kernel() {
  static const std::unique_ptr<app::AmigaKernel> k = [] {
    auto p = std::make_unique<app::AmigaKernel>();
    while (!p->build(4096)) {
    }
    return p;
  }();
  return *k;
}

std::string stem_of(unsigned slot, const char* name) {
  char out[64] = {};
  const std::size_t n = app::og_pti_stem(slot, name, out, sizeof out);
  return std::string(out, n);
}

// The largest number of frames that still fits `max` frames out at `rate` (ceil(f x 44100 / rate) <= max  <=>  f <= max x rate / 44100).
std::uint64_t largest_fit(std::uint32_t rate) { return static_cast<std::uint64_t>(app::kOgMaxFrames) * rate / 44100u; }

std::vector<std::int16_t> run(const std::vector<std::int16_t>& src, const app::OgPcmPlan& plan, const app::OgLoop& loop, std::uint32_t* clipped = nullptr) {
  std::vector<std::int16_t> out(plan.frames_out);
  CHECK(app::og_resample(kernel(), src.data(), static_cast<std::uint32_t>(src.size()), plan.step_q32, plan.frames_out, loop, out.data(), 0, plan.frames_out, clipped) == plan.frames_out);
  return out;
}

}  // namespace

// ---- 1. the name -------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("og pcm: the .pti stem is NN + the library's stem, at most 31 bytes, a name the card guard takes, read back by the OG loader's own classifier") {
  const std::string long31(31, 'k');
  const std::string long32(32, 'k');
  const char* const names[] = {"Kick",   "Kick (hard)", "Rock & Roll", "Don't stop", "caf\xC3\xA9 \xE2\x82\xAC", "CON", "con.wav", "NUL", "COM1", "name.", "name . ", "a..b", "  lead space", ".hidden",
                               "../..",  "a/b\\c:d*e?f", "",           " ",          long31.c_str(),             long32.c_str(), "trailing dot at the cut 1234567.", "UPPER lower 0123456789 - _"};
  for (const char* name : names) {
    for (unsigned slot : {0u, 8u, 9u, 47u}) {
      INFO("name '" << name << "' slot " << slot);
      char out[64] = {};
      const std::size_t n = app::og_pti_stem(slot, name, out, sizeof out);
      NEED(n != 0u);
      CHECK(n == std::strlen(out));
      CHECK(n <= app::kOgPtiStemMax);
      CHECK(guard::valid_name(out));
      CHECK(out[n - 1] != ' ');
      CHECK(out[n - 1] != '.');
      CHECK(out[0] == static_cast<char>('0' + (slot + 1) / 10));
      CHECK(out[1] == static_cast<char>('0' + (slot + 1) % 10));
      CHECK(out[2] == ' ');
      // the file the plan writes is guard-valid as a path of an OG folder's instruments/ and the OG loader reads it back as that slot
      const std::string file = std::string(out) + ".pti";
      const formats::og::FileName cls = formats::og::classify_file_name(file.c_str());
      CHECK(cls.file_class == formats::og::FileClass::Pti);
      CHECK(cls.index == slot);
    }
  }
}

TEST_CASE("og pcm: the stem cuts at 28 bytes of name, drops a trailing space or dot left by the cut, and names no two slots alike") {
  CHECK(stem_of(0, "Kick") == "01 Kick");
  CHECK(stem_of(47, "Kick") == "48 Kick");
  CHECK(stem_of(0, std::string(28, 'a').c_str()) == "01 " + std::string(28, 'a'));  // 31 bytes exactly
  CHECK(stem_of(0, std::string(32, 'a').c_str()) == "01 " + std::string(28, 'a'));  // cut
  CHECK(stem_of(0, (std::string(27, 'a') + " tail").c_str()) == "01 " + std::string(27, 'a'));      // the cut lands on the space: it goes
  CHECK(stem_of(0, (std::string(27, 'a') + ".tail").c_str()) == "01 " + std::string(27, 'a'));      // ... or on the dot
  CHECK(stem_of(0, "") == "01 Instrument 01");
  CHECK(stem_of(1, "") == "02 Instrument 02");
  CHECK(stem_of(0, "CON") == "01 CON_");                                                   // the library's rule, then the prefix
  CHECK(stem_of(0, "Kick (hard) & 'x'") == "01 Kick _hard_ _ _x_");                         // bytes the guard does not take become '_'
  CHECK(stem_of(0, "caf\xC3\xA9") == "01 caf__");                                           // UTF-8: one '_' a byte
  CHECK(stem_of(0, "name.") == "01 name");
  // the same name in two slots never collides (the NN prefix), and every slot of one name differs
  std::vector<std::string> seen;
  for (unsigned slot = 0; slot < 48; ++slot) {
    seen.push_back(stem_of(slot, "Same"));
  }
  for (std::size_t i = 0; i < seen.size(); ++i) {
    for (std::size_t j = i + 1; j < seen.size(); ++j) {
      CHECK(seen[i] != seen[j]);
    }
  }
  // out of range and too small a buffer
  char out[64];
  CHECK(app::og_pti_stem(48, "x", out, sizeof out) == 0u);
  CHECK(app::og_pti_stem(0, "x", out, 31) == 0u);
  CHECK(app::og_pti_stem(0, "x", out, 32) == 4u);
  CHECK(app::og_pti_stem(0, "x", nullptr, 64) == 0u);
}

// ---- 2. the plan -----------------------------------------------------------------------------------------------------------------------------

TEST_CASE("og pcm: the plan of a Modern sample: ceil(frames x 44100 / rate), the identity at 44,100 Hz or no rate, the limit to the frame") {
  // no rate recorded, or the engine's: the PCM as it is
  for (std::uint32_t rate : {0u, 44100u}) {
    const app::OgPcmPlan p = app::og_pcm_plan(12345, rate);
    CHECK(p.status == app::OgPcmStatus::Ok);
    CHECK(p.identity);
    CHECK(p.frames_out == 12345u);
    CHECK(p.step_q32 == (std::uint64_t{1} << 32));
  }
  CHECK(app::og_pcm_plan(0, 22050).status == app::OgPcmStatus::Empty);
  // the lengths
  CHECK(app::og_pcm_plan(1000, 22050).frames_out == 2000u);
  CHECK(app::og_pcm_plan(1000, 48000).frames_out == 919u);   // 918.75 up
  CHECK(app::og_pcm_plan(1000, 16574).frames_out == 2661u);  // 2660.8 up
  CHECK(app::og_pcm_plan(1000, 8287).frames_out == 5322u);   // 5321.8 up
  CHECK(app::og_pcm_plan(1, 8287).frames_out == 6u);
  CHECK(app::og_pcm_plan(1, 48000).frames_out == 1u);
  // the step is rate / 44100 in Q32.32 (source frames per output frame)
  const app::OgPcmPlan half = app::og_pcm_plan(1000, 22050);
  CHECK(half.step_q32 == (std::uint64_t{1} << 31));
  CHECK_FALSE(half.identity);
  CHECK(app::og_pcm_plan(1000, 48000).step_q32 == (std::uint64_t{48000} << 32) / 44100u);
  // the limit: the largest length that fits writes, one frame more is refused (never cut)
  CHECK(app::og_pcm_plan(app::kOgMaxFrames, 44100).status == app::OgPcmStatus::Ok);
  CHECK(app::og_pcm_plan(app::kOgMaxFrames + 1u, 44100).status == app::OgPcmStatus::TooLong);
  CHECK(app::og_pcm_plan(app::kOgMaxFrames + 1u, 0).status == app::OgPcmStatus::TooLong);
  for (std::uint32_t rate : {8287u, 16574u, 22050u, 48000u, 96000u}) {
    INFO("rate " << rate);
    const std::uint32_t fit = static_cast<std::uint32_t>(largest_fit(rate));
    const app::OgPcmPlan ok = app::og_pcm_plan(fit, rate);
    CHECK(ok.status == app::OgPcmStatus::Ok);
    CHECK(ok.frames_out <= app::kOgMaxFrames);
    const app::OgPcmPlan over = app::og_pcm_plan(fit + 1u, rate);
    CHECK(over.status == app::OgPcmStatus::TooLong);
    CHECK(over.frames_out > app::kOgMaxFrames);
  }
  // the rate is held to the engine's range as the voices do (1,000..384,000 Hz)
  CHECK(app::og_pcm_plan(1000, 10).frames_out == app::og_pcm_plan(1000, 1000).frames_out);
  CHECK(app::og_pcm_plan(384000, 1000000).frames_out == app::og_pcm_plan(384000, 384000).frames_out);
}

TEST_CASE("og pcm: the plan of an Amiga-engine sample reads the data at 8,287.14 Hz whatever its label, and keeps the tune") {
  const app::OgPcmPlan p = app::og_pcm_plan_amiga(1000);
  CHECK(p.status == app::OgPcmStatus::Ok);
  CHECK_FALSE(p.identity);
  CHECK(p.frames_out == 5322u);  // 1000 x 44100 / 8287.14 = 5321.7 up
  const double rho = static_cast<double>(p.step_q32) / 4294967296.0;
  CHECK(rho == doctest::Approx(3546895.0 / 428.0 / 44100.0).epsilon(1e-9));
  CHECK(app::og_pcm_plan_amiga(0).status == app::OgPcmStatus::Empty);
  // the limit
  const std::uint64_t fit = static_cast<std::uint64_t>(app::kOgMaxFrames) * 3546895u / (428u * 44100u);
  CHECK(app::og_pcm_plan_amiga(static_cast<std::uint32_t>(fit)).status == app::OgPcmStatus::Ok);
  CHECK(app::og_pcm_plan_amiga(static_cast<std::uint32_t>(fit) + 1u).status == app::OgPcmStatus::TooLong);
  // the fine tune: what the Amiga voice rounds it to (a MED step is 12.5 hundredths, -8..7 steps), as a whole number of hundredths
  CHECK(app::og_amiga_fine(0) == 0);
  CHECK(app::og_amiga_fine(6) == 0);
  CHECK(app::og_amiga_fine(7) == 13);
  CHECK(app::og_amiga_fine(12) == 13);
  CHECK(app::og_amiga_fine(30) == 25);
  CHECK(app::og_amiga_fine(100) == 88);   // 7 steps: the top of the Amiga's range
  CHECK(app::og_amiga_fine(-6) == 0);
  CHECK(app::og_amiga_fine(-7) == -13);
  CHECK(app::og_amiga_fine(-100) == -100);
  CHECK(app::og_amiga_fine(-30) == -25);
}

TEST_CASE("og pcm: the fine tune an Amiga instrument is exported with is the MED step the voice rounds it to, over the whole -100..100 range") {
  int previous = -1000;
  for (int fine = -100; fine <= 100; ++fine) {
    const int out = app::og_amiga_fine(fine);
    INFO("fine " << fine << " -> " << out);
    CHECK(out >= -100);
    CHECK(out <= 88);                     // 7 steps of 12.5: the voice's own limit
    CHECK(out >= previous);               // monotonic
    previous = out;
    // the exported value is a whole step of 12.5 hundredths (rounded to a whole hundredth) and within half a step of what was asked, except past the top of the voice's range
    const double steps = static_cast<double>(out) / 12.5;
    CHECK(std::fabs(steps - std::round(steps)) < 0.06);
    if (fine <= 93) {
      CHECK(std::fabs(static_cast<double>(out) - static_cast<double>(fine)) <= 6.5);
    }
  }
}

TEST_CASE("og pcm: a sine through the Amiga plan keeps its pitch at the unity note: data frames at 8,287.14 Hz, whatever the label and the tune (the 16,574 Hz conversion included)") {
  // The Amiga voice at note n plays the data at 8,287.14 x 2^((n + tune - 60)/12) frames a second; a Modern note plays the exported frames at 44,100 x 2^((n + tune - 60)/12). With the plan's step
  // the two are the same pitch for every tune, so the tune stays as it is. At the unity note and tune 0 the pitch of a sine of f cycles a frame is f x 8,287.14 Hz.
  for (double f : {0.02, 0.05, 0.11}) {
    std::vector<std::int16_t> src(4000);
    for (std::size_t i = 0; i < src.size(); ++i) {
      src[i] = static_cast<std::int16_t>(std::lround(12000.0 * std::sin(2.0 * 3.14159265358979323846 * f * static_cast<double>(i))));
    }
    const app::OgPcmPlan plan = app::og_pcm_plan_amiga(static_cast<std::uint32_t>(src.size()));
    const std::vector<std::int16_t> out = run(src, plan, app::OgLoop{});
    // the frequency by upward zero crossings with linear interpolation, over the middle of the output
    double first = -1.0;
    double last = -1.0;
    unsigned crossings = 0;
    for (std::size_t i = out.size() / 8; i + 1 < out.size() - out.size() / 8; ++i) {
      if (out[i] < 0 && out[i + 1] >= 0) {
        const double at = static_cast<double>(i) + static_cast<double>(-out[i]) / static_cast<double>(out[i + 1] - out[i]);
        if (first < 0.0) {
          first = at;
        }
        last = at;
        ++crossings;
      }
    }
    NEED(crossings > 10u);
    const double cycles_per_out_frame = static_cast<double>(crossings - 1) / (last - first);
    const double hz = cycles_per_out_frame * 44100.0;
    const double want = f * 3546895.0 / 428.0;
    INFO("f " << f << " measured " << hz << " Hz, wanted " << want << " Hz");
    CHECK(hz == doctest::Approx(want).epsilon(0.0005));
    // a conversion at 16,574 Hz (tune +12) holds data of f' = f / 2 cycles a frame: at tune 12 it plays at f' x 8,287.14 x 2 = the same pitch; the exported sine at 2 f' keeps tune +12 and says the same
  }
}

TEST_CASE("og pcm: the points are fractions of the length, so they name the same time in the resampled sample to under one frame of either") {
  const std::uint32_t frames_in = 77777;
  for (std::uint32_t rate : {8287u, 16574u, 22050u, 48000u}) {
    const app::OgPcmPlan p = app::og_pcm_plan(frames_in, rate);
    NEED(p.status == app::OgPcmStatus::Ok);
    for (std::uint32_t point : {0u, 1u, 100u, 12345u, 30000u, 40000u, 65534u, 65535u}) {
      const std::uint32_t src = app::og_point_frame(point, frames_in);
      const std::uint32_t out = app::og_point_frame(point, p.frames_out);
      const double expect_out = static_cast<double>(src) * 44100.0 / static_cast<double>(rate);
      const double err = std::fabs(static_cast<double>(out) - expect_out);
      INFO("rate " << rate << " point " << point << " src " << src << " out " << out);
      CHECK(err <= 1.0 + 44100.0 / static_cast<double>(rate));  // one output frame, and one source frame in output frames
    }
    CHECK(app::og_point_frame(65535, p.frames_out) == p.frames_out);  // the end point is the end of the sample
  }
}

TEST_CASE("og pcm: the loop of an instrument in whole-sample frames, as the voices bind it") {
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  in.sampler.play_mode = 1;
  in.sampler.start = 0;
  in.sampler.end = 65535;
  in.sampler.loop_start = 16384;
  in.sampler.loop_end = 49152;
  app::OgLoop loop = app::og_loop_of(in, 1000);
  CHECK(loop.active);
  CHECK(loop.mode == 1);
  // the voices' own arithmetic (og_samples.cpp at_point): a float multiply by frames / 65535, truncated
  auto voice_frame = [](std::uint32_t point, std::uint32_t frames) {
    return point >= 65535u ? frames : static_cast<std::uint32_t>(static_cast<float>(point) * (static_cast<float>(frames) / 65535.0f));
  };
  CHECK(loop.start == voice_frame(16384, 1000));
  CHECK(loop.end == voice_frame(49152, 1000));
  // on a long sample the exact integer form and the voices' float form may differ by a frame; the loop is the voices'
  for (std::uint32_t frames : {44100u, 441000u, 3000000u, 5898240u}) {
    in.sampler.loop_start = 12345;
    in.sampler.loop_end = 54321;
    const app::OgLoop l = app::og_loop_of(in, frames);
    CHECK(l.active);
    CHECK(l.start == voice_frame(12345, frames));
    CHECK(l.end == voice_frame(54321, frames));
    const std::uint32_t exact = app::og_point_frame(12345, frames);
    CHECK((l.start == exact || l.start + 1u == exact || l.start == exact + 1u));
  }
  in.sampler.loop_start = 16384;
  in.sampler.loop_end = 49152;
  in.sampler.play_mode = 0;
  CHECK_FALSE(app::og_loop_of(in, 1000).active);  // a one shot
  in.sampler.play_mode = 3;
  CHECK(app::og_loop_of(in, 1000).mode == 3);
  in.sampler.play_mode = 4;
  CHECK_FALSE(app::og_loop_of(in, 1000).active);  // a slice mode
  in.sampler.play_mode = 1;
  in.sampler.loop_start = 40000;
  in.sampler.loop_end = 40001;
  CHECK_FALSE(app::og_loop_of(in, 1000).active);  // under two frames: a one shot
  in.sampler.loop_start = 100;
  in.sampler.loop_end = 65535;
  in.sampler.end = 30000;
  CHECK_FALSE(app::og_loop_of(in, 1000).active);  // the loop outside the window
  CHECK_FALSE(app::og_loop_of(in, 0).active);
}

// ---- 3. the resampler ------------------------------------------------------------------------------------------------------------------------

TEST_CASE("og pcm: a DC level keeps its level to the first and the last frame (edge-extended, nothing fades), where the Amiga conversion's silence fades the ends") {
  for (std::uint32_t rate : {8287u, 16574u, 22050u, 48000u, 96000u}) {
    INFO("rate " << rate);
    const std::vector<std::int16_t> src(500, 12000);
    const app::OgPcmPlan plan = app::og_pcm_plan(static_cast<std::uint32_t>(src.size()), rate);
    const std::vector<std::int16_t> out = run(src, plan, app::OgLoop{});
    for (std::size_t i = 0; i < out.size(); ++i) {
      NEED(out[i] >= 11999);
      NEED(out[i] <= 12001);
    }
  }
  // the contrast: the Amiga conversion's resampler reads silence outside the sample and loses level over its first frames
  const std::vector<std::int16_t> src(500, 12000);
  const app::OgPcmPlan plan = app::og_pcm_plan(500, 22050);
  std::vector<std::int16_t> old(plan.frames_out);
  app::amiga_resample(kernel(), src.data(), 500, plan.step_q32, false, old.data(), 0, plan.frames_out);
  CHECK(old[0] < 11800);  // it loses level over the first frames (a few percent here, the half of an impulse)
}

TEST_CASE("og pcm: a one-shot's first frame is not attenuated: a sample that starts loud starts loud") {
  std::vector<std::int16_t> src(300, 0);
  for (std::size_t i = 0; i < 40; ++i) {
    src[i] = 20000;  // a hard start: the first frames are the loudest
  }
  const app::OgPcmPlan plan = app::og_pcm_plan(300, 22050);
  const std::vector<std::int16_t> out = run(src, plan, app::OgLoop{});
  CHECK(out[0] > 19000);
  CHECK(out[1] > 19000);
  CHECK(out[10] > 19000);
  // and an impulse at frame 0 alone: its response is the kernel's, the level of the frame before it is the frame itself, not silence
  std::vector<std::int16_t> imp(300, 0);
  imp[0] = 20000;
  const std::vector<std::int16_t> o2 = run(imp, plan, app::OgLoop{});
  CHECK(o2[0] > 10000);  // old: about half the impulse; here the left side is the repeated frame 0
}

TEST_CASE("og pcm: a looped sample is seamless across the loop's end in the new rate, where the data past the loop is not what plays") {
  // 400 frames: a sine of period 50 frames, 4 cycles in the loop [100, 300), and garbage after the loop that the voice never plays
  std::vector<std::int16_t> src(400);
  for (std::size_t i = 0; i < src.size(); ++i) {
    src[i] = i < 300 ? static_cast<std::int16_t>(std::lround(15000.0 * std::sin(2.0 * 3.14159265358979323846 * static_cast<double>(i) / 50.0))) : std::int16_t{30000};
  }
  app::OgLoop loop;
  loop.active = true;
  loop.mode = 1;
  loop.start = 100;
  loop.end = 300;
  const app::OgPcmPlan plan = app::og_pcm_plan(400, 22050);  // step 0.5: the loop is output frames [200, 600)
  const std::vector<std::int16_t> out = run(src, plan, loop);
  double worst = 0.0;
  for (std::size_t m = 560; m < 600; ++m) {  // just before the loop's end: the kernel reaches 32 source frames past it
    const double want = 15000.0 * std::sin(2.0 * 3.14159265358979323846 * (static_cast<double>(m) * 0.5) / 50.0);
    worst = std::fmax(worst, std::fabs(static_cast<double>(out[m]) - want));
  }
  INFO("worst error before the loop end " << worst);
  CHECK(worst < 300.0);  // 2 percent of the amplitude
  // the control: with no loop the garbage past the end is read and the same frames are far off
  const std::vector<std::int16_t> flat = run(src, plan, app::OgLoop{});
  double worst_flat = 0.0;
  for (std::size_t m = 560; m < 600; ++m) {
    const double want = 15000.0 * std::sin(2.0 * 3.14159265358979323846 * (static_cast<double>(m) * 0.5) / 50.0);
    worst_flat = std::fmax(worst_flat, std::fabs(static_cast<double>(flat[m]) - want));
  }
  CHECK(worst_flat > 3000.0);
}

// A loop is the voice's own: before its start the voice has just come round from its end, so an output frame at or past the start must see the loop's tail, not the pre-loop data or the first frame repeated.
TEST_CASE("og pcm: a loop that starts at frame 0 has no click at its wrap, forward or backward: it equals the same loop played three times over and cut from the middle") {
  for (std::uint8_t mode : {std::uint8_t{1}, std::uint8_t{2}}) {
    INFO("mode " << static_cast<unsigned>(mode));
    // one period of a sawtooth over the whole sample, which is the loop [0, 200): the wrap is a real step, the hardest edge for a band-limited kernel
    std::vector<std::int16_t> one(200);
    for (std::size_t i = 0; i < one.size(); ++i) {
      one[i] = static_cast<std::int16_t>(-15000 + 150 * static_cast<int>(i));
    }
    std::vector<std::int16_t> three;
    for (int copy = 0; copy < 3; ++copy) {
      three.insert(three.end(), one.begin(), one.end());
    }
    app::OgLoop loop;
    loop.active = true;
    loop.mode = mode;
    loop.start = 0;
    loop.end = 200;
    const app::OgPcmPlan plan = app::og_pcm_plan(200, 22050);  // step 0.5, 400 output frames
    const std::vector<std::int16_t> out = run(one, plan, loop);
    const std::vector<std::int16_t> ref = run(three, app::og_pcm_plan(600, 22050), app::OgLoop{});  // the middle copy is output frames 400..799
    NEED(ref.size() == 1200u);
    int worst = 0;
    for (std::size_t m = 0; m < out.size(); ++m) {  // every frame, the first and the last 32 included
      const int e = out[m] - ref[400 + m];
      worst = std::max(worst, e < 0 ? -e : e);
    }
    INFO("worst difference from the periodic reference " << worst);
    CHECK(worst <= 2);
    // the control: edge-extended (no loop) is far off at both ends
    const std::vector<std::int16_t> flat = run(one, plan, app::OgLoop{});
    int worst_flat = 0;
    for (std::size_t m = 0; m < flat.size(); ++m) {
      const int e = flat[m] - ref[400 + m];
      worst_flat = std::max(worst_flat, e < 0 ? -e : e);
    }
    CHECK(worst_flat > 2000);
  }
}

TEST_CASE("og pcm: a loop that starts inside the sample: frames before it keep the real pre-loop data, frames at it read the loop's tail") {
  // pre-loop [0, 100) is a loud constant that the loop's own data is not; the loop [100, 300) is a slow sine, one cycle; after it, garbage
  std::vector<std::int16_t> src(400);
  for (std::size_t i = 0; i < src.size(); ++i) {
    src[i] = i < 100 ? std::int16_t{20000} : (i < 300 ? static_cast<std::int16_t>(std::lround(9000.0 * std::sin(2.0 * 3.14159265358979323846 * static_cast<double>(i - 100) / 200.0))) : std::int16_t{-30000});
  }
  app::OgLoop loop;
  loop.active = true;
  loop.mode = 1;
  loop.start = 100;
  loop.end = 300;
  const app::OgPcmPlan plan = app::og_pcm_plan(400, 22050);
  const std::vector<std::int16_t> out = run(src, plan, loop);
  const std::vector<std::int16_t> flat = run(src, plan, app::OgLoop{});
  // an output frame centred before the loop reads only real data on its left side, as it does with no loop (the first 60 output frames: centre under 30)
  for (std::size_t m = 0; m < 60; ++m) {
    CHECK(out[m] == flat[m]);
  }
  // an output frame centred at the loop's start reads the loop's tail before it: the sine continues through the wrap (its value there is 0, and the slope is the sine's)
  double worst = 0.0;
  for (std::size_t m = 200; m < 240; ++m) {  // output frames 200.. = source 100..: the loop's first frames
    const double want = 9000.0 * std::sin(2.0 * 3.14159265358979323846 * (static_cast<double>(m) * 0.5 - 100.0) / 200.0);
    worst = std::fmax(worst, std::fabs(static_cast<double>(out[m]) - want));
  }
  INFO("worst error at the loop start " << worst);
  CHECK(worst < 450.0);
}

TEST_CASE("og pcm: a ping-pong loop reflects at its start as well as its end") {
  std::vector<std::int16_t> src(200);
  for (std::size_t i = 0; i < src.size(); ++i) {
    src[i] = i < 50 ? std::int16_t{-20000} : static_cast<std::int16_t>(100 * static_cast<int>(i));  // garbage before the loop, then a ramp
  }
  app::OgLoop loop;
  loop.active = true;
  loop.mode = 3;
  loop.start = 50;
  loop.end = 150;
  const app::OgPcmPlan plan = app::og_pcm_plan(200, 22050);
  const std::vector<std::int16_t> out = run(src, plan, loop);
  // output frame 101 sits at source 50.5: the ramp's value there is about 5050; the reflected side makes a V, so it stays near 5000 and is never near the -20000 garbage
  CHECK(out[101] > 4500);
  CHECK(out[101] < 6500);
  CHECK(out[103] > 4500);
}

TEST_CASE("og pcm: a ping-pong loop reflects at its end, a one-shot extends its last frame") {
  std::vector<std::int16_t> src(200);
  for (std::size_t i = 0; i < src.size(); ++i) {
    src[i] = static_cast<std::int16_t>(i < 150 ? 100 * static_cast<int>(i) : -20000);  // a ramp up to the loop's end, then data that is not played
  }
  app::OgLoop loop;
  loop.active = true;
  loop.mode = 3;
  loop.start = 50;
  loop.end = 150;
  const app::OgPcmPlan plan = app::og_pcm_plan(200, 22050);
  const std::vector<std::int16_t> out = run(src, plan, loop);
  // output frame 299 sits at source 149.5: the kernel sees the ramp turn down (reflected), never the -20000
  CHECK(out[299] > 13000);
  CHECK(out[299] < 15500);
  // a one shot's last frames: the last frame repeated, no fade to silence
  std::vector<std::int16_t> shot(200, 7000);
  const std::vector<std::int16_t> o2 = run(shot, plan, app::OgLoop{});
  CHECK(o2.back() >= 6999);
}

TEST_CASE("og pcm: a full-scale square rings and is saturated at 16 bits, counted, never wrapped around") {
  for (std::uint32_t rate : {22050u, 48000u, 8287u}) {
    INFO("rate " << rate);
    std::vector<std::int16_t> src(800);
    for (std::size_t i = 0; i < src.size(); ++i) {
      src[i] = ((i / 20) % 2 == 0) ? std::int16_t{32767} : std::int16_t{-32768};
    }
    const app::OgPcmPlan plan = app::og_pcm_plan(static_cast<std::uint32_t>(src.size()), rate);
    std::uint32_t clipped = 0;
    const std::vector<std::int16_t> out = run(src, plan, app::OgLoop{}, &clipped);
    CHECK(clipped > 0u);  // the Gibbs overshoot of a band-limited square is past full scale
    // plateaus' interiors keep their sign and level: a wrapped overshoot would flip them
    const double per = static_cast<double>(plan.frames_out) / static_cast<double>(src.size());
    for (std::size_t block = 1; block + 1 < 40; ++block) {
      const std::size_t mid = static_cast<std::size_t>((static_cast<double>(block) * 20.0 + 10.0) * per);
      if (mid >= out.size()) {
        continue;
      }
      if (block % 2 == 0) {
        CHECK(out[mid] > 30000);
      } else {
        CHECK(out[mid] < -30000);
      }
    }
  }
}

TEST_CASE("og pcm: a 1-frame and a 31-frame sample, silence, and chunked output equals one pass") {
  const app::OgPcmPlan one = app::og_pcm_plan(1, 22050);
  const std::vector<std::int16_t> o1 = run(std::vector<std::int16_t>{5000}, one, app::OgLoop{});
  NEED(o1.size() == 2u);
  CHECK(o1[0] == 5000);
  CHECK(o1[1] == 5000);
  std::vector<std::int16_t> s31(31);
  for (std::size_t i = 0; i < s31.size(); ++i) {
    s31[i] = static_cast<std::int16_t>(1000 * static_cast<int>(i));
  }
  const app::OgPcmPlan p31 = app::og_pcm_plan(31, 48000);
  const std::vector<std::int16_t> o31 = run(s31, p31, app::OgLoop{});
  CHECK(o31.size() == p31.frames_out);
  for (std::int16_t v : o31) {
    CHECK(v >= -1);
    CHECK(v <= 30500);
  }
  const std::vector<std::int16_t> silence(300, 0);
  const app::OgPcmPlan ps = app::og_pcm_plan(300, 16574);
  for (std::int16_t v : run(silence, ps, app::OgLoop{})) {
    CHECK(v == 0);
  }
  // chunks of any size give the same frames as one pass
  std::vector<std::int16_t> src(1000);
  for (std::size_t i = 0; i < src.size(); ++i) {
    src[i] = static_cast<std::int16_t>(std::lround(9000.0 * std::sin(0.07 * static_cast<double>(i))));
  }
  const app::OgPcmPlan plan = app::og_pcm_plan(1000, 22050);
  const std::vector<std::int16_t> whole = run(src, plan, app::OgLoop{});
  std::vector<std::int16_t> parts(plan.frames_out);
  for (std::uint32_t at = 0; at < plan.frames_out;) {
    const std::uint32_t n = 37;
    const std::uint32_t got = app::og_resample(kernel(), src.data(), 1000, plan.step_q32, plan.frames_out, app::OgLoop{}, parts.data() + at, at, n);
    NEED(got != 0u);
    at += got;
  }
  CHECK(parts == whole);
  // a request past the end writes nothing; a kernel that is not ready writes nothing
  std::int16_t spare[4] = {1, 2, 3, 4};
  CHECK(app::og_resample(kernel(), src.data(), 1000, plan.step_q32, plan.frames_out, app::OgLoop{}, spare, plan.frames_out, 4) == 0u);
  app::AmigaKernel cold;
  CHECK(app::og_resample(cold, src.data(), 1000, plan.step_q32, plan.frames_out, app::OgLoop{}, spare, 0, 4) == 0u);
}

// ---- C1b: the tail of a looped sample, the slice budget and the byte source --------------------------------------------------------------------------

// The part between the loop's end and the window's end is never played by a loop but the file keeps it: an output frame centred in it reads the real data, not the wrapped loop.
TEST_CASE("og pcm: the real tail past a loop's end is exported as it is, not as the wrapped loop") {
  std::vector<std::int16_t> src(400);
  for (std::size_t i = 0; i < src.size(); ++i) {
    src[i] = i < 300 ? static_cast<std::int16_t>(std::lround(15000.0 * std::sin(2.0 * 3.14159265358979323846 * static_cast<double>(i) / 50.0))) : std::int16_t{30000};
  }
  for (std::uint8_t mode : {std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{3}}) {
    INFO("play mode " << int(mode));
    app::OgLoop loop;
    loop.active = true;
    loop.mode = mode;
    loop.start = 100;
    loop.end = 300;
    const app::OgPcmPlan plan = app::og_pcm_plan(400, 22050);  // step 0.5: the tail is output frames [600, 800)
    const std::vector<std::int16_t> out = run(src, plan, loop);
    NEED(out.size() == 800u);
    for (std::size_t m = 640; m < 800; ++m) {  // centred at 320 and on: every tap is past the loop's end (the kernel reaches 16 source frames)
      CHECK(out[m] >= 29900);
      CHECK(out[m] <= 30100);
    }
    // and the loop itself still wraps: the frames just before its end are the sine, not the tail
    double worst = 0.0;
    for (std::size_t m = 560; m < 590; ++m) {
      const double want = 15000.0 * std::sin(2.0 * 3.14159265358979323846 * (static_cast<double>(m) * 0.5) / 50.0);
      worst = std::fmax(worst, std::fabs(static_cast<double>(out[m]) - want));
    }
    CHECK(worst < 300.0);
  }
}

TEST_CASE("og pcm: a slice is a few hundred frames at most, fewer as the ratio grows, never under 8") {
  CHECK(app::og_frames_per_step(std::uint64_t{1} << 32) == 128u);                // ratio 1: 64 taps a frame
  CHECK(app::og_frames_per_step((std::uint64_t{1} << 32) / 5) == 128u);          // ratio 0.19: not stretched, still 64 taps
  CHECK(app::og_frames_per_step((std::uint64_t{1} << 32) / 100) == 128u);
  CHECK(app::og_frames_per_step(app::og_pcm_plan(1000, 234000).step_q32) == 24u);  // ratio 5.3 (a 234 kHz source): 340 taps
  CHECK(app::og_frames_per_step(app::og_pcm_plan(1000, 384000).step_q32) == 14u);  // ratio 8.7: 557 taps
  CHECK(app::og_frames_per_step(app::og_pcm_plan(1000, 8287).step_q32) == 128u);   // the Amiga rate up to 44,100 Hz: ratio 0.19, not stretched
  CHECK(app::og_frames_per_step(std::uint64_t{1000} << 32) == 8u);
}

TEST_CASE("og pcm: the resample source builds the kernel first, makes nothing until armed, a slice a time, and gives the frames of one pass") {
  std::vector<std::int16_t> src(3000);
  for (std::size_t i = 0; i < src.size(); ++i) {
    src[i] = static_cast<std::int16_t>(std::lround(9000.0 * std::sin(0.05 * static_cast<double>(i))));
  }
  const app::OgPcmPlan plan = app::og_pcm_plan(3000, 22050);
  NEED(plan.frames_out == 6000u);
  const std::vector<std::int16_t> whole = run(src, plan, app::OgLoop{});

  auto cold = std::make_unique<app::AmigaKernel>();
  app::OgResampleSource source;
  source.set(*cold, src.data(), 3000, plan.step_q32, plan.frames_out, app::OgLoop{});
  std::uint8_t buf[512];
  // the first reads build the kernel (a slice each) and say WouldBlock, armed or not
  unsigned builds = 0;
  source.arm();
  while (!cold->ready()) {
    const core::IoResult r = source.read(buf, sizeof buf);
    CHECK(r.status == core::IoStatus::WouldBlock);
    CHECK(r.count == 0u);
    NEED(++builds < 100u);
  }
  CHECK(builds > 1u);  // a few hundred entries a read, not at once
  // not armed (the slice of the last step is spent): WouldBlock, nothing made
  core::IoResult r{0, core::IoStatus::Ok};
  std::vector<std::int16_t> got;
  unsigned reads = 0;
  for (;;) {
    r = source.read(buf, sizeof buf);
    if (r.status == core::IoStatus::WouldBlock) {
      CHECK(r.count == 0u);
      source.arm();  // the next step
      NEED(++reads < 5000u);
      continue;
    }
    if (r.status == core::IoStatus::End) {
      break;
    }
    NEED(r.status == core::IoStatus::Ok);
    NEED(r.count > 0u);
    NEED(r.count % 2u == 0u);
    CHECK(r.count / 2u <= app::og_frames_per_step(plan.step_q32));  // a slice never makes more than its budget
    for (std::size_t i = 0; i < r.count; i += 2) {
      got.push_back(static_cast<std::int16_t>(static_cast<std::uint16_t>(buf[i] | (buf[i + 1] << 8))));
    }
  }
  CHECK(got == whole);
  CHECK(source.done() == plan.frames_out);
  CHECK(source.clipped() == 0u);
  CHECK(source.read(buf, sizeof buf).status == core::IoStatus::End);  // and again
  // a small ask is honoured (an odd count rounds down) and the budget is shared by the reads of a step
  source.set(*cold, src.data(), 3000, plan.step_q32, plan.frames_out, app::OgLoop{});
  source.arm();
  r = source.read(buf, 5);
  CHECK(r.status == core::IoStatus::Ok);
  CHECK(r.count == 4u);
  // an unusable source fails rather than reading wild
  app::OgResampleSource unset;
  CHECK(unset.read(buf, sizeof buf).status == core::IoStatus::Failed);
}

TEST_CASE("og pcm: the resample source counts the frames it had to saturate") {
  std::vector<std::int16_t> sq(400);
  for (std::size_t i = 0; i < sq.size(); ++i) {
    sq[i] = ((i / 20u) & 1u) != 0u ? std::int16_t{32767} : std::int16_t{-32768};
  }
  const app::OgPcmPlan plan = app::og_pcm_plan(400, 48000);
  auto k = std::make_unique<app::AmigaKernel>();
  while (!k->build(4096)) {
  }
  app::OgResampleSource source;
  source.set(*k, sq.data(), 400, plan.step_q32, plan.frames_out, app::OgLoop{});
  std::uint8_t buf[512];
  for (;;) {
    source.arm();
    const core::IoResult r = source.read(buf, sizeof buf);
    if (r.status == core::IoStatus::End) {
      break;
    }
    NEED(r.status == core::IoStatus::Ok);
  }
  CHECK(source.clipped() > 0u);
}
