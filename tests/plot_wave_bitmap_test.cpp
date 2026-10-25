// SPDX-License-Identifier: GPL-3.0-or-later
//
// The wave bitmap (ui/plot/wave_bitmap.h) without a rasteriser, so that it also runs on the Cortex-M4F (ctest qemu_plot): the row
// renderer equals a pixel-by-pixel reference, uploads are chunks of at most 4 KB at word-aligned addresses into the idle slot, a
// Busy display is retried, requests are deduplicated, a refused upload is counted, clip runs are found, and a cache that is
// still building is never rendered from. The uploads go to a FakeDisplay that records them; `Ram` replays them.
#include <doctest/doctest.h>

#include <o3os/platform/host/fakes.h>
#include <o3os/ui/plot/peak_cache.h>
#include <o3os/ui/plot/wave_bitmap.h>

#include <cstdint>
#include <cstring>
#include <vector>

using namespace o3os;
using namespace o3os::ui::plot;
using platform::host::FakeDisplay;

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
  std::uint32_t below(std::uint32_t n) { return n == 0 ? 0 : next() % n; }
};

// A sound with its cache: signals that make a waveform worth looking at (decay, noise, silence, a clipped square).
struct Sound {
  std::vector<std::uint32_t> words;
  std::vector<std::uint64_t> store;
  Frame frames;
  unsigned channels;
  PeakCache cache;
  Sound(Frame n, unsigned ch, unsigned kind, std::uint32_t seed = 1, bool build = true)
      : words((static_cast<std::size_t>(n) * ch + 1) / 2 + 1),
        store(PeakCache::elements_required(n, ch) / 4 + 1),
        frames(n),
        channels(ch) {
    Rng rng(seed);
    auto* p = reinterpret_cast<std::int16_t*>(words.data());
    for (Frame f = 0; f < n; ++f) {
      for (unsigned c = 0; c < ch; ++c) {
        int v = 0;
        switch (kind) {
          case 0: {  // a decaying burst of noise: loud at the start, dying away
            const int amp = static_cast<int>(30000u * (n - f) / n);
            v = amp == 0 ? 0 : static_cast<int>(rng.below(static_cast<std::uint32_t>(2 * amp + 1))) - amp;
            break;
          }
          case 1:  // silence
            v = 0;
            break;
          case 2:  // a square at full scale: every column clips
            v = ((f / 37u) % 2u) != 0 ? 32767 : -32768;
            break;
          case 3:  // a slow triangle with an offset: no column crosses the zero line
            v = 4000 + static_cast<int>(((f * 7u) % 20000u)) - ((f * 7u) % 40000u > 20000u ? 10000 : 0);
            break;
          default:  // noise at full range
            v = static_cast<int>(rng.below(65536u)) - 32768;
            break;
        }
        p[static_cast<std::size_t>(f) * ch + c] = static_cast<std::int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
      }
    }
    const bool ok = cache.init(reinterpret_cast<std::int16_t*>(store.data()), store.size() * 4, p, n, ch);
    CHECK(ok);
    if (build) {
      cache.build();
    }
  }
};

// RAM_G as far as the wave slots, replayed from a recording display's uploads.
struct Ram {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(kRamGWaveEnd, 0);
  void apply(const FakeDisplay& d) {
    for (const FakeDisplay::Upload& u : d.uploads()) {
      CHECK(u.addr + u.bytes.size() <= bytes.size());
      if (u.addr + u.bytes.size() <= bytes.size()) {
        std::memcpy(bytes.data() + u.addr, u.bytes.data(), u.bytes.size());
      }
    }
  }
};

constexpr std::size_t kPlenty = 1u << 22;  // a recording cap nothing here reaches

}  // namespace

TEST_CASE("plot wave bitmap: the row renderer equals the pixel-by-pixel reference, L1 and L4, any width and height") {
  struct Case {
    unsigned kind, width, height;
    Frame first, span;
  };
  const Case cases[] = {
      {0, 740, 150, 0, 60000},    {0, 800, 240, 0, 70000},  {4, 333, 97, 100, 20000}, {2, 129, 33, 0, 9000},
      {1, 64, 10, 0, 3000},       {3, 301, 120, 0, 40000},  {0, 7, 2, 0, 50000},      {0, 1, 1, 0, 100},
      {4, 9, 240, 5000, 100},     {0, 200, 60, 0, 90},      {0, 640, 128, 30000, 90000},  // past the end: empty columns
      {0, 31, 17, 0, 40},         {4, 800, 1, 0, 70000},
  };
  Sound decay(65000, 1, 0, 3), noise2(65000, 2, 4, 5), square(65000, 1, 2, 17), quiet(65000, 1, 1, 1), offset(65000, 1, 3, 1);
  for (const Case& c : cases) {
    for (const WaveDepth depth : {WaveDepth::L1, WaveDepth::L4}) {
      Sound& sound = c.kind == 0 ? decay : (c.kind == 4 ? noise2 : (c.kind == 2 ? square : (c.kind == 3 ? offset : quiet)));
      FakeDisplay display;
      display.record_uploads(kPlenty);
      WaveBitmap bm;
      WaveKey key;
      key.cache = &sound.cache;
      key.epoch = sound.cache.epoch();
      key.first = c.first;
      key.span = c.span;
      key.width = static_cast<std::uint16_t>(c.width);
      key.height = static_cast<std::uint16_t>(c.height);
      key.channel = static_cast<std::uint8_t>(c.kind == 4 ? 1 : 0);
      key.depth = depth;
      bm.request(0, key);
      CHECK(bm.busy());
      CHECK_FALSE(bm.ready(0));
      CHECK(bm.service_all(display));
      CHECK(bm.ready(0));
      CHECK(bm.shows(0, key));
      std::vector<PeakPair> peaks(c.width);
      sound.cache.columns(key.channel, c.first, c.span, c.width, peaks.data());
      const unsigned stride = WaveBitmap::stride_of(c.width, depth);
      std::vector<std::uint8_t> want(static_cast<std::size_t>(stride) * c.height);
      WaveBitmap::render_reference(peaks.data(), c.width, c.height, depth, want.data());
      Ram ram;
      ram.apply(display);
      const bool same = std::memcmp(want.data(), ram.bytes.data() + bm.shown_addr(0), want.size()) == 0;
      CHECK_MESSAGE(same, "kind " << c.kind << " " << c.width << "x" << c.height << " depth " << static_cast<int>(depth));
      CHECK(bm.shown_stride(0) == stride);
    }
  }
}

TEST_CASE("plot wave bitmap: the L1 picture is what the peaks say, hand-checked") {
  // Four columns of a 10-row lane: full range, nothing, the upper half, a single sample at zero.
  std::vector<std::uint32_t> words(8);
  auto* p = reinterpret_cast<std::int16_t*>(words.data());
  p[0] = 32767;
  p[1] = -32768;   // column 0: both extremes
  p[2] = 0;
  p[3] = 0;        // column 1: 0 .. 0 (the zero row)
  p[4] = 32767;
  p[5] = 1000;     // column 2: upper half
  p[6] = -32768;
  p[7] = -32768;   // column 3: the bottom row
  std::vector<std::uint64_t> store(PeakCache::elements_required(8, 1) / 4 + 1);
  PeakCache cache;
  CHECK(cache.init(reinterpret_cast<std::int16_t*>(store.data()), store.size() * 4, p, 8, 1));
  cache.build();
  FakeDisplay display;
  display.record_uploads(kPlenty);
  WaveBitmap bm;
  WaveKey key;
  key.cache = &cache;
  key.epoch = cache.epoch();
  key.span = 8;
  key.width = 4;
  key.height = 10;
  bm.request(0, key);
  CHECK(bm.service_all(display));
  Ram ram;
  ram.apply(display);
  const std::uint8_t* img = ram.bytes.data() + bm.shown_addr(0);
  // columns hold 2 frames each: col 0 = frames 0,1 (32767, -32768): rows 0..9; col 1 = (0,0): row 4 (wave_zero_row(10));
  // col 2 = (32767, 1000): rows 0..4; col 3 = (-32768,-32768): row 9.
  CHECK(wave_zero_row(10) == 4);
  // L1, stride 1: bit 7 is column 0.
  const unsigned expect_rows[10] = {
      0b1010,  // row 0: col 0 and col 2
      0b1010, 0b1010, 0b1010,
      0b1110,  // row 4: col 0, col 1 (zero row), col 2
      0b1000, 0b1000, 0b1000, 0b1000,
      0b1001,  // row 9: col 0 and col 3
  };
  for (unsigned y = 0; y < 10; ++y) {
    CHECK_MESSAGE(img[y] == (expect_rows[y] << 4), "row " << y << " is " << static_cast<int>(img[y]));
  }
}

TEST_CASE("plot wave bitmap: L4 differs from L1 only on edge pixels, with fractional alpha, and the interior is full") {
  Sound s(50000, 1, 0, 9);
  const unsigned w = 400, h = 120;
  std::vector<PeakPair> peaks(w);
  s.cache.columns(0, 0, 50000, w, peaks.data());
  std::vector<std::uint8_t> l1(WaveBitmap::stride_of(w, WaveDepth::L1) * h), l4(WaveBitmap::stride_of(w, WaveDepth::L4) * h);
  WaveBitmap::render_reference(peaks.data(), w, h, WaveDepth::L1, l1.data());
  WaveBitmap::render_reference(peaks.data(), w, h, WaveDepth::L4, l4.data());
  const unsigned s1 = WaveBitmap::stride_of(w, WaveDepth::L1), s4 = WaveBitmap::stride_of(w, WaveDepth::L4);
  unsigned edge_partial = 0, edge_total = 0;
  for (unsigned c = 0; c < w; ++c) {
    unsigned first = h, last = 0;
    for (unsigned y = 0; y < h; ++y) {
      const bool lit = ((l1[y * s1 + c / 8] >> (7 - c % 8)) & 1u) != 0;
      const unsigned a = (c % 2 == 0) ? l4[y * s4 + c / 2] >> 4 : l4[y * s4 + c / 2] & 0xFu;
      CHECK_MESSAGE(lit == (a != 0), "column " << c << " row " << y);  // the same pixels are touched
      if (lit) {
        first = y < first ? y : first;
        last = y;
      }
    }
    for (unsigned y = first; y <= last; ++y) {
      const unsigned a = (c % 2 == 0) ? l4[y * s4 + c / 2] >> 4 : l4[y * s4 + c / 2] & 0xFu;
      if (y != first && y != last) {
        CHECK_MESSAGE(a == 15u, "interior pixel of column " << c);
      } else {
        CHECK(a >= 6u);
        CHECK(a <= 15u);
        ++edge_total;
        edge_partial += a < 15u ? 1u : 0u;
      }
    }
  }
  CHECK(edge_partial > edge_total / 4);  // L4 really does carry fractions: most edges are not full
}

TEST_CASE("plot wave bitmap: uploads are chunks of at most 4 KB into the idle slot, word aligned, in order, and the old picture survives") {
  Sound s(80000, 2, 0, 11);
  FakeDisplay display;
  display.record_uploads(kPlenty);
  WaveBitmap bm;
  const auto key_of = [&](Frame first, Frame span) {
    WaveKey k;
    k.cache = &s.cache;
    k.epoch = s.cache.epoch();
    k.first = first;
    k.span = span;
    k.width = 740;
    k.height = 150;
    k.channel = 0;
    k.depth = WaveDepth::L1;
    return k;
  };
  const WaveKey a = key_of(0, 80000);
  bm.request(0, a);
  CHECK(bm.service_all(display));
  const std::size_t after_first = display.accepted_uploads();
  const unsigned stride = WaveBitmap::stride_of(740, WaveDepth::L1);
  const std::size_t total = static_cast<std::size_t>(stride) * 150;  // 93 * 150 = 13,950 bytes
  CHECK(after_first == (total + WaveBitmap::kChunkBytes - 1) / WaveBitmap::kChunkBytes);  // four uploads
  const std::uint32_t slot_a = bm.shown_addr(0);
  CHECK(slot_a == wave_slot_addr(0, 0));
  Ram ram;
  ram.apply(display);
  std::vector<std::uint8_t> picture_a(ram.bytes.begin() + slot_a, ram.bytes.begin() + slot_a + static_cast<std::ptrdiff_t>(total));

  // A second view goes to the other slot, chunk by chunk, and the shown slot is not touched until it is complete.
  const WaveKey b = key_of(20000, 30000);
  bm.request(0, b);
  CHECK(bm.busy());
  std::size_t passes = 0;
  const std::size_t base = display.uploads().size();
  while (bm.service(display)) {
    ++passes;
    CHECK(bm.shows(0, a));  // still the old picture, from the old slot
    CHECK(bm.shown_addr(0) == slot_a);
    for (std::size_t i = base; i < display.uploads().size(); ++i) {  // nothing the display was given lies in the shown slot
      const FakeDisplay::Upload& u = display.uploads()[i];
      CHECK(u.addr >= wave_slot_addr(0, 1));
      CHECK(u.addr + u.bytes.size() <= wave_slot_addr(0, 1) + kWaveSlotBytes);
    }
  }
  CHECK(bm.shows(0, b));
  CHECK(bm.shown_addr(0) == wave_slot_addr(0, 1));
  std::uint32_t expected_addr = wave_slot_addr(0, 1);
  for (std::size_t i = base; i < display.uploads().size(); ++i) {
    const FakeDisplay::Upload& u = display.uploads()[i];
    CHECK(u.bytes.size() <= hal::kMaxUploadBytes);
    CHECK(u.addr % hal::kUploadAlign == 0);
    CHECK(u.addr == expected_addr);  // consecutive chunks, in order
    expected_addr += static_cast<std::uint32_t>(u.bytes.size());
  }
  CHECK(expected_addr == wave_slot_addr(0, 1) + total);
  CHECK(display.uploads().size() - base == 4);
  CHECK(passes + 1 == 4);
  // the old picture's bytes are what they were
  Ram after;
  after.apply(display);
  CHECK(std::memcmp(picture_a.data(), after.bytes.data() + slot_a, total) == 0);
  // and back to the first view: it goes to slot 0 again
  bm.request(0, a);
  CHECK(bm.service_all(display));
  CHECK(bm.shown_addr(0) == slot_a);
  CHECK(bm.completed() == 3);
  CHECK(bm.failed() == 0);
}

TEST_CASE("plot wave bitmap: a Busy display is retried with the chunk it was given, and the picture comes out right") {
  Sound s(40000, 1, 0, 2);
  FakeDisplay fake;
  fake.record_uploads(kPlenty);
  WaveBitmap bm;
  WaveKey key;
  key.cache = &s.cache;
  key.epoch = s.cache.epoch();
  key.span = 40000;
  key.width = 300;
  key.height = 100;
  key.depth = WaveDepth::L4;
  bm.request(0, key);
  fake.set_busy(true);
  for (int i = 0; i < 5; ++i) {
    CHECK(bm.service(fake));  // refused: work remains
  }
  CHECK(fake.accepted_uploads() == 0);
  fake.set_busy(false);
  CHECK(bm.service_all(fake));
  CHECK(bm.shows(0, key));
  CHECK(bm.failed() == 0);
  Ram ram;
  ram.apply(fake);
  std::vector<PeakPair> peaks(300);
  s.cache.columns(0, 0, 40000, 300, peaks.data());
  std::vector<std::uint8_t> want(WaveBitmap::stride_of(300, WaveDepth::L4) * 100);
  WaveBitmap::render_reference(peaks.data(), 300, 100, WaveDepth::L4, want.data());
  CHECK(std::memcmp(want.data(), ram.bytes.data() + bm.shown_addr(0), want.size()) == 0);
}

TEST_CASE("plot wave bitmap: requests are deduplicated, a new request replaces a job in progress, an invalid key cancels") {
  Sound s(30000, 1, 0, 4);
  FakeDisplay fake;
  WaveBitmap bm;
  const auto key_of = [&](Frame span, unsigned w, unsigned h, WaveDepth depth) {
    WaveKey k;
    k.cache = &s.cache;
    k.epoch = s.cache.epoch();
    k.span = span;
    k.width = static_cast<std::uint16_t>(w);
    k.height = static_cast<std::uint16_t>(h);
    k.depth = depth;
    return k;
  };
  const WaveKey a = key_of(30000, 200, 100, WaveDepth::L1);
  const WaveKey b = key_of(15000, 200, 100, WaveDepth::L1);
  bm.request(0, a);
  bm.request(0, a);  // the same: nothing new
  CHECK(bm.service_all(fake));
  CHECK(bm.completed() == 1);
  bm.request(0, a);  // already shown: nothing to do
  CHECK_FALSE(bm.busy());
  CHECK_FALSE(bm.service(fake));
  // a job in progress is replaced
  const WaveKey big = key_of(30000, 800, 240, WaveDepth::L4);
  bm.request(0, big);
  CHECK(bm.service(fake));  // one chunk of many
  bm.request(0, b);         // the user scrolled on: the half-done picture is dropped
  CHECK(bm.service_all(fake));
  CHECK(bm.shows(0, b));
  CHECK(bm.completed() == 2);
  // asking for what is shown again, while a job for another view is queued, drops the job
  bm.request(0, a);
  CHECK(bm.busy());
  bm.request(0, b);
  CHECK_FALSE(bm.busy());
  // an invalid key (zero width) cancels
  bm.request(0, a);
  WaveKey bad = a;
  bad.width = 0;
  bm.request(0, bad);
  CHECK_FALSE(bm.busy());
  WaveKey none = a;
  none.cache = nullptr;
  bm.request(0, none);
  CHECK_FALSE(bm.busy());
  // a lane out of range is ignored; the two lanes are independent
  bm.request(5, a);
  CHECK_FALSE(bm.busy());
  bm.request(1, a);
  bm.request(0, a);
  CHECK(bm.busy());
  CHECK(bm.service_all(fake));
  CHECK(bm.ready(1));
  CHECK(bm.shows(1, a));
  CHECK(bm.shown_addr(1) == wave_slot_addr(1, 0));
}

TEST_CASE("plot wave bitmap: a refused upload drops the job and is counted") {
  Sound s(10000, 1, 0, 6);
  struct Refusing final : hal::Display {
    bool begin_frame() noexcept override { return true; }
    bool write(const std::uint32_t*, std::size_t) noexcept override { return true; }
    void end_frame() noexcept override {}
    void abort_frame() noexcept override {}
    hal::UploadResult upload(std::uint32_t, const std::uint8_t*, std::size_t) noexcept override {
      return hal::UploadResult::BadRange;
    }
  } refusing;
  WaveBitmap bm;
  WaveKey key;
  key.cache = &s.cache;
  key.epoch = s.cache.epoch();
  key.span = 10000;
  key.width = 100;
  key.height = 50;
  bm.request(0, key);
  CHECK_FALSE(bm.service_all(refusing));
  CHECK(bm.failed() == 1);
  CHECK_FALSE(bm.busy());
  CHECK_FALSE(bm.ready(0));
}

TEST_CASE("plot wave bitmap: clip runs are found, merged beyond eight, and travel with the picture") {
  std::vector<PeakPair> peaks(40, PeakPair{-100, 100});
  const unsigned clipped[] = {2, 3, 4, 10, 20, 21, 30};
  for (const unsigned c : clipped) {
    peaks[c] = PeakPair{-100, 32767};
  }
  peaks[35] = PeakPair{-32768, 0};
  ClipRun runs[kMaxClipRuns];
  const unsigned n = WaveBitmap::find_clips(peaks.data(), 40, runs);
  CHECK(n == 5);
  CHECK((runs[0].x0 == 2 && runs[0].x1 == 4));
  CHECK((runs[1].x0 == 10 && runs[1].x1 == 10));
  CHECK((runs[2].x0 == 20 && runs[2].x1 == 21));
  CHECK((runs[3].x0 == 30 && runs[3].x1 == 30));
  CHECK((runs[4].x0 == 35 && runs[4].x1 == 35));
  // twelve separate clips: eight runs, the last stretched to the end of the clips
  std::vector<PeakPair> many(60, PeakPair{-5, 5});
  for (unsigned i = 0; i < 12; ++i) {
    many[i * 5] = PeakPair{0, 32767};
  }
  CHECK(WaveBitmap::find_clips(many.data(), 60, runs) == kMaxClipRuns);
  CHECK(runs[kMaxClipRuns - 1].x1 == 55);
  // an empty column is not a clip
  peaks.assign(4, kNoPeak);
  CHECK(WaveBitmap::find_clips(peaks.data(), 4, runs) == 0);
  // through the bitmap: the full-scale square clips every column
  Sound sq(20000, 1, 2, 1);
  FakeDisplay display;
  WaveBitmap bm;
  WaveKey key;
  key.cache = &sq.cache;
  key.epoch = sq.cache.epoch();
  key.span = 20000;
  key.width = 200;
  key.height = 80;
  bm.request(0, key);
  CHECK(bm.service_all(display));
  CHECK(bm.shown_clip_count(0) == 1);
  CHECK((bm.shown_clips(0)[0].x0 == 0 && bm.shown_clips(0)[0].x1 == 199));
}

TEST_CASE("plot wave bitmap: nothing is rendered from a cache that is still building; a rebuilt one is rendered again") {
  Sound s(60000, 1, 0, 3, false);  // initialised, not built
  for (std::uint64_t& w : s.store) {
    w = 0x7F7F7F7F7F7F7F7Full;  // the storage holds whatever it held: a render from it would show
  }
  FakeDisplay display;
  display.record_uploads(kPlenty);
  WaveBitmap bm;
  WaveKey key;
  key.cache = &s.cache;
  key.epoch = s.cache.epoch();
  key.span = 60000;
  key.width = 300;
  key.height = 120;
  CHECK_FALSE(s.cache.built());
  CHECK_FALSE(key.valid());
  bm.request(0, key);
  CHECK_FALSE(bm.busy());
  CHECK(bm.service_all(display));
  CHECK(display.accepted_uploads() == 0);
  CHECK(bm.completed() == 0);
  // half way through the build: still nothing
  s.cache.build_begin();
  CHECK_FALSE(s.cache.build_step(500));
  key.epoch = s.cache.epoch();
  CHECK_FALSE(key.valid());
  bm.request(0, key);
  CHECK_FALSE(bm.busy());
  // a request made while the cache was built, still queued when its build begins again, is dropped without being a failure
  while (!s.cache.build_step(1u << 30)) {
  }
  key.epoch = s.cache.epoch();
  CHECK(key.valid());
  bm.request(0, key);
  CHECK(bm.busy());
  s.cache.build_begin();
  CHECK(bm.service_all(display));
  CHECK(bm.failed() == 0);
  CHECK(bm.completed() == 0);
  CHECK(display.accepted_uploads() == 0);
  while (!s.cache.build_step(1u << 30)) {
  }
  // after the build the view is rendered, from the entries the build made
  key.epoch = s.cache.epoch();
  bm.request(0, key);
  CHECK(bm.service_all(display));
  CHECK(bm.shows(0, key));
  std::vector<PeakPair> peaks(300);
  s.cache.columns(0, 0, 60000, 300, peaks.data());
  std::vector<std::uint8_t> want(WaveBitmap::stride_of(300, WaveDepth::L1) * 120);
  WaveBitmap::render_reference(peaks.data(), 300, 120, WaveDepth::L1, want.data());
  Ram ram;
  ram.apply(display);
  CHECK(std::memcmp(want.data(), ram.bytes.data() + bm.shown_addr(0), want.size()) == 0);
  // the sound is edited and the cache rebuilt: the picture of the old samples is not the new view's, though its key's other fields are
  const WaveKey old_key = bm.shown_key(0);
  auto* p = reinterpret_cast<std::int16_t*>(s.words.data());
  for (Frame f = 0; f < 60000; ++f) {
    p[f] = static_cast<std::int16_t>(p[f] / 4);
  }
  s.cache.build();
  WaveKey new_key = old_key;
  new_key.epoch = s.cache.epoch();
  CHECK(new_key.valid());
  CHECK(new_key != old_key);
  CHECK_FALSE(bm.shows(0, new_key));
  bm.request(0, new_key);
  CHECK(bm.busy());
  CHECK(bm.service_all(display));
  CHECK(bm.shows(0, new_key));
  s.cache.columns(0, 0, 60000, 300, peaks.data());
  std::vector<std::uint8_t> want_new(want.size());
  WaveBitmap::render_reference(peaks.data(), 300, 120, WaveDepth::L1, want_new.data());
  Ram ram_new;
  ram_new.apply(display);
  CHECK(std::memcmp(want_new.data(), ram_new.bytes.data() + bm.shown_addr(0), want.size()) == 0);
  CHECK(want_new != want);
}
