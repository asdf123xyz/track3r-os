// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading a step from the model arena with a generation check (seq/rows.h, OS-005c): the copy is of the project's step; an edit that lands inside the window is
// detected and the copy is made again; a pattern that goes away reads as empty; a second tear is reported. Host and Cortex-M4F (`qemu_seq_model`).
#include <doctest/doctest.h>

#include <o3os/model/project.h>
#include <o3os/seq/rows.h>

#include <cstdint>

#include "support/seq_fixture.h"

using namespace o3os;
using namespace o3os::test;

TEST_CASE("seq rows: the copy of a step is the step, and a missing pattern reads as empty") {
  fresh_project(4, 2);
  model::Project& p = seq_project();
  CHECK(p.create_pattern(7, 16) == model::Status::Ok);
  const model::Step s = make_step(60, 2, seq::fx::kRoll, 4, seq::fx::kPanning, 80, 99);
  CHECK(p.set_step(7, 2, 5, s) == model::Status::Ok);
  model::Step out = make_step(1);
  CHECK(seq::read_step(p, 7, 2, 5, out) == seq::RowStatus::Ok);
  CHECK(out.note == s.note);
  CHECK(out.instr == s.instr);
  CHECK(out.vol == s.vol);
  CHECK(out.fx[0].type == s.fx[0].type);
  CHECK(out.fx[0].value == s.fx[0].value);
  CHECK(out.fx[1].type == s.fx[1].type);
  CHECK(out.fx[1].value == s.fx[1].value);
  // an empty step is read as it is
  CHECK(seq::read_step(p, 7, 0, 0, out) == seq::RowStatus::Ok);
  CHECK(model::is_empty(out));
  // no such pattern, a track or a step out of range: the empty step and NoPattern
  out = make_step(1);
  CHECK(seq::read_step(p, 8, 0, 0, out) == seq::RowStatus::NoPattern);
  CHECK(model::is_empty(out));
  out = make_step(1);
  CHECK(seq::read_step(p, 0, 0, 0, out) == seq::RowStatus::NoPattern);
  CHECK(model::is_empty(out));
  CHECK(seq::read_step(p, 7, 16, 0, out) == seq::RowStatus::NoPattern);
  CHECK(seq::read_step(p, 7, 0, 128, out) == seq::RowStatus::NoPattern);
  // a step past the track's length is still the step (the model keeps them)
  CHECK(p.set_step(7, 0, 100, s) == model::Status::Ok);
  CHECK(seq::read_step(p, 7, 0, 100, out) == seq::RowStatus::Ok);
  CHECK(out.note == s.note);
}

TEST_CASE("seq rows: an edit inside the window is detected and the copy repeated; a second one is a tear") {
  // The check over two functions, so that a test can land an edit between the reads.
  std::uint32_t gen = 5;
  int copies = 0;
  // no edit: one copy
  CHECK(seq::checked_copy([&] { return gen; }, [&] { ++copies; }) == seq::RowStatus::Ok);
  CHECK(copies == 1);
  // an edit during the first copy: the generation moves, the second attempt is clean
  copies = 0;
  gen = 5;
  CHECK(seq::checked_copy([&] { return gen; }, [&] {
          if (++copies == 1) {
            ++gen;
          }
        }) == seq::RowStatus::Ok);
  CHECK(copies == 2);
  // edits during both: torn
  copies = 0;
  CHECK(seq::checked_copy([&] { return gen; }, [&] {
          ++copies;
          ++gen;
        }) == seq::RowStatus::Torn);
  CHECK(copies == 2);  // bounded: two attempts
  // a pattern that vanishes (generation 0) is NoPattern, before or between
  gen = 0;
  copies = 0;
  CHECK(seq::checked_copy([&] { return gen; }, [&] { ++copies; }) == seq::RowStatus::NoPattern);
  CHECK(copies == 0);
  gen = 9;
  copies = 0;
  CHECK(seq::checked_copy([&] { return gen; }, [&] {
          ++copies;
          gen = 0;  // deleted while being read
        }) == seq::RowStatus::NoPattern);
  CHECK(copies == 1);
}

TEST_CASE("seq rows: the generation really moves on an edit, and track lengths are clamped") {
  fresh_project(4, 1);
  model::Project& p = seq_project();
  CHECK(p.create_pattern(3, 8) == model::Status::Ok);
  const std::uint32_t g0 = p.pattern_generation(3);
  CHECK(g0 != 0u);
  CHECK(p.set_step(3, 0, 0, make_step(50)) == model::Status::Ok);
  CHECK(p.pattern_generation(3) != g0);
  CHECK(seq::track_length(p, 3, 0, 99) == 8u);
  CHECK(seq::track_length(p, 9, 0, 99) == 99u);  // no pattern: the fallback
  CHECK(p.set_track_length(3, 1, 5) == model::Status::Ok);
  CHECK(seq::track_length(p, 3, 1, 99) == 5u);
  CHECK(seq::track_length(p, 3, 16, 7) == 7u);
  CHECK(p.delete_pattern(3) == model::Status::Ok);
  CHECK(p.pattern_generation(3) == 0u);
  model::Step out;
  CHECK(seq::read_step(p, 3, 0, 0, out) == seq::RowStatus::NoPattern);
}
