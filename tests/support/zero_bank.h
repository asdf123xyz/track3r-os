// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A Perform bank whose rows hold 0, as every bank was before OS-PRFDEFAULTS (a new bank now holds each FX's three default values, model::kPerfDefaultValues). The older tests pick their own
// values from 0 with the encoder or a loaded state; they take this bank and keep their arithmetic. A test of the defaults takes a plain seq::PerformBank.

#include <o3os/seq/perform.h>

namespace o3os::test {

class ZeroBank : public seq::PerformBank {
 public:
  ZeroBank() noexcept {
    model::PerfState s = state();
    for (auto& row : s.value) {
      for (std::int16_t& v : row) {
        v = 0;
      }
    }
    load_slots(s);
  }
};

}  // namespace o3os::test
