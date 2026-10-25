// SPDX-License-Identifier: GPL-3.0-or-later
// The gallery and budget states of OS-002d: the generated parameter page (Instrument PARAMS) on every page, with the Learn overlay over it,
// without a project, and the budgeted sweep. Drawn through the app's scene (the registry and the overlay slot), so what is measured and
// looked at is what the device draws.
#include "screens_app.h"

#include <o3os/app/model.h>
#include <o3os/app/screens/scene.h>
#include <o3os/app/screens/screen.h>
#include <o3os/app/view_state.h>
#include <o3os/model/params.h>
#include <o3os/sim/demo_project.h>

namespace o3os::sim {

namespace {

using app::Model;
using app::OverlayId;
using app::ScreenId;
using app::ViewState;
namespace ip = model::instrument_param;

// The project of a state: a zeroed static (it is 1.9 MB, like the device's arena in SDRAM), refilled by every state that draws it.
model::Project& project() {
  static model::ModelArena arena;
  return arena.project;
}

Model make_model(const model::Project* p) {
  Model m{};
  m.project = p;
  m.device.init(core::ParamKind::Device);
  return m;
}

ViewState make_view(unsigned group, unsigned env, int instrument, int row, OverlayId overlay) {
  ViewState v;
  v.screen = ScreenId::Instrument;
  v.page[app::screen_index(ScreenId::Instrument)] = static_cast<std::uint8_t>(group);
  v.params.env = static_cast<int>(env);
  v.params.row = row;
  v.instrument = instrument;
  v.overlay = overlay;
  return v;
}

void draw(ui::Renderer& r, const model::Project* p, const ViewState& v) {
  const Model m = make_model(p);
  app::render_scene(m, v, r);
}

// ---- the typical pages ----------------------------------------------------------------------------------------------------

void draw_voice(ui::Renderer& r, unsigned) {
  fill_demo_project(project());
  draw(r, &project(), make_view(ip::GroupVoice, 0, 1, 1, OverlayId::None));
}

// ACID (instrument 4): a low-pass with some resonance, the cursor on the resonance.
void draw_filter(ui::Renderer& r, unsigned) {
  fill_demo_project(project());
  draw(r, &project(), make_view(ip::GroupFilter, 0, 4, 3, OverlayId::None));
}

void draw_envelope(ui::Renderer& r, unsigned) {
  fill_demo_project(project());
  draw(r, &project(), make_view(ip::GroupEnvelope, 0, 1, 2, OverlayId::None));
}

// The cutoff's envelope: seconds-long times.
void draw_envelope_cutoff(ui::Renderer& r, unsigned) {
  fill_demo_project(project());
  draw(r, &project(), make_view(ip::GroupEnvelope, 2, 4, 2, OverlayId::None));
}

void draw_sends(ui::Renderer& r, unsigned) {
  fill_demo_project(project());
  draw(r, &project(), make_view(ip::GroupSends, 0, 3, 3, OverlayId::None));
}

// Learn over the filter page: the cutoff of instrument 1 under the cursor, bound to CC 74.
void draw_learn(ui::Renderer& r, unsigned) {
  fill_demo_project_with_bindings(project());
  draw(r, &project(), make_view(ip::GroupFilter, 0, 1, 2, OverlayId::Learn));
}

// No project: the defaults, and the info line says so.
void draw_no_project(ui::Renderer& r, unsigned) { draw(r, nullptr, make_view(ip::GroupVoice, 0, 1, 0, OverlayId::None)); }

// ---- the budgeted sweep ---------------------------------------------------------------------------------------------------
//
// Every page (the ENV page for each of its six envelopes), every row under the cursor, three contents of the instrument (the demo's, every
// parameter at its largest and at its smallest, which are the widest and the narrowest texts), with and without a controller bound to
// every row of the page (the CCs are three digits), and with and without the Learn overlay over it.
constexpr unsigned kContents = 3;
constexpr unsigned kPageEnvs = 3 + ip::kEnvelopes;  // voice, filter, sends once; the ENV page once for each envelope
constexpr unsigned kMaxRows = 6;
constexpr unsigned kSweep = kPageEnvs * kMaxRows * kContents * 2 * 2;

void draw_worst(ui::Renderer& r, unsigned variant) {
  unsigned v = variant;
  const bool overlay = v % 2 != 0;
  v /= 2;
  const bool bound = v % 2 != 0;
  v /= 2;
  const unsigned content = v % kContents;
  v /= kContents;
  const unsigned row = v % kMaxRows;
  v /= kMaxRows;
  const unsigned page_env = v % kPageEnvs;
  const unsigned group = page_env == 0 ? ip::GroupVoice : (page_env == 1 ? ip::GroupFilter : (page_env == 2 ? ip::GroupSends : ip::GroupEnvelope));
  const unsigned env = page_env >= 3 ? page_env - 3 : 0;

  model::Project& p = project();
  fill_demo_project(p);
  std::uint16_t fields[8];
  const unsigned n = model::param_group_fields(core::ParamKind::Instrument, group, env, fields, 8);
  for (unsigned i = 0; i < n; ++i) {
    const core::ParamId id = core::param_id(core::ParamKind::Instrument, 0, fields[i]);
    const core::ParamDesc* d = model::param_desc(id);
    if (d != nullptr && content != 0) {
      (void)p.set_param(id, content == 1 ? d->max : d->min);
    }
    if (bound && i < core::kLearnSlots) {
      core::LearnBinding b{id, static_cast<std::uint8_t>(100 + i), 16, static_cast<std::uint8_t>(core::LearnMode::Relative), 0};
      (void)p.set_learn_slot(i, b);
    }
  }
  draw(r, &p, make_view(group, env, 1, static_cast<int>(row < n ? row : 0), overlay ? OverlayId::Learn : OverlayId::None));
}

// OS-LFO-GRAPH: the MOD page's LFO picture over a 16-step window, every shape (Rev Saw .. Sine) at five speeds (128 steps: an eighth of a cycle; 16 steps: one; 4 steps: four; 1/2 step: 32, the most
// cycles drawn; 1/16 step: the band) on the amp slot (24 rates, from the sixth of the 29) and on the cutoff's (29), amount 100 %: the sine at 32 cycles is the picture with the most vertices.
constexpr unsigned kLfoShapes = 6;
constexpr unsigned kLfoSpeedsShown = 5;
constexpr unsigned kLfoSweep = kLfoShapes * kLfoSpeedsShown * 2;

void draw_lfo_worst(ui::Renderer& r, unsigned variant) {
  static constexpr unsigned kRateIndex[kLfoSpeedsShown] = {0, 6, 10, 16, 24};  // the 29-rate table: 128, 16, 4, 1/2, 1/16 steps
  const unsigned shape = variant % kLfoShapes;
  const unsigned speed = (variant / kLfoShapes) % kLfoSpeedsShown;
  const unsigned env = variant / (kLfoShapes * kLfoSpeedsShown) == 0 ? 0u : 2u;
  model::Project& p = project();
  fill_demo_project(p);
  const auto set = [&](unsigned field, float v) { (void)p.set_param(core::param_id(core::ParamKind::Instrument, 0, ip::mod(env, field)), v); };
  set(ip::ModSource, static_cast<float>(ip::SourceLfo));
  set(ip::LfoShape, static_cast<float>(shape));
  // The amp slot has no 128-step rate (its table starts at the sixth of the 29): its first cell takes its slowest rate instead of wrapping below zero.
  const unsigned skip = env == 0 ? ip::kOtherLfoSpeeds - ip::kAmpLfoSpeeds : 0u;
  set(ip::LfoSpeed, static_cast<float>(kRateIndex[speed] > skip ? kRateIndex[speed] - skip : 0u));
  set(ip::LfoAmount, 1.0f);
  draw(r, &p, make_view(ip::GroupEnvelope, env, 1, 1, OverlayId::None));
}

}  // namespace

const ScreenState kParamsStates[9] = {
    {"voice", false, 1, 0, draw_voice},
    {"filter", false, 1, 0, draw_filter},
    {"env", false, 1, 0, draw_envelope},
    {"env_cutoff", false, 1, 0, draw_envelope_cutoff},
    {"sends", false, 1, 0, draw_sends},
    {"learn", false, 1, 0, draw_learn},
    {"no_project", false, 1, 0, draw_no_project},
    {"worst", true, kSweep, kBudgetWords, draw_worst},
    {"lfo_worst", true, kLfoSweep, kBudgetWords, draw_lfo_worst},
};

}  // namespace o3os::sim
