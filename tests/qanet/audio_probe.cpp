// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-QANET C4: see audio_probe.h. The measurements first (moved from audio_matrix_test.cpp), then the probe.
#include "audio_probe.h"

#include <o3os/app/state_line.h>
#include <o3os/audio/voices.h>
#include <o3os/model/pattern.h>
#include <o3os/model/step.h>
#include <o3os/sim/scenario.h>
#include <o3os/sim/scenario_assert.h>
#include <o3os/sim/sim_audio.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>

namespace o3os::qanet_audio {

// ---- the measurements ------------------------------------------------------------------------------------------------------------------

double cents(double hz, double ref) { return 1200.0 * std::log2(hz / ref); }


// The pitch of a render: YIN readings over 100 ms windows (hop 25 ms) from the onset on, and the pitch most of them agree on (the largest group within 5 cents; a tie goes to the group nearest the median).
// One reading over "the longest loud run" depends on where that run is cut: the Kit's loop_bwd and ping_pong have phase slips at their loop wraps (observed: a zero crossing 0.9 ms late at 81 ms, in every path),
// and a window of 8192 samples that straddles one reads 15 cents flat, while the same note in a longer render (the export) has its window elsewhere and reads true. Windows of one length at fixed offsets from
// the onset see the same stretches in every path, and the group that agrees ignores the ones that straddle a slip. A sound too short for a second window (a slice) is read once.
// THE LIMIT: it reads the majority. A detune or a glide that covers less than about half of the windows (the first 150 to 200 ms of a note, say) is the minority and does not move the reading; two paths that
// differ only there agree here. The spread (the largest window minus the smallest, in cents) is printed with every reading in the log, so that a note that moves shows (`spread` in `say`).
double steady_pitch(const sim::Signal& s, double onset, double* spread_cents) {
  const std::vector<double> x = s.mid();
  const std::size_t win = static_cast<std::size_t>(0.100 * s.rate);
  const std::size_t hop = static_cast<std::size_t>(0.025 * s.rate);
  std::vector<double> reads;
  for (std::size_t at = static_cast<std::size_t>(std::max(0.0, onset) * 0.001 * s.rate); at + 3u * win / 5u <= x.size(); at += hop) {
    const std::size_t end = std::min(x.size(), at + win);
    const double hz = sim::pitch_hz(std::vector<double>(x.begin() + static_cast<std::ptrdiff_t>(at), x.begin() + static_cast<std::ptrdiff_t>(end)), s.rate);
    if (hz > 0.0) {
      reads.push_back(hz);
    }
  }
  if (reads.empty()) {
    return 0.0;
  }
  std::sort(reads.begin(), reads.end());
  if (spread_cents != nullptr) {
    *spread_cents = cents(reads.back(), reads.front());  // the windows' disagreement: what a minority (a glide, a slip) hides in the pitch
  }
  const double median = reads[reads.size() / 2u];
  double best = median;
  std::size_t best_n = 0;
  for (double r : reads) {
    std::size_t n = 0;
    for (double q : reads) {
      n += std::fabs(cents(q, r)) <= kPitchCents ? 1u : 0u;
    }
    if (n > best_n || (n == best_n && std::fabs(cents(r, median)) < std::fabs(cents(best, median)))) {
      best = r;
      best_n = n;
    }
  }
  std::vector<double> group;
  for (double q : reads) {
    if (std::fabs(cents(q, best)) <= kPitchCents) {
      group.push_back(q);
    }
  }
  return group[group.size() / 2u];
}

// The rate of the rising zero crossings (each placed by linear interpolation) from 30 ms after the onset for up to 300 ms: (crossings - 1) over the time from the first to the last. A reading that
// does not depend on a window phase or a dip of the difference function, so that it can say when YIN is wrong (OS-BUG-MODEPITCH). 0: fewer than three crossings.
static double zero_cross_hz(const sim::Signal& s, double onset) {
  const std::vector<double> x = s.mid();
  const std::size_t from = static_cast<std::size_t>((onset + 30.0) * 0.001 * s.rate);
  const std::size_t to = std::min(x.size(), from + static_cast<std::size_t>(0.300 * s.rate));
  unsigned n = 0;
  double first = 0.0;
  double last = 0.0;
  for (std::size_t i = from; i + 1u < to; ++i) {
    if (x[i] <= 0.0 && x[i + 1u] > 0.0) {
      last = static_cast<double>(i) + (0.0 - x[i]) / (x[i + 1u] - x[i]);
      first = n == 0u ? last : first;
      ++n;
    }
  }
  return n < 3u ? 0.0 : static_cast<double>(n - 1u) * s.rate / (last - first);
}

bool Measure::confirmed() const { return hz <= 0.0 || zc_hz <= 0.0 || std::fabs(cents(hz, zc_hz)) <= kZcCents; }

Measure measure(const sim::Pcm& pcm, double* spread_cents) {
  const sim::Signal s = sim::to_signal(pcm);
  Measure m;
  m.onset = sim::onset_ms(s);
  m.hz = m.onset < 0.0 ? 0.0 : steady_pitch(s, m.onset, spread_cents);
  m.zc_hz = m.onset < 0.0 ? 0.0 : zero_cross_hz(s, m.onset);
  return m;
}

// The tone track of a render of the two-pitch sample qaloop (C3 follow-up, tone 2): per 20 ms bin, which of its two tones sounds (1 the low one, 441 Hz, 2 the high one, 689 Hz at the root note; 0 silence),
// told by the zero crossings of the bin. The pitch reading (YIN over 100 ms windows) calls a pure 441 Hz window 224 Hz here (an octave error on a note whose amplitude moves) and cannot tell a forward loop from a
// backward one, whose difference is the ORDER of the tones; the track can. 20 bins: the first 400 ms.
std::vector<int> tone_track(const sim::Pcm& pcm) {
  const std::vector<double> x = sim::to_signal(pcm).mid();
  const std::size_t bin = static_cast<std::size_t>(0.020 * pcm.rate);
  std::vector<int> out;
  for (std::size_t at = 0; out.size() < 20u && at + bin <= x.size(); at += bin) {
    unsigned zc = 0;
    double peak = 0.0;
    for (std::size_t i = at; i + 1u < at + bin; ++i) {
      zc += (x[i] <= 0.0 && x[i + 1u] > 0.0) ? 1u : 0u;
      peak = std::max(peak, std::fabs(x[i]));
    }
    out.push_back(peak < 0.01 ? 0 : (static_cast<double>(zc) / 0.020 < 565.0 ? 1 : 2));
  }
  return out;
}


// One path against the reference (the Pattern step's render). Empty: they agree. `onset_matters` false (the F5 preview only): the onset is a latency of the audition's read, reported by the test and held
// to a budget by OS-QANET D3 (OS-BUG-PREVLAT); here the preview has to sound, and sound at the step's pitch.
std::vector<Finding> compare(const std::string& cell, const std::string& path, const sim::Pcm& got, const sim::Pcm& ref, bool onset_matters, bool tracks) {
  std::vector<Finding> out;
  const Measure g = measure(got);
  const Measure r = measure(ref);
  const std::string paths = path + " vs step";
  char buf[96];
  if ((g.onset < 0.0) != (r.onset < 0.0)) {
    out.push_back({cell, paths, "silent", g.onset < 0.0 ? "the path is silent, the step sounds" : "the path sounds, the step is silent", 0.0});
    return out;
  }
  if (g.onset < 0.0) {
    out.push_back({cell, paths, "dead", "both silent", 0.0});  // a harness problem, reported as a finding so that it cannot pass quietly
    return out;
  }
  if (onset_matters && std::fabs(g.onset - r.onset) > kOnsetMs) {
    std::snprintf(buf, sizeof buf, "onset %+.1f ms", g.onset - r.onset);
    out.push_back({cell, paths, "onset", buf, g.onset - r.onset});
  }
  if (tracks) {
    // a render of two tones is compared by its tone track (below), not by one pitch: the reading is an octave off on some windows of it
  } else if ((g.hz > 0.0) != (r.hz > 0.0)) {
    out.push_back({cell, paths, "nopitch", g.hz > 0.0 ? "the step has no periodic part" : "the path has no periodic part", 0.0});
  } else if (g.hz > 0.0 && std::fabs(cents(g.hz, r.hz)) > kPitchCents && g.confirmed() && r.confirmed()) {  // a difference is reported when the crossing rate backs both readings (OS-BUG-MODEPITCH: YIN alone read -44 c where the crossings said 16 c at most)
    std::snprintf(buf, sizeof buf, "pitch %+.0f c (%.1f Hz against %.1f Hz)", cents(g.hz, r.hz), g.hz, r.hz);
    out.push_back({cell, paths, "pitch", buf, cents(g.hz, r.hz)});
  } else if (g.hz > 0.0 && g.zc_hz > 0.0 && r.zc_hz > 0.0 && !(g.confirmed() && r.confirmed()) && std::fabs(cents(g.zc_hz, r.zc_hz)) > kPitchCents && std::fabs(cents(g.hz, r.hz)) > kPitchCents &&
             (cents(g.hz, r.hz) > 0.0) == (cents(g.zc_hz, r.zc_hz) > 0.0)) {
    // a render the crossings do not back (a harmonic-rich note: YIN and the crossings disagree): the two crossing rates are compared instead, so that a real shift is not dropped. A shift moves both readings the same
    // way; the crossing rate alone moves on a noisy or evolving sound (granular, ring, a swept 303, the Amiga engine's noise) whose YIN pitch is the same in both renders: that is the probe, not the sound.
    std::snprintf(buf, sizeof buf, "pitch %+.0f c by the crossing rate (%.1f Hz against %.1f Hz)", cents(g.zc_hz, r.zc_hz), g.zc_hz, r.zc_hz);
    out.push_back({cell, paths, "pitch", buf, cents(g.zc_hz, r.zc_hz)});
  }
  if (tracks) {  // the order of the two tones (a loop's direction); a boundary may move a bin or two between paths (the onsets differ by under 2 ms), more is a different sound
    const std::vector<int> a = tone_track(got);
    const std::vector<int> b = tone_track(ref);
    unsigned differ = 0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
      differ += a[i] != b[i] ? 1u : 0u;
    }
    if (differ > 3u) {
      std::snprintf(buf, sizeof buf, "tones %u of 20 bins (20 ms each) play the other tone than the step's", differ);
      out.push_back({cell, paths, "tones", buf, static_cast<double>(differ)});
    }
  }
  return out;
}

// `p` played `by` cents higher (a linear-interpolating resampler: the pad path with a planted detune).
sim::Pcm detuned(const sim::Pcm& p, double by) {
  const double ratio = std::pow(2.0, by / 1200.0);
  sim::Pcm q = p;
  const std::size_t n = p.frames();
  for (std::size_t i = 0; i < n; ++i) {
    const double at = static_cast<double>(i) * ratio;
    const std::size_t k = static_cast<std::size_t>(at);
    const double f = at - static_cast<double>(k);
    for (std::size_t ch = 0; ch < 2u; ++ch) {
      const double a = k < n ? p.lr[2u * k + ch] : 0.0;
      const double b = k + 1u < n ? p.lr[2u * (k + 1u) + ch] : 0.0;
      q.lr[2u * i + ch] = static_cast<std::int16_t>(std::lround(a + (b - a) * f));
    }
  }
  return q;
}

// ---- the recipes (the matrix's, moved) ---------------------------------------------------------------------------------------------------------

std::string pick_mode(unsigned mode, bool settle) {
  std::string s = "tap sampleplay\npress shift\ntap f1\nrelease shift\ntap up 8\n";
  if (mode > 0u) {
    s += "tap down " + std::to_string(mode) + "\n";
  }
  return s + "tap f1\n" + (settle ? "wait 50\n" : "");
}
const std::string kChop = "tap sampleplay\ntap sampleplay\nwait 50\n";
const std::string kEven = kChop + "press shift\ntap down 20\nrelease shift\ntap f2\n";
const std::string kAdd = kChop + "tap f4\n";
const std::string kAuto = kChop + "tap f1\n@until inst.1.play_mode=5\n";

// ---- what the probe watches ---------------------------------------------------------------------------------------------------------------------

namespace {

std::uint64_t fnv(const void* p, std::size_t n, std::uint64_t h = 0xCBF29CE484222325ull) {
  for (std::size_t i = 0; i < n; ++i) {
    h = (h ^ static_cast<const unsigned char*>(p)[i]) * 0x100000001B3ull;
  }
  return h;
}

}  // namespace

unsigned watch_instruments(InstWatch& w, const model::Project& p, bool undo, std::string* what) {
  unsigned first = 0;
  for (unsigned i = 0; i < model::kMaxInstruments; ++i) {
    const model::Instrument* in = p.instrument(i);  // (nullptr: the slot holds no instrument yet; a load creates one, which is a change)
    InstWatch::One now;
    now.seen = true;
    if (in != nullptr) {
      const model::SamplerParams& sp = in->sampler;
      now.path = fnv(in->sample_path, sizeof in->sample_path);
      now.slices = fnv(sp.slices, sizeof sp.slices);
      const std::uint16_t window[5] = {sp.start, sp.loop_start, sp.loop_end, sp.end, sp.grain_type};
      now.loop = fnv(window, sizeof window);
      now.mode = sp.play_mode;
      now.count = sp.slice_count;
    }
    InstWatch::One& was = w.one[i];
    if (was.seen && first == 0u) {
      const bool path = now.path != was.path;
      const bool mode = now.mode != was.mode;
      const bool slices = now.slices != was.slices || now.count != was.count;
      const bool loop = now.loop != was.loop;
      if (path || mode || slices || loop) {
        first = i + 1u;
        if (what != nullptr) {
          *what = undo ? "undo" : path ? (was.count > 0u ? "load_over_sliced" : "load") : slices ? "slices" : mode ? "mode" : "loop";
        }
      }
    }
    was = now;
  }
  return first;
}

// ---- the probe ----------------------------------------------------------------------------------------------------------------------------------

namespace {

bool run_text(sim::Simulator& s, const std::string& text, std::string& error) {
  std::vector<sim::ScenarioCommand> cmds;
  if (!sim::parse_scenario(text, cmds, error)) {
    return false;
  }
  return sim::run_scenario(cmds, s, [](const std::string&, const sim::Simulator&) { return true; }, error);
}

std::string state_of(const sim::Simulator& s) {
  char buf[1024];
  const std::size_t n = app::describe_state(s.app(), buf, sizeof buf);
  return std::string(buf, std::min(n, sizeof buf - 1));
}

std::string field_of(const std::string& line, const char* key) {
  const std::string k = std::string(key) + "=";
  std::size_t at = 0;
  while (true) {
    at = line.find(k, at);
    if (at == std::string::npos) {
      return std::string();
    }
    if (at == 0 || line[at - 1] == ' ') {
      break;
    }
    at += k.size();
  }
  at += k.size();
  const std::size_t end = line.find(' ', at);
  return line.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

// Steps until no voice sounds (a pad's note, the transport's: the sample bank rebinds in a quiet window only, and 20 ms past it the rebind has landed). false: still sounding at the bound.
bool wait_quiet(sim::Simulator& s, unsigned max_ms) {
  for (unsigned i = 0; i < max_ms; ++i) {
    if (s.audio()->voices().voices_in_use() == 0u) {
      s.wait_ms(20);
      return s.audio()->voices().voices_in_use() == 0u;
    }
    s.step();
  }
  return false;
}

// A render that is still loud 250 to 350 ms after it started sounding: a sustained note (a loop, a long sample), whose pitch is read; a one shot of a few ms has no pitch to compare.
bool sustained(const sim::Pcm& p, double onset) {
  if (onset < 0.0) {
    return false;
  }
  const sim::Signal all = sim::to_signal(p);
  double peak = 0.0;
  for (double v : all.mid()) {
    peak = std::max(peak, std::fabs(v));
  }
  const sim::Signal late = sim::to_signal(p, onset + 250.0, onset + 350.0);
  double late_peak = 0.0;
  for (double v : late.mid()) {
    late_peak = std::max(late_peak, std::fabs(v));
  }
  return peak > 0.0 && late.size() > 0u && late_peak > 0.05 * peak;
}

// The shape of the sound against the reference's: the best normalised correlation of the first 150 ms (from each one's onset, the lag searched within 2.5 ms) of the mid channel. A sound played at the
// wrong pitch is stretched or squeezed and does not line up whatever the lag; two renders of one note do (amplitude and pan do not matter). 1.0 for a pair that has nothing to compare.
double shape_correlation(const sim::Pcm& got, double got_onset, const sim::Pcm& ref, double ref_onset) {
  const std::vector<double> g = sim::to_signal(got).mid();
  const std::vector<double> r = sim::to_signal(ref).mid();
  const double rate = ref.rate;
  const long gs = static_cast<long>(got_onset * 0.001 * rate);
  const long rs = static_cast<long>(ref_onset * 0.001 * rate);
  const long len = static_cast<long>(0.150 * rate);
  const long lag_max = static_cast<long>(0.0025 * rate);
  double best = -1.0;
  for (long lag = -lag_max; lag <= lag_max; ++lag) {
    double sgg = 0.0, srr = 0.0, sgr = 0.0;
    long n = 0;
    for (long i = 0; i < len; ++i) {
      const long gi = gs + lag + i;
      const long ri = rs + i;
      if (gi < 0 || ri < 0 || gi >= static_cast<long>(g.size()) || ri >= static_cast<long>(r.size())) {
        continue;
      }
      sgg += g[static_cast<std::size_t>(gi)] * g[static_cast<std::size_t>(gi)];
      srr += r[static_cast<std::size_t>(ri)] * r[static_cast<std::size_t>(ri)];
      sgr += g[static_cast<std::size_t>(gi)] * r[static_cast<std::size_t>(ri)];
      ++n;
    }
    if (n > 0 && sgg > 0.0 && srr > 0.0) {
      best = std::max(best, sgr / std::sqrt(sgg * srr));
    }
  }
  return best < -0.5 ? 1.0 : best;
}

constexpr double kShapeMin = 0.90;  // below it the two sounds are not one note at one pitch

// compare() for any content: onset, and the pitch when both sounds are sustained (a short one shot has no pitch worth 5 cents), and the shape of the sound.
std::vector<Finding> compare_any(const std::string& cell, const std::string& path, const sim::Pcm& got, const sim::Pcm& ref) {
  std::vector<Finding> all = compare(cell, path, got, ref);
  const Measure g = measure(got);
  const Measure r = measure(ref);
  const bool both = sustained(got, g.onset) && sustained(ref, r.onset);
  std::vector<Finding> out;
  for (Finding& f : all) {
    if ((f.what == "pitch" || f.what == "nopitch") && !both) {
      continue;
    }
    out.push_back(f);
  }
  if (g.onset >= 0.0 && r.onset >= 0.0) {
    const double c = shape_correlation(got, g.onset, ref, r.onset);
    if (c < kShapeMin) {
      char buf[96];
      std::snprintf(buf, sizeof buf, "shape %.2f (the first 150 ms of the sound do not line up with the step's)", c);
      out.push_back({cell, path + " vs step", "shape", buf, c});
    }
  }
  return out;
}

// Does the pattern in view play nothing in its first eight rows? (the step reference is the only sound then)
bool quiet_rows(const sim::Simulator& s, unsigned pattern) {
  const model::Pattern* p = s.project() != nullptr ? s.project()->pattern(pattern) : nullptr;
  if (p == nullptr) {
    return false;
  }
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    for (unsigned r = 0; r < 8u; ++r) {
      if (!model::is_empty(p->steps[t][r])) {
        return false;
      }
    }
  }
  return true;
}

// No track is muted and none is soloed: the step plays.
bool tracks_open(const sim::Simulator& s) {
  for (unsigned t = 0; t < s.project()->active_tracks(); ++t) {
    const model::TrackSlot* slot = s.project()->track(t);
    if (slot != nullptr && (slot->flags & (model::kTrackMute | model::kTrackSolo)) != 0u) {
      return false;
    }
  }
  return true;
}

unsigned pad_of(const model::Instrument& in) { return in.sampler.play_mode == 5u ? 1u : 16u; }

}  // namespace

bool probe_possible(const sim::Simulator& s, unsigned instrument, std::string* why) {
  std::string reason;
  const model::Instrument* in = s.project() != nullptr && instrument >= 1u && instrument <= model::kMaxInstruments ? s.project()->instrument(instrument - 1u) : nullptr;
  if (!s.has_audio() || s.project() == nullptr) {
    reason = "no engine";
  } else if (in == nullptr || in->kind != model::InstrumentKind::Sample || in->sample_path[0] == '\0') {
    reason = "instrument " + std::to_string(instrument) + " has no sample";  // nothing a pad would play (no sample, a MIDI or module instrument)
  } else if (field_of(state_of(s), "playing") == "1") {
    reason = "the transport is playing";  // the descriptor is held for as long as it plays (the ring cells of the matrix size that); the probe needs a stopped one
  } else if (!tracks_open(s)) {
    reason = "a track is muted or soloed";  // a muted track or a solo silences the pad: the fresh pad could not be told from a stale one (clearing them first would take the 5 ms the fresh pad is about)
  }
  if (why != nullptr) {
    *why = reason;
  }
  return reason.empty();
}

void probe_instrument(sim::Simulator& s, unsigned instrument, const std::string& label, const ProbeOptions& opt, ProbeReport& rep) {
  std::string why_not_probed;
  if (!probe_possible(s, instrument, &why_not_probed)) {
    ++rep.skipped;
    rep.log.push_back(label + ": " + why_not_probed);
    return;
  }
  const model::Instrument* in = s.project()->instrument(instrument - 1u);
  const unsigned pad = pad_of(*in);
  std::string error;
  const std::string tag = std::to_string(rep.probes + rep.skipped + 1u) + "_" + std::to_string(instrument);
  // The pad straight after the edit: the Pattern screen (two ticks) and the pad, as the matrix's `.fresh` runs do. A pad is held 200 ms in a 400 ms render.
  const std::string selecting = field_of(state_of(s), "instrument") != std::to_string(instrument) ? "action instrument.select " + std::to_string(instrument) + "\n" : std::string();
  if (!run_text(s, selecting + "tap pattern\n", error) || !s.render_begin("pf" + tag, 400, false, error)) {
    ++rep.skipped;
    rep.log.push_back(label + ": " + error);
    return;
  }
  s.pad_hold(pad, 200);
  s.wait_ms(200);
  if (field_of(state_of(s), "screen") != "PATTERN") {
    ++rep.skipped;
    rep.log.push_back(label + ": the Pattern screen is not reachable (a dialog is up)");
    return;
  }
  // Let the stale voice go (a note on a wrong descriptor can be seconds long): Play and Stop end every note, then the pool is quiet and the bank has rebound.
  if (!run_text(s, "tap play\nwait 30\ntap play\n", error) || !wait_quiet(s, 4000)) {
    ++rep.skipped;
    rep.log.push_back(label + ": the voices did not go quiet " + error);
    return;
  }
  if (!s.render_begin("ps" + tag, 400, false, error)) {
    ++rep.skipped;
    rep.log.push_back(label + ": " + error);
    return;
  }
  s.pad_hold(pad, 200);
  s.wait_ms(200);
  if (!run_text(s, "tap play\nwait 30\ntap play\n", error) || !wait_quiet(s, 4000)) {
    ++rep.skipped;
    rep.log.push_back(label + ": the settled pad did not play out " + error);
    return;
  }
  const sim::Pcm* fresh = s.render_result("pf" + tag);
  const sim::Pcm* settled0 = s.render_result("ps" + tag);
  if (fresh == nullptr || settled0 == nullptr) {
    ++rep.skipped;
    rep.log.push_back(label + ": a render did not close");
    return;
  }
  sim::Pcm settled = *settled0;
  if (opt.plant_detune_cents != 0.0 && opt.plant_label == label) {
    settled = detuned(settled, opt.plant_detune_cents);  // the self-test's fault: a pad path that stays off pitch
  }
  ++rep.probes;
  // The Pattern step: clear the first eight rows of the pattern in view (nothing else may sound), write the pad's note in the first row of the cursor's track, play the pattern.
  const sim::Pcm* step = nullptr;
  std::string why_not;
  const std::string st0 = state_of(s);
  const unsigned pattern = static_cast<unsigned>(std::strtoul(field_of(st0, "pattern").c_str(), nullptr, 10));
  if (!tracks_open(s)) {
    why_not = "a track is muted or soloed";
  } else {
    if (!quiet_rows(s, pattern)) {
      std::string text = "tap pattern\nwait 50\n";
      text += field_of(state_of(s), "mode") != "0" ? "tap rec\n" : std::string();
      text += "action cursor.jump_fraction 0\ntap left 100\npress shift\ntap right 40\ntap down 7\nrelease shift\ntap delete\nwait 50\ntap rec\nwait 20\n";
      if (!run_text(s, text, error)) {
        why_not = "the clear failed: " + error;
      }
    }
    if (why_not.empty() && !quiet_rows(s, pattern)) {
      why_not = "the first rows would not clear";
    }
  }
  if (why_not.empty()) {
    std::string text = "tap pattern\nwait 50\n";
    text += field_of(state_of(s), "mode") != "0" ? "tap rec\n" : std::string();
    text += "tap note\naction cursor.jump_fraction 0\npad " + std::to_string(pad) + "\ntap rec\nwait 600\ntap pattern\nwait 100\n";
    if (!run_text(s, text, error) || !s.render_begin("pt" + tag, 400, false, error) || !run_text(s, "tap play\nwait 200\ntap play\nwait 196\n", error)) {
      why_not = "the step could not be written: " + error;
    } else {
      step = s.render_result("pt" + tag);
      if (step == nullptr || measure(*step).onset < 0.0) {
        why_not = "the step is silent";
        step = nullptr;
      }
    }
  }
  const sim::Pcm& ref = step != nullptr ? *step : settled;
  const sim::Pcm fresh_copy = *fresh;
  std::string line = label + ": instrument " + std::to_string(instrument) + ", reference " + (step != nullptr ? "the Pattern step" : "the settled pad (" + why_not + ")");
  if (step != nullptr) {
    ++rep.stepped;
  } else {
    ++rep.unstepped;
  }
  const Measure mf = measure(fresh_copy);
  const Measure ms = measure(settled);
  char buf[160];
  std::snprintf(buf, sizeof buf, "; pad fresh %.1f Hz @%.1f ms, pad settled %.1f Hz @%.1f ms", mf.hz, mf.onset, ms.hz, ms.onset);
  line += buf;
  if (mf.onset < 0.0 && ms.onset < 0.0 && (step == nullptr || measure(*step).onset < 0.0)) {
    line += "; nothing sounds (no finding)";  // a sample of silence, or an instrument with nothing to play
    rep.log.push_back(line);
    return;
  }
  rep.log.push_back(line);
  for (Finding& f : compare_any(label, "pad:fresh", fresh_copy, ref)) {
    rep.findings.push_back(std::move(f));
  }
  if (step != nullptr) {
    for (Finding& f : compare_any(label, "pad:settled", settled, ref)) {
      rep.findings.push_back(std::move(f));
    }
  }
}

namespace {

bool ends_wav(const std::string& name) {
  std::string l = name;
  for (char& c : l) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return l.size() > 4u && l.compare(l.size() - 4u, 4u, ".wav") == 0;
}

bool bound(sim::Simulator& s, unsigned slot, unsigned max_ticks) {
  for (unsigned i = 0; i < max_ticks; ++i) {
    if (s.audio()->voices().sample(slot).length != 0u) {
      return true;
    }
    s.step();
  }
  return false;
}

// The walk is left wherever it was: a chooser or a dialog up, the transport playing, a card job in flight. The session wants none of them (a fork: nothing of it goes back to the walk).
bool settle_ui(sim::Simulator& s) {
  std::string error;
  for (unsigned i = 0; i < 4u; ++i) {
    const std::string st = state_of(s);
    const std::string overlay = field_of(st, "overlay");
    if (overlay != "NONE" && overlay != "NOTIFY") {
      if (!run_text(s, "tap f8\nwait 50\n", error)) {
        return false;
      }
    } else if (field_of(st, "playing") == "1") {
      if (!run_text(s, "tap play\nwait 100\n", error)) {
        return false;
      }
    } else {
      break;
    }
  }
  for (unsigned i = 0; i < 6000u && s.app().card_jobs_busy(); ++i) {
    s.step();
  }
  (void)run_text(s, "tap master\nwait 50\npress shift\ntap f7\ntap f8\nrelease shift\nwait 50\ntap pattern\nwait 50\n", error);  // the Mixer screen's Shift + F7 and Shift + F8: every mute and every solo off (a muted track or a solo silences the pad, and the walk presses those keys too)
  const std::string st = state_of(s);
  return (field_of(st, "overlay") == "NONE" || field_of(st, "overlay") == "NOTIFY") && field_of(st, "playing") != "1" && !s.app().card_jobs_busy();
}

// The edit session: a WAV of the card goes into the instrument, in a loop, and a few edits of the Wave screen are made and probed one by one.
void edit_session(sim::Simulator& s, unsigned instrument, const WalkProbe& in, const ProbeOptions& opt, ProbeReport& rep) {
  const unsigned slot = instrument - 1u;
  std::string error;
  if (!settle_ui(s)) {
    ++rep.skipped;
    rep.log.push_back("session: the walk's screen would not settle (" + state_of(s) + ")");
    return;
  }
  // To the card's root (F2, the folder above, until the listing stops changing; the walk may be anywhere), where the WAVs of the card are.
  if (!run_text(s, "tap pattern\naction instrument.select " + std::to_string(instrument) + "\ntap sampleload\nwait 400\ntap f2\nwait 300\ntap f2\nwait 300\ntap f2\nwait 300\ntap f2\nwait 300\ntap f2\nwait 300\ntap f2\nwait 1500\n", error)) {
    rep.error = "session: " + error;
    return;
  }
  for (unsigned i = 0; i < 6000u && s.app().card_jobs_busy(); ++i) {  // (the listing's own scans are card jobs too)
    s.step();
  }
  bool found = false;
  const std::string current = s.project()->instrument(slot) != nullptr ? std::string(s.project()->instrument(slot)->sample_path) : std::string();
  for (unsigned i = 0; i < 80u && !found; ++i) {
    const std::string sel = field_of(state_of(s), "sel");
    found = ends_wav(sel) && (current.size() < sel.size() || current.compare(current.size() - sel.size(), sel.size(), sel) != 0);  // a file the instrument does not hold already (the same path would change nothing)
    if (!found && !run_text(s, "tap down\n", error)) {
      return;
    }
  }
  if (!found) {
    ++rep.skipped;
    rep.log.push_back("session: no WAV under the cursor of Sample load (" + state_of(s) + ")");
    return;
  }
  InstWatch w;
  std::string what;
  (void)watch_instruments(w, *s.project(), false, nullptr);
  // (an instrument that is there asks REPLACE first, OS-BUG-SLDOVER: the second F1 answers)
  if (!run_text(s, s.project()->instrument(slot) != nullptr ? "tap f1\nwait 50\ntap f1\n" : "tap f1\n", error)) {
    rep.error = "session: " + error;
    return;
  }
  // The load, probed as soon as the model says it landed (the import's job sets the name some passes before the bank binds the audio: the slot is empty to the engine and a note on it is dropped).
  bool landed = false;
  for (unsigned i = 0; i < 3000u && !landed; ++i) {
    landed = watch_instruments(w, *s.project(), false, &what) != 0u;
    if (!landed) {
      s.step();
    }
  }
  if (!landed) {
    ++rep.skipped;
    std::string paths;
    for (unsigned i = 0; i < model::kMaxInstruments; ++i) {
      const model::Instrument* x = s.project()->instrument(i);
      paths += x != nullptr && x->sample_path[0] != '\0' ? " " + std::to_string(i + 1) + "=" + x->sample_path : std::string();
    }
    rep.log.push_back("session: the WAV did not load (" + state_of(s) + ", instruments with a sample:" + paths + ", was `" + current + "` in " + std::to_string(instrument) + ")");
    return;
  }
  probe_instrument(s, instrument, what, opt, rep);
  if (!bound(s, slot, 3000)) {
    ++rep.skipped;
    rep.log.push_back("session: the loaded WAV was never bound");
    return;
  }
  // The edits: the first is the loop (the sustained note whose pitch is read to 5 cents), then those the options name or the seed draws.
  std::vector<std::string> names;
  if (!opt.session_edits.empty()) {
    std::istringstream ss(opt.session_edits);
    for (std::string n; std::getline(ss, n, ',');) {
      names.push_back(n);
    }
  } else {
    // Six programs of three edits, dealt out by the seed (each is a SLICEPITCH-prone path: a play mode, a chop, an undo, a load over a chop). The load is probed first, whatever the program.
    static const char* const kPrograms[][3] = {{"mode1", "even", "undo"}, {"mode1", "add", "reload"}, {"mode1", "auto", "undo"}, {"mode1", "even", "del"}, {"mode2", "mode6", "undo"}, {"mode1", "even", "reload"}};
    const unsigned k = static_cast<unsigned>(in.seed % (sizeof kPrograms / sizeof kPrograms[0]));
    for (const char* n : kPrograms[k]) {
      if (names.size() < opt.session_edits_max) {
        names.push_back(n);
      }
    }
  }
  for (const std::string& n : names) {
    const model::Instrument* cur = s.project()->instrument(slot);
    std::string text;
    bool job = false;
    if (n == "even") {
      text = kEven;
    } else if (n == "add") {
      text = kAdd;
    } else if (n == "auto") {
      text = kChop + "tap f1\n";
      job = true;
    } else if (n == "del") {
      text = kChop + "tap f5\n";
    } else if (n == "undo") {
      text = kChop + "tap f8\n";
    } else if (n == "reload") {
      text = "tap sampleload\nwait 500\ntap f1\nwait 50\ntap f1\n";
    } else if (n.compare(0, 4, "mode") == 0) {
      text = pick_mode(static_cast<unsigned>(std::atoi(n.c_str() + 4)), false);
    } else {
      rep.error = "session: no edit named " + n;
      return;
    }
    (void)cur;
    // The edit's keys, all but the last, then the last key as the edit itself: the probe's first input follows it with no wait.
    if (!run_text(s, text, error)) {
      rep.error = "session " + n + ": " + error;
      return;
    }
    if (job) {  // the slicer answers some passes later (the matrix's `@until inst.1.play_mode=5`)
      for (unsigned i = 0; i < 3000u; ++i) {
        const model::Instrument* now = s.project()->instrument(slot);
        if (now != nullptr && now->sampler.play_mode == 5u) {
          break;
        }
        s.step();
      }
    } else if (n == "reload") {
      for (unsigned i = 0; i < 700u; ++i) {  // the import is a job too; the model's slices clear when the load lands
        s.step();
      }
    }
    if (watch_instruments(w, *s.project(), n == "undo", &what) != 0u) {
      const std::string label = n == "reload" ? what : (n.compare(0, 4, "mode") == 0 ? std::string("mode") : n);  // an edit is named by what the person did
      probe_instrument(s, instrument, label, opt, rep);
    }
  }
}

}  // namespace

ProbeReport probe_walk_state(const WalkProbe& in, const ProbeOptions& opt) {
  ProbeReport rep;
  namespace fs = std::filesystem;
  auto s = std::make_unique<sim::Simulator>();
  const fs::path card(in.card_dir);
  s->set_scenario_dir(card.parent_path().string());
  std::string text = "document\ncard " + card.filename().string() + "\naudio on\n";
  for (const std::string& l : in.lines) {
    text += l + "\n";
  }
  std::string error;
  if (!run_text(*s, text, error)) {
    rep.error = "the replay of the walk: " + error;
    return rep;
  }
  if (s->project() == nullptr || sim::model_digest(*s->project()) != in.digest) {
    ++rep.diverged;
    rep.log.push_back("the replay of the walk did not end in its document (" + (s->project() != nullptr ? sim::model_digest(*s->project()).substr(0, 12) : std::string("none")) + ", the walk's was " + in.digest.substr(0, 12) + "): not probed");
    return rep;
  }
  if (in.probe_now && in.instrument != 0u) {
    probe_instrument(*s, in.instrument, in.label, opt, rep);
  }
  if (in.session && opt.session) {
    edit_session(*s, in.instrument != 0u ? in.instrument : 1u, in, opt, rep);
  }
  return rep;
}

}  // namespace o3os::qanet_audio
