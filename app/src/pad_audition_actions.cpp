// SPDX-License-Identifier: GPL-3.0-or-later
// OS-AUDIT-40 / OS-AUDIT-26 (new Community OS behaviour): a pad auditions an instrument on the screens that give it no other job. Sample load: the pad also chooses the target instrument
// (pad n = instrument n + 1, as on Params and the Pattern Instr column) and plays it; Mixer, Config and File: the pad plays the current instrument. The sound is the Pattern pads' path
// (PreviewSink::pad_down, held until the pad's release: App::run_once hands every pad release to PreviewSink::pad_up, whatever the screen is by then, so a pad held across a screen change ends).
#include "action_handlers.h"

#include <o3os/app/pad_note.h>

namespace o3os::app {

bool pad_audition(ActionContext& ctx, core::ActionArg arg) noexcept {
  ViewState& v = ctx.view;
  if (arg < 0 || arg >= static_cast<core::ActionArg>(model::edit::kPadCount)) {
    return false;
  }
  const bool load = v.screen == ScreenId::SampleLoad;
  if (!load && v.screen != ScreenId::Mixer && v.screen != ScreenId::Config && v.screen != ScreenId::File) {
    return false;  // the other screens keep their pad job
  }
  const unsigned pad = static_cast<unsigned>(arg);
  const bool changed = load && instrument_select(ctx, arg + 1);  // (an instrument 1..48: the board's 48 pads)
  if (ctx.preview != nullptr) {
    ctx.preview->pad_down(pad, static_cast<unsigned>(v.pattern.track()), static_cast<unsigned>(v.instrument), pad_note(ctx.model.project, v.instrument, v.edit.pads, pad));
  }
  return changed;
}

}  // namespace o3os::app
