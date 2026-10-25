// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "support/mmd_fixture.h"
#include "support/need.h"
#include "support/volume_law.h"

#include <o3os/formats/med/map.h>
#include <o3os/formats/med/mmd.h>
#include <o3os/model/step.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

using namespace o3os;
using namespace o3os::formats::med;
using o3os::test::mmd::Bytes;
using o3os::test::mmd::InstrSpec;
using o3os::test::mmd::ModuleSpec;

// The pure part of slice M2 (docs/formats/med.md sections 3 and 4): the arithmetic of notes, volumes and tempo, the command
// table row by row, the track plan and the instrument mapping. Every expected value was worked out by hand (or with exact
// integer arithmetic in a separate script) from the rules in map.h, not copied from the output; the second opinion on the
// pitch, the notes and the loops is libopenmpt (tests/med_openmpt_oracle.py).

namespace {

SongInfo song_of(unsigned version = 1) {
  SongInfo s;
  s.version = static_cast<std::uint8_t>(version);
  s.def_tempo = 33;
  s.tempo2 = 6;
  return s;
}

Cell cell(unsigned note, unsigned inst, unsigned cmd, unsigned data, unsigned ext = 0, bool hi = false) {
  Cell c;
  c.note = static_cast<std::uint8_t>(note);
  c.instrument = static_cast<std::uint8_t>(inst);
  c.command = static_cast<std::uint8_t>(cmd);
  c.data = static_cast<std::uint8_t>(data);
  c.ext = static_cast<std::uint8_t>(ext);
  c.note_hi = hi;
  return c;
}

}  // namespace

// ---- notes -------------------------------------------------------------------------------------------------------------

TEST_CASE("med map: a note is the MED note + 48 + playtransp, 24 less in MMD3") {
  SongInfo s = song_of(1);
  struct Row { unsigned version; int transp; unsigned note; unsigned want; bool out; };
  const Row rows[] = {
      {1, 0, 1, 49, false},     // C-1 is our C-4: libopenmpt's own note 49
      {1, 0, 13, 61, false},    // C-2 is the unity note, 61
      {0, 0, 63, 111, false},   // the top of an MMD0 cell
      {2, 0, 80, 128, false},   // the last note the model holds
      {2, 0, 81, 0, true},      // one above: dropped
      {1, 0, 127, 0, true},
      {1, -12, 13, 49, false},  // playtransp
      {1, 12, 1, 61, false},
      {1, -49, 1, 0, true},     // below C-0
      {1, -48, 1, 1, false},    // C-0 itself
      {1, -47, 1, 2, false},
      {3, 0, 1, 25, false},     // MMD3: two octaves lower
      {3, 0, 37, 61, false},
      {3, 0, 104, 128, false},
      {3, -30, 1, 0, true},
  };
  for (const Row& r : rows) {
    CAPTURE(r.version);
    CAPTURE(r.transp);
    CAPTURE(r.note);
    s.version = static_cast<std::uint8_t>(r.version);
    s.play_transpose = static_cast<std::int8_t>(r.transp);
    const NoteResult n = map_note(s, cell(r.note, 0, 0, 0));
    CHECK(n.note == r.want);
    CHECK(n.out_of_range == r.out);
    CHECK(!n.cut);
  }
}

TEST_CASE("med map: no note is none, the exact byte 0x80 is a cut, bit 7 on a note is ignored") {
  const SongInfo s = song_of(1);
  NoteResult n = map_note(s, cell(0, 5, 0, 0));
  CHECK(n.note == 0);
  CHECK(!n.out_of_range);
  n = map_note(s, cell(0, 0, 0, 0, 0, true));  // 0x80
  CHECK(n.note == model::kNoteCut);
  CHECK(n.cut);
  n = map_note(s, cell(1, 0, 0, 0, 0, true));  // 0x81: OpenMPT masks the bit and shows C-4 (note 49)
  CHECK(n.note == 49);
  CHECK(n.hi_ignored);
  CHECK(!n.cut);
}

// ---- volume ------------------------------------------------------------------------------------------------------------

TEST_CASE("med map: 0C is decimal-coded, 7 bit when the song says VOLHEX") {
  SongInfo s = song_of(1);
  struct Row { unsigned data; unsigned want; bool clamped; };
  // decimal-coded: 0x64 is 64; the model's byte is 2 x v capped at 127 (back: (v127 + 1) / 2)
  const Row dec[] = {{0x00, 0, false}, {0x01, 2, false}, {0x32, 64, false}, {0x40, 80, false}, {0x63, 126, false},
                     {0x64, 127, false}, {0x65, 127, true}, {0x1F, 50, false}, {0x7F, 127, true}, {0xFF, 127, true}};
  for (const Row& r : dec) {
    CAPTURE(r.data);
    const VolumeResult v = map_volume(s, static_cast<std::uint8_t>(r.data));
    CHECK(v.value == r.want);
    CHECK(v.clamped == r.clamped);
  }
  s.flags = kFlagVolHex;
  // libopenmpt 0.7.3 reads ((v & 0x7F) + 1) / 2 here (0x40 is 32, 0x7F is 64); the model's column is 7 bit, so the byte is kept
  const Row hex[] = {{0x00, 0, false}, {0x20, 32, false}, {0x40, 64, false}, {0x7F, 127, false}, {0x80, 0, true}, {0xFF, 127, true}};
  for (const Row& r : hex) {
    CAPTURE(r.data);
    const VolumeResult v = map_volume(s, static_cast<std::uint8_t>(r.data));
    CHECK(v.value == r.want);
    CHECK(v.clamped == r.clamped);
  }
  // 0..64 of the original survive the round trip
  s.flags = 0;
  for (unsigned v64 = 0; v64 <= 64; ++v64) {
    const std::uint8_t data = static_cast<std::uint8_t>(((v64 / 10) << 4) | (v64 % 10));
    CHECK(volume_to_med(map_volume(s, data).value) == v64);
  }
}

// ---- tempo -------------------------------------------------------------------------------------------------------------

TEST_CASE("med map: a BPM as the Tempo FX and its fine correction") {
  struct Row { unsigned x100; unsigned value; unsigned fine; bool exact; bool clamped; };
  const Row rows[] = {
      {12338, 62, 126, false, false},  // 123.38: 124 BPM minus half a BPM is 123.5, 0.12 off
      {12500, 63, 124, true, false},   // 125.00: 126 minus one BPM
      {12400, 62, 0, true, false},     // 124.00: no correction
      {24677, 123, 131, false, false},
      {20000, 100, 0, true, false},
      {19999, 100, 0, false, false},   // under a quarter of a BPM off: no correction, inexact
      {4000, 20, 0, true, false},      // 40 BPM, the model's lowest
      {40099, 200, 132, false, false}, // 400.99: the last that rounds to 200, a whole BPM over
      {40100, 200, 0, false, true},    // 401.00: past the scale
      {46736, 200, 0, false, true},    // above 400 BPM: the nearest
      {80000, 200, 0, false, true},
  };
  for (const Row& r : rows) {
    CAPTURE(r.x100);
    const TempoFx t = tempo_fx(r.x100);
    CHECK(t.value == r.value);
    CHECK(t.fine == r.fine);
    CHECK(t.exact == r.exact);
    CHECK(t.clamped == r.clamped);
  }
}

TEST_CASE("med map: the Tempo FX is within an eighth of a BPM of every BPM the model holds, up to 400") {
  for (unsigned x = model::kMinTempoX100; x <= 40000; ++x) {
    const TempoFx t = tempo_fx(x);
    NEED(!t.clamped);
    NEED(t.value >= 5 && t.value <= 200);
    NEED(t.fine == 0 || (t.fine >= 124 && t.fine <= 132 && t.fine != 128));
    const int k = t.fine == 0 ? 0 : static_cast<int>(t.fine) - 128;
    const int back = 200 * static_cast<int>(t.value) + 25 * k;  // hundredths of a BPM
    const int err = back > static_cast<int>(x) ? back - static_cast<int>(x) : static_cast<int>(x) - back;
    NEED(err <= 12);
    NEED(t.exact == (err == 0));
  }
  for (unsigned x = 40101; x <= model::kMaxTempoX100; x += 97) {
    CHECK(tempo_fx(x).clamped);
  }
}

TEST_CASE("med map: roll, micro-move, gate, LFO rate, glide") {
  bool e = false;
  // retrigger every y ticks in a line of 6: 3 -> twice (period 2), 2 -> three times (3), 1 -> six (period 5 = 1/6)
  CHECK(roll_for(6, 3, e) == 2);
  CHECK(e);
  CHECK(roll_for(6, 2, e) == 3);
  CHECK(e);
  CHECK(roll_for(6, 1, e) == 5);
  CHECK(e);
  CHECK(roll_for(8, 1, e) == 6);   // 8 per step is period 6
  CHECK(e);
  CHECK(roll_for(12, 1, e) == 7);
  CHECK(e);
  CHECK(roll_for(16, 1, e) == 8);
  CHECK(e);
  CHECK(roll_for(6, 5, e) == 0);   // closer to one per line than to two
  CHECK(!e);
  CHECK(roll_for(6, 6, e) == 0);   // never fires inside the line: nothing, and exact
  CHECK(e);
  CHECK(roll_for(6, 0, e) == 0);
  CHECK(e);
  CHECK(roll_for(5, 1, e) == 4);   // five per line: nearest of 4 (err 1) and 6 (err 1): the smaller wins
  CHECK(!e);
  CHECK(roll_for(7, 2, e) == 3);   // 7 / 2 = 3.5: 3 (err 1) over 4 (err 1): the smaller
  CHECK(!e);
  CHECK(micro_move_for(6, 3, e) == 50);
  CHECK(e);
  CHECK(micro_move_for(6, 2, e) == 33);
  CHECK(!e);
  CHECK(micro_move_for(4, 1, e) == 25);
  CHECK(e);
  CHECK(micro_move_for(6, 9, e) == 100);  // a delay past the line
  CHECK(!e);
  CHECK(gate_for(6, 3, e) == 50);
  CHECK(e);
  CHECK(gate_for(6, 4, e) == 67);
  CHECK(!e);
  CHECK(lfo_rate_for(0, 28) == 0);
  CHECK(lfo_rate_for(15, 28) == 28);
  CHECK(lfo_rate_for(3, 28) == 6);
  CHECK(lfo_rate_for(15, 23) == 23);
  CHECK(lfo_rate_for(4, 23) == 6);
  // glide: 214 period units at `speed` per tick, tick 20,263 us: speed 16 is 13.4 ticks, 271 ms, preset 14 (20 ms per step to 1 s)
  CHECK(glide_for(16, 20263) == 14);
  CHECK(glide_for(1, 20263) == 62);    // 4,336 ms: past one second, 280 ms per step
  CHECK(glide_for(2, 20263) == 54);    // 2,168 ms
  CHECK(glide_for(255, 20263) == 1);   // 17 ms
  CHECK(glide_for(1, 600000) == 100);  // far beyond 15 s: the end of the scale
}

// ---- tracks ------------------------------------------------------------------------------------------------------------

namespace {
ImportReport& report() {
  static ImportReport r;
  r.reset();
  return r;
}
}  // namespace

TEST_CASE("med map: tracks that carry nothing take no slot, the rest keep their order") {
  ImportReport& r = report();
  r.track[0].cells = 5;
  r.track[3].cells = 1;
  r.track[7].cells = 9;
  CHECK(plan_tracks(r) == 3);
  CHECK(r.tracks_used == 3);
  CHECK(r.tracks_kept == 3);
  CHECK(r.tracks_dropped == 0);
  CHECK(r.track[0].slot == 0);
  CHECK(r.track[1].slot == kNoSlot);
  CHECK(r.track[3].slot == 1);
  CHECK(r.track[7].slot == 2);
}

TEST_CASE("med map: more than 16 used tracks keep the busiest, the lower track winning a tie, in MED order") {
  ImportReport& r = report();
  for (unsigned t = 0; t < 18; ++t) {
    r.track[t].cells = 50;  // eighteen equal tracks: two too many
  }
  r.track[20].cells = 1;    // the quietest of all
  r.track[21].cells = 200;  // the busiest, last in MED order
  CHECK(plan_tracks(r) == 16);
  CHECK(r.tracks_used == 20);
  CHECK(r.tracks_kept == 16);
  CHECK(r.tracks_dropped == 4);
  // dropped: track 20 (1 cell) and, of the equal ones, the three highest numbers (the lower track wins a tie)
  CHECK(r.track[20].slot == kNoSlot);
  CHECK(r.track[17].slot == kNoSlot);
  CHECK(r.track[16].slot == kNoSlot);
  CHECK(r.track[15].slot == kNoSlot);
  for (unsigned t = 0; t < 15; ++t) {
    CHECK(r.track[t].slot == t);  // kept in MED order
  }
  CHECK(r.track[21].slot == 15);
}

TEST_CASE("med map: 64 used tracks, the busiest 16, no slot twice") {
  ImportReport& r = report();
  for (unsigned t = 0; t < 64; ++t) {
    r.track[t].cells = 1 + (t * 37u) % 61u;  // distinct enough, with a few ties
  }
  CHECK(plan_tracks(r) == 16);
  CHECK(r.tracks_dropped == 48);
  std::uint32_t slots = 0, min_kept = ~0u, max_dropped = 0;
  for (unsigned t = 0; t < 64; ++t) {
    if (r.track[t].slot != kNoSlot) {
      CHECK(r.track[t].slot < 16);
      CHECK(((slots >> r.track[t].slot) & 1u) == 0);
      slots |= 1u << r.track[t].slot;
      min_kept = r.track[t].cells < min_kept ? r.track[t].cells : min_kept;
    } else {
      max_dropped = r.track[t].cells > max_dropped ? r.track[t].cells : max_dropped;
    }
  }
  CHECK(slots == 0xFFFFu);
  CHECK(min_kept >= max_dropped);  // nothing dropped is busier than anything kept
}

TEST_CASE("med map: pan and volume of a track") {
  SongInfo s = song_of(1);
  // Paula's hardware panning: L R R L, repeated (8 channels: L R R L L R R L)
  const unsigned want[8] = {0, 100, 100, 0, 0, 100, 100, 0};
  for (unsigned t = 0; t < 8; ++t) {
    CHECK(track_pan_percent(s, false, 0, t) == want[t]);
  }
  s.version = 2;
  s.flags2 = kFlag2Mix;  // SoundStudio mixing: no hardware to copy, centre
  CHECK(track_pan_percent(s, false, 0, 1) == 50);
  CHECK(track_pan_percent(s, true, 0, 1) == 50);
  CHECK(track_pan_percent(s, true, -16, 1) == 0);
  CHECK(track_pan_percent(s, true, 16, 1) == 100);
  CHECK(track_pan_percent(s, true, 8, 1) == 75);
  CHECK(track_pan_percent(s, true, -8, 1) == 25);
  CHECK(track_pan_percent(s, true, 1, 1) == 53);
  CHECK(track_pan_percent(s, true, 100, 1) == 100);   // out of range: clamped
  CHECK(track_pan_percent(s, true, -100, 1) == 0);
  s.flags2 = 0;
  CHECK(track_pan_percent(s, false, 0, 1) == 100);    // an MMD2 song without a pan table and without mixing is Paula's
  CHECK(track_volume_percent(64) == 100);
  CHECK(track_volume_percent(32) == 50);
  CHECK(track_volume_percent(0) == 0);
  CHECK(track_volume_percent(1) == 2);
  CHECK(track_volume_percent(200) == 100);
}

// ---- the commands, row by row -----------------------------------------------------------------------------------------

namespace {

struct Harness {
  SongInfo song = song_of(1);
  ImportOptions opt;
  InstFact facts[caps::kMaxSamples + 1];
  ImportReport rep;
  LineMapper m;

  // instrument 1: a sample of 1000 frames, default volume 64; 2: MIDI, default volume 32; 3: a sample of 65,536 frames, volume 64
  void start(bool structural = false, unsigned tempo = 33, unsigned spd = 6) {
    rep.reset();
    for (InstFact& f : facts) f = InstFact();
    facts[1].svol = 64;
    facts[1].frames = 1000;
    facts[2].svol = 32;
    facts[2].midi = true;
    facts[3].svol = 64;
    facts[3].frames = 65536;
    m.begin(song, opt, facts, tempo, spd, structural, rep);
    m.begin_piece(0, 0, false);
  }
  model::Step one(unsigned note, unsigned inst, unsigned cmd, unsigned data, unsigned ext = 0) {
    m.begin_line();
    m.cell(0, cell(note, inst, cmd, data, ext), 0);
    m.end_line(1);
    return m.step(0);
  }
};

Harness& h() {
  static Harness x;  // 5 KB: not on the stack
  return x;
}

struct Want {
  unsigned note = 61, inst = 1, vol = 255;
  unsigned t1 = 0, v1 = 0, t2 = 0, v2 = 0;
};

void check_step(const model::Step& s, const Want& w) {
  CHECK(s.note == w.note);
  CHECK(s.instr == w.inst);
  CHECK(s.vol == w.vol);
  CHECK(s.fx[0].type == w.t1);
  CHECK(s.fx[0].value == w.v1);
  CHECK(s.fx[1].type == w.t2);
  CHECK(s.fx[1].value == w.v2);
}

unsigned dropped(unsigned cmd) { return h().rep.cmd_dropped[cmd]; }
unsigned approx(unsigned cmd) { return h().rep.cmd_approx[cmd]; }

}  // namespace

TEST_CASE("med map: commands that become an OG FX (expected values by hand)") {
  Harness& x = h();
  struct Row {
    unsigned cmd, data;
    Want want;
    bool approx_class;
  };
  // Tempo 33, speed 6, PAL: 123.38 BPM, a tick of 20,263 us; instrument 1 has 1,000 frames
  const Row rows[] = {
      {0x01, 0x10, {61, 1, 255, 41, 16}, true},        // slide up: the slide FX (41), unit unconfirmed
      {0x02, 0x20, {61, 1, 255, 42, 32}, true},
      {0x03, 0x10, {61, 1, 255, 19, 14}, true},        // glide: 271 ms is preset 14
      {0x04, 0x35, {61, 1, 255, 33, 6}, true},         // vibrato speed 3 of 15 on 28 presets is 6
      {0x14, 0x35, {61, 1, 255, 33, 6}, true},
      {0x07, 0x4F, {61, 1, 255, 23, 6}, true},         // tremolo speed 4 of 15 on 23 presets
      {0x0F, 0xF1, {61, 1, 255, 3, 2}, false},         // twice per line: Roll 1/2
      {0x0F, 0xF3, {61, 1, 255, 3, 3}, false},         // three times: Roll 1/3
      {0x0F, 0xF2, {61, 1, 255, 2, 50}, true},         // delay: Micro-move 50
      {0x0F, 0xFD, {61, 1, 255, 19, 0}, true},         // set pitch: a glide at full speed
      {0x15, 0x00, {61, 1, 255, 34, 99}, true},        // finetune 0 is in tune: 99
      {0x15, 0x03, {61, 1, 255, 34, 136}, true},       // 99 + 12.375 x 3
      {0x15, 0x07, {61, 1, 255, 34, 186}, true},
      {0x15, 0xF8, {61, 1, 255, 34, 0}, true},         // -8: the bottom of the scale
      {0x15, 0xFF, {61, 1, 255, 34, 87}, true},        // -1
      {0x18, 0x03, {61, 1, 255, 20, 50}, false},       // cut after 3 of 6 ticks
      {0x18, 0x04, {61, 1, 255, 20, 67}, true},
      {0x19, 0x02, {61, 1, 255, 22, 51}, true},        // 512 of 1,000 frames
      {0x2E, 0x08, {61, 1, 255, 31, 75}, true},        // track pan 8 of 16
      {0x2E, 0xF0, {61, 1, 255, 31, 0}, true},         // -16
      {0x2E, 0x10, {61, 1, 255, 31, 100}, true},
      {0x2E, 0x00, {61, 1, 255, 31, 50}, true},
  };
  for (const Row& r : rows) {
    CAPTURE(r.cmd);
    CAPTURE(r.data);
    x.start();
    const model::Step s = x.one(13, 1, r.cmd, r.data);
    check_step(s, r.want);
    CHECK(approx(r.cmd) == (r.approx_class ? 1u : 0u));
    CHECK(dropped(r.cmd) == 0);
    CHECK(x.rep.fx_overflow == 0);
  }
}

TEST_CASE("med map: tempo, speed and the fine tempo") {
  Harness& x = h();
  x.start();
  // 0F 21: tempo 33 at speed 6 is 123.38 BPM: Tempo 62 and a half BPM off it
  model::Step s = x.one(0, 0, 0x0F, 0x21);
  check_step(s, {0, 0, 255, 15, 62, 44, 126});
  CHECK(approx(0x0F) == 1);
  CHECK(x.rep.tempo_fx == 1);
  CHECK(x.rep.tempo_fx_fine == 1);
  // 0F 7D: tempo 125 is 467.36 BPM, above 400: the nearest Tempo, counted as clamped
  x.start();
  s = x.one(0, 0, 0x0F, 0x7D);
  check_step(s, {0, 0, 255, 15, 200});
  CHECK(x.rep.tempo_fx_clamped == 1);
  CHECK(approx(0x0F) == 1);
  // 09 03: speed 3 at tempo 33 is 246.77 BPM (24,677): Tempo 123, Speed 3; the fine correction would need a third slot
  x.start();
  s = x.one(0, 0, 0x09, 0x03);
  check_step(s, {0, 0, 255, 15, 123, 43, 3});
  CHECK(approx(0x09) == 1);
  CHECK(x.rep.speed_fx == 1);
  CHECK(x.rep.tempo_fx_fine == 0);
  CHECK(x.m.speed() == 3);
  // the state follows the line: speed 3, then tempo 60 at speed 3: 1500 x 709,379 x 60 / (474,326 x 3) = 44,865 -> Tempo 224 clamped
  s = x.one(0, 0, 0x0F, 0x3C);
  CHECK(s.fx[0].type == 15);
  CHECK(x.m.tempo() == 60);
  // 09 with a value the flow ignores
  x.start();
  s = x.one(0, 0, 0x09, 0x00);
  check_step(s, {0, 0, 255});
  CHECK(dropped(0x09) == 1);
  x.start();
  s = x.one(0, 0, 0x09, 0x21);
  CHECK(dropped(0x09) == 1);
  CHECK(x.m.speed() == 6);
  // 125 BPM exactly (BPM mode 125, 4 lines per beat, speed 6): Tempo 63 less one BPM, exact: no approximation counted
  x.song.flags2 = kFlag2Bpm | 3;
  x.start(false, 125, 6);
  s = x.one(0, 0, 0x0F, 0x7D);
  check_step(s, {0, 0, 255, 15, 63, 44, 124});
  CHECK(approx(0x0F) == 0);
  x.song.flags2 = 0;
}

TEST_CASE("med map: speed and tempo state restart at a piece, from the piece's entry values") {
  Harness& x = h();
  x.start();
  x.m.begin_piece(100, 4, false);
  CHECK(x.m.tempo() == 100);
  CHECK(x.m.speed() == 4);
  // 100 at speed 4: 1500 x 709,379 x 100 / (474,326 x 4) = 56,083 -> clamped (560 BPM)
  model::Step s = x.one(0, 0, 0x09, 0x06);   // speed 6 at tempo 100: 37,389 -> Tempo 187, r = -11 -> k = 0: no fine
  CHECK(s.fx[0].type == 15);
  CHECK(s.fx[0].value == 187);
  x.m.begin_piece(0, 0, false);              // a piece that sets no tempo or speed: the song's start
  CHECK(x.m.tempo() == 33);
  CHECK(x.m.speed() == 6);
}

TEST_CASE("med map: commands the flow has already executed leave nothing in a linearised song") {
  Harness& x = h();
  const struct { unsigned cmd, data; } rows[] = {{0x0B, 0x02}, {0x16, 0x03}, {0x1D, 0x10}, {0x1E, 0x04}, {0x0F, 0x00}, {0x0F, 0xFE}};
  for (const auto& r : rows) {
    CAPTURE(r.cmd);
    x.start();
    const model::Step s = x.one(13, 1, r.cmd, r.data);
    check_step(s, {61, 1, 255});
    CHECK(dropped(r.cmd) == 0);
    CHECK(approx(r.cmd) == 0);
  }
}

TEST_CASE("med map: in the structural fallback breaks become Break Pattern, jumps Order jump, the rest is dropped and counted") {
  Harness& x = h();
  x.start(true);
  model::Step s = x.one(13, 1, 0x0F, 0x00);
  check_step(s, {61, 1, 255, 13, 1});
  CHECK(approx(0x0F) == 0);
  x.start(true);
  s = x.one(13, 1, 0x1D, 0x00);
  check_step(s, {61, 1, 255, 13, 1});
  CHECK(approx(0x1D) == 0);
  x.start(true);
  s = x.one(13, 1, 0x1D, 0x08);  // the target line is dropped
  check_step(s, {61, 1, 255, 13, 1});
  CHECK(approx(0x1D) == 1);
  for (unsigned cmd : {0x16u, 0x1Eu}) {
    x.start(true);
    s = x.one(13, 1, cmd, 0x03);
    check_step(s, {61, 1, 255});
    CHECK(dropped(cmd) == 1);
  }
  // 0B xx is core FX 45 Order jump, approximate: the order counts the pieces the fallback cut, not the play sequence's entries
  x.start(true);
  s = x.one(13, 1, 0x0B, 0x03);
  check_step(s, {61, 1, 255, 45, 3});
  CHECK(approx(0x0B) == 1);
  CHECK(dropped(0x0B) == 0);
  x.start(true);
  s = x.one(13, 1, 0x0B, 0xFE);   // position 254, the last the model has
  check_step(s, {61, 1, 255, 45, 254});
  x.start(true);
  s = x.one(13, 1, 0x0B, 0xFF);   // 255 is no position
  check_step(s, {61, 1, 255});
  CHECK(dropped(0x0B) == 1);
  x.start(true);
  s = x.one(13, 1, 0x0F, 0xFE);  // the end of the song cannot be kept
  check_step(s, {61, 1, 255});
  CHECK(dropped(0x0F) == 1);
}

TEST_CASE("med map: commands without a target are dropped and counted by command") {
  Harness& x = h();
  const unsigned rows[][2] = {{0x00, 0x37}, {0x05, 0x12}, {0x06, 0x12}, {0x08, 0x21}, {0x0A, 0x05}, {0x0D, 0x05}, {0x0E, 0x01},
                              {0x10, 0x40}, {0x11, 0x08}, {0x12, 0x08}, {0x1C, 0x05}, {0x13, 0x01}, {0x17, 0x01}, {0x21, 0x01},
                              {0x28, 0x01}, {0x2A, 0x01}, {0x2D, 0x01}, {0x2F, 0x01}, {0x30, 0x01}, {0x36, 0x01}, {0x3F, 0x01},
                              {0x40, 0x01}, {0xFF, 0x01}, {0x0F, 0xF8}, {0x0F, 0xF9}, {0x0F, 0xFA}, {0x0F, 0xFB}, {0x0F, 0xF4},
                              {0x0F, 0xF7}, {0x0F, 0xFC}, {0x03, 0x00}, {0x20, 0x05}};
  for (const auto& r : rows) {
    CAPTURE(r[0]);
    CAPTURE(r[1]);
    x.start();
    const model::Step s = x.one(13, 1, r[0], r[1]);
    check_step(s, {61, 1, 255});
    CHECK(dropped(r[0]) == 1);
    CHECK(approx(r[0]) == 0);
  }
}

TEST_CASE("med map: 0C goes into the volume column, 1A and 1B bake from the volume the track has") {
  Harness& x = h();
  x.start();
  model::Step s = x.one(13, 1, 0x0C, 0x40);
  check_step(s, {61, 1, 80});
  s = x.one(13, 1, 0x0C, 0x65);
  check_step(s, {61, 1, 127});
  CHECK(x.rep.volume_clamped == 1);
  // an instrument number sets the volume to its default (64); 1B 10 then takes 16 off: 48, as the column 96
  x.start();
  s = x.one(13, 1, 0x1B, 0x10);
  check_step(s, {61, 1, 96});
  s = x.one(0, 0, 0x1B, 0x10);  // no instrument this time: the volume is the 48 left from the line before: 32 -> 64
  check_step(s, {0, 0, 64});
  s = x.one(0, 0, 0x1A, 0x05);  // 37 -> 74
  check_step(s, {0, 0, 74});
  s = x.one(0, 0, 0x1A, 0xFF);  // up to 64: full
  check_step(s, {0, 0, 127});
  s = x.one(0, 0, 0x1B, 0xFF);  // down to 0
  check_step(s, {0, 0, 0});
  // 0C then 1A in one line (two pages): the later volume wins and the overwrite is counted
  x.start();
  x.m.begin_line();
  x.m.cell(0, cell(13, 1, 0x0C, 0x20), 0);   // 20 decimal-coded = 20 -> column 40
  x.m.cell(0, cell(0, 0, 0x1A, 0x05), 1);    // 25 -> 50
  CHECK(x.m.step(0).vol == 50);
  CHECK(x.rep.volume_overwritten == 1);
  // a track that never had a volume: 1A has nothing to start from
  x.start();
  s = x.one(0, 0, 0x1A, 0x05);
  check_step(s, {0, 0, 255});
  CHECK(dropped(0x1A) == 1);
  CHECK(x.rep.state_unknown == 1);
  // VOLHEX: 7 bit, kept
  x.song.flags = kFlagVolHex;
  x.start();
  s = x.one(13, 1, 0x0C, 0x40);
  check_step(s, {61, 1, 64});
  x.song.flags = 0;
}

TEST_CASE("med map: 1F, 20 and 29 need what they act on") {
  Harness& x = h();
  // 1F x y: delay x ticks, retrigger every y; speed 6. 23: Roll 1/2 (exact) and Micro-move 33 (inexact)
  x.start();
  model::Step s = x.one(13, 1, 0x1F, 0x23);
  check_step(s, {61, 1, 255, 3, 2, 2, 33});
  CHECK(approx(0x1F) == 1);
  x.start();
  s = x.one(13, 1, 0x1F, 0x03);
  check_step(s, {61, 1, 255, 3, 2});
  CHECK(approx(0x1F) == 0);
  x.start();
  s = x.one(13, 1, 0x1F, 0x30);
  check_step(s, {61, 1, 255, 2, 50});
  CHECK(approx(0x1F) == 0);
  x.start();
  s = x.one(13, 1, 0x1F, 0x05);  // every 5 of 6 ticks: closer to one per line than to two: no Roll, an approximation
  check_step(s, {61, 1, 255});
  CHECK(approx(0x1F) == 1);
  x.start();
  s = x.one(0, 1, 0x1F, 0x23);   // no note: nothing happens
  check_step(s, {0, 1, 255});
  CHECK(approx(0x1F) == 0);
  CHECK(dropped(0x1F) == 0);
  // 20 00 reverses a note; without a note, nothing; 20 xx skips samples: dropped
  x.start();
  s = x.one(13, 1, 0x20, 0x00);
  check_step(s, {61, 1, 255, 26, 1});
  x.start();
  s = x.one(0, 1, 0x20, 0x00);
  check_step(s, {0, 1, 255});
  CHECK(dropped(0x20) == 0);
  // 29: xx of the extension byte of the length, as a percentage
  x.start();
  s = x.one(13, 1, 0x29, 0x08, 0x10);
  check_step(s, {61, 1, 255, 22, 50});
  CHECK(approx(0x29) == 1);
  x.start();
  s = x.one(13, 1, 0x29, 0x08, 0x00);  // without the extension byte there is no length
  check_step(s, {61, 1, 255});
  CHECK(dropped(0x29) == 1);
  // 19 uses the instrument of the cell, else the one the track played last, else drops
  x.start();
  s = x.one(13, 3, 0x19, 0x80);        // instrument 3 has 65,536 frames: 32,768 of them is exactly half
  check_step(s, {61, 3, 255, 22, 50});
  CHECK(approx(0x19) == 0);
  s = x.one(13, 0, 0x19, 0x40);        // no instrument number: still 3: 16,384 is a quarter
  check_step(s, {61, 0, 255, 22, 25});
  x.start();
  s = x.one(13, 0, 0x19, 0x40);        // nothing played yet on this track
  check_step(s, {61, 0, 255});
  CHECK(dropped(0x19) == 1);
  CHECK(x.rep.state_unknown == 1);
  x.start();
  s = x.one(13, 2, 0x19, 0x40);        // instrument 2 is MIDI: no sample to take a percentage of
  CHECK(dropped(0x19) == 1);
  // 0F FF is a cut when the cell has no note, dropped when it has one
  x.start();
  s = x.one(0, 0, 0x0F, 0xFF);
  check_step(s, {model::kNoteCut, 0, 255});
  x.start();
  s = x.one(13, 1, 0x0F, 0xFF);
  check_step(s, {61, 1, 255});
  CHECK(dropped(0x0F) == 1);
}

TEST_CASE("med map: MIDI controllers are CC A..E on a MIDI instrument only") {
  Harness& x = h();
  x.start();
  model::Step s = x.one(13, 2, 0x31, 0x7F);
  check_step(s, {61, 2, 255, 8, 127});
  CHECK(approx(0x31) == 1);
  s = x.one(13, 2, 0x35, 0xFF);                      // 0x80 and up: the controller range is 0..127
  check_step(s, {61, 2, 255, 12, 127});
  s = x.one(13, 2, 0x36, 0x01);                      // the sixth has no CC
  CHECK(dropped(0x36) == 1);
  s = x.one(13, 1, 0x32, 0x01);                      // a sample instrument
  check_step(s, {61, 1, 255});
  CHECK(dropped(0x32) == 1);
  x.start();
  s = x.one(13, 0, 0x32, 0x01);                      // no instrument at all
  CHECK(dropped(0x32) == 1);
  CHECK(x.rep.state_unknown == 1);
}

TEST_CASE("med map: a third command finds no FX slot and is dropped and counted") {
  Harness& x = h();
  x.start();
  x.m.begin_line();
  x.m.cell(0, cell(13, 1, 0x0C, 0x40), 0);   // the volume column: no slot used
  x.m.cell(0, cell(0, 0, 0x01, 0x10), 1);    // slot 1
  x.m.cell(0, cell(0, 0, 0x02, 0x10), 2);    // slot 2
  x.m.cell(0, cell(0, 0, 0x04, 0x35), 3);    // none left
  check_step(x.m.step(0), {61, 1, 80, 41, 16, 42, 16});
  CHECK(dropped(0x04) == 1);
  CHECK(x.rep.fx_overflow == 1);
  CHECK(approx(0x04) == 0);
  // 09 needs two slots (Tempo, Speed): with one taken, the Speed is dropped, the Tempo kept
  x.start();
  x.m.begin_line();
  x.m.cell(0, cell(13, 1, 0x01, 0x10), 0);
  x.m.cell(0, cell(0, 0, 0x09, 0x03), 1);
  check_step(x.m.step(0), {61, 1, 255, 41, 16, 15, 123});
  CHECK(dropped(0x09) == 1);
}

TEST_CASE("med map: a replayed line (1E) has no notes and no instruments, its commands stay") {
  Harness& x = h();
  x.start();
  x.m.begin_piece(0, 0, true);
  const model::Step s = x.one(13, 1, 0x0C, 0x40);
  check_step(s, {0, 0, 80});
  CHECK(x.rep.notes_out_of_range == 0);
}

TEST_CASE("med map: tracks are independent and out-of-range notes are counted") {
  Harness& x = h();
  x.start();
  x.m.begin_line();
  x.m.cell(0, cell(13, 1, 0x0C, 0x40), 0);
  x.m.cell(5, cell(100, 3, 0, 0), 0);        // MMD1 note 100 + 48: beyond 128
  x.m.cell(5, cell(0, 0, 0x1A, 0x08), 1);    // track 5 has instrument 3 (volume 64): 64
  x.m.cell(64, cell(1, 1, 0, 0), 0);         // a track number past the model's is ignored
  CHECK(x.m.step(0).vol == 80);
  CHECK(x.m.step(0).instr == 1);
  CHECK(x.m.step(5).note == 0);
  CHECK(x.m.step(5).instr == 3);
  CHECK(x.m.step(5).vol == 127);
  CHECK(x.rep.notes_out_of_range == 1);
  CHECK(x.m.step(1).instr == 0);
  const model::Step cut = [&] {
    x.m.begin_line();
    x.m.cell(2, cell(0, 4, 0, 0, 0, true), 0);   // 0x80: a cut
    return x.m.step(2);
  }();
  CHECK(cut.note == model::kNoteCut);
  CHECK(cut.instr == 4);
  CHECK(x.rep.note_cuts == 1);
}

TEST_CASE("med map: a note without an instrument keeps the volume a command gave the track (libopenmpt measured)") {
  Harness& x = h();
  x.start();
  // 0C 20 is 20 of 64: the next bare note plays at 20, not at the instrument's 64, so the step carries it (column 40)
  check_step(x.one(13, 1, 0x0C, 0x20), {61, 1, 40});
  check_step(x.one(15, 0, 0, 0), {63, 0, 40});
  CHECK(x.rep.volume_baked == 1);
  // an instrument number sets the instrument's volume again: nothing to write, and the bare note after it needs nothing either
  check_step(x.one(13, 1, 0, 0), {61, 1, 255});
  check_step(x.one(15, 0, 0, 0), {63, 0, 255});
  CHECK(x.rep.volume_baked == 1);
  // a volume the cell gives itself is not overwritten; a cell without a note has nothing to carry it
  check_step(x.one(13, 1, 0x0C, 0x10), {61, 1, 20});
  check_step(x.one(15, 0, 0x0C, 0x30), {63, 0, 60});
  CHECK(x.rep.volume_baked == 1);
  check_step(x.one(0, 0, 0, 0x00), {0, 0, 255});
  CHECK(x.rep.volume_baked == 1);
  // 1B changes it too (it is baked into the column of its own cell, and the next bare note keeps that)
  x.start();
  check_step(x.one(13, 1, 0x1B, 0x10), {61, 1, 96});
  check_step(x.one(15, 0, 0, 0), {63, 0, 96});
  CHECK(x.rep.volume_baked == 1);
  // the instrument's own default is not written even when a command once set it: MIDI instrument 2 has volume 32
  x.start();
  check_step(x.one(13, 2, 0x0C, 0x32), {61, 2, 64});
  check_step(x.one(15, 0, 0, 0), {63, 0, 255});
  CHECK(x.rep.volume_baked == 0);
  // a track that never had a volume has nothing to keep; a track without a slot is not touched (end_line's mask)
  x.start();
  check_step(x.one(15, 0, 0, 0), {63, 0, 255});
  x.start();
  x.m.begin_line();
  x.m.cell(0, cell(13, 1, 0x0C, 0x20), 0);
  x.m.end_line(0);
  x.m.begin_line();
  x.m.cell(0, cell(15, 0, 0, 0), 0);
  x.m.end_line(0);
  CHECK(x.m.step(0).vol == 255);
  CHECK(x.rep.volume_baked == 0);
  // a replayed line has no notes
  x.start();
  x.one(13, 1, 0x0C, 0x20);
  x.m.begin_piece(0, 0, true);
  check_step(x.one(15, 0, 0, 0), {0, 0, 255});
  // 0C 00 is silence, and it stays: the bare note after it is baked as 0
  x.start();
  x.one(13, 1, 0x0C, 0x00);
  check_step(x.one(15, 0, 0, 0), {63, 0, 0});
  CHECK(x.rep.volume_baked == 1);
}

TEST_CASE("med map: a track without a slot still moves time: 09 and 0F tempo are mapped, nothing else is") {
  Harness& x = h();
  x.start();
  x.m.begin_line();
  x.m.timing_cell(19, cell(13, 1, 0x0F, 0x32));        // tempo 50 at speed 6: 186.94 BPM, Tempo 93 and a correction of 1 BPM
  CHECK(x.m.step(19).fx[0].type == 15);
  CHECK(x.m.step(19).fx[0].value == 93);
  CHECK(x.m.step(19).fx[1].type == 44);
  CHECK(x.m.step(19).fx[1].value == 132);
  CHECK(x.m.step(19).note == 0);                        // no note, no instrument: only what moves time
  CHECK(x.m.step(19).instr == 0);
  CHECK(x.m.tempo() == 50);
  x.m.begin_line();
  x.m.timing_cell(19, cell(0, 0, 0x09, 0x05));          // speed 5 at tempo 50: 224.33 BPM: Tempo 112, Speed 5
  CHECK(x.m.step(19).fx[0].type == 15);
  CHECK(x.m.step(19).fx[0].value == 112);
  CHECK(x.m.step(19).fx[1].type == 43);
  CHECK(x.m.step(19).fx[1].value == 5);
  CHECK(x.m.speed() == 5);
  // anything else is not touched, and not counted
  const unsigned before_drop = x.rep.cmd_dropped[0x0A], before_approx = x.rep.cmd_approx[0x01];
  x.m.begin_line();
  for (unsigned cmd : {0x01u, 0x0Au, 0x0Cu, 0x15u, 0x1Fu, 0x0Fu}) x.m.timing_cell(19, cell(13, 1, cmd, cmd == 0x0F ? 0xF3u : 0x10u));
  CHECK(model::is_empty(x.m.step(19)));
  CHECK(x.rep.cmd_dropped[0x0A] == before_drop);
  CHECK(x.rep.cmd_approx[0x01] == before_approx);
  x.m.timing_cell(64, cell(0, 0, 0x0F, 0x32));          // not a track
  // the flow commands only in the structural fallback
  x.m.begin_line();
  x.m.timing_cell(19, cell(0, 0, 0x1D, 0x04));
  CHECK(model::is_empty(x.m.step(19)));
  x.start(true);
  x.m.begin_line();
  x.m.timing_cell(19, cell(0, 0, 0x1D, 0x04));
  CHECK(x.m.step(19).fx[0].type == 13);
  x.m.begin_line();
  x.m.timing_cell(19, cell(0, 0, 0x0F, 0x00));
  CHECK(x.m.step(19).fx[0].type == 13);
  x.m.begin_line();
  x.m.timing_cell(19, cell(0, 0, 0x0B, 0x07));
  CHECK(x.m.step(19).fx[0].type == 45);
  CHECK(x.m.step(19).fx[0].value == 7);
}

TEST_CASE("med map: the engine window carries MED commands verbatim as 128 + command") {
  Harness& x = h();
  x.opt.engine_fx = true;
  // every command with an Amiga-engine entry in section 4 (the data byte untouched)
  const unsigned engine_cmds[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x0A, 0x0D, 0x0E, 0x11, 0x12, 0x14, 0x15,
                                  0x18, 0x19, 0x1A, 0x1B, 0x1F, 0x20, 0x29};
  for (unsigned cmd : engine_cmds) {
    CAPTURE(cmd);
    x.start();
    const model::Step s = x.one(13, 1, cmd, 0x5A);
    check_step(s, {61, 1, 255, 128 + cmd, 0x5A});
    CHECK(approx(cmd) == 0);
    CHECK(dropped(cmd) == 0);
  }
  // 0F: the six sub-commands the engine has, verbatim; the five nobody describes are kept raw and counted as dropped
  for (unsigned d : {0xF1u, 0xF2u, 0xF3u, 0xF8u, 0xF9u, 0xFDu}) {
    x.start();
    check_step(x.one(13, 1, 0x0F, d), {61, 1, 255, 143, d});
    CHECK(dropped(0x0F) == 0);
  }
  for (unsigned d : {0xF4u, 0xF5u, 0xF6u, 0xF7u, 0xFCu}) {
    x.start();
    check_step(x.one(13, 1, 0x0F, d), {61, 1, 255, 143, d});
    CHECK(dropped(0x0F) == 1);
  }
  // undefined commands: kept raw, counted
  for (unsigned cmd : {0x13u, 0x17u, 0x21u, 0x28u, 0x2Au, 0x2Du, 0x2Fu, 0x30u}) {
    x.start();
    check_step(x.one(13, 1, cmd, 0x01), {61, 1, 255, 128 + cmd, 0x01});
    CHECK(dropped(cmd) == 1);
  }
  // commands without an entry keep their other target
  x.start();
  check_step(x.one(13, 1, 0x0C, 0x40), {61, 1, 80});
  x.start();
  check_step(x.one(13, 1, 0x2E, 0x08), {61, 1, 255, 31, 75});
  x.start();
  check_step(x.one(13, 1, 0x09, 0x03), {61, 1, 255, 15, 123, 43, 3});
  x.start();
  x.one(13, 1, 0x10, 0x01);
  CHECK(dropped(0x10) == 1);
  // the window is 128..191 for MED commands 00..3F
  CHECK(fx::kEngineBase + 0x3F == fx::kEngineLast);
  // 1E: executed by the flow in a linearised song, a command of the engine in the structural fallback
  x.start();
  check_step(x.one(13, 1, 0x1E, 0x02), {61, 1, 255});
  x.start(true);
  check_step(x.one(13, 1, 0x1E, 0x02), {61, 1, 255, 128 + 0x1E, 0x02});
  // a full step: no slot for the engine command
  x.start();
  x.m.begin_line();
  x.m.cell(0, cell(13, 1, 0x01, 0x10), 0);
  x.m.cell(0, cell(0, 0, 0x02, 0x10), 1);
  x.m.cell(0, cell(0, 0, 0x04, 0x35), 2);
  CHECK(dropped(0x04) == 1);
  CHECK(x.rep.fx_overflow == 1);
  x.opt.engine_fx = false;
}

// ---- instruments -------------------------------------------------------------------------------------------------------

namespace {

struct Mod {
  Bytes bytes;
  SpanSource src;
  Reader rd;
  Status open;
  explicit Mod(const ModuleSpec& m)
      : bytes(test::mmd::build(m)), src(bytes.data(), static_cast<std::uint32_t>(bytes.size())) {
    open = rd.open(src);
  }
};

ModuleSpec module_of(std::vector<InstrSpec> instruments, unsigned version = 1) {
  ModuleSpec m;
  m.version = version;
  test::mmd::BlockSpec b;
  b.tracks = 4;
  b.lines = 4;
  m.blocks = {b};
  m.playseq = {0};
  m.instruments = std::move(instruments);
  if (version >= 2) {
    test::mmd::PlaySeqSpec p;
    p.entries = {0};
    m.pseqs = {p};
    m.sections = {0};
  }
  return m;
}

InstrSpec sample(unsigned frames, int type = 0) {
  InstrSpec s;
  s.type = type;
  for (unsigned i = 0; i < frames * ((type & 0x10) ? 2u : 1u) * ((type & 0x20) ? 2u : 1u); ++i) {
    s.data.push_back(static_cast<std::uint8_t>(i * 3));
  }
  if (type & 0x20) {  // length is one channel's
    s.length = static_cast<long>(frames * ((type & 0x10) ? 2u : 1u));
  }
  return s;
}

struct Mapped {
  bool made = false;
  model::Instrument inst;
  InstReport rep;
};

Mapped map_slot(const ModuleSpec& spec, unsigned slot, Clock clock = Clock::Pal, std::uint64_t left = caps::kSampleArenaBytes) {
  Mapped out;
  Mod m(spec);
  CHECK(m.open.ok());
  InstrumentInfo info;
  if (!m.open.ok() || !m.rd.instrument(slot, info).ok()) {
    FAIL_CHECK("the module did not open or the instrument did not read");
    return out;
  }
  out.made = map_instrument(m.rd, slot, info, clock, left, out.inst, out.rep);
  return out;
}

}  // namespace

TEST_CASE("med map: a plain sample at the Paula rate") {
  InstrSpec s = sample(64);
  s.svol = 48;
  s.name = "lead";
  const Mapped r = map_slot(module_of({s}), 0);
  NEED(r.made);
  CHECK(r.rep.result == InstResult::Sample);
  CHECK(r.rep.frames == 64);
  CHECK(r.inst.kind == model::InstrumentKind::Sample);
  CHECK(std::strcmp(r.inst.sample_path, "samples/instr01.wav") == 0);   // this OS's name for the slot (og.h, the WAV importer)
  CHECK(r.inst.sample_rate == 8287);          // PAL: 3,546,895 / 428
  CHECK(r.inst.sampler.volume == 45);         // svol 48: -2.5 dB, level 45 (OS-BUG-PASTE24; it was the linear 75)
  CHECK(r.inst.sampler.tune == 0);
  CHECK(r.inst.sampler.fine_tune == 0);
  CHECK(r.inst.sampler.play_mode == 0);       // no loop: one-shot
  CHECK(r.inst.sampler.loop_start == 0);
  CHECK(r.inst.sampler.start == 0);
  CHECK(r.inst.sampler.end == 65535);
  CHECK(r.rep.flags == 0);
  CHECK(model::validate_instrument(r.inst, 0) == nullptr);
  const Mapped ntsc = map_slot(module_of({s}), 0, Clock::Ntsc);
  CHECK(ntsc.inst.sample_rate == 8363);       // 3,579,545 / 428
  CHECK(paula_rate(Clock::Pal) == 8287);
  CHECK(paula_rate(Clock::Ntsc) == 8363);
}

TEST_CASE("med map: strans is the tune, clamped to the tune range of 24 and flagged") {
  for (int st : {-12, -1, 0, 5, 24, -24}) {
    CAPTURE(st);
    InstrSpec s = sample(8);
    s.strans = static_cast<std::int8_t>(st);
    const Mapped r = map_slot(module_of({s}), 0);
    NEED(r.made);
    CHECK(r.inst.sampler.tune == st);
    CHECK((r.rep.flags & kInstTuneClamped) == 0);
  }
  for (int st : {25, 40, -25, -128, 127}) {
    CAPTURE(st);
    InstrSpec s = sample(8);
    s.strans = static_cast<std::int8_t>(st);
    const Mapped r = map_slot(module_of({s}), 0);
    NEED(r.made);
    CHECK(r.inst.sampler.tune == (st > 0 ? 24 : -24));
    CHECK((r.rep.flags & kInstTuneClamped) != 0);
  }
}

TEST_CASE("med map: each instrument kind's unity note: types 1, 5, 6 at C-1 (+12), 2, 3, 4 at C-2 (0), type 7 and synth at C-4 (-24)") {
  // octaves 5, 3, 2, 4, 6, 7 for types 1..6; the data holds 2^N - 1 units of 4 frames
  const struct { int type; unsigned octaves; int tune; } rows[] = {{1, 5, 12}, {2, 3, 0}, {3, 2, 0}, {4, 4, 0}, {5, 6, 12}, {6, 7, 12}};
  for (const auto& r : rows) {
    CAPTURE(r.type);
    InstrSpec s;
    s.type = r.type;
    s.data.assign(4u * ((1u << r.octaves) - 1u), 0x22);
    s.strans = 3;
    const Mapped m = map_slot(module_of({s}), 0);
    NEED(m.made);
    CHECK(m.inst.sampler.tune == r.tune + 3);
    CHECK(m.rep.frames == 4u << (r.octaves - 1));   // the longest octave only
    CHECK((m.rep.flags & kInstOctave) != 0);
  }
  InstrSpec e = sample(32, 7);
  const Mapped x = map_slot(module_of({e}), 0);
  NEED(x.made);
  CHECK(x.inst.sampler.tune == -24);
  CHECK((x.rep.flags & kInstExtSample) != 0);
  e.strans = 5;  // -24 + 5
  CHECK(map_slot(module_of({e}), 0).inst.sampler.tune == -19);
  e.strans = -12;  // -36: clamped
  const Mapped c = map_slot(module_of({e}), 0);
  CHECK(c.inst.sampler.tune == -24);
  CHECK((c.rep.flags & kInstTuneClamped) != 0);
}

TEST_CASE("med map: the PCM of a multi-octave sample is the longest octave, where it sits in the file") {
  // 3 octaves of 5 frames: 5 + 10 + 20 = 35; the longest starts at frame 15
  InstrSpec s;
  s.type = 2;
  s.data.assign(35, 0);
  const Mapped r = map_slot(module_of({s}), 0);
  NEED(r.made);
  Mod m(module_of({s}));
  InstrumentInfo info;
  NEED(m.rd.instrument(0, info).ok());
  CHECK(r.rep.src == info.data + 15);
  CHECK(r.rep.frames == 20);
  CHECK(r.rep.src_right == 0);
  CHECK(r.rep.src_bits == 8);
  // 16-bit, two octaves of 4 frames: 4 + 8 = 12 frames of 2 bytes; the longest starts at frame 4
  InstrSpec w;
  w.type = 0x13;
  w.data.assign(24, 0);
  const Mapped r16 = map_slot(module_of({w}), 0);
  NEED(r16.made);
  Mod m16(module_of({w}));
  NEED(m16.rd.instrument(0, info).ok());
  CHECK(r16.rep.src == info.data + 8);
  CHECK(r16.rep.frames == 8);
  CHECK(r16.rep.src_bits == 16);
  CHECK((r16.rep.flags & kInstSixteenBit) != 0);
  // stereo: the right channel is the second block of data
  InstrSpec st = sample(10, 0x20);
  const Mapped rs = map_slot(module_of({st}), 0);
  NEED(rs.made);
  Mod ms(module_of({st}));
  NEED(ms.rd.instrument(0, info).ok());
  CHECK(rs.rep.src == info.data);
  CHECK(rs.rep.src_right == info.data + 10);
  CHECK((rs.rep.flags & kInstStereoMix) != 0);
}

TEST_CASE("med map: finetune is 12.5 cents a step, svol is a level on the law, a disabled instrument is silent") {
  const struct { int ft; int fine; } rows[] = {{-8, -100}, {-1, -13}, {0, 0}, {1, 13}, {2, 25}, {7, 88}};
  for (const auto& r : rows) {
    CAPTURE(r.ft);
    InstrSpec s = sample(8);
    s.ext = {0, 0, 0, static_cast<std::uint8_t>(r.ft), 0, 0};
    ModuleSpec m = module_of({s}, 2);
    m.s_ext_entrsz = 6;
    const Mapped x = map_slot(m, 0);
    NEED(x.made);
    CHECK(x.inst.sampler.fine_tune == r.fine);
  }
  const struct { unsigned svol; unsigned vol; } vols[] = {{0, 0}, {1, 1}, {32, 37}, {48, 45}, {63, 50}, {64, 50}, {200, 50}};
  for (const auto& r : vols) {
    CAPTURE(r.svol);
    InstrSpec s = sample(8);
    s.svol = static_cast<std::uint8_t>(r.svol);
    CHECK(map_slot(module_of({s}), 0).inst.sampler.volume == r.vol);
  }
  InstrSpec d = sample(8);
  d.ext = {0, 0, 0, 0, 0, 4};  // flags: disabled
  ModuleSpec m = module_of({d}, 2);
  m.s_ext_entrsz = 6;
  const Mapped x = map_slot(m, 0);
  NEED(x.made);
  CHECK(x.inst.sampler.volume == 0);
  CHECK((x.rep.flags & kInstDisabled) != 0);
}

TEST_CASE("med map: hold, decay and default pitch are not mapped and say so") {
  InstrSpec s = sample(8);
  s.ext = {2, 3, 0, 0, 25, 0};
  ModuleSpec m = module_of({s}, 2);
  m.s_ext_entrsz = 6;
  const Mapped x = map_slot(m, 0);
  NEED(x.made);
  CHECK((x.rep.flags & kInstHoldDecay) != 0);
  CHECK((x.rep.flags & kInstDefaultPitch) != 0);
  InstrSpec plain = sample(8);
  CHECK((map_slot(module_of({plain}), 0).rep.flags & (kInstHoldDecay | kInstDefaultPitch)) == 0);
}

TEST_CASE("med map: loops, quantised to the model's 16-bit positions, and where libxmp and OpenMPT differ") {
  // 100 frames, loop 20 + 40: positions are 65535 x 20 / 100 = 13107 and 65535 x 60 / 100 = 39321, which the frames recover
  InstrSpec s = sample(100);
  s.rep = 10;     // units of two frames
  s.replen = 20;
  Mapped r = map_slot(module_of({s}), 0);
  NEED(r.made);
  CHECK(r.inst.sampler.play_mode == 1);
  CHECK(r.inst.sampler.loop_start == 13107);
  CHECK(r.inst.sampler.loop_end == 39321);
  CHECK((r.rep.flags & kInstLoopQuantised) == 0);
  // a loop to the end of the sample is 65535
  s.rep = 0;
  s.replen = 50;
  r = map_slot(module_of({s}), 0);
  CHECK(r.inst.sampler.loop_start == 0);
  CHECK(r.inst.sampler.loop_end == 65535);
  CHECK((r.rep.flags & kInstLoopQuantised) == 0);
  // replen 1 is no loop
  s.replen = 1;
  CHECK(map_slot(module_of({s}), 0).inst.sampler.play_mode == 0);
}

TEST_CASE("med map: a multi-octave loop is the shortest octave's, in fractions of any octave") {
  // type 2: 3 octaves of 8 frames (56 in all); loop 2 + 4 frames of the shortest = a quarter .. three quarters
  InstrSpec s;
  s.type = 2;
  s.data.assign(56, 0);
  s.rep = 1;
  s.replen = 2;
  const Mapped r = map_slot(module_of({s}), 0);
  NEED(r.made);
  CHECK(r.rep.frames == 32);
  CHECK(r.inst.sampler.loop_start == 16384);
  CHECK(r.inst.sampler.loop_end == 49151);
  CHECK((r.rep.flags & kInstLoopQuantised) == 0);
}

TEST_CASE("med map: the loop flag: MMD0/1 with an InstrExt that clears it do not loop (OpenMPT), and the difference is flagged") {
  InstrSpec s = sample(20);
  s.rep = 0;
  s.replen = 10;
  // MMD1, no InstrExt: libxmp and OpenMPT agree, the loop plays
  Mapped r = map_slot(module_of({s}, 1), 0);
  CHECK(r.inst.sampler.play_mode == 1);
  CHECK((r.rep.flags & kInstLoopRule) == 0);
  // MMD1 with InstrExt, loop flag set: both loop
  s.ext = {0, 0, 0, 0, 0, 1};
  ModuleSpec m = module_of({s}, 1);
  m.s_ext_entrsz = 6;
  r = map_slot(m, 0);
  CHECK(r.inst.sampler.play_mode == 1);
  CHECK((r.rep.flags & kInstLoopRule) == 0);
  // MMD1 with InstrExt, loop flag clear but replen > 1: libxmp loops, OpenMPT (libopenmpt 0.7.3, measured) does not
  s.ext = {0, 0, 0, 0, 0, 0};
  m = module_of({s}, 1);
  m.s_ext_entrsz = 6;
  r = map_slot(m, 0);
  CHECK(r.inst.sampler.play_mode == 0);
  CHECK((r.rep.flags & kInstLoopRule) != 0);
  // MMD2 reads the flag in both
  m = module_of({s}, 2);
  m.s_ext_entrsz = 6;
  r = map_slot(m, 0);
  CHECK(r.inst.sampler.play_mode == 0);
  CHECK((r.rep.flags & kInstLoopRule) == 0);
  s.ext = {0, 0, 0, 0, 0, 1};
  m = module_of({s}, 2);
  m.s_ext_entrsz = 6;
  CHECK(map_slot(m, 0).inst.sampler.play_mode == 1);
  // replen 1 is the sentinel for "no loop" whatever the flag says: libxmp reads the MMD2 flag alone and loops 2 frames
  s.rep = 0;
  s.replen = 1;
  s.ext = {0, 0, 0, 0, 0, 1};
  m = module_of({s}, 2);
  m.s_ext_entrsz = 6;
  r = map_slot(m, 0);
  CHECK(r.inst.sampler.play_mode == 0);
  CHECK((r.rep.flags & kInstLoopRule) != 0);
  s.replen = 10;
  // ping-pong (flag 8) is play mode 3
  s.ext = {0, 0, 0, 0, 0, 9};
  m = module_of({s}, 2);
  m.s_ext_entrsz = 6;
  CHECK(map_slot(m, 0).inst.sampler.play_mode == 3);
}

TEST_CASE("med map: synth and hybrid instruments become their first waveform, the tables dropped") {
  InstrSpec s;
  s.has_synth = true;
  s.synth.wforms = 2;
  s.synth.waves = {Bytes(8, 0x10), Bytes(16, 0x20)};
  s.strans = 2;
  Mapped r = map_slot(module_of({s}), 0);
  NEED(r.made);
  CHECK(r.rep.frames == 8);
  CHECK((r.rep.flags & kInstSynth) != 0);
  CHECK(r.inst.sampler.tune == -22);              // -24 + strans
  CHECK(r.inst.sampler.play_mode == 1);           // the single cycle loops
  CHECK(r.inst.sampler.loop_start == 0);
  CHECK(r.inst.sampler.loop_end == 65535);
  InstrSpec h;
  h.has_synth = true;
  h.synth.hybrid = true;
  h.synth.wforms = 2;
  h.synth.waves = {Bytes(12, 0x30), Bytes(8, 0x40)};
  r = map_slot(module_of({h}), 0);
  NEED(r.made);
  CHECK(r.rep.frames == 12);
  CHECK((r.rep.flags & kInstHybrid) != 0);
  CHECK(r.inst.sampler.tune == -24);
  // a synth without any waveform has nothing to play
  InstrSpec none;
  none.has_synth = true;
  none.synth.wforms = 0xFFFF;
  r = map_slot(module_of({none}), 0);
  CHECK(!r.made);
  CHECK(r.rep.result == InstResult::NoWave);
}

TEST_CASE("med map: MIDI instruments, empty slots and what is refused") {
  InstrSpec mi;
  mi.midich = 5;
  mi.midipreset = 9;
  mi.svol = 32;
  mi.name = "pad";
  Mapped r = map_slot(module_of({mi}), 0);
  NEED(r.made);
  CHECK(r.rep.result == InstResult::Midi);
  CHECK(r.inst.kind == model::InstrumentKind::Midi);
  CHECK(r.inst.midi_channel == 5);
  CHECK(r.inst.sampler.volume == 50);
  CHECK((r.rep.flags & kInstMidiDropped) != 0);   // the program has no place
  CHECK(model::validate_instrument(r.inst, 0) == nullptr);
  mi.midipreset = 0;
  CHECK((map_slot(module_of({mi}), 0).rep.flags & kInstMidiDropped) == 0);
  mi.midich = 20;
  r = map_slot(module_of({mi}), 0);
  CHECK(r.inst.midi_channel == 16);
  CHECK((r.rep.flags & kInstChannelClamped) != 0);
  // a MIDI instrument that also has a sample: the sample is not kept, and it says so
  InstrSpec both = sample(8);
  both.midich = 3;
  r = map_slot(module_of({both}), 0);
  CHECK(r.rep.result == InstResult::Midi);
  CHECK((r.rep.flags & kInstMidiDropped) != 0);
  // no instrument at all, and an instrument of no frames
  InstrSpec gone;
  gone.present = false;
  r = map_slot(module_of({gone}), 0);
  CHECK(!r.made);
  CHECK(r.rep.result == InstResult::None);
  InstrSpec zero = sample(0);
  r = map_slot(module_of({zero}), 0);
  CHECK(!r.made);
  CHECK(r.rep.result == InstResult::EmptyData);
  // a type the reader does not take (packed data, 0x80)
  InstrSpec packed = sample(8);
  packed.type = 0x80;
  r = map_slot(module_of({packed}), 0);
  CHECK(!r.made);
  CHECK(r.rep.result == InstResult::Unsupported);
}

TEST_CASE("med map: an instrument that does not fit the sample arena is refused with its number") {
  InstrSpec a = sample(1000);
  Mapped r = map_slot(module_of({a}), 0, Clock::Pal, 1999);   // 1,000 frames of 16 bits are 2,000 bytes
  CHECK(!r.made);
  CHECK(r.rep.result == InstResult::ArenaFull);
  r = map_slot(module_of({a}), 0, Clock::Pal, 2000);
  CHECK(r.made);
  // the arena is the one of ADR 0013 amendment 1
  CHECK(caps::kSampleArenaBytes == 10816ull * 1024ull);
}

TEST_CASE("med map: the report starts clean and resets field by field") {
  ImportReport& r = report();
  CHECK(r.patterns == 0);
  CHECK(r.track[0].slot == kNoSlot);
  CHECK(r.track[63].slot == kNoSlot);
  r.cmd_dropped[0xFF] = 3;
  r.cmd_approx[0] = 2;
  r.instrument[62].frames = 9;
  r.track[5].cells = 4;
  r.flow.visits = 5;
  r.reset();
  CHECK(r.cmd_dropped[0xFF] == 0);
  CHECK(r.cmd_approx[0] == 0);
  CHECK(r.instrument[62].frames == 0);
  CHECK(r.track[5].cells == 0);
  CHECK(r.flow.visits == 0);
}

// The MED import: svol 0..64 (InstrSpec::svol, map_slot as med_map_test.cpp uses them) to `sampler.volume`. It was the linear percent (64 -> 100, +24 dB); it is the level on the law now (64 -> 50, 0 dB).
TEST_CASE("OS-BUG-PASTE24 option C: a MED sample volume 64 is level 50 (0 dB) and the rest follow the log law") {
  for (unsigned svol = 0; svol <= 64; ++svol) {
    InstrSpec s = sample(8);
    s.svol = static_cast<std::uint8_t>(svol);
    const Mapped r = map_slot(module_of({s}), 0);
    NEED(r.made);
    o3os::test::check_import_level(svol, r.inst.sampler.volume);
  }
}

// A MED MIDI instrument's `volume` is a velocity, read linearly (default_velocity * 127 / 100): option C (OS-BUG-PASTE24, D2) maps the SAMPLE instruments' levels, never this one, and neither the track volumes
// (track_volume_percent). Plain case: it holds before and after the fix.
TEST_CASE("med map: a MIDI instrument's volume stays the linear velocity percent under option C (OS-BUG-PASTE24)") {
  const struct { unsigned svol; unsigned vol; } vols[] = {{0, 0}, {32, 50}, {48, 75}, {64, 100}};
  for (const auto& r : vols) {
    CAPTURE(r.svol);
    InstrSpec mi;
    mi.midich = 5;
    mi.svol = static_cast<std::uint8_t>(r.svol);
    mi.name = "pad";
    const Mapped m = map_slot(module_of({mi}), 0);
    NEED(m.made);
    CHECK(m.inst.kind == model::InstrumentKind::Midi);
    CHECK(m.inst.sampler.volume == r.vol);
  }
}

