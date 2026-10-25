// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-E2E-4: the audio fingerprints (sim/include/o3os/sim/audio_fingerprint.h) on synthetic renders: what a print holds (the bands add up to the signal's power, the f0 of a held tone to a cent, the onset to a
// sample or two, the balance, a planted click, the quiet frames), that the same render gives the same text, the file's round trip and every way a file can be wrong, and each rule of the comparison at its edge
// (passes just inside, fails just outside) with a small change of timing or level that must pass and a large one that must not.
#include <doctest/doctest.h>

#include <o3os/sim/audio_check.h>
#include <o3os/sim/audio_fingerprint.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace o3os::sim;

// (the build has no exceptions, so doctest's REQUIRE is not available: a failed precondition ends the case)
#define FP_REQUIRE(cond) \
  do {                   \
    const bool fp_ok = static_cast<bool>(cond); \
    CHECK(fp_ok);        \
    if (!fp_ok) {        \
      return;            \
    }                    \
  } while (0)

constexpr double kRate = 44117.647;
constexpr double kTau = 6.283185307179586;

// A stereo Pcm: a sine (with 3 ms fades) of `hz` and amplitude `amp` in both channels from `start_ms` to `end_ms`, silence elsewhere, `total_ms` long. `right` scales the right channel.
Pcm tone(double hz, double amp, double start_ms, double end_ms, double total_ms, double right = 1.0) {
  Pcm p;
  p.rate = kRate;
  const std::size_t n = static_cast<std::size_t>(total_ms * kRate / 1000.0);
  p.lr.assign(2u * n, 0);
  const std::size_t a = static_cast<std::size_t>(start_ms * kRate / 1000.0);
  const std::size_t b = std::min(n, static_cast<std::size_t>(end_ms * kRate / 1000.0));
  for (std::size_t i = a; i < b; ++i) {
    // 3 ms fades at both ends, as the engine's declick ramps: a bare start or end is a kink that the click count (rightly) finds
    const double fade = std::min({1.0, static_cast<double>(i - a) / (0.003 * kRate), static_cast<double>(b - 1u - i) / (0.003 * kRate)});
    const double v = fade * amp * std::sin(kTau * hz * static_cast<double>(i - a) / kRate);
    p.lr[2u * i] = static_cast<std::int16_t>(std::lround(v * 32768.0));
    p.lr[2u * i + 1u] = static_cast<std::int16_t>(std::lround(v * right * 32768.0));
  }
  return p;
}

FpSet one(const FpRender& r, const char* name = "r") {
  FpSet s;
  s[name] = r;
  return s;
}

// A fingerprint to bend: a held 441 Hz tone of 0.5 from 100 ms to 900 ms of 1 s.
FpRender held() { return fingerprint_render(tone(441.18, 0.5, 100.0, 900.0, 1000.0)); }

bool holds(const FpRender& want, const FpRender& got) { return compare_fingerprints("k", one(want), one(got)).failures.empty(); }

TEST_CASE("fingerprint: the bands of a sine add up to its power, in the band that holds it") {
  const FpRender r = held();
  FP_REQUIRE(r.frames.size() == 10u);
  double sum = 0.0;
  unsigned loudest = 0;
  for (unsigned b = 0; b < kFpBands; ++b) {
    sum += std::pow(10.0, r.frames[4].bands[b] / 10.0);
    if (r.frames[4].bands[b] > r.frames[4].bands[loudest]) {
      loudest = b;
    }
  }
  CHECK(10.0 * std::log10(sum) == doctest::Approx(-9.03).epsilon(0.01));  // amplitude 0.5: mean square 0.125 = -9.03 dB; the mid channel of two equal channels is the same (each band rounded to 0.1 dB)
  double lo = 0.0, hi = 0.0;
  fp_band_edges(loudest, lo, hi);
  CHECK(lo <= 441.18);
  CHECK(441.18 < hi);
  CHECK(r.frames[4].bands[loudest] > -13.0);
  // silence is all floor
  const FpRender silent = fingerprint_render(tone(441.18, 0.0, 0.0, 300.0, 300.0));
  for (const FpFrame& f : silent.frames) {
    for (unsigned b = 0; b < kFpBands; ++b) {
      CHECK(f.bands[b] == kFpBandFloorDb);
    }
  }
}

TEST_CASE("fingerprint: the band edges are 24 bands from 30 Hz to 20 kHz") {
  double lo = 0.0, hi = 0.0;
  fp_band_edges(0, lo, hi);
  CHECK(lo == doctest::Approx(30.0));
  double prev = hi;
  for (unsigned b = 1; b < kFpBands; ++b) {
    fp_band_edges(b, lo, hi);
    CHECK(lo == doctest::Approx(prev));
    CHECK(hi > lo);
    prev = hi;
  }
  CHECK(hi == doctest::Approx(20000.0));
}

TEST_CASE("fingerprint: the f0, the onset, the levels and the balance of a held tone") {
  const FpRender r = held();
  FP_REQUIRE(r.frames.size() == 10u);
  CHECK(r.frames[0].f0 == 0.0);  // silence
  CHECK(r.frames[9].f0 == 0.0);
  for (std::size_t f = 2; f <= 7u; ++f) {  // (the 200 ms windows of frames 1 and 8 reach past the tone's ends)
    CHECK(1200.0 * std::fabs(std::log2(r.frames[f].f0 / 441.18)) < 1.0);
    CHECK(r.frames[f].peak_db == doctest::Approx(-6.0).epsilon(0.02));
    CHECK(r.frames[f].rms_db == doctest::Approx(-9.0).epsilon(0.02));
    CHECK(r.frames[f].pan_db == 0.0);
    CHECK(r.frames[f].clicks == 0u);
    CHECK(std::fabs(r.frames[f].dc_lsb) < 120.0);  // the last partial period of a tone is not an offset
  }
  CHECK(r.frames[0].peak_db > kFpLevelFloorDb);  // (its window reaches 50 ms into the tone)
  CHECK(r.frames[1].peak_db > r.frames[0].peak_db);
  CHECK(fingerprint_render(tone(441.18, 0.0, 0.0, 300.0, 300.0)).frames[1].rms_db == kFpLevelFloorDb);
  FP_REQUIRE(r.onsets_ms.size() == 1u);
  CHECK(r.onsets_ms[0] == doctest::Approx(100.0).epsilon(0.001));  // -50 dB is reached within a sample or two of the start (a sine from zero: 0.5 sin(x) = 0.003 at x = 0.0063, 0.01 ms)
}

TEST_CASE("fingerprint: a frame that changes note in the middle has no f0, a frame that holds one has it") {
  Pcm p = tone(441.18, 0.5, 0.0, 150.0, 400.0);
  const Pcm up = tone(661.0, 0.5, 150.0, 400.0, 400.0);
  for (std::size_t i = 0; i < p.lr.size(); ++i) {
    p.lr[i] = static_cast<std::int16_t>(p.lr[i] + up.lr[i]);
  }
  const FpRender r = fingerprint_render(p);
  FP_REQUIRE(r.frames.size() == 4u);
  CHECK(1200.0 * std::fabs(std::log2(r.frames[0].f0 / 441.18)) < 1.0);
  CHECK(r.frames[1].f0 == 0.0);  // 100..200 ms: 441 Hz for the first 50 ms and 661 Hz for the second
  CHECK(1200.0 * std::fabs(std::log2(r.frames[2].f0 / 661.0)) < 1.0);
}

TEST_CASE("fingerprint: a tone in one channel is a balance, a quiet tone has none") {
  const FpRender left = fingerprint_render(tone(441.18, 0.5, 100.0, 900.0, 1000.0, 0.0));
  CHECK(left.frames[4].pan_db == kFpPanLimitDb);
  const FpRender half = fingerprint_render(tone(441.18, 0.5, 100.0, 900.0, 1000.0, 0.5));
  CHECK(half.frames[4].pan_db == doctest::Approx(6.0).epsilon(0.01));
  const FpRender quiet = fingerprint_render(tone(441.18, 0.0002, 100.0, 900.0, 1000.0));  // -74 dB: under the 'quiet' line, no f0, no balance
  CHECK(quiet.frames[4].f0 == 0.0);
  CHECK(quiet.frames[4].pan_db == 0.0);
}

TEST_CASE("fingerprint: the render's length decides the frames; a tail of 50 ms stays, a shorter one goes") {
  CHECK(fingerprint_render(tone(441.18, 0.5, 0.0, 255.0, 255.0)).frames.size() == 3u);   // 100, 100, 55 ms
  CHECK(fingerprint_render(tone(441.18, 0.5, 0.0, 240.0, 240.0)).frames.size() == 2u);   // a tail of 40 ms
  CHECK(fingerprint_render(tone(441.18, 0.5, 0.0, 30.0, 30.0)).frames.size() == 1u);     // one frame at least
  CHECK(fingerprint_render(Pcm()).frames.empty());
  CHECK(fingerprint_render(tone(441.18, 0.5, 0.0, 30.0, 30.0), true).frames[0].f0 == 0.0);
}

TEST_CASE("fingerprint: a planted step is a click in its frame and nowhere else") {
  Pcm p = tone(441.18, 0.5, 0.0, 1000.0, 1000.0);
  const std::size_t at = static_cast<std::size_t>(0.455 * kRate);
  for (std::size_t i = at; i < p.frames(); ++i) {
    for (std::size_t c = 0; c < 2u; ++c) {
      p.lr[2u * i + c] = static_cast<std::int16_t>(std::max(-32768, std::min(32767, p.lr[2u * i + c] + 3000)));
    }
  }
  const FpRender r = fingerprint_render(p);
  for (std::size_t f = 0; f < r.frames.size(); ++f) {
    CHECK((r.frames[f].clicks != 0u) == (f == 4u));
  }
}

TEST_CASE("fingerprint: the onsets: a note after a gap, two notes, a decay that has not fallen far enough") {
  Pcm p = tone(441.18, 0.5, 100.0, 200.0, 1000.0);
  const Pcm second = tone(441.18, 0.5, 300.0, 400.0, 1000.0);
  for (std::size_t i = 0; i < p.lr.size(); ++i) {
    p.lr[i] = static_cast<std::int16_t>(p.lr[i] + second.lr[i]);
  }
  const FpRender r = fingerprint_render(p);
  FP_REQUIRE(r.onsets_ms.size() == 2u);
  CHECK(r.onsets_ms[0] == doctest::Approx(100.0).epsilon(0.001));
  CHECK(r.onsets_ms[1] == doctest::Approx(300.0).epsilon(0.001));
  // a level step while the note sounds (under 12 dB) is not an onset
  Pcm q = tone(441.18, 0.25, 100.0, 600.0, 1000.0);
  const Pcm louder = tone(441.18, 0.25, 300.0, 600.0, 1000.0);
  for (std::size_t i = 0; i < q.lr.size(); ++i) {
    q.lr[i] = static_cast<std::int16_t>(q.lr[i] + louder.lr[i]);
  }
  CHECK(fingerprint_render(q).onsets_ms.size() == 1u);
  // a render that starts in the middle of a sound has its onset at 0
  const FpRender early = fingerprint_render(tone(441.18, 0.5, 0.0, 300.0, 300.0));
  FP_REQUIRE(early.onsets_ms.size() == 1u);
  CHECK(early.onsets_ms[0] < 0.1);
  // silence has none
  CHECK(fingerprint_render(tone(441.18, 0.0, 0.0, 300.0, 300.0)).onsets_ms.empty());
}

TEST_CASE("fingerprint: the same render gives the same text, and the text reads back to the same print") {
  const Pcm p = tone(441.18, 0.5, 100.0, 900.0, 1000.0);
  const FpRender a = fingerprint_render(p);
  const FpRender b = fingerprint_render(p);
  FpSet set;
  set["one"] = a;
  set["two.t1"] = fingerprint_render(tone(661.0, 0.25, 0.0, 400.0, 400.0), true);
  const std::string text = fingerprint_json("k", set);
  CHECK(text == fingerprint_json("k", [&] { FpSet s; s["one"] = b; s["two.t1"] = fingerprint_render(tone(661.0, 0.25, 0.0, 400.0, 400.0), true); return s; }()));
  FpSet back;
  std::string error;
  FP_REQUIRE(parse_fingerprint(text, "k", back, error));
  CHECK(error.empty());
  FP_REQUIRE(back.size() == 2u);
  CHECK(back["two.t1"].nof0);
  CHECK(!back["one"].nof0);
  CHECK(fingerprint_json("k", back) == text);  // every value is already what the file keeps
  const FpCompare c = compare_fingerprints("k", set, back);
  CHECK(c.failures.empty());
  CHECK(c.identical);
  // about 150 bytes a frame
  CHECK(text.size() < 3000u);
}

TEST_CASE("fingerprint: a file that is wrong is refused, with a reason") {
  FpSet set;
  set["r"] = held();
  const std::string good = fingerprint_json("k", set);
  FpSet out;
  std::string error;
  CHECK(parse_fingerprint(good, "k", out, error));
  CHECK(!parse_fingerprint(good, "other", out, error));
  CHECK(error.find("key") != std::string::npos);
  CHECK(!parse_fingerprint(good.substr(0, good.size() / 2), "k", out, error));
  CHECK(!parse_fingerprint("", "k", out, error));
  CHECK(!parse_fingerprint("[1,2]", "k", out, error));
  CHECK(!parse_fingerprint(good + "x", "k", out, error));
  std::string v2 = good;
  v2.replace(v2.find("\"version\": 1"), 12u, "\"version\": 2");
  CHECK(!parse_fingerprint(v2, "k", out, error));
  CHECK(error.find("update_audio_fingerprints") != std::string::npos);
  std::string frames = good;
  frames.replace(frames.find("\"frames\": 10"), 12u, "\"frames\": 11");
  CHECK(!parse_fingerprint(frames, "k", out, error));
  std::string bands = good;
  bands.replace(bands.find("\"bands\": 24"), 11u, "\"bands\": 23");
  CHECK(!parse_fingerprint(bands, "k", out, error));
  std::string clicks = good;
  clicks.replace(clicks.find("\"clicks\": ["), 11u, "\"clicks_x\": [");
  CHECK(!parse_fingerprint(clicks, "k", out, error));
  std::string deep(20, '[');
  CHECK(!parse_fingerprint(deep, "k", out, error));
}

TEST_CASE("fingerprint: a small change of timing and level passes, a large one does not") {
  const FpRender want = held();
  CHECK(holds(want, held()));
  // 0.5 ms later, 0.2 dB louder: the same sound
  const FpCompare near = compare_fingerprints("k", one(want), one(fingerprint_render(tone(441.18, 0.5 * 1.023, 100.5, 900.5, 1000.0))));
  CHECK_MESSAGE(near.failures.empty(), near.markdown);
  // 2 ms later: an onset has moved
  const FpCompare late = compare_fingerprints("k", one(want), one(fingerprint_render(tone(441.18, 0.5, 102.0, 900.0, 1000.0))));
  bool moved = false;
  for (const std::string& f : late.failures) {
    moved = moved || f.find("onset 0 moved") != std::string::npos;
  }
  CHECK(moved);
  // 3 dB louder: the spectrum, the peak and the level
  const FpCompare loud = compare_fingerprints("k", one(want), one(fingerprint_render(tone(441.18, 0.5 * 1.4125, 100.0, 900.0, 1000.0))));
  CHECK(!loud.failures.empty());
  // a tone 2 % up (34 cents)
  const FpCompare sharp = compare_fingerprints("k", one(want), one(fingerprint_render(tone(441.18 * 1.02, 0.5, 100.0, 900.0, 1000.0))));
  bool f0 = false;
  for (const std::string& f : sharp.failures) {
    f0 = f0 || f.find("f0 moved") != std::string::npos;
  }
  CHECK(f0);
  // a different sound in the same place (a second tone an octave up at the same level)
  Pcm two = tone(441.18, 0.35, 100.0, 900.0, 1000.0);
  const Pcm up = tone(882.36, 0.35, 100.0, 900.0, 1000.0);
  for (std::size_t i = 0; i < two.lr.size(); ++i) {
    two.lr[i] = static_cast<std::int16_t>(two.lr[i] + up.lr[i]);
  }
  CHECK(!holds(want, fingerprint_render(two)));
}

TEST_CASE("fingerprint: each rule at its edge") {
  const FpRender want = held();
  // the spectrum is judged on a flat one, every band in range (a band 50 dB under the loudest does not count): a shift of d dB on all 24 bands is a distance of d
  FpRender flat = want;
  for (FpFrame& f : flat.frames) {
    f.bands.fill(-20.0);
  }
  const auto shifted = [&](double db_all, std::size_t only_frame = static_cast<std::size_t>(-1)) {
    FpRender r = flat;
    for (std::size_t f = 0; f < r.frames.size(); ++f) {
      if (only_frame == static_cast<std::size_t>(-1) || f == only_frame) {
        for (double& b : r.frames[f].bands) {
          b += db_all;
        }
      }
    }
    return r;
  };
  // the distance of a frame: 1.4 dB on every band of one frame passes (max 1.4, mean 0.14), 1.6 fails; 0.45 on every frame passes (mean), 0.55 fails (max 0.55)
  CHECK(holds(flat, shifted(1.4, 4)));
  CHECK(!holds(flat, shifted(1.6, 4)));
  CHECK(holds(flat, shifted(0.45)));
  const FpCompare mean = compare_fingerprints("k", one(flat), one(shifted(0.55)));
  FP_REQUIRE(mean.failures.size() == 1u);
  CHECK(mean.failures[0].find("mean spectral distance") != std::string::npos);
  const FpCompare maxed = compare_fingerprints("k", one(flat), one(shifted(1.6, 4)));
  CHECK(maxed.failures[0].find("frame 4 (400-500 ms)") != std::string::npos);
  // a band more than 50 dB under the loudest does not count: 20 dB on the quiet bands of a note moves nothing
  FpRender quiet = want;
  for (double& b : quiet.frames[4].bands) {
    if (b < -60.0) {
      b += 20.0;
    }
  }
  CHECK(holds(want, quiet));
  // the edge of that range, 50 dB: 23 bands 45 dB under the loudest still count (5 dB of them fails), 55 dB under they do not (15 dB of them is nothing)
  const auto under = [&](double level_db) {
    FpRender r = flat;
    for (FpFrame& f : r.frames) {
      f.bands.fill(level_db);
      f.bands[0] = -20.0;
    }
    return r;
  };
  CHECK(!holds(under(-65.0), under(-80.0)));
  CHECK(holds(under(-75.0), under(-90.0)));
  // the onsets: 0.9 ms passes, 1.1 ms fails, a note more or less fails
  FpRender o = want;
  o.onsets_ms[0] += 0.9;
  CHECK(holds(want, o));
  o.onsets_ms[0] = want.onsets_ms[0] + 1.1;
  CHECK(!holds(want, o));
  o = want;
  o.onsets_ms.push_back(500.0);
  CHECK(!holds(want, o));
  // f0: 9 cents passes, 11 fails; a fundamental that appears or goes fails
  FpRender f = want;
  f.frames[4].f0 *= std::pow(2.0, 9.0 / 1200.0);
  CHECK(holds(want, f));
  f.frames[4].f0 = want.frames[4].f0 * std::pow(2.0, 11.0 / 1200.0);
  CHECK(!holds(want, f));
  f.frames[4].f0 = 0.0;
  CHECK(!holds(want, f));
  f = want;
  f.frames[0].f0 = 100.0;
  CHECK(!holds(want, f));
  // levels: 0.9 dB passes, 1.1 fails (peak, rms and balance)
  FpRender l = want;
  l.frames[4].peak_db += 0.9;
  l.frames[4].rms_db -= 0.9;
  l.frames[4].pan_db += 0.9;
  CHECK(holds(want, l));
  l.frames[4].peak_db = want.frames[4].peak_db + 1.1;
  CHECK(!holds(want, l));
  l = want;
  l.frames[4].pan_db += 1.1;
  CHECK(!holds(want, l));
  // dc: an offset of a hundred steps passes, of a thousand does not
  FpRender d = want;
  d.frames[4].dc_lsb += 100.0;
  CHECK(holds(want, d));
  d.frames[4].dc_lsb += 1000.0;
  CHECK(!holds(want, d));
  // clicks: a clean frame must stay clean; 20 may move by 6 (a quarter of the larger, 26 / 4) and not by 7
  FpRender c = want;
  c.frames[4].clicks = 1;
  CHECK(!holds(want, c));
  FpRender busy = want;
  busy.frames[4].clicks = 20;
  c = busy;
  c.frames[4].clicks = 26;
  CHECK(holds(busy, c));
  c.frames[4].clicks = 27;
  CHECK(!holds(busy, c));
  // the shape: another number of frames, the no-f0 flag, a render that is only on one side
  FpRender fewer = want;
  fewer.frames.pop_back();
  CHECK(!holds(want, fewer));
  FpRender nof0 = want;
  nof0.nof0 = true;
  CHECK(!holds(want, nof0));
  FpSet a, b;
  a["x"] = want;
  b["y"] = want;
  const FpCompare sides = compare_fingerprints("k", a, b);
  CHECK(sides.failures.size() == 2u);
  CHECK(!sides.identical);
}

TEST_CASE("fingerprint: the report names the frame, the band and the number") {
  const FpRender want = held();
  FpRender moved = want;
  moved.frames[4].bands[10] -= 3.0;
  moved.onsets_ms[0] += 0.3;
  const FpCompare c = compare_fingerprints("k", one(want), one(moved));
  CHECK(c.failures.empty());  // within the rules: 3 dB in one band of 24 is a distance of 0.6 dB
  CHECK(!c.identical);
  CHECK(c.markdown.find("## k") == 0u);
  CHECK(c.markdown.find("### r: changed within the rules") != std::string::npos);
  CHECK(c.markdown.find("| 4 | 400-500 ms |") != std::string::npos);
  CHECK(c.markdown.find("b10 ") != std::string::npos);
  CHECK(c.markdown.find("onsets (ms)") != std::string::npos);
  const FpCompare same = compare_fingerprints("k", one(want), one(want));
  CHECK(same.identical);
  CHECK(same.markdown.find("identical") != std::string::npos);
}

TEST_CASE("audio_check: band_levels_db: the Parseval check, the floor and the edges") {
  std::vector<double> x(8192);
  for (std::size_t i = 0; i < x.size(); ++i) {
    x[i] = 0.5 * std::sin(kTau * 1000.0 * static_cast<double>(i) / kRate);
  }
  const std::vector<double> bands = band_levels_db(x, 0, 4412, kRate, 24, 30.0, 20000.0, -90.0);
  FP_REQUIRE(bands.size() == 24u);
  double sum = 0.0;
  for (const double b : bands) {
    sum += std::pow(10.0, b / 10.0);
  }
  CHECK(10.0 * std::log10(sum) == doctest::Approx(-9.03).epsilon(0.01));
  // silence, an empty window, a window past the end, bad edges: the floor
  const std::vector<double> zero(4412, 0.0);
  for (const double b : band_levels_db(zero, 0, 4412, kRate, 24, 30.0, 20000.0, -90.0)) {
    CHECK(b == -90.0);
  }
  CHECK(band_levels_db(x, 9000, 100, kRate, 24, 30.0, 20000.0, -90.0)[0] == -90.0);
  CHECK(band_levels_db(x, 0, 0, kRate, 24, 30.0, 20000.0, -90.0)[3] == -90.0);
  CHECK(band_levels_db(x, 0, 100, kRate, 4, 100.0, 50.0, -90.0)[3] == -90.0);
  // a tone below the first edge and above the last is in no band
  std::vector<double> low(8192);
  for (std::size_t i = 0; i < low.size(); ++i) {
    low[i] = 0.5 * std::sin(kTau * 15.0 * static_cast<double>(i) / kRate);
  }
  double top = -90.0;
  for (const double b : band_levels_db(low, 0, 8192, kRate, 24, 30.0, 20000.0, -90.0)) {
    top = std::max(top, b);
  }
  CHECK(top < -35.0);  // only the leakage of the Hann window's skirt reaches the first band
}

}  // namespace
