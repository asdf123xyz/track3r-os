// SPDX-License-Identifier: GPL-3.0-or-later
//
// Offline-render determinism and golden event dumps (OS-005c; ADR 0013 D8 "offline render: the same graph in pull mode, faster than real time"): the sequencer's events are
// the same, to the sample and the field, whatever the block size, so that an export of a project is the project; and a few synthetic patterns have their event streams written
// out in full, so that a change of behaviour shows as a changed line and not only as a changed hash. Host and Cortex-M4F (`qemu_seq_model`).
#include <doctest/doctest.h>

#include <o3os/seq/sequencer.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "support/need.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"

using namespace o3os;
using namespace o3os::test;
using audio::EventKind;
namespace fx = o3os::seq::fx;

namespace {

StaticSlot<seq::Sequencer>& g_slot = seq_slot();
Collector& g_col = seq_collector();

#if defined(__arm__)
constexpr bool kTarget = true;
#else
constexpr bool kTarget = false;
#endif

// A busy project: a song of three patterns of different lengths, polymetric tracks, every deterministic FX, a MIDI track, voice FX, swing and a tempo change.
void busy_project() {
  fresh_project(6, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(13300) == model::Status::Ok);
  CHECK(p.set_swing(58) == model::Status::Ok);
  CHECK(p.create_pattern(1, 12) == model::Status::Ok);
  CHECK(p.create_pattern(2, 7) == model::Status::Ok);
  CHECK(p.create_pattern(3, 16) == model::Status::Ok);
  CHECK(p.set_track_length(1, 1, 5) == model::Status::Ok);
  CHECK(p.set_track_length(3, 2, 9) == model::Status::Ok);
  CHECK(p.append_order(1) == model::Status::Ok);
  CHECK(p.append_order(2) == model::Status::Ok);
  CHECK(p.append_order(3) == model::Status::Ok);
  CHECK(p.append_order(2) == model::Status::Ok);
  std::uint32_t seed = 12345;
  const auto rnd = [&seed]() {
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
  };
  const std::uint32_t lens[3] = {12, 7, 16};
  for (std::uint32_t pid = 1; pid <= 3; ++pid) {
    for (std::uint32_t t = 0; t < 6; ++t) {
      for (std::uint32_t k = 0; k < lens[pid - 1]; ++k) {
        // The order of evaluation of arguments and operands is unspecified in C++, and the generator is a sequence: every draw is its own statement, so that the host and the
        // target build the same project (and so the same pinned hash).
        model::Step s = model::kEmptyStep;
        const std::uint32_t r = rnd() % 100;
        if (r < 45) {
          const std::uint8_t note = static_cast<std::uint8_t>(36 + rnd() % 48);
          const std::uint8_t instr = t == 5 ? std::uint8_t{11} : static_cast<std::uint8_t>(1 + rnd() % 4);
          s = make_step(note, instr);
        } else if (r < 52) {
          const std::uint32_t pick = rnd() % 3;
          const std::uint32_t pick2 = rnd() % 2;
          const std::uint8_t kind = pick == 0 ? model::kNoteOff : (pick2 == 0 ? model::kNoteCut : model::kNoteFade);
          s = special_step(kind, t == 5 ? std::uint8_t{11} : std::uint8_t{1});
        }
        for (int c = 0; c < 2; ++c) {
          if (rnd() % 100 < 30) {
            static const std::uint8_t types[] = {fx::kMicroMove, fx::kRoll, fx::kGate, fx::kVelocity, fx::kPanning, fx::kLowPass, fx::kGlide, fx::kCcA, fx::kTempo, fx::kSwing, fx::kSpeed,
                                                 fx::kBreakPattern, fx::kChance, fx::kRandomNote, fx::kRandomVelocity, 130};
            const std::uint8_t ty = types[rnd() % sizeof types];
            std::uint8_t v = static_cast<std::uint8_t>(rnd() % 101);
            if (ty == fx::kRoll) {
              const std::uint32_t period = 1 + rnd() % 8;
              const std::uint32_t dir = rnd() % 6;
              v = static_cast<std::uint8_t>(period + 9 * dir);
            } else if (ty == fx::kTempo) {
              v = static_cast<std::uint8_t>(30 + rnd() % 80);
            } else if (ty == fx::kSwing) {
              v = static_cast<std::uint8_t>(25 + rnd() % 51);
            } else if (ty == fx::kSpeed) {
              v = static_cast<std::uint8_t>(1 + rnd() % 12);
            } else if (ty == fx::kBreakPattern) {
              if (rnd() % 4 != 0) {
                continue;  // a break now and then
              }
              v = 1;
            }
            s.fx[c] = {ty, v};
          }
        }
        CHECK(p.set_step(pid, t, k, s) == model::Status::Ok);
      }
    }
  }
}

}  // namespace

TEST_CASE("determinism: the events are the same whatever the block size, to the sample and the field") {
  busy_project();
  seq::Sequencer::Config cfg;
  cfg.emit_clock = true;
  cfg.emit_ticks = true;
  cfg.seed = 99;
  std::uint64_t run_to = kTarget ? 120000 : 700000;
  if (std::getenv("O3OS_GOLDEN_RUN") != nullptr) {
    run_to = static_cast<std::uint64_t>(std::atoll(std::getenv("O3OS_GOLDEN_RUN")));  // to work out the pinned figures of the other platform
  }
  const std::uint32_t sizes_single[] = {64, 1, 2, 7, 33, 100, 128, 255, 1000};
  std::uint64_t ref_hash = 0;
  std::uint32_t ref_count = 0;
  const std::uint32_t n_sizes = kTarget ? 6u : 9u;  // block size 1 is a million calls: not on the target
  for (std::uint32_t i = 0; i < n_sizes; ++i) {
    const std::uint32_t bs = sizes_single[i];
    if (kTarget && bs == 1u) {
      continue;
    }
    seq::Sequencer& s = g_slot.make(seq_project());
    s.configure(cfg);
    g_col.n = 0;
    g_col.total_events = 0;
    g_col.hash = 0xCBF29CE484222325ull;
    g_col.keep = false;
    s.play_song(0);
    // a larger array than the default: a big block holds many events
    static audio::SeqEvent out[1024];
    for (std::uint64_t t = 0; t < run_to;) {
      const std::uint32_t n = static_cast<std::uint32_t>(run_to - t < bs ? run_to - t : bs);
      const std::uint32_t c = s.step(t, n, out, 1024);
      for (std::uint32_t k = 0; k < c; ++k) {
        g_col.add(t, out[k]);
      }
      t += n;
    }
    CHECK_MESSAGE(s.stats().late_events == 0u, bs);
    CHECK_MESSAGE(s.stats().deferred == 0u, bs);
    CHECK_MESSAGE(s.stats().queue_overflow == 0u, bs);
    if (ref_count == 0u) {
      ref_hash = g_col.hash;
      ref_count = g_col.total_events;
      CHECK(ref_count > (kTarget ? 300u : 2000u));
    } else {
      CHECK_MESSAGE(g_col.hash == ref_hash, bs);
      CHECK_MESSAGE(g_col.total_events == ref_count, bs);
    }
  }
  g_col.keep = true;
  // a cycle of mixed sizes, and a random walk of sizes
  {
    seq::Sequencer& s = g_slot.make(seq_project());
    s.configure(cfg);
    g_col.n = 0;
    g_col.total_events = 0;
    g_col.hash = 0xCBF29CE484222325ull;
    g_col.keep = false;
    s.play_song(0);
    static audio::SeqEvent out[1024];
    std::uint32_t x = 7;
    for (std::uint64_t t = 0; t < run_to;) {
      x = x * 1103515245u + 12345u;
      const std::uint32_t want = 1u + ((x >> 16) % 300u);
      const std::uint32_t n = static_cast<std::uint32_t>(run_to - t < want ? run_to - t : want);
      const std::uint32_t c = s.step(t, n, out, 1024);
      for (std::uint32_t k = 0; k < c; ++k) {
        g_col.add(t, out[k]);
      }
      t += n;
    }
    CHECK(g_col.hash == ref_hash);
    CHECK(g_col.total_events == ref_count);
    g_col.keep = true;
  }
  // the hash of the busy run is pinned: a change of behaviour of ANY event shows here (the seeded random FX are part of it). One set of figures per clock: the
  // 44,100 Hz default and the pre-1.0 4080 cycles a sample (-DO3OS_RATE_44100=OFF) are other grids. The per-pattern text goldens below are the 4080 clock's only
  // (a dump of sample times per clock would double them): the 44,100 Hz build is covered by this hash, and `tools/ci.sh host-off` runs the texts.
  if (std::getenv("O3OS_GOLDEN") != nullptr) {
    std::printf("GOLDEN busy hash 0x%016llx events %u (run %llu)\n", static_cast<unsigned long long>(ref_hash), static_cast<unsigned>(ref_count), static_cast<unsigned long long>(run_to));
  }
  if (std::getenv("O3OS_GOLDEN_RUN") == nullptr) {
#if defined(O3OS_RATE_44100) && O3OS_RATE_44100
    CHECK(ref_hash == (kTarget ? 0x75b7bbc971a2077bull : 0x27b8e6cc7425c380ull));
    CHECK(ref_count == (kTarget ? 451u : 2349u));
#else
    CHECK(ref_hash == (kTarget ? 0x730f9d852529aa9cull : 0x1308868e7c158a3full));
    CHECK(ref_count == (kTarget ? 451u : 2338u));
#endif
  }
}

TEST_CASE("determinism: a tempo ParamChange at a block boundary common to the partitions gives the same events; a seed gives another run") {
  busy_project();
  seq::Sequencer::Config cfg;
  cfg.emit_clock = true;
  std::uint64_t ref = 0;
  const std::uint32_t sizes[] = {64, 128, 256, 512, 32, 16};
  for (std::uint32_t i = 0; i < 6; ++i) {
    seq::Sequencer& s = g_slot.make(seq_project());
    s.configure(cfg);
    g_col.n = 0;
    g_col.total_events = 0;
    g_col.hash = 0xCBF29CE484222325ull;
    g_col.keep = false;
    s.play_song(0);
    static audio::SeqEvent out[1024];
    const std::uint64_t run_to = kTarget ? 60000 : 300000;
    for (std::uint64_t t = 0; t < run_to;) {
      if (t == 16384) {
        CHECK(s.apply(core::ParamChange{seq::tempo_param(), 150.0f, 1}));
        CHECK(s.apply(core::ParamChange{seq::swing_param(), 40.0f, 1}));
      }
      const std::uint32_t n = static_cast<std::uint32_t>(run_to - t < sizes[i] ? run_to - t : sizes[i]);
      const std::uint32_t c = s.step(t, n, out, 1024);
      for (std::uint32_t k = 0; k < c; ++k) {
        g_col.add(t, out[k]);
      }
      t += n;
    }
    if (i == 0) {
      ref = g_col.hash;
    } else {
      CHECK_MESSAGE(g_col.hash == ref, sizes[i]);
    }
  }
  g_col.keep = true;
  // another seed is another run (the random FX), the same seed the same run
  std::uint64_t h[3] = {0, 0, 0};
  const std::uint32_t seeds[3] = {5, 6, 5};
  for (int i = 0; i < 3; ++i) {
    cfg.seed = seeds[i];
    seq::Sequencer& s = g_slot.make(seq_project());
    s.configure(cfg);
    g_col.n = 0;
    g_col.total_events = 0;
    g_col.hash = 0xCBF29CE484222325ull;
    g_col.keep = false;
    s.play_song(0);
    g_col.run(s, 0, 100000, 64);
    h[i] = g_col.hash;
  }
  g_col.keep = true;
  CHECK(h[0] != h[1]);
  CHECK(h[0] == h[2]);
}


namespace {

// Compares a dump with its golden text, line by line, and says which line differs; with O3OS_GOLDEN set it prints the dump instead (to write a new golden).
void check_golden(const char* name, const Collector& c, std::uint64_t skip_mask, const char* golden) {
#if defined(O3OS_RATE_44100) && O3OS_RATE_44100
  (void)name; (void)c; (void)skip_mask; (void)golden;  // the goldens are the default clock's grid (4080 cycles a sample)
  return;
#endif
  static char buf[16000];
  dump(c, buf, sizeof buf, skip_mask);
  if (std::getenv("O3OS_GOLDEN") != nullptr) {
    std::printf("GOLDEN-BEGIN %s\n%sGOLDEN-END\n", name, buf);
    return;
  }
  const char* a = buf;
  const char* b = golden;
  unsigned line = 1;
  while (*a != '\0' || *b != '\0') {
    const char* ae = std::strchr(a, '\n');
    const char* be = std::strchr(b, '\n');
    const std::size_t al = ae != nullptr ? static_cast<std::size_t>(ae - a) : std::strlen(a);
    const std::size_t bl = be != nullptr ? static_cast<std::size_t>(be - b) : std::strlen(b);
    if (al != bl || std::memcmp(a, b, al) != 0) {
      char ga[96], gb[96];
      std::snprintf(ga, sizeof ga, "%.*s", static_cast<int>(al < 90 ? al : 90), a);
      std::snprintf(gb, sizeof gb, "%.*s", static_cast<int>(bl < 90 ? bl : 90), b);
      INFO(name << " line " << line << ": got [" << ga << "] want [" << gb << "]");
      CHECK(false);
      return;
    }
    a += al + (ae != nullptr ? 1 : 0);
    b += bl + (be != nullptr ? 1 : 0);
    ++line;
  }
  CHECK(true);
}

constexpr std::uint64_t kNoise = (std::uint64_t{1} << static_cast<unsigned>(EventKind::MidiClock)) | (std::uint64_t{1} << static_cast<unsigned>(EventKind::Tick)) |
                                  (std::uint64_t{1} << static_cast<unsigned>(EventKind::FxReset));

}  // namespace

TEST_CASE("golden: swing and micro-move on a straight pattern") {
  fresh_project(4, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
  CHECK(p.set_swing(60) == model::Status::Ok);
  CHECK(p.create_pattern(1, 8) == model::Status::Ok);
  for (std::uint32_t k = 0; k < 8; ++k) {
    CHECK(p.set_step(1, 0, k, make_step(static_cast<std::uint8_t>(60 + k))) == model::Status::Ok);
  }
  CHECK(p.set_step(1, 1, 0, make_step(48, 2, fx::kMicroMove, 50)) == model::Status::Ok);
  CHECK(p.set_step(1, 1, 3, make_step(50, 2, fx::kMicroMove, 100, fx::kVelocity, 80)) == model::Status::Ok);
  seq::Sequencer& s = g_slot.make(p);
  s.play_pattern(1);
  g_col.n = 0;
  g_col.run(s, 0, 33200, 64);
  check_golden("swing", g_col, kNoise, R"GOLD(0 STEP 255 1 255 255 0 0
0 ON 0 60 0 255 0 0
2482 ON 1 48 1 255 0 0
4963 STEP 255 1 255 255 0 1
4963 ON 0 61 0 255 0 0
11029 STEP 255 1 255 255 0 2
11029 ON 0 62 0 255 0 0
15993 STEP 255 1 255 255 0 3
15993 ON 0 63 0 255 0 0
21806 ON 1 50 1 101 0 0
22059 STEP 255 1 255 255 0 4
22059 ON 0 64 0 255 0 0
27022 STEP 255 1 255 255 0 5
27022 ON 0 65 0 255 0 0
33088 STEP 255 1 255 255 0 6
33088 ON 0 66 0 255 0 0
)GOLD");
}

TEST_CASE("golden: roll, gate, off, cut and fade") {
  fresh_project(4, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
  CHECK(p.create_pattern(1, 8) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 0, make_step(60, 1, fx::kRoll, 4)) == model::Status::Ok);               // 1/4, constant
  CHECK(p.set_step(1, 0, 2, make_step(62, 1, fx::kGate, 50)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 3, make_step(64, 1, fx::kRoll, 9 * 4 + 5)) == model::Status::Ok);        // 1/6, note up
  CHECK(p.set_step(1, 0, 5, special_step(model::kNoteOff)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 6, make_step(67, 1, fx::kRoll, 9 * 2 + 4, 0, 0, 100)) == model::Status::Ok);  // volume up, 1/4, velocity 100
  CHECK(p.set_step(1, 0, 7, special_step(model::kNoteCut)) == model::Status::Ok);
  CHECK(p.set_step(1, 1, 1, make_step(50, 2)) == model::Status::Ok);
  CHECK(p.set_step(1, 1, 2, special_step(model::kNoteFade)) == model::Status::Ok);
  seq::Sequencer& s = g_slot.make(p);
  s.play_pattern(1);
  g_col.n = 0;
  g_col.run(s, 0, 44200, 64);
  check_golden("roll", g_col, kNoise, R"GOLD(0 STEP 255 1 255 255 0 0
0 ON 0 60 0 255 0 0
689 OFF 0 60 0 0 0 0
1494 ON 0 60 0 255 1 0
2183 OFF 0 60 0 0 0 0
2872 ON 0 60 0 255 1 0
3562 OFF 0 60 0 0 0 0
4251 ON 0 60 0 255 1 0
4940 OFF 0 60 0 0 0 0
5515 STEP 255 1 255 255 0 1
5515 ON 1 50 1 255 0 0
5630 ON 0 60 0 255 1 0
6319 OFF 0 60 0 0 0 0
7008 ON 0 60 0 255 1 0
7698 OFF 0 60 0 0 0 0
8387 ON 0 60 0 255 1 0
9076 OFF 0 60 0 0 0 0
9766 ON 0 60 0 255 1 0
10455 OFF 0 60 0 0 0 0
11029 STEP 255 1 255 255 0 2
11029 ON 0 62 0 255 0 0
11029 FADE 1 0 0 0 0 0
13787 OFF 0 62 0 0 0 0
16544 STEP 255 1 255 255 0 3
16544 ON 0 64 0 255 0 0
17004 OFF 0 64 0 0 0 0
17578 ON 0 65 0 255 1 0
18038 OFF 0 65 0 0 0 0
18497 ON 0 66 0 255 1 0
18957 OFF 0 66 0 0 0 0
19416 ON 0 67 0 255 1 0
19876 OFF 0 67 0 0 0 0
20335 ON 0 68 0 255 1 0
20795 OFF 0 68 0 0 0 0
21255 ON 0 69 0 255 1 0
21714 OFF 0 69 0 0 0 0
22059 STEP 255 1 255 255 0 4
22174 ON 0 70 0 255 1 0
22633 OFF 0 70 0 0 0 0
23093 ON 0 71 0 255 1 0
23552 OFF 0 71 0 0 0 0
24012 ON 0 72 0 255 1 0
24472 OFF 0 72 0 0 0 0
24931 ON 0 73 0 255 1 0
25391 OFF 0 73 0 0 0 0
25850 ON 0 74 0 255 1 0
26310 OFF 0 74 0 0 0 0
26769 ON 0 75 0 255 1 0
27229 OFF 0 75 0 0 0 0
27574 STEP 255 1 255 255 0 5
27574 OFF 0 75 0 0 0 0
33088 STEP 255 1 255 255 0 6
33088 ON 0 67 0 0 0 0
33778 OFF 0 67 0 0 0 0
34582 ON 0 67 0 27 1 0
35271 OFF 0 67 0 0 0 0
35960 ON 0 67 0 52 1 0
36650 OFF 0 67 0 0 0 0
37339 ON 0 67 0 77 1 0
38028 OFF 0 67 0 0 0 0
38603 STEP 255 1 255 255 0 7
38603 CUT 0 0 0 0 0 0
40097 ON 0 67 0 27 1 0
40786 OFF 0 67 0 0 0 0
41475 ON 0 67 0 52 1 0
42165 OFF 0 67 0 0 0 0
42854 ON 0 67 0 77 1 0
43543 OFF 0 67 0 0 0 0
44118 PAT 255 1 255 255 0 8
44118 STEP 255 1 255 255 0 0
44118 ON 0 60 0 255 0 0
)GOLD");
}

TEST_CASE("golden: a song with a tempo FX, a swing FX and a break") {
  fresh_project(4, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(24000) == model::Status::Ok);
  CHECK(p.create_pattern(1, 4) == model::Status::Ok);
  CHECK(p.create_pattern(2, 6) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 0, make_step(61)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 2, make_step(63, 1, fx::kTempo, 60)) == model::Status::Ok);   // 120 BPM from step 2
  CHECK(p.set_step(2, 0, 0, make_step(62)) == model::Status::Ok);
  CHECK(p.set_step(2, 0, 1, make_step(64, 1, fx::kSwing, 70)) == model::Status::Ok);
  CHECK(p.set_step(2, 0, 3, make_step(65, 1, fx::kBreakPattern, 1)) == model::Status::Ok);
  CHECK(p.append_order(1) == model::Status::Ok);
  CHECK(p.append_order(2) == model::Status::Ok);
  seq::Sequencer::Config cfg;
  cfg.song_loop = false;
  seq::Sequencer& s = g_slot.make(p);
  s.configure(cfg);
  s.play_song(0);
  g_col.n = 0;
  g_col.run(s, 0, 40000, 64);
  check_golden("song", g_col, kNoise, R"GOLD(0 STEP 255 1 0 0 0 0
0 ON 0 61 0 255 0 0
2757 STEP 255 1 0 0 0 1
5515 STEP 255 1 0 0 0 2
5515 ON 0 63 0 255 0 0
11029 STEP 255 1 0 0 0 3
16544 PAT 255 2 1 0 0 6
16544 STEP 255 2 1 0 0 0
16544 ON 0 62 0 255 0 0
22059 STEP 255 2 1 0 0 1
22059 ON 0 64 0 255 0 0
28676 STEP 255 2 1 0 0 2
33088 STEP 255 2 1 0 0 3
33088 ON 0 65 0 255 0 0
39706 SONGEND 255 0 0 0 0 0
39706 OFF 0 65 0 0 0 0
39706 STOPPED 255 0 0 0 0 0
)GOLD");
}

TEST_CASE("golden: a MIDI track, the CC rows and the voice FX") {
  fresh_project(4, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
  CHECK(p.create_pattern(1, 8) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 0, make_step(60, 11)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 1, make_step(62, 11, fx::kCcA, 99)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 2, special_step(model::kNoteOff, 11)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 3, make_step(64, 11)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 4, make_step(65, 1)) == model::Status::Ok);
  CHECK(p.set_step(1, 1, 0, make_step(50, 1, fx::kPanning, 20, fx::kLowPass, 70)) == model::Status::Ok);
  CHECK(p.set_step(1, 1, 2, fx_step(fx::kPanning, 90)) == model::Status::Ok);
  CHECK(p.set_step(1, 1, 3, fx_step(fx::kOff, 0)) == model::Status::Ok);
  CHECK(p.set_step(1, 1, 5, make_step(52, 1, 130, 7)) == model::Status::Ok);
  seq::Sequencer::Config cfg;
  cfg.emit_clock = true;
  seq::Sequencer& s = g_slot.make(p);
  s.configure(cfg);
  s.play_pattern(1);
  g_col.n = 0;
  g_col.run(s, 0, 40000, 64);
  s.stop();
  g_col.run(s, 40000, 40064, 64);
  check_golden("midi", g_col, kNoise & ~(std::uint64_t{1} << static_cast<unsigned>(EventKind::FxReset)) & ~(std::uint64_t{1} << static_cast<unsigned>(EventKind::MidiClock)), R"GOLD(0 FXRESET 255 0 0 0 0 0
0 MSTART 255 0 0 0 0 0
0 STEP 255 1 255 255 0 0
0 MON 0 60 127 2 0 0
0 MOD 1 31 20 0 2 0
0 MOD 1 27 70 1 5 0
0 ON 1 50 0 255 0 0
0 MCLK 255 0 0 0 0 0
919 MCLK 255 0 0 0 0 505290264
1838 MCLK 255 0 0 0 0 1010580528
2757 MCLK 255 0 0 0 0 1515870792
3676 MCLK 255 0 0 0 0 2021161056
4596 MCLK 255 0 0 0 0 -1768515976
5515 STEP 255 1 255 255 0 1
5515 MCC 0 0 99 255 0 0
5515 MOFF 0 60 0 2 0 0
5515 MON 0 62 127 2 0 0
5515 MCLK 255 0 0 0 0 -1263225712
6434 MCLK 255 0 0 0 0 -757935448
7353 MCLK 255 0 0 0 0 -252645184
8272 MCLK 255 0 0 0 0 252645080
9191 MCLK 255 0 0 0 0 757935344
10110 MCLK 255 0 0 0 0 1263225608
11029 STEP 255 1 255 255 0 2
11029 MOFF 0 62 0 2 0 0
11029 MOD 1 31 90 0 2 0
11029 MCLK 255 0 0 0 0 1768515872
11949 MCLK 255 0 0 0 0 -2021161160
12868 MCLK 255 0 0 0 0 -1515870896
13787 MCLK 255 0 0 0 0 -1010580632
14706 MCLK 255 0 0 0 0 -505290368
15625 MCLK 255 0 0 0 0 -104
16544 STEP 255 1 255 255 0 3
16544 MON 0 64 127 2 0 0
16544 MODEND 1 0 0 0 2 0
16544 MCLK 255 0 0 0 0 505290160
17463 MCLK 255 0 0 0 0 1010580424
18382 MCLK 255 0 0 0 0 1515870688
19301 MCLK 255 0 0 0 0 2021160952
20221 MCLK 255 0 0 0 0 -1768516080
21140 MCLK 255 0 0 0 0 -1263225816
22059 STEP 255 1 255 255 0 4
22059 MOFF 0 64 0 2 0 0
22059 ON 0 65 0 255 0 0
22059 MCLK 255 0 0 0 0 -757935552
22978 MCLK 255 0 0 0 0 -252645288
23897 MCLK 255 0 0 0 0 252644976
24816 MCLK 255 0 0 0 0 757935240
25735 MCLK 255 0 0 0 0 1263225504
26654 MCLK 255 0 0 0 0 1768515768
27574 STEP 255 1 255 255 0 5
27574 MOD 1 130 7 0 17 0
27574 MODEND 1 0 0 0 5 0
27574 ON 1 52 0 255 0 0
27574 MCLK 255 0 0 0 0 -2021161264
28493 MCLK 255 0 0 0 0 -1515871000
29412 MCLK 255 0 0 0 0 -1010580736
30331 MCLK 255 0 0 0 0 -505290472
31250 MCLK 255 0 0 0 0 -208
32169 MCLK 255 0 0 0 0 505290056
33088 STEP 255 1 255 255 0 6
33088 MCLK 255 0 0 0 0 1010580320
34007 MCLK 255 0 0 0 0 1515870584
34926 MCLK 255 0 0 0 0 2021160848
35846 MCLK 255 0 0 0 0 -1768516184
36765 MCLK 255 0 0 0 0 -1263225920
37684 MCLK 255 0 0 0 0 -757935656
38603 STEP 255 1 255 255 0 7
38603 MCLK 255 0 0 0 0 -252645392
39522 MCLK 255 0 0 0 0 252644872
40000 OFF 0 65 0 0 0 0
40000 OFF 1 52 0 0 0 0
40000 MSTOP 255 0 0 0 0 0
40000 STOPPED 255 0 0 0 0 0
)GOLD");
}

// OS-CPU10 slice 2 (B2): a track with nothing due (no open step or note, no delayed start, no MIDI note to end, not the first microstep) does no work in run_microstep. The busy song above
// has delayed starts, rolls and gates; this run adds what touches the skipped state from outside: the MIDI track muted while its note sounds, a solo, the Perform stutter on a track. Its
// event stream is pinned on the code BEFORE the skip (dev a8abd929): the skip changes no event.
TEST_CASE("determinism: mutes, a solo and a stutter in the busy song give the pinned event stream (the idle-track skip changes no event)") {
  busy_project();
  model::Project& p = seq_project();
  seq::Sequencer::Config cfg;
  cfg.emit_clock = true;
  cfg.emit_ticks = true;
  cfg.seed = 99;
  std::uint64_t run_to = kTarget ? 120000 : 700000;
  if (std::getenv("O3OS_GOLDEN_RUN") != nullptr) {
    run_to = static_cast<std::uint64_t>(std::atoll(std::getenv("O3OS_GOLDEN_RUN")));  // to work out the pinned figures of the other platform
  }
  seq::Sequencer& s = g_slot.make(p);
  s.configure(cfg);
  g_col.n = 0;
  g_col.total_events = 0;
  g_col.hash = 0xCBF29CE484222325ull;
  g_col.keep = false;
  s.play_song(0);
  static audio::SeqEvent out[1024];
  const auto flags = [&p](std::uint32_t t, std::uint8_t f) {
    model::TrackSlot ts = *p.track(t);
    ts.flags = f;
    CHECK(p.set_track(t, ts) == model::Status::Ok);
  };
  std::uint32_t blk = 0;
  for (std::uint64_t t = 0; t < run_to; ++blk) {
    if (blk % 97u == 13u) flags(5, model::kTrackMute);
    if (blk % 97u == 40u) flags(5, 0);
    if (blk % 131u == 20u) flags(1, model::kTrackMute);
    if (blk % 131u == 70u) flags(1, 0);
    if (blk % 211u == 100u) flags(2, model::kTrackSolo);
    if (blk % 211u == 150u) flags(2, 0);
    if (blk % 83u == 10u) s.set_perf_stutter(3, 1u + blk % 15u);
    if (blk % 83u == 50u) s.set_perf_stutter(3, 0);
    const std::uint32_t n = static_cast<std::uint32_t>(run_to - t < 64u ? run_to - t : 64u);
    const std::uint32_t c = s.step(t, n, out, 1024);
    for (std::uint32_t k = 0; k < c; ++k) {
      g_col.add(t, out[k]);
    }
    t += n;
  }
  CHECK(s.stats().late_events == 0u);
  // one set of figures per clock (the 44,100 Hz default; 4080 cycles a sample with -DO3OS_RATE_44100=OFF)
#if defined(O3OS_RATE_44100) && O3OS_RATE_44100
  const std::uint64_t want = kTarget ? 0x0d56a4b8c750af5aull : 0xfd9b35b65938b4f1ull;
  const std::uint32_t want_events = kTarget ? 477u : 2472u;
#else
  const std::uint64_t want = kTarget ? 0xa861e826e88360b7ull : 0xabfb47322721579cull;
  const std::uint32_t want_events = kTarget ? 477u : 2453u;
#endif
  if (std::getenv("O3OS_GOLDEN_RUN") == nullptr) {
    CHECK_MESSAGE(g_col.hash == want, "mute digest changed");
    CHECK_MESSAGE(g_col.total_events == want_events, "mute event count");
  }
  if (g_col.hash != want || g_col.total_events != want_events) {
    std::printf("DIGEST mutes 0x%08x%08x events %u\n", static_cast<unsigned>(g_col.hash >> 32), static_cast<unsigned>(g_col.hash & 0xFFFFFFFFu), static_cast<unsigned>(g_col.total_events));
  }
  g_col.keep = true;
}
