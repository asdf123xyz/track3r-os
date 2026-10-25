// SPDX-License-Identifier: GPL-3.0-or-later
// OS-LFOSINE (docs/specs/OS-LFOSINE.md section 4, docs/formats/og.md): OG has five LFO shapes (0..4). Export writes the Sine shape (5) as Triangle (2), leaves every other byte of the file as it was, and
// counts it (ExportDrops::lfo_sine, one a sample instrument with a Sine LFO), which the loss line and the page of the export dialog name. Import holds a shape byte above 4 (never an OG value) to Triangle.
#include <doctest/doctest.h>

#include <o3os/app/og_survey.h>

#include "support/need.h"
#include "support/og_fixture.h"

#include <o3os/formats/og.h>
#include <o3os/model/instrument.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <cstring>
#include <string>

using namespace o3os;
using namespace o3os::test::og;
using formats::Progress;
using o3os::test::MemorySource;

namespace {

constexpr unsigned kLfoAt = 212;  // formats/src/og_codec.h instr::kLfo
constexpr unsigned kLfoStride = 8;

model::Project& fresh() {
  model::Project& p = arena(0).project;
  p.reset();
  return p;
}

// A .pti version 9 head imported into slot 0 of a fresh project, its six LFO shape bytes patched to `shapes`.
model::Project& import_with_shapes(const std::uint8_t (&shapes)[6]) {
  model::Project& proj = fresh();
  Bytes f = instrument_bytes({});
  for (unsigned i = 0; i < 6; ++i) {
    f[kLfoAt + i * kLfoStride] = shapes[i];
  }
  p32(f, 388, formats::crc32(f.data(), 388));
  formats::og::InstrumentReader reader(true);
  formats::ReadParams params;
  params.index = 0;
  CHECK(run_reader(reader, f, arena(0), params) == Progress::Done);
  return proj;
}

Progress export_slot0(const model::Project& p, Bytes& out, formats::og::ExportDrops* drops) {
  static formats::og::InstrumentWriter w;
  formats::WriteParams params;
  params.index = 0;
  const Progress r = run_writer(w, p, out, params);
  if (drops != nullptr) {
    *drops = w.export_report().drops;
  }
  return r;
}

}  // namespace

TEST_CASE("og lfosine: Export writes Sine as Triangle, keeps every other shape and byte, and counts the instrument once") {
  fresh();
  model::Project& proj = import_with_shapes({0, 1, 2, 3, 4, 0});
  model::Instrument in = *proj.instrument(0);
  in.sampler.lfo[1].shape = model::kLfoShapeSine;
  in.sampler.lfo[3].shape = model::kLfoShapeSine;
  in.sampler.lfo[5].shape = model::kLfoShapeSine;
  Bytes plain;
  formats::og::ExportDrops none;
  {
    model::Project& p2 = import_with_shapes({0, 1, 2, 3, 4, 0});
    NEED(export_slot0(p2, plain, &none) == Progress::Done);
    CHECK(none.lfo_sine == 0u);
    CHECK_FALSE(none.any_lost());
  }
  model::Project& p3 = import_with_shapes({0, 1, 2, 3, 4, 0});
  NEED(p3.set_instrument(0, in) == model::Status::Ok);
  Bytes out;
  formats::og::ExportDrops d;
  NEED(export_slot0(p3, out, &d) == Progress::Done);
  NEED(out.size() == plain.size());
  CHECK(out[kLfoAt + 1 * kLfoStride] == 2);  // Sine written as Triangle
  CHECK(out[kLfoAt + 3 * kLfoStride] == 2);
  CHECK(out[kLfoAt + 5 * kLfoStride] == 2);
  CHECK(out[kLfoAt + 0 * kLfoStride] == 0);  // the others as they were
  CHECK(out[kLfoAt + 2 * kLfoStride] == 2);
  CHECK(out[kLfoAt + 4 * kLfoStride] == 4);
  CHECK(d.lfo_sine == 1u);  // the instrument, not the LFOs
  CHECK(d.any_lost());
  // Against the file the same instrument writes with its shapes untouched, only those three bytes differ (and the CRC at 388, which covers them).
  unsigned differing = 0;
  for (std::size_t i = 0; i < 388; ++i) {
    differing += out[i] != plain[i] ? 1u : 0u;
  }
  CHECK(differing == 3u);
}

TEST_CASE("og lfosine: the notice names it: a kind of the loss items and the loss line") {
  formats::og::ExportDrops d;
  char line[48];
  CHECK_FALSE(app::og_loss_line(d, line, sizeof line));
  d.lfo_sine = 2;
  CHECK(d.any_lost());
  app::OgLossItem items[app::kOgLossMax];
  const unsigned n = app::og_loss_items(d, items);
  NEED(n == 1u);
  CHECK(items[0].n == 2u);
  CHECK(std::string(items[0].brief) == "SINE LFO");
  CHECK(std::string(items[0].full) == "SINE LFO SHAPES WRITTEN AS TRIANGLE");
  CHECK(app::og_loss_line(d, line, sizeof line));
  CHECK(std::string(line) == "2 SINE LFO");
  formats::og::ExportDrops sum;
  sum.add(d);
  sum.add(d);
  CHECK(sum.lfo_sine == 4u);
}

TEST_CASE("og lfosine: Import holds a shape byte above 4 to Triangle (OG has five shapes; 5 is this OS's Sine)") {
  model::Project& proj = import_with_shapes({0, 4, 5, 6, 255, 3});
  NEED(proj.instrument(0) != nullptr);
  const std::uint8_t want[6] = {0, 4, 2, 2, 2, 3};
  for (unsigned i = 0; i < 6; ++i) {
    CHECK(proj.instrument(0)->sampler.lfo[i].shape == want[i]);
  }
}
