// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// One frame's content: the base screen in view and the overlay slot, drawn in that order, the base told what the overlay
// covers. The app draws every frame this way, and the gallery and the budget harness draw their states through the same
// function (sim/screens_app.cpp), so what they measure and show is what the device draws.

#include <o3os/app/model.h>
#include <o3os/app/view_state.h>
#include <o3os/ui/renderer.h>

namespace o3os::app {

// Between Renderer::begin() and end(). The screen of `view.screen` (Pattern if that one is not implemented), then the
// overlay of `view.shown_overlay()`, if any, then (OS-POWER P1) the line and the strip of a hold of Power, over the modal overlays too.
void render_scene(const Model& model, const ViewState& view, ui::Renderer& r) noexcept;

}  // namespace o3os::app
