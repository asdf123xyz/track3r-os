// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// `diag cache check` (OS-PERF3): the pure half. The patterns the check writes, what it must read back, the verdict of every sub-check and the lines it prints; host-tested
// (tests/mk66_cache_check_test.cpp). The accesses themselves are bench.cpp's: they need the cache and the SDRAM.
//
// THE LINES (a test session greps them, so they are stable):
//   cache check <part>: PASS|FAIL|SKIP <detail>     one per sub-check; <part> is scratch, control, hit-alias, hit-memory, hit-inval, restore or pcm (hit: all three hit parts skipped)
//   cache check: PASS                                every sub-check ran and passed
//   cache check: FAIL <part> [<part> ...]           the sub-checks that failed
//   cache check: SKIP <why>                          nothing failed but the cache-dependent sub-checks could not run (the cache is off, or the image has no SDRAM alias)
//
// THE WRITE-HIT STEP (hit-alias, hit-memory, hit-inval). A block of lines is read through the alias so that the cache holds it, then written through the alias with stores of 32, 16 and 8
// bits (kHitPlan: distinct values at different offsets inside each 16-byte line, two bytes of every line left alone), and read back three ways: through the alias (a write hit must have
// updated the cached copy), through 0x7000_0000 (write-through: memory has the stores) and through the alias again after a full invalidate (the line refilled from memory). Before the
// stores the last byte of every line is changed THROUGH 0x7000_0000, behind the cache (the probe): a line that is held still reads its old byte through the alias, a line that was evicted
// meanwhile reads the new one. So the readback says by itself how many of the stores were hits, which is what the step is worth; and a store that missed is no failure (the cache does not
// allocate on a write miss, memory has it).

#include <o3os/audio/diag.h>
#include <o3os/platform/mk66/lmem.h>

#include <cstddef>
#include <cstdint>

namespace o3os::platform::mk66::bench {

enum class Verdict : std::uint8_t { Pass, Fail, Skip };
inline constexpr const char* verdict_name(Verdict v) noexcept { return v == Verdict::Pass ? "PASS" : (v == Verdict::Fail ? "FAIL" : "SKIP"); }

// ---- the patterns --------------------------------------------------------------------------------------------------------------------------
static_assert(lmem::kLineBytes == 16u, "the hit plan is laid out for 16-byte lines");

struct HitStore {
  std::uint8_t offset;  // inside the line, a multiple of width
  std::uint8_t width;   // bytes: 4, 2 or 1
};
// Word 0 of the line is a 32-bit store, word 1 two 16-bit stores, word 2 four 8-bit stores, word 3 one 16-bit store beside two bytes no store touches.
inline constexpr HitStore kHitPlan[] = {{0, 4}, {4, 2}, {6, 2}, {8, 1}, {9, 1}, {10, 1}, {11, 1}, {12, 2}};
inline constexpr unsigned kHitPlanCount = sizeof kHitPlan / sizeof kHitPlan[0];
inline constexpr unsigned kHitKeep = 14;   // no store writes it: it must keep the base pattern (a sub-word store must not touch its neighbours)
inline constexpr unsigned kHitProbe = 15;  // no store writes it: changed behind the cache before the stores

constexpr bool hit_plan_valid() noexcept {
  unsigned covered = 0;  // bit n: byte n of the line is stored by some op
  for (unsigned i = 0; i < kHitPlanCount; ++i) {
    const unsigned o = kHitPlan[i].offset;
    const unsigned w = kHitPlan[i].width;
    if ((w != 1u && w != 2u && w != 4u) || o % w != 0u || o + w > kHitKeep) {
      return false;
    }
    for (unsigned j = 0; j < w; ++j) {
      if ((covered & (1u << (o + j))) != 0u) {
        return false;
      }
      covered |= 1u << (o + j);
    }
  }
  return covered == (1u << kHitKeep) - 1u;
}
static_assert(hit_plan_valid(), "the plan stores bytes 0..13 of a line once each, aligned, and leaves 14 and 15");

// Byte `pos` (0..15) of line `line` before the stores / written by the stores. The step per position is 17 (odd: all sixteen bytes of a line differ), and the difference between the two,
// -16 x line - 104 (mod 256), is never zero: a store that did not happen is always seen.
constexpr std::uint8_t hit_base_byte(std::uint32_t line, unsigned pos) noexcept { return static_cast<std::uint8_t>(line * 53u + pos * 17u + 0xC3u); }
constexpr std::uint8_t hit_new_byte(std::uint32_t line, unsigned pos) noexcept { return static_cast<std::uint8_t>(line * 37u + pos * 17u + 0x5Bu); }
// What goes into the last byte behind the cache: never the base byte.
constexpr std::uint8_t hit_probe_byte(std::uint32_t line) noexcept { return static_cast<std::uint8_t>(~hit_base_byte(line, kHitProbe)); }

// The value store `op` of line `line` writes (its low `width` bytes; little-endian, so byte `offset` is the lowest).
constexpr std::uint32_t hit_store_value(std::uint32_t line, unsigned op) noexcept {
  std::uint32_t v = 0;
  for (unsigned j = kHitPlan[op].width; j-- > 0u;) {
    v = (v << 8) | hit_new_byte(line, kHitPlan[op].offset + j);
  }
  return v;
}

// The three states of a line as 32-bit words: Base (before anything), Memory (after the stores and the probe: what 0x7000_0000 and a refill show) and Cached (what a line that was HELD
// when it was stored to shows through the alias: as Memory, but the probe byte is still the base's).
enum class HitImage : std::uint8_t { Base, Cached, Memory };
constexpr std::uint32_t hit_word(std::uint32_t line, unsigned w, HitImage image) noexcept {
  std::uint32_t v = 0;
  for (unsigned j = 4; j-- > 0u;) {
    const unsigned pos = w * 4u + j;
    std::uint8_t b = 0;
    if (image == HitImage::Base || pos == kHitKeep || (pos == kHitProbe && image == HitImage::Cached)) {
      b = hit_base_byte(line, pos);
    } else if (pos == kHitProbe) {
      b = hit_probe_byte(line);
    } else {
      b = hit_new_byte(line, pos);
    }
    v = (v << 8) | b;
  }
  return v;
}

// ---- the judgement of one line -------------------------------------------------------------------------------------------------------------
struct HitJudge {
  std::uint8_t bad;  // bit w: word w of the line read wrong
  bool held;         // alias readback only: the probe byte was the old one, so the cache held the line when it was stored to
};
// A line read through the alias right after the stores. Words 0..2 and bytes 12..14 must be the stored values; byte 15 says whether the line was held (the base byte) or evicted (the
// probe byte); anything else in it is wrong.
constexpr HitJudge hit_judge_alias(std::uint32_t line, const std::uint32_t* got) noexcept {
  HitJudge j{0, false};
  for (unsigned w = 0; w < 3; ++w) {
    if (got[w] != hit_word(line, w, HitImage::Cached)) {
      j.bad = static_cast<std::uint8_t>(j.bad | (1u << w));
    }
  }
  if (((got[3] ^ hit_word(line, 3, HitImage::Cached)) & 0x00FFFFFFu) != 0u) {
    j.bad = static_cast<std::uint8_t>(j.bad | 8u);
  }
  const std::uint32_t top = got[3] >> 24;
  j.held = top == hit_base_byte(line, kHitProbe);
  if (!j.held && top != hit_probe_byte(line)) {
    j.bad = static_cast<std::uint8_t>(j.bad | 8u);
  }
  return j;
}
// A line read from memory (through 0x7000_0000, or through the alias after the cache was invalidated): exactly the Memory image.
constexpr std::uint8_t hit_judge_memory(std::uint32_t line, const std::uint32_t* got) noexcept {
  std::uint8_t bad = 0;
  for (unsigned w = 0; w < 4; ++w) {
    if (got[w] != hit_word(line, w, HitImage::Memory)) {
      bad = static_cast<std::uint8_t>(bad | (1u << w));
    }
  }
  return bad;
}

// What one readback of the block adds up to.
struct HitCounts {
  std::uint32_t lines = 0;
  std::uint32_t held = 0;      // alias readback: lines the cache held when they were stored to
  std::uint32_t base_bad = 0;  // alias readback: words of the block that did not read as the base pattern before the stores
  std::uint32_t bad[4] = {};   // lines whose word 0 (32-bit store), 1 (16-bit stores), 2 (8-bit stores), 3 (16-bit store and the two bytes left alone) read wrong
};
constexpr void hit_count(HitCounts& c, std::uint8_t bad) noexcept {
  for (unsigned w = 0; w < 4; ++w) {
    c.bad[w] += (bad >> w) & 1u;
  }
}
constexpr std::uint32_t hit_bad_total(const HitCounts& c) noexcept { return c.bad[0] + c.bad[1] + c.bad[2] + c.bad[3]; }
// More than half of the lines must have been held for the step to say anything about write HITS (the same tolerance as the control: the code the loops run takes some of the cache's sets
// and an interrupt can evict a line).
constexpr bool hit_held_enough(std::uint32_t lines, std::uint32_t held) noexcept { return held > lines / 2u; }

// ---- the verdicts --------------------------------------------------------------------------------------------------------------------------
constexpr Verdict judge_scratch(std::uint32_t bad_alias, std::uint32_t bad_direct) noexcept { return bad_alias == 0u && bad_direct == 0u ? Verdict::Pass : Verdict::Fail; }
// The negative control: words changed through the other address behind cached lines must read stale through the alias (more than half of them: the cache holds lines) and fresh after an
// invalidate. Nothing is held when the cache is off: skipped.
constexpr Verdict judge_control(bool cache_on, std::uint32_t words, std::uint32_t stale_before, std::uint32_t stale_after) noexcept {
  return !cache_on ? Verdict::Skip : (stale_after == 0u && stale_before > words / 2u ? Verdict::Pass : Verdict::Fail);
}
constexpr Verdict judge_hit_alias(bool cache_on, const HitCounts& c) noexcept {
  return !cache_on ? Verdict::Skip : (c.base_bad == 0u && hit_bad_total(c) == 0u && hit_held_enough(c.lines, c.held) ? Verdict::Pass : Verdict::Fail);
}
constexpr Verdict judge_hit_clean(bool cache_on, const HitCounts& c) noexcept { return !cache_on ? Verdict::Skip : (hit_bad_total(c) == 0u ? Verdict::Pass : Verdict::Fail); }
constexpr Verdict judge_restore(std::uint32_t bad_alias, std::uint32_t bad_direct) noexcept { return judge_scratch(bad_alias, bad_direct); }
constexpr Verdict judge_pcm(std::uint32_t alias, std::uint32_t direct, std::uint32_t after_invalidate) noexcept {
  return alias == direct && direct == after_invalidate ? Verdict::Pass : Verdict::Fail;
}

enum class Part : std::uint8_t { Scratch, Control, HitAlias, HitMemory, HitInval, Restore, Pcm, Count };
inline constexpr const char* kPartName[] = {"scratch", "control", "hit-alias", "hit-memory", "hit-inval", "restore", "pcm"};
static_assert(sizeof kPartName / sizeof kPartName[0] == static_cast<std::size_t>(Part::Count), "one name per Part");

// The verdicts of a run, for the last line.
struct Tally {
  std::uint8_t failed = 0;   // bit Part
  std::uint8_t skipped = 0;  // bit Part
  constexpr void add(Part p, Verdict v) noexcept {
    const auto bit = static_cast<std::uint8_t>(1u << static_cast<unsigned>(p));
    if (v == Verdict::Fail) {
      failed = static_cast<std::uint8_t>(failed | bit);
    } else if (v == Verdict::Skip) {
      skipped = static_cast<std::uint8_t>(skipped | bit);
    }
  }
  constexpr Verdict overall() const noexcept { return failed != 0u ? Verdict::Fail : (skipped != 0u ? Verdict::Skip : Verdict::Pass); }
};

// ---- the lines -----------------------------------------------------------------------------------------------------------------------------
namespace detail {
inline audio::diag::Text& head(audio::diag::Text& t, const char* part, Verdict v) noexcept { return t.str("cache check ").str(part).str(": ").str(verdict_name(v)).chr(' '); }
inline audio::diag::Text& wrong(audio::diag::Text& t, const HitCounts& c) noexcept {
  return t.str("wrong lines: u32 ").dec(c.bad[0]).str(", u16 ").dec(c.bad[1]).str(", u8 ").dec(c.bad[2]).str(", tail ").dec(c.bad[3]).nl();
}
}  // namespace detail

inline std::size_t format_check_scratch(Verdict v, std::uint32_t words, std::uint32_t bad_alias, std::uint32_t bad_direct, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  detail::head(t, kPartName[static_cast<unsigned>(Part::Scratch)], v).dec(words).str(" words written through the alias read back wrong ").dec(bad_alias).str(" times through it and ").dec(bad_direct).str(" through 0x7000_0000\n");
  return t.size();
}
inline std::size_t format_check_control(Verdict v, std::uint32_t words, std::uint32_t stale_before, std::uint32_t stale_after, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  detail::head(t, kPartName[static_cast<unsigned>(Part::Control)], v);
  if (v == Verdict::Skip) {
    t.str("the cache is off, nothing is held to be stale\n");
  } else {
    t.dec(words).str(" words changed through 0x7000_0000 behind cached lines: ").dec(stale_before).str(" read stale through the alias, ").dec(stale_after).str(" after an invalidate\n");
  }
  return t.size();
}
// The three hit parts as one line when the cache is off.
inline std::size_t format_check_hit_skip(char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  detail::head(t, "hit", Verdict::Skip).str("the cache is off, no store is a write hit\n");
  return t.size();
}
inline std::size_t format_check_hit_alias(Verdict v, const HitCounts& c, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  detail::head(t, kPartName[static_cast<unsigned>(Part::HitAlias)], v).dec(c.lines).str(" lines read through the alias, ").dec(c.held).str(" held when written to; base wrong ").dec(c.base_bad).str(" words; read back through the alias, ");
  detail::wrong(t, c);
  return t.size();
}
inline std::size_t format_check_hit_memory(Verdict v, const HitCounts& c, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  detail::head(t, kPartName[static_cast<unsigned>(Part::HitMemory)], v).str("read through 0x7000_0000, ");
  detail::wrong(t, c);
  return t.size();
}
inline std::size_t format_check_hit_inval(Verdict v, const HitCounts& c, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  detail::head(t, kPartName[static_cast<unsigned>(Part::HitInval)], v).str("read through the alias after a full invalidate, ");
  detail::wrong(t, c);
  return t.size();
}
inline std::size_t format_check_restore(Verdict v, std::uint32_t words, std::uint32_t bad_alias, std::uint32_t bad_direct, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  detail::head(t, kPartName[static_cast<unsigned>(Part::Restore)], v).dec(words).str(" words put back: wrong ").dec(bad_alias).str(" through the alias, ").dec(bad_direct).str(" through 0x7000_0000\n");
  return t.size();
}
inline std::size_t format_check_pcm(Verdict v, std::uint32_t bytes, std::uint32_t alias, std::uint32_t direct, std::uint32_t after_invalidate, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  detail::head(t, kPartName[static_cast<unsigned>(Part::Pcm)], v).dec(bytes).str(" bytes, sum through the alias ").hex32(alias).str(", through 0x7000_0000 ").hex32(direct).str(", through the alias after an invalidate ").hex32(after_invalidate).nl();
  return t.size();
}
// The last line: PASS, FAIL and the failed parts, or SKIP.
inline std::size_t format_check_final(const Tally& tally, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  t.str("cache check: ").str(verdict_name(tally.overall()));
  if (tally.overall() == Verdict::Fail) {
    for (unsigned p = 0; p < static_cast<unsigned>(Part::Count); ++p) {
      if ((tally.failed >> p) & 1u) {
        t.chr(' ').str(kPartName[p]);
      }
    }
  } else if (tally.overall() == Verdict::Skip) {
    t.str(" the cache is off, the control and hit checks did not run (diag cache on)");
  }
  t.nl();
  return t.size();
}
// The image has no SDRAM alias: nothing to check.
inline std::size_t format_check_no_alias(char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  t.str("cache check: SKIP the SDRAM alias is not enabled in this image\n");
  return t.size();
}

}  // namespace o3os::platform::mk66::bench
