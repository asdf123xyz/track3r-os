// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-007h: an OG project's samples play as recorded. The unit played the samples of a v16 project from firmware 1.9.0 as "very low, garbled noises": the
// loader bound every instrument as a plain window of its WAV played one shot at the pitch of the step's note, and 12 of the 16 instruments of such a
// project are not plain: 9 are BEAT SLICE (play mode 5: the note picks the slice, note - 24, and the pitch is the root's whatever the note, so a
// drum break of 12 slices played from notes 24..35 is 12 hits at their own pitch) and 2 are WAVETABLE (mode 6: the sample is a stack of 2048-frame
// cycles, played as an oscillator over one of them). Played one shot from note 24, a whole break or a whole stack of cycles comes out three octaves low and
// is not what was recorded.
//
// The oracle is independent of the product code: the WAV is decoded here by a minimal RIFF walk, the instrument's parameters are read from the bytes of
// its .pti at the offsets of docs/formats/og.md section 5, and the expected audio is made by a double-precision reference of the play
// modes (single shot, slice, beat slice, wavetable; the note tables are 2^((note - 60) / 12), the wavetable
// ones that times window x 523.2511306 / 44100). What is checked is what the voices put out when the instrument is triggered at the note that plays it at
// its own pitch, through the real loader (app::OgSampleLoader) and the real voices, against that reference: correlation above 0.99, the RMS ratio equal
// to the instrument's gain (the 1.2+ volume law, smoothing.h) within 2 %, and no sample further than a few LSB.
//
// Two runs share it: a synthetic project made here (always runs: CI keeps the root cause covered) and a v1.9.0 Workspace from the optional local fixtures/ directory
// (O3_FIXTURES_DIR names its `sd` directory; skipped with a message when the variable is unset or has no such workspace; only counts are printed, nothing of its content). Host only (it makes files).
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/og_folder_check.h"
#include "support/static_slot.h"

#include <o3os/app/og_folder.h>
#include <o3os/app/og_samples.h>
#include <o3os/audio/voices.h>
#include <o3os/formats/crc32.h>
#include <o3os/hal/storage.h>
#include <o3os/platform/host/dir_storage.h>

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace hs = o3os::hal::storage;
using o3os::app::OgFolderLoader;
using o3os::app::OgSampleLoader;
using o3os::audio::VoicePool;
using o3os::platform::host::DirStorage;
using namespace o3os::test::og;

namespace {

constexpr std::uint32_t kBlk = VoicePool::kBlock;
constexpr std::uint32_t kSkip = 64;             // the attack ramp (32 samples) and a margin
constexpr std::uint32_t kMaxCompare = 1u << 17;  // samples compared of one instrument: 3 s
constexpr double kPi = 3.14159265358979323846;
constexpr double kC5OverRate = 523.2511306011972 / 44100.0;  // the wavetable pitch constant: note i of a window = notes[i] x window x this

struct TempDir {
  fs::path path;
  TempDir() {
    static unsigned counter = 0;
    path = fs::temp_directory_path() / ("o3os_sample_fidelity_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

void write_file(const fs::path& root, const std::string& rel, const Bytes& bytes) {
  const fs::path p = root / rel;
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

Bytes read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// ---- The independent decoder and the instrument's own words ---------------------------------------------------------------------------------

std::uint32_t rd16(const Bytes& b, std::size_t at) { return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8); }
std::uint32_t rd32(const Bytes& b, std::size_t at) { return rd16(b, at) | (rd16(b, at + 2) << 16); }

// A minimal RIFF walk: chunk id, size, a pad byte after an odd size; "fmt " must say PCM, mono, 44.1 kHz, 16 bit; the samples are the "data" chunk.
bool decode_wav(const Bytes& b, std::vector<std::int16_t>& pcm) {
  if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0) {
    return false;
  }
  bool fmt_ok = false;
  std::size_t at = 12;
  while (at + 8 <= b.size()) {
    const std::uint32_t size = rd32(b, at + 4);
    const std::size_t body = at + 8;
    if (std::memcmp(b.data() + at, "fmt ", 4) == 0 && body + 16 <= b.size()) {
      fmt_ok = rd16(b, body) == 1 && rd16(b, body + 2) == 1 && rd32(b, body + 4) == 44100 && rd16(b, body + 14) == 16;
    } else if (std::memcmp(b.data() + at, "data", 4) == 0) {
      if (!fmt_ok || body + size > b.size()) {
        return false;
      }
      pcm.resize(size / 2u);
      for (std::size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = static_cast<std::int16_t>(rd16(b, body + 2u * i));
      }
      return true;
    }
    at = body + size + (size & 1u);
  }
  return false;
}

struct Raw {  // the fields of a .pti that decide how it plays (docs/formats/og.md section 5, file offsets)
  unsigned mode = 0;
  unsigned start = 0, loop1 = 0, loop2 = 0, end = 0;
  int tune = 0, fine = 0;
  unsigned volume = 0;
  unsigned slice_count = 0, selected = 0;
  unsigned slices[48] = {};
  unsigned wt_size = 0;
  unsigned wt_window = 0;
};

Raw raw_of(const Bytes& pti) {
  Raw r;
  r.mode = pti[76];
  r.start = rd16(pti, 78);
  r.loop1 = rd16(pti, 80);
  r.loop2 = rd16(pti, 82);
  r.end = rd16(pti, 84);
  r.tune = static_cast<std::int8_t>(pti[270]);
  r.fine = static_cast<std::int8_t>(pti[271]);
  r.volume = pti[272];
  r.slice_count = pti[376] > 48u ? 48u : pti[376];
  r.selected = pti[377];
  for (unsigned i = 0; i < 48; ++i) {
    r.slices[i] = rd16(pti, 280 + 2u * i);
  }
  r.wt_size = rd16(pti, 64);
  r.wt_window = rd32(pti, 88);
  return r;
}

// ---- The reference: the play functions written out again, in doubles ----------------------------------------------------------------------------

// The position of a point in samples: the float product point x (length / 65535), truncated.
std::uint32_t point_sample(unsigned point, std::size_t frames) {
  return static_cast<std::uint32_t>(static_cast<float>(point) * (static_cast<float>(frames) / 65535.0f));
}

double semitone(int note_index) { return std::pow(2.0, (note_index - 60) / 12.0); }  // notes[i] / notes[60]

// notes[note + tune], moved `fine` hundredths of the way to the neighbouring entry (the fine tune law), the table running 0..119.
double og_pitch(int note, int tune, int fine) {
  int n = note + tune;
  n = n < 0 ? 0 : (n > 119 ? 119 : n);
  const double base = semitone(n);
  if (fine > 0 && n + 1 <= 119) {
    return base + fine * (semitone(n + 1) - base) / 100.0;
  }
  if (fine < 0 && n - 1 >= 0) {
    return base + (-fine) * (semitone(n - 1) - base) / 100.0;
  }
  return base;
}

struct Expect {
  std::vector<double> y;  // unit gain, the samples a note makes at 44.1 kHz until it ends (a one shot) or for `want` samples (a loop, an oscillator)
  bool ok = false;
};

// `window` of `pcm`: [first, first + len), read at `inc` per output sample with linear interpolation, holding nothing past the end (the note ends).
void play_window(const std::vector<std::int16_t>& pcm, std::uint32_t first, std::uint32_t len, double inc, bool loop, std::size_t want, std::vector<double>& y) {
  double pos = 0.0;
  while (y.size() < want) {
    if (pos >= static_cast<double>(len)) {
      if (!loop) {
        break;
      }
      pos = std::fmod(pos, static_cast<double>(len));
    }
    const std::uint32_t i = static_cast<std::uint32_t>(pos);
    const double f = pos - static_cast<double>(i);
    const std::uint32_t j = i + 1u < len ? i + 1u : (loop ? 0u : i);
    y.push_back(pcm[first + i] + f * (pcm[first + j] - pcm[first + i]));
    pos += inc;
  }
}

Expect expect_note(const Raw& r, const std::vector<std::int16_t>& pcm, unsigned note, std::size_t want) {
  Expect e;
  const std::size_t frames = pcm.size();
  switch (r.mode) {
    case 0:  // single shot: [start, end)
    case 1: {  // forward loop: [start, end), the loop inside it
      const std::uint32_t s = point_sample(r.start, frames);
      const std::uint32_t t = point_sample(r.end, frames);
      if (r.start >= r.end || s >= t || t > frames) {
        return e;
      }
      const double inc = og_pitch(static_cast<int>(note), r.tune, r.fine);
      if (r.mode == 1) {
        const std::uint32_t l1 = point_sample(r.loop1, frames);
        const std::uint32_t l2 = point_sample(r.loop2, frames);
        if (l1 >= s && l1 < l2 && l2 <= t) {  // the loop of the window: it plays [s, l2) and then [l1, l2) for ever
          double pos = 0.0;
          const double lens = static_cast<double>(l2 - s), loops = static_cast<double>(l1 - s);
          while (e.y.size() < want) {
            if (pos >= lens) {
              pos = loops + std::fmod(pos - loops, lens - loops);
            }
            const std::uint32_t i = static_cast<std::uint32_t>(pos);
            const std::uint32_t j = i + 1u < static_cast<std::uint32_t>(lens) ? i + 1u : static_cast<std::uint32_t>(loops);
            const double f = pos - static_cast<double>(i);
            e.y.push_back(pcm[s + i] + f * (pcm[s + j] - pcm[s + i]));
            pos += inc;
          }
          e.ok = true;
          return e;
        }
      }
      play_window(pcm, s, t - s, inc, false, want, e.y);
      break;
    }
    case 4:    // slice: the selected slice, the pitch of the note
    case 5: {  // beat slice: the slice the note names (note - 24, clamped), the pitch of note 60
      unsigned k = r.selected;
      unsigned pitch_note = note;
      if (r.mode == 5) {
        pitch_note = 60;
        k = note < 24u ? 0u : note - 24u;
        k = r.slice_count == 0 ? 0u : (k > r.slice_count - 1u ? r.slice_count - 1u : k);
      }
      const unsigned from = r.slice_count > 0 ? r.slices[k] : 0u;
      const unsigned to = (r.slice_count > 1 && k != r.slice_count - 1u && k + 1u < 48u) ? r.slices[k + 1] : 65535u;
      std::uint32_t s = point_sample(from, frames);
      std::uint32_t t = point_sample(to, frames);
      t = to == 65535u ? static_cast<std::uint32_t>(frames) : t;
      if (t < s) {
        t = static_cast<std::uint32_t>(frames);  // an end before the start: the slice runs to the end of the sample (this OS's rule)
      }
      if (s >= frames || t == s) {
        return e;
      }
      play_window(pcm, s, t - s, og_pitch(static_cast<int>(pitch_note), r.tune, r.fine), false, want, e.y);
      break;
    }
    case 6: {  // wavetable: one window of wt_size frames as an oscillator
      const bool valid = r.wt_size >= 32 && r.wt_size <= 2048 && (r.wt_size & (r.wt_size - 1u)) == 0u;
      const std::uint32_t n = valid ? r.wt_size : 2048u;  // any other size leaves the 2048 table
      const std::uint32_t whole = static_cast<std::uint32_t>(frames) - static_cast<std::uint32_t>(frames) % n;
      if (whole < n) {
        return e;
      }
      std::uint64_t pos = static_cast<std::uint64_t>(n) * r.wt_window;
      pos = pos > whole - n ? whole - n : pos;
      play_window(pcm, static_cast<std::uint32_t>(pos), n, og_pitch(static_cast<int>(note), r.tune, r.fine) * n * kC5OverRate, true, want, e.y);
      break;
    }
    default:
      return e;  // backward, ping-pong, granular: not played yet
  }
  e.ok = !e.y.empty();
  return e;
}

// ---- Rendering through the real voices -------------------------------------------------------------------------------------------------------

bool g_neutral = true;  // render_note plays the instrument without its modulators (false: the test that shows the modulators matter)

std::vector<std::int16_t> render_note(VoicePool& v, unsigned note, unsigned slot, std::size_t want) {
  std::vector<std::int16_t> out;
  v.cut(0);
  for (unsigned i = 0; i < 4; ++i) {  // the cut's residual dies away
    v.begin_block();
    v.render(0, kBlk);
    v.end_block();
  }
  // The amp envelope (OS-007k) has its own tests (audio_voices_test, og_samples_test); this one holds the sample's playback (window, pitch, slice, gain) against
  // the reference, which has no envelope, so the voices' copy of the instrument plays without it. A fixture instrument's envelope would shape every note.
  // The same for the filter (OS-007l: audio_svf_test) and for the four modulators (OS-MOD, OS-GRAN: the finetune, pan, amp LFO and wavetable position ones; audio_voices_test): the
  // reference is the sample as recorded, a finetune LFO is a vibrato and a wavetable position one moves the window. A .pti's modulator that is on (the fixtures' 2, 6 and 16 have one)
  // would otherwise be held against a reference that has no modulation.
  o3os::audio::SampleDesc d = v.sample(slot);
  // The same for the glide, the overdrive and the bit depth (OS-IFX1): the fixtures' instruments hold values in those fields (the synthetic head sets glide 123, overdrive 65),
  // and the reference plays the sample alone.
  const auto live = [](const o3os::audio::FilterMod& m) { return m.kind != o3os::audio::kFmodNone; };
  if (g_neutral && (d.env.enable || d.filter.enable || d.glide != 0u || d.overdrive != 0u || d.bit_depth != 16u || live(d.pan_mod) || live(d.tune_mod) || live(d.amp_mod) ||
                    live(d.pos_mod))) {
    d.env.enable = false;
    d.filter.enable = false;
    d.glide = 0;
    d.overdrive = 0;
    d.bit_depth = 16;
    d.pan_mod.kind = d.tune_mod.kind = d.amp_mod.kind = d.pos_mod.kind = o3os::audio::kFmodNone;
    v.set_sample(slot, d);
  }
  v.begin_block();
  v.note_on(0, note, slot, o3os::audio::kVelocityDefault);
  v.render(0, kBlk);
  v.end_block();
  for (;;) {
    const std::int16_t* f = v.feed(0);
    for (std::uint32_t k = 0; k < kBlk; ++k) {
      out.push_back(f != nullptr ? f[k] : std::int16_t{0});
    }
    if (out.size() >= want || !v.track_playing(0)) {
      break;
    }
    v.begin_block();
    v.render(0, kBlk);
    v.end_block();
  }
  return out;
}

struct Score {
  bool ran = false;
  double corr = 0.0;
  double rms_ratio = 0.0;  // out / reference, to be the voices' gain
  double max_err = 0.0;
  std::size_t n = 0;
};

Score compare(const Expect& e, const std::vector<std::int16_t>& out, double gain) {
  Score s;
  const std::size_t n = std::min(e.y.size(), out.size());
  if (n <= kSkip + 1000u) {
    return s;
  }
  double sxy = 0, sxx = 0, syy = 0;
  for (std::size_t i = kSkip; i < n; ++i) {
    const double x = static_cast<double>(out[i]), y = std::min(32767.0, std::max(-32768.0, e.y[i] * gain));  // the voices clip at the rails
    sxy += x * y;
    sxx += x * x;
    syy += y * y;
    s.max_err = std::max(s.max_err, std::fabs(x - y));
  }
  s.ran = true;
  s.n = n - kSkip;
  s.corr = (sxx > 0 && syy > 0) ? sxy / std::sqrt(sxx * syy) : 0.0;
  s.rms_ratio = syy > 0 ? std::sqrt(sxx / syy) : 0.0;
  return s;
}

// The instrument's gain in a 1.2+ project (OS-007i; the fixtures are version 16), in double and from the law's own words rather than from the loader's code:
// level 0 silent, level 1 -24 dB, then 0.48 dB x (level - 50) up to +24 dB at 100 (a 101-entry law, our model, kept for compatibility: audio/smoothing.h, ADR 0013 PAR-023 level curve).
double bound_gain(unsigned volume) {
  const unsigned v = volume > 100 ? 100 : volume;
  return v == 0 ? 0.0 : std::pow(10.0, (v == 1 ? -24.0 : 0.48 * (static_cast<double>(v) - 50.0)) / 20.0);
}

struct Verdict {
  bool expected = false;  // the reference has this note
  bool ran = false;       // enough samples to compare
  bool ok = false;
  Score score;
};

Verdict judge(VoicePool& v, unsigned slot, const Raw& r, const std::vector<std::int16_t>& pcm, unsigned note) {
  Verdict out;
  const std::size_t want = kMaxCompare + kSkip;
  const Expect e = expect_note(r, pcm, note, want);
  if (!e.ok) {
    return out;  // the reference plays nothing (or the voices have no such mode yet): nothing to compare
  }
  out.expected = true;
  const double gain = bound_gain(r.volume);
  out.score = compare(e, render_note(v, note, slot, want), gain);
  out.ran = out.score.ran;
  const Score& s = out.score;
  out.ok = s.ran && s.corr > 0.99 && (gain == 0.0 || std::fabs(s.rms_ratio - 1.0) < 0.02) && s.max_err < 8.0 + 0.01 * 32768.0 * gain;
  return out;
}

double g_worst_corr = 1.0;       // over every note compared, for the line the fixture run prints
double g_worst_rms_error = 0.0;  // |rms ratio - 1|

// One note of one instrument against the reference. True: it agrees.
bool note_agrees(VoicePool& v, unsigned slot, const Raw& r, const std::vector<std::int16_t>& pcm, unsigned note, const char* what) {
  const Verdict d = judge(v, slot, r, pcm, note);
  if (!d.expected) {
    return true;
  }
  if (!d.ran) {
    CHECK_MESSAGE(false, what << " slot " << slot << " note " << note << ": too short to compare");
    return false;
  }
  const Score& s = d.score;
  g_worst_corr = std::min(g_worst_corr, s.corr);
  g_worst_rms_error = std::max(g_worst_rms_error, std::fabs(s.rms_ratio - 1.0));
  CHECK_MESSAGE(d.ok, what << " slot " << slot << " mode " << r.mode << " note " << note << ": corr " << s.corr << " rms ratio " << s.rms_ratio << " max err " << s.max_err << " over "
                           << s.n << " samples");
  return d.ok;
}

// Every instrument of the project, each at the notes that play it at its own pitch: the root (60) for the modes that follow the note's pitch, one note of
// each slice for a beat slice (24 + k: the pitch is the root's), the root for a wavetable and an octave below and above it. `pcms[slot]` is the audio the
// OG's instrument holds, `raws[slot]` its words. Counts: how many instruments were checked and how many agreed.
struct Tally {
  unsigned checked = 0, agreed = 0;
  unsigned by_mode[8] = {};
};

void check_all(VoicePool& v, const std::vector<std::vector<std::int16_t>>& pcms, const std::vector<Raw>& raws, const std::vector<bool>& present, Tally& t) {
  for (unsigned slot = 0; slot < pcms.size(); ++slot) {
    if (!present[slot]) {
      continue;
    }
    const Raw& r = raws[slot];
    bool ok = true;
    bool any = false;
    const auto one = [&](unsigned note, const char* what) {
      ok = note_agrees(v, slot, r, pcms[slot], note, what) && ok;
      any = true;
    };
    switch (r.mode) {
      case 5: {
        const unsigned count = r.slice_count == 0 ? 1u : r.slice_count;
        one(24, "beat slice, first slice");
        one(24 + count - 1u, "beat slice, last slice");
        if (count > 2) {
          one(24 + count / 2u, "beat slice, a middle slice");
        }
        one(12, "beat slice, a note below the root: the first slice");
        one(100, "beat slice, a note above the last slice: the last slice");
        break;
      }
      case 6:
        one(60, "wavetable, C-5");
        one(48, "wavetable, an octave down");
        one(72, "wavetable, an octave up");
        break;
      default:
        one(60, "root");
        break;
    }
    if (any) {
      ++t.checked;
      ++t.by_mode[r.mode & 7u];
      t.agreed += ok ? 1u : 0u;
    }
  }
}

// ---- A project folder's instruments through the real loader ---------------------------------------------------------------------------------

bool load_folder(DirStorage& card, const char* folder, std::vector<std::int16_t>& arena, VoicePool& pool, OgSampleLoader& sl, OgFolderLoader& pl, o3os::model::ModelArena& model) {
  model.project.reset();
  if (!pl.begin(card, folder, model) || run_loader(pl, 16 * 1024) != OgFolderLoader::State::Done) {
    return false;
  }
  for (unsigned i = 0; i < o3os::audio::kSampleSlots; ++i) {
    pool.set_sample(i, o3os::audio::SampleDesc{});
  }
  if (!sl.begin(card, pl.folder(), model.project, pl.pti_slots(), pool, arena.data(), static_cast<std::uint32_t>(arena.size()),
                o3os::app::instrument_law(pl.report().version))) {
    return false;
  }
  for (unsigned n = 0; n < 1000000u && sl.state() == OgSampleLoader::State::Running; ++n) {
    (void)sl.step(16 * 1024);
  }
  return sl.state() == OgSampleLoader::State::Done;
}

VoicePool& pool() {
  static o3os::test::StaticSlot<VoicePool> slot;
  return slot.make();
}
OgSampleLoader& sample_loader() {
  static OgSampleLoader sl;
  return sl;
}

// ---- The synthetic project ------------------------------------------------------------------------------------------------------------------

constexpr unsigned kBreakFrames = 40000;  // four segments of 10,000 frames, each its own tone
constexpr unsigned kWtWindow = 256;
constexpr unsigned kWtWindows = 8;

// A drum-break stand-in: segment s is a sine of 330 x (s + 1) Hz with a decay, so that every slice is told apart by its pitch and by its place.
std::vector<std::int16_t> break_pcm() {
  std::vector<std::int16_t> p(kBreakFrames);
  for (unsigned i = 0; i < kBreakFrames; ++i) {
    const unsigned seg = i / 10000u, k = i % 10000u;
    const double env = 1.0 - 0.7 * (static_cast<double>(k) / 10000.0);
    p[i] = static_cast<std::int16_t>(std::lround(14000.0 * env * std::sin(2.0 * kPi * 330.0 * (seg + 1) * i / 44100.0)));
  }
  return p;
}

// A wavetable: window w is one cycle holding harmonics 1..w + 1 (a sine at w = 0, richer as it goes).
std::vector<std::int16_t> table_pcm() {
  std::vector<std::int16_t> p(kWtWindow * kWtWindows);
  for (unsigned w = 0; w < kWtWindows; ++w) {
    for (unsigned i = 0; i < kWtWindow; ++i) {
      double s = 0.0;
      for (unsigned h = 1; h <= w + 1; ++h) {
        s += std::sin(2.0 * kPi * h * i / kWtWindow) / h;
      }
      p[w * kWtWindow + i] = static_cast<std::int16_t>(std::lround(9000.0 * s));
    }
  }
  return p;
}

Bytes pcm_bytes(const std::vector<std::int16_t>& p) {
  Bytes b;
  for (const std::int16_t s : p) {
    b.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(s) & 255u));
    b.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(s) >> 8));
  }
  return b;
}

struct PtiSpec {
  unsigned mode = 0;
  unsigned start = 0, loop1 = 0, loop2 = 65535, end = 65535;
  int tune = 0, fine = 0;
  unsigned volume = 50;
  std::vector<unsigned> slices;
  unsigned selected = 0;
  unsigned wt_size = 2048;
  unsigned wt_window = 0;
  bool tune_lfo = false;  // OS-SLICEFID: the finetune modulator (slot 5) is an LFO, on
  bool pos_lfo = false;   // ... and the wavetable position modulator (slot 3)
};

// A .pti: the fixture head with its sound fields set at the offsets of og.md section 5, the CRC made again, then the PCM.
Bytes pti_file(const PtiSpec& s, const std::vector<std::int16_t>& pcm) {
  InstrumentSpec spec;
  spec.pti = true;
  spec.frames = 0;
  spec.file_name = "Synthetic";
  Bytes b = instrument_bytes(spec);  // 392 bytes of head, no PCM
  p32(b, 60, static_cast<std::uint32_t>(pcm.size()));
  b[76] = static_cast<std::uint8_t>(s.mode);
  p16(b, 78, s.start);
  p16(b, 80, s.loop1);
  p16(b, 82, s.loop2);
  p16(b, 84, s.end);
  b[270] = static_cast<std::uint8_t>(s.tune);
  b[271] = static_cast<std::uint8_t>(s.fine);
  b[272] = static_cast<std::uint8_t>(s.volume);
  for (std::size_t i = 0; i < 48; ++i) {
    p16(b, 280 + 2 * i, i < s.slices.size() ? s.slices[i] : 0u);
  }
  b[376] = static_cast<std::uint8_t>(s.slices.size());
  b[377] = static_cast<std::uint8_t>(s.selected);
  p16(b, 64, s.wt_size);
  p32(b, 88, s.wt_window);
  const auto lfo_slot = [&b](unsigned slot, unsigned speed, float amount) {  // modulator `slot` on, as an LFO (loop byte), triangle at `speed`
    b[92 + 20 * slot + 18] = 1;
    b[92 + 20 * slot + 19] = 1;
    b[212 + 8 * slot] = 2;
    b[212 + 8 * slot + 1] = static_cast<std::uint8_t>(speed);
    pf(b, 212 + 8 * slot + 4, amount);
  };
  if (s.tune_lfo) {
    lfo_slot(5, 10, 1.0f);
  }
  if (s.pos_lfo) {
    lfo_slot(3, 14, 0.3f);
  }
  p32(b, 388, o3os::formats::crc32(b.data(), 388));
  const Bytes audio = pcm_bytes(pcm);
  b.insert(b.end(), audio.begin(), audio.end());
  return b;
}

}  // namespace

TEST_CASE("og sample fidelity: beat slice, slice, wavetable and one shot instruments play as recorded (synthetic, no fixture)") {
  TempDir tmp;
  const std::vector<std::int16_t> brk = break_pcm();
  const std::vector<std::int16_t> table = table_pcm();
  std::vector<PtiSpec> specs(5);
  std::vector<std::vector<std::int16_t>> pcms(5);
  // slot 0: beat slice, four slices at the quarters of the sample
  specs[0].mode = 5;
  specs[0].slices = {0, 16384, 32768, 49152};
  specs[0].volume = 100;
  pcms[0] = brk;
  // slot 1: wavetable, 256-frame windows, window 3
  specs[1].mode = 6;
  specs[1].wt_size = kWtWindow;
  specs[1].wt_window = 3;
  specs[1].pos_lfo = true;  // OS-SLICEFID: a wavetable position LFO is on (the fixtures' 6th instrument has one)
  specs[1].volume = 80;
  pcms[1] = table;
  // slot 2: slice mode, the third slice (the pitch follows the note)
  specs[2].mode = 4;
  specs[2].slices = {0, 16384, 32768, 49152};
  specs[2].selected = 2;
  specs[2].volume = 60;
  pcms[2] = brk;
  // slot 3: one shot, the whole sample, volume 50 (our default), tuned up two semitones and 30 hundredths
  specs[3].mode = 0;
  specs[3].tune = 2;
  specs[3].fine = 30;
  specs[3].tune_lfo = true;  // a finetune LFO is on (the fixtures' 2nd and 16th instruments have a finetune modulator)
  pcms[3] = brk;
  // slot 4: beat slice with no slices: the whole sample at the root's pitch, a note above or below changes nothing
  specs[4].mode = 5;
  specs[4].volume = 70;
  pcms[4] = brk;

  write_file(tmp.path, "Workspace/project.mt", project_bytes(ProjectSpec{}));
  std::vector<Raw> raws;
  std::vector<bool> present(5, true);
  for (unsigned i = 0; i < 5; ++i) {
    const Bytes f = pti_file(specs[i], pcms[i]);
    write_file(tmp.path, std::string("Workspace/instruments/instrument_0") + std::to_string(i + 1) + ".pti", f);
    raws.push_back(raw_of(f));
  }

  DirStorage card(tmp.path.string());
  NEED(card.mount() == hs::Status::Ok);
  std::vector<std::int16_t> arena(400000, std::int16_t{0});
  VoicePool& v = pool();
  OgSampleLoader& sl = sample_loader();
  NEED(load_folder(card, "Workspace", arena, v, sl, loader(), o3os::test::og::arena(0)));
  CHECK(sl.report().loaded == 5u);
  CHECK(sl.report().missing == 0u);
  CHECK(sl.report().approximated == 0u);  // none of these modes is an approximation any more

  // OS-SLICEFID: the modulators are in the voices' descriptors (the loader binds them) and the check below plays without them, as it plays without the envelope and the filter.
  const o3os::audio::SampleDesc wt_live = v.sample(1), one_shot_live = v.sample(3);
  CHECK(wt_live.pos_mod.kind == o3os::audio::kFmodLfo);
  CHECK(one_shot_live.tune_mod.kind == o3os::audio::kFmodLfo);

  Tally t;
  check_all(v, pcms, raws, present, t);
  CHECK(t.checked == 5u);
  CHECK(t.agreed == 5u);
  CHECK(t.by_mode[5] == 2u);
  CHECK(t.by_mode[6] == 1u);
  CHECK(t.by_mode[4] == 1u);
  CHECK(t.by_mode[0] == 1u);

  // The oracle can tell that too: with the modulators left on, the wavetable (its window moves) and the tuned one shot (a vibrato) do not agree with the reference. Before the
  // modulators were set aside here, the fixtures' instruments that have one failed (13 of 16 agreed).
  g_neutral = false;
  for (const unsigned slot : {1u, 3u}) {
    v.set_sample(slot, slot == 1u ? wt_live : one_shot_live);
    const Verdict d = judge(v, slot, raws[slot], pcms[slot], 60u);
    CHECK_MESSAGE(d.expected, "slot " << slot << ": nothing expected");
    CHECK_MESSAGE(!d.ok, "slot " << slot << ": agrees with its modulator on (corr " << d.score.corr << ")");
  }
  g_neutral = true;

  // The oracle can tell: the bindings of before OS-007h (every instrument a plain window of the whole sample, one shot at the note's pitch) do not agree with it,
  // for the beat slice at its first slice (the only note where the window starts right but the pitch of note 24 is three octaves low), for the one
  // that is a slice mode's selected slice, and for the wavetable (the whole stack of cycles one shot).
  for (const unsigned slot : {0u, 1u, 2u}) {
    const o3os::audio::SampleDesc now = v.sample(slot);
    o3os::audio::SampleDesc old;
    old.data = now.data - now.offset;  // the start of the instrument's PCM, wherever the loader put it
    old.length = static_cast<std::uint32_t>(pcms[slot].size());
    old.gain = now.gain;
    v.set_sample(slot, old);
    const unsigned note = raws[slot].mode == 5u ? 24u : 60u;
    const Verdict d = judge(v, slot, raws[slot], pcms[slot], note);
    CHECK_MESSAGE(d.expected, "slot " << slot << ": nothing expected");
    CHECK_MESSAGE(!d.ok, "slot " << slot << " mode " << raws[slot].mode << ": a plain window agrees with the reference (corr " << d.score.corr << ")");
  }
}

TEST_CASE("og sample fidelity: a v1.9.0 Workspace (fixtures: O3_FIXTURES_DIR, skipped when unset): every instrument against an independent decode of its WAV") {
  // The fixtures are named by O3_FIXTURES_DIR (the `sd` directory) and nothing else: no search upwards from this file (a build tree may sit inside another checkout, and a CI run must not depend
  // on what happens to be above it; ci.sh points the variable at a directory that does not exist).
  const char* env = std::getenv("O3_FIXTURES_DIR");
  if (env == nullptr || env[0] == '\0') {
    std::printf("og sample fidelity: O3_FIXTURES_DIR is not set, fixture run skipped (set it to the fixtures/sd directory)\n");
    return;
  }
  const fs::path root(env);
  const fs::path ws = root / "v1.9.0-workspace";
  if (!fs::is_directory(ws)) {
    std::printf("og sample fidelity: %s has no v1.9.0-workspace, fixture run skipped\n", env);
    return;
  }
  DirStorage card(root.string());
  NEED(card.mount() == hs::Status::Ok);
  std::vector<std::int16_t> arena(5400000, std::int16_t{0});
  VoicePool& v = pool();
  OgSampleLoader& sl = sample_loader();
  NEED(load_folder(card, "v1.9.0-workspace", arena, v, sl, loader(), o3os::test::og::arena(0)));

  std::vector<std::vector<std::int16_t>> pcms(16);
  std::vector<Raw> raws(16);
  std::vector<bool> present(16, false);
  unsigned decoded = 0;
  for (unsigned slot = 0; slot < 16; ++slot) {
    char name[64];
    std::snprintf(name, sizeof name, "instruments/instrument_%02u.pti", slot + 1);
    const Bytes pti = read_file(ws / name);
    std::snprintf(name, sizeof name, "samples/instr%02u.wav", slot + 1);
    const Bytes wav = read_file(ws / name);
    if (pti.size() > 392 && decode_wav(wav, pcms[slot])) {
      raws[slot] = raw_of(pti);
      present[slot] = true;
      ++decoded;
    }
  }
  CHECK(decoded > 0u);
  CHECK(sl.report().loaded == decoded);

  Tally t;
  check_all(v, pcms, raws, present, t);
  std::printf("og sample fidelity: %u of %u instruments agree with the independent decode (plain/loop %u, slice %u, beat slice %u, wavetable %u); worst correlation %.5f, worst RMS ratio error %.5f\n",
              t.agreed, t.checked, t.by_mode[0] + t.by_mode[1] + t.by_mode[2] + t.by_mode[3], t.by_mode[4], t.by_mode[5], t.by_mode[6], g_worst_corr, g_worst_rms_error);
  CHECK(t.checked == decoded);
  CHECK(t.agreed == t.checked);
}
