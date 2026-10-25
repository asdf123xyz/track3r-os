// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The cursor interface of OS-002b: what the navigation actions (nav.left / right / up / down / scroll, cursor.jump_*,
// cursor.page, cursor.track, cursor.home, cursor.end) move. A screen or an overlay that has something to move implements
// it, and the actions work on whatever owns the input through it without ever naming a screen: adding a screen with a list
// is its class, and nothing in the action files. The defaults do nothing (`count` 0), which is what a screen without a
// cursor, or the Learn overlay, wants.
//
// The model is a list along one axis (the main one: steps, positions, cells); a screen with a second axis (the pattern's
// columns and tracks, the Navigator's grid) takes it in `step` and `track`. Every move clamps to the list and returns true
// only if something visible changed.

#include <o3os/app/model.h>
#include <o3os/app/view_state.h>

namespace o3os::app {

class Cursor {
 public:
  // Items along the main axis: 0 when there is nothing to move. The view is there for a list whose length depends on where the user is
  // (the generated edit page has a different number of rows on each of its pages, OS-002d).
  virtual int count(const Model& model, const ViewState& view) const noexcept {
    (void)model;
    (void)view;
    return 0;
  }
  virtual int position(const ViewState& view) const noexcept {
    (void)view;
    return 0;
  }
  // How many items a page is (what is visible).
  virtual int page() const noexcept { return 1; }
  // OS-AUDIT-28: a list that the encoder may move faster than a row a detent while it is spun (nav.scroll through enc_accel.h). False for everything that edits a value or is a cursor in a pattern.
  virtual bool accelerates() const noexcept { return false; }
  // To an item, clamped to 0..count-1. True if the cursor or the window moved.
  virtual bool set_position(const Model& model, ViewState& view, int position) const noexcept {
    (void)model;
    (void)view;
    (void)position;
    return false;
  }
  // One arrow: dx = -1 / +1 is left / right, dy = -1 / +1 is up / down (one of them is not 0). A list moves on up / down.
  virtual bool step(const Model& model, ViewState& view, int dx, int dy) const noexcept {
    return dx == 0 && dy != 0 && set_position(model, view, position(view) + dy);
  }
  // OS-ROWWRAP: step() for a FRESH press of the arrow (not an auto-repeat of a held one). A screen whose list wraps at its ends does it here, so that a held arrow stops at the
  // edge instead of cycling. By default no list wraps: the same as step().
  virtual bool step_press(const Model& model, ViewState& view, int dx, int dy) const noexcept { return step(model, view, dx, dy); }
  // cursor.track: `delta` tracks (a second axis, if the screen has one).
  virtual bool track(const Model& model, ViewState& view, int delta) const noexcept {
    (void)model;
    (void)view;
    (void)delta;
    return false;
  }
  // cursor.home / cursor.end: the start and the end, by default the first and the last item.
  virtual bool home(const Model& model, ViewState& view) const noexcept { return set_position(model, view, 0); }
  virtual bool end(const Model& model, ViewState& view) const noexcept {
    return set_position(model, view, count(model, view) - 1);
  }

 protected:
  Cursor() = default;
  ~Cursor() = default;  // never deleted through the interface: static instances, no heap
  Cursor(const Cursor&) = delete;
  Cursor& operator=(const Cursor&) = delete;
};

}  // namespace o3os::app
