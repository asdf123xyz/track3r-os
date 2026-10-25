// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-USBRET: the device setting USB RETURN and the USB driver's return flag kept in step (the decision of the platform's main loop, as a header so that the host tests drive it).
//
//   SETTING -> DRIVER  when the setting changed (its generation moved), and once at the first pass after boot (the settings file's value): the driver is asked for it. In STANDARD it
//                      only remembers it; in AUDIO a different value re-enumerates the unit. A request the driver cannot take (a change is already in flight) is made again on a later pass.
//   DRIVER -> SETTING  when the driver's target differs from the setting although the setting did not change: the console's `usb return` and `usb profile <n> out|duplex` changed the
//                      driver, and the setting (so Config) follows.
//
// The driver is a small policy (the MK66's wraps usb0, the tests use a fake):
//   bool ready()               the driver runs and presents a descriptor set (nothing is decided while it does not)
//   bool disk()                the driver presents the DISK set, or is changing to it (OS-USBDISK): nothing is decided then either, see below
//   bool target()              the return the driver presents, or is changing to
//   bool request(bool on)      ask for it with the mode and profile as they are; false: refused for now (busy)
// THE DISK SET IS FINAL (OS-USBDISK boot disk, v0.6.8 bug). The boot disk starts the port with the DISK descriptors without reading the settings (the card is the computer's: nothing is
// mounted), so the settings are the defaults, USB AUDIO On among them, and the first pass of these syncs would take the setting for a change to carry out and re-enumerate the unit as AUDIO
// in the middle of the computer's enumeration. While the driver presents or is changing to DISK, neither sync asks for anything, stores anything or marks the setting as seen; the way out of
// DISK is a restart (or, in the running mode, the eject, which is the hand-over's own call).
// Main loop only, once a pass: two generation reads and a compare. No allocation, no blocking.
//
// OS-USBAUDIO adds the setting USB AUDIO (Off: STANDARD, On: AUDIO, the default) in two places:
//   AT BOOT   usb_boot_layout(): the descriptor set the port enumerates with the first time, from the setting, the layout (profile) and USB RETURN, decided before the pull-up goes on
//             (main.cpp starts USB after the card is read), so the host sees one device and no STANDARD-then-AUDIO switch.
//   RUNTIME   sync_usb_audio(): the same sync as USB RETURN (SETTING -> DRIVER), for the mode. NOT DRIVER -> SETTING: the console's `usb mode audio|standard` is runtime only and leaves the
//             setting alone (a reboot comes up as the setting says). The adapter asks for the mode together with the setting USB RETURN, and sync_usb_audio runs before sync_usb_return, so one
//             APPLY of the two settings is one re-enumeration (the return sync then finds the driver's target already equal to its setting).

#include <o3os/audio/graph_params.h>
#include <o3os/audio/stem_map.h>
#include <o3os/core/param.h>
#include <o3os/model/params.h>
#include <o3os/model/settings.h>
#include <o3os/usb/usb_types.h>

#include <cstdint>

namespace o3os::app {

class UsbReturnSync {
 public:
  enum class Act : std::uint8_t {
    None,
    Request,  // ask the driver for the setting's value; report the outcome with accepted()
    Store,    // write the driver's value into the setting; report the setting's new generation with stored()
  };

  // `want` is the setting's value, `generation` its generation, `have` the driver's target.
  Act step(bool want, std::uint32_t generation, bool have) noexcept {
    if (!seen_ || generation != generation_) {
      if (want == have) {
        accepted(generation);  // already the same: nothing to ask, and the generation is settled
        return Act::None;
      }
      return Act::Request;
    }
    return want != have ? Act::Store : Act::None;
  }
  // After Act::Request: the driver took the value (true), or refused for now (false: the setting stays unseen and step() asks again).
  void accepted(std::uint32_t generation) noexcept {
    seen_ = true;
    generation_ = generation;
  }
  // After Act::Store: the setting's generation after the write, so that the write is not taken for a change of the setting.
  void stored(std::uint32_t generation) noexcept { generation_ = generation; }

 private:
  std::uint32_t generation_ = 0;
  bool seen_ = false;
};

// One pass over a driver policy (above) and the device settings.
template <class Driver>
void sync_usb_return(UsbReturnSync& sync, model::Settings& device, Driver& driver) noexcept {
  namespace ds = model::device_setting;
  if (!driver.ready() || driver.disk()) {
    return;
  }
  const bool want = device.get(ds::UsbReturn) != 0.0f;
  const std::uint32_t generation = device.generation(ds::UsbReturn);
  switch (sync.step(want, generation, driver.target())) {
    case UsbReturnSync::Act::Request:
      if (driver.request(want)) {
        sync.accepted(generation);
      }
      break;
    case UsbReturnSync::Act::Store:
      (void)device.set(ds::UsbReturn, driver.target() ? 1.0f : 0.0f);
      sync.stored(device.generation(ds::UsbReturn));
      break;
    case UsbReturnSync::Act::None:
      break;
  }
}

// ---- USB AUDIO (OS-USBAUDIO) ---------------------------------------------------------------------------------------------------------------

// The set the port is first enumerated with. `mode` is AUDIO when the setting is On and the engine can feed `layout` (the profile: the boot's is 8, "8 OUT / 2 IN"); otherwise STANDARD, with
// the layout and the return still kept for a later `usb mode audio` (STANDARD's descriptors do not depend on either). The layout is the profile the driver starts with (nothing else saves one:
// a project's USB part brings its own profile and stem map after its load, which is a re-enumeration only when it differs).
struct UsbBootLayout {
  usb::Mode mode = usb::Mode::Standard;
  usb::Profile profile = usb::Profile::P8;
  bool usb_return = true;
};

inline UsbBootLayout usb_boot_layout(bool usb_audio, usb::Profile layout, bool usb_return) noexcept {
  UsbBootLayout b;
  b.mode = usb_audio && audio::StemMap::supported(layout) ? usb::Mode::Audio : usb::Mode::Standard;
  b.profile = layout;
  b.usb_return = usb_return;
  return b;
}

// The mode the unit presents (or is changing to) and a request for another, as a driver policy for sync_usb_audio below (the MK66's wraps usb0, the tests use a fake):
//   bool ready()               the driver runs
//   bool disk()                the driver presents the DISK set, or is changing to it: the sync does nothing (see the note above sync_usb_return)
//   bool audio()               the driver presents AUDIO, or is changing to it
//   bool request(bool audio)   ask for AUDIO or STANDARD with the profile as it is and USB RETURN as the setting has it; false: refused for now (busy)
// The sync is the one of USB RETURN (UsbReturnSync: a change of the setting is requested, the first pass after boot included); its Store, the driver moved by the console, is dropped.
using UsbAudioSync = UsbReturnSync;

template <class Driver>
void sync_usb_audio(UsbAudioSync& sync, const model::Settings& device, Driver& driver) noexcept {
  namespace ds = model::device_setting;
  if (!driver.ready() || driver.disk()) {
    return;
  }
  const bool want = device.get(ds::UsbAudio) != 0.0f;
  const std::uint32_t generation = device.generation(ds::UsbAudio);
  if (sync.step(want, generation, driver.audio()) == UsbAudioSync::Act::Request && driver.request(want)) {
    sync.accepted(generation);
  }
}

// OS-006e: the device setting RETURN LEVEL and the engine's. The level of the USB return to the outputs is the engine's own parameter (audio::graph_param::master::UsbReturnLevel); this tells it the
// setting's value when the setting's generation moved (Config, a bound controller, the settings file's value at the first pass) and at the first pass, over the ParamChange ring the main
// loop produces (ramp 0: the engine's own smoothing, one block). A full ring is tried again on a later pass. Main loop only, once a pass: one generation read and a compare.
class UsbReturnLevelSync {
 public:
  bool step(const model::Settings& device, core::ParamRing& ring) noexcept {
    namespace ds = model::device_setting;
    const std::uint32_t generation = device.generation(ds::UsbReturnLevel);
    if (seen_ && generation == generation_) {
      return false;
    }
    if (!ring.push(core::ParamChange{audio::master_param(audio::graph_param::master::UsbReturnLevel), device.get(ds::UsbReturnLevel), 0})) {
      return false;
    }
    seen_ = true;
    generation_ = generation;
    return true;
  }

 private:
  std::uint32_t generation_ = 0;
  bool seen_ = false;
};

}  // namespace o3os::app
