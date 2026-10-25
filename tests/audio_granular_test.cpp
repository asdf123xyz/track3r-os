// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-GRAN: the granular voice (play mode 7; audio/voices.h GRANULAR). The oracles are written here, independently of the voice, from the grain model that audio/voices.h GRANULAR documents:
// the grain's geometry (u16 position units, the float point conversion), the three loop types (forward, backward, ping-pong) as the
// real-valued position of a reflecting or wrapping read, the amplitude shapes by the integer map of the position onto a 1000-entry table, and the table's own values (half sine,
// triangle) against the formulas.
// Also: no read ever leaves the sample (a poisoned guard around it), the output does not depend on how the block is cut into spans, a granular note's life (attack, release, retrigger),
// and the descriptor's sanitising. Host and Cortex-M4F.
#include <doctest/doctest.h>

#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include "support/static_slot.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

// doctest here has no exceptions: REQUIRE is not available, so a failed precondition is a failed check and the test case returns.
#define REQ(cond) \
  do {            \
    if (!(cond)) { \
      FAIL_CHECK("precondition failed: " #cond); \
      return;     \
    }             \
  } while (0)

namespace {

namespace audio = o3os::audio;
namespace haudio = o3os::hal::audio;
using audio::SampleDesc;
using audio::VoicePool;

constexpr std::uint32_t kBlk = haudio::kBlock;
constexpr std::uint32_t kGuard = 4096;     // poisoned samples on each side of the sample
constexpr std::uint32_t kMaxLen = 65535;
constexpr std::int16_t kPoison = 31000;

std::int16_t g_buf[kGuard + kMaxLen + kGuard];

VoicePool& pool_a() {
  static o3os::test::StaticSlot<VoicePool> slot;
  return slot.make();
}
VoicePool& pool_b() {
  static o3os::test::StaticSlot<VoicePool> slot;
  return slot.make();
}

enum class Fill { Wave, Ramp, Dc };

// The sample of `len` frames at g_buf + kGuard, the rest poison. Wave: a sum of two sines of amplitude 5000 + 3000 (never near the poison); Ramp: i; Dc: 20000.
const std::int16_t* fill(std::uint32_t len, Fill kind) {
  for (std::uint32_t i = 0; i < kGuard + kMaxLen + kGuard; ++i) {
    g_buf[i] = kPoison;
  }
  for (std::uint32_t i = 0; i < len; ++i) {
    double v;
    switch (kind) {
      case Fill::Ramp: v = static_cast<double>(i % 30000u); break;
      case Fill::Dc: v = 20000.0; break;
      default: v = 5000.0 * std::sin(0.071 * static_cast<double>(i)) + 3000.0 * std::sin(0.0113 * static_cast<double>(i) + 1.0); break;
    }
    g_buf[kGuard + i] = static_cast<std::int16_t>(std::lround(v));
  }
  return g_buf + kGuard;
}

struct Cfg {
  std::uint32_t len = kMaxLen;
  unsigned gr_len = 300;
  unsigned pos = 20000;
  unsigned shape = 0;  // 0 square, 1 triangle, 2 gauss
  unsigned type = 0;   // 0 forward, 1 backward, 2 ping-pong
  unsigned note = 60;
  Fill kind = Fill::Wave;
};

// The descriptor of `c` over a sample already in place (fill() was called for c.len).
SampleDesc desc_over(const std::int16_t* pcm, const Cfg& c) {
  SampleDesc d{pcm, c.len, 0, 0, 65536};
  d.granular = true;
  d.gr_len = static_cast<std::uint16_t>(c.gr_len);
  d.gr_pos = static_cast<std::uint16_t>(c.pos);
  d.gr_shape = static_cast<std::uint8_t>(c.shape);
  d.loop_mode = static_cast<std::uint8_t>(c.type);
  return d;
}

SampleDesc make_desc(const Cfg& c) { return desc_over(fill(c.len, c.kind), c); }

// ---- The oracle ---------------------------------------------------------------------------------------------------------------------------------------------------------
struct Geom {
  int start = 0, avail = 0, l1 = 0, l2 = 0;
};

// The granular geometry (audio/voices.h GRANULAR, grain_geom), typed again independently: u16 units, u16 points, float conversion; held so that the grain is at least one sample long and inside the sample.
Geom ref_geom(std::uint32_t len, unsigned gr_len, unsigned pos) {
  const std::uint16_t units = static_cast<std::uint16_t>(gr_len * 65535u / len);
  const int half = units / 2;
  const int down = static_cast<int>(pos) - half;
  const int up = static_cast<int>(pos) + half;
  const std::uint16_t s = down > 0 ? static_cast<std::uint16_t>(down) : 0;
  const std::uint16_t e = up < 65535 ? static_cast<std::uint16_t>(up) : 65535;
  const std::uint16_t p1 = static_cast<std::uint16_t>(s + 1);
  const std::uint16_t p2 = static_cast<std::uint16_t>(e - 1);
  const float r = static_cast<float>(len) / 65535.0f;
  const auto at = [r](std::uint16_t g) { return static_cast<int>(static_cast<float>(g) * r); };
  Geom g;
  g.start = std::min(at(s), static_cast<int>(len) - 1);
  g.avail = static_cast<int>(len) - g.start;
  g.l1 = std::max(at(p1) - g.start, 0);
  g.l2 = std::max(at(p2) - g.start, 0);
  g.l2 = std::max(g.l2, g.l1 + 1);
  return g;
}

// The amplitude table the voice indexes: gain(shape, i) as 16 bits, by the formulas (the half sine and the triangle), 0 at 999.
double table_gain(unsigned shape, int i) {
  if (shape == 0) {
    return 65535.0;
  }
  if (i >= 999) {
    return 0.0;
  }
  if (shape == 1) {
    return std::floor(65535.0 * static_cast<double>(i <= 500 ? i : 1000 - i) / 500.0 + 0.5);
  }
  return std::floor(65535.0 * std::sin(3.14159265358979323846 * static_cast<double>(i) / 1000.0) + 0.5);
}

struct Ref {
  Geom g;
  const std::int16_t* pcm = nullptr;
  unsigned shape = 0, type = 0;
  double inc = 1.0;
  double p = 0.0;
  bool bwd = false;
  std::uint32_t n = 0;  // samples made

  Ref(const Cfg& c, const std::int16_t* data, double step) : g(ref_geom(c.len, c.gr_len, c.pos)), pcm(data), shape(c.shape), type(c.type), inc(step), bwd(c.type == 1) {}

  double at(int i) const { return static_cast<double>(pcm[g.start + (i < 0 ? 0 : (i >= g.avail ? g.avail - 1 : i))]); }

  // The next output sample (the attack ramp of kAttackSamples included: unity gain, no amp envelope).
  double next() {
    const int x = static_cast<int>(std::floor(p));
    const double f = p - static_cast<double>(x);
    const double s = at(x) + (at(x + 1) - at(x)) * f;
    const int xe = (bwd && f > 0.0) ? x + 1 : x;
    const int k = xe - g.l1;
    const int span = g.l2 - g.l1;
    const int idx = k <= 0 ? 0 : (k >= span ? 999 : (k * 999) / span);
    const double amp = n >= audio::kAttackSamples ? 1.0 : static_cast<double>(n) / static_cast<double>(audio::kAttackSamples);
    const double out = s * table_gain(shape, idx) / 65536.0 * amp;
    ++n;
    const double lo = g.l1, hi = g.l2, w = hi - lo;
    if (type == 0) {
      p += inc;
      if (p >= hi) {
        p = lo + std::fmod(p - hi, w);
      }
    } else if (type == 1) {
      p -= inc;
      if (p <= lo) {
        p = hi - std::fmod(lo - p, w);
      }
    } else if (!bwd) {
      p += inc;
      if (p >= hi) {
        p = std::max(2.0 * hi - 1.0 - p, lo);
        bwd = true;
      }
    } else {
      p -= inc;
      if (p <= lo) {
        p = std::min(2.0 * lo + 1.0 - p, hi);
        bwd = false;
      }
    }
    return out;
  }
};

// Renders `samples` samples (rounded up to whole blocks) of track 0 into `out` (the voice's own feed).
void render(VoicePool& p, std::vector<std::int16_t>& out, unsigned samples) {
  for (unsigned b = 0; b < (samples + kBlk - 1u) / kBlk; ++b) {
    p.begin_block();
    p.render(0, kBlk);
    p.end_block();
    const std::int16_t* f = p.feed(0);
    for (std::uint32_t i = 0; i < kBlk; ++i) {
      out.push_back(f != nullptr ? f[i] : std::int16_t{0});
    }
  }
}

double inc_of(unsigned note) { return static_cast<double>(audio::tuned_inc_q32(note, 0, 0)) / 4294967296.0; }

}  // namespace

// ---- The shapes ---------------------------------------------------------------------------------------------------------------------------------------------------------
TEST_CASE("granular: a DC sample under each shape is the table's gain at the integer map of the position, entry by entry") {
  // len 65535: one sample is one position unit, so with gr_len 1002 the grain is 1002 wide and its loop span 1000: the index is floor(k x 999 / 1000), every table entry is reached.
  for (unsigned shape = 0; shape < 3; ++shape) {
    Cfg c;
    c.shape = shape;
    c.gr_len = 1002;
    c.pos = 30000;
    c.kind = Fill::Dc;
    VoicePool& p = pool_a();
    p.set_sample(0, make_desc(c));
    REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
    std::vector<std::int16_t> out;
    render(p, out, 1100);  // the grain's 1000 and the wrap
    const Geom g = ref_geom(c.len, c.gr_len, c.pos);
    REQ(g.l2 - g.l1 == 1000);
    unsigned bad = 0;
    for (int x = static_cast<int>(audio::kAttackSamples); x < g.l2; ++x) {  // past the attack ramp, before the wrap (x is the sample's index: inc 1.0)
      const int k = x - g.l1;
      const int idx = k <= 0 ? 0 : (k * 999) / 1000;
      const int want = (20000 * static_cast<int>(table_gain(shape, idx))) >> 16;
      bad += std::abs(out[static_cast<std::size_t>(x)] - want) > 1 ? 1u : 0u;  // the formulas round where a reference generator may be a unit off
    }
    CHECK_MESSAGE(bad == 0u, "shape " << shape << ": " << bad << " samples off the table");
  }
}

TEST_CASE("granular: the grain's geometry follows the u16 arithmetic of the grain model, at the edges and for a grain longer than the sample") {
  struct Case {
    std::uint32_t len;
    unsigned gr_len, pos;
  };
  const Case cases[] = {{65535, 300, 20000}, {65535, 300, 0}, {65535, 300, 65535}, {1000, 441, 32768}, {300, 441, 32768}, {200000, 44, 65535}, {200000, 44, 0}, {5, 441, 100}, {1, 44, 0}};
  for (const Case& cs : cases) {
    Cfg c;
    c.len = cs.len;
    c.gr_len = cs.gr_len;
    c.pos = cs.pos;
    c.type = 0;
    // a ramp makes the position visible: the first sample past the attack is the grain's start + x (square shape: gain 65535/65536)
    std::vector<std::int16_t> data(cs.len);
    for (std::uint32_t i = 0; i < cs.len; ++i) {
      data[i] = static_cast<std::int16_t>(i % 20000u);
    }
    SampleDesc d{data.data(), cs.len, 0, 0, 65536};
    d.granular = true;
    d.gr_len = static_cast<std::uint16_t>(cs.gr_len);
    d.gr_pos = static_cast<std::uint16_t>(cs.pos);
    VoicePool& p = pool_a();
    p.set_sample(0, d);
    REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
    std::vector<std::int16_t> out;
    render(p, out, 200);
    const Geom g = ref_geom(cs.len, cs.gr_len, cs.pos);
    const int x = 100;  // a sample inside the first block, past the attack, at position x (before any wrap when the loop is long enough; else the reference's wrap decides)
    Ref r(c, data.data(), 1.0);
    double want = 0.0;
    for (int i = 0; i <= x; ++i) {
      want = r.next();
    }
    CHECK_MESSAGE(std::abs(out[static_cast<std::size_t>(x)] - want) <= 3.0, "len " << cs.len << " gr_len " << cs.gr_len << " pos " << cs.pos << ": got " << out[static_cast<std::size_t>(x)] << " want " << want
                                                                                  << " (grain start " << g.start << ", loop " << g.l1 << ".." << g.l2 << ")");
    CHECK(g.l2 > g.l1);
    CHECK(g.start >= 0);
    CHECK(g.start < static_cast<int>(cs.len));
  }
}

// ---- The loop types -----------------------------------------------------------------------------------------------------------------------------------------------------
TEST_CASE("granular: forward, backward and ping-pong grains follow the real-valued oracle sample by sample, across wraps and turns, at several pitches and shapes") {
  struct Case {
    unsigned type, shape, note, gr_len, pos;
  };
  const Case cases[] = {
      {0, 0, 60, 300, 20000}, {0, 2, 67, 300, 20000}, {0, 1, 55, 777, 40000}, {1, 0, 60, 300, 20000}, {1, 2, 64, 500, 1000}, {1, 1, 72, 300, 65535},
      {2, 0, 60, 300, 20000}, {2, 2, 67, 300, 20000}, {2, 1, 50, 500, 30000}, {2, 2, 84, 300, 100},
  };
  for (const Case& cs : cases) {
    Cfg c;
    c.type = cs.type;
    c.shape = cs.shape;
    c.note = cs.note;
    c.gr_len = cs.gr_len;
    c.pos = cs.pos;
    VoicePool& p = pool_a();
    p.set_sample(0, make_desc(c));
    REQ(p.note_on(0, cs.note, 0, audio::kVelocityDefault));
    std::vector<std::int16_t> out;
    render(p, out, 1500);
    Ref r(c, g_buf + kGuard, inc_of(cs.note));
    int worst = 0;
    unsigned at_worst = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
      const double want = r.next();
      const int e = static_cast<int>(std::lround(std::abs(out[i] - want)));
      if (e > worst) {
        worst = e;
        at_worst = static_cast<unsigned>(i);
      }
    }
    CHECK_MESSAGE(worst <= 3, "type " << cs.type << " shape " << cs.shape << " note " << cs.note << " gr_len " << cs.gr_len << " pos " << cs.pos << ": worst error " << worst << " at sample " << at_worst);
  }
}

TEST_CASE("granular: a backward grain starts at the loop's end at once, and a ping-pong grain reaches the end then comes back") {
  Cfg c;
  c.kind = Fill::Ramp;
  c.len = 20000;
  c.gr_len = 400;
  c.pos = 30000;  // len 20000: ratio 0.305; the grain ~400 samples
  c.type = 1;
  VoicePool& p = pool_a();
  p.set_sample(0, make_desc(c));
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  std::vector<std::int16_t> out;
  render(p, out, 400);
  const Geom g = ref_geom(c.len, c.gr_len, c.pos);
  // a ramp: out ~ start + x. After the first sample the voice runs backward from loop2: strictly falling, 1 a sample
  for (int i = 40; i < 200; ++i) {
    CHECK(out[static_cast<std::size_t>(i)] < out[static_cast<std::size_t>(i - 1)]);
  }
  CHECK(out[100] > g.start + g.l1 - 5);
  c.type = 2;
  VoicePool& q = pool_b();
  q.set_sample(0, make_desc(c));
  REQ(q.note_on(0, 60, 0, audio::kVelocityDefault));
  std::vector<std::int16_t> o2;
  render(q, o2, 800);
  bool rose = false, fell = false;
  for (std::size_t i = 40; i + 1 < o2.size(); ++i) {
    rose = rose || o2[i + 1] > o2[i];
    fell = fell || o2[i + 1] < o2[i];
  }
  CHECK(rose);
  CHECK(fell);
}

// ---- Safety -------------------------------------------------------------------------------------------------------------------------------------------------------------
TEST_CASE("granular: no read leaves the sample, whatever the position, length, pitch or type (a poisoned guard around it stays out of the output)") {
  const std::uint32_t lens[] = {1000, 300, 20000, 65535, 2, 7};
  const unsigned positions[] = {0, 1, 32768, 65534, 65535};
  const unsigned gr_lens[] = {44, 441, 44100};
  const unsigned notes[] = {24, 60, 96, 127};
  unsigned bad = 0, runs = 0;
  for (const std::uint32_t len : lens) {
    // The sample depends on the length only, and nothing writes to it: filled and scaled once per length, not for each of its 180 runs (the double
    // sines of fill() made this the slowest QEMU suite). The signal is small so that a poison read (31000) cannot hide: |signal| <= 4000.
    const std::int16_t* pcm = fill(len, Fill::Wave);
    for (std::uint32_t i = 0; i < len; ++i) {
      g_buf[kGuard + i] = static_cast<std::int16_t>(g_buf[kGuard + i] / 3);
    }
    for (const unsigned pos : positions) {
      for (const unsigned gl : gr_lens) {
        for (unsigned type = 0; type < 3; ++type) {
          for (const unsigned note : notes) {
            Cfg c;
            c.len = len;
            c.pos = pos;
            c.gr_len = gl;
            c.type = type;
            c.shape = (len + pos + type) % 3u;
            const SampleDesc d = desc_over(pcm, c);
            VoicePool& p = pool_a();
            p.set_sample(0, d);
            if (!p.note_on(0, note, 0, audio::kVelocityDefault)) {
              continue;
            }
            std::vector<std::int16_t> out;
            render(p, out, 300);
            ++runs;
            for (const std::int16_t v : out) {
              bad += std::abs(v) > 4500 ? 1u : 0u;
            }
          }
        }
      }
    }
  }
  CHECK(runs > 1000u);
  CHECK(bad == 0u);
}

TEST_CASE("granular: the output does not depend on how the block is cut into spans") {
  for (unsigned type = 0; type < 3; ++type) {
    Cfg c;
    c.type = type;
    c.shape = 2;
    c.gr_len = 260;
    VoicePool& a = pool_a();
    VoicePool& b = pool_b();
    a.set_sample(0, make_desc(c));
    b.set_sample(0, make_desc(c));
    REQ(a.note_on(0, 63, 0, audio::kVelocityDefault));
    REQ(b.note_on(0, 63, 0, audio::kVelocityDefault));
    std::vector<std::int16_t> oa, ob;
    render(a, oa, 20 * kBlk);
    for (unsigned blk = 0; blk < 20; ++blk) {
      b.begin_block();
      b.render(0, 13);
      b.render(13, 14);
      b.render(14, 37);
      b.render(37, kBlk);
      b.end_block();
      const std::int16_t* f = b.feed(0);
      for (std::uint32_t i = 0; i < kBlk; ++i) {
        ob.push_back(f[i]);
      }
    }
    CHECK(oa == ob);
  }
}

// ---- A note's life ------------------------------------------------------------------------------------------------------------------------------------------------------
TEST_CASE("granular: the note attacks from silence, releases to silence, a retrigger restarts the grain, and the amp envelope and gain apply") {
  Cfg c;
  c.kind = Fill::Dc;
  c.shape = 0;
  c.gr_len = 600;
  VoicePool& p = pool_a();
  SampleDesc d = make_desc(c);
  d.gain = 32768;  // half
  p.set_sample(0, d);
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  std::vector<std::int16_t> out;
  render(p, out, 128);
  CHECK(out[0] == 0);  // the attack starts from silence
  CHECK(std::abs(out[100] - 10000) <= 2);  // half of 20000 x 65535 / 65536
  p.note_off(0);
  render(p, out, 600);
  CHECK(out[128 + 256 + 70] == 0);  // 256 samples of release, then the residual's 64
  CHECK(p.voices_in_use() == 0u);
  // retrigger: a new note on the track while it sounds takes the voice over; the grain restarts (position 0: the first sample past the attack is the grain's start)
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  render(p, out, 128);
  REQ(p.note_on(0, 64, 0, audio::kVelocityDefault));
  CHECK(p.voices_in_use() == 1u);
  std::vector<std::int16_t> again;
  render(p, again, 128);
  CHECK(std::abs(again[100] - 10000) <= 2);
}

TEST_CASE("granular: set_sample drops the loop, the slices and the wavetable of a granular descriptor, and holds the shape and the type") {
  const std::int16_t* pcm = fill(4000, Fill::Wave);
  SampleDesc d{pcm, 4000, 10, 3000, 65536};
  d.granular = true;
  d.wt_size = 256;
  d.beat_slice = true;
  d.gr_shape = 9;
  d.loop_mode = 7;
  VoicePool& p = pool_a();
  p.set_sample(0, d);
  const SampleDesc& s = p.sample(0);
  CHECK(s.granular);
  CHECK(s.loop_start == 0u);
  CHECK(s.loop_end == 0u);
  CHECK(s.wt_size == 0u);
  CHECK_FALSE(s.beat_slice);
  CHECK(s.gr_shape == 0u);
  CHECK(s.loop_mode == 0u);
}

TEST_CASE("granular: a non-granular descriptor is untouched by all of it (the plain forward loop plays as before)") {
  const std::int16_t* pcm = fill(4000, Fill::Ramp);
  SampleDesc d{pcm, 4000, 0, 4000, 65536};
  VoicePool& p = pool_a();
  p.set_sample(0, d);
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  std::vector<std::int16_t> out;
  render(p, out, 128);
  CHECK_FALSE(p.sample(0).granular);
  CHECK(out[100] == 100);  // a ramp read at 1.0: the sample's own value
}

// OS-RATEPITCH: the rate factor scales the increment of a granular voice like any other, so a grain of a 88,200 Hz sample at the root plays as one of a 44,100 Hz sample an octave up, and one of a
// 22,050 Hz sample as the one an octave down (the factors 2 and 1/2 are exact in the Q28; the octave's increment is the root's shifted): the same samples, all three grain types.
TEST_CASE("granular: a grain takes the sample's rate factor: 88,200 Hz at the root is 44,100 Hz an octave up, 22,050 Hz is an octave down (OS-RATEPITCH)") {
  for (unsigned type = 0; type < 3; ++type) {
    for (const std::uint32_t hz : {88200u, 22050u}) {
      Cfg c;
      c.type = type;
      c.shape = 2;
      c.gr_len = 300;
      c.pos = 20000;
      SampleDesc plain = make_desc(c);
      SampleDesc scaled = plain;
      scaled.rate_q28 = audio::rate_factor_q28(hz);
      std::vector<std::int16_t> want, got;
      VoicePool& p = pool_a();
      p.set_sample(0, plain);
      REQ(p.note_on(0, hz == 88200u ? 72u : 48u, 0, audio::kVelocityDefault));
      render(p, want, 1100);
      VoicePool& q = pool_b();
      q.set_sample(0, scaled);
      REQ(q.note_on(0, 60, 0, audio::kVelocityDefault));
      render(q, got, 1100);
      REQ(want.size() == got.size());
      CHECK_MESSAGE(want == got, "type " << type << " rate " << hz);
      p.cut(0);
      q.cut(0);
    }
  }
}
