// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/model/instrument_use.h>

namespace o3os::model {

namespace {

constexpr unsigned kSlots = static_cast<unsigned>(kMaxInstruments);
static_assert(kMaxInstruments == 64, "the sets are 64-bit masks");

// The slots from `centre` - v to `centre` + v, clamped to the table (the sequencer's clamp).
std::uint64_t window(unsigned centre, unsigned v) noexcept {
  const unsigned lo = centre > v ? centre - v : 0u;
  const unsigned hi = centre + v < kSlots - 1u ? centre + v : kSlots - 1u;
  std::uint64_t m = 0;
  for (unsigned i = lo; i <= hi; ++i) {
    m |= std::uint64_t{1} << i;
  }
  return m;
}

std::uint64_t widen(std::uint64_t set, unsigned v) noexcept {
  std::uint64_t out = set;
  for (unsigned i = 0; i < kSlots; ++i) {
    if (((set >> i) & 1u) != 0u) {
      out |= window(i, v);
    }
  }
  return out;
}

}  // namespace

std::uint64_t instruments_in_use(const Project& project) noexcept {
  std::uint64_t named[kMaxTracks] = {};     // the slots the steps of each track name, and the windows of their Random Instrument
  unsigned idle_window[kMaxTracks] = {};    // the widest Random Instrument on a step of the track that names no instrument of its own
  for (std::uint32_t id = 1; id <= kMaxPatternId; ++id) {
    const Pattern* pattern = project.pattern(id);
    if (pattern == nullptr) {
      continue;
    }
    for (std::uint32_t t = 0; t < kMaxTracks; ++t) {
      for (std::uint32_t s = 0; s < kMaxPatternLength; ++s) {
        const Step& step = pattern->steps[t][s];
        unsigned v = 0;  // the windows of the columns that are Random Instrument add: the second column's pick is the first's centre
        for (const StepFx& fx : step.fx) {
          v += fx.type == kFxRandomInstrument ? fx.value : 0u;
        }
        if (step.instr >= 1u && step.instr <= kSlots) {
          named[t] |= window(step.instr - 1u, v);
        } else if (v > idle_window[t]) {
          idle_window[t] = v;
        }
      }
    }
  }
  std::uint64_t used = 0;
  for (std::uint32_t t = 0; t < kMaxTracks; ++t) {
    std::uint64_t reach = named[t];
    while (idle_window[t] != 0u && reach != 0u) {  // a track that plays "the last instrument" at random can drift as far as its windows go: the closure (at most 64 rounds)
      const std::uint64_t next = widen(reach, idle_window[t]);
      if (next == reach) {
        break;
      }
      reach = next;
    }
    used |= reach;
  }
  const Settings& settings = project.settings();
  for (unsigned i = 0; i < core::kLearnSlots; ++i) {
    const core::LearnBinding& b = settings.learn(i);
    if (!core::learn_empty(b) && core::param_kind(b.param) == core::ParamKind::Instrument && (b.flags & core::kLearnFollow) == 0u && core::param_index(b.param) < kSlots) {
      used |= std::uint64_t{1} << core::param_index(b.param);
    }
  }
  return used;
}

std::uint64_t instruments_unused(const Project& project, unsigned* modules_skipped, std::uint64_t held) noexcept {
  const std::uint64_t used = instruments_in_use(project) | held;
  std::uint64_t unused = 0;
  unsigned modules = 0;
  for (unsigned slot = 0; slot < kSlots; ++slot) {
    const Instrument* in = project.instrument(slot);
    if (in == nullptr || ((used >> slot) & 1u) != 0u) {
      continue;
    }
    if (in->kind == InstrumentKind::Module) {
      ++modules;
    } else {
      unused |= std::uint64_t{1} << slot;
    }
  }
  if (modules_skipped != nullptr) {
    *modules_skipped = modules;
  }
  return unused;
}

}  // namespace o3os::model
