// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/sim/audio_check.h>

#include <o3os/platform/host/sha256.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace o3os::sim {

namespace {

constexpr double kPi = 3.14159265358979323846;

// ---- a radix-2 FFT (the frames of the centroid, the zipper and the alias measure) --------------------------------------------------------

class Fft {
 public:
  explicit Fft(std::size_t n) : n_(n), cos_(n / 2u), sin_(n / 2u), rev_(n) {
    for (std::size_t i = 0; i < n / 2u; ++i) {
      const double a = -2.0 * kPi * static_cast<double>(i) / static_cast<double>(n);
      cos_[i] = std::cos(a);
      sin_[i] = std::sin(a);
    }
    unsigned bits = 0;
    while ((std::size_t{1} << bits) < n) {
      ++bits;
    }
    for (std::size_t i = 0; i < n; ++i) {
      std::size_t r = 0;
      for (unsigned b = 0; b < bits; ++b) {
        r |= ((i >> b) & 1u) << (bits - 1u - b);
      }
      rev_[i] = r;
    }
  }
  std::size_t size() const { return n_; }
  // In place, forward, unscaled.
  void forward(std::vector<double>& re, std::vector<double>& im) const {
    for (std::size_t i = 0; i < n_; ++i) {
      if (rev_[i] > i) {
        std::swap(re[i], re[rev_[i]]);
        std::swap(im[i], im[rev_[i]]);
      }
    }
    for (std::size_t len = 2; len <= n_; len <<= 1) {
      const std::size_t half = len / 2u;
      const std::size_t step = n_ / len;
      for (std::size_t base = 0; base < n_; base += len) {
        for (std::size_t k = 0; k < half; ++k) {
          const double wr = cos_[k * step];
          const double wi = sin_[k * step];
          const std::size_t a = base + k;
          const std::size_t b = a + half;
          const double tr = re[b] * wr - im[b] * wi;
          const double ti = re[b] * wi + im[b] * wr;
          re[b] = re[a] - tr;
          im[b] = im[a] - ti;
          re[a] += tr;
          im[a] += ti;
        }
      }
    }
  }

 private:
  std::size_t n_;
  std::vector<double> cos_;
  std::vector<double> sin_;
  std::vector<std::size_t> rev_;
};

std::vector<double> hann(std::size_t n) {
  std::vector<double> w(n);
  for (std::size_t i = 0; i < n; ++i) {
    w[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n));
  }
  return w;
}

// A 4-term Blackman-Harris window: -92 dB sidelobes, a main lobe of 4 bins either side (alias_db needs the dynamic range; the Hann skirt is only -77 dB beyond 8 bins).
std::vector<double> blackman_harris(std::size_t n) {
  std::vector<double> w(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double t = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n);
    w[i] = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2.0 * t) - 0.01168 * std::cos(3.0 * t);
  }
  return w;
}

// The magnitudes of bins 0..n/2 of a windowed frame of x starting at `at` (zero past the end).
void magnitudes(const std::vector<double>& x, std::size_t at, const Fft& fft, const std::vector<double>& window, std::vector<double>& re, std::vector<double>& im, std::vector<double>& mag) {
  const std::size_t n = fft.size();
  for (std::size_t i = 0; i < n; ++i) {
    re[i] = at + i < x.size() ? x[at + i] * window[i] : 0.0;
    im[i] = 0.0;
  }
  fft.forward(re, im);
  for (std::size_t k = 0; k <= n / 2u; ++k) {
    mag[k] = std::sqrt(re[k] * re[k] + im[k] * im[k]);
  }
}

double median_of(std::vector<double>& v) {
  if (v.empty()) {
    return 0.0;
  }
  const std::size_t mid = v.size() / 2u;
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
  return v[mid];
}

// The median and 1.4826 x the median absolute deviation of v[lo, hi).
void median_and_sigma(const std::vector<double>& v, std::size_t lo, std::size_t hi, double& median, double& sigma) {
  std::vector<double> w(v.begin() + static_cast<std::ptrdiff_t>(lo), v.begin() + static_cast<std::ptrdiff_t>(hi));
  median = median_of(w);
  for (double& e : w) {
    e = std::fabs(e - median);
  }
  sigma = 1.4826 * median_of(w);
}

// The 90th percentile of |v| over v[lo, hi): a spread that does not collapse when half the window is exactly zero (a linearly interpolated tone has d2 = 0 at every second sample, where the MAD is 0 or
// the smallest of the others by the parity of the window), and that ignores the few per cent of a window a click is. (The second difference of a signal has no offset to take out: no median.)
double spread90(const std::vector<double>& v, std::size_t lo, std::size_t hi) {
  std::vector<double> w(v.begin() + static_cast<std::ptrdiff_t>(lo), v.begin() + static_cast<std::ptrdiff_t>(hi));
  for (double& e : w) {
    e = std::fabs(e);
  }
  const std::size_t at = static_cast<std::size_t>(0.9 * static_cast<double>(w.size() - 1u));
  std::nth_element(w.begin(), w.begin() + static_cast<std::ptrdiff_t>(at), w.end());
  return w[at];
}

double rms_of(const std::vector<double>& x) {
  if (x.empty()) {
    return 0.0;
  }
  double sum = 0.0;
  for (const double v : x) {
    sum += v * v;
  }
  return std::sqrt(sum / static_cast<double>(x.size()));
}

double mean_of(const std::vector<double>& x) {
  if (x.empty()) {
    return 0.0;
  }
  double sum = 0.0;
  for (const double v : x) {
    sum += v;
  }
  return sum / static_cast<double>(x.size());
}

}  // namespace

// ---- the samples -------------------------------------------------------------------------------------------------------------------------

std::vector<double> Signal::mid() const {
  std::vector<double> m(l.size());
  for (std::size_t i = 0; i < l.size(); ++i) {
    m[i] = 0.5 * (l[i] + r[i]);
  }
  return m;
}

Signal to_signal(const Pcm& pcm, double from_ms, double to_ms) {
  Signal s;
  s.rate = pcm.rate;
  const std::size_t frames = pcm.frames();
  const auto at = [&](double ms) {
    const double f = std::floor(ms * pcm.rate / 1000.0 + 0.5);
    return f <= 0.0 ? std::size_t{0} : std::min(frames, static_cast<std::size_t>(f));
  };
  const std::size_t a = at(from_ms);
  const std::size_t b = to_ms < 0.0 ? frames : std::max(a, at(to_ms));
  s.l.reserve(b - a);
  s.r.reserve(b - a);
  for (std::size_t i = a; i < b; ++i) {
    s.l.push_back(static_cast<double>(pcm.lr[2u * i]) / 32768.0);
    s.r.push_back(static_cast<double>(pcm.lr[2u * i + 1u]) / 32768.0);
  }
  return s;
}

// ---- the measurements --------------------------------------------------------------------------------------------------------------------

double db_of(double amplitude) {
  if (!(amplitude > 1e-10)) {
    return kSilenceDb;
  }
  return std::max(kSilenceDb, 20.0 * std::log10(amplitude));
}

double rms_db(const Signal& s) {
  if (s.size() == 0u) {
    return kSilenceDb;
  }
  const double l = rms_of(s.l);
  const double r = rms_of(s.r);
  return db_of(std::sqrt(0.5 * (l * l + r * r)));
}

double peak_db(const Signal& s) {
  double peak = 0.0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    peak = std::max({peak, std::fabs(s.l[i]), std::fabs(s.r[i])});
  }
  return db_of(peak);
}

double dc_offset(const Signal& s) { return std::max(std::fabs(mean_of(s.l)), std::fabs(mean_of(s.r))); }

bool is_silent(const Signal& s) {
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (std::fabs(s.l[i]) > kLsb * 1.0001 || std::fabs(s.r[i]) > kLsb * 1.0001) {
      return false;
    }
  }
  return true;
}

// The second difference against its local spread (see audio_check.h). The spread is the 90th percentile of |d2| of the 129 samples around a hop of 32; a window that is mostly digital silence (the edge of a note) has none,
// so the limit at a hop is the largest spread of the windows 128 samples before, at and after it: the first ramp of a note is judged by the note it leads into, not by the silence before it.
unsigned count_clicks(const std::vector<double>& x) {
  constexpr std::size_t kHop = 32;
  constexpr std::size_t kReach = 64;
  constexpr std::size_t kNeighbour = 4;  // hops: 128 samples
  constexpr double kK = 13.0;
  constexpr double kFloor = 24.0 * kLsb;
  constexpr std::size_t kMerge = 4;
  const std::size_t n = x.size();
  if (n < 8u) {
    return 0;
  }
  std::vector<double> d2(n, 0.0);
  for (std::size_t i = 1; i + 1 < n; ++i) {
    d2[i] = x[i + 1] - 2.0 * x[i] + x[i - 1];
  }
  const std::size_t hops = (n + kHop - 1u) / kHop;
  std::vector<double> sigma(hops, 0.0);
  for (std::size_t h = 0; h < hops; ++h) {
    const std::size_t centre = h * kHop + kHop / 2u;
    const std::size_t lo = std::min(centre > kReach ? centre - kReach : 0u, n - 1u);
    const std::size_t hi = std::max(std::min(n, centre + kReach + 1u), lo + 1u);
    sigma[h] = spread90(d2, lo, hi);
  }
  unsigned clicks = 0;
  std::size_t last = 0;
  bool any = false;
  for (std::size_t h = 0; h < hops; ++h) {
    double spread = sigma[h];
    if (h >= kNeighbour) spread = std::max(spread, sigma[h - kNeighbour]);
    if (h + kNeighbour < hops) spread = std::max(spread, sigma[h + kNeighbour]);
    const double limit = std::max(kK * spread, kFloor);
    for (std::size_t i = std::max<std::size_t>(h * kHop, 1u); i < std::min(h * kHop + kHop, n - 1u); ++i) {
      if (std::fabs(d2[i]) > limit) {
        if (!any || i - last > kMerge) {
          ++clicks;
        }
        any = true;
        last = i;
      }
    }
  }
  return clicks;
}

unsigned count_clicks(const Signal& s) { return count_clicks(s.l) + count_clicks(s.r); }

double onset_ms(const Signal& s, double floor_db) {
  const double floor = std::pow(10.0, floor_db / 20.0);
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (std::fabs(s.l[i]) >= floor || std::fabs(s.r[i]) >= floor) {
      return 1000.0 * static_cast<double>(i) / s.rate;
    }
  }
  return -1.0;
}

// YIN (de Cheveigne and Kawahara, 2002) on the loudest stable part.
double pitch_hz(const std::vector<double>& x, double rate) {
  const std::size_t frame = static_cast<std::size_t>(rate * 0.010);
  if (frame < 8u || x.size() < 4u * frame) {
    return 0.0;
  }
  const std::size_t frames = x.size() / frame;
  std::vector<double> level(frames);
  double loudest = 0.0;
  for (std::size_t f = 0; f < frames; ++f) {
    double sum = 0.0;
    for (std::size_t i = f * frame; i < (f + 1u) * frame; ++i) {
      sum += x[i] * x[i];
    }
    level[f] = std::sqrt(sum / static_cast<double>(frame));
    loudest = std::max(loudest, level[f]);
  }
  if (loudest < kLsb) {
    return 0.0;
  }
  // The longest run of frames within 6 dB of the loudest.
  std::size_t best_start = 0;
  std::size_t best_len = 0;
  for (std::size_t f = 0; f < frames;) {
    if (level[f] < 0.5 * loudest) {
      ++f;
      continue;
    }
    std::size_t g = f;
    while (g < frames && level[g] >= 0.5 * loudest) {
      ++g;
    }
    if (g - f > best_len) {
      best_start = f;
      best_len = g - f;
    }
    f = g;
  }
  constexpr std::size_t kMaxSegment = 8192;
  const std::size_t run = best_len * frame;
  const std::size_t seg = std::min(run, kMaxSegment);
  const std::size_t tau_max = std::min(static_cast<std::size_t>(std::ceil(rate / 50.0)), seg / 2u);
  const std::size_t tau_min = 8;  // 5.5 kHz: above it the period is too few samples for the difference function (it finds a sub-multiple)
  if (seg < 2u * tau_min + 16u || tau_max <= tau_min + 2u) {
    return 0.0;
  }
  const std::size_t begin = best_start * frame + (run - seg) / 2u;  // the middle of the run
  const std::size_t w = seg - tau_max;
  std::vector<double> d(tau_max + 1u, 0.0);
  for (std::size_t tau = 1; tau <= tau_max; ++tau) {
    double sum = 0.0;
    for (std::size_t j = 0; j < w; ++j) {
      const double e = x[begin + j] - x[begin + j + tau];
      sum += e * e;
    }
    d[tau] = sum;
  }
  std::vector<double> dn(tau_max + 1u, 1.0);
  double running = 0.0;
  for (std::size_t tau = 1; tau <= tau_max; ++tau) {
    running += d[tau];
    dn[tau] = running > 0.0 ? d[tau] * static_cast<double>(tau) / running : 1.0;
  }
  std::size_t pick = 0;
  for (std::size_t tau = tau_min; tau < tau_max; ++tau) {
    if (dn[tau] < 0.1) {
      while (tau + 1u < tau_max && dn[tau + 1u] < dn[tau]) {
        ++tau;
      }
      pick = tau;
      break;
    }
  }
  if (pick == 0u) {
    return 0.0;
  }
  // A parabola through the minimum and its neighbours: good to a few cents; the spectrum below refines it.
  double tau = static_cast<double>(pick);
  if (pick > 1u && pick + 1u <= tau_max) {
    const double a = dn[pick - 1u];
    const double b = dn[pick];
    const double c = dn[pick + 1u];
    const double den = a - 2.0 * b + c;
    if (den > 1e-12) {
      tau += 0.5 * (a - c) / den;
    }
  }
  const double coarse = rate / tau;
  // The Hann-windowed spectrum of the same part, zero-padded 8 times: the strongest bin within 3 % of the coarse value, and a parabola through the logs of it and its neighbours.
  std::size_t nfft = 1;
  while (nfft < 8u * seg) {
    nfft <<= 1;
  }
  const Fft fft(nfft);
  const std::vector<double> window = hann(seg);
  std::vector<double> re(nfft, 0.0), im(nfft, 0.0);
  for (std::size_t i = 0; i < seg; ++i) {
    re[i] = x[begin + i] * window[i];
  }
  fft.forward(re, im);
  const double bin_hz = rate / static_cast<double>(nfft);
  const std::size_t lo = std::max<std::size_t>(2u, static_cast<std::size_t>(coarse * 0.97 / bin_hz));
  const std::size_t hi = std::min(nfft / 2u - 2u, static_cast<std::size_t>(coarse * 1.03 / bin_hz) + 1u);
  std::size_t top = 0;
  double top_mag = 0.0;
  for (std::size_t k = lo; k <= hi; ++k) {
    const double m = re[k] * re[k] + im[k] * im[k];
    if (m > top_mag) {
      top_mag = m;
      top = k;
    }
  }
  if (top <= lo || top >= hi) {
    return coarse;  // the peak is at the edge of the range (or nothing is there): the time-domain value stands
  }
  const auto lm = [&](std::size_t k) { return std::log(re[k] * re[k] + im[k] * im[k] + 1e-300); };
  const double a = lm(top - 1u);
  const double b = lm(top);
  const double c = lm(top + 1u);
  const double den = a - 2.0 * b + c;
  const double delta = den < -1e-12 ? 0.5 * (a - c) / den : 0.0;
  return (static_cast<double>(top) + delta) * bin_hz;
}

std::vector<double> band_levels_db(const std::vector<double>& x, std::size_t from, std::size_t len, double rate, unsigned bands, double lo_hz, double hi_hz, double floor_db) {
  std::vector<double> out(bands, floor_db);
  if (from >= x.size() || len == 0u || bands == 0u || !(lo_hz > 0.0) || !(hi_hz > lo_hz)) {
    return out;
  }
  len = std::min(len, x.size() - from);
  std::size_t nfft = 8192;
  while (nfft < len) {
    nfft <<= 1;
  }
  const std::vector<double> window = hann(len);
  double window_power = 0.0;
  for (const double w : window) {
    window_power += w * w;
  }
  const Fft fft(nfft);
  std::vector<double> re(nfft, 0.0), im(nfft, 0.0);
  for (std::size_t i = 0; i < len; ++i) {
    re[i] = x[from + i] * window[i];
  }
  fft.forward(re, im);
  // Parseval: the mean square of the signal is sum |X|^2 / (nfft * sum w^2) over all bins, twice the positive half's for a real signal.
  const double scale = 2.0 / (static_cast<double>(nfft) * window_power);
  const double ratio = std::log(hi_hz / lo_hz) / static_cast<double>(bands);
  std::vector<double> power(bands, 0.0);
  for (std::size_t k = 1; k < nfft / 2u; ++k) {
    const double f = static_cast<double>(k) * rate / static_cast<double>(nfft);
    if (f < lo_hz || f >= hi_hz) {
      continue;
    }
    const double b = std::floor(std::log(f / lo_hz) / ratio);
    const std::size_t band = std::min<std::size_t>(bands - 1u, static_cast<std::size_t>(b < 0.0 ? 0.0 : b));
    power[band] += (re[k] * re[k] + im[k] * im[k]) * scale;
  }
  for (unsigned b = 0; b < bands; ++b) {
    out[b] = power[b] > 0.0 ? std::max(floor_db, 10.0 * std::log10(power[b])) : floor_db;
  }
  return out;
}

double centroid_hz(const std::vector<double>& x, double rate) {
  constexpr std::size_t kN = 2048;
  constexpr std::size_t kHopN = 1024;
  if (x.empty()) {
    return 0.0;
  }
  const Fft fft(kN);
  const std::vector<double> window = hann(kN);
  std::vector<double> re(kN), im(kN), mag(kN / 2u + 1u);
  // The frames, and the loudest one's energy.
  std::vector<std::size_t> at;
  std::vector<double> energy;
  double loudest = 0.0;
  for (std::size_t p = 0; p == 0u || p + kN <= x.size(); p += kHopN) {
    double e = 0.0;
    for (std::size_t i = p; i < std::min(x.size(), p + kN); ++i) {
      e += x[i] * x[i];
    }
    at.push_back(p);
    energy.push_back(e);
    loudest = std::max(loudest, e);
  }
  if (loudest <= 0.0) {
    return 0.0;
  }
  std::vector<double> sum(kN / 2u + 1u, 0.0);
  for (std::size_t f = 0; f < at.size(); ++f) {
    if (energy[f] < 0.01 * loudest) {
      continue;
    }
    magnitudes(x, at[f], fft, window, re, im, mag);
    for (std::size_t k = 0; k < sum.size(); ++k) {
      sum[k] += mag[k];
    }
  }
  double num = 0.0;
  double den = 0.0;
  for (std::size_t k = 1; k < sum.size(); ++k) {
    num += static_cast<double>(k) * rate / static_cast<double>(kN) * sum[k];
    den += sum[k];
  }
  return den > 0.0 ? num / den : 0.0;
}

double pan_balance_db(const Signal& s) {
  const double l = db_of(rms_of(s.l));
  const double r = db_of(rms_of(s.r));
  return std::max(-kPanLimitDb, std::min(kPanLimitDb, l - r));
}

// Spectral flux spikes (see audio_check.h).
unsigned count_zipper(const std::vector<double>& x, double rate) {
  (void)rate;
  constexpr std::size_t kN = kZipperFrame;
  constexpr std::size_t kHopN = kZipperHop;
  constexpr std::size_t kReach = 24;
  constexpr double kK = 8.0;
  constexpr double kFloor = 0.05;
  constexpr std::size_t kMerge = 8;
  if (x.size() < kN + 2u * kHopN) {
    return 0;
  }
  const Fft fft(kN);
  const std::vector<double> window = hann(kN);
  std::vector<double> re(kN), im(kN), mag(kN / 2u + 1u), previous(kN / 2u + 1u, 0.0);
  std::vector<double> flux;
  bool first = true;
  for (std::size_t p = 0; p + kN <= x.size(); p += kHopN) {
    magnitudes(x, p, fft, window, re, im, mag);
    if (!first) {
      double up = 0.0;
      double total = 0.0;
      for (std::size_t k = 1; k < mag.size(); ++k) {
        up += std::max(0.0, mag[k] - previous[k]);
        total += previous[k];
      }
      flux.push_back(total > 1e-9 ? up / total : 0.0);
    }
    previous = mag;
    first = false;
  }
  unsigned spikes = 0;
  std::size_t last = 0;
  bool any = false;
  for (std::size_t i = 0; i < flux.size(); ++i) {
    const std::size_t lo = i > kReach ? i - kReach : 0u;
    const std::size_t hi = std::min(flux.size(), i + kReach + 1u);
    double median = 0.0;
    double sigma = 0.0;
    median_and_sigma(flux, lo, hi, median, sigma);
    if (flux[i] > kFloor && flux[i] > median + kK * sigma) {
      if (!any || i - last > kMerge) {
        ++spikes;
      }
      any = true;
      last = i;
    }
  }
  return spikes;
}

double alias_db(const std::vector<double>& x, double rate) {
  constexpr std::size_t kN = kAliasFrame;
  constexpr std::size_t kNear = 6;
  (void)rate;
  if (x.size() < kN) {
    return std::numeric_limits<double>::quiet_NaN();  // too short to measure
  }
  const Fft fft(kN);
  const std::vector<double> window = blackman_harris(kN);
  std::vector<double> re(kN), im(kN), mag(kN / 2u + 1u);
  magnitudes(x, (x.size() - kN) / 2u, fft, window, re, im, mag);  // the middle of the signal
  std::size_t peak = 4;
  for (std::size_t k = 4; k < mag.size(); ++k) {
    if (mag[k] > mag[peak]) {
      peak = k;
    }
  }
  if (mag[peak] <= 0.0) {
    return std::numeric_limits<double>::quiet_NaN();  // silence: no tone
  }
  std::vector<bool> signal(mag.size(), false);
  for (std::size_t k = 0; k < 4u && k < signal.size(); ++k) {
    signal[k] = true;  // DC
  }
  for (std::size_t h = 1; h * peak < mag.size() + kNear; ++h) {
    const std::size_t centre = h * peak;
    for (std::size_t k = centre > kNear ? centre - kNear : 0u; k <= centre + kNear && k < mag.size(); ++k) {
      signal[k] = true;
    }
  }
  double total = 0.0;
  double outside = 0.0;
  for (std::size_t k = 0; k < mag.size(); ++k) {
    const double e = mag[k] * mag[k];
    total += e;
    if (!signal[k]) {
      outside += e;
    }
  }
  if (total <= 0.0 || outside <= 0.0) {
    return kSilenceDb;
  }
  return std::max(kSilenceDb, 10.0 * std::log10(outside / total));
}

double null_db(const Signal& a, const Signal& b, std::size_t lag) {
  const std::size_t n = std::min(a.size() > lag ? a.size() - lag : std::size_t{0}, b.size());
  double ea = 0.0;
  double eb = 0.0;
  double er = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double al = a.l[i + lag];
    const double ar = a.r[i + lag];
    ea += al * al + ar * ar;
    eb += b.l[i] * b.l[i] + b.r[i] * b.r[i];
    const double dl = al - b.l[i];
    const double dr = ar - b.r[i];
    er += dl * dl + dr * dr;
  }
  if (n == 0u || (ea <= 0.0 && eb <= 0.0)) {
    return std::numeric_limits<double>::quiet_NaN();  // nothing was compared, or silence against silence: a null that proves nothing
  }
  if (er <= 0.0) {
    return kSilenceDb;
  }
  if (ea <= 0.0) {
    return -kSilenceDb;
  }
  return std::max(kSilenceDb, 10.0 * std::log10(er / ea));
}

Signal sum_signals(const std::vector<Signal>& parts) {
  Signal out;
  if (parts.empty()) {
    return out;
  }
  out.rate = parts[0].rate;
  std::size_t n = parts[0].size();
  for (const Signal& p : parts) {
    n = std::min(n, p.size());
  }
  out.l.assign(n, 0.0);
  out.r.assign(n, 0.0);
  for (const Signal& p : parts) {
    for (std::size_t i = 0; i < n; ++i) {
      out.l[i] += p.l[i];
      out.r[i] += p.r[i];
    }
  }
  return out;
}

double adsr_level(const Adsr& m, double t) {
  if (t < 0.0) {
    return 0.0;
  }
  const auto held = [&](double at) {  // the level before the release, `at` ms after the onset
    if (at < m.attack_ms) {
      return m.attack_ms > 0.0 ? at / m.attack_ms : 1.0;
    }
    at -= m.attack_ms;
    if (at < m.decay_ms) {
      return 1.0 + (m.sustain - 1.0) * (m.decay_ms > 0.0 ? at / m.decay_ms : 1.0);
    }
    return m.sustain;
  };
  if (t < m.off_ms) {
    return held(t);
  }
  const double from = held(m.off_ms);
  if (m.release_ms <= 0.0 || t >= m.off_ms + m.release_ms) {
    return 0.0;
  }
  return from * (1.0 - (t - m.off_ms) / m.release_ms);
}

// The largest value of the (piecewise linear) model on [a, b] ms after the onset: at an end or at one of its knees.
static double adsr_peak_between(const Adsr& m, double a, double b) {
  double top = std::max(adsr_level(m, a), adsr_level(m, b));
  for (const double knee : {m.attack_ms, m.attack_ms + m.decay_ms, m.off_ms, m.off_ms + m.release_ms}) {
    if (knee > a && knee < b) {
      top = std::max(top, adsr_level(m, knee));
    }
  }
  return top;
}

double envelope_error(const Signal& s, const Adsr& model) {
  const double start = onset_ms(s);
  if (start < 0.0) {
    return -1.0;
  }
  const std::size_t window = std::max<std::size_t>(1u, static_cast<std::size_t>(s.rate * 0.003));
  const std::size_t step = std::max<std::size_t>(1u, static_cast<std::size_t>(s.rate * 0.001));
  std::vector<double> env;
  std::vector<double> begin_ms;
  double top = 0.0;
  for (std::size_t p = 0; p + window <= s.size(); p += step) {
    double peak = 0.0;
    for (std::size_t i = p; i < p + window; ++i) {
      peak = std::max({peak, std::fabs(s.l[i]), std::fabs(s.r[i])});
    }
    env.push_back(peak);
    begin_ms.push_back(1000.0 * static_cast<double>(p) / s.rate);
    top = std::max(top, peak);
  }
  if (top <= 0.0) {
    return -1.0;
  }
  // A window's peak is the largest of the sound in it: the model is read the same way (its largest value over the window), so that a steep ramp is not a difference of its own.
  const double window_ms = 1000.0 * static_cast<double>(window) / s.rate;
  double worst = 0.0;
  for (std::size_t i = 0; i < env.size(); ++i) {
    const double t = begin_ms[i] - start;
    if (t < 0.0) {
      continue;  // (a window that begins before the onset holds the silence and the start of the ramp: the model starts at the onset, which the -50 dB threshold finds to within 0.15 ms)
    }
    worst = std::max(worst, std::fabs(env[i] / top - adsr_peak_between(model, t, t + window_ms)));
  }
  return worst;
}

// ---- files -------------------------------------------------------------------------------------------------------------------------------

std::vector<std::uint8_t> wav_bytes(const Pcm& pcm) {
  const std::uint32_t data = static_cast<std::uint32_t>(pcm.lr.size() * 2u);
  const std::uint32_t rate = static_cast<std::uint32_t>(pcm.rate + 0.5);
  std::vector<std::uint8_t> out;
  out.reserve(44u + data);
  const auto put32 = [&out](std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) {
      out.push_back(static_cast<std::uint8_t>(v >> (8u * i)));
    }
  };
  const auto put16 = [&out](std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
  };
  const auto tag = [&out](const char* t) {
    for (unsigned i = 0; i < 4; ++i) {
      out.push_back(static_cast<std::uint8_t>(t[i]));
    }
  };
  tag("RIFF");
  put32(36u + data);
  tag("WAVE");
  tag("fmt ");
  put32(16);
  put16(1);  // PCM
  put16(2);  // channels
  put32(rate);
  put32(rate * 4u);
  put16(4);
  put16(16);
  tag("data");
  put32(data);
  for (const std::int16_t v : pcm.lr) {
    put16(static_cast<std::uint16_t>(v));
  }
  return out;
}

std::string pcm_sha256(const Pcm& pcm) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(pcm.lr.size() * 2u);
  for (const std::int16_t v : pcm.lr) {
    bytes.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(v)));
    bytes.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(v) >> 8));
  }
  return platform::host::sha256_hex(bytes.data(), bytes.size());
}

// ---- the grammar -------------------------------------------------------------------------------------------------------------------------

namespace {

std::string lower_of(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

bool parse_double(const std::string& s, double& out) {
  if (s.empty()) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  const double v = std::strtod(s.c_str(), &end);
  if (errno != 0 || *end != '\0' || !std::isfinite(v)) {
    return false;
  }
  out = v;
  return true;
}

// "x±t" or "x+-t": the position of the sign and its width; npos when there is none. The first character may be a sign of the number.
bool split_tolerance(const std::string& s, std::string& value, std::string& tol) {
  std::size_t at = s.find("\xC2\xB1");
  std::size_t width = 2;
  if (at == std::string::npos) {
    at = s.find("+-", 1);
    width = 2;
  }
  if (at == std::string::npos) {
    return false;
  }
  value = s.substr(0, at);
  tol = s.substr(at + width);
  return true;
}

// A value: a number, or @ref with an optional +n, -n or *n. false: `error` says why.
bool parse_value(const std::string& s, AudioCheck& c, std::string& error) {
  if (!s.empty() && s[0] == '@') {
    std::size_t end = s.find_first_of("+-*", 1);
    c.ref = s.substr(1, end == std::string::npos ? std::string::npos : end - 1u);
    if (c.ref.empty()) {
      error = "'" + s + "': @ needs the name of a render";
      return false;
    }
    if (end != std::string::npos) {
      c.op = s[end];
      if (!parse_double(s.substr(end + 1u), c.operand)) {
        error = "'" + s + "': after " + std::string(1, c.op) + " comes a number";
        return false;
      }
    }
    return true;
  }
  if (!parse_double(s, c.value)) {
    error = "'" + s + "' is not a number";
    return false;
  }
  return true;
}

// pitch value: a number of Hz, m<note>, or @ref.
bool parse_pitch_value(const std::string& s, AudioCheck& c, std::string& error) {
  if (s.size() > 1u && (s[0] == 'm' || s[0] == 'M')) {
    double note = 0.0;
    if (!parse_double(s.substr(1), note) || note < 0.0 || note > 127.0) {
      error = "'" + s + "': m<note> wants a MIDI note 0..127";
      return false;
    }
    c.value = 440.0 * std::pow(2.0, (note - 69.0) / 12.0);
    return true;
  }
  return parse_value(s, c, error);
}

bool parse_adsr(const std::string& s, Adsr& m, std::string& error) {
  const std::string head = "adsr(";
  if (s.compare(0, head.size(), head) != 0 || s.empty() || s.back() != ')') {
    error = "envelope wants adsr(a=<ms>,d=<ms>,s=<0..1>,r=<ms>,off=<ms>)";
    return false;
  }
  std::string body = s.substr(head.size(), s.size() - head.size() - 1u);
  bool have_off = false;
  std::size_t p = 0;
  while (p <= body.size()) {
    std::size_t comma = body.find(',', p);
    if (comma == std::string::npos) {
      comma = body.size();
    }
    const std::string item = body.substr(p, comma - p);
    p = comma + 1u;
    const std::size_t eq = item.find('=');
    double v = 0.0;
    if (eq == std::string::npos || !parse_double(item.substr(eq + 1u), v) || v < 0.0) {
      error = "envelope: '" + item + "' is not <key>=<number>";
      return false;
    }
    const std::string key = lower_of(item.substr(0, eq));
    if (key == "a") {
      m.attack_ms = v;
    } else if (key == "d") {
      m.decay_ms = v;
    } else if (key == "s") {
      if (v > 1.0) {
        error = "envelope: s is a level 0..1";
        return false;
      }
      m.sustain = v;
    } else if (key == "r") {
      m.release_ms = v;
    } else if (key == "off") {
      m.off_ms = v;
      have_off = true;
    } else {
      error = "envelope: unknown key '" + key + "' (a, d, s, r, off)";
      return false;
    }
  }
  if (!have_off) {
    error = "envelope: off=<ms> (when the note is released, from its onset) is needed";
    return false;
  }
  return true;
}

}  // namespace

bool parse_audio_assert(const std::vector<std::string>& words, AudioAssert& out, std::string& error) {
  out = AudioAssert{};
  if (words.empty()) {
    error = "assert audio needs the name of a render and a check";
    return false;
  }
  out.name = words[0];
  for (std::size_t i = 1; i < words.size(); ++i) {
    const std::string& w = words[i];
    const std::string lw = lower_of(w);
    AudioCheck c;
    c.text = w;
    std::string value, tol;
    if (lw.compare(0, 3, "in=") == 0) {
      const std::string span = w.substr(3);
      const std::size_t dash = span.find('-');
      double a = 0.0;
      double b = -1.0;
      if (dash == std::string::npos || !parse_double(span.substr(0, dash), a) || a < 0.0 || (lower_of(span.substr(dash + 1u)) != "end" && (!parse_double(span.substr(dash + 1u), b) || b <= a))) {
        error = "'" + w + "': in= wants <from>-<to> in ms, 0 <= from < to, or <from>-end";
        return false;
      }
      out.from_ms = a;
      out.to_ms = b;
      continue;
    }
    if (lw == "silent" || lw == "sound") {
      c.kind = lw == "silent" ? AudioCheck::Kind::Silent : AudioCheck::Kind::Sound;
      out.checks.push_back(c);
      continue;
    }
    std::size_t op_at = w.find_first_of("=<>");
    if (op_at == std::string::npos || op_at == 0u || op_at + 1u >= w.size()) {
      error = "'" + w + "' is no check (pitch= rms= peak= dc< silent sound clicks= onset= envelope= centroid< pan= zipper= alias< null=, or in=)";
      return false;
    }
    const std::string key = lower_of(w.substr(0, op_at));
    const char sign = w[op_at];
    const std::string rest = w.substr(op_at + 1u);
    c.cmp = sign == '<' ? AudioCheck::Cmp::Less : (sign == '>' ? AudioCheck::Cmp::Greater : AudioCheck::Cmp::Near);
    const bool tolerance = split_tolerance(rest, value, tol);
    if (!tolerance) {
      value = rest;
    }
    const auto need_tol = [&](const char* what) {
      if (!tolerance || !parse_double(tol, c.tol) || c.tol < 0.0) {
        error = "'" + w + "': " + what + " needs a tolerance, x+-n (or x\xC2\xB1n)";
        return false;
      }
      return true;
    };
    if (key == "pitch") {
      c.kind = AudioCheck::Kind::Pitch;
      if (c.cmp != AudioCheck::Cmp::Near || !tolerance) {
        error = "'" + w + "': pitch=<Hz>+-<n>c";
        return false;
      }
      std::string cents = tol;
      if (cents.empty() || (cents.back() != 'c' && cents.back() != 'C')) {
        error = "'" + w + "': the pitch tolerance is in cents: +-<n>c";
        return false;
      }
      cents.pop_back();
      if (!parse_double(cents, c.tol) || c.tol < 0.0 || !parse_pitch_value(value, c, error)) {
        if (error.empty()) {
          error = "'" + w + "': '" + tol + "' is not a number of cents";
        }
        return false;
      }
    } else if (key == "rms" || key == "peak" || key == "centroid" || key == "dc") {
      c.kind = key == "rms" ? AudioCheck::Kind::Rms : (key == "peak" ? AudioCheck::Kind::Peak : (key == "dc" ? AudioCheck::Kind::Dc : AudioCheck::Kind::Centroid));
      if (c.cmp == AudioCheck::Cmp::Near && !need_tol(key.c_str())) {
        return false;
      }
      if (c.cmp != AudioCheck::Cmp::Near && tolerance) {
        error = "'" + w + "': < and > take no tolerance";
        return false;
      }
      if (!parse_value(value, c, error)) {
        return false;
      }
    } else if (key == "alias") {
      c.kind = AudioCheck::Kind::Alias;
      if (c.cmp == AudioCheck::Cmp::Near || tolerance || !parse_value(value, c, error)) {
        if (error.empty()) {
          error = "'" + w + "': alias<dB or alias>dB";
        }
        return false;
      }
    } else if (key == "clicks" || key == "zipper") {
      c.kind = key == "clicks" ? AudioCheck::Kind::Clicks : AudioCheck::Kind::Zipper;
      double n = 0.0;
      if (c.cmp == AudioCheck::Cmp::Greater || tolerance || !parse_double(value, n) || n < 0.0 || n != std::floor(n) || (key == "zipper" && c.cmp != AudioCheck::Cmp::Near)) {
        error = "'" + w + "': " + key + (key == "clicks" ? "=<count> or clicks<<count>" : "=<count>");
        return false;
      }
      c.value = n;
    } else if (key == "onset") {
      c.kind = AudioCheck::Kind::Onset;
      if (c.cmp != AudioCheck::Cmp::Near || !need_tol("onset") || !parse_value(value, c, error)) {
        if (error.empty()) {
          error = "'" + w + "': onset=<ms>+-<d>";
        }
        return false;
      }
    } else if (key == "envelope") {
      c.kind = AudioCheck::Kind::Envelope;
      if (c.cmp != AudioCheck::Cmp::Near || !need_tol("envelope") || !parse_adsr(value, c.model, error)) {
        if (error.empty()) {
          error = "'" + w + "': envelope=adsr(...)+-<e>";
        }
        return false;
      }
    } else if (key == "pan") {
      c.kind = AudioCheck::Kind::Pan;
      const std::string lv = lower_of(value);
      if (c.cmp != AudioCheck::Cmp::Near) {
        error = "'" + w + "': pan=L, pan=R, pan=C+-<d> or pan=<dB>+-<d>";
        return false;
      }
      if (lv == "l" || lv == "r") {
        if (tolerance) {
          error = "'" + w + "': pan=L and pan=R take no tolerance";
          return false;
        }
        c.pan = lv == "l" ? AudioCheck::PanKind::Left : AudioCheck::PanKind::Right;
      } else if (lv == "c") {
        c.pan = AudioCheck::PanKind::Centre;
        if (!need_tol("pan=C")) {
          return false;
        }
      } else {
        c.pan = AudioCheck::PanKind::Db;
        if (!tolerance || !parse_double(tol, c.tol) || c.tol < 0.0 || !parse_double(value, c.value)) {
          error = "'" + w + "': pan=L, pan=R, pan=C+-<d> or pan=<dB>+-<d>";
          return false;
        }
      }
    } else if (key == "null") {
      c.kind = AudioCheck::Kind::Null;
      // <other>[+<other>...],<dB>[,lag=<n>]
      std::vector<std::string> parts;
      for (std::size_t from = 0;;) {
        const std::size_t comma = rest.find(',', from);
        parts.push_back(rest.substr(from, comma == std::string::npos ? std::string::npos : comma - from));
        if (comma == std::string::npos) {
          break;
        }
        from = comma + 1u;
      }
      double lag = 0.0;
      if (c.cmp != AudioCheck::Cmp::Near || parts.size() < 2u || parts.size() > 3u || parts[0].empty() || !parse_double(parts[1], c.value) ||
          (parts.size() == 3u && (lower_of(parts[2]).compare(0, 4, "lag=") != 0 || !parse_double(parts[2].substr(4), lag) || lag < 0.0 || lag > 4096.0 || lag != std::floor(lag)))) {
        error = "'" + w + "': null=<other render>[+<other render>...],<dB>[,lag=<frames 0..4096>]";
        return false;
      }
      c.lag = static_cast<std::uint32_t>(lag);
      for (std::size_t from = 0;;) {
        const std::size_t plus = parts[0].find('+', from);
        c.others.push_back(parts[0].substr(from, plus == std::string::npos ? std::string::npos : plus - from));
        if (c.others.back().empty()) {
          error = "'" + w + "': an empty render name in the sum";
          return false;
        }
        if (plus == std::string::npos) {
          break;
        }
        from = plus + 1u;
      }
    } else {
      error = "'" + w + "': unknown check '" + key + "' (pitch rms peak dc silent sound clicks onset envelope centroid pan zipper alias null)";
      return false;
    }
    out.checks.push_back(c);
  }
  if (out.checks.empty()) {
    error = "assert audio " + out.name + ": no check";
    return false;
  }
  return true;
}

// ---- evaluation --------------------------------------------------------------------------------------------------------------------------

namespace {

std::string num(double v) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.4g", v);
  return buf;
}

// One measurement of `kind` on a signal (the value a reference takes too).
bool measure(AudioCheck::Kind kind, const Signal& s, double& out) {
  switch (kind) {
    case AudioCheck::Kind::Rms: out = rms_db(s); return true;
    case AudioCheck::Kind::Peak: out = peak_db(s); return true;
    case AudioCheck::Kind::Dc: out = dc_offset(s); return true;
    case AudioCheck::Kind::Centroid: out = centroid_hz(s.mid(), s.rate); return out > 0.0;  // 0: silence
    case AudioCheck::Kind::Alias: out = alias_db(s.mid(), s.rate); return !std::isnan(out);
    case AudioCheck::Kind::Onset: out = onset_ms(s); return out >= 0.0;
    case AudioCheck::Kind::Pitch: out = pitch_hz(s.mid(), s.rate); return out > 0.0;
    default: return false;
  }
}

const char* name_of(AudioCheck::Kind k) {
  switch (k) {
    case AudioCheck::Kind::Pitch: return "pitch";
    case AudioCheck::Kind::Rms: return "rms";
    case AudioCheck::Kind::Peak: return "peak";
    case AudioCheck::Kind::Dc: return "dc";
    case AudioCheck::Kind::Silent: return "silent";
    case AudioCheck::Kind::Sound: return "sound";
    case AudioCheck::Kind::Clicks: return "clicks";
    case AudioCheck::Kind::Onset: return "onset";
    case AudioCheck::Kind::Envelope: return "envelope";
    case AudioCheck::Kind::Centroid: return "centroid";
    case AudioCheck::Kind::Pan: return "pan";
    case AudioCheck::Kind::Zipper: return "zipper";
    case AudioCheck::Kind::Alias: return "alias";
    case AudioCheck::Kind::Null: return "null";
  }
  return "?";
}

}  // namespace

std::string audio_problem(const AudioAssert& a, const PcmLookup& lookup, std::string* summary) {
  const Pcm* pcm = lookup(a.name);
  if (pcm == nullptr) {
    return "there is no finished render '" + a.name + "' (`render <name> <ms>`, then the time to run it)";
  }
  const Signal s = to_signal(*pcm, a.from_ms, a.to_ms);
  if (s.size() == 0u) {
    return "the render '" + a.name + "' has no frames in the window";
  }
  if (summary != nullptr) {
    summary->clear();
  }
  const auto note = [&summary](const std::string& what) {
    if (summary != nullptr) {
      *summary += (summary->empty() ? "" : ", ") + what;
    }
  };
  for (const AudioCheck& c : a.checks) {
    const std::string head = c.text + ": ";
    switch (c.kind) {
      case AudioCheck::Kind::Silent:
      case AudioCheck::Kind::Sound: {
        const bool silent = is_silent(s);
        note(silent ? "silent" : "peak " + num(peak_db(s)) + " dB");
        if (silent != (c.kind == AudioCheck::Kind::Silent)) {
          return head + (silent ? "the render is silent" : "the render is not silent (peak " + num(peak_db(s)) + " dB)");
        }
        break;
      }
      case AudioCheck::Kind::Clicks: {
        const unsigned n = count_clicks(s);
        note("clicks " + std::to_string(n));
        const bool ok = c.cmp == AudioCheck::Cmp::Less ? static_cast<double>(n) < c.value : static_cast<double>(n) == c.value;
        if (!ok) {
          return head + "measured " + std::to_string(n) + " click(s), wanted " + (c.cmp == AudioCheck::Cmp::Less ? "fewer than " : "") + num(c.value);
        }
        break;
      }
      case AudioCheck::Kind::Zipper: {
        if (is_silent(s)) {
          return head + "nothing to measure (the render is silent in the window)";
        }
        if (s.size() < kZipperMinFrames) {
          return head + "the window is too short to measure (" + std::to_string(s.size()) + " frames, at least " + std::to_string(kZipperMinFrames) + ")";
        }
        const unsigned n = count_zipper(s.mid(), s.rate);
        note("zipper " + std::to_string(n));
        if (static_cast<double>(n) != c.value) {
          return head + "measured " + std::to_string(n) + " spectral-flux spike(s), wanted " + num(c.value);
        }
        break;
      }
      case AudioCheck::Kind::Envelope: {
        const double e = envelope_error(s, c.model);
        if (e < 0.0) {
          return head + "the render never starts (no sample reaches -50 dB)";
        }
        note("envelope off by " + num(e));
        if (e > c.tol) {
          return head + "the level curve is off the model by " + num(e) + " of the peak, wanted at most " + num(c.tol);
        }
        break;
      }
      case AudioCheck::Kind::Pan: {
        if (is_silent(s)) {
          return head + "nothing to measure (both channels are silent in the window)";
        }
        const double balance = pan_balance_db(s);
        note("pan " + num(balance) + " dB");
        bool ok = false;
        switch (c.pan) {
          case AudioCheck::PanKind::Left: ok = balance >= 40.0; break;
          case AudioCheck::PanKind::Right: ok = balance <= -40.0; break;
          case AudioCheck::PanKind::Centre: ok = std::fabs(balance) <= c.tol; break;
          case AudioCheck::PanKind::Db: ok = std::fabs(balance - c.value) <= c.tol; break;
        }
        if (!ok) {
          return head + "the balance (20 log10 L/R) is " + num(balance) + " dB";
        }
        break;
      }
      case AudioCheck::Kind::Null: {
        std::vector<Signal> parts;
        std::string names;
        for (const std::string& name : c.others) {
          const Pcm* other = lookup(name);
          if (other == nullptr) {
            return head + "there is no finished render '" + name + "'";
          }
          parts.push_back(to_signal(*other, a.from_ms, a.to_ms));
          names += (names.empty() ? "" : "+") + name;
        }
        const Signal sum = sum_signals(parts);
        if (sum.size() + c.lag < s.size()) {
          return head + "the renders differ in length in the window (" + std::to_string(s.size()) + " frames and " + std::to_string(sum.size()) + (c.lag != 0u ? " with a lag of " + std::to_string(c.lag) : std::string()) + ")";
        }
        const double residual = null_db(s, sum, c.lag);
        if (std::isnan(residual)) {
          return head + (s.size() <= c.lag ? "the lag of " + std::to_string(c.lag) + " frames leaves nothing to compare in a window of " + std::to_string(s.size()) : std::string("nothing to compare (this render and the others are all silent in the window)"));
        }
        note("null " + names + " " + num(residual) + " dB");
        if (!(residual <= c.value)) {
          return head + "the residual is " + num(residual) + " dB re this render, wanted at most " + num(c.value);
        }
        break;
      }
      default: {  // a number: pitch, rms, peak, dc, centroid, alias, onset
        double want = c.value;
        if (!c.ref.empty()) {
          const Pcm* other = lookup(c.ref);
          if (other == nullptr) {
            return head + "there is no finished render '" + c.ref + "'";
          }
          double ref = 0.0;
          if (!measure(c.kind, to_signal(*other, a.from_ms, a.to_ms), ref)) {
            return head + "'" + c.ref + "' has no " + name_of(c.kind) + " to compare with";
          }
          want = c.op == '+' ? ref + c.operand : (c.op == '-' ? ref - c.operand : (c.op == '*' ? ref * c.operand : ref));
        }
        double got = 0.0;
        if (!measure(c.kind, s, got)) {
          return head + (c.kind == AudioCheck::Kind::Pitch ? "no periodic part (the render holds no steady tone of 50 Hz to 11 kHz)" : (c.kind == AudioCheck::Kind::Alias ? "nothing to measure (it needs a tone and at least " + std::to_string(kAliasFrame) + " frames in the window, this one has " + std::to_string(s.size()) + ")" : "nothing to measure (the render never reaches -50 dB)"));
        }
        const char* unit = c.kind == AudioCheck::Kind::Pitch || c.kind == AudioCheck::Kind::Centroid ? " Hz" : (c.kind == AudioCheck::Kind::Onset ? " ms" : (c.kind == AudioCheck::Kind::Dc ? "" : " dB"));
        bool ok = false;
        std::string detail;
        if (c.cmp == AudioCheck::Cmp::Less) {
          ok = got < want;
        } else if (c.cmp == AudioCheck::Cmp::Greater) {
          ok = got > want;
        } else if (c.kind == AudioCheck::Kind::Pitch) {
          const double cents = 1200.0 * std::log2(got / want);
          ok = std::fabs(cents) <= c.tol;
          detail = " (" + num(cents) + " cents off, at most " + num(c.tol) + ")";
        } else {
          ok = std::fabs(got - want) <= c.tol;
          detail = " (off by " + num(got - want) + ", at most " + num(c.tol) + ")";
        }
        note(std::string(name_of(c.kind)) + " " + num(got) + unit);
        if (!ok) {
          return head + "measured " + num(got) + unit + ", wanted " + (c.cmp == AudioCheck::Cmp::Less ? "under " : (c.cmp == AudioCheck::Cmp::Greater ? "over " : "")) + num(want) + unit + detail;
        }
        break;
      }
    }
  }
  return std::string();
}

}  // namespace o3os::sim
