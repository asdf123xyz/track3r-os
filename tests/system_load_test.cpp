// SPDX-License-Identifier: GPL-3.0-or-later
// The status strip's CPU window and MEM percentage (OS-FU3, app/system_load.h).

#include <doctest/doctest.h>

#include <o3os/app/governor_watch.h>
#include <o3os/app/system_load.h>

#include <cstring>
#include <string>

using namespace o3os::app;

TEST_CASE("CpuMeter publishes the peak of a half-second window") {
  CpuMeter m;
  const std::uint32_t block = 1000;
  AudioLoad a{100, 100, block};
  CHECK(m.feed(1000, a) == SystemLoad::kUnknown);  // the first window is not over
  a.render_last = 300;
  CHECK(m.feed(1100, a) == SystemLoad::kUnknown);
  a.render_last = 50;
  CHECK(m.feed(1499, a) == SystemLoad::kUnknown);
  CHECK(m.feed(1500, a) == 30);                      // the peak, not the last
  a.render_last = 100;
  CHECK(m.feed(1600, a) == 30);                      // held until the next window ends
  CHECK(m.feed(2000, a) == 10);                      // the next window's own peak
}

TEST_CASE("CpuMeter sees a spike that only the all-time maximum remembers") {
  CpuMeter m;
  AudioLoad a{0, 500, 1000};
  (void)m.feed(0, a);  // primes: a maximum from before is not the window's
  CHECK(m.feed(500, a) == 0);
  a.render_max = 800;  // rose between two reads
  (void)m.feed(600, a);
  CHECK(m.feed(1000, a) == 80);
  CHECK(m.feed(1500, a) == 0);  // the maximum did not rise again
}

TEST_CASE("CpuMeter clamps a late block to 100 and is unknown without a block length") {
  CpuMeter m;
  AudioLoad a{2000, 2000, 1000};
  (void)m.feed(0, a);
  CHECK(m.feed(500, a) == 100);
  CHECK(m.feed(1000, AudioLoad{1, 1, 0}) == SystemLoad::kUnknown);
}

TEST_CASE("percent_of rounds down, reaches 100 only when full and survives a big arena") {
  CHECK(percent_of(0, 0) == SystemLoad::kUnknown);
  CHECK(percent_of(0, 100) == 0);
  CHECK(percent_of(99, 100) == 99);
  CHECK(percent_of(1, 1000) == 0);
  CHECK(percent_of(100, 100) == 100);
  CHECK(percent_of(5, 3) == 100);
  CHECK(percent_of(4u * 1024u * 1024u, 8u * 1024u * 1024u) == 50);
  CHECK(percent_of(0xF0000000u, 0xFFFFFFFFu) == 93);
}

TEST_CASE("VoiceMeter holds the peak of a half-second window and flashes for half a second after a steal (OS-VOX)") {
  VoiceMeter m;
  m.feed(1000, 3, 0);  // primes: the counter is taken, no flash
  CHECK(m.voices() == 3u);
  CHECK_FALSE(m.steal(1000));
  m.feed(1100, 9, 0);
  m.feed(1200, 2, 0);
  CHECK(m.voices() == 9u);  // a rise shows at once
  m.feed(1500, 1, 0);       // the window ends: the peak of it (9) is published, the next starts from now
  CHECK(m.voices() == 9u);
  m.feed(1600, 1, 0);
  CHECK(m.voices() == 9u);
  m.feed(2000, 1, 0);       // the next window's peak (1) replaces it
  CHECK(m.voices() == 1u);
  m.feed(2100, 1, 4);       // the counter rose: a steal
  CHECK(m.steal(2100));
  CHECK(m.steal(2599));
  CHECK_FALSE(m.steal(2600));
  m.feed(2300, 1, 5);       // another one extends it
  CHECK(m.steal(2799));
  CHECK_FALSE(m.steal(2800));
  // A steal counted before the first feed is not news.
  VoiceMeter n;
  n.feed(5000, 0, 7);
  CHECK_FALSE(n.steal(5000));
}

TEST_CASE("SystemLoad compares the voices and the steal flag too (a redraw follows any change, OS-VOX)") {
  SystemLoad a;
  SystemLoad b;
  CHECK(a == b);
  b.voices = 3;
  CHECK(a != b);
  a = b;
  b.pool = 16;
  CHECK(a != b);
  a = b;
  b.steal = true;
  CHECK(a != b);
}

// ---- OS-GOV: the CPU readout's colour, the toast of the module governor, the per-voice cost text --------------------------------------------------------

TEST_CASE("the CPU readout is normal up to 70 %, amber above 70 and red above 90 (OS-GOV)") {
  CHECK(cpu_level(SystemLoad::kUnknown) == CpuLevel::Normal);  // "--": no source
  CHECK(cpu_level(0) == CpuLevel::Normal);
  CHECK(cpu_level(70) == CpuLevel::Normal);  // above, not from
  CHECK(cpu_level(71) == CpuLevel::Warn);
  CHECK(cpu_level(90) == CpuLevel::Warn);
  CHECK(cpu_level(91) == CpuLevel::Alert);
  CHECK(cpu_level(100) == CpuLevel::Alert);
  CHECK(kCpuWarnPercent == 70);
  CHECK(kCpuAlertPercent == 90);
}

TEST_CASE("the CPU chip has hysteresis: a level is entered above its threshold and left only 3 points under it (OS-GOV)") {
  // entering is the plain threshold
  CHECK(next_cpu_level(CpuLevel::Normal, 70) == CpuLevel::Normal);
  CHECK(next_cpu_level(CpuLevel::Normal, 71) == CpuLevel::Warn);
  CHECK(next_cpu_level(CpuLevel::Normal, 90) == CpuLevel::Warn);
  CHECK(next_cpu_level(CpuLevel::Normal, 91) == CpuLevel::Alert);
  CHECK(next_cpu_level(CpuLevel::Warn, 91) == CpuLevel::Alert);
  // staying: amber holds down to 68, red down to 88
  CHECK(next_cpu_level(CpuLevel::Warn, 70) == CpuLevel::Warn);
  CHECK(next_cpu_level(CpuLevel::Warn, 68) == CpuLevel::Warn);
  CHECK(next_cpu_level(CpuLevel::Warn, 67) == CpuLevel::Normal);
  CHECK(next_cpu_level(CpuLevel::Alert, 90) == CpuLevel::Alert);
  CHECK(next_cpu_level(CpuLevel::Alert, 88) == CpuLevel::Alert);
  CHECK(next_cpu_level(CpuLevel::Alert, 87) == CpuLevel::Warn);  // red falls to amber, not to nothing
  CHECK(next_cpu_level(CpuLevel::Alert, 60) == CpuLevel::Normal);
  CHECK(next_cpu_level(CpuLevel::Alert, SystemLoad::kUnknown) == CpuLevel::Normal);
  // a figure that sits on the line does not flicker: 69 / 71 alternating stays amber once it got there, and 89 / 91 stays red
  CpuLevel level = CpuLevel::Normal;
  unsigned changes = 0;
  for (unsigned i = 0; i < 40; ++i) {
    const CpuLevel next = next_cpu_level(level, i % 2 == 0 ? 71 : 69);
    changes += next != level ? 1u : 0u;
    level = next;
  }
  CHECK(level == CpuLevel::Warn);
  CHECK(changes == 1u);
  changes = 0;
  for (unsigned i = 0; i < 40; ++i) {
    const CpuLevel next = next_cpu_level(level, i % 2 == 0 ? 91 : 89);
    changes += next != level ? 1u : 0u;
    level = next;
  }
  CHECK(level == CpuLevel::Alert);
  CHECK(changes == 1u);
}

namespace {
AudioLoad gov_load(std::uint32_t drops, std::uint32_t mutes, unsigned last_kind = 0, unsigned slot = 0) {
  AudioLoad a;
  a.gov_drops = drops;
  a.gov_mutes = mutes;
  a.gov_last = static_cast<std::uint16_t>((last_kind << 8) | slot);
  return a;
}
constexpr unsigned kDropKind = 1;  // audio::gov::Event
constexpr unsigned kMuteKind = 2;
}  // namespace

TEST_CASE("GovernorWatch: the first event shows at once, the ones inside the gap wait and are shown together, never more than one toast every 3 s (OS-GOV)") {
  GovernorWatch w;
  GovernorWatch::Toast t;
  CHECK_FALSE(w.feed(0, gov_load(9, 1), t));  // counters from before the App was built are not news
  CHECK_FALSE(w.feed(100, gov_load(9, 1), t));
  CHECK(w.feed(1000, gov_load(10, 1, kDropKind, 4), t));  // a drop on instrument 5 (slot 4): at once
  CHECK(t.severity == o3os::core::Severity::Warn);
  CHECK(std::strcmp(t.text.c_str(), "CPU FULL: INS 05 NOTE DROPPED") == 0);
  CHECK_FALSE(w.feed(1500, gov_load(11, 1, kDropKind, 4), t));  // inside the gap
  CHECK_FALSE(w.feed(3999, gov_load(13, 1, kDropKind, 4), t));  // still inside (1000 + 3000 = 4000)
  CHECK(w.feed(4000, gov_load(13, 1, kDropKind, 4), t));        // the gap is over: the three drops together
  CHECK(std::strcmp(t.text.c_str(), "CPU FULL: 3 NOTES DROPPED") == 0);
  CHECK_FALSE(w.feed(4001, gov_load(13, 1, kDropKind, 4), t));  // nothing new: nothing
  CHECK_FALSE(w.feed(9000, gov_load(13, 1, kDropKind, 4), t));
  // after a quiet spell the next event is shown at once again
  CHECK(w.feed(9500, gov_load(14, 1, kDropKind, 0), t));
  CHECK(std::strcmp(t.text.c_str(), "CPU FULL: INS 01 NOTE DROPPED") == 0);
}

TEST_CASE("GovernorWatch: a mute outranks a drop; the text names the instrument (OS-GOV)") {
  GovernorWatch w;
  GovernorWatch::Toast t;
  CHECK_FALSE(w.feed(0, gov_load(0, 0), t));
  CHECK(w.feed(10, gov_load(2, 1, kMuteKind, 11), t));
  CHECK(t.severity == o3os::core::Severity::Error);
  CHECK(std::strcmp(t.text.c_str(), "INS 12 MUTED: OVER ITS CPU BUDGET") == 0);
  CHECK(w.feed(3010, gov_load(6, 1, kDropKind, 0), t));  // four drops
  CHECK(std::strcmp(t.text.c_str(), "CPU FULL: 4 NOTES DROPPED") == 0);
  CHECK(t.text.size() <= o3os::core::kNotifyTextChars);
}

TEST_CASE("GovernorWatch survives the 32-bit wrap of a counter (OS-GOV)") {
  GovernorWatch w;
  GovernorWatch::Toast t;
  CHECK_FALSE(w.feed(0, gov_load(0xFFFFFFFFu, 0), t));
  CHECK(w.feed(10, gov_load(1, 0, kDropKind, 0), t));  // 0xFFFFFFFF -> 0 -> 1: two drops across the wrap
  CHECK(std::strcmp(t.text.c_str(), "CPU FULL: 2 NOTES DROPPED") == 0);
}

TEST_CASE("append_voice_cost: whole percent of the block, \"<1%\" under half a percent, \"UNMEASURED\" before the first measure (OS-GOV)") {
  const auto text = [](std::uint32_t cycles) {
    o3os::core::FixedString<24> s;
    append_voice_cost(s, cycles);
    return std::string(s.c_str());
  };
  CHECK(text(0) == "UNMEASURED");
  CHECK(text(42700) == "~16% PER VOICE");  // the TB-303
  CHECK(text(13056) == "~5% PER VOICE");
  CHECK(text(500) == "<1% PER VOICE");
  CHECK(text(1000) == "<1% PER VOICE");
  CHECK(text(1500) == "~1% PER VOICE");  // 0.57 % rounds to 1
  CHECK(text(261120) == "~100% PER VOICE");
}
