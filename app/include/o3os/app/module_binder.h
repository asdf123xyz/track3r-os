// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Opening a project that has module instruments (OS-012a, ADR 0002): the modules its instruments name are loaded from the card (module/loader.h) and every
// Module instrument is bound to an instance of its module (audio/module_voices.h), in slices of the main loop. A module that cannot be had, whatever the
// reason (no file, a file that fails validation, a module quarantined by this boot or by the last crash, no room, an instance that would not start), leaves
// its instruments MUTED: they stay in the project, a note on them sounds nothing, and the report says which module and why ("standalone first, SD extends",
// ADR 0009: a missing or broken SD file never leaves the device unusable). The project itself is never touched.
//
// The caller has retired the old instances, waited for the render to be clear of them (ModuleVoices::quiescent), released them and cleared the set, as a
// project change does; this loads what the new project names. A module that is already in the set (a native one registered by the host build, or loaded
// for an earlier instrument) is not read again. Each distinct module is loaded once, in the order of the project's module table.
//
// A Module instrument's parameter blob (model::Instrument::params, param_len bytes) is given to bind(): one little-endian float32 per parameter of the
// module, in its order. The project's ModuleRef ABI numbers are what the project was saved with: they are informational (the module's own header decides
// what loads), and a difference is counted in `abi_differs`.
//
// Contexts: main loop. One step() moves about `budget` bytes of a module file or binds one instrument. No heap.

#include <o3os/audio/module_voices.h>
#include <o3os/hal/storage.h>
#include <o3os/model/project.h>
#include <o3os/module/loader.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

struct ModuleProblem {
  enum class Kind : std::uint8_t {
    NotFound,     // no modules/instruments/<name>.tmod (or effects/)
    Refused,      // the file failed validation (status says how): quarantined until reboot
    Quarantined,  // refused earlier this boot, or the module the last run crashed in
    NoRoom,       // the arenas could not hold it
    CardError,    // the card failed while reading it
    WrongKind,    // an effect or other kind named by an instrument
    NotRunnable,  // loaded, but this build cannot call it (no native entry)
    BindFailed,   // the instance would not start (status in `bind`)
  };
  Kind kind;
  std::uint8_t slot;  // the instrument (0-based), 0xFF for a problem of the module as such
  module::Status status;
  audio::ModuleVoices::Bind bind;
  char name[module::kStemMax + 1];
};

struct ModuleReport {
  static constexpr unsigned kMaxProblems = 8;
  unsigned instruments = 0;   // Module instruments in the project
  unsigned modules = 0;       // distinct modules they name
  unsigned loaded = 0;        // of them, loaded by this binder
  unsigned already = 0;       // of them, already in the set
  unsigned bound = 0;         // instruments that play
  unsigned muted = 0;         // instruments left silent
  unsigned abi_differs = 0;   // modules whose ABI version is not the one the project was saved with
  unsigned problem_count = 0;
  unsigned problems_dropped = 0;
  ModuleProblem problems[kMaxProblems] = {};
};

class ModuleBinder {
 public:
  enum class State : std::uint8_t { Idle, Running, Done };

  // Starts on `project`'s Module instruments. False when a run is in progress. `sync` is the platform's code-sync hook (ModuleLoader::SyncFn).
  bool begin(hal::storage::Storage& storage, const model::Project& project, module::ModuleSet& set, module::ModuleLoader& loader, audio::ModuleVoices& voices,
             module::ModuleLoader::SyncFn sync) noexcept;
  State step(std::size_t budget) noexcept;
  State state() const noexcept { return state_; }
  const ModuleReport& report() const noexcept { return report_; }

 private:
  void note(ModuleProblem::Kind kind, unsigned slot, const char* name, module::Status status, audio::ModuleVoices::Bind bind) noexcept;
  bool wanted(unsigned module_index) const noexcept;
  void load_next(std::size_t budget) noexcept;
  void bind_next() noexcept;

  enum class Phase : std::uint8_t { Load, Bind };
  hal::storage::Storage* storage_ = nullptr;
  const model::Project* project_ = nullptr;
  module::ModuleSet* set_ = nullptr;
  module::ModuleLoader* loader_ = nullptr;
  audio::ModuleVoices* voices_ = nullptr;
  module::ModuleLoader::SyncFn sync_ = nullptr;
  State state_ = State::Idle;
  Phase phase_ = Phase::Load;
  unsigned cursor_ = 0;      // module table index (Load) / instrument slot (Bind)
  bool loading_ = false;     // a ModuleLoader run is in progress for module `cursor_`
  ModuleReport report_{};
};

// One line for the log: "modules: 2 instruments, 1 loaded, 1 bound, 1 muted".
std::size_t describe_modules(const ModuleReport& r, char* out, std::size_t cap) noexcept;
// A problem as a line: "mod: instrument 3 muted: tb303 not found (modules/instruments/tb303.tmod)". `slot` 0xFF names the module only.
std::size_t describe_module_problem(const ModuleProblem& p, char* out, std::size_t cap) noexcept;

}  // namespace o3os::app
