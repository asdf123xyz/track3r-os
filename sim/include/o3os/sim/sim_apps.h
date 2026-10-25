// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The shipped apps in the simulator's card (OS-APPUX S0, scenario command `apps on`). The simulator has no SD module loader: a `.tmod` is an Armv7E-M image and the host cannot run it. What
// the host can run is the same source built as a shared object (modules/CMakeLists.txt o3m_<name>_host: the files the per-app tests dlopen), so this rig does what the unit's card does with the
// apps in /modules/apps, with the host objects standing in for the images:
//
//   the card    `modules/apps/<stem>.tmod` is written for every shipped app (a one-line placeholder: the card's folder is how MODULES lists apps; the OS reads a card file only to load it, and a module
//               that is in the set is never loaded from the card, module_ui.cpp ModuleUi::load);
//   the set     each host object is dlopen'ed and registered as a native module under its stem with the capabilities its manifest asks for (module::ModuleSet::add_native: the same checks as a
//               file's header, ABI, sizes, tables; the descriptor `o3m_entry` is the one the .tmod carries);
//   the host    app::ModuleUi over the card, with the simulator's clock (the views' time budget and the apps' `now_ms` run on the scenario's clock, so a run stays a function of the script),
//               an audio tap that carries nothing (the Tuner says NO SIGNAL, the Scope draws a flat line) and a MIDI-in ring that nothing feeds (MIDI Monitor says WAITING FOR MIDI).
//
// The list of apps is the build's (sim/CMakeLists.txt O3SIM_APPS, one entry for each modules/apps/<name>/module.json: the configure step fails when the two differ), and the table the rig reads
// is GENERATED from it (sim_apps_table.h): the stem, the object's path and the manifest's capability word (<NAME>_CAPS of the generated descriptor header, tools/o3m_gen.py), so nothing
// is written twice. Pads (the SDK example, no manifest) is not in it.
//
// Not simulated: the audio engine behind the tap (an `audio on` run has no apps: the parser refuses the pair), MIDI input, the loader's checks of a real file (tests/module_loader_test.cpp), the
// module region's addresses, the cost of a call on the Cortex-M4F. A host test and a golden of an app page say what the page draws, not that the app runs on the unit.
//
// Main loop only (the simulator's thread).

#include <o3os/app/module_ui.h>
#include <o3os/audio/app_tap.h>
#include <o3os/audio/module_voices.h>
#include <o3os/hal/clock.h>
#include <o3os/hal/storage.h>
#include <o3os/midi/monitor.h>
#include <o3os/module/loader.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace o3os::sim {

class SimApps {
 public:
  // Opens the shipped apps over `card` (mounted, with `root` as its directory: the placeholders are written there) and the simulator's `clock`. nullptr: `error` says which object did not open or
  // which check refused it.
  static std::unique_ptr<SimApps> create(hal::storage::Storage& card, const std::string& root, const hal::Clock& clock, std::string& error);
  ~SimApps();
  SimApps(const SimApps&) = delete;
  SimApps& operator=(const SimApps&) = delete;

  app::ModuleUi& ui() noexcept { return ui_; }
  // The apps the rig carries (the build's list), and their stems in the order of the list.
  unsigned count() const noexcept { return static_cast<unsigned>(stems_.size()); }
  const char* stem(unsigned i) const noexcept { return i < stems_.size() ? stems_[i].c_str() : ""; }

 private:
  SimApps() = default;
  bool open(hal::storage::Storage& card, const std::string& root, const hal::Clock& clock, std::string& error);

  struct AlignedDelete {
    void operator()(std::uint8_t* p) const noexcept { ::operator delete[](p, std::align_val_t{64}); }
  };
  using Block = std::unique_ptr<std::uint8_t[], AlignedDelete>;

  std::vector<void*> handles_;      // the shared objects, closed last: the set's entries point into them
  std::vector<std::string> stems_;
  Block scratch_;                   // the voices' output blocks (the apps render nothing, ModuleVoices wants its scratch attached)
  Block arena_;                     // the cold arena: the apps' state blocks (the largest is the Tuner's 40,000 bytes)
  module::ModuleSet set_;
  module::ModuleLoader loader_;
  audio::ModuleVoices voices_;
  audio::AppTap tap_;
  midi::MidiMonitor monitor_;
  app::ModuleUi ui_;
};

}  // namespace o3os::sim
