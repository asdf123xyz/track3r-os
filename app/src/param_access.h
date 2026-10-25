// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the actions that touch parameters share (private to app/src, OS-002d): finding the parameter under the cursor, reading and
// writing one wherever it lives (the project through the editor an undoable action is handed, the device in its settings block), and
// telling the audio side.

#include "action_handlers.h"

#include <o3os/core/learn.h>
#include <o3os/core/param.h>

namespace o3os::app {

// The parameter the base screen's cursor is on (core::ParamId::None when it has none).
core::ParamId focus_param(const ActionContext& ctx) noexcept;

// The value of a parameter; false when there is nothing to read: a project parameter without a project, an instrument slot that is free.
bool read_param(const Model& model, core::ParamId id, float& value) noexcept;
// Its generation (project or device block), 0 if it does not exist.
std::uint32_t param_generation_of(const Model& model, core::ParamId id) noexcept;

enum class WriteResult : std::uint8_t { Changed, Unchanged, Rejected, NoProject, NoInstrument };

// Sets a parameter to `value` (it must be in range: BadValue is Rejected). A project parameter goes through ctx.edit, so it is part of the
// undo unit the dispatcher opened; without an editor it is NoProject. A change is sent to the audio side as a ParamChange (ramp 0: the
// engine's own smoothing).
// `tell` false (OS-MUTE: a deferred mute, which the sequencer tells the strip at its boundary): the project is written, nothing is sent.
WriteResult write_param(ActionContext& ctx, core::ParamId id, float value, bool tell = true) noexcept;

// OS-UNDOGRP: the click of an encoder on a parameter row. The edit that follows may join the undo unit of the last click on the same row (a sweep is one
// unit); a pause of kClickGroupMs since the last click ends the group first. Device settings have no undo and nothing to merge.
inline constexpr std::uint32_t kClickGroupMs = 1500;  // the tap timeout (D8)
void merge_click(ActionContext& ctx, core::ParamId id) noexcept;

// Pushes `{id, value, ramp}` to the ring if one is attached; a full ring counts a drop.
void send_param_change(const ActionContext& ctx, core::ParamId id, float value, std::uint32_t ramp = 0) noexcept;

// `detents` fine steps of a parameter that is not under the cursor (OS-MASTERVOL: the master volume from any screen), as the encoder moves the one that is: the same clamp, the click group
// and the message when nothing could be written. True when the parameter changed or a message was posted.
bool param_edit_id(ActionContext& ctx, core::ParamId id, int detents) noexcept;

// An info notification saying why nothing could be written ("NO PROJECT OPEN"). Returns whether the screen changed.
bool say_not_written(ActionContext& ctx, WriteResult result) noexcept;

}  // namespace o3os::app
