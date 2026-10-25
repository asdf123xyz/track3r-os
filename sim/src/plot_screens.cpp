// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/sim/plot_screens.h>

#include <o3os/ui/components.h>
#include <o3os/ui/plot/curve.h>
#include <o3os/ui/plot/meters.h>
#include <o3os/ui/plot/peak_cache.h>
#include <o3os/ui/plot/plot_math.h>
#include <o3os/ui/plot/scope.h>
#include <o3os/ui/plot/spectrum.h>
#include <o3os/ui/plot/wave_bitmap.h>
#include <o3os/ui/plot/wave_view.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace o3os::sim {

namespace {

using ui::Axis;
using ui::Color;
using ui::Face;
using ui::Rect;
namespace plot = ui::plot;

// ---- Deterministic synthetic signals ---------------------------------------------------------------------------------
//
// Integer arithmetic only: the gallery's goldens hash the pictures, so the signals must not depend on a libm. A sine is
// the parabola with the classic correction (error under 0.1 %); noise is xorshift32; a drum is a pitch-dropping sine with an
// exponential (shift-and-subtract) decay, a noise burst for the snare and short ticks for the hat.

constexpr std::uint32_t kRate = 44100;
constexpr plot::Frame kFrames = 1u << 16;

struct Rng {
  std::uint32_t s = 2463534242u;
  std::uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  int noise() { return static_cast<int>(next() >> 16) - 32768; }  // -32768 .. 32767
};

// sin(2 pi phase / 2^32) in Q15.
int isin(std::uint32_t phase) {
  const bool negative = (phase & 0x80000000u) != 0;
  const std::int64_t x = (phase & 0x7FFFFFFFu) >> 16;                 // 0..32767: the half period
  const std::int64_t p = (x * (32768 - x)) >> 13;                    // the parabola, 0..32768
  const std::int64_t y = (p * (25395 + ((7373 * p) >> 15))) >> 15;   // 0.775 p + 0.225 p^2
  const int v = static_cast<int>(y > 32767 ? 32767 : y);
  return negative ? -v : v;
}
constexpr std::uint32_t inc_of(std::uint32_t hz) { return static_cast<std::uint32_t>((std::uint64_t{hz} << 32) / kRate); }

void clamp_into(std::vector<std::int16_t>& out, const std::vector<int>& mix, unsigned channels, unsigned ch) {
  for (std::size_t i = 0; i < mix.size(); ++i) {
    out[i * channels + ch] = static_cast<std::int16_t>(mix[i] > 32767 ? 32767 : (mix[i] < -32768 ? -32768 : mix[i]));
  }
}

void add_kick(std::vector<int>& mix, std::size_t start, int gain_q8) {
  std::uint32_t phase = 0;
  std::uint64_t inc = inc_of(48), extra = inc_of(190);
  std::int64_t amp = std::int64_t{32767} * gain_q8 / 256;
  for (std::size_t i = start; i < mix.size() && amp > 8; ++i) {
    mix[i] += static_cast<int>((std::int64_t{isin(phase)} * amp) >> 15);
    phase += static_cast<std::uint32_t>(inc + extra);
    extra -= extra >> 10;
    amp -= amp >> 12;
  }
}
void add_snare(std::vector<int>& mix, std::size_t start, int gain_q8, Rng& rng) {
  std::uint32_t phase = 0;
  std::int64_t amp = std::int64_t{26000} * gain_q8 / 256, tone = std::int64_t{14000} * gain_q8 / 256;
  for (std::size_t i = start; i < mix.size() && amp > 8; ++i) {
    mix[i] += static_cast<int>((std::int64_t{rng.noise()} * amp) >> 15) + static_cast<int>((std::int64_t{isin(phase)} * tone) >> 15);
    phase += inc_of(185);
    amp -= amp >> 11;
    tone -= tone >> 9;
  }
}
void add_hat(std::vector<int>& mix, std::size_t start, int gain_q8, Rng& rng) {
  std::int64_t amp = std::int64_t{12000} * gain_q8 / 256;
  int prev = 0;
  for (std::size_t i = start; i < mix.size() && amp > 8; ++i) {
    const int n = rng.noise();
    mix[i] += static_cast<int>((std::int64_t{n - prev} * amp) >> 16);  // the difference of noise: bright
    prev = n;
    amp -= amp >> 8;
  }
}

// A sound with its cache, built once. The sample and storage vectors come from the heap (the simulator may): their
// alignment is the allocator's, 16 bytes, which PeakCache::init() checks.
struct Sound {
  std::vector<std::int16_t> samples;
  std::vector<std::int16_t> store;
  plot::PeakCache cache;
  unsigned channels = 1;

  void finish(unsigned ch) {
    channels = ch;
    store.assign(plot::PeakCache::elements_required(kFrames, ch), 0);
    cache.init(store.data(), store.size(), samples.data(), kFrames, ch);
    cache.build();
  }
};

// A drum loop, hot enough that the kick clips: kick, snare, hats. The right channel is the left a little changed.
const Sound& drum_stereo() {
  static const Sound sound = [] {
    Sound s;
    s.samples.assign(std::size_t{kFrames} * 2, 0);
    for (unsigned ch = 0; ch < 2; ++ch) {
      Rng rng;
      rng.s += ch * 977u;
      std::vector<int> mix(kFrames, 0);
      add_kick(mix, 0, ch == 0 ? 400 : 330);  // 1.56x and 1.29x: the left channel clips
      add_snare(mix, 20000, 200 + 20 * ch, rng);
      add_kick(mix, 32768, 190);
      for (std::size_t t = 40000; t < kFrames; t += 5500) {
        add_hat(mix, t, 150, rng);
      }
      clamp_into(s.samples, mix, 2, ch);
    }
    s.finish(2);
    return s;
  }();
  return sound;
}
const Sound& drum_mono() {
  static const Sound sound = [] {
    Sound s;
    s.samples.assign(kFrames, 0);
    Rng rng;
    std::vector<int> mix(kFrames, 0);
    add_kick(mix, 0, 230);
    add_snare(mix, 16384, 220, rng);
    add_kick(mix, 24576, 200);
    add_snare(mix, 40960, 200, rng);
    for (std::size_t t = 49152; t < kFrames; t += 2048) {
      add_hat(mix, t, 120, rng);
    }
    clamp_into(s.samples, mix, 1, 0);
    s.finish(1);
    return s;
  }();
  return sound;
}
// A sine sweep from 40 Hz to 8 kHz.
const Sound& chirp_mono() {
  static const Sound sound = [] {
    Sound s;
    s.samples.assign(kFrames, 0);
    std::uint32_t phase = 0;
    std::uint64_t inc = inc_of(40);
    for (plot::Frame i = 0; i < kFrames; ++i) {
      // a parabolic swell: silent at both ends, full in the middle
      const std::int64_t x = std::int64_t{i} * 2 - kFrames;  // -kFrames .. kFrames
      const std::int64_t env = 32000 - (32000 * x * x) / (std::int64_t{kFrames} * kFrames);
      s.samples[i] = static_cast<std::int16_t>((std::int64_t{isin(phase)} * env) >> 15);
      phase += static_cast<std::uint32_t>(inc);
      inc += (inc * 85) >> 20;  // x200 over the sound
    }
    s.finish(1);
    return s;
  }();
  return sound;
}
const Sound& noise_mono() {
  static const Sound sound = [] {
    Sound s;
    s.samples.assign(kFrames, 0);
    Rng rng;
    rng.s = 99991u;
    for (plot::Frame i = 0; i < kFrames; ++i) {
      const std::int64_t env = (std::int64_t{kFrames - i} * 24000) / kFrames;  // a fade-out
      s.samples[i] = static_cast<std::int16_t>((std::int64_t{rng.noise()} * env) >> 15);
    }
    s.finish(1);
    return s;
  }();
  return sound;
}
const Sound& sine_mono() {
  static const Sound sound = [] {
    Sound s;
    s.samples.assign(kFrames, 0);
    std::uint32_t phase = 0;
    for (plot::Frame i = 0; i < kFrames; ++i) {
      // an attack of 4096 frames and a long decay: the shape of a plucked tone
      const std::int64_t env = i < 4096 ? std::int64_t{i} * 32767 / 4096 : std::int64_t{32767} * (kFrames - i) / (kFrames - 4096);
      s.samples[i] = static_cast<std::int16_t>((std::int64_t{isin(phase)} * env) >> 15);
      phase += inc_of(110);
    }
    s.finish(1);
    return s;
  }();
  return sound;
}

// ---- The standard chrome ---------------------------------------------------------------------------------------------
//
// The status strip on top (the title in the Display face and a few Label fields), the info line and the F-key key-caps
// below, as the pattern view has them: the plot states are drawn between, so their budget includes what a screen pays.

struct Frame {
  Rect pane;  // between the strip and the info line
};

constexpr const char* kKeys[8] = {"WAVE", "CHOP", "LOOP", "FILL", "ZOOM", "SEL", "PLAY", "UNDO"};

Frame chrome(ui::Renderer& r, const char* title, const char* caption, const char* value, const char* info) {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const int left = 2 * m.pad;
  ui::strip(r, {0, 0, ui::kScreenW, m.strip_h}, ui::Edge::Bottom);
  ui::FieldRow row(r, left, (m.strip_h + t.geometry(Face::Display).cap_h) / 2);
  row.put(Face::Display, title, Color::ChromeText);
  row.skip(2 * m.pad);
  row.field(caption, Face::Data, value, Color::ChromeDim, Color::ChromeText);
  const int info_y = ui::kScreenH - m.keys_h - m.info_h;
  ui::strip(r, {0, info_y, ui::kScreenW, m.info_h + m.keys_h}, ui::Edge::Top);
  r.text(left, ui::caps_y(t, Face::Label, info_y + m.border, m.info_h - m.border), info, Color::ChromeDim, Face::Label);
  ui::soft_keys(r, {0, ui::kScreenH - m.keys_h, ui::kScreenW, m.keys_h}, kKeys, 8);
  return Frame{{0, m.strip_h, ui::kScreenW, info_y - m.strip_h}};
}

// The bitmap job runs in the frame it is wanted in: the picture goes to the display behind the renderer (a RAM_G upload may
// be made inside an open frame) and is drawn straight after, so a state is a pure function of its variant.
struct Job {
  std::unique_ptr<plot::WaveBitmap> bitmap = std::make_unique<plot::WaveBitmap>();
};

bool wave_lane(ui::Renderer& r, Job& job, unsigned lane_index, const Rect& lane, const Sound& s, unsigned channel,
               const plot::WaveWindow& w, plot::WaveMode mode, const plot::WaveMarks& marks,
               const plot::WaveStyle& style = plot::WaveStyle{}) {
  if (mode != plot::WaveMode::Vector) {
    job.bitmap->request(lane_index, plot::wave_key(s.cache, 0, channel, w, lane,
                                                   mode == plot::WaveMode::BitmapL1 ? plot::WaveDepth::L1 : plot::WaveDepth::L4));
    job.bitmap->service_all(r.display());
  }
  return plot::draw_wave(r, lane, s.cache, channel, w, mode, job.bitmap.get(), lane_index, 0, marks, style);
}

// A time ruler under (or over) a lane: ticks every nice step of seconds, labelled in the Micro face.
void time_ruler(ui::Renderer& r, const Rect& lane, const plot::WaveWindow& w) {
  const plot::AxisSpec axis{plot::AxisKind::Linear, static_cast<float>(w.first) / static_cast<float>(kRate),
                            static_cast<float>(w.first + w.span) / static_cast<float>(kRate)};
  plot::Tick ticks[16];
  const unsigned n = plot::make_ticks(axis, static_cast<unsigned>(lane.w / 70), ticks, 16);
  const int y = lane.y + lane.h;
  for (unsigned i = 0; i < n; ++i) {
    const int x = lane.x + static_cast<int>(plot::axis_t(axis, ticks[i].value) * static_cast<float>(lane.w - 1) + 0.5f);
    r.rule(x, y + 1, ticks[i].major ? 5 : 3, Axis::Vertical, Color::TextDim);
  }
  for (unsigned i = 0; i < n; ++i) {
    if (!ticks[i].major) {
      continue;
    }
    char text[10];
    plot::format_value(ticks[i].value, text);
    const int x = lane.x + static_cast<int>(plot::axis_t(axis, ticks[i].value) * static_cast<float>(lane.w - 1) + 0.5f);
    r.text(x - r.text_width(text, Face::Micro) / 2, y + 8, text, Color::TextDim, Face::Micro);
  }
}

plot::WaveMarks editor_marks(const plot::WaveWindow& w, bool full) {
  plot::WaveMarks m;
  m.zero_line = true;
  m.clip_tint = true;
  m.has_selection = true;
  m.sel_begin = w.first + w.span * 30 / 100;
  m.sel_end = w.first + w.span * 54 / 100;
  m.has_loop = full;
  m.loop_start = w.first + w.span * 12 / 100;
  m.loop_end = w.first + w.span * 80 / 100;
  m.has_playhead = true;
  m.playhead = w.first + w.span * 40 / 100;
  if (full) {
    const unsigned at[] = {0, 14, 31, 50, 62, 85};
    for (const unsigned a : at) {
      m.slices[m.slice_count++] = w.first + w.span * a / 100;
    }
  }
  return m;
}

// ---- Wave states -----------------------------------------------------------------------------------------------------

// The sample editor: a stereo drum loop as two L1 bitmap lanes with every mark, and a time ruler.
void draw_wave_l1(ui::Renderer& r, unsigned) {
  const ui::Metrics& m = r.theme().metrics;
  const Frame f = chrome(r, "SAMPLE", "DRUMLOOP.WAV", "L+R 44100", "L  sel 0:19.5 - 0:35.7   loop on   6 slices");
  const int x = 36, w = ui::kScreenW - x - 2 * m.pad;
  const Rect left{x, f.pane.y + 2 * m.pad, w, 176}, right{x, left.y + left.h + 2 * m.pad, w, 176};
  const plot::WaveWindow win{0, kFrames};
  Job job;
  r.frame({left.x - 1, left.y - 1, left.w + 2, left.h + 2}, Color::Empty);
  r.frame({right.x - 1, right.y - 1, right.w + 2, right.h + 2}, Color::Empty);
  const Sound& s = drum_stereo();
  wave_lane(r, job, 0, left, s, 0, win, plot::WaveMode::BitmapL1, editor_marks(win, true));
  wave_lane(r, job, 1, right, s, 1, win, plot::WaveMode::BitmapL1, editor_marks(win, true));
  r.text(m.pad, left.y + left.h / 2 - 8, "L", Color::TextDim, Face::Label);
  r.text(m.pad, right.y + right.h / 2 - 8, "R", Color::TextDim, Face::Label);
  time_ruler(r, right, win);
}

// The same loop zoomed to the kick, mono, in L4: antialiased edges and a sample-accurate selection.
void draw_wave_l4(ui::Renderer& r, unsigned) {
  const ui::Metrics& m = r.theme().metrics;
  const Frame f = chrome(r, "SAMPLE", "KICK.WAV", "MONO 44100", "zoom 1:35   sel 0.01 - 0.12 s");
  const int x = 36, w = ui::kScreenW - x - 2 * m.pad;
  const Rect top{x, f.pane.y + 2 * m.pad, w, 160}, bottom{x, top.y + top.h + 24 + 2 * m.pad, w, 160};
  Job job;
  const plot::WaveWindow whole{0, kFrames}, zoom{0, 9000};
  r.frame({top.x - 1, top.y - 1, top.w + 2, top.h + 2}, Color::Empty);
  r.frame({bottom.x - 1, bottom.y - 1, bottom.w + 2, bottom.h + 2}, Color::Empty);
  const Sound& s = drum_mono();
  wave_lane(r, job, 0, top, s, 0, whole, plot::WaveMode::BitmapL4, editor_marks(whole, false));
  time_ruler(r, top, whole);
  wave_lane(r, job, 1, bottom, s, 0, zoom, plot::WaveMode::BitmapL4, editor_marks(zoom, false));
  time_ruler(r, bottom, zoom);
}

// A sample list: a vector thumbnail beside each name, and the first frames of the chirp as a line with dots below.
void draw_wave_list(ui::Renderer& r, unsigned) {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Frame f = chrome(r, "SAMPLES", "SELECTED", "04 CHIRP", "vector thumbnails: two strips a column, 120 columns");
  struct Row {
    const char* name;
    const Sound* sound;
  } rows[] = {{"01 SINE", &sine_mono()}, {"02 KICK", &drum_mono()}, {"03 NOISE", &noise_mono()}, {"04 CHIRP", &chirp_mono()}};
  const int row_h = 44, thumb_w = 120, thumb_h = 32;
  Job job;
  int y = f.pane.y + 2 * m.pad;
  plot::WaveStyle style;
  for (unsigned i = 0; i < 4; ++i, y += row_h) {
    const bool selected = i == 3;
    if (selected) {
      r.highlight_row({0, y - m.pad, ui::kScreenW, row_h});
    }
    r.text(2 * m.pad, y + 6, rows[i].name, selected ? Color::SelText : Color::Text, Face::Data);
    const Rect thumb{200, y, thumb_w, thumb_h};
    r.panel({thumb.x - 1, thumb.y - 1, thumb.w + 2, thumb.h + 2}, Color::Empty);
    r.panel(thumb, Color::Pane);
    wave_lane(r, job, 0, thumb, *rows[i].sound, 0, plot::WaveWindow{0, kFrames}, plot::WaveMode::Vector, plot::WaveMarks{}, style);
  }
  // the first 40 frames of the chirp, 17 px apart
  const Rect zoom{36, y + 2 * m.pad, 700, 130};
  r.frame({zoom.x - 1, zoom.y - 1, zoom.w + 2, zoom.h + 2}, Color::Empty);
  plot::WaveMarks marks;
  marks.zero_line = true;
  wave_lane(r, job, 0, zoom, chirp_mono(), 0, plot::WaveWindow{40000, 40}, plot::WaveMode::Vector, marks, style);
  r.text(zoom.x, zoom.y + zoom.h + 6, "40 frames: a line through the frames, a dot on each", Color::TextDim, Face::Label);
}


// ---- Curve states ----------------------------------------------------------------------------------------------------

// An envelope of polynomial pieces only (no libm: the gallery's hashes must not depend on one): a cubic ease-out attack, a
// quadratic decay to the sustain level, a hold, a quadratic release.
float envelope(float t) {
  const float attack = 0.05f, decay = 0.40f, sustain = 0.60f, release_at = 1.40f, release = 0.55f;
  if (t < attack) {
    const float u = 1.0f - t / attack;
    return 1.0f - u * u * u;
  }
  if (t < attack + decay) {
    const float u = 1.0f - (t - attack) / decay;
    return sustain + (1.0f - sustain) * u * u;
  }
  if (t < release_at) {
    return sustain;
  }
  if (t < release_at + release) {
    const float u = 1.0f - (t - release_at) / release;
    return sustain * u * u;
  }
  return 0.0f;
}

// A small LFO shape: a triangle blended into a parabola (exact in float arithmetic).
float lfo(float t) {
  const float tri = t < 0.5f ? 4.0f * t - 1.0f : 3.0f - 4.0f * t;  // -1 .. 1
  return 0.5f + 0.5f * (0.65f * tri + 0.35f * tri * tri * tri);
}

void field(ui::Renderer& r, int x, int y, const char* caption, const char* value) {
  r.text(x, y, caption, Color::TextDim, Face::Label);
  r.text(x, y + 16, value, Color::Text, Face::Data);
}

void draw_curve_env(ui::Renderer& r, unsigned) {
  const Frame f = chrome(r, "ENVELOPE", "INSTRUMENT", "01 KICK", "A 50 ms   D 400 ms   S 60 %   R 550 ms");
  const plot::AxisSpec x{plot::AxisKind::Linear, 0.0f, 2.0f}, y{plot::AxisKind::Linear, 0.0f, 1.0f};
  const Rect area{56, f.pane.y + 24, 540, 320};
  plot::CurveStyle style;
  plot::draw_axes(r, area, x, y, style);
  float values[512];
  for (unsigned i = 0; i < 512; ++i) {
    values[i] = envelope(2.0f * static_cast<float>(i) / 511.0f);
  }
  plot::CurveSeries series;
  series.y = values;
  series.n = 512;
  series.x_lo = 0.0f;
  series.x_hi = 2.0f;
  auto scratch = std::make_unique<plot::CurveScratch>();
  plot::draw_series(r, area, x, y, series, true, style, *scratch);
  const plot::CurveHandle handles[] = {{0.05f, 1.0f, false}, {0.45f, 0.6f, true}, {1.40f, 0.6f, false}, {1.95f, 0.0f, false}};
  plot::draw_handles(r, area, x, y, handles, 4, style);
  // the parameters, and the LFO beside them
  const int px = area.x + area.w + 36;
  field(r, px, area.y, "ATTACK", "50 ms");
  field(r, px, area.y + 44, "DECAY", "400 ms");
  field(r, px, area.y + 88, "SUSTAIN", "60 %");
  field(r, px, area.y + 132, "RELEASE", "550 ms");
  const Rect lfo_area{px, area.y + 214, 116, 82};
  const plot::AxisSpec lx{plot::AxisKind::Linear, 0.0f, 1.0f}, ly{plot::AxisKind::Linear, 0.0f, 1.0f};
  float shape[128];
  for (unsigned i = 0; i < 128; ++i) {
    shape[i] = lfo(static_cast<float>(i) / 127.0f);
  }
  plot::CurveSeries ls;
  ls.y = shape;
  ls.n = 128;
  r.text(px, lfo_area.y - 18, "LFO SHAPE", Color::TextDim, Face::Label);
  plot::CurveStyle lstyle;
  lstyle.line = Color::Fx;
  lstyle.fill = Color::Fx;
  r.panel({lfo_area.x - 1, lfo_area.y - 1, lfo_area.w + 2, lfo_area.h + 2}, Color::Empty);
  r.panel(lfo_area, Color::Pane);
  plot::draw_series(r, lfo_area, lx, ly, ls, true, lstyle, *scratch);
}

// RBJ audio-EQ-cookbook sections: the gallery builds its own coefficients; the library only plots them.
plot::Biquad rbj_lowpass(float fs, float f0, float q) {
  const float w0 = 6.2831853f * f0 / fs, alpha = std::sin(w0) / (2.0f * q), c = std::cos(w0), a0 = 1.0f + alpha;
  return {(1.0f - c) / 2.0f / a0, (1.0f - c) / a0, (1.0f - c) / 2.0f / a0, -2.0f * c / a0, (1.0f - alpha) / a0};
}
plot::Biquad rbj_peaking(float fs, float f0, float q, float gain_db) {
  const float a = std::pow(10.0f, gain_db / 40.0f), w0 = 6.2831853f * f0 / fs, alpha = std::sin(w0) / (2.0f * q),
              c = std::cos(w0), a0 = 1.0f + alpha / a;
  return {(1.0f + alpha * a) / a0, -2.0f * c / a0, (1.0f - alpha * a) / a0, -2.0f * c / a0, (1.0f - alpha / a) / a0};
}

void draw_curve_filter(ui::Renderer& r, unsigned) {
  const Frame f = chrome(r, "FILTER", "INSTRUMENT", "04 CHIRP", "LP 1.2 kHz  Q 3.5   PEAK 180 Hz +6 dB");
  const plot::AxisSpec x{plot::AxisKind::LogFreq, 20.0f, 20000.0f}, y{plot::AxisKind::Db, -24.0f, 24.0f};
  const Rect area{56, f.pane.y + 24, 704, 320};
  plot::CurveStyle style;
  plot::draw_axes(r, area, x, y, style);
  constexpr unsigned kN = 384;
  float phi[kN], total[kN], section[kN];
  plot::response_basis(x, 44100.0f, kN, phi);
  plot::biquad_response_db(rbj_lowpass(44100.0f, 1200.0f, 3.5f), phi, kN, total);
  plot::biquad_response_db(rbj_peaking(44100.0f, 180.0f, 1.2f, 6.0f), phi, kN, section);
  for (unsigned i = 0; i < kN; ++i) {
    total[i] += section[i];
  }
  plot::CurveSeries series;
  series.y = total;
  series.n = kN;
  series.x_lo = 20.0f;
  series.x_hi = 20000.0f;  // evenly spaced in the axis: the basis is evenly spaced in log frequency, so this is a log series
  // The series is uniform in the axis, not in Hz: give explicit x so that the log axis places them correctly.
  float xs[kN];
  for (unsigned i = 0; i < kN; ++i) {
    xs[i] = plot::axis_value(x, static_cast<float>(i) / static_cast<float>(kN - 1));
  }
  series.x = xs;
  auto scratch = std::make_unique<plot::CurveScratch>();
  plot::draw_series(r, area, x, y, series, true, style, *scratch);
  // handles: the cutoff (being dragged) and the peak
  const auto at = [&](float hz) {
    const float s = std::sin(3.14159265f * hz / 44100.0f);
    return plot::biquad_db(rbj_lowpass(44100.0f, 1200.0f, 3.5f), s * s) + plot::biquad_db(rbj_peaking(44100.0f, 180.0f, 1.2f, 6.0f), s * s);
  };
  const plot::CurveHandle handles[] = {{1200.0f, at(1200.0f), true}, {180.0f, at(180.0f), false}};
  plot::draw_handles(r, area, x, y, handles, 2, style);
}

// ---- Scope, spectrum, meters -----------------------------------------------------------------------------------------

// A block of a Lissajous: left 3 : right 2, the right channel's phase advanced by `phi` (a quarter of a turn is 1 << 30).
std::vector<std::int16_t> lissajous_block(unsigned frames, std::uint32_t phi, unsigned a, unsigned b) {
  std::vector<std::int16_t> lr(std::size_t{frames} * 2);
  const std::uint32_t step = static_cast<std::uint32_t>((std::uint64_t{1} << 32) / frames);
  for (unsigned i = 0; i < frames; ++i) {
    lr[2 * std::size_t{i}] = static_cast<std::int16_t>(isin(step * i * a) * 7 / 8);
    lr[2 * std::size_t{i} + 1] = static_cast<std::int16_t>(isin(step * i * b + phi) * 7 / 8);
  }
  return lr;
}

void draw_scope(ui::Renderer& r, unsigned) {
  const Frame f = chrome(r, "SCOPE", "MASTER", "L/R + M/S", "X-Y 3:2   phosphor trail 4 blocks");
  const int side = 330;
  const Rect left{40, f.pane.y + (f.pane.h - side) / 2, side, side}, right{ui::kScreenW - 40 - side, left.y, side, side};
  plot::Scope lissajous, gonio;
  for (unsigned g = 0; g < 4; ++g) {
    const auto lr = lissajous_block(2048, (1u << 28) * (g + 1), 3, 2);
    lissajous.push(lr.data(), 2048, plot::ScopeMode::LeftRight);
    // a stereo signal with a phase offset, as a goniometer shows it (mid up, side across); the offset wanders
    const auto st = lissajous_block(2048, (1u << 27) * (g + 2), 1, 1);
    gonio.push(st.data(), 2048, plot::ScopeMode::MidSide);
  }
  plot::ScopeStyle xy, dots;
  xy.lines = true;  // a Lissajous is a curve: the line through its points
  lissajous.draw(r, left, xy);
  gonio.draw(r, right, dots);  // a goniometer is a cloud of stereo samples: dots
  r.text(left.x + left.w / 2 - r.text_width("L / R  3:2", Face::Label) / 2, left.y + left.h + 8, "L / R  3:2", Color::TextDim, Face::Label);
  r.text(right.x + right.w / 2 - r.text_width("MID / SIDE", Face::Label) / 2, right.y + right.h + 8, "MID / SIDE", Color::TextDim, Face::Label);
}

// Feeds a spectrum the blocks of a drum loop that end on a snare, so that the bars are the snare's broadband burst and the
// peak caps keep the kick's low end, the hats' top and the ring of the toms in between.
void run_spectrum(plot::Spectrum& s, unsigned blocks) {
  const Sound& drums = drum_mono();
  const Sound& tone = chirp_mono();
  std::vector<std::int16_t> mix(512);
  const std::size_t end = 16384 + 700;  // a little into the snare
  for (unsigned b = 0; b < blocks; ++b) {
    const std::size_t at = end - 512 - std::size_t{blocks - 1 - b} * 480;
    const std::size_t sweep = 20000 + std::size_t{b} * 900;  // a tone sweeping up under the drums
    for (unsigned i = 0; i < 512; ++i) {
      mix[i] = static_cast<std::int16_t>(drums.samples[at + i] / 2 + tone.samples[sweep + i] / 6);
    }
    s.analyze(mix.data(), 1);
  }
}

void draw_spectrum_state(ui::Renderer& r, unsigned) {
  const Frame f = chrome(r, "SPECTRUM", "INPUT", "MASTER 512", "48 bars 86 Hz - 16 kHz   peak hold 1 s");
  plot::Spectrum::Config cfg;
  plot::Spectrum bars, fill;
  bars.init(cfg);
  run_spectrum(bars, 24);
  fill.init(cfg);
  run_spectrum(fill, 24);
  const plot::AxisSpec hz{plot::AxisKind::LogFreq, cfg.f_lo, cfg.f_hi}, db{plot::AxisKind::Db, cfg.floor_db, cfg.ceil_db};
  const Rect top{56, f.pane.y + 18, 704, 230}, bottom{56, top.y + top.h + 34, 704, 84};
  plot::CurveStyle style;
  plot::draw_axes(r, top, hz, db, style);
  plot::SpectrumStyle bar_style;
  plot::draw_spectrum(r, top, bars, bar_style);
  plot::draw_axes(r, bottom, hz, plot::AxisSpec{plot::AxisKind::Db, cfg.floor_db, cfg.ceil_db}, style);
  plot::SpectrumStyle fill_style;
  fill_style.shape = plot::SpectrumShape::Fill;
  fill_style.bar = Color::Vol;
  fill_style.caps = false;
  plot::draw_spectrum(r, bottom, fill, fill_style);
}

void draw_meters_state(ui::Renderer& r, unsigned) {
  const Frame f = chrome(r, "METERS", "MIXER", "8 TRACKS + MASTER", "peak, RMS, hold and clip; 60 dB scale");
  plot::DbScale scale;
  scale.init(-60.0f, 0.0f);
  plot::MeterStyle style;
  constexpr unsigned kTracks = 8;
  Rect rects[kTracks + 2];
  plot::MeterState states[kTracks + 2];
  const int bar_h = 316, top = f.pane.y + 24;
  for (unsigned i = 0; i < kTracks; ++i) {
    rects[i] = {80 + static_cast<int>(i) * 40, top, 22, bar_h};
  }
  rects[kTracks] = {470, top, 34, bar_h};
  rects[kTracks + 1] = {514, top, 34, bar_h};
  // readings: each track a different level, a few updates so that the peaks have fallen and the holds have not
  for (unsigned i = 0; i < kTracks + 2; ++i) {
    const float peak = i == 1 ? 1.0f : (i == 9 ? 0.80f : 0.88f - 0.085f * static_cast<float>(i));
    const float rms = peak * (i == 1 ? 0.40f : 0.50f);
    states[i].update({peak, rms});  // the hold keeps this peak ...
    for (unsigned k = 0; k < 8; ++k) {
      states[i].update({peak * 0.82f, rms});  // ... the bars settle at 0.82 of it (the peak) and the RMS
    }
  }
  plot::draw_meters(r, rects, states, kTracks + 2, Axis::Vertical, scale, style);
  const float marks[] = {0.0f, -6.0f, -12.0f, -24.0f, -48.0f};
  plot::draw_meter_scale(r, rects[0], Axis::Vertical, scale, marks, 5, style);
  plot::draw_meter_scale(r, rects[kTracks], Axis::Vertical, scale, marks, 5, style);
  for (unsigned i = 0; i < kTracks; ++i) {
    char name[3] = {static_cast<char>('1' + i), '\0', '\0'};
    r.text(rects[i].x + 2, rects[i].y + rects[i].h + 6, name, Color::TextDim, Face::Label);
  }
  r.text(rects[kTracks].x + 2, rects[kTracks].y + rects[kTracks].h + 6, "L", Color::TextDim, Face::Label);
  r.text(rects[kTracks + 1].x + 2, rects[kTracks + 1].y + rects[kTracks + 1].h + 6, "R", Color::TextDim, Face::Label);
  // horizontal bars: a send, a return, the cue
  Rect hbars[4];
  plot::MeterState hs[4];
  const int hx = 600, hw = 176;
  for (unsigned i = 0; i < 4; ++i) {
    hbars[i] = {hx, top + 20 + static_cast<int>(i) * 52, hw, 16};
    const float peak = 0.9f - 0.2f * static_cast<float>(i);
    hs[i].update({peak, peak * 0.5f});
    for (unsigned k = 0; k < 8; ++k) {
      hs[i].update({peak * 0.82f, peak * 0.5f});
    }
  }
  plot::draw_meters(r, hbars, hs, 4, Axis::Horizontal, scale, style);
  const char* names[4] = {"SEND A", "SEND B", "RETURN", "CUE"};
  for (unsigned i = 0; i < 4; ++i) {
    r.text(hx, hbars[i].y - 18, names[i], Color::TextDim, Face::Label);
  }
  const float hmarks[] = {-48.0f, -24.0f, -12.0f, 0.0f};
  plot::draw_meter_scale(r, hbars[3], Axis::Horizontal, scale, hmarks, 4, style);
}

// ---- A page of several plots: what a screen that mixes them pays ------------------------------------------------------

void draw_page(ui::Renderer& r, unsigned) {
  const Frame f = chrome(r, "SAMPLE", "OVERVIEW", "KICK.WAV", "wave, response, spectrum, scope and levels on one page");
  Job job;
  const int m = 8;
  const Rect wave{m, f.pane.y + m, 520, 120};
  r.frame({wave.x - 1, wave.y - 1, wave.w + 2, wave.h + 2}, Color::Empty);
  const plot::WaveWindow win{0, kFrames};
  wave_lane(r, job, 0, wave, drum_mono(), 0, win, plot::WaveMode::BitmapL4, editor_marks(win, false));
  time_ruler(r, wave, win);
  // spectrum, top right
  plot::Spectrum::Config cfg;
  cfg.bars = 24;
  plot::Spectrum spectrum;
  spectrum.init(cfg);
  run_spectrum(spectrum, 16);
  const Rect sp{wave.x + wave.w + 56, wave.y, 190, wave.h};
  const plot::AxisSpec hz{plot::AxisKind::LogFreq, cfg.f_lo, cfg.f_hi}, db{plot::AxisKind::Db, cfg.floor_db, cfg.ceil_db};
  plot::CurveStyle style;
  plot::draw_axes(r, sp, hz, db, style);
  plot::draw_spectrum(r, sp, spectrum, plot::SpectrumStyle{});
  // filter response, bottom left
  const Rect cv{wave.x + 48, wave.y + wave.h + 52, 330, 150};
  const plot::AxisSpec fx{plot::AxisKind::LogFreq, 20.0f, 20000.0f}, fy{plot::AxisKind::Db, -24.0f, 24.0f};
  plot::draw_axes(r, cv, fx, fy, style);
  constexpr unsigned kN = 256;
  float phi[kN], db_curve[kN], xs[kN];
  plot::response_basis(fx, 44100.0f, kN, phi);
  plot::biquad_response_db(rbj_lowpass(44100.0f, 800.0f, 2.5f), phi, kN, db_curve);
  for (unsigned i = 0; i < kN; ++i) {
    xs[i] = plot::axis_value(fx, static_cast<float>(i) / static_cast<float>(kN - 1));
  }
  plot::CurveSeries series;
  series.x = xs;
  series.y = db_curve;
  series.n = kN;
  auto scratch = std::make_unique<plot::CurveScratch>();
  plot::draw_series(r, cv, fx, fy, series, true, style, *scratch);
  const plot::CurveHandle h[] = {{800.0f, 0.0f + 8.0f, true}};
  plot::draw_handles(r, cv, fx, fy, h, 1, style);
  // scope, bottom middle
  plot::Scope scope;
  for (unsigned g = 0; g < 4; ++g) {
    const auto lr = lissajous_block(2048, (1u << 28) * (g + 1), 3, 2);
    scope.push(lr.data(), 2048, plot::ScopeMode::LeftRight);
  }
  const Rect sc{cv.x + cv.w + 32, cv.y, 150, 150};
  plot::ScopeStyle page_scope;
  page_scope.lines = true;
  scope.draw(r, sc, page_scope);
  // meters, bottom right
  plot::DbScale scale;
  scale.init(-60.0f, 0.0f);
  Rect rects[4];
  plot::MeterState states[4];
  for (unsigned i = 0; i < 4; ++i) {
    rects[i] = {sc.x + sc.w + 40 + static_cast<int>(i) * 22, cv.y, 14, 150};
    const float peak = 0.9f - 0.15f * static_cast<float>(i);
    states[i].update({peak, peak * 0.6f});
    states[i].update({peak * 0.5f, peak * 0.6f});
  }
  plot::draw_meters(r, rects, states, 4, Axis::Vertical, scale, plot::MeterStyle{});
  const float marks[] = {0.0f, -12.0f, -36.0f};
  plot::draw_meter_scale(r, rects[0], Axis::Vertical, scale, marks, 3, plot::MeterStyle{});
}

// ---- The worst cases -------------------------------------------------------------------------------------------------
//
// One variant per plot, each drawn at the most words that plot can take (the declared maxima: the widest vector wave with
// every mark; the longest zoomed line; a curve with all its ticks, a full-size fill and every handle; the scope with all
// four generations; 64 bars with caps; sixteen meters), in the standard chrome.

constexpr unsigned kWorstVariants = 7;

void draw_worst(ui::Renderer& r, unsigned variant) {
  static const char* const kNames[kWorstVariants] = {"vector wave",  "zoomed line", "curve",  "scope",
                                                      "spectrum 64", "16 meters",   "wave L4 + marks"};
  const Frame f = chrome(r, "WORST", "CASE", kNames[variant % kWorstVariants], "every plot at its declared maximum of display-list words");
  Job job;
  auto scratch = std::make_unique<plot::CurveScratch>();
  plot::CurveStyle style;
  switch (variant % kWorstVariants) {
    case 0: {  // the vector envelope at its widest grouped size, every mark on, 32 slices, 8 clip runs
      const Rect lane{80, f.pane.y + 40, 640, 150};
      plot::WaveMarks marks = editor_marks(plot::WaveWindow{0, kFrames}, true);
      marks.slice_count = plot::kWaveMaxSlices;
      for (unsigned i = 0; i < plot::kWaveMaxSlices; ++i) {
        marks.slices[i] = i * 2000u;
      }
      r.panel(lane, Color::Pane);
      wave_lane(r, job, 0, lane, noise_mono(), 0, plot::WaveWindow{0, kFrames}, plot::WaveMode::Vector, marks);
      break;
    }
    case 1: {  // a line through the frames with a dot on each: the longest view that is a line
      const Rect lane{40, f.pane.y + 40, 720, 200};
      const plot::WaveStyle ws;
      plot::WaveMarks marks;
      marks.zero_line = true;
      r.panel(lane, Color::Pane);
      wave_lane(r, job, 0, lane, noise_mono(), 0, plot::WaveWindow{1000, ws.max_vertices * 2u - 2u}, plot::WaveMode::Vector, marks);
      wave_lane(r, job, 0, Rect{40, lane.y + 220, 120, 100}, noise_mono(), 0, plot::WaveWindow{1000, 20}, plot::WaveMode::Vector, marks);
      break;
    }
    case 2: {  // a series of 1000 noisy points through a log axis: ticks everywhere, a full fill, sixteen handles
      const plot::AxisSpec x{plot::AxisKind::Linear, 0.0f, 1.0f}, y{plot::AxisKind::Db, -48.0f, 12.0f};
      const Rect area{56, f.pane.y + 24, 704, 320};
      plot::draw_axes(r, area, x, y, style);
      float v[1000];
      std::uint32_t s = 7;
      for (unsigned i = 0; i < 1000; ++i) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        v[i] = -18.0f + 20.0f * (static_cast<float>(s >> 8) / 16777216.0f) + (i % 100 < 50 ? 8.0f : -8.0f);
      }
      plot::CurveSeries series;
      series.y = v;
      series.n = 1000;
      style.max_vertices = plot::kCurveMaxVertices;
      style.tolerance_px = 0.0f;  // keep every point the budget allows
      plot::draw_series(r, area, x, y, series, true, style, *scratch);
      plot::CurveHandle handles[plot::kCurveMaxHandles];
      for (unsigned i = 0; i < plot::kCurveMaxHandles; ++i) {
        handles[i] = {static_cast<float>(i) / 15.0f, -30.0f + 3.0f * static_cast<float>(i), i == 5};
      }
      plot::draw_handles(r, area, x, y, handles, plot::kCurveMaxHandles, style);
      break;
    }
    case 3: {  // the scope, four generations of 96 points, as lines
      plot::Scope scope;
      for (unsigned g = 0; g < 4; ++g) {
        const auto lr = lissajous_block(4096, (1u << 28) * (g + 1), 7, 5);
        scope.push(lr.data(), 4096, plot::ScopeMode::LeftRight);
      }
      plot::ScopeStyle ss;
      ss.lines = true;
      scope.draw(r, {200, f.pane.y + 10, 400, 390}, ss);
      break;
    }
    case 4: {  // 64 bars, caps, and the fill form below
      plot::Spectrum::Config cfg;
      cfg.bars = plot::Spectrum::kMaxBars;
      plot::Spectrum s;
      s.init(cfg);
      run_spectrum(s, 24);
      const Rect area{56, f.pane.y + 24, 704, 250};
      plot::draw_axes(r, area, plot::AxisSpec{plot::AxisKind::LogFreq, cfg.f_lo, cfg.f_hi},
                      plot::AxisSpec{plot::AxisKind::Db, cfg.floor_db, cfg.ceil_db}, style);
      plot::draw_spectrum(r, area, s, plot::SpectrumStyle{});
      break;
    }
    case 5: {  // sixteen meters, all with a clip lamp and a hold
      plot::DbScale scale;
      scale.init(-60.0f, 0.0f);
      Rect rects[16];
      plot::MeterState states[16];
      for (unsigned i = 0; i < 16; ++i) {
        rects[i] = {60 + static_cast<int>(i) * 44, f.pane.y + 24, 28, 320};
        states[i].update({1.0f, 0.5f});
        states[i].update({0.4f, 0.5f});
      }
      plot::draw_meters(r, rects, states, 16, Axis::Vertical, scale, plot::MeterStyle{});
      const float marks[] = {0.0f, -3.0f, -6.0f, -12.0f, -24.0f, -48.0f};
      plot::draw_meter_scale(r, rects[0], Axis::Vertical, scale, marks, 6, plot::MeterStyle{});
      break;
    }
    default: {  // the bitmap wave with every mark, two lanes
      const plot::WaveWindow win{0, kFrames};
      plot::WaveMarks marks = editor_marks(win, true);
      marks.slice_count = plot::kWaveMaxSlices;
      for (unsigned i = 0; i < plot::kWaveMaxSlices; ++i) {
        marks.slices[i] = i * 2000u;
      }
      const Rect a{32, f.pane.y + 16, 760, 180}, b{32, a.y + 196, 760, 180};
      wave_lane(r, job, 0, a, drum_stereo(), 0, win, plot::WaveMode::BitmapL4, marks);
      wave_lane(r, job, 1, b, drum_stereo(), 1, win, plot::WaveMode::BitmapL1, marks);
      break;
    }
  }
}

}  // namespace

const ScreenState kPlotStates[kPlotStateCount] = {
    {"wave_l1", true, 1, kBudgetWords, draw_wave_l1},
    {"wave_l4", true, 1, kBudgetWords, draw_wave_l4},
    {"wave_list", true, 1, kBudgetWords, draw_wave_list},
    {"curve_env", true, 1, kBudgetWords, draw_curve_env},
    {"curve_filter", true, 1, kBudgetWords, draw_curve_filter},
    {"scope", true, 1, kBudgetWords, draw_scope},
    {"spectrum", true, 1, kBudgetWords, draw_spectrum_state},
    {"meters", true, 1, kBudgetWords, draw_meters_state},
    {"page", true, 1, kBudgetWords, draw_page},
    {"worst", true, kWorstVariants, kBudgetWords, draw_worst},
};

}  // namespace o3os::sim
