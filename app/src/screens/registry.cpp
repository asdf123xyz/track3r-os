// SPDX-License-Identifier: GPL-3.0-or-later
// The screen registry (OS-002b): every screen the Tracker's eleven screen buttons reach, in the Navigator's order, with the
// ones that exist (Pattern, Song, Instrument PARAMS, File, Sample load, Wave, Sample edit, Config) and the ones that do not yet. One row to add a screen (see screens/screen.h).
#include <o3os/app/screens/config_screen.h>
#include <o3os/app/screens/file_screen.h>
#include <o3os/app/screens/input_test_screen.h>
#include <o3os/app/screens/mixer_screen.h>
#include <o3os/app/screens/modules_screen.h>
#include <o3os/app/screens/params_screen.h>
#include <o3os/app/screens/perform_screen.h>
#include <o3os/app/screens/record_screen.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/app/screens/sample_edit_screen.h>
#include <o3os/app/screens/sample_load_screen.h>
#include <o3os/app/screens/screen.h>
#include <o3os/app/screens/song_screen.h>
#include <o3os/app/screens/wave_screen.h>

namespace o3os::app {

namespace {

using ui::Icon;

// Names are the product-ux section 4 screen names; keys are the buttons' names on the device. Not implemented: nullptr,
// shown disabled by the Navigator. Icons come from the 11 of the icon face (ui/assets.h), so some repeat.
constexpr ScreenEntry kScreens[] = {
    {ScreenId::Pattern, "PATTERN", &kPatternScreen, Button::Pattern, "PATTERN", Icon::Loop},
    {ScreenId::Song, "SONG", &kSongScreen, Button::Song, "SONG", Icon::Play},
    {ScreenId::Instrument, "INSTRUMENT", &kParamsScreen, Button::Params, "PARAMS", Icon::Module},
    {ScreenId::Wave, "WAVE", &kWaveScreen, Button::SamplePlay, "SAMPLE PLAY", Icon::Play},
    {ScreenId::SampleEdit, "SAMPLE EDIT", &kSampleEditScreen, Button::SampleEdit, "SAMPLE EDIT", Icon::Mute},
    {ScreenId::SampleLoad, "SAMPLE LOAD", &kSampleLoadScreen, Button::SampleLoad, "SAMPLE LOAD", Icon::Folder},
    {ScreenId::Record, "RECORD", &kRecordScreen, Button::SampleRec, "SAMPLE REC", Icon::Record},
    {ScreenId::Mixer, "MIXER", &kMixerScreen, Button::Master, "MASTER", Icon::Solo},
    {ScreenId::Perform, "PERFORM", &kPerformScreen, Button::Performance, "PERFORMANCE", Icon::Midi},
    {ScreenId::File, "FILE", &kFileScreen, Button::File, "FILE", Icon::Folder},
    {ScreenId::Config, "CONFIG", &kConfigScreen, Button::Config, "CONFIG", Icon::Lock},
    // OS-012b: the SD modules' screen: in the Navigator, no button of its own (Button::F1 is a placeholder, as for the hidden rows).
    {ScreenId::Modules, "MODULES", &kModulesScreen, Button::F1, "NAVIGATOR", Icon::Module, false, true},
    // OS-007d: the hardware session's diagnostic. Hidden: not in the Navigator, no screen button (F1 stands in for `button`, unused);
    // the platform opens it from a boot chord.
    {ScreenId::InputTest, "INPUT TEST", &kInputTestScreen, Button::F1, "F1", Icon::Midi, true},
    // OS-012b: a running app's page: opened by launching an app on MODULES, left by the exit chord. Hidden: not in the Navigator.
    {ScreenId::ModuleApp, "APP", &kModuleAppScreen, Button::F1, "F7 + F8", Icon::Module, true},
};
constexpr std::size_t kScreenRows = sizeof(kScreens) / sizeof(kScreens[0]);
constexpr std::size_t kVisibleRows = kScreenRows - 2;  // the two hidden rows are last
static_assert(kScreenRows == kScreenCount, "one row per screen id");
static_assert(kScreens[kVisibleRows].hidden && kScreens[kVisibleRows + 1].hidden && !kScreens[kVisibleRows - 1].hidden, "the hidden rows are the last ones");
static_assert(screens_valid(kScreens, kScreenRows), "screen registry: duplicate or malformed row");

}  // namespace

ScreenTable screen_registry() noexcept { return ScreenTable{kScreens, kScreenRows}; }

ScreenTable navigator_screens() noexcept { return ScreenTable{kScreens, kVisibleRows}; }

const ScreenEntry* find_screen(ScreenTable table, ScreenId id) noexcept {
  for (std::size_t i = 0; i < table.count; ++i) {
    if (table.items[i].id == id) {
      return &table.items[i];
    }
  }
  return nullptr;
}

const ScreenEntry* screen_at(ScreenTable table, int index) noexcept {
  return index >= 0 && static_cast<std::size_t>(index) < table.count ? &table.items[index] : nullptr;
}

int screen_position(ScreenTable table, ScreenId id) noexcept {
  for (std::size_t i = 0; i < table.count; ++i) {
    if (table.items[i].id == id) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool switch_screen(ScreenTable table, const Model& model, ViewState& view, ScreenId target) noexcept {
  const ScreenEntry* to = find_screen(table, target);
  if (to == nullptr || to->screen == nullptr || target == view.screen) {
    return false;
  }
  const ScreenEntry* from = find_screen(table, view.screen);
  if (from != nullptr && from->screen != nullptr) {
    from->screen->leave(model, view);
  }
  view.screen = target;
  view.peeking = false;  // OS-SONGPEEK: a screen change that is not the peek's own (nav.peek sets the flag after it) ends the peek
  view.params.discard_pending();  // OS-USBAUDIO: a value waiting to be applied belongs to the page it was chosen on
  to->screen->enter(model, view);
  return true;
}

bool navigate_to(ScreenTable table, const Model& model, ViewState& view, ScreenId target) noexcept {
  const ScreenEntry* to = find_screen(table, target);
  if (to == nullptr) {
    return false;
  }
  if (to->screen == nullptr) {  // its button is pressed but it does not exist yet: say so
    core::FixedString<core::kNotifyTextChars> text;
    text.assign(to->name);
    text.append(" IS NOT AVAILABLE YET");
    return view.notify.post(core::Severity::Info, text.c_str(), "nav", ViewState::kMessageMs);
  }
  if (target == view.screen) {  // the current screen's own button: its next page
    return to->screen->repress(model, view);
  }
  return switch_screen(table, model, view, target);
}

}  // namespace o3os::app
