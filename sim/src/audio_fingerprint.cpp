// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/sim/audio_fingerprint.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace o3os::sim {

namespace {

// A value rounded to the digits the file keeps (scale 10: 0.1, 100: 0.01), with no negative zero: the print and its re-read are the same double.
double rounded(double v, double scale) {
  const double r = std::round(v * scale) / scale;
  return r == 0.0 ? 0.0 : r;
}

double clamped(double v, double lo, double hi) { return std::min(hi, std::max(lo, v)); }

// The fundamental of the frame mid[at, at + m) when its two halves have the same one (within 20 cents; the geometric mean of the two), else 0. A frame that holds a change of note gives YIN a tie
// between two equally steady notes, which a tiny change of level or timing breaks the other way: such a frame has no f0, and a note held through both halves has one.
double steady_f0(const std::vector<double>& mid, std::size_t at, std::size_t m, double rate) {
  const std::size_t half = m / 2u;
  const auto slice = [&](std::size_t from, std::size_t to) { return std::vector<double>(mid.begin() + static_cast<std::ptrdiff_t>(from), mid.begin() + static_cast<std::ptrdiff_t>(to)); };
  const double a = pitch_hz(slice(at, at + half), rate);
  const double b = pitch_hz(slice(at + half, at + m), rate);
  if (!(a > 0.0) || !(b > 0.0) || 1200.0 * std::fabs(std::log2(a / b)) > 20.0) {
    return 0.0;
  }
  return std::sqrt(a * b);
}

// ---- the onsets --------------------------------------------------------------------------------------------------------------------------

std::vector<double> find_onsets(const Pcm& pcm) {
  std::vector<double> out;
  const double decay = std::exp(-1.0 / (pcm.rate * 0.010));
  const double floor_amp = std::pow(10.0, -50.0 / 20.0);
  double env = 0.0;   // the peak follower
  double low = 0.0;   // its lowest value since it was armed (0: the render starts from silence)
  double peak = 0.0;  // its highest since the onset
  bool armed = true;
  for (std::size_t i = 0; i < pcm.frames(); ++i) {
    const double l = std::fabs(static_cast<double>(pcm.lr[2u * i])) * kLsb;
    const double r = std::fabs(static_cast<double>(pcm.lr[2u * i + 1u])) * kLsb;
    env = std::max(std::max(l, r), env * decay);
    if (armed) {
      if (env >= floor_amp && env >= 4.0 * low) {
        if (out.size() < kFpMaxOnsets) {
          out.push_back(rounded(1000.0 * static_cast<double>(i) / pcm.rate, 100.0));
        }
        armed = false;
        peak = env;
      } else {
        low = std::min(low, env);
      }
    } else {
      peak = std::max(peak, env);
      if (env < 0.25 * peak) {
        armed = true;
        low = env;
      }
    }
  }
  return out;
}

// ---- the file ----------------------------------------------------------------------------------------------------------------------------

// A number with at most `digits` decimals, without the zeros at its end ("-90", "-14.1"): the files are mostly the floor's -90.
void put(std::string& s, int digits, double v) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.*f", digits, v);
  std::size_t n = std::strlen(buf);
  if (std::strchr(buf, '.') != nullptr) {
    while (buf[n - 1u] == '0') {
      buf[--n] = '\0';
    }
    if (buf[n - 1u] == '.') {
      buf[--n] = '\0';
    }
  }
  s += buf;
}

template <typename Get>
void put_array(std::string& s, const char* name, const std::vector<FpFrame>& frames, int digits, Get get) {
  s += "   \"";
  s += name;
  s += "\": [";
  for (std::size_t i = 0; i < frames.size(); ++i) {
    if (i != 0u) {
      s += ',';
    }
    put(s, digits, get(frames[i]));
  }
  s += "],\n";
}

// A small JSON reader for the files above (and anything else of the same shape): numbers, strings without escapes beyond \" and \\, arrays, objects; at most 8 levels deep.
struct Json {
  enum class T : std::uint8_t { Null, Num, Str, Arr, Obj };
  T t = T::Null;
  double num = 0.0;
  std::string str;
  std::vector<Json> arr;
  std::vector<std::string> keys;  // Obj: the names, and in `arr` the values
};

class JsonReader {
 public:
  explicit JsonReader(const std::string& s) : s_(s) {}
  bool parse(Json& out, std::string& error) {
    if (!value(out, 0) || (skip(), i_ != s_.size())) {
      error = err_.empty() ? "unexpected text after the value (offset " + std::to_string(i_) + ")" : err_;
      return false;
    }
    return true;
  }

 private:
  void skip() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) {
      ++i_;
    }
  }
  bool fail(const char* what) {
    if (err_.empty()) {
      err_ = std::string(what) + " (offset " + std::to_string(i_) + ")";
    }
    return false;
  }
  bool string(std::string& out) {
    ++i_;  // the opening quote
    while (i_ < s_.size() && s_[i_] != '"') {
      if (s_[i_] == '\\' && i_ + 1u < s_.size() && (s_[i_ + 1u] == '"' || s_[i_ + 1u] == '\\')) {
        ++i_;
      }
      out += s_[i_++];
    }
    if (i_ >= s_.size()) {
      return fail("a string is not closed");
    }
    ++i_;
    return true;
  }
  bool value(Json& out, unsigned depth) {
    skip();
    if (depth > 8u || i_ >= s_.size()) {
      return fail(depth > 8u ? "nested too deep" : "the text ends early");
    }
    const char c = s_[i_];
    if (c == '"') {
      out.t = Json::T::Str;
      return string(out.str);
    }
    if (c == '[' || c == '{') {
      const bool object = c == '{';
      out.t = object ? Json::T::Obj : Json::T::Arr;
      ++i_;
      skip();
      if (i_ < s_.size() && s_[i_] == (object ? '}' : ']')) {
        ++i_;
        return true;
      }
      for (;;) {
        skip();
        if (object) {
          std::string key;
          if (i_ >= s_.size() || s_[i_] != '"' || !string(key)) {
            return fail("a name is expected");
          }
          skip();
          if (i_ >= s_.size() || s_[i_] != ':') {
            return fail("':' is expected");
          }
          ++i_;
          out.keys.push_back(key);
        }
        out.arr.emplace_back();
        if (!value(out.arr.back(), depth + 1u)) {
          return false;
        }
        skip();
        if (i_ < s_.size() && s_[i_] == ',') {
          ++i_;
          continue;
        }
        if (i_ < s_.size() && s_[i_] == (object ? '}' : ']')) {
          ++i_;
          return true;
        }
        return fail("',' or the end of the list is expected");
      }
    }
    char* end = nullptr;
    const double v = std::strtod(s_.c_str() + i_, &end);
    if (end == s_.c_str() + i_ || !std::isfinite(v)) {
      return fail("a value is expected");
    }
    i_ = static_cast<std::size_t>(end - s_.c_str());
    out.t = Json::T::Num;
    out.num = v;
    return true;
  }

  const std::string& s_;
  std::size_t i_ = 0;
  std::string err_;
};

const Json* member(const Json& obj, const char* name) {
  for (std::size_t i = 0; i < obj.keys.size(); ++i) {
    if (obj.keys[i] == name) {
      return &obj.arr[i];
    }
  }
  return nullptr;
}

bool number_array(const Json& render, const char* name, std::size_t want, std::vector<double>& out, std::string& error) {
  const Json* a = member(render, name);
  if (a == nullptr || a->t != Json::T::Arr || (want != static_cast<std::size_t>(-1) && a->arr.size() != want)) {
    error = std::string("'") + name + "' is missing or is not a list of " + (want == static_cast<std::size_t>(-1) ? "numbers" : std::to_string(want) + " numbers");
    return false;
  }
  out.clear();
  for (const Json& v : a->arr) {
    if (v.t != Json::T::Num) {
      error = std::string("'") + name + "' holds something that is not a number";
      return false;
    }
    out.push_back(v.num);
  }
  return true;
}

// ---- the comparison ----------------------------------------------------------------------------------------------------------------------

std::string fixed(double v, int digits) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.*f", digits, v);
  return buf;
}

std::string band_label(unsigned band) {
  double lo = 0.0, hi = 0.0;
  fp_band_edges(band, lo, hi);
  return "b" + std::to_string(band) + " " + fixed(lo, 0) + "-" + fixed(hi, 0) + " Hz";
}

std::string span_ms(std::size_t frame) {
  const long ms = static_cast<long>(kFpFrameMs);
  return std::to_string(static_cast<long>(frame) * ms) + "-" + std::to_string(static_cast<long>(frame + 1u) * ms) + " ms";
}

// The difference of one band of two frames, in dB, where a band more than kFpBandRangeDb under the loudest band of either frame counts as being at that level: the skirts of a window and the
// splash of an attack are 70 dB and more under a note and move by tens of dB when it moves by a millisecond, which nobody hears.
double band_delta(const FpFrame& a, const FpFrame& b, unsigned band) {
  const double floor = std::max(*std::max_element(a.bands.begin(), a.bands.end()), *std::max_element(b.bands.begin(), b.bands.end())) - kFpBandRangeDb;
  return std::max(b.bands[band], floor) - std::max(a.bands[band], floor);
}

bool same_frame(const FpFrame& a, const FpFrame& b) {
  return a.bands == b.bands && a.f0 == b.f0 && a.peak_db == b.peak_db && a.rms_db == b.rms_db && a.pan_db == b.pan_db && a.dc_lsb == b.dc_lsb && a.clicks == b.clicks;
}

std::string list_of(const std::vector<double>& v) {
  std::string s = "[";
  for (std::size_t i = 0; i < v.size(); ++i) {
    s += (i != 0u ? ", " : "") + fixed(v[i], 2);
  }
  return s + "]";
}

// A bounded list of findings: the first few in full, then a count.
class Findings {
 public:
  explicit Findings(std::vector<std::string>* sink, const std::string& name) : sink_(sink), name_(name) {}
  void add(const std::string& what) {
    if (++count_ <= kShown) {
      sink_->push_back(name_ + ": " + what);
    }
  }
  void finish() {
    if (count_ > kShown) {
      sink_->push_back(name_ + ": ... and " + std::to_string(count_ - kShown) + " more");
    }
  }

 private:
  static constexpr unsigned kShown = 6;
  std::vector<std::string>* sink_;
  std::string name_;
  unsigned count_ = 0;
};

void compare_render(const std::string& name, const FpRender& want, const FpRender& got, FpCompare& out) {
  Findings fail(&out.failures, name);
  const bool same = want.nof0 == got.nof0 && want.onsets_ms == got.onsets_ms && want.frames.size() == got.frames.size() &&
                    std::equal(want.frames.begin(), want.frames.end(), got.frames.begin(), same_frame);
  if (same) {
    out.markdown += "### " + name + ": identical (" + std::to_string(got.frames.size()) + " frames, " + std::to_string(got.onsets_ms.size()) + " onsets)\n\n";
    return;
  }
  out.identical = false;
  std::string md;
  bool failed = false;
  const auto bad = [&](const std::string& what) {
    fail.add(what);
    failed = true;
  };
  if (want.nof0 != got.nof0) {
    bad(std::string("the scenario's `fingerprint` line is ") + (got.nof0 ? "now" : "no longer") + " `nof0`, the stored one is not: run update_audio_fingerprints");
  }
  if (want.frames.size() != got.frames.size()) {
    bad("the render has " + std::to_string(got.frames.size()) + " frames, the stored fingerprint " + std::to_string(want.frames.size()));
  }
  const std::size_t frames = std::min(want.frames.size(), got.frames.size());

  // the spectrum
  double lsd_max = 0.0, lsd_sum = 0.0;
  std::size_t lsd_at = 0;
  std::vector<double> lsd(frames, 0.0);
  for (std::size_t f = 0; f < frames; ++f) {
    double sq = 0.0;
    for (unsigned b = 0; b < kFpBands; ++b) {
      const double d = band_delta(want.frames[f], got.frames[f], b);
      sq += d * d;
    }
    lsd[f] = std::sqrt(sq / kFpBands);
    lsd_sum += lsd[f];
    if (lsd[f] > lsd_max) {
      lsd_max = lsd[f];
      lsd_at = f;
    }
  }
  const double lsd_mean = frames != 0u ? lsd_sum / static_cast<double>(frames) : 0.0;
  if (lsd_max > kFpLsdMaxDb) {
    bad("frame " + std::to_string(lsd_at) + " (" + span_ms(lsd_at) + "): spectral distance " + fixed(lsd_max, 2) + " dB, over " + fixed(kFpLsdMaxDb, 1) + " dB");
  }
  if (lsd_mean > kFpLsdMeanDb) {
    bad("mean spectral distance " + fixed(lsd_mean, 2) + " dB, over " + fixed(kFpLsdMeanDb, 1) + " dB");
  }
  md += "- spectral distance: max " + fixed(lsd_max, 2) + " dB (frame " + std::to_string(lsd_at) + "), mean " + fixed(lsd_mean, 2) + " dB (limits " + fixed(kFpLsdMaxDb, 1) + " and " + fixed(kFpLsdMeanDb, 1) + ")\n";

  // the onsets
  if (want.onsets_ms != got.onsets_ms) {
    md += "- onsets (ms): " + list_of(want.onsets_ms) + " -> " + list_of(got.onsets_ms) + "\n";
  }
  if (want.onsets_ms.size() != got.onsets_ms.size()) {
    bad("the render has " + std::to_string(got.onsets_ms.size()) + " onsets, the stored fingerprint " + std::to_string(want.onsets_ms.size()));
  } else {
    for (std::size_t i = 0; i < got.onsets_ms.size(); ++i) {
      if (std::fabs(got.onsets_ms[i] - want.onsets_ms[i]) > kFpOnsetMs) {
        bad("onset " + std::to_string(i) + " moved from " + fixed(want.onsets_ms[i], 2) + " ms to " + fixed(got.onsets_ms[i], 2) + " ms, more than " + fixed(kFpOnsetMs, 1) + " ms");
      }
    }
  }

  // the rest of each frame
  std::string frame_rows;
  for (std::size_t f = 0; f < frames; ++f) {
    const FpFrame& a = want.frames[f];
    const FpFrame& b = got.frames[f];
    if (same_frame(a, b)) {
      continue;
    }
    std::string row = "| " + std::to_string(f) + " | " + span_ms(f) + " | " + fixed(lsd[f], 2) + " | ";
    // the bands that moved most
    std::array<unsigned, kFpBands> order{};
    for (unsigned i = 0; i < kFpBands; ++i) {
      order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](unsigned x, unsigned y) { return std::fabs(band_delta(a, b, x)) > std::fabs(band_delta(a, b, y)); });
    bool any = false;
    for (unsigned k = 0; k < 4u; ++k) {
      const unsigned band = order[k];
      if (std::fabs(band_delta(a, b, band)) >= 0.1) {
        row += (any ? "; " : "") + band_label(band) + ": " + fixed(a.bands[band], 1) + " -> " + fixed(b.bands[band], 1);
        any = true;
      }
    }
    row += (any ? "" : "-");
    row += " | ";
    std::string other;
    const auto note = [&](const char* what, const std::string& from, const std::string& to) { other += std::string(other.empty() ? "" : "; ") + what + " " + from + " -> " + to; };
    if (a.f0 != b.f0 && !got.nof0) {
      note("f0", fixed(a.f0, 2) + " Hz", fixed(b.f0, 2) + " Hz");
      if ((a.f0 > 0.0) != (b.f0 > 0.0)) {
        bad("frame " + std::to_string(f) + " (" + span_ms(f) + "): f0 " + (a.f0 > 0.0 ? "was " + fixed(a.f0, 2) + " Hz, now none" : "was none, now " + fixed(b.f0, 2) + " Hz"));
      } else if (a.f0 > 0.0 && 1200.0 * std::fabs(std::log2(b.f0 / a.f0)) > kFpF0Cents) {
        bad("frame " + std::to_string(f) + " (" + span_ms(f) + "): f0 moved from " + fixed(a.f0, 2) + " Hz to " + fixed(b.f0, 2) + " Hz (" + fixed(1200.0 * std::log2(b.f0 / a.f0), 1) + " cents), more than " + fixed(kFpF0Cents, 0));
      }
    }
    if (a.peak_db != b.peak_db) {
      note("peak", fixed(a.peak_db, 1), fixed(b.peak_db, 1) + " dB");
      if (std::fabs(b.peak_db - a.peak_db) > kFpLevelDb) {
        bad("frame " + std::to_string(f) + " (" + span_ms(f) + "): peak " + fixed(a.peak_db, 1) + " dB -> " + fixed(b.peak_db, 1) + " dB, more than " + fixed(kFpLevelDb, 0) + " dB");
      }
    }
    if (a.rms_db != b.rms_db) {
      note("rms", fixed(a.rms_db, 1), fixed(b.rms_db, 1) + " dB");
      if (std::fabs(b.rms_db - a.rms_db) > kFpLevelDb) {
        bad("frame " + std::to_string(f) + " (" + span_ms(f) + "): rms " + fixed(a.rms_db, 1) + " dB -> " + fixed(b.rms_db, 1) + " dB, more than " + fixed(kFpLevelDb, 0) + " dB");
      }
    }
    if (a.pan_db != b.pan_db) {
      note("balance", fixed(a.pan_db, 1), fixed(b.pan_db, 1) + " dB");
      if (std::fabs(b.pan_db - a.pan_db) > kFpLevelDb) {
        bad("frame " + std::to_string(f) + " (" + span_ms(f) + "): balance " + fixed(a.pan_db, 1) + " dB -> " + fixed(b.pan_db, 1) + " dB, more than " + fixed(kFpLevelDb, 0) + " dB");
      }
    }
    if (a.dc_lsb != b.dc_lsb) {
      note("dc", fixed(a.dc_lsb, 1), fixed(b.dc_lsb, 1) + " LSB");
      if (std::fabs(b.dc_lsb - a.dc_lsb) > kFpDcLsb) {
        bad("frame " + std::to_string(f) + " (" + span_ms(f) + "): dc " + fixed(a.dc_lsb, 1) + " -> " + fixed(b.dc_lsb, 1) + " LSB, more than " + fixed(kFpDcLsb, 0));
      }
    }
    if (a.clicks != b.clicks) {
      note("clicks", std::to_string(a.clicks), std::to_string(b.clicks));
      const unsigned diff = a.clicks > b.clicks ? a.clicks - b.clicks : b.clicks - a.clicks;
      if (diff > std::max(a.clicks, b.clicks) / 4u) {
        bad("frame " + std::to_string(f) + " (" + span_ms(f) + "): " + std::to_string(b.clicks) + " clicks, the stored fingerprint " + std::to_string(a.clicks));
      }
    }
    frame_rows += row + (other.empty() ? "-" : other) + " |\n";
  }
  fail.finish();
  out.markdown += "### " + name + ": " + (failed ? "CHANGED beyond the rules" : "changed within the rules") + "\n\n" + md + "\n";
  if (!frame_rows.empty()) {
    out.markdown += "| frame | time | distance dB | bands that moved most (dB re full scale) | other |\n|---|---|---|---|---|\n" + frame_rows + "\n";
  }
}

}  // namespace

void fp_band_edges(unsigned band, double& lo_hz, double& hi_hz) {
  const double ratio = std::log(kFpBandHiHz / kFpBandLoHz) / static_cast<double>(kFpBands);
  lo_hz = kFpBandLoHz * std::exp(ratio * static_cast<double>(band));
  hi_hz = kFpBandLoHz * std::exp(ratio * static_cast<double>(band + 1u));
}

FpRender fingerprint_render(const Pcm& pcm, bool nof0) {
  FpRender out;
  out.nof0 = nof0;
  const std::size_t n = pcm.frames();
  if (n == 0u || !(pcm.rate > 0.0)) {
    return out;
  }
  const Signal all = to_signal(pcm);
  const std::vector<double> mid = all.mid();
  const std::size_t len = static_cast<std::size_t>(std::llround(pcm.rate * kFpFrameMs / 1000.0));
  const std::size_t tail = static_cast<std::size_t>(std::llround(pcm.rate * kFpTailMs / 1000.0));
  // The levels and the spectrum of a frame are those of a Hann window of twice its length centred on it (hop = the frame), over the render with silence outside it: the edge of a note that
  // a change moves by a millisecond across a frame boundary then changes a frame by a hundredth of its energy, not from nothing to a note. `pad` frames of zeros at both ends make the window.
  const std::size_t pad = len / 2u;
  const std::size_t span = 2u * len;
  const auto padded = [&](const std::vector<double>& x) {
    std::vector<double> y(x.size() + 2u * pad, 0.0);
    std::copy(x.begin(), x.end(), y.begin() + static_cast<std::ptrdiff_t>(pad));
    return y;
  };
  const std::vector<double> mid_p = padded(mid), l_p = padded(all.l), r_p = padded(all.r);
  std::vector<double> window(span);
  double sum_w = 0.0, sum_w2 = 0.0;
  for (std::size_t i = 0; i < span; ++i) {
    window[i] = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * static_cast<double>(i) / static_cast<double>(span));
    sum_w += window[i];
    sum_w2 += window[i] * window[i];
  }
  for (std::size_t at = 0; at < n; at += len) {
    const std::size_t m = std::min(len, n - at);
    if (m < len && m < tail && at != 0u) {
      break;
    }
    FpFrame f;
    const std::vector<double> bands = band_levels_db(mid_p, at, span, pcm.rate, kFpBands, kFpBandLoHz, kFpBandHiHz, kFpBandFloorDb);
    for (unsigned b = 0; b < kFpBands; ++b) {
      f.bands[b] = rounded(bands[b], 10.0);
    }
    double peak = 0.0, wl = 0.0, wr = 0.0, el = 0.0, er = 0.0;
    for (std::size_t i = 0; i < span; ++i) {
      const double l = at + i < l_p.size() ? l_p[at + i] : 0.0;
      const double r = at + i < r_p.size() ? r_p[at + i] : 0.0;
      peak = std::max(peak, window[i] * std::max(std::fabs(l), std::fabs(r)));
      wl += window[i] * l;
      wr += window[i] * r;
      el += window[i] * window[i] * l * l;
      er += window[i] * window[i] * r * r;
    }
    const double rms = db_of(std::sqrt((el + er) / (2.0 * sum_w2)));
    f.peak_db = rounded(std::max(kFpLevelFloorDb, db_of(peak)), 10.0);
    f.rms_db = rounded(std::max(kFpLevelFloorDb, rms), 10.0);
    f.dc_lsb = rounded(std::max(std::fabs(wl), std::fabs(wr)) / sum_w / kLsb, 10.0);
    Signal frame;
    frame.rate = pcm.rate;
    frame.l.assign(all.l.begin() + static_cast<std::ptrdiff_t>(at), all.l.begin() + static_cast<std::ptrdiff_t>(at + m));
    frame.r.assign(all.r.begin() + static_cast<std::ptrdiff_t>(at), all.r.begin() + static_cast<std::ptrdiff_t>(at + m));
    f.clicks = count_clicks(frame);
    if (rms > kFpQuietDb) {
      f.pan_db = rounded(clamped(20.0 * std::log10(std::max(el, 1e-30) / std::max(er, 1e-30)) / 2.0, -kFpPanLimitDb, kFpPanLimitDb), 10.0);
      if (!nof0) {
        f.f0 = rounded(steady_f0(mid, at, m, pcm.rate), 100.0);
      }
    }
    out.frames.push_back(f);
  }
  out.onsets_ms = find_onsets(pcm);
  return out;
}

std::string fingerprint_json(const std::string& key, const FpSet& set) {
  std::string s = "{\n \"version\": " + std::to_string(kFpVersion) + ",\n \"key\": \"" + key + "\",\n \"frame_ms\": " + std::to_string(static_cast<int>(kFpFrameMs)) + ",\n \"bands\": " + std::to_string(kFpBands) + ",\n \"renders\": {\n";
  bool first = true;
  for (const auto& entry : set) {
    const FpRender& r = entry.second;
    s += first ? "" : ",\n";
    first = false;
    s += "  \"" + entry.first + "\": {\n   \"frames\": " + std::to_string(r.frames.size()) + ",\n   \"onsets_ms\": [";
    for (std::size_t i = 0; i < r.onsets_ms.size(); ++i) {
      s += i != 0u ? "," : "";
      put(s, 2, r.onsets_ms[i]);
    }
    s += "],\n";
    if (r.nof0) {
      s += "   \"nof0\": 1,\n";
    } else {
      put_array(s, "f0_hz", r.frames, 2, [](const FpFrame& f) { return f.f0; });
    }
    put_array(s, "peak_db", r.frames, 1, [](const FpFrame& f) { return f.peak_db; });
    put_array(s, "rms_db", r.frames, 1, [](const FpFrame& f) { return f.rms_db; });
    put_array(s, "pan_db", r.frames, 1, [](const FpFrame& f) { return f.pan_db; });
    put_array(s, "dc_lsb", r.frames, 1, [](const FpFrame& f) { return f.dc_lsb; });
    put_array(s, "clicks", r.frames, 0, [](const FpFrame& f) { return static_cast<double>(f.clicks); });
    s += "   \"bands_db\": [\n";
    for (std::size_t i = 0; i < r.frames.size(); ++i) {
      s += "    [";
      for (unsigned b = 0; b < kFpBands; ++b) {
        s += b != 0u ? "," : "";
        put(s, 1, r.frames[i].bands[b]);
      }
      s += i + 1u < r.frames.size() ? "],\n" : "]\n";
    }
    s += "   ]\n  }";
  }
  s += "\n }\n}\n";
  return s;
}

bool parse_fingerprint(const std::string& text, const std::string& key, FpSet& set, std::string& error) {
  Json root;
  if (!JsonReader(text).parse(root, error)) {
    return false;
  }
  const Json* version = root.t == Json::T::Obj ? member(root, "version") : nullptr;
  if (version == nullptr || version->t != Json::T::Num || static_cast<int>(version->num) != kFpVersion) {
    error = "the file is not a version " + std::to_string(kFpVersion) + " fingerprint: run update_audio_fingerprints";
    return false;
  }
  const Json* k = member(root, "key");
  if (k == nullptr || k->t != Json::T::Str || k->str != key) {
    error = "the file's key is not '" + key + "'";
    return false;
  }
  const Json* fm = member(root, "frame_ms");
  const Json* nb = member(root, "bands");
  if (fm == nullptr || nb == nullptr || fm->num != kFpFrameMs || nb->num != static_cast<double>(kFpBands)) {
    error = "the file's frame_ms or bands is not " + std::to_string(static_cast<int>(kFpFrameMs)) + " or " + std::to_string(kFpBands) + ": run update_audio_fingerprints";
    return false;
  }
  const Json* renders = member(root, "renders");
  if (renders == nullptr || renders->t != Json::T::Obj) {
    error = "'renders' is missing";
    return false;
  }
  set.clear();
  for (std::size_t r = 0; r < renders->keys.size(); ++r) {
    const Json& j = renders->arr[r];
    const std::string& name = renders->keys[r];
    const Json* count = j.t == Json::T::Obj ? member(j, "frames") : nullptr;
    if (count == nullptr || count->t != Json::T::Num || count->num < 0.0 || count->num > 100000.0) {
      error = "render '" + name + "': 'frames' is missing";
      return false;
    }
    const std::size_t frames = static_cast<std::size_t>(count->num);
    FpRender out;
    std::vector<double> f0, peak, rms, pan, dc, clicks;
    std::string why;
    const Json* nof0 = member(j, "nof0");
    out.nof0 = nof0 != nullptr && nof0->t == Json::T::Num && nof0->num != 0.0;
    if (out.nof0) {
      f0.assign(frames, 0.0);
    }
    if (!number_array(j, "onsets_ms", static_cast<std::size_t>(-1), out.onsets_ms, why) || (!out.nof0 && !number_array(j, "f0_hz", frames, f0, why)) || !number_array(j, "peak_db", frames, peak, why) ||
        !number_array(j, "rms_db", frames, rms, why) || !number_array(j, "pan_db", frames, pan, why) || !number_array(j, "dc_lsb", frames, dc, why) ||
        !number_array(j, "clicks", frames, clicks, why)) {
      error = "render '" + name + "': " + why;
      return false;
    }
    const Json* bands = member(j, "bands_db");
    if (bands == nullptr || bands->t != Json::T::Arr || bands->arr.size() != frames) {
      error = "render '" + name + "': 'bands_db' is not a list of " + std::to_string(frames) + " frames";
      return false;
    }
    out.frames.resize(frames);
    for (std::size_t f = 0; f < frames; ++f) {
      const Json& row = bands->arr[f];
      if (row.t != Json::T::Arr || row.arr.size() != kFpBands) {
        error = "render '" + name + "': frame " + std::to_string(f) + " of 'bands_db' is not " + std::to_string(kFpBands) + " numbers";
        return false;
      }
      for (unsigned b = 0; b < kFpBands; ++b) {
        if (row.arr[b].t != Json::T::Num) {
          error = "render '" + name + "': 'bands_db' holds something that is not a number";
          return false;
        }
        out.frames[f].bands[b] = row.arr[b].num;
      }
      out.frames[f].f0 = f0[f];
      out.frames[f].peak_db = peak[f];
      out.frames[f].rms_db = rms[f];
      out.frames[f].pan_db = pan[f];
      out.frames[f].dc_lsb = dc[f];
      out.frames[f].clicks = clicks[f] >= 0.0 && clicks[f] < 1e6 ? static_cast<unsigned>(clicks[f]) : 0u;
    }
    set[name] = out;
  }
  return true;
}

FpCompare compare_fingerprints(const std::string& key, const FpSet& want, const FpSet& got) {
  FpCompare out;
  out.markdown = "## " + key + "\n\n";
  for (const auto& g : got) {
    const auto w = want.find(g.first);
    if (w == want.end()) {
      out.identical = false;
      out.failures.push_back(g.first + ": no stored fingerprint for this render");
      out.markdown += "### " + g.first + ": NEW (" + std::to_string(g.second.frames.size()) + " frames, " + std::to_string(g.second.onsets_ms.size()) + " onsets), no stored fingerprint\n\n";
      continue;
    }
    compare_render(g.first, w->second, g.second, out);
  }
  for (const auto& w : want) {
    if (got.find(w.first) == got.end()) {
      out.identical = false;
      out.failures.push_back(w.first + ": the stored fingerprint has a render the scenario no longer fingerprints");
      out.markdown += "### " + w.first + ": REMOVED, the scenario no longer fingerprints it\n\n";
    }
  }
  return out;
}

}  // namespace o3os::sim
