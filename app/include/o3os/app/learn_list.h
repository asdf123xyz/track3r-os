// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The bindings of both Learn stores as one list (OS-013a): what the Learn overlay shows and what its cursor, delete and clear-all act on. The project's used slots come first
// (when a project is open), then the device's, each in slot order; an empty slot is not in the list. The list is read from the stores on every call (at most 2 x 32 slots), so
// it is never stale and costs no storage.

#include <o3os/app/model.h>
#include <o3os/core/learn.h>

namespace o3os::app {

struct LearnEntry {
  const core::LearnBinding* binding;
  core::LearnScope scope;
  unsigned slot;  // the slot in its store
};

// How many bindings are in use in the two stores.
unsigned learn_entry_count(const Model& model) noexcept;
// The `index`-th binding of the list; false when there is none.
bool learn_entry_at(const Model& model, unsigned index, LearnEntry& out) noexcept;

}  // namespace o3os::app
