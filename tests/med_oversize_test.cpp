// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "support/mmd_fixture.h"
#include "support/need.h"

#include <o3os/formats/med/flow.h>
#include <o3os/formats/med/mmd.h>

#include <cstdint>

using namespace o3os;
using namespace o3os::formats::med;
using o3os::formats::Progress;

// Files too big for the QEMU target (4 MiB of RAM), so host only: the sample-arena budget of docs/formats/med.md 2.6 item 6 on a
// file that really holds a sample of that size, and the block that is built to make the flow simulation slow.

TEST_CASE("med oversize: a sample at the arena budget is accepted, one frame more is flagged and its number reported") {
  test::mmd::ModuleSpec big = test::mmd::sample_module(1);
  big.instruments.resize(1);
  big.instruments[0].type = 0;
  big.instruments[0].data.assign(static_cast<std::size_t>(caps::kSampleArenaBytes / 2) + 1, 0x01);  // 16-bit PCM in the arena
  big.s_ext_entrsz = 0;
  big.i_ext_entrsz = 0;
  big.songname.clear();
  big.annotation.clear();
  {
    const test::mmd::Bytes bytes = test::mmd::build(big);
    SpanSource src(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
    Reader r;
    NEED(r.open(src).ok());
    Summary s;
    NEED(r.validate(s).ok());
    CHECK(s.oversize_mask == 1);
    InstrumentInfo i;
    NEED(r.instrument(0, i).ok());
    CHECK((i.notes & kNoteOversize) != 0);
    CHECK(i.pcm_bytes == (caps::kSampleArenaBytes / 2 + 1) * 2);
    CHECK(s.pcm_bytes > caps::kSampleArenaBytes);
  }
  big.instruments[0].data.pop_back();
  {
    const test::mmd::Bytes bytes = test::mmd::build(big);
    SpanSource src(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
    Reader r;
    NEED(r.open(src).ok());
    Summary s;
    NEED(r.validate(s).ok());
    CHECK(s.oversize_mask == 0);
    CHECK(s.pcm_bytes == caps::kSampleArenaBytes);
  }
}

// The reviewer's case: 64 tracks, 9,999 lines and 16 command pages that all name the same data, so the file is 3.8 MB but a
// line costs 64 x 17 = 1,088 cells and 17 reads. Weighted by cells the work is capped after 1,927 lines (2^21 / 1,088), where
// counting lines alone would let it run to the 32,640 line cap (35 million cells).
TEST_CASE("med flow: 64 tracks, 9,999 lines and 16 shared pages are refused within the cell budget") {
  test::mmd::ModuleSpec m;
  m.version = 1;
  m.deftempo = 33;
  m.tempo2 = 6;
  test::mmd::BlockSpec b;
  b.tracks = 64;
  b.lines = 9999;
  b.share_pages = true;
  b.pages.resize(16);  // sixteen pages of empty cells, all at the data of the first
  m.blocks = {b};
  m.playseq = {0};
  const test::mmd::Bytes bytes = test::mmd::build(m);
  SpanSource src(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  Reader r;
  NEED(r.open(src).ok());
  Summary sum;
  NEED(r.validate(sum).ok());
  CHECK(sum.pages_max == 16);
  CHECK(sum.max_lines == 9999);

  static LinearSong out;
  FlowOptions opt;
  const Status s = linearise(r, opt, out);
  CHECK(s.error == Error::LimitExceeded);
  CHECK(out.report.cells > opt.max_cells);
  CHECK(out.report.cells <= opt.max_cells + 64u * 17u);
  CHECK(out.report.visits == 1928);  // the line that crosses 2^21 cells at 1,088 a line; the line cap would allow 32,640
  CHECK(out.report.visits < flow_caps::kMaxVisits);
  // through a job in small steps it stops at the same line, each step a bounded piece of work
  FlowJob job;
  job.begin(r, opt, out, false);
  unsigned steps = 0;
  while (job.step(10000) == Progress::Running) ++steps;
  CHECK(job.progress() == Progress::Failed);
  CHECK(job.status().error == Error::LimitExceeded);
  CHECK(out.report.visits == 1928);
  CHECK(steps > 100);
  CHECK(steps < 400);
  // the structural fallback counts the same cells while it scans the block, and refuses it too
  const Status st = structural(r, opt, out);
  CHECK(st.error == Error::LimitExceeded);
  CHECK(out.report.cells <= opt.max_cells + 64u * 17u);
  // the same block without the extra pages is fine: 64 x 9,999 cells, 79 pieces of at most 128 lines
  m.blocks[0].pages.clear();
  m.blocks[0].share_pages = false;
  const test::mmd::Bytes sb = test::mmd::build(m);
  SpanSource ss(sb.data(), static_cast<std::uint32_t>(sb.size()));
  Reader sr;
  NEED(sr.open(ss).ok());
  CHECK(linearise(sr, opt, out).ok());
  CHECK(out.report.cells == 64u * 9999u);
  CHECK(out.pattern_count == 79);
}
