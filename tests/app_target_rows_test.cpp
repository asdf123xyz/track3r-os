// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-APPUX S5 (docs/specs/OS-APPUX.md 3.7): the target rows every tool app shares (modules/apps/common/o3m_edit.h: read_target, edit_target, fill_target_row, step_instrument), against a fake read table.
// The apps' own pages are in tests/app_grammar_test.cpp (through the simulator); this is the helper's rules: what the launch gives, what each row may reach, what keeps the others inside the pattern,
// what Delete puts back, and the rows with no launch context.
#include <doctest/doctest.h>

#include "../modules/apps/common/o3m_edit.h"

#include <cstring>
#include <string>

namespace {

using namespace o3edit;

// Pattern 1: four tracks of 16, 16, 32 and 16 rows. Pattern 2: two tracks of 8 rows. Any other pattern does not exist.
o3m_launch_ctx g_ctx;
uint32_t g_from_tools = 1;

uint32_t fake_launch(o3m_launch_ctx* out) {
  *out = g_ctx;
  return g_from_tools;
}
int32_t fake_pattern_info(uint16_t pattern, o3m_pattern_info* out) {
  if (pattern != 1u && pattern != 2u) {
    return O3M_EDIT_RANGE;
  }
  std::memset(out->track_length, 0, sizeof out->track_length);
  if (pattern == 1u) {
    out->tracks = 4;
    out->length = 32;
    out->track_length[0] = out->track_length[1] = out->track_length[3] = 16;
    out->track_length[2] = 32;
  } else {
    out->tracks = 2;
    out->length = 8;
    out->track_length[0] = out->track_length[1] = 8;
  }
  return O3M_EDIT_OK;
}

o3m_read_api make_read() {
  o3m_read_api r;
  std::memset(&r, 0, sizeof r);
  r.version = O3M_READ_API_VERSION;
  r.size = sizeof r;
  r.launch_ctx = fake_launch;
  r.pattern_info = fake_pattern_info;
  return r;
}
o3m_read_api g_read = make_read();

void launch(uint32_t pattern, uint32_t track, uint32_t row, uint32_t block_len = 0, uint32_t block_tracks = 0, uint32_t instrument = 4) {
  std::memset(&g_ctx, 0, sizeof g_ctx);
  g_ctx.size = sizeof g_ctx;
  g_ctx.flags = O3M_LAUNCH_FROM_TOOLS;
  g_ctx.pattern = pattern;
  g_ctx.track = track;
  g_ctx.block_start = row;
  g_ctx.block_length = block_len;
  g_ctx.block_tracks = static_cast<uint8_t>(block_tracks);
  g_ctx.instrument = instrument;
  g_from_tools = 1;
}

std::string value_of(const Target& t, uint32_t which) {
  PageRow r;
  fill_target_row(&r, &t, which);
  return r.value;
}

}  // namespace

TEST_CASE("target rows: the launch is the Pattern cursor, the rows read it 1-based and Delete's values are the launch's") {
  launch(1, 2, 5);  // track 3, row 6 on the screen, no block
  Target t{};
  CHECK(read_target(&g_read, &t, 16));
  CHECK((t.pattern == 1u && t.first_track == 2u && t.first_row == 5u && t.tracks == 1u && t.rows == 16u && t.instrument == 5u));
  CHECK((t.home_pattern == 1u && t.home_track == 2u && t.home_row == 5u && t.home_rows == 16u && t.home_tracks == 1u));
  CHECK(value_of(t, kTrPattern) == "1");
  CHECK(value_of(t, kTrTrack) == "3");
  CHECK(value_of(t, kTrRow) == "6");
  CHECK(value_of(t, kTrRows) == "16");
  CHECK(value_of(t, kTrTracks) == "1");
  CHECK(track_rows(&t, 0) == 16u);  // the 32-row track holds all 16 from row 6

  launch(1, 0, 4, 20, 3);  // a block of 3 tracks x 20 rows from row 5
  CHECK(read_target(&g_read, &t, 16));
  CHECK((t.has_block == 1u && t.tracks == 3u && t.rows == 20u && t.home_tracks == 3u && t.home_rows == 20u));
  CHECK(track_rows(&t, 0) == 12u);  // the first track is 16 rows long: rows 5..16
}

TEST_CASE("target rows: the end of the track is the default rows when the app asks for 0; a missing pattern keeps what the context gave") {
  launch(1, 2, 20);
  Target t{};
  CHECK(read_target(&g_read, &t, 0));
  CHECK(t.rows == 12u);  // 32 - 20
  launch(9, 1, 3, 8, 2);  // pattern 9 does not exist, but the block is the context's
  CHECK_FALSE(read_target(&g_read, &t, 16));
  CHECK((t.from_tools == 1u && t.have_pattern == 0u && t.rows == 8u && t.tracks == 2u && t.home_rows == 8u));
  launch(9, 1, 3);
  CHECK_FALSE(read_target(&g_read, &t, 16));
  CHECK(t.rows == 16u);
  CHECK(value_of(t, kTrPattern) == "9 (EMPTY)");
}

TEST_CASE("target rows: each row moves inside the pattern and stops at its ends") {
  launch(1, 0, 0);
  Target t{};
  (void)read_target(&g_read, &t, 16);
  edit_target(&g_read, &t, kTrTrack, 100, false);
  CHECK(t.first_track == 3u);  // four tracks
  edit_target(&g_read, &t, kTrTrack, -100, false);
  CHECK(t.first_track == 0u);
  edit_target(&g_read, &t, kTrTrack, 2, false);  // the 32-row track
  edit_target(&g_read, &t, kTrRow, 1000, false);
  CHECK(t.first_row == 31u);
  edit_target(&g_read, &t, kTrRow, -1000, false);
  CHECK(t.first_row == 0u);
  edit_target(&g_read, &t, kTrRows, 1000, false);
  CHECK(t.rows == 128u);
  edit_target(&g_read, &t, kTrRows, -1000, false);
  CHECK(t.rows == 1u);
  edit_target(&g_read, &t, kTrTracks, 100, false);
  CHECK(t.tracks == 4u);
  edit_target(&g_read, &t, kTrTracks, -100, false);
  CHECK(t.tracks == 1u);
  edit_target(&g_read, &t, kTrPattern, -100, false);
  CHECK(t.pattern == 1u);
  edit_target(&g_read, &t, kTrPattern, 1000, false);
  CHECK((t.pattern == 255u && t.have_pattern == 0u));
}

TEST_CASE("target rows: another pattern or track keeps the rest inside it; Delete puts back one row's launch value") {
  launch(1, 3, 12);  // track 4 of pattern 1, row 13
  Target t{};
  (void)read_target(&g_read, &t, 16);
  edit_target(&g_read, &t, kTrPattern, 1, false);  // pattern 2: two tracks of 8 rows
  CHECK((t.pattern == 2u && t.have_pattern == 1u));
  CHECK(t.first_track == 1u);  // pulled in
  CHECK(t.first_row == 7u);
  CHECK(track_rows(&t, 0) == 1u);
  edit_target(&g_read, &t, kTrPattern, 1, false);  // pattern 3 does not exist: reads (EMPTY), the rest stays
  CHECK((t.pattern == 3u && t.have_pattern == 0u));
  CHECK(value_of(t, kTrPattern) == "3 (EMPTY)");
  CHECK((t.first_track == 1u && t.first_row == 7u));
  CHECK(track_rows(&t, 0) == 0u);  // nothing is known of it
  edit_target(&g_read, &t, kTrPattern, 0, true);
  CHECK((t.pattern == 1u && t.have_pattern == 1u && t.info.tracks == 4u));
  edit_target(&g_read, &t, kTrTrack, 0, true);
  CHECK(t.first_track == 3u);
  edit_target(&g_read, &t, kTrRow, 0, true);
  CHECK(t.first_row == 12u);  // (track 4 of pattern 1 has 16 rows)
  edit_target(&g_read, &t, kTrRows, 5, false);
  CHECK(t.rows == 21u);
  edit_target(&g_read, &t, kTrRows, 0, true);
  CHECK(t.rows == 16u);
}

TEST_CASE("target rows: a launch with no block on a pattern that does not exist asks for the end of the track; it is resolved when PATTERN reaches one that exists, and Delete on ROWS gives it back") {
  launch(9, 2, 3);  // pattern 9 does not exist: Chords (rows 0) reads ROWS 0
  Target t{};
  CHECK_FALSE(read_target(&g_read, &t, 0));
  CHECK((t.rows == 0u && t.home_rows == 0u));
  CHECK(value_of(t, kTrRows) == "0");
  edit_target(&g_read, &t, kTrRows, 0, true);  // (Delete before the pattern is known keeps the launch: still to the end)
  CHECK(t.rows == 0u);
  edit_target(&g_read, &t, kTrPattern, -8, false);  // pattern 1: track 3 has 32 rows, the cursor row is 4
  CHECK((t.pattern == 1u && t.have_pattern == 1u));
  CHECK(t.rows == 29u);
  CHECK(value_of(t, kTrRows) == "29");
  CHECK(track_rows(&t, 0) == 29u);
  edit_target(&g_read, &t, kTrRows, -24, false);
  CHECK(t.rows == 5u);
  edit_target(&g_read, &t, kTrRows, 0, true);
  CHECK(t.rows == 29u);  // the launch's "to the end of the track", not 1
  // an app that asks for a number (Rhythm Gen's 16) is not touched by any of this
  launch(9, 2, 3);
  CHECK_FALSE(read_target(&g_read, &t, 16));
  edit_target(&g_read, &t, kTrPattern, -8, false);
  CHECK((t.rows == 16u && t.home_rows == 16u));
}

TEST_CASE("target rows: a block that starts past its first track's end keeps its start when ROWS or TRACKS move; moving TRACK or ROW is the user's own move and pulls it in") {
  launch(1, 0, 20, 8, 3);  // track 1 has 16 rows, track 3 has 32: rows 21-28 exist on track 3 only
  Target t{};
  CHECK(read_target(&g_read, &t, 16));
  CHECK((track_rows(&t, 0) == 0u && track_rows(&t, 2) == 8u));
  edit_target(&g_read, &t, kTrRows, 4, false);
  CHECK((t.first_row == 20u && t.rows == 12u && track_rows(&t, 2) == 12u));
  edit_target(&g_read, &t, kTrTracks, -1, false);
  CHECK((t.first_row == 20u && t.tracks == 2u));
  edit_target(&g_read, &t, kTrTracks, 1, false);
  CHECK(t.first_row == 20u);
  edit_target(&g_read, &t, kTrTrack, 1, false);  // track 2 also has 16 rows: the start is pulled in
  CHECK(t.first_row == 15u);
}

TEST_CASE("target rows: a launch with no context shows dim dashes and nothing moves; a block of a missing pattern still resets") {
  Target t{};
  g_from_tools = 0;
  std::memset(&g_ctx, 0, sizeof g_ctx);
  CHECK_FALSE(read_target(&g_read, &t, 16));
  for (uint32_t which = 0; which < kTargetRows; ++which) {
    PageRow r;
    fill_target_row(&r, &t, which);
    CHECK(std::string(r.value) == "-");
    CHECK(r.dim);
    const Target before = t;
    edit_target(&g_read, &t, which, 3, false);
    edit_target(&g_read, &t, which, 0, true);
    CHECK(std::memcmp(&before, &t, sizeof t) == 0);
  }
  g_from_tools = 1;
}

TEST_CASE("target rows: the instrument row runs 1..64 with the launch instrument as its default") {
  CHECK(step_instrument(5, 9, 1, false) == 6u);
  CHECK(step_instrument(5, 9, -10, false) == 1u);
  CHECK(step_instrument(5, 9, 1000, false) == 64u);
  CHECK(step_instrument(5, 9, 0, true) == 9u);
  CHECK(step_instrument(5, 0, 0, true) == 1u);  // (a home of 0 is no launch: clamped, and the apps never edit then)
}
