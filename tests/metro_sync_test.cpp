// SPDX-License-Identifier: GPL-3.0-or-later
// OS-METRO: the device settings METRO and METRO VOL carried to the engine and the sequencer (app/metro_sync.h), against the real settings block and a ParamChange ring: the settings file's values at
// the first pass, one set of changes per change of a setting's generation (a bound controller moves them too), the sequencer told only whether the metronome is on, and a full ring tried again.
#include <doctest/doctest.h>

#include <o3os/app/metro_sync.h>
#include <o3os/audio/graph_params.h>
#include <o3os/core/param.h>
#include <o3os/model/params.h>
#include <o3os/model/settings.h>
#include <o3os/seq/sequencer.h>

#include <cstdint>
#include <cstring>

namespace {

namespace ds = o3os::model::device_setting;
namespace gp = o3os::audio::graph_param;
using o3os::app::MetroSync;
using o3os::core::ParamChange;
using o3os::core::ParamRing;

o3os::model::Settings device() {
  o3os::model::Settings s;
  std::memset(&s, 0, sizeof s);
  s.init(o3os::core::ParamKind::Device);
  return s;
}

ParamRing& ring() {
  static ParamRing r;
  ParamChange c;
  while (r.pop(c)) {
  }
  return r;
}

const o3os::core::ParamId kMode = o3os::audio::master_param(gp::master::MetroMode);
const o3os::core::ParamId kLevel = o3os::audio::master_param(gp::master::MetroLevel);
const o3os::core::ParamId kSeq = o3os::core::param_id(o3os::core::ParamKind::Project, 0, o3os::seq::seq_param::Metro);

}  // namespace

TEST_CASE("metro sync: the first pass sends the settings file's values to the graph and tells the sequencer the metronome is on") {
  o3os::model::Settings d = device();
  ParamRing& r = ring();
  MetroSync sync;
  CHECK(sync.step(d, r));
  ParamChange c;
  CHECK(r.pop(c));
  CHECK(c.id == kMode);
  CHECK(c.value == 1.0f);  // Cue, the default
  CHECK(c.ramp_samples == 0u);
  CHECK(r.pop(c));
  CHECK(c.id == kLevel);
  CHECK(c.value == 50.0f);
  CHECK(r.pop(c));
  CHECK(c.id == kSeq);
  CHECK(c.value == 1.0f);
  CHECK_FALSE(r.pop(c));
  // Nothing changed: nothing is sent.
  CHECK(sync.step(d, r));
  CHECK_FALSE(r.pop(c));
}

TEST_CASE("metro sync: a change of METRO sends the mode and the sequencer's switch; a change of METRO VOL sends the level alone; the same value sends nothing") {
  o3os::model::Settings d = device();
  ParamRing& r = ring();
  MetroSync sync;
  CHECK(sync.step(d, r));
  ParamChange c;
  while (r.pop(c)) {
  }
  CHECK(d.set(ds::MetroMode, 2.0f) == o3os::model::Status::Ok);  // Main
  CHECK(sync.step(d, r));
  CHECK(r.pop(c));
  CHECK(c.id == kMode);
  CHECK(c.value == 2.0f);
  CHECK(r.pop(c));
  CHECK(c.id == kSeq);
  CHECK(c.value == 1.0f);
  CHECK_FALSE(r.pop(c));
  CHECK(d.set(ds::MetroMode, 0.0f) == o3os::model::Status::Ok);  // Off: the sequencer stops emitting
  CHECK(sync.step(d, r));
  CHECK(r.pop(c));
  CHECK(c.id == kMode);
  CHECK(c.value == 0.0f);
  CHECK(r.pop(c));
  CHECK(c.id == kSeq);
  CHECK(c.value == 0.0f);
  CHECK_FALSE(r.pop(c));
  CHECK(d.set(ds::MetronomeVolume, 80.0f) == o3os::model::Status::Ok);
  CHECK(sync.step(d, r));
  CHECK(r.pop(c));
  CHECK(c.id == kLevel);
  CHECK(c.value == 80.0f);
  CHECK_FALSE(r.pop(c));
  CHECK(d.set(ds::MetronomeVolume, 80.0f) == o3os::model::Status::Ok);  // the same value: no new generation
  CHECK(sync.step(d, r));
  CHECK_FALSE(r.pop(c));
  // COUNT-IN is not the engine's: it is read when Play is pressed.
  CHECK(d.set(ds::CountIn, 2.0f) == o3os::model::Status::Ok);
  CHECK(sync.step(d, r));
  CHECK_FALSE(r.pop(c));
}

TEST_CASE("metro sync: a ring with no room is tried again on a later pass, and what is owed is not lost") {
  o3os::model::Settings d = device();
  ParamRing& r = ring();
  // Fill the ring leaving room for one change.
  const ParamChange filler{o3os::core::param_id(o3os::core::ParamKind::Master, 0, 0), 1.0f, 0};
  while (r.push(filler)) {
  }
  ParamChange c;
  CHECK(r.pop(c));  // room for exactly one
  MetroSync sync;
  CHECK_FALSE(sync.step(d, r));  // the mode went, the level and the sequencer's switch are still owed
  CHECK_FALSE(sync.step(d, r));  // the ring is full again
  unsigned metro_changes = 0;
  while (r.pop(c)) {
    metro_changes += (c.id == kMode || c.id == kLevel || c.id == kSeq) ? 1u : 0u;
  }
  CHECK(metro_changes == 1u);
  CHECK(sync.step(d, r));  // the ring is empty: the rest goes
  bool level = false;
  bool seq = false;
  bool mode = false;
  while (r.pop(c)) {
    level = level || c.id == kLevel;
    seq = seq || c.id == kSeq;
    mode = mode || c.id == kMode;
  }
  CHECK(level);
  CHECK(seq);
  CHECK_FALSE(mode);  // already sent
  CHECK(sync.step(d, r));
  CHECK_FALSE(r.pop(c));
}
