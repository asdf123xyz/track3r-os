// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The plot gallery (OS-003d): every plot type of ui/plot drawn from deterministic synthetic signals (sine, noise, a drum
// hit, a chirp, a stereo phase), each state composed with the standard chrome (a status strip on top, the info line and the
// key-caps below) so that its display-list budget is the one a real screen would have to meet. It is a screen of the
// registry (sim/screens.h), so the budget harness measures it in every theme and both translate modes and the gallery draws it
// in every theme. The states are in plot_screens.cpp; this header is how the registry reaches them.

#include <o3os/sim/screens.h>

namespace o3os::sim {

inline constexpr unsigned kPlotStateCount = 10;
extern const ScreenState kPlotStates[kPlotStateCount];

}  // namespace o3os::sim
