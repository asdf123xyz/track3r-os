// SPDX-License-Identifier: GPL-3.0-or-later
// OS-009 P1a.1: the pad board against the reference vectors: all 48 pads, the 40 scales, every offset,
// every root the Config accepts, and the shape of the board.
// Expected values: output of this project's own reference model (maintainer, 2026-10-18), kept as test vectors (data/pattern_edit_vectors.h): a digest of
// the 48 notes of every board of a scale, and nine boards written out in full. Whether the official firmware agrees is an open research question.
#include <doctest/doctest.h>

#include <o3os/app/pad_board.h>
#include <o3os/core/scales.h>

#include <cstdint>
#include <initializer_list>

#include "data/pattern_edit_vectors.h"
#include "support/step_vectors.h"

using namespace o3os;
namespace vec = o3os::test::vec;
namespace vectors = o3os::test::vectors;

TEST_CASE("pad board: every pad, scale, offset and root against the reference vectors") {
  unsigned boards = 0;
  unsigned group = 0;
  for (unsigned scale = 0; scale < 41; ++scale) {  // 40 is past the table: both clamp it to 39
    if (core::scale_corrected(scale)) {
      continue;  // the four rows PAR-073 names are corrected to the standard definitions of their names on purpose (OS-PTOOLS): the next case
    }
    vec::Digest dg;
    for (unsigned offset = 0; offset <= 14; ++offset) {  // 14 is past the largest: both clamp it to 13
      for (unsigned root = 0; root <= 62; ++root) {
        app::PadBoard board;
        board.set(static_cast<std::uint8_t>(scale), static_cast<std::uint8_t>(offset), static_cast<std::uint8_t>(root));
        for (unsigned pad = 0; pad < 48; ++pad) {
          const std::uint8_t note = board.note(pad);
          dg.add_bytes(&note, 1);
        }
        ++boards;
      }
    }
    CHECK_MESSAGE(dg.value() == vectors::kPadDigest[group], "scale " << scale);
    ++group;
  }
  CHECK(boards == 37u * 15u * 63u);
  CHECK(group == sizeof vectors::kPadDigest / sizeof vectors::kPadDigest[0]);
}

TEST_CASE("pad board: boards of the reference vectors written out in full") {
  for (const vectors::Board& b : vectors::kBoards) {
    app::PadBoard board;
    board.set(static_cast<std::uint8_t>(b.scale), static_cast<std::uint8_t>(b.offset), static_cast<std::uint8_t>(b.root));
    for (unsigned pad = 0; pad < 48; ++pad) {
      CHECK_MESSAGE(board.note(pad) == b.notes[pad], "scale " << b.scale << " offset " << b.offset << " root " << b.root << " pad " << pad);
    }
  }
}

TEST_CASE("pad board: the four corrected scales (OS-PTOOLS, PAR-073) put only their own notes on the pads, every root") {
  for (const unsigned scale : {11u, 17u, 23u, 36u}) {
    for (unsigned root = 0; root < 12; ++root) {
      app::PadBoard board;
      board.set(static_cast<std::uint8_t>(scale), 12, static_cast<std::uint8_t>(36 + root));
      for (unsigned pad = 0; pad < 48; ++pad) {
        if (board.note(pad) >= app::PadBoard::kMaxNote) {
          continue;  // the top pads repeat the highest note the board has (kMaxNote), in scale or not
        }
        CHECK_MESSAGE(core::is_in_scale(static_cast<std::uint8_t>(board.note(pad)), static_cast<std::uint8_t>(root), static_cast<std::uint8_t>(scale)), "scale " << scale << " root " << root << " pad " << pad);
      }
    }
  }
}

TEST_CASE("pad board: the default layout, a row of twelve a semitone apart, the top row the highest") {
  const app::PadBoard board;  // scale 0, offset 12, root 36
  CHECK((board.scale() == 0 && board.offset() == 12 && board.root() == 36));
  // The bottom row of pads (36..47) is the lowest octave from the root, each row continues the one below it.
  for (unsigned i = 0; i < 12; ++i) {
    CHECK(board.note(36 + i) == 36 + i);
    CHECK(board.note(24 + i) == 48 + i);
    CHECK(board.note(12 + i) == 60 + i);
    CHECK(board.note(i) == 72 + i);
  }
  CHECK(board.step_note(36) == 37);  // a step stores the note plus one: C-3 is byte 37
  CHECK(board.step_note(47) == 48);
  CHECK(board.step_note(48) == 0);   // not a pad
  CHECK(board.note(96) == 60);       // the preview key
  CHECK(board.note(48) == 0);
  CHECK(board.note(255) == 0);
  // The board never goes above note 120 (kMaxNote): from a high root on a sparse scale the top pads repeat it.
  app::PadBoard high;
  high.set(24, 12, 62);  // whole tone
  CHECK(high.note(0) == 120);
  unsigned top = 0;
  for (unsigned pad = 0; pad < 48; ++pad) {
    CHECK(high.note(pad) <= app::PadBoard::kMaxNote);
    top += high.note(pad) == 120 ? 1u : 0u;
  }
  CHECK(top >= 2u);
}

TEST_CASE("pad board: the rows of the offsets, and the pads that play a note") {
  app::PadBoard board;
  // Offset 0: every row starts on the note the row below starts on. 12: a row continues one past the end of the one below. 13: five up.
  board.set(0, 0, 36);
  CHECK((board.note(36) == 36 && board.note(24) == 36 && board.note(12) == 36 && board.note(0) == 36));
  board.set(0, 13, 36);
  CHECK((board.note(36) == 36 && board.note(24) == 41 && board.note(12) == 46 && board.note(0) == 51));
  board.set(0, 14, 36);  // above the largest offset is the largest
  CHECK(board.offset() == 13);
  // C major from C-3: the notes of the scale only, the root first.
  board.set(3, 12, 36);
  const std::uint8_t major[12] = {36, 38, 40, 41, 43, 45, 47, 48, 50, 52, 53, 55};
  for (unsigned i = 0; i < 12; ++i) {
    CHECK(board.note(36 + i) == major[i]);
  }
  // pads_with_note: the pads that play a note.
  board.set(0, 12, 36);
  std::uint8_t pads[48];
  CHECK(board.pads_with_note(60, pads) == 1u);
  CHECK(pads[12] == 1);  // note 60 is the first pad of the third row from the bottom
  unsigned lit = 0;
  for (const std::uint8_t p : pads) {
    lit += p;
  }
  CHECK(lit == 1u);
  CHECK(board.pads_with_note(10, pads) == 0u);
  CHECK(board.pads_with_note(-1, pads) == 0u);
}
