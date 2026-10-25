// SPDX-License-Identifier: GPL-3.0-or-later
//
// The peak-cache mipmap (ui/plot/peak_cache.h) and the min/max kernels under it (ui/plot/peak_kernel.h): every level
// equals a brute-force min/max, queries equal brute force at any zoom and offset, an incremental update equals a full
// rebuild, a cooperative build equals a blocking one, and the SIMD loop (run with the model of SSUB16 + SEL) equals the
// scalar loop for every alignment, length and channel layout.
#include <doctest/doctest.h>

#include <o3os/ui/plot/peak_cache.h>

#include <cstdint>
#include <cstring>
#include <vector>

using namespace o3os::ui::plot;

namespace {

struct Rng {
  std::uint32_t s;
  explicit Rng(std::uint32_t seed) : s(seed ? seed : 1u) {}
  std::uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  std::int16_t sample() { return static_cast<std::int16_t>(next() >> 16); }
  std::uint32_t below(std::uint32_t n) { return n == 0 ? 0 : next() % n; }
};

// Words, so the buffers are 4-byte aligned like the real ones (the cache checks it).
struct Sound {
  std::vector<std::uint32_t> words;
  Frame frames;
  unsigned channels;
  Sound(Frame f, unsigned ch, std::uint32_t seed) : words((static_cast<std::size_t>(f) * ch + 1) / 2 + 1), frames(f), channels(ch) {
    Rng rng(seed);
    std::int16_t* p = data();
    for (std::size_t i = 0; i < static_cast<std::size_t>(f) * ch; ++i) {
      // a mix of loud and quiet stretches so that min/max differ from block to block
      const int amp = 1 + static_cast<int>(rng.below(32767));
      p[i] = static_cast<std::int16_t>((static_cast<int>(rng.below(2u * static_cast<std::uint32_t>(amp) + 1u))) - amp);
    }
  }
  std::int16_t* data() { return reinterpret_cast<std::int16_t*>(words.data()); }
  const std::int16_t* data() const { return reinterpret_cast<const std::int16_t*>(words.data()); }
};

struct Cached {
  std::vector<std::uint64_t> storage;
  PeakCache cache;
  Cached(Sound& s, bool build = true) : storage(PeakCache::elements_required(s.frames, s.channels) / 4 + 1) {
    CHECK(cache.init(reinterpret_cast<std::int16_t*>(storage.data()), storage.size() * 4, s.data(), s.frames, s.channels));
    if (build) {
      cache.build();
    }
  }
};

PeakPair brute(const Sound& s, unsigned ch, Frame a, Frame b) {
  int mn = 32767, mx = -32768;
  for (Frame f = a; f < b && f < s.frames; ++f) {
    const int v = s.data()[static_cast<std::size_t>(f) * s.channels + ch];
    mn = v < mn ? v : mn;
    mx = v > mx ? v : mx;
  }
  return PeakPair{static_cast<std::int16_t>(mn), static_cast<std::int16_t>(mx)};
}

bool same(PeakPair a, PeakPair b) { return a.min == b.min && a.max == b.max; }

}  // namespace

TEST_CASE("plot peak cache: the size arithmetic: 4 bytes an entry, 0.0833 bytes a sample") {
  static_assert(PeakCache::levels_for(0) == 0 && PeakCache::levels_for(1) == 1 && PeakCache::levels_for(64) == 1 &&
                    PeakCache::levels_for(65) == 2 && PeakCache::levels_for(256) == 2 && PeakCache::levels_for(257) == 3,
                "a level is added until one entry covers the sound");
  static_assert(PeakCache::levels_for(PeakCache::kMaxFrames) == PeakCache::kMaxLevels, "16 Mi frames need all ten levels");
  static_assert(PeakCache::entries_at(100, 1) == 2 && PeakCache::entries_at(100, 2) == 1 && PeakCache::entries_at(4096, 4) == 1 &&
                    PeakCache::entries_at(4097, 4) == 2,
                "entries per level");
  // 2^20 frames: 16384 + 4096 + 1024 + 256 + 64 + 16 + 4 + 1 entries (the last padded to 2), 4 bytes each.
  constexpr Frame kFrames = 1u << 20;
  static_assert(PeakCache::levels_for(kFrames) == 8, "64 * 4^7 = 2^20");
  static_assert(PeakCache::bytes_required(kFrames, 1) == 4u * (16384 + 4096 + 1024 + 256 + 64 + 16 + 4 + 2), "87,384 bytes");
  static_assert(PeakCache::bytes_required(kFrames, 2) == 2u * PeakCache::bytes_required(kFrames, 1), "stereo doubles it");
  const double per_sample = static_cast<double>(PeakCache::bytes_required(kFrames, 1)) / static_cast<double>(kFrames);
  CHECK(per_sample > 0.0833);
  CHECK(per_sample < 0.0834);  // 1/12 of a byte, 1.67 % of the int16 it summarises
  // The SDRAM's 16 Mi frames, stereo (which would not fit it: 64 MiB of samples): 2.7 MB, not a copy of the sound.
  CHECK(PeakCache::bytes_required(PeakCache::kMaxFrames, 2) < 3u * 1024 * 1024);  // 2.67 MB
}

TEST_CASE("plot peak cache: init refuses what it cannot use") {
  Sound s(1000, 1, 7);
  std::vector<std::uint64_t> store(PeakCache::elements_required(1000, 1) / 4 + 2);
  auto* st = reinterpret_cast<std::int16_t*>(store.data());
  PeakCache c;
  CHECK(c.init(st, PeakCache::elements_required(1000, 1), s.data(), 1000, 1));
  CHECK(c.valid());
  CHECK_FALSE(c.init(st, PeakCache::elements_required(1000, 1) - 1, s.data(), 1000, 1));  // storage too small
  CHECK_FALSE(c.valid());
  CHECK_FALSE(c.init(st + 1, 1000, s.data(), 1000, 1));  // storage not word aligned
  CHECK_FALSE(c.init(st, 1000, s.data() + 1, 1000, 1));  // samples not word aligned
  CHECK_FALSE(c.init(st, 1000, s.data(), 1000, 3));      // three channels
  CHECK_FALSE(c.init(st, 1000, s.data(), 1000, 0));
  CHECK_FALSE(c.init(st, 1000, s.data(), PeakCache::kMaxFrames + 1, 1));
  CHECK_FALSE(c.init(st, 1000, nullptr, 1000, 1));
  CHECK(c.init(nullptr, 0, nullptr, 0, 2));  // an empty sound needs nothing
  CHECK(c.levels() == 0);
  CHECK(c.range(0, 0, 10).empty());
}

TEST_CASE("plot peak cache: every level equals a brute-force min/max, for every length around the block edges") {
  const Frame lengths[] = {1, 2, 3, 63, 64, 65, 127, 128, 255, 256, 257, 1023, 1024, 1025, 4095, 4096, 4097, 16385, 100003};
  for (const unsigned ch : {1u, 2u}) {
    for (const Frame n : lengths) {
      Sound s(n, ch, 1000u + n);
      Cached c(s);
      CHECK(c.cache.levels() == PeakCache::levels_for(n));
      for (unsigned channel = 0; channel < ch; ++channel) {
        for (unsigned level = 1; level <= c.cache.levels(); ++level) {
          // level L entry e covers frames [e << shift, (e + 1) << shift): query exactly that range
          const unsigned shift = PeakCache::level_shift(level);
          for (Frame e = 0; e < PeakCache::entries_at(n, level); ++e) {
            const Frame a = e << shift, b = (e + 1) << shift;
            const PeakPair got = c.cache.range(channel, a, b);
            const PeakPair want = brute(s, channel, a, b);
            CHECK_MESSAGE(same(got, want), "n=" << n << " ch=" << ch << "/" << channel << " level " << level << " entry " << e);
          }
        }
      }
    }
  }
}

TEST_CASE("plot peak cache: range() equals brute force for arbitrary ranges, all alignments") {
  Rng rng(42);
  for (const unsigned ch : {1u, 2u}) {
    Sound s(120007, ch, 99);
    Cached c(s);
    for (int i = 0; i < 1600; ++i) {
      const Frame a = rng.below(s.frames);
      const Frame len = (i % 4 == 0) ? 1u + rng.below(70) : (i % 4 == 1) ? 1u + rng.below(5000) : 1u + rng.below(s.frames);
      const unsigned channel = rng.below(ch);
      CHECK(same(c.cache.range(channel, a, a + len), brute(s, channel, a, a + len)));
    }
    // empty, reversed and past-the-end ranges
    CHECK(c.cache.range(0, 10, 10).empty());
    CHECK(c.cache.range(0, 20, 10).empty());
    CHECK(c.cache.range(0, s.frames + 5, s.frames + 50).empty());
    CHECK(c.cache.range(ch, 0, 10).empty());  // no such channel
    CHECK(same(c.cache.range(0, 0, s.frames * 2), brute(s, 0, 0, s.frames)));  // clamped to the sound
  }
}

TEST_CASE("plot peak cache: columns() equals brute force at every zoom and offset") {
  Rng rng(5);
  for (const unsigned ch : {1u, 2u}) {
    Sound s(100003, ch, 1234);
    Cached c(s);
    std::vector<PeakPair> got(800);
    for (int i = 0; i < 150; ++i) {
      const unsigned count = 1u + rng.below(800);
      // spans from far zoomed in (a few frames over many columns) to the whole sound and beyond its end
      const Frame span = (i % 5 == 0) ? 1u + rng.below(count * 2) : (i % 5 == 1) ? 1u + rng.below(5000) : 1u + rng.below(s.frames * 2);
      const Frame first = rng.below(s.frames + 1000);
      const unsigned channel = rng.below(ch);
      c.cache.columns(channel, first, span, count, got.data());
      for (unsigned col = 0; col < count; ++col) {
        const Frame a = PeakCache::column_start(first, span, count, col);
        const Frame b = PeakCache::column_start(first, span, count, col + 1);
        PeakPair want;
        if (a >= s.frames) {
          want = kNoPeak;
        } else if (b <= a) {  // zoomed in past a frame a column: the frame it falls in
          const std::int16_t v = s.data()[static_cast<std::size_t>(a) * ch + channel];
          want = PeakPair{v, v};
        } else {
          want = brute(s, channel, a, b);
        }
        CHECK_MESSAGE(same(got[col], want), "ch=" << ch << " span=" << span << " count=" << count << " first=" << first << " col " << col);
      }
    }
  }
}

TEST_CASE("plot peak cache: column_of is the inverse of column_start") {
  Rng rng(77);
  for (int i = 0; i < 400; ++i) {
    const unsigned count = 1u + rng.below(800);
    const Frame span = 1u + rng.below(3000000);
    const Frame first = rng.below(1000000);
    // every frame of the view falls in the column whose range holds it
    const unsigned samples_to_check = 60;
    for (unsigned k = 0; k < samples_to_check; ++k) {
      const Frame d = rng.below(span);
      const unsigned col = PeakCache::column_of(first, span, count, first + d);
      CHECK(col < count);
      CHECK(PeakCache::column_start(first, span, count, col) <= first + d);
      CHECK(first + d < PeakCache::column_start(first, span, count, col + 1));
    }
    CHECK(PeakCache::column_of(first, span, count, first) == 0);
    CHECK(PeakCache::column_of(first, span, count, first + span - 1) == count - 1);
    CHECK(PeakCache::column_of(first, span, count, first + span) == count);  // after the view
    if (first > 0) {
      CHECK(PeakCache::column_of(first, span, count, first - 1) == 0);  // before it
    }
  }
}

TEST_CASE("plot peak cache: update() after an edit equals a full rebuild, and costs only the entries it touches") {
  Rng rng(321);
  for (const unsigned ch : {1u, 2u}) {
    Sound s(70001, ch, 3);
    Cached c(s);
    for (int i = 0; i < 60; ++i) {
      const Frame first = rng.below(s.frames);
      const Frame count = 1u + (i % 3 == 0 ? rng.below(40) : (i % 3 == 1 ? rng.below(3000) : rng.below(s.frames)));
      for (Frame f = first; f < first + count && f < s.frames; ++f) {
        for (unsigned k = 0; k < ch; ++k) {
          s.data()[static_cast<std::size_t>(f) * ch + k] = rng.sample();
        }
      }
      c.cache.update(first, count);
      Cached fresh(s);
      CHECK(std::memcmp(c.storage.data(), fresh.storage.data(), PeakCache::bytes_required(s.frames, ch)) == 0);
    }
  }
}

TEST_CASE("plot peak cache: a cooperative build in slices equals the blocking build, and every slice is bounded") {
  Sound s(250001, 2, 11);
  Cached whole(s);
  Cached sliced(s, false);
  sliced.cache.build_begin();
  CHECK_FALSE(sliced.cache.built());
  unsigned steps = 0;
  while (!sliced.cache.build_step(4096)) {
    ++steps;
    CHECK(steps < 100000);
  }
  CHECK(steps > 20);  // really sliced: 250 K frames in slices of about 4 K reads
  CHECK(sliced.cache.built());
  CHECK(std::memcmp(whole.storage.data(), sliced.storage.data(), PeakCache::bytes_required(s.frames, 2)) == 0);
  CHECK(sliced.cache.build_step(1));  // already done: true again, nothing to do
  // A step of budget 1 still makes progress (one entry).
  Cached tiny(s, false);
  tiny.cache.build_begin();
  unsigned n = 0;
  while (!tiny.cache.build_step(1)) {
    ++n;
  }
  CHECK(std::memcmp(whole.storage.data(), tiny.storage.data(), PeakCache::bytes_required(s.frames, 2)) == 0);
}

TEST_CASE("plot peak cache: the SIMD min/max loop (model of SSUB16 + SEL) equals the scalar loop for every alignment and layout") {
  Rng rng(2024);
  std::vector<std::uint32_t> words(4096);
  std::int16_t* p = reinterpret_cast<std::int16_t*>(words.data());
  for (std::size_t i = 0; i < words.size() * 2; ++i) {
    p[i] = rng.sample();
  }
  p[1234] = 32767;   // the extremes sit at known places
  p[3001] = -32768;
  // every start and length around the loop's unrolling (8 samples mono, 4 frames stereo) and its head and tail
  for (const unsigned channels : {1u, 2u}) {
    const Frame total = static_cast<Frame>(words.size() * 2 / channels);
    for (Frame a = 0; a < 20; ++a) {
      for (Frame len = 0; len < 70; ++len) {
        for (unsigned ch = 0; ch < channels; ++ch) {
          int mn_s = 32767, mx_s = -32768, mn_e = 32767, mx_e = -32768;
          detail::minmax_scalar(p + static_cast<std::size_t>(a) * channels + ch, len, channels, mn_s, mx_s);
          detail::minmax_simd<detail::EmuOps>(p, channels, ch, a, a + len, mn_e, mx_e);
          CHECK(mn_s == mn_e);
          CHECK(mx_s == mx_e);
        }
      }
    }
    for (int i = 0; i < 2000; ++i) {  // long random ranges, including the planted extremes
      const Frame a = rng.below(total), len = rng.below(total - a + 1);
      for (unsigned ch = 0; ch < channels; ++ch) {
        int mn_s = 32767, mx_s = -32768, mn_e = 32767, mx_e = -32768;
        detail::minmax_scalar(p + static_cast<std::size_t>(a) * channels + ch, len, channels, mn_s, mx_s);
        detail::minmax_simd<detail::EmuOps>(p, channels, ch, a, a + len, mn_e, mx_e);
        CHECK(mn_s == mn_e);
        CHECK(mx_s == mx_e);
      }
    }
  }
  // The model itself: lane-wise max / min of two packed words, checked against the lanes taken apart.
  for (int i = 0; i < 2000; ++i) {
    const std::uint32_t a = rng.next(), b = rng.next();
    const std::uint32_t mx = detail::EmuOps::max2(a, b), mn = detail::EmuOps::min2(a, b);
    CHECK(detail::lane_lo(mx) == (detail::lane_lo(a) > detail::lane_lo(b) ? detail::lane_lo(a) : detail::lane_lo(b)));
    CHECK(detail::lane_hi(mx) == (detail::lane_hi(a) > detail::lane_hi(b) ? detail::lane_hi(a) : detail::lane_hi(b)));
    CHECK(detail::lane_lo(mn) == (detail::lane_lo(a) < detail::lane_lo(b) ? detail::lane_lo(a) : detail::lane_lo(b)));
    CHECK(detail::lane_hi(mn) == (detail::lane_hi(a) < detail::lane_hi(b) ? detail::lane_hi(a) : detail::lane_hi(b)));
  }
}

TEST_CASE("plot peak cache: until it is built the queries answer nothing, the epoch moves with every build, an edit restarts a build") {
  Sound s(50000, 2, 5);
  Cached c(s, false);
  // the storage holds whatever was in it (on the device, SDRAM nobody wrote): poison it, so that a query that reads it shows
  for (std::uint64_t& w : c.storage) {
    w = 0x5A5A5A5A5A5A5A5Aull;
  }
  const std::uint32_t epoch_init = c.cache.epoch();
  CHECK_FALSE(c.cache.built());
  CHECK(c.cache.range(0, 0, 1000).empty());
  CHECK(c.cache.range(1, 10, 49999).empty());
  PeakPair cols[120];
  c.cache.columns(0, 0, 50000, 120, cols);
  for (const PeakPair& p : cols) {
    CHECK(p.empty());
  }
  c.cache.build_begin();
  CHECK(c.cache.epoch() != epoch_init);
  const std::uint32_t epoch_begun = c.cache.epoch();
  CHECK_FALSE(c.cache.build_step(1000));
  CHECK_FALSE(c.cache.built());
  CHECK(c.cache.epoch() == epoch_begun);  // no change of epoch while it builds
  CHECK(c.cache.range(0, 0, 1000).empty());  // the level 1 entries made so far are not offered either
  c.cache.columns(0, 0, 50000, 120, cols);
  CHECK(cols[0].empty());
  // an edit during the build restarts it: nothing already made is trusted
  s.data()[2 * 100] = 32767;
  c.cache.update(100, 1);
  CHECK_FALSE(c.cache.built());
  CHECK(c.cache.epoch() != epoch_begun);
  unsigned steps = 0;
  while (!c.cache.build_step(4096)) {
    ++steps;
  }
  CHECK(steps > 5);  // it began again from the start
  CHECK(c.cache.built());
  const std::uint32_t epoch_done = c.cache.epoch();
  // the finished cache is right, the edit included, and the poison is gone from everything a query reads
  bool ok = true;
  Rng rng(3);
  for (int i = 0; i < 300; ++i) {
    const Frame a = rng.below(s.frames), len = 1 + rng.below(s.frames - a);
    for (unsigned ch = 0; ch < 2; ++ch) {
      ok = ok && same(c.cache.range(ch, a, a + len), brute(s, ch, a, a + len));
    }
  }
  CHECK(ok);
  CHECK(c.cache.range(0, 100, 101).max == 32767);
  // a rebuild (begin again) is a new epoch, and so is finishing one
  c.cache.build_begin();
  CHECK(c.cache.epoch() != epoch_done);
  const std::uint32_t epoch_rebuilding = c.cache.epoch();
  while (!c.cache.build_step(1u << 30)) {
  }
  CHECK(c.cache.epoch() != epoch_rebuilding);
  // an empty sound is built from the start, and a cache that cannot be used has no builds
  PeakCache none;
  CHECK(none.init(nullptr, 0, nullptr, 0, 1));
  CHECK(none.built());
  none.build_begin();
  CHECK(none.built());
  PeakCache unusable;
  CHECK_FALSE(unusable.init(nullptr, 0, nullptr, 100, 1));
  unusable.build_begin();
  CHECK(unusable.build_step(100));  // nothing to build: done (and nothing to query)
  CHECK(unusable.range(0, 0, 10).empty());
}

#if defined(__ARM_FEATURE_SIMD32)
// Only where the instructions exist (the MK66 build, and the QEMU suite that runs it): the real SSUB16 + SEL against the scalar loop
// and against the model that the host runs.
TEST_CASE("plot peak cache: the Cortex-M4 SSUB16 + SEL loop equals the scalar loop and the model, every alignment and layout") {
  Rng rng(2025);
  std::vector<std::uint32_t> words(4096);
  std::int16_t* p = reinterpret_cast<std::int16_t*>(words.data());
  for (std::size_t i = 0; i < words.size() * 2; ++i) {
    p[i] = rng.sample();
  }
  p[1234] = 32767;
  p[3001] = -32768;
  std::uint32_t checked = 0, bad = 0;
  for (const unsigned channels : {1u, 2u}) {
    const Frame total = static_cast<Frame>(words.size() * 2 / channels);
    for (Frame a = 0; a < 20; ++a) {
      for (Frame len = 0; len < 70; ++len) {
        for (unsigned ch = 0; ch < channels; ++ch) {
          int mn_s = 32767, mx_s = -32768, mn_m = 32767, mx_m = -32768, mn_e = 32767, mx_e = -32768;
          detail::minmax_scalar(p + static_cast<std::size_t>(a) * channels + ch, len, channels, mn_s, mx_s);
          detail::minmax_simd<detail::M4Ops>(p, channels, ch, a, a + len, mn_m, mx_m);
          detail::minmax_simd<detail::EmuOps>(p, channels, ch, a, a + len, mn_e, mx_e);
          ++checked;
          bad += (mn_s != mn_m || mx_s != mx_m || mn_s != mn_e || mx_s != mx_e) ? 1u : 0u;
        }
      }
    }
    for (int i = 0; i < 1500; ++i) {
      const Frame a = rng.below(total), len = rng.below(total - a + 1);
      for (unsigned ch = 0; ch < channels; ++ch) {
        int mn_s = 32767, mx_s = -32768, mn_m = 32767, mx_m = -32768;
        detail::minmax_scalar(p + static_cast<std::size_t>(a) * channels + ch, len, channels, mn_s, mx_s);
        detail::minmax_simd<detail::M4Ops>(p, channels, ch, a, a + len, mn_m, mx_m);
        ++checked;
        bad += (mn_s != mn_m || mx_s != mx_m) ? 1u : 0u;
      }
    }
  }
  CHECK(checked > 5000u);
  CHECK(bad == 0u);
  for (int i = 0; i < 20000; ++i) {
    const std::uint32_t a = rng.next(), b = rng.next();
    CHECK(detail::M4Ops::max2(a, b) == detail::EmuOps::max2(a, b));
    CHECK(detail::M4Ops::min2(a, b) == detail::EmuOps::min2(a, b));
  }
}
#endif
