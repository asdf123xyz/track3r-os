// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace o3os::model {

// What a mutator of the model says (project.h, settings.h, editor.h). A rejected mutation changes nothing and bumps nothing.
enum class [[nodiscard]] Status : std::uint8_t {
  Ok,
  BadIndex,   // id, slot, position or step outside its range
  BadValue,   // value outside its range, text too long or containing a NUL, unsafe path or name
  Exists,     // pattern id already in use
  NotFound,   // no such pattern
  Full,       // pattern pool, song, module table or unknown-chunk pool is full
};

}  // namespace o3os::model
