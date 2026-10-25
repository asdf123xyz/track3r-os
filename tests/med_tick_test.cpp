// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MED tick processor (audio/med_tick.h, OS-008c) against libxmp's frames (tests/data/med_tick_golden.txt, made by tools/med_tick_golden.py: libxmp is not in the
// build) and against the formula of each command. Pure code: host and Cortex-M4F.
#include <doctest/doctest.h>

#include <o3os/audio/med_tick.h>
#include <o3os/audio/paula.h>

#include "support/need.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using o3os::audio::med::Cfg;
using o3os::audio::med::Note;
using o3os::audio::med::Out;
using o3os::audio::med::Track;

struct Cell {
  int row, note, inst, cmd, data;
};
struct Frame {
  long period;  // 1/4096 units
  int volume;
  int restart;
};
struct Scenario {
  std::string name;
  int stslide = 0, speed = 6, rows = 0;
  std::vector<Cell> cells;
  std::vector<Frame> frames;
};

std::vector<Scenario> load_golden() {
  std::vector<Scenario> out;
#ifdef O3_MED_TICK_GOLDEN
  std::FILE* f = std::fopen(O3_MED_TICK_GOLDEN, "r");
  if (f == nullptr) {
    return out;
  }
  char line[256];
  while (std::fgets(line, sizeof line, f) != nullptr) {
    if (line[0] == '#') {
      continue;
    }
    char name[64];
    int a, b, c, d, e;
    long p;
    if (std::sscanf(line, "scenario %63s stslide %d speed %d rows %d", name, &a, &b, &c) == 4) {
      Scenario s;
      s.name = name;
      s.stslide = a;
      s.speed = b;
      s.rows = c;
      out.push_back(s);
    } else if (std::sscanf(line, "cell %d %d %d %d %d", &a, &b, &c, &d, &e) == 5) {
      out.back().cells.push_back({a, b, c, d, e});
    } else if (std::sscanf(line, "frame %d %ld %d %d", &a, &p, &b, &c) == 4) {
      out.back().frames.push_back({p, b, c});
    }
  }
  std::fclose(f);
#endif
  return out;
}

// Drives a Track the way audio::AmigaVoices drives it from the sequencer's events (audio/amiga_voices.h): a row with a note replaces both columns, a row without
// a note changes a column that has a command and leaves an empty one, and the importer writes `E00 00` on the row after a command ("no effect"), which is what
// ends a one-row command here. MED's 0C is the step's volume column, the instrument's volume 64 otherwise.
struct Rig {
  Track t;
  Cfg cfg;
  int vol = 64;
  bool col_cmd = false;
  bool cut = false;

  void row(const Cell* c, int speed, std::vector<Out>& frames, int ft = 0) {
    const bool note = c != nullptr && c->note != 0;
    const bool has_cmd = c != nullptr && c->cmd != 0x0C && (c->cmd != 0 || c->data != 0);
    if (has_cmd) {
      t.set_cmd(0, static_cast<std::uint8_t>(c->cmd), static_cast<std::uint8_t>(c->data));
      col_cmd = true;
    } else if (note || col_cmd) {
      if (note) {
        t.end_cmd(0);
      } else {
        t.set_cmd(0, 0, 0);  // the importer's terminator
      }
      col_cmd = false;
    }
    if (note) {
      Note n;
      n.note = c->note + 47;
      n.finetune = ft;
      n.volume = static_cast<std::uint8_t>(c->cmd == 0x0C ? (c->data > 64 ? 64 : c->data) : 64);
      t.note_on(n);
    }
    for (int k = 0; k < speed; ++k) {
      Out o;
      t.tick(static_cast<unsigned>(k), static_cast<unsigned>(speed), cfg, o);
      if (o.start) {
        cut = false;
      }
      cut = cut || o.cut;
      if (cut) {
        o.volume = 0;  // a cut note is silent until the next one (what libxmp reports)
      }
      frames.push_back(o);
    }
  }
};

std::vector<Out> run(const Scenario& s) {
  Rig rig;
  rig.cfg.slide_tick0 = s.stslide == 0;
  std::vector<Out> frames;
  for (int r = 0; r < s.rows; ++r) {
    const Cell* cell = nullptr;
    for (const Cell& c : s.cells) {
      if (c.row == r) {
        cell = &c;
      }
    }
    rig.row(cell, s.speed, frames);
  }
  return frames;
}

}  // namespace

TEST_CASE("med tick: the golden frames of libxmp") {
  const std::vector<Scenario> all = load_golden();
  NEED(all.size() >= 20);
  for (const Scenario& s : all) {
    const std::vector<Out> got = run(s);
    NEED(got.size() == s.frames.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
      INFO(s.name << " frame " << i);
      const double want_period = static_cast<double>(s.frames[i].period) / 4096.0;
      CHECK(std::fabs(static_cast<double>(got[i].period) - want_period) <= 1.0);
      CHECK(static_cast<int>(got[i].volume) == s.frames[i].volume);
      CHECK((got[i].start ? 1 : 0) == s.frames[i].restart);
    }
  }
}

TEST_CASE("med tick: formulas the golden frames cannot show") {
  Cfg cfg;
  Out o;
  {  // the second column runs too, and with the first (a slide in one, a volume slide in the other)
    Track t;
    Note n;
    n.note = 60;
    n.volume = 40;
    t.note_on(n);
    t.set_cmd(0, 0x01, 0x04);
    t.set_cmd(1, 0x0D, 0x02);
    t.tick(0, 6, cfg, o);
    CHECK(o.start);
    CHECK(o.period == 428 - 4);  // the slide runs on tick 0 (OctaMED's default)
    CHECK(o.volume == 38);
    t.tick(1, 6, cfg, o);
    CHECK(o.period == 428 - 8);
    CHECK(o.volume == 36);
    CHECK(!o.start);
  }
  {  // the SoundTracker flag: slides from tick 1
    Track t;
    cfg.slide_tick0 = false;
    Note n;
    t.note_on(n);
    t.set_cmd(0, 0x02, 0x05);
    t.tick(0, 6, cfg, o);
    CHECK(o.period == 428);
    t.tick(1, 6, cfg, o);
    CHECK(o.period == 433);
    cfg.slide_tick0 = true;
  }
  {  // Paula's range: 113 at the top, 65535 at the bottom
    Track t;
    Note n;
    n.note = 83;  // B-3: 113
    t.note_on(n);
    t.set_cmd(0, 0x01, 0xFF);
    t.tick(0, 6, cfg, o);
    CHECK(o.period == 113);
    Track u;
    u.note_on(n);
    u.set_cmd(0, 0x02, 0xFF);
    for (unsigned k = 0; k < 6; ++k) {
      u.tick(k, 6, cfg, o);
    }
    CHECK(o.period == 113 + 6 * 255);
    for (int row = 0; row < 60; ++row) {
      for (unsigned k = 0; k < 6; ++k) {
        u.tick(k, 6, cfg, o);
      }
    }
    CHECK(o.period == 65535);
  }
  {  // a command that is not in the table does nothing, and no command is no command
    Track t;
    Note n;
    t.note_on(n);
    t.set_cmd(0, 0x2A, 0x77);
    t.set_cmd(1, 0x00, 0x00);
    t.tick(0, 6, cfg, o);
    t.tick(1, 6, cfg, o);
    CHECK(o.period == 428);
    CHECK(o.volume == 64);
  }
  {  // 0F F8 / F9: the LED filter, once at the row
    Track t;
    t.set_cmd(0, 0x0F, 0xF9);
    t.tick(0, 6, cfg, o);
    CHECK(o.led == 1);
    t.tick(1, 6, cfg, o);
    CHECK(o.led == -1);  // said once, at the row's tick 0
  }
}
