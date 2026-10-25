// SPDX-License-Identifier: GPL-3.0-or-later
//
// `diag cache check` (cache_check.h; OS-PERF3): the pattern of the write-hit step, what it must read back, how a line is judged, the verdict of each sub-check and the lines a test
// session greps. Host and Cortex-M4F (`qemu_mk66`). The accesses (bench.cpp) need the cache and the SDRAM and are the unit's `diag cache check` item.
#include <doctest/doctest.h>

#include <o3os/platform/mk66/cache_check.h>

#include <cstdint>
#include <cstring>

namespace bench = o3os::platform::mk66::bench;

namespace {

constexpr std::uint32_t kLines = 1024;  // more lines than the unit's block: the byte functions wrap at 256, the pattern must hold for every residue

// A line as sixteen bytes, and as four little-endian words (the unit and the tests are little-endian).
struct Line {
  std::uint8_t b[16];
  std::uint32_t word(unsigned w) const {
    std::uint32_t v;
    std::memcpy(&v, b + w * 4u, 4);
    return v;
  }
  void words(std::uint32_t out[4]) const {
    for (unsigned w = 0; w < 4; ++w) {
      out[w] = word(w);
    }
  }
};

Line base_line(std::uint32_t line) {
  Line l;
  for (unsigned p = 0; p < 16; ++p) {
    l.b[p] = bench::hit_base_byte(line, p);
  }
  return l;
}

// What the stores of the plan do to a line, byte by byte through memcpy of the width the plan says: the reference the word-level expectations are checked against.
void apply_plan(Line& l, std::uint32_t line) {
  for (unsigned op = 0; op < bench::kHitPlanCount; ++op) {
    const std::uint32_t v = bench::hit_store_value(line, op);
    const unsigned off = bench::kHitPlan[op].offset;
    switch (bench::kHitPlan[op].width) {
      case 4: {
        std::uint32_t x = v;
        std::memcpy(l.b + off, &x, 4);
        break;
      }
      case 2: {
        std::uint16_t x = static_cast<std::uint16_t>(v);
        std::memcpy(l.b + off, &x, 2);
        break;
      }
      default:
        l.b[off] = static_cast<std::uint8_t>(v);
        break;
    }
  }
}

}  // namespace

TEST_CASE("cache check: the plan stores bytes 0..13 once each, aligned, at widths 32, 16 and 8 bits, and leaves two bytes of every line") {
  CHECK(bench::hit_plan_valid());
  unsigned w32 = 0, w16 = 0, w8 = 0;
  for (unsigned i = 0; i < bench::kHitPlanCount; ++i) {
    const unsigned w = bench::kHitPlan[i].width;
    w32 += w == 4u;
    w16 += w == 2u;
    w8 += w == 1u;
  }
  CHECK(w32 == 1u);
  CHECK(w16 == 3u);
  CHECK(w8 == 4u);
  CHECK(bench::kHitKeep == 14u);
  CHECK(bench::kHitProbe == 15u);
}

TEST_CASE("cache check: a store that did not happen, or landed at the wrong offset, always shows") {
  for (std::uint32_t line = 0; line < kLines; ++line) {
    INFO("line " << line);
    bool new_differs = true;
    bool base_distinct = true;
    bool new_distinct = true;
    for (unsigned p = 0; p < 16; ++p) {
      new_differs = new_differs && bench::hit_new_byte(line, p) != bench::hit_base_byte(line, p);
      for (unsigned q = p + 1; q < 16; ++q) {
        base_distinct = base_distinct && bench::hit_base_byte(line, p) != bench::hit_base_byte(line, q);
        new_distinct = new_distinct && bench::hit_new_byte(line, p) != bench::hit_new_byte(line, q);
      }
    }
    CHECK(new_differs);      // every stored byte differs from the byte it replaces
    CHECK(base_distinct);    // the sixteen bytes of the base are pairwise different, so are the new ones: a store at the wrong offset is a different byte there
    CHECK(new_distinct);
    CHECK(bench::hit_probe_byte(line) != bench::hit_base_byte(line, bench::kHitProbe));  // the probe is a change
  }
}

TEST_CASE("cache check: the word images are the plan applied to the base, byte for byte") {
  for (std::uint32_t line = 0; line < kLines; ++line) {
    Line base = base_line(line);
    for (unsigned w = 0; w < 4; ++w) {
      CHECK(base.word(w) == bench::hit_word(line, w, bench::HitImage::Base));
    }
    Line stored = base;
    apply_plan(stored, line);
    // the bytes the plan does not store are the base's
    CHECK(stored.b[bench::kHitKeep] == base.b[bench::kHitKeep]);
    CHECK(stored.b[bench::kHitProbe] == base.b[bench::kHitProbe]);
    // Cached: the stores in a line whose last byte the cache still has as it was
    for (unsigned w = 0; w < 4; ++w) {
      CHECK(stored.word(w) == bench::hit_word(line, w, bench::HitImage::Cached));
    }
    // Memory: the same, with the probe byte put there behind the cache
    Line memory = stored;
    memory.b[bench::kHitProbe] = bench::hit_probe_byte(line);
    for (unsigned w = 0; w < 4; ++w) {
      CHECK(memory.word(w) == bench::hit_word(line, w, bench::HitImage::Memory));
    }
  }
}

TEST_CASE("cache check: a line the cache held, a line it did not, and a line that is wrong") {
  for (std::uint32_t line = 0; line < kLines; ++line) {
    std::uint32_t held[4];
    std::uint32_t miss[4];
    for (unsigned w = 0; w < 4; ++w) {
      held[w] = bench::hit_word(line, w, bench::HitImage::Cached);
      miss[w] = bench::hit_word(line, w, bench::HitImage::Memory);
    }
    const bench::HitJudge h = bench::hit_judge_alias(line, held);
    CHECK(h.bad == 0u);
    CHECK(h.held);
    const bench::HitJudge m = bench::hit_judge_alias(line, miss);
    CHECK(m.bad == 0u);  // evicted before the store: memory has everything, no failure
    CHECK_FALSE(m.held);
    CHECK(bench::hit_judge_memory(line, miss) == 0u);
    CHECK(bench::hit_judge_memory(line, held) == 8u);  // memory must have the probe: a stale last byte is word 3
  }
}

TEST_CASE("cache check: every word of a line that missed its update is reported against its own width") {
  const std::uint32_t line = 37;
  // The cache did not take the stores of word `w`: it still has the base there.
  for (unsigned w = 0; w < 4; ++w) {
    std::uint32_t got[4];
    for (unsigned i = 0; i < 4; ++i) {
      got[i] = bench::hit_word(line, i, bench::HitImage::Cached);
    }
    got[w] = bench::hit_word(line, w, bench::HitImage::Base);
    const bench::HitJudge j = bench::hit_judge_alias(line, got);
    CHECK(j.bad == (1u << w));
    // The probe byte of the base word 3 is the "held" byte, but its other bytes are not the stores'.
    std::uint32_t mem[4];
    for (unsigned i = 0; i < 4; ++i) {
      mem[i] = bench::hit_word(line, i, bench::HitImage::Memory);
    }
    mem[w] = bench::hit_word(line, w, bench::HitImage::Base);
    CHECK(bench::hit_judge_memory(line, mem) == (1u << w));
  }
  // A garbage last byte is wrong, not "evicted".
  std::uint32_t got[4];
  for (unsigned i = 0; i < 4; ++i) {
    got[i] = bench::hit_word(line, i, bench::HitImage::Cached);
  }
  got[3] ^= 0x01000000u;
  CHECK(bench::hit_judge_alias(line, got).bad == 8u);
  CHECK_FALSE(bench::hit_judge_alias(line, got).held);
}

TEST_CASE("cache check: faults of a cache that handles sub-word write hits badly are caught") {
  // Each model is a plausible defect of a write hit. The plan must expose every one of them in the word that holds the access.
  for (std::uint32_t line = 0; line < kLines; ++line) {
    const Line base = base_line(line);
    // A. an 8-bit store hit replaces the whole word with the byte zero-extended (the neighbours are lost): word 2 ends as the last byte store alone
    {
      std::uint32_t got[4];
      for (unsigned w = 0; w < 4; ++w) {
        got[w] = bench::hit_word(line, w, bench::HitImage::Cached);
      }
      got[2] = bench::hit_new_byte(line, 11);
      CHECK(bench::hit_judge_alias(line, got).bad == 4u);
    }
    // B. a 16-bit store hit is dropped (the line keeps the base there): words 1 and 3
    {
      Line l = base;
      apply_plan(l, line);
      l.b[4] = base.b[4];
      l.b[5] = base.b[5];
      std::uint32_t got[4];
      l.words(got);
      CHECK((bench::hit_judge_alias(line, got).bad & 2u) != 0u);
    }
    // C. a 32-bit store hit lands one word up
    {
      Line l = base;
      apply_plan(l, line);
      std::uint32_t got[4];
      l.words(got);
      got[1] = bench::hit_word(line, 0, bench::HitImage::Cached);
      CHECK((bench::hit_judge_alias(line, got).bad & 3u) != 0u);
    }
    // D. a store hit refills the whole line from memory first (the probe byte turns fresh) and drops the stores: every word of the stores is wrong
    {
      std::uint32_t got[4];
      for (unsigned w = 0; w < 4; ++w) {
        got[w] = bench::hit_word(line, w, bench::HitImage::Base);
      }
      got[3] = (got[3] & 0x00FFFFFFu) | (static_cast<std::uint32_t>(bench::hit_probe_byte(line)) << 24);
      CHECK(bench::hit_judge_alias(line, got).bad == 0xFu);
    }
  }
}

TEST_CASE("cache check: counts and the thresholds of the verdicts") {
  bench::HitCounts c;
  c.lines = 128;
  c.held = 128;
  CHECK(bench::hit_bad_total(c) == 0u);
  bench::hit_count(c, 0u);
  CHECK(bench::hit_bad_total(c) == 0u);
  bench::hit_count(c, 0x5u);  // words 0 and 2
  CHECK(c.bad[0] == 1u);
  CHECK(c.bad[1] == 0u);
  CHECK(c.bad[2] == 1u);
  CHECK(c.bad[3] == 0u);
  CHECK(bench::hit_bad_total(c) == 2u);

  bench::HitCounts ok;
  ok.lines = 128;
  ok.held = 128;
  CHECK(bench::judge_hit_alias(true, ok) == bench::Verdict::Pass);
  CHECK(bench::judge_hit_clean(true, ok) == bench::Verdict::Pass);
  CHECK(bench::judge_hit_alias(false, ok) == bench::Verdict::Skip);
  CHECK(bench::judge_hit_clean(false, ok) == bench::Verdict::Skip);
  CHECK(bench::judge_hit_alias(true, c) == bench::Verdict::Fail);
  CHECK(bench::judge_hit_clean(true, c) == bench::Verdict::Fail);
  bench::HitCounts based = ok;
  based.base_bad = 1;
  CHECK(bench::judge_hit_alias(true, based) == bench::Verdict::Fail);
  CHECK(bench::judge_hit_clean(true, based) == bench::Verdict::Pass);  // (the base reading is the alias step's)
  // more than half of the lines must have been held: 64 of 128 is not enough, 65 is
  bench::HitCounts few = ok;
  few.held = 64;
  CHECK(bench::judge_hit_alias(true, few) == bench::Verdict::Fail);
  few.held = 65;
  CHECK(bench::judge_hit_alias(true, few) == bench::Verdict::Pass);
  few.held = 0;
  CHECK(bench::judge_hit_alias(true, few) == bench::Verdict::Fail);  // a cache that holds nothing proves nothing about hits

  CHECK(bench::judge_scratch(0, 0) == bench::Verdict::Pass);
  CHECK(bench::judge_scratch(1, 0) == bench::Verdict::Fail);
  CHECK(bench::judge_scratch(0, 1) == bench::Verdict::Fail);
  CHECK(bench::judge_restore(0, 0) == bench::Verdict::Pass);
  CHECK(bench::judge_restore(0, 3) == bench::Verdict::Fail);
  CHECK(bench::judge_control(false, 256, 0, 0) == bench::Verdict::Skip);
  CHECK(bench::judge_control(true, 256, 256, 0) == bench::Verdict::Pass);
  CHECK(bench::judge_control(true, 256, 129, 0) == bench::Verdict::Pass);
  CHECK(bench::judge_control(true, 256, 128, 0) == bench::Verdict::Fail);  // the cache held at most half: nothing was shown
  CHECK(bench::judge_control(true, 256, 256, 1) == bench::Verdict::Fail);  // the invalidate left a stale word
  CHECK(bench::judge_pcm(1, 1, 1) == bench::Verdict::Pass);
  CHECK(bench::judge_pcm(1, 2, 1) == bench::Verdict::Fail);
  CHECK(bench::judge_pcm(1, 1, 2) == bench::Verdict::Fail);
  CHECK(bench::judge_pcm(0, 0, 0) == bench::Verdict::Pass);  // (no samples loaded)
}

TEST_CASE("cache check: the lines a test session greps") {
  char buf[256];
  std::size_t n = bench::format_check_scratch(bench::Verdict::Pass, 16384, 0, 0, buf, sizeof buf);
  CHECK(n == std::strlen(buf));
  CHECK(std::strcmp(buf, "cache check scratch: PASS 16384 words written through the alias read back wrong 0 times through it and 0 through 0x7000_0000\n") == 0);
  bench::format_check_scratch(bench::Verdict::Fail, 16384, 3, 0, buf, sizeof buf);
  CHECK(std::strcmp(buf, "cache check scratch: FAIL 16384 words written through the alias read back wrong 3 times through it and 0 through 0x7000_0000\n") == 0);

  bench::format_check_control(bench::Verdict::Pass, 256, 256, 0, buf, sizeof buf);
  CHECK(std::strcmp(buf, "cache check control: PASS 256 words changed through 0x7000_0000 behind cached lines: 256 read stale through the alias, 0 after an invalidate\n") == 0);
  bench::format_check_control(bench::Verdict::Skip, 256, 0, 0, buf, sizeof buf);
  CHECK(std::strcmp(buf, "cache check control: SKIP the cache is off, nothing is held to be stale\n") == 0);
  bench::format_check_hit_skip(buf, sizeof buf);
  CHECK(std::strcmp(buf, "cache check hit: SKIP the cache is off, no store is a write hit\n") == 0);

  bench::HitCounts c;
  c.lines = 128;
  c.held = 127;
  c.bad[2] = 4;
  bench::format_check_hit_alias(bench::Verdict::Fail, c, buf, sizeof buf);
  CHECK(std::strcmp(buf, "cache check hit-alias: FAIL 128 lines read through the alias, 127 held when written to; base wrong 0 words; read back through the alias, wrong lines: u32 0, u16 0, u8 4, tail 0\n") == 0);
  bench::format_check_hit_memory(bench::Verdict::Pass, bench::HitCounts{}, buf, sizeof buf);
  CHECK(std::strcmp(buf, "cache check hit-memory: PASS read through 0x7000_0000, wrong lines: u32 0, u16 0, u8 0, tail 0\n") == 0);
  bench::format_check_hit_inval(bench::Verdict::Pass, bench::HitCounts{}, buf, sizeof buf);
  CHECK(std::strcmp(buf, "cache check hit-inval: PASS read through the alias after a full invalidate, wrong lines: u32 0, u16 0, u8 0, tail 0\n") == 0);
  bench::format_check_restore(bench::Verdict::Pass, 512, 0, 0, buf, sizeof buf);
  CHECK(std::strcmp(buf, "cache check restore: PASS 512 words put back: wrong 0 through the alias, 0 through 0x7000_0000\n") == 0);
  bench::format_check_pcm(bench::Verdict::Pass, 7777776, 0x1234ABCD, 0x1234ABCD, 0x1234ABCD, buf, sizeof buf);
  CHECK(std::strcmp(buf, "cache check pcm: PASS 7777776 bytes, sum through the alias 0x1234ABCD, through 0x7000_0000 0x1234ABCD, through the alias after an invalidate 0x1234ABCD\n") == 0);
  bench::format_check_no_alias(buf, sizeof buf);
  CHECK(std::strcmp(buf, "cache check: SKIP the SDRAM alias is not enabled in this image\n") == 0);

  // Every line fits the console's line buffer with the largest numbers, one per line, and never contains the final line's "cache check: " (so the grep for the verdict matches one line).
  bench::HitCounts big;
  big.lines = big.held = big.base_bad = 0xFFFFFFFFu;
  big.bad[0] = big.bad[1] = big.bad[2] = big.bad[3] = 0xFFFFFFFFu;
  const std::size_t lens[] = {
      bench::format_check_scratch(bench::Verdict::Fail, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, buf, sizeof buf),
      bench::format_check_control(bench::Verdict::Fail, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, buf, sizeof buf),
      bench::format_check_hit_alias(bench::Verdict::Fail, big, buf, sizeof buf),
      bench::format_check_hit_memory(bench::Verdict::Fail, big, buf, sizeof buf),
      bench::format_check_hit_inval(bench::Verdict::Fail, big, buf, sizeof buf),
      bench::format_check_restore(bench::Verdict::Fail, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, buf, sizeof buf),
      bench::format_check_pcm(bench::Verdict::Fail, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, buf, sizeof buf),
  };
  for (const std::size_t len : lens) {
    CHECK(len > 0u);
    CHECK(len < sizeof buf - 1u);
    CHECK(len <= 240u);  // (the longest, hit-alias, is 236 with every number at 4,294,967,295; real ones are far shorter; bench.cpp's buffer is 256)
  }
}

TEST_CASE("cache check: the last line is PASS only when every sub-check ran and passed") {
  char buf[160];
  bench::Tally t;
  for (unsigned p = 0; p < static_cast<unsigned>(bench::Part::Count); ++p) {
    t.add(static_cast<bench::Part>(p), bench::Verdict::Pass);
  }
  CHECK(t.overall() == bench::Verdict::Pass);
  std::size_t n = bench::format_check_final(t, buf, sizeof buf);
  CHECK(n == std::strlen(buf));
  CHECK(std::strcmp(buf, "cache check: PASS\n") == 0);

  bench::Tally f = t;
  f.add(bench::Part::HitAlias, bench::Verdict::Fail);
  f.add(bench::Part::Pcm, bench::Verdict::Fail);
  f.add(bench::Part::Control, bench::Verdict::Skip);  // a failure outranks a skip
  CHECK(f.overall() == bench::Verdict::Fail);
  bench::format_check_final(f, buf, sizeof buf);
  CHECK(std::strcmp(buf, "cache check: FAIL hit-alias pcm\n") == 0);

  bench::Tally s;
  s.add(bench::Part::Scratch, bench::Verdict::Pass);
  s.add(bench::Part::Control, bench::Verdict::Skip);
  s.add(bench::Part::HitAlias, bench::Verdict::Skip);
  s.add(bench::Part::Pcm, bench::Verdict::Pass);
  CHECK(s.overall() == bench::Verdict::Skip);
  bench::format_check_final(s, buf, sizeof buf);
  CHECK(std::strcmp(buf, "cache check: SKIP the cache is off, the control and hit checks did not run (diag cache on)\n") == 0);
  CHECK(std::strstr(buf, "PASS") == nullptr);  // a grep for PASS does not match a run that did not exercise the cache

  // All seven names appear in a total failure.
  bench::Tally all;
  for (unsigned p = 0; p < static_cast<unsigned>(bench::Part::Count); ++p) {
    all.add(static_cast<bench::Part>(p), bench::Verdict::Fail);
  }
  bench::format_check_final(all, buf, sizeof buf);
  CHECK(std::strcmp(buf, "cache check: FAIL scratch control hit-alias hit-memory hit-inval restore pcm\n") == 0);
}
