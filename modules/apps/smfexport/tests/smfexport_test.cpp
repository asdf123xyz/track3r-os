// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-H4 (new Community OS behaviour): SMF EXPORT. The file's arithmetic (smfexport_math.h) against the rules of docs/specs/OS-H4.md section 3, with a small SMF reader of this test parsing every byte back (header,
// track count, tempo, time signature, track names, every note's tick, channel, velocity and length), and the module's host shared object driven through the ABI as the OS drives it, on a fake of the export
// calls (o3m_test::FakeApi): the pattern, the F7 TOOLS selection, a Micro-move row, a MIDI instrument's channel, a sample instrument's default velocity, BUSY retried with the same bytes, the chunks of at most
// 1 KiB, the status polled to DONE and to FAILED, every refusal the OS can answer, the page, and the packed .tmod. What the OS does with the bytes (the atomic commit, the guard, the abort) is
// tests/app_export_test.cpp. Host only.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../../tests/o3m_test_support.h"

#include "../../common/o3m_edit.h"
#include "../smfexport_math.h"

#include <algorithm>
#include <cstring>
#include <o3os/seq/fx.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace o3m_test;
using namespace o3os::module;
namespace sx = smfexport;

namespace {

constexpr std::uint32_t kReadExport = O3M_CAP_READ_PROJECT | O3M_CAP_EXPORT_FILES;

o3m_step note_at(std::uint8_t note, std::uint8_t instr = 0, std::uint8_t vol = 255) { return o3m_step{note, instr, vol, 0, 0, 0, 0, 0}; }

// ---- a Standard MIDI File reader, enough for what the app writes ----------------------------------------------------------------------------------------------------

struct Ev {
  std::uint32_t tick = 0;
  std::uint8_t status = 0, d1 = 0, d2 = 0;  // a channel message
  std::uint8_t meta = 0;                    // 0, or the meta type of an FF event
  std::vector<std::uint8_t> data;           // a meta event's bytes
};
struct Note {
  std::uint32_t on = 0, off = 0, channel = 0, key = 0, vel = 0;
};
struct Smf {
  bool ok = false;
  std::string why;
  unsigned format = 99, ntrks = 0, division = 0;
  std::vector<std::vector<Ev>> tracks;
  std::vector<std::uint32_t> track_ticks;  // the tick of each track's end-of-track
  std::vector<std::string> names;
  std::uint32_t tempo_us = 0;
  std::vector<std::uint8_t> sig;
  std::vector<Note> notes(std::size_t track) const {
    std::vector<Note> out;
    std::map<std::uint32_t, std::size_t> open;  // key -> index in out
    for (const Ev& e : tracks[track]) {
      if (e.meta != 0) {
        continue;
      }
      const std::uint32_t ch = e.status & 15u;
      const std::uint32_t key = (ch << 8) | e.d1;
      if ((e.status & 0xF0u) == 0x90u && e.d2 != 0) {
        out.push_back(Note{e.tick, 0xFFFFFFFFu, ch, e.d1, e.d2});
        open[key] = out.size() - 1;
      } else if ((e.status & 0xF0u) == 0x80u || (e.status & 0xF0u) == 0x90u) {
        const auto it = open.find(key);
        if (it != open.end()) {
          out[it->second].off = e.tick;
          open.erase(it);
        }
      }
    }
    return out;
  }
};

Smf parse(const std::vector<std::uint8_t>& b) {
  Smf s;
  std::size_t at = 0;
  const auto fail = [&](const char* why) {
    s.why = why;
    return s;
  };
  const auto u32 = [&](std::size_t p) { return (static_cast<std::uint32_t>(b[p]) << 24) | (static_cast<std::uint32_t>(b[p + 1]) << 16) | (static_cast<std::uint32_t>(b[p + 2]) << 8) | b[p + 3]; };
  if (b.size() < 14 || std::memcmp(b.data(), "MThd", 4) != 0 || u32(4) != 6) {
    return fail("no MThd");
  }
  s.format = (b[8] << 8) | b[9];
  s.ntrks = (b[10] << 8) | b[11];
  s.division = (b[12] << 8) | b[13];
  at = 14;
  for (unsigned t = 0; t < s.ntrks; ++t) {
    if (at + 8 > b.size() || std::memcmp(b.data() + at, "MTrk", 4) != 0) {
      return fail("no MTrk");
    }
    const std::uint32_t len = u32(at + 4);
    at += 8;
    if (at + len > b.size()) {
      return fail("MTrk longer than the file");
    }
    const std::size_t end = at + len;
    std::vector<Ev> evs;
    std::uint32_t tick = 0;
    std::string name;
    bool ended = false;
    while (at < end) {
      std::uint32_t delta = 0;
      for (int k = 0; k < 4; ++k) {
        if (at >= end) {
          return fail("cut in a delta");
        }
        const std::uint8_t c = b[at++];
        delta = (delta << 7) | (c & 0x7Fu);
        if ((c & 0x80u) == 0) {
          break;
        }
      }
      tick += delta;
      if (at >= end) {
        return fail("cut after a delta");
      }
      Ev e;
      e.tick = tick;
      const std::uint8_t st = b[at++];
      if (st == 0xFF) {
        if (at + 2 > end) {
          return fail("cut in a meta");
        }
        e.meta = b[at++];
        const std::uint32_t n = b[at++];
        if (at + n > end) {
          return fail("meta past the chunk");
        }
        e.data.assign(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
        if (e.meta == 0x03) {
          name.assign(e.data.begin(), e.data.end());
        } else if (e.meta == 0x51 && n == 3) {
          s.tempo_us = (static_cast<std::uint32_t>(e.data[0]) << 16) | (static_cast<std::uint32_t>(e.data[1]) << 8) | e.data[2];
        } else if (e.meta == 0x58) {
          s.sig = e.data;
        } else if (e.meta == 0x2F) {
          ended = true;
          s.track_ticks.push_back(tick);
        }
      } else if (st >= 0x80 && st < 0xF0) {
        const unsigned data = (st & 0xF0u) == 0xC0u || (st & 0xF0u) == 0xD0u ? 1u : 2u;
        if (at + data > end) {
          return fail("cut in a channel message");
        }
        e.status = st;
        e.d1 = b[at++];
        e.d2 = data == 2 ? b[at++] : 0;
      } else {
        return fail("running status or a system message");
      }
      evs.push_back(e);
    }
    if (at != end || !ended || evs.empty() || evs.back().meta != 0x2F) {
      return fail("a track does not end with its end-of-track");
    }
    s.tracks.push_back(evs);
    s.names.push_back(name);
  }
  if (at != b.size()) {
    return fail("bytes after the last track");
  }
  s.ok = true;
  return s;
}

// A resolver for the math tests: the channel is the instrument's table entry or the track's, the velocity the instrument's.
struct TestRes {
  std::uint32_t start = 0;
  std::uint32_t chan[65] = {};      // 0: the track's own
  std::uint32_t defvel[65] = {};    // 0: 127
  std::uint32_t first_instrument() const { return start; }
  std::uint32_t channel(std::uint32_t track, std::uint32_t instr) const { return instr >= 1 && instr <= 64 && chan[instr] != 0 ? chan[instr] - 1 : track % 16; }
  std::uint32_t default_velocity(std::uint32_t instr) const { return instr >= 1 && instr <= 64 && defvel[instr] != 0 ? defvel[instr] : 127; }
};

std::vector<std::uint8_t> track_bytes(const std::vector<o3m_step>& steps, std::uint32_t track, TestRes& res, sx::Counts* counts = nullptr) {
  std::vector<std::uint8_t> out(sx::kTrackBytesMax);
  const std::uint32_t n = sx::build_track<o3m_step, TestRes>(out.data(), static_cast<std::uint32_t>(out.size()), steps.data(), static_cast<std::uint32_t>(steps.size()), track, res, counts);
  out.resize(n);
  return out;
}

// The file of one track as a whole SMF (header, tempo track, the track) for the reader.
Smf one_track(const std::vector<o3m_step>& steps, std::uint32_t track, TestRes& res, sx::Counts* counts = nullptr) {
  std::vector<std::uint8_t> file(sx::kHeaderBytes + sx::kTempoTrackBytes);
  const std::uint32_t h = sx::build_header(file.data(), static_cast<std::uint32_t>(file.size()), 2);
  const std::uint32_t t = sx::build_tempo_track(file.data() + h, static_cast<std::uint32_t>(file.size()) - h, 12000);
  CHECK(h == sx::kHeaderBytes);
  CHECK(t == sx::kTempoTrackBytes);
  const std::vector<std::uint8_t> body = track_bytes(steps, track, res, counts);
  file.insert(file.end(), body.begin(), body.end());
  return parse(file);
}

}  // namespace

// ---- the arithmetic -----------------------------------------------------------------------------------------------------------------------------

TEST_CASE("smfexport math: the copy of micromove_start is the core's, for every byte; the offset is half of it in ticks") {
  for (unsigned v = 0; v < 256; ++v) {
    CHECK(sx::micromove_start(static_cast<std::uint8_t>(v)) == o3os::seq::micromove_start(static_cast<std::uint8_t>(v)));
    CHECK(sx::micromove_ticks(static_cast<std::uint8_t>(v)) == o3os::seq::micromove_start(static_cast<std::uint8_t>(v)) / 2u);
    CHECK(sx::micromove_ticks(static_cast<std::uint8_t>(v)) <= 23u);  // always inside the row's 24 ticks
  }
  CHECK(sx::micromove_ticks(0) == 0u);
  CHECK(sx::micromove_ticks(100) == 23u);
}

TEST_CASE("smfexport math: the tempo meta is the exact microseconds per quarter note, for every tempo the OS allows") {
  CHECK(sx::tempo_us(12000) == 500000u);
  CHECK(sx::tempo_us(4000) == 1500000u);
  CHECK(sx::tempo_us(80000) == 75000u);
  CHECK(sx::tempo_us(13000) == 461538u);
  CHECK(sx::tempo_us(0) == 500000u);  // (no project: 120)
  for (std::uint32_t x = 4000; x <= 80000; x += 7) {
    CHECK(sx::tempo_us(x) == static_cast<std::uint32_t>(6000000000ull / x));
  }
}

TEST_CASE("smfexport math: the header and the tempo track") {
  std::vector<std::uint8_t> f(sx::kHeaderBytes + sx::kTempoTrackBytes);
  const std::uint32_t h = sx::build_header(f.data(), static_cast<std::uint32_t>(f.size()), 3);
  const std::uint32_t t = sx::build_tempo_track(f.data() + h, static_cast<std::uint32_t>(f.size()) - h, 12345);
  CHECK(h + t == f.size());
  // two more empty tracks so that the reader's count is met
  for (int i = 0; i < 2; ++i) {
    const std::uint8_t trk[12] = {'M', 'T', 'r', 'k', 0, 0, 0, 4, 0, 0xFF, 0x2F, 0};
    f.insert(f.end(), trk, trk + 12);
  }
  const Smf s = parse(f);
  NEED(s.ok);
  CHECK(s.why.empty());
  CHECK((s.format == 1u && s.ntrks == 3u && s.division == 96u));
  CHECK(s.tempo_us == sx::tempo_us(12345));
  CHECK(s.sig == std::vector<std::uint8_t>({4, 2, 24, 8}));  // 4/4
  // too small a buffer is refused, not overrun
  std::uint8_t small[10] = {};
  CHECK(sx::build_header(small, sizeof small, 1) == 0u);
  CHECK(sx::build_tempo_track(small, sizeof small, 12000) == 0u);
}

TEST_CASE("smfexport math: notes, ticks, channels, velocities and lengths read back exactly") {
  std::vector<o3m_step> st(16, note_at(0));
  st[0] = note_at(49, 1, 100);   // C-4 at row 0, velocity 100
  st[2] = note_at(51, 2);        // next note: the first ends where this one starts; no volume: the instrument's default
  st[5] = note_at(255);          // off
  st[6] = note_at(60, 0, 0);     // volume 0 is velocity 1
  st[8] = note_at(61, 0, 127);
  st[9] = note_at(254);          // cut
  st[10] = note_at(62, 0, 200);  // a volume above 127 is 127
  st[12] = note_at(63);
  st[14] = note_at(253);         // fade ends the note too
  // 13 stays: the last note ends at row 14. A note on row 15 would run to the end of the range.
  st[15] = note_at(64, 3, 90);
  TestRes res;
  res.chan[2] = 5;      // instrument 2 is a MIDI instrument on channel 5
  res.defvel[1] = 63;   // instrument 1's own volume
  res.defvel[2] = 127;
  sx::Counts c{};
  const Smf s = one_track(st, 3, res, &c);  // track index 3: "Track 4", channel 4 for an instrument with none
  NEED(s.ok);
  CHECK(s.why.empty());
  NEED(s.tracks.size() == 2u);
  CHECK(s.names[1] == "Track 4");
  CHECK(s.track_ticks[1] == 16u * 24u);  // ends at the end of the range
  const std::vector<Note> n = s.notes(1);
  NEED(n.size() == 7u);
  // instrument 1 has no channel of its own (track index 3 -> channel index 3); instrument 2 is on channel 5 (index 4) and the notes with none carry it; instrument 3 has none
  CHECK((n[0].on == 0u && n[0].off == 48u && n[0].channel == 3u && n[0].key == 48u && n[0].vel == 100u));
  CHECK((n[1].on == 48u && n[1].off == 120u && n[1].channel == 4u && n[1].key == 50u && n[1].vel == 127u));
  CHECK((n[2].on == 144u && n[2].off == 192u && n[2].channel == 4u && n[2].key == 59u && n[2].vel == 1u));
  CHECK((n[3].on == 192u && n[3].off == 216u && n[3].channel == 4u && n[3].key == 60u && n[3].vel == 127u));
  CHECK((n[4].on == 240u && n[4].off == 288u && n[4].channel == 4u && n[4].key == 61u && n[4].vel == 127u));
  CHECK((n[5].on == 288u && n[5].off == 336u && n[5].channel == 4u && n[5].key == 62u && n[5].vel == 127u));
  CHECK((n[6].on == 360u && n[6].off == 384u && n[6].channel == 3u && n[6].key == 63u && n[6].vel == 90u));
  CHECK(c.notes == 7u);
}

TEST_CASE("smfexport math: the first note of a track takes the instrument the page started from, and a note with no instrument anywhere takes the track's channel") {
  std::vector<o3m_step> st(4, note_at(0));
  st[0] = note_at(60);
  st[2] = note_at(62, 4);
  TestRes res;
  res.chan[4] = 9;
  res.start = 4;  // the instrument in use when the page was opened: a MIDI instrument on channel 9
  Smf s = one_track(st, 0, res);
  NEED(s.ok);
  std::vector<Note> n = s.notes(1);
  NEED(n.size() == 2u);
  CHECK(n[0].channel == 8u);
  CHECK(n[1].channel == 8u);
  res.start = 0;
  s = one_track(st, 17, res);  // track 17 (0-based) is channel 2
  NEED(s.ok);
  n = s.notes(1);
  NEED(n.size() == 2u);
  CHECK(n[0].channel == 1u);  // 17 mod 16
  CHECK(n[1].channel == 8u);  // instrument 4 from row 2
  CHECK(s.names[1] == "Track 18");
}

TEST_CASE("smfexport math: Micro-move moves the note-on and ends the note before it; the first FX column wins") {
  std::vector<o3m_step> st(8, note_at(0));
  st[0] = note_at(60);
  st[1] = note_at(62);
  st[1].fx2_type = sx::kFxMicroMove;
  st[1].fx2_value = 100;   // 23 ticks late
  st[2] = note_at(64);
  st[2].fx1_type = sx::kFxMicroMove;
  st[2].fx1_value = 50;
  st[2].fx2_type = sx::kFxMicroMove;
  st[2].fx2_value = 100;   // the first column's value stands
  st[3] = note_at(255);
  st[3].fx1_type = sx::kFxMicroMove;
  st[3].fx1_value = 30;    // an Off step is moved as well
  TestRes res;
  const Smf s = one_track(st, 0, res);
  NEED(s.ok);
  const std::vector<Note> n = s.notes(1);
  NEED(n.size() == 3u);
  CHECK(n[0].on == 0u);
  CHECK(n[0].off == 24u + sx::micromove_ticks(100));  // ends exactly where the next one begins
  CHECK(n[1].on == 24u + sx::micromove_ticks(100));
  CHECK(n[1].on == 24u + 23u);
  CHECK(n[1].off == 48u + sx::micromove_ticks(50));
  CHECK(n[2].on == 48u + sx::micromove_ticks(50));
  CHECK(n[2].off == 72u + sx::micromove_ticks(30));
  // every event is in order in time
  std::uint32_t last = 0;
  for (const Ev& e : s.tracks[1]) {
    CHECK(e.tick >= last);
    last = e.tick;
  }
}

TEST_CASE("smfexport math: the FX that are not exported are counted by number, Micro-move is not one of them") {
  std::vector<o3m_step> st(6, note_at(0));
  st[0] = note_at(60);
  st[0].fx1_type = 14;  // Chord: not expanded in v1
  st[0].fx1_value = 3;
  st[1] = note_at(0);
  st[1].fx2_type = 7;   // an FX on a step without a note counts too
  st[2] = note_at(62);
  st[2].fx1_type = 14;
  st[2].fx2_type = sx::kFxMicroMove;
  st[3] = note_at(64);
  st[3].fx1_type = 128;  // an engine's own
  st[4] = note_at(65);
  st[4].fx1_type = 200;
  st[5] = note_at(66);
  st[5].fx2_type = 42;
  TestRes res;
  sx::Counts c{};
  (void)track_bytes(st, 0, res, &c);
  CHECK(c.skipped[14] == 2u);
  CHECK(c.skipped[7] == 1u);
  CHECK(c.skipped[42] == 1u);
  CHECK(c.skipped[43] == 2u);  // 128 and 200
  CHECK(c.skipped[2] == 0u);
  CHECK(c.notes == 5u);
}

TEST_CASE("smfexport math: the worst track fits the buffer (128 rows, a note and a Micro-move on every row)") {
  std::vector<o3m_step> st(128, note_at(60, 1, 100));
  for (auto& s : st) {
    s.fx1_type = sx::kFxMicroMove;
    s.fx1_value = 100;
  }
  TestRes res;
  const std::vector<std::uint8_t> b = track_bytes(st, 15, res);
  CHECK(b.size() != 0u);
  CHECK(b.size() <= sx::kTrackBytesMax);
  // a buffer that is too small is refused whole
  std::vector<std::uint8_t> tiny(64);
  CHECK(sx::build_track<o3m_step, TestRes>(tiny.data(), 64, st.data(), 128, 0, res, nullptr) == 0u);
  // no rows still gives a valid empty track
  const std::vector<std::uint8_t> none = track_bytes(std::vector<o3m_step>(), 0, res);
  CHECK(none.size() == 8u + 11u + 4u);  // header, delta + name meta ("Track 1"), delta + end of track
  CHECK(sx::has_note(st.data(), 128));
  CHECK_FALSE(sx::has_note(std::vector<o3m_step>(8, note_at(255)).data(), 8));
}

TEST_CASE("smfexport math: a track built in slices is byte for byte the track built whole, and a slice takes no more rows than it is given (OS-FINALQA FQA-10b1)") {
  std::vector<o3m_step> st(128);
  for (std::uint32_t j = 0; j < 128; ++j) {
    st[j] = j % 5 == 4 ? note_at(255) : note_at(static_cast<std::uint8_t>(20 + j % 90), static_cast<std::uint8_t>(j % 3), j % 2 == 0 ? 255 : static_cast<std::uint8_t>(j));
    st[j].fx1_type = j % 7 == 0 ? sx::kFxMicroMove : static_cast<std::uint8_t>(j % 4 == 0 ? 7 : 0);
    st[j].fx1_value = static_cast<std::uint8_t>(j);
  }
  TestRes res;
  res.chan[1] = 5;
  for (std::uint32_t rows : {0u, 1u, 33u, 128u}) {
    const std::vector<o3m_step> part(st.begin(), st.begin() + rows);
    sx::Counts whole;
    std::memset(&whole, 0, sizeof whole);
    const std::vector<std::uint8_t> ref = track_bytes(part, 3, res, &whole);
    for (std::uint32_t slice : {1u, 7u, sx::kRowsPerSlice, 128u}) {
      std::vector<std::uint8_t> out(sx::kTrackBytesMax);
      sx::Counts c;
      std::memset(&c, 0, sizeof c);
      sx::TrackWriter w;
      w.begin(out.data(), static_cast<std::uint32_t>(out.size()), rows, 3, res);
      std::uint32_t taken = 0;
      while (!w.complete()) {
        const std::uint32_t k = w.run(part.data(), slice, res, &c);
        CHECK(k <= slice);
        CHECK(k > 0u);
        taken += k;
      }
      CHECK(taken == rows);
      CHECK(w.run(part.data(), slice, res, &c) == 0u);  // nothing past the end
      out.resize(w.finish());
      CHECK(out == ref);
      CHECK(c.notes == whole.notes);
      CHECK(std::memcmp(c.skipped, whole.skipped, sizeof c.skipped) == 0);
    }
  }
}

// ---- the app ------------------------------------------------------------------------------------------------------------------------------------

namespace {

// Four tracks of 16 rows with a few notes each, a tempo of 120.
void fixture(EditRig& r, unsigned tracks = 4) {
  r.api.pat_tracks = tracks;
  for (unsigned t = 0; t < tracks; ++t) {
    for (unsigned row = 0; row < 16; row += 4) {
      r.api.steps[t][row] = note_at(static_cast<std::uint8_t>(49 + t + row), 1, static_cast<std::uint8_t>(80 + row));
    }
  }
}

// Frames until the page stops saying EXPORTING (or `frames` run out): F1 only starts the job, the frames do the work (OS-FINALQA FQA-10b1).
void settle(EditRig& r, unsigned frames = 60) {
  for (unsigned i = 0; i < frames; ++i) {
    if (!r.shows(r.draw(), "EXPORTING")) {
      break;
    }
  }
}

// F1, then frames until the export is over (or `frames` run out).
void run_export(EditRig& r, unsigned frames = 60) {
  r.press(O3M_BTN_F1);
  settle(r, frames);
  (void)r.draw();
}

}  // namespace

namespace {

// The note count the page shows while it writes ("<n> NOTES ..."): 0 when it shows none.
std::uint32_t page_notes(const Drawing& d) {
  for (const auto& t : d.texts) {
    const std::size_t at = t.s.find(" NOTE");
    if (at != std::string::npos) {
      std::size_t from = at;
      while (from != 0u && t.s[from - 1u] >= '0' && t.s[from - 1u] <= '9') {
        --from;
      }
      return static_cast<std::uint32_t>(std::strtoul(t.s.substr(from, at - from).c_str(), nullptr, 10));
    }
  }
  return 0;
}

// The worst pattern: 16 tracks of 128 rows, a note and a Micro-move on every row.
void worst_pattern(EditRig& r) {
  r.api.pat_tracks = 16;
  for (unsigned t = 0; t < 16; ++t) {
    r.api.pat_len[t] = 128;
    for (unsigned row = 0; row < 128; ++row) {
      r.api.steps[t][row] = note_at(static_cast<std::uint8_t>(30 + row % 60), 1, static_cast<std::uint8_t>(1 + row));
      r.api.steps[t][row].fx1_type = sx::kFxMicroMove;
      r.api.steps[t][row].fx1_value = static_cast<std::uint8_t>(row);
    }
  }
}

struct Worst {
  unsigned frames = 0;
  unsigned steps_calls = 0, writes = 0;  // the most any one frame made
  std::uint32_t notes = 0;               // the most note-ons any one frame built
};

// F1, then every frame to the end, with what each call into the app did (the host calls it made, the notes it built).
Worst measure(EditRig& r, unsigned max_frames = 600) {
  Worst w;
  const auto& a = r.api;
  const int s0 = a.steps_reads;
  r.press(O3M_BTN_F1);  // input(): nothing is read or written
  CHECK(a.steps_reads == s0);
  CHECK(a.write_calls == 0u);
  CHECK_FALSE(a.export_open_now);
  std::uint32_t notes = 0;
  while (w.frames < max_frames) {
    const int s1 = a.steps_reads;
    const unsigned w1 = a.write_calls;
    const Drawing d = r.draw();
    ++w.frames;
    w.steps_calls = std::max(w.steps_calls, static_cast<unsigned>(a.steps_reads - s1));
    w.writes = std::max(w.writes, a.write_calls - w1);
    const std::uint32_t n = page_notes(d);
    w.notes = std::max(w.notes, n - notes);
    notes = n;
    if (!r.shows(d, "EXPORTING")) {
      break;
    }
  }
  return w;
}

}  // namespace

TEST_CASE("smfexport app: F1 only starts the job; no call into the app does more than a bounded amount of work, so no call can run over the view's 4 ms (OS-FINALQA FQA-10b1)") {
  // On the unit the F1 call itself read every track, built three of them and opened the file: over the 4 ms of a call into an app (the code and the state of an app are in SDRAM), and the
  // module was quarantined. Now each frame takes units (one track scanned, one slice of kRowsPerSlice rows built, one chunk written ..), at most kUnitsPerFrame, and the host calls and the note-ons of
  // a frame are what the unit count says.
  EditRig r;
  worst_pattern(r);
  NEED(r.start(O3M_SMFEXPORT_HOST_SO, kReadExport));
  const Worst w = measure(r);
  CHECK(w.frames < 600u);
  CHECK(w.steps_calls <= 2u * sx::kUnitsPerFrame);  // a unit reads at most twice (the instrument above the range, the track)
  CHECK(w.writes <= sx::kUnitsPerFrame);
  CHECK(w.notes <= sx::kUnitsPerFrame * sx::kRowsPerSlice);
  CHECK(w.notes > 0u);
  CHECK(r.api.chunk_max <= sx::kChunk);
  NEED(r.api.exports.size() == 1u);
  const Smf s = parse(r.api.exports[0].data);
  NEED(s.ok);
  CHECK(s.ntrks == 17u);
  for (unsigned t = 1; t <= 16; ++t) {
    CHECK(s.notes(t).size() == 128u);
  }
  CHECK(page_notes(r.draw()) == 2048u);
}

TEST_CASE("smfexport app: the clock guard: a frame takes no unit once the host's millisecond clock has moved, and the file is the same") {
  std::vector<std::uint8_t> free_run;
  {
    EditRig r;
    worst_pattern(r);
    NEED(r.start(O3M_SMFEXPORT_HOST_SO, kReadExport));
    (void)measure(r);
    NEED(r.api.exports.size() == 1u);
    free_run = r.api.exports[0].data;
  }
  EditRig r;
  worst_pattern(r);
  r.api.tick_every = 3;  // t0, the check before the first unit, the check before the second: the clock moves on the third call
  NEED(r.start(O3M_SMFEXPORT_HOST_SO, kReadExport));
  const Worst w = measure(r, 2000);
  CHECK(w.frames < 2000u);
  CHECK(w.steps_calls <= 2u);
  CHECK(w.writes <= 1u);
  CHECK(w.notes <= sx::kRowsPerSlice);
  NEED(r.api.exports.size() == 1u);
  CHECK(r.api.exports[0].data == free_run);
}

TEST_CASE("smfexport app: descriptor") {
  EditRig r;
  NEED(r.lib.open(O3M_SMFEXPORT_HOST_SO));
  const o3m_module& m = *r.lib.entry;
  CHECK((m.abi_major == 1u && m.abi_minor == 10u && m.kind == O3M_KIND_APP));  // (1.10: its PATTERN row is declared and kept with the project, OS-APPUX S3)
  // ABI 1.9 (OS-KEYROWDLG S5): the labels of the page's key row; F8 is the OS's BACK, so the app labels F1..F7 only; Shift + Fn runs Fn (no app reads mods), so the Shift row says the same
  CHECK((m.ui != nullptr && m.ui->keys != nullptr && m.ui->struct_size == O3M_UI_SIZE(9)));
  static const o3m_keys no_keys = {{nullptr}, {nullptr}};
  const o3m_keys* keys = m.ui != nullptr && m.ui->keys != nullptr ? m.ui->keys(nullptr) : nullptr;
  CHECK(keys != nullptr);
  keys = keys != nullptr ? keys : &no_keys;
  CHECK((keys->f[0] != nullptr && std::strcmp(keys->f[0], "EXPORT") == 0));
  CHECK((keys->shift_f[0] != nullptr && std::strcmp(keys->shift_f[0], "EXPORT") == 0));  // Shift + Fn runs Fn (no app reads mods): the Shift row says the same
  CHECK(keys->f[1] == nullptr);
  CHECK(keys->shift_f[1] == nullptr);
  CHECK(keys->f[2] == nullptr);
  CHECK(keys->shift_f[2] == nullptr);
  CHECK(keys->f[3] == nullptr);
  CHECK(keys->shift_f[3] == nullptr);
  CHECK(keys->f[4] == nullptr);
  CHECK(keys->shift_f[4] == nullptr);
  CHECK(keys->f[5] == nullptr);
  CHECK(keys->shift_f[5] == nullptr);
  CHECK(keys->f[6] == nullptr);
  CHECK(keys->shift_f[6] == nullptr);
  CHECK(keys->f[7] == nullptr);
  CHECK(keys->shift_f[7] == nullptr);
  CHECK(std::string(m.name) == "SMF Export");
  CHECK(m.view_count == 1u);
  CHECK((m.param_count == 1u && m.set_param != nullptr));  // PATTERN: declared as storage (the page draws it itself)
}

TEST_CASE("smfexport app: the whole pattern from the app list: one MTrk per track with a note, every note, the tempo, and a file named for the pattern") {
  EditRig r;
  fixture(r);
  r.api.tempo = 13000;
  r.api.instr[1].kind = O3M_INSTRUMENT_SAMPLE;
  NEED(r.start(O3M_SMFEXPORT_HOST_SO, kReadExport));  // (no launch context: all zeros, flags 0)
  run_export(r);
  NEED(r.api.exports.size() == 1u);
  const auto& f = r.api.exports[0];
  CHECK(f.name == "PAT001");
  CHECK(f.ext == "mid");
  CHECK(f.closed);
  CHECK(r.api.chunk_max <= 1024u);
  const Smf s = parse(f.data);
  NEED(s.ok);
  CHECK(s.why.empty());
  CHECK((s.format == 1u && s.ntrks == 5u && s.division == 96u));  // the tempo track and four
  CHECK(s.tempo_us == 461538u);
  CHECK(s.sig == std::vector<std::uint8_t>({4, 2, 24, 8}));
  NEED(s.tracks.size() == 5u);
  for (unsigned t = 0; t < 4; ++t) {
    CHECK(s.names[t + 1] == "Track " + std::to_string(t + 1));
    const std::vector<Note> n = s.notes(t + 1);
    NEED(n.size() == 4u);
    for (unsigned k = 0; k < 4; ++k) {
      CHECK(n[k].on == k * 4u * 24u);
      CHECK(n[k].off == (k + 1) * 4u * 24u);  // each note runs to the next, the last to the end of the track
      CHECK(n[k].key == 48u + t + k * 4u);
      CHECK(n[k].vel == 80u + k * 4u);
      CHECK(n[k].channel == t);  // instrument 1 is a sample: the track's channel
    }
    CHECK(s.track_ticks[t + 1] == 16u * 24u);
  }
  const Drawing d = r.draw();
  CHECK(r.shows(d, "DONE"));
  CHECK(r.shows(d, "APPEXPORT/SMFEXPORT/PAT001.mid"));
  CHECK(r.shows(d, "16 NOTES"));
  CHECK(r.shows(d, "4 TRACKS"));
}

TEST_CASE("smfexport app: an empty track is left out, and the header counts only the written ones") {
  EditRig r;
  fixture(r, 6);
  for (unsigned row = 0; row < 16; ++row) {
    r.api.steps[1][row] = note_at(0);
    r.api.steps[4][row] = note_at(255);  // only Offs: no note
  }
  NEED(r.start(O3M_SMFEXPORT_HOST_SO, kReadExport));
  run_export(r);
  NEED(r.api.exports.size() == 1u);
  const Smf s = parse(r.api.exports[0].data);
  NEED(s.ok);
  CHECK(s.ntrks == 5u);  // tempo + tracks 1, 3, 4, 6
  NEED(s.names.size() == 5u);
  CHECK(s.names[1] == "Track 1");
  CHECK(s.names[2] == "Track 3");
  CHECK(s.names[3] == "Track 4");
  CHECK(s.names[4] == "Track 6");
}

TEST_CASE("smfexport app: BUSY is retried with the same bytes, one chunk is at most 1 KiB, and the file is byte-identical to a run with no BUSY") {
  std::vector<std::uint8_t> clean;
  {
    EditRig r;
    fixture(r, 16);
    for (unsigned t = 0; t < 16; ++t) {
      r.api.pat_len[t] = 128;
      for (unsigned row = 0; row < 128; row += 2) {
        r.api.steps[t][row] = note_at(static_cast<std::uint8_t>(30 + row % 60), 1, static_cast<std::uint8_t>(1 + row));
        r.api.steps[t][row].fx1_type = row % 8 == 0 ? 2 : 0;
        r.api.steps[t][row].fx1_value = static_cast<std::uint8_t>(row);
      }
    }
    NEED(r.start(O3M_SMFEXPORT_HOST_SO, kReadExport));
    run_export(r, 400);
    NEED(r.api.exports.size() == 1u);
    clean = r.api.exports[0].data;
    CHECK(clean.size() > 8192u);  // more than the ring of the OS: it needs the BUSY handling for real
    CHECK(r.api.chunk_max <= 1024u);
    const Smf s = parse(clean);
    CHECK(s.ok);
    CHECK(s.ntrks == 17u);
  }
  for (unsigned every : {1u, 2u, 3u, 5u}) {
    EditRig r;
    fixture(r, 16);
    for (unsigned t = 0; t < 16; ++t) {
      r.api.pat_len[t] = 128;
      for (unsigned row = 0; row < 128; row += 2) {
        r.api.steps[t][row] = note_at(static_cast<std::uint8_t>(30 + row % 60), 1, static_cast<std::uint8_t>(1 + row));
        r.api.steps[t][row].fx1_type = row % 8 == 0 ? 2 : 0;
        r.api.steps[t][row].fx1_value = static_cast<std::uint8_t>(row);
      }
    }
    r.api.busy_every = every == 1u ? 2u : every;  // (every call BUSY would never finish: every second)
    NEED(r.start(O3M_SMFEXPORT_HOST_SO, kReadExport));
    run_export(r, 2000);
    NEED(r.api.exports.size() == 1u);
    CHECK(r.api.busy_answers > 2u);
    CHECK(r.api.exports[0].data == clean);
  }
}

TEST_CASE("smfexport app: the status is polled until DONE; WRITING answers keep the page at EXPORTING") {
  EditRig r;
  fixture(r);
  r.api.status_delay = 4;
  NEED(r.start(O3M_SMFEXPORT_HOST_SO, kReadExport));
  r.press(O3M_BTN_F1);
  Drawing d = r.draw();
  CHECK(r.shows(d, "EXPORTING"));
  for (int i = 0; i < 3; ++i) {
    d = r.draw();
  }
  CHECK(r.shows(d, "EXPORTING"));
  for (int i = 0; i < 6; ++i) {
    d = r.draw();
  }
  CHECK(r.shows(d, "DONE"));
  CHECK_FALSE(r.shows(d, "EXPORTING"));
  CHECK(r.api.status_polls >= 5u);
  // a second F1 makes a second file (the OS names it)
  r.api.final_name = "PAT001_001.mid";
  run_export(r);
  CHECK(r.api.exports.size() == 2u);
  CHECK(r.shows(r.draw(), "PAT001_001.mid"));
}

TEST_CASE("smfexport app: the F7 TOOLS selection is the range: its tracks and rows only, from the first selected row, named PAT001_SEL") {
  EditRig r;
  fixture(r);
  r.api.pat_len[2] = 8;
  r.launch_tools(1, 0, 4, 8, 2);  // tracks 2 and 3 (index 1 and 2), rows 5..12 (index 4..11)
  NEED(r.start(O3M_SMFEXPORT_HOST_SO, kReadExport));
  run_export(r);
  NEED(r.api.exports.size() == 1u);
  CHECK(r.api.exports[0].name == "PAT001_SEL");
  const Smf s = parse(r.api.exports[0].data);
  NEED(s.ok);
  CHECK(s.ntrks == 3u);
  CHECK(s.names[1] == "Track 2");
  CHECK(s.names[2] == "Track 3");
  // track index 1: notes at rows 4 and 8 of the pattern -> ticks 0 and 96 of the block; its range is 8 rows long
  const std::vector<Note> a = s.notes(1);
  NEED(a.size() == 2u);
  CHECK((a[0].on == 0u && a[0].off == 96u && a[0].key == 48u + 1u + 4u));
  CHECK((a[1].on == 96u && a[1].off == 8u * 24u && a[1].key == 48u + 1u + 8u));
  CHECK(s.track_ticks[1] == 8u * 24u);
  // track index 2 is 8 rows long: the block starts at row 4 and gets 4 rows: the note at row 4 only
  const std::vector<Note> b = s.notes(2);
  NEED(b.size() == 1u);
  CHECK((b[0].on == 0u && b[0].off == 4u * 24u));
  CHECK(s.track_ticks[2] == 4u * 24u);
}

TEST_CASE("smfexport app: a MIDI instrument's channel, and a sample instrument's volume as the default velocity") {
  EditRig r;
  r.api.pat_tracks = 2;
  r.api.add_sample(1, 0, 0.0f);  // VOLUME 50
  r.api.instr[2].kind = O3M_INSTRUMENT_MIDI;
  r.api.instr[2].midi_channel = 11;
  r.api.instr[3].kind = O3M_INSTRUMENT_MIDI;
  r.api.instr[3].midi_channel = 0;  // (a channel the OS did not report: the track's)
  r.api.steps[0][0] = note_at(60, 1);   // sample, no volume: 50 * 127 / 100
  r.api.steps[0][4] = note_at(62, 2);   // MIDI channel 11, no volume: 127
  r.api.steps[0][8] = note_at(64, 2, 33);
  r.api.steps[1][0] = note_at(60, 3);
  NEED(r.start(O3M_SMFEXPORT_HOST_SO, kReadExport));
  run_export(r);
  NEED(r.api.exports.size() == 1u);
  const Smf s = parse(r.api.exports[0].data);
  NEED(s.ok);
  const std::vector<Note> a = s.notes(1);
  NEED(a.size() == 3u);
  CHECK((a[0].channel == 0u && a[0].vel == 63u));
  CHECK((a[1].channel == 10u && a[1].vel == 127u));
  CHECK((a[2].channel == 10u && a[2].vel == 33u));
  const std::vector<Note> b = s.notes(2);
  NEED(b.size() == 1u);
  CHECK((b[0].channel == 1u && b[0].vel == 127u));
  // the instruments were asked once each, not once a note
  CHECK(r.api.info_reads <= 3);
}

TEST_CASE("smfexport app: a block that starts below the top takes, for a note with no instrument, the last one the track names above it") {
  EditRig r;
  r.api.pat_tracks = 2;
  r.api.instr[2].kind = O3M_INSTRUMENT_MIDI;
  r.api.instr[2].midi_channel = 7;
  r.api.instr[3].kind = O3M_INSTRUMENT_MIDI;
  r.api.instr[3].midi_channel = 9;
  r.api.steps[0][1] = note_at(60, 2);  // above the block: the track plays instrument 2 from here
  r.api.steps[0][6] = note_at(62, 0);  // in the block, names none: instrument 2, channel 7
  r.api.steps[1][6] = note_at(64, 0);  // another track, nothing named above it: the page's instrument (the cursor's), here none: the track's channel
  r.launch_tools(0, 0, 4, 8, 2);
  NEED(r.start(O3M_SMFEXPORT_HOST_SO, kReadExport));
  run_export(r);
  NEED(r.api.exports.size() == 1u);
  const Smf s = parse(r.api.exports[0].data);
  NEED(s.ok);
  const std::vector<Note> a = s.notes(1);
  NEED(a.size() == 1u);
  CHECK((a[0].channel == 6u && a[0].on == 2u * 24u));
  const std::vector<Note> b = s.notes(2);
  NEED(b.size() == 1u);
  CHECK(b[0].channel == 1u);
}

namespace {
int g_steps_calls = 0;
std::int32_t (*g_orig_get_steps)(std::uint16_t, std::uint8_t, std::uint8_t, o3m_step*, std::uint8_t) = nullptr;
}  // namespace

TEST_CASE("smfexport app: a track that loses its notes after the header counted it is written empty, so that the file stays well-formed") {
  EditRig r;
  r.api.pat_tracks = 2;
  r.api.steps[0][0] = note_at(60, 1);
  r.api.steps[1][0] = note_at(62, 1);
  g_steps_calls = 0;
  g_orig_get_steps = r.api.read.get_steps;
  r.api.read.get_steps = [](std::uint16_t p, std::uint8_t t, std::uint8_t row, o3m_step* out, std::uint8_t n) -> std::int32_t {
    ++g_steps_calls;
    return g_steps_calls == 4 ? O3M_EDIT_RANGE : g_orig_get_steps(p, t, row, out, n);  // the first pass reads 2 tracks (calls 1, 2); the second pass's read of track 2 (call 4) fails
  };
  NEED(r.start(O3M_SMFEXPORT_HOST_SO, kReadExport));
  run_export(r);
  NEED(r.api.exports.size() == 1u);
  const Smf s = parse(r.api.exports[0].data);
  NEED(s.ok);
  CHECK(s.ntrks == 3u);  // the tempo track and the two the header counted
  CHECK(s.notes(1).size() == 1u);
  CHECK(s.notes(2).empty());
  CHECK(s.names[2] == "Track 2");
}

TEST_CASE("smfexport app: the instrument-info of an OS of 1.6 (16 bytes) gives no channel; the app uses the track's") {
  EditRig r;
  r.api.pat_tracks = 1;
  r.api.instr[2].kind = O3M_INSTRUMENT_MIDI;
  r.api.instr[2].midi_channel = 11;
  r.api.steps[0][0] = note_at(60, 2);
  // the app asks with its 20-byte struct; an OS of 1.6 answers size 16 and leaves midi_channel as the app set it (0)
  r.api.read.instrument_info = [](std::uint8_t, o3m_instrument_info* out) -> std::int32_t {
    out->kind = O3M_INSTRUMENT_MIDI;
    out->engine = 0;
    out->param_count = 0;
    out->size = 16;
    return O3M_EDIT_OK;
  };
  NEED(r.start(O3M_SMFEXPORT_HOST_SO, kReadExport));
  run_export(r);
  NEED(r.api.exports.size() == 1u);
  const Smf s = parse(r.api.exports[0].data);
  NEED(s.ok);
  const std::vector<Note> a = s.notes(1);
  NEED(a.size() == 1u);
  CHECK(a[0].channel == 0u);
}

TEST_CASE("smfexport app: PATTERN (the encoder) picks the pattern; one that does not exist says so and writes nothing") {
  EditRig r;
  fixture(r);
  NEED(r.start(O3M_SMFEXPORT_HOST_SO, kReadExport));
  r.turn(1);  // pattern 2: the fake has only pattern 1
  Drawing d = r.draw();
  CHECK(r.shows(d, "2 (EMPTY)"));
  r.press(O3M_BTN_F1);
  d = r.draw();
  CHECK(r.shows(d, "DOES NOT EXIST"));
  CHECK(r.api.exports.empty());
  r.turn(-1);
  CHECK(r.shows(r.draw(), "WHOLE PATTERN"));
  r.turn(-5);  // the lowest is 1
  CHECK(r.shows(r.draw(), "PATTERN"));
  run_export(r);
  CHECK(r.api.exports.size() == 1u);
  // a tool launched on a pattern with a block ignores the block for another pattern; the pattern in view is the context's
  EditRig q;
  fixture(q);
  q.launch_tools(0, 0, 0, 4, 1);
  q.api.launch.pattern = 1;
  NEED(q.start(O3M_SMFEXPORT_HOST_SO, kReadExport));
  CHECK(q.shows(q.draw(), "BLOCK  TRACK 1  ROWS 1-4"));
}

TEST_CASE("smfexport app: every refusal the OS can answer is on the page, nothing is written after it, and the next F1 starts again") {
  struct Case {
    int open, write, close;
    std::uint32_t status;
    const char* shows;
  };
  const Case cases[] = {
      {O3M_EXPORT_NO_CARD, 0, 0, O3M_EXPORT_DONE, "NO CARD"},
      {O3M_EXPORT_LIMIT, 0, 0, O3M_EXPORT_DONE, "16 FILES"},
      {O3M_EXPORT_RANGE, 0, 0, O3M_EXPORT_DONE, "NOT EXPORTED"},
      {O3M_EXPORT_STATE, 0, 0, O3M_EXPORT_DONE, "NOT EXPORTED"},
      {0, O3M_EXPORT_LIMIT, 0, O3M_EXPORT_DONE, "NOT EXPORTED  (CODE 3)"},
      {0, O3M_EXPORT_STATE, 0, O3M_EXPORT_DONE, "NOT EXPORTED  (CODE 4)"},
      {0, 0, O3M_EXPORT_STATE, O3M_EXPORT_DONE, "NOT EXPORTED  (CODE 4)"},
      {0, 0, 0, O3M_EXPORT_FAILED, "NOT WRITTEN"},
  };
  for (const Case& c : cases) {
    EditRig r;
    fixture(r);
    r.api.open_result = c.open;
    r.api.write_result = c.write;
    r.api.close_result = c.close;
    r.api.final_status = c.status;
    NEED(r.start(O3M_SMFEXPORT_HOST_SO, kReadExport));
    run_export(r);
    const Drawing d = r.draw();
    CHECK_MESSAGE(r.shows(d, c.shows), c.shows);
    CHECK_FALSE(r.shows(d, "DONE"));
    if (c.open != 0) {
      CHECK(r.api.write_calls == 0u);
    }
    // the API is healthy again: F1 works
    r.api.open_result = r.api.write_result = r.api.close_result = 0;
    r.api.final_status = O3M_EXPORT_DONE;
    r.api.export_open_now = false;
    run_export(r);
    CHECK_MESSAGE(r.shows(r.draw(), "DONE"), c.shows);
  }
}

TEST_CASE("smfexport app: no notes, a host without the export calls, and a project-less host") {
  {
    EditRig r;
    r.api.pat_tracks = 2;
    NEED(r.start(O3M_SMFEXPORT_HOST_SO, kReadExport));
    r.press(O3M_BTN_F1);
    settle(r);
    CHECK(r.shows(r.draw(), "NO NOTES IN THE RANGE"));
    CHECK(r.api.exports.empty());
  }
  {
    EditRig r;
    fixture(r);
    NEED(r.start(O3M_SMFEXPORT_HOST_SO, O3M_CAP_READ_PROJECT));  // the export members are NULL
    r.press(O3M_BTN_F1);
    CHECK(r.shows(r.draw(), "CANNOT EXPORT"));
    CHECK(r.api.exports.empty());
  }
  {
    EditRig r;
    NEED(r.lib.open(O3M_SMFEXPORT_HOST_SO));
    o3m_host h = make_app_host(r.api, kReadExport);
    h.read = nullptr;  // no read_project at all: the app refuses at init
    CHECK(!r.inst.start_with(*r.lib.entry, h));
  }
}

TEST_CASE("smfexport app: the page stays inside the canvas at the smallest body and at the biggest, in every state") {
  EditRig r;
  fixture(r);
  r.launch_tools(0, 0, 0, 16, 4);
  NEED(r.start(O3M_SMFEXPORT_HOST_SO, kReadExport));
  r.check_layout(480, 288, "idle small");
  r.check_layout(800, 384, "idle big");
  run_export(r);
  r.check_layout(480, 288, "done small");
  r.check_layout(800, 384, "done big");
  // a failure with a long message
  r.api.open_result = O3M_EXPORT_NO_CARD;
  r.press(O3M_BTN_F1);
  settle(r);
  r.check_layout(480, 288, "failed small");
}

TEST_CASE("smfexport app: the packed .tmod asks for read_project and export_files, declares ABI 1.7, and fits the module slab") {
  const Tmod t = load_tmod(O3M_SMFEXPORT_TMOD);
  if (!t.present) {
    return;
  }
  CHECK(t.header == Status::Ok);
  CHECK(t.crc_ok);
  CHECK(t.relocation == Status::Ok);
  CHECK(t.descriptor == Status::Ok);
  CHECK(t.h.kind == O3M_KIND_APP);
  CHECK(t.h.abi_minor == 10u);
  CHECK(t.h.reserved2[0] == (O3M_CAP_READ_PROJECT | O3M_CAP_EXPORT_FILES));
  CHECK(t.h.state_bytes <= 2912u);  // (SDRAM, one app at a time; the sliced builder of FQA-10b1 is +80 B)
  CHECK(t.h.hot_size == 0u);  // an app has no hot code: nothing goes into the SRAM slab
  MESSAGE("smfexport.tmod: " << t.file_bytes << " bytes (image " << t.h.image_size << ", " << t.h.reloc_count << " relocations)");
  CHECK(t.file_bytes < 16384u);
}

TEST_CASE("smfexport app: PATTERN is kept with set_own_param (AUTO is 0), a launch from MODULES starts at the kept pattern, a launch from TOOLS at the cursor's (OS-APPUX S3)") {
  EditRig r;
  r.launch_tools(0, 0, 0, 0, 0);  // the cursor's pattern is 1
  NEED(r.start(O3M_SMFEXPORT_HOST_SO, kReadExport));
  CHECK(r.api.own_params.empty());
  r.turn(4);  // pattern 5
  NEED(r.api.own_params.size() == 1u);
  CHECK((r.api.own_params[0].index == 0u && r.api.own_params[0].value == 5.0f));
  r.press(O3M_BTN_DELETE);  // the pattern it opened on, kept as AUTO
  CHECK((r.api.own_params.size() == 2u && r.api.own_params[1].index == 0u && r.api.own_params[1].value == 0.0f));
  // from TOOLS the kept value is not the start (the cursor is): pattern 9 kept, the page opens on the cursor's pattern 1
  r.inst.set(0, 9.0f);
  r.enter();
  CHECK(r.shows(r.draw(), "1"));
  CHECK(!r.shows(r.draw(), "9"));
  // from MODULES (no launch context) the kept pattern is the start; AUTO is pattern 1; a value past the range is clamped
  r.api.launch = {sizeof(o3m_launch_ctx), 0, 0, 0, 0, 0, 0, 0, {0, 0, 0}};
  r.inst.set(0, 9.0f);
  r.enter();
  CHECK(r.shows(r.draw(), "9"));
  r.inst.set(0, 0.0f);
  r.enter();
  CHECK(r.shows(r.draw(), "1"));
  r.inst.set(0, 5000.0f);
  r.enter();
  CHECK(r.shows(r.draw(), "255"));
}
