// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The real audio engine inside the simulator (OS-E2E-2, scenario command `audio on`, `render <name> <ms>`, `assert audio`; docs/specs/OS-E2E.md). The unit's composition root is
// platform/mk66/src/app_main.cpp, which is firmware and which nothing may link; this wires the same libraries the way the host tests do (tests/audio_demo_test.cpp,
// tests/play_after_load_test.cpp), minus the hardware:
//
//   audio::Engine over a TapMem, the sampler voices (the card's pool when the simulator has a card, else the rig's own), the sequencer over the open document as the engine's event source
//   (seq::event_source), the live notes' box (the pads, the previews), the two send effects and the master limiter in their slots, the ParamChange ring as the app's param ring, the
//   sequencer's position as the app's play position, and the app's sinks: the transport and the track and perform sides on the sequencer's command ring, the live preview on the live queue.
//   The Amiga voices (OS-E2E-9, as app_main.cpp's engine.attach_amiga): the instruments whose engine is Amiga (a MOD or an OctaMED module imported by File > IMPORT) play through a Paula channel, driven by the MED
//   tick processor from the sequencer's Tick events (Sequencer::Config::emit_ticks, on as the unit has it).
//   The WAV export (OS-E2E-9, as app_main.cpp's wire_export): seq::OfflineRender over the engine and a sequencer of its own for each pass, the staging ring of app::ExportSource, handed to the save layer
//   (export_wiring()). The live callback is the engine's own trampoline, so an export parks it as it parks the unit's render line, and the blocks of a live `render` that are due meanwhile are silence.
//   The TB-303 (OS-E2E-9): the module's HOST shared object (modules/CMakeLists.txt o3m_tb303_host, the same source as the .tmod, dlopen'ed from the path the build puts in O3SIM_TB303_HOST_SO) is bound with
//   audio::ModuleVoices::bind to every Module instrument of the document that names "tb303", with the instrument's parameter blob, and the engine's modules are attached (the unit's make_modules and the
//   binder, without a card, a module file or the module UI). A Module instrument that names another module stays unbound (muted): E2E-7.
//   Not wired (E2E-7, later slices): the other SD modules, their card loader and the module UI, the MIDI link and its ports, the recorder and the capture hardware, the apps' audio tap, the USB stem map.
//
// TIME. The simulator ticks 1 ms; the engine renders blocks of 64 frames at its own rate (hal/audio.h, 44,117.647 Hz). An integer accumulator adds the rate (in millionths of a frame) every tick and
// renders a block each time 64 frames' worth has gathered: 0 or 1 block a tick, 689.06 blocks a simulated second, no wall clock anywhere. Within a tick the app's pass comes first and then the
// audio, so what the app did in a tick (a pad, Play) is heard from that tick's block on, and a note begins at a block boundary (a live note) or at its sample offset (a sequencer note).
//
// CAPTURES. `render <name> <ms>` opens a window of <ms> simulated milliseconds starting now; the blocks the engine renders in those ticks (the master, as the codec gets it: interleaved L, R, 16 bit)
// are the render. The window closes by itself when its time is up, whichever commands moved the clock. `stems` also captures the tracks' post-fader taps (the default USB profile: T1..T8, each mono) and the
// master pair as the stem tap sees it, as the renders `<name>.t1` .. `<name>.t8` and `<name>.m`; a stems render is a whole number of stem blocks (128 frames), so an odd last block of the master is left out.
//
// THE SOUND. Nothing on the card is needed: a rig without a card gives the document's instruments a test bank (below), bound through app::make_sample_desc from the instruments of the project, so a
// parameter the project holds (volume, pan, envelope, filter, bit depth) acts as on the unit. With a card the project's own samples are what the loader bound; no test bank then.
//
//   slot 0  "sine"   100 samples a period, 40 periods, looped: 441.18 Hz at the root note (kRootNote 60) at the engine's rate, amplitude 0.5 of full scale
//   slot 1  "noise"  8192 samples of xorshift32 white noise (seed 0x1F2E3D4C), amplitude 0.5, looped
//   slot 2  "pluck"  a one shot of 22050 samples: the sine's period with an amplitude that decays by e every 0.12 s, starting at 0.8
//   slots 3..       the sine again (any pattern of the demo project sounds)
//
// Determinism: a run is a pure function of the script. The fake cycle counter, the xorshift seed, the integer pacing and the engine's own fixed-point and float arithmetic have no input but the
// script. Float results are reproducible on one machine and compiler (they are across runs: o3sim --twice compares the SHA-256 of every render); across machines the checks use tolerances.

#include <o3os/app/actions.h>
#include <o3os/app/export_source.h>
#include <o3os/app/pad_board.h>
#include <o3os/app/mix_board.h>
#include <o3os/app/perform_board.h>
#include <o3os/app/track_state.h>
#include <o3os/audio/amiga_voices.h>
#include <o3os/audio/engine.h>
#include <o3os/audio/live_notes.h>
#include <o3os/audio/master_limiter.h>
#include <o3os/audio/module_voices.h>
#include <o3os/audio/play_position.h>
#include <o3os/audio/send_delay.h>
#include <o3os/audio/send_reverb.h>
#include <o3os/audio/tap_mem.h>
#include <o3os/audio/voices.h>
#include <o3os/core/param.h>
#include <o3os/model/project.h>
#include <o3os/seq/offline.h>
#include <o3os/seq/sequencer.h>
#include <o3os/sim/audio_check.h>
#include <o3os/usb/stream.h>

#include <cstdint>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace o3os::sim {

struct SimExportRig;  // the offline renderer and what it works in (sim_audio.cpp)

// A 16-byte aligned block from the aligned operator new, with the matching delete (the module instances want 16-byte state; a plain delete[] on it is a mismatch).
struct AlignedDelete {
  void operator()(std::uint8_t* p) const noexcept { ::operator delete[](p, std::align_val_t{16}); }
};
using AlignedBlock = std::unique_ptr<std::uint8_t[], AlignedDelete>;

class SimAudio {
 public:
  static constexpr std::uint32_t kMaxRenderMs = 10000;  // a render is at most this long (the journeys keep to 2 s)
  static constexpr unsigned kStemTracks = 8;            // the default USB profile (P10): T1..T8 and the master pair

  // `project`: the document the sequencer plays (it must outlive the rig). `voices`: the pool the engine renders, the card's loader having bound its samples; nullptr: the rig makes a pool of its own and binds
  // the test bank to it.
  SimAudio(const model::Project& project, audio::VoicePool* voices);
  ~SimAudio();
  SimAudio(const SimAudio&) = delete;
  SimAudio& operator=(const SimAudio&) = delete;

  // What the simulator hands to the app.
  app::TransportSink& transport();
  app::PreviewSink& preview();
  app::MixBoard& mix();
  app::PerformBoard& perform();
  core::ParamRing& params() { return engine_->params(); }
  const audio::PlayPosition& play_position() const { return sequencer_->play_position(); }

  // One simulated millisecond: renders the blocks that are due (0 or 1) and feeds the renders that are open.
  void tick();

  // `render`: a window of `ms` ticks from now. false: `error` says why (the name is used, a window is open, too long).
  bool begin_render(const std::string& name, std::uint32_t ms, bool stems, std::string& error);
  // A finished render by name (the stems' too), nullptr when there is none or it is still open.
  const Pcm* render(const std::string& name) const;
  // The name of the render that is open, or "" (and how many ticks it still has).
  std::string open_render(std::uint32_t* ticks_left = nullptr) const;
  // Every finished render in the order of its name (--twice compares their SHA-256, o3sim --wav writes them).
  struct Finished {
    std::string name;
    const Pcm* pcm;
  };
  std::vector<Finished> finished() const;

  // OS-E2E-9: what the save layer needs for File > EXPORT (SaveService::attach_export): the offline renderer, a ring of 32 KiB, no cycle counter (a step renders up to 48 blocks).
  app::ExportSource::Wiring export_wiring();
  // OS-E2E-9 (scenario `wav`): a finished render made by the script (a WAV file of the card). false: the name is used.
  bool add_render(const std::string& name, Pcm pcm, std::string& error);

  // For tests and the simulator: the engine's pieces.
  audio::Engine& engine() { return *engine_; }
  seq::Sequencer& sequencer() { return *sequencer_; }
  std::uint64_t blocks_rendered() const { return blocks_; }
  audio::VoicePool& voices() { return voices_; }
  audio::ModuleVoices& modules() { return *modules_; }
  // Binds the test bank (slot by slot, through app::make_sample_desc over the instruments of `project`); the pool must be quiet.
  void bind_test_bank(const model::Project& project);
  // The Module instruments of `project` that name a module this rig has (the TB-303) are bound to an instance. Returns how many. The pool must be quiet.
  unsigned bind_modules(const model::Project& project);

 private:
  class Transport;
  class Preview;
  class Tracks;
  class Perform;

  void render_block();
  void collect_stems();
  void close_window();

  std::unique_ptr<audio::VoicePool> own_voices_;
  audio::VoicePool& voices_;
  const model::Project* project_;
  std::unique_ptr<SimExportRig> export_;
  std::unique_ptr<audio::ModuleVoices> modules_;
  AlignedBlock module_scratch_;
  std::vector<AlignedBlock> module_states_;   // one aligned, zeroed state block an instance (kept for the life of the rig)
  void* module_handle_ = nullptr;                                // the host shared object (dlopen)
  std::unique_ptr<audio::TapMem> taps_;
  std::unique_ptr<audio::AmigaVoices> amiga_;
  std::unique_ptr<audio::Engine> engine_;
  std::unique_ptr<seq::Sequencer> sequencer_;
  std::unique_ptr<audio::LiveBox> live_;
  std::unique_ptr<audio::SeqEvent[]> events_;
  std::unique_ptr<audio::SendDelay> delay_;
  std::unique_ptr<std::uint32_t[]> delay_mem_;
  std::unique_ptr<audio::SendReverb> reverb_;
  std::unique_ptr<std::int16_t[]> reverb_mem_;
  std::unique_ptr<audio::MasterLimiter> limiter_;
  std::unique_ptr<app::MixBoard> mix_;
  std::unique_ptr<app::PerformBoard> perform_;
  std::unique_ptr<Transport> transport_;
  std::unique_ptr<Preview> preview_;
  std::unique_ptr<Tracks> tracks_;
  std::unique_ptr<Perform> perform_sink_;
  std::unique_ptr<std::int16_t[]> bank_pcm_;
  std::map<std::string, Pcm> renders_;  // the finished ones and the open one (open_name_); a stems render's parts are `<name>.t1` .. `.t8` and `<name>.m`
  std::string open_name_;
  std::uint32_t open_ticks_ = 0;
  bool open_stems_ = false;
  std::uint32_t open_blocks_ = 0;  // blocks fed to the open render (a stems render must end on an even number)
  std::unique_ptr<usb::StemReader> reader_;
  std::uint64_t acc_ = 0;  // millionths of a frame gathered towards the next block
  std::uint64_t blocks_ = 0;
};

}  // namespace o3os::sim
