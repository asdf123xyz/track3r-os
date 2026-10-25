// SPDX-License-Identifier: GPL-3.0-or-later
// PAT-OPS-A: DUPLICATE, EXPAND and SHRINK as pure functions over one track's column (model/pattern_ops.h): the doubling, the spread, the halving, the odd lengths, the refusal over 128 steps, what
// is left alone past the lengths, and that nothing is changed in the input.
#include "support/need.h"

#include <o3os/model/pattern_ops.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>

using namespace o3os;
using namespace o3os::model;

namespace {

// A column whose step i holds note i + 1 (1..128), instrument 1 and volume 100, so that every step differs from its neighbours and from an empty one.
void fill_column(Step* col, unsigned from = 0, unsigned to = kMaxPatternLength) {
  for (unsigned i = 0; i < kMaxPatternLength; ++i) {
    col[i] = kEmptyStep;
  }
  for (unsigned i = from; i < to; ++i) {
    col[i].note = static_cast<std::uint8_t>(i + 1u);
    col[i].instr = 1;
    col[i].vol = 100;
    col[i].fx[0] = {static_cast<std::uint8_t>(1 + i % 40u), static_cast<std::uint8_t>(i)};
  }
}

bool same(const Step& a, const Step& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

}  // namespace

TEST_CASE("pattern ops: the lengths") {
  CHECK(ops::result_length(ops::Op::Duplicate, 16) == 32);
  CHECK(ops::result_length(ops::Op::Expand, 16) == 32);
  CHECK(ops::result_length(ops::Op::Shrink, 16) == 8);
  CHECK(ops::result_length(ops::Op::Shrink, 15) == 8);  // an odd length rounds up
  CHECK(ops::result_length(ops::Op::Shrink, 1) == 1);
  CHECK(ops::result_length(ops::Op::Shrink, 128) == 64);
  CHECK(ops::result_length(ops::Op::Duplicate, 64) == 128);
  CHECK(ops::result_length(ops::Op::Expand, 65) == 130);  // (the unchecked result: apply refuses it)
}

TEST_CASE("pattern ops: DUPLICATE doubles the length and repeats the steps") {
  Step in[kMaxPatternLength];
  Step out[kMaxPatternLength];
  fill_column(in, 0, 16);
  unsigned n = 0;
  NEED(ops::apply(ops::Op::Duplicate, in, 16, out, n));
  CHECK(n == 32);
  for (unsigned i = 0; i < 16; ++i) {
    CHECK(same(out[i], in[i]));
    CHECK(same(out[16 + i], in[i]));
  }
  for (unsigned i = 32; i < kMaxPatternLength; ++i) {
    CHECK(same(out[i], in[i]));  // past both lengths: as it was
  }
}

TEST_CASE("pattern ops: DUPLICATE overwrites the kept steps past the length, but not past the new one") {
  Step in[kMaxPatternLength];
  Step out[kMaxPatternLength];
  fill_column(in);  // all 128 steps hold something; the track is 8 long, so 8..127 are the kept ones
  unsigned n = 0;
  NEED(ops::apply(ops::Op::Duplicate, in, 8, out, n));
  CHECK(n == 16);
  for (unsigned i = 0; i < 8; ++i) {
    CHECK(same(out[8 + i], in[i]));
  }
  for (unsigned i = 16; i < kMaxPatternLength; ++i) {
    CHECK(same(out[i], in[i]));
  }
}

TEST_CASE("pattern ops: EXPAND puts step i at 2i with an empty step after it") {
  Step in[kMaxPatternLength];
  Step out[kMaxPatternLength];
  fill_column(in, 0, 16);
  unsigned n = 0;
  NEED(ops::apply(ops::Op::Expand, in, 16, out, n));
  CHECK(n == 32);
  for (unsigned i = 0; i < 16; ++i) {
    CHECK(same(out[2 * i], in[i]));
    CHECK(is_empty(out[2 * i + 1]));
  }
  for (unsigned i = 32; i < kMaxPatternLength; ++i) {
    CHECK(same(out[i], in[i]));
  }
}

TEST_CASE("pattern ops: SHRINK keeps the even steps and clears down to the old length") {
  Step in[kMaxPatternLength];
  Step out[kMaxPatternLength];
  fill_column(in);
  unsigned n = 0;
  NEED(ops::apply(ops::Op::Shrink, in, 32, out, n));
  CHECK(n == 16);
  for (unsigned i = 0; i < 16; ++i) {
    CHECK(same(out[i], in[2 * i]));
  }
  for (unsigned i = 16; i < 32; ++i) {
    CHECK(is_empty(out[i]));  // the new length up to the old one
  }
  for (unsigned i = 32; i < kMaxPatternLength; ++i) {
    CHECK(same(out[i], in[i]));  // past both lengths: as it was
  }
}

TEST_CASE("pattern ops: an odd length shrinks to ceil(L/2) and keeps the last step") {
  Step in[kMaxPatternLength];
  Step out[kMaxPatternLength];
  fill_column(in, 0, 7);
  unsigned n = 0;
  NEED(ops::apply(ops::Op::Shrink, in, 7, out, n));
  CHECK(n == 4);
  CHECK(same(out[0], in[0]));
  CHECK(same(out[1], in[2]));
  CHECK(same(out[2], in[4]));
  CHECK(same(out[3], in[6]));  // (the odd length's last step is an even one)
  for (unsigned i = 4; i < 7; ++i) {
    CHECK(is_empty(out[i]));
  }
  fill_column(in, 0, 1);
  NEED(ops::apply(ops::Op::Shrink, in, 1, out, n));
  CHECK(n == 1);  // one step stays one step
  CHECK(same(out[0], in[0]));
}

TEST_CASE("pattern ops: shrink(expand(p)) is p, for every length that expands") {
  for (unsigned len = 1; len <= 64; ++len) {
    Step p[kMaxPatternLength];
    Step x[kMaxPatternLength];
    Step back[kMaxPatternLength];
    fill_column(p, 0, len);
    unsigned n = 0;
    NEED(ops::apply(ops::Op::Expand, p, len, x, n));
    CHECK(n == 2 * len);
    unsigned m = 0;
    NEED(ops::apply(ops::Op::Shrink, x, n, back, m));
    CHECK(m == len);
    for (unsigned i = 0; i < len; ++i) {
      CHECK(same(back[i], p[i]));
    }
  }
}

TEST_CASE("pattern ops: 65..128 steps are refused for DUPLICATE and EXPAND, and nothing is written") {
  Step in[kMaxPatternLength];
  Step out[kMaxPatternLength];
  fill_column(in);
  for (unsigned len : {65u, 100u, 127u, 128u}) {
    for (const ops::Op op : {ops::Op::Duplicate, ops::Op::Expand}) {
      std::memset(out, 0x5A, sizeof out);
      unsigned n = 77;
      CHECK_FALSE(ops::apply(op, in, len, out, n));
      CHECK(n == 77);
      CHECK(static_cast<std::uint8_t>(out[0].note) == 0x5A);
      CHECK(static_cast<std::uint8_t>(out[127].fx[1].value) == 0x5A);
    }
  }
  unsigned n = 0;
  CHECK(ops::apply(ops::Op::Duplicate, in, 64, out, n));  // 64 doubles to exactly 128
  CHECK(n == 128);
  CHECK(ops::apply(ops::Op::Expand, in, 64, out, n));
  CHECK(n == 128);
  CHECK(ops::apply(ops::Op::Shrink, in, 128, out, n));  // SHRINK is never refused
  CHECK(n == 64);
}

TEST_CASE("pattern ops: a length outside 1..128 is refused") {
  Step in[kMaxPatternLength];
  Step out[kMaxPatternLength];
  fill_column(in);
  unsigned n = 0;
  for (const ops::Op op : {ops::Op::Duplicate, ops::Op::Expand, ops::Op::Shrink}) {
    CHECK_FALSE(ops::apply(op, in, 0, out, n));
    CHECK_FALSE(ops::apply(op, in, 129, out, n));
  }
}

TEST_CASE("pattern ops: the input is not changed, and each track goes by its own length") {
  Step in[kMaxPatternLength];
  Step before[kMaxPatternLength];
  Step out[kMaxPatternLength];
  fill_column(in);
  std::memcpy(before, in, sizeof in);
  unsigned n = 0;
  for (const ops::Op op : {ops::Op::Duplicate, ops::Op::Expand, ops::Op::Shrink}) {
    NEED(ops::apply(op, in, 24, out, n));
    CHECK(std::memcmp(before, in, sizeof in) == 0);
  }
  // Two tracks of one pattern, 16 and 48 steps long: the lengths of the results are 32 and 96 (DUPLICATE), 8 and 24 (SHRINK).
  unsigned a = 0;
  unsigned b = 0;
  NEED(ops::apply(ops::Op::Duplicate, in, 16, out, a));
  NEED(ops::apply(ops::Op::Duplicate, in, 48, out, b));
  CHECK(a == 32);
  CHECK(b == 96);
  NEED(ops::apply(ops::Op::Shrink, in, 16, out, a));
  NEED(ops::apply(ops::Op::Shrink, in, 48, out, b));
  CHECK(a == 8);
  CHECK(b == 24);
}
