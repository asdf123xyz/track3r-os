// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The key to the session's editor (session.h). Only app/src/actions.cpp (dispatch) and app/src/edit_actions.cpp (the history
// actions) include this file; tools/check_layers.py fails the build's `layers` test for any other file that does, so that no
// action handler can edit the project except through ActionContext::edit, which dispatch() gives to undoable actions only.

#include <o3os/app/session.h>

namespace o3os::app {

struct SessionAccess {
  static model::Editor* editor(Session& s) noexcept { return s.editor_; }
};

}  // namespace o3os::app
