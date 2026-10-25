// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The offline render of a song or a pattern (OS-WAVEXP): the sequencer and the engine run from the main loop, block after block, faster than real time, while the live render stands down.
// It implements audio::OfflineRenderer, the pull contract the export writes its file from (app/export_source.h); the engine's side of the handover is Engine::offline_* (audio/engine.h,
// OFFLINE RENDERING, which has the ownership protocol).
//
// WHAT IS RENDERED. The MAIN mix, as a live Play of the same project would make it: the same Engine::render_block path (the graph, its limiter and send effects, the voices, the SD modules, the
// Amiga voices), the mixer as it is now (mutes, solos, levels, sends: whatever the ParamChange ring has carried to the graph), the project's tempo and swing, the project's FX. NOT in it: the codec's
// line-in and the USB return (a silent block is the input), the live notes (pads, MIDI keyboard, previews: an export stops the ones held when it starts: OS-EXPNOTE), and nothing the MIDI out stage does
// (the export's sequencer has no MidiLink: PAR-063, "MIDI Out suppressed during offline export", holds by construction), the external clock (the export's timeline is its own: the project's tempo).
//
// THE SEQUENCER. The export has a Sequencer of its own (the live one is not touched: its transport, its resume point and its MIDI link stay as they were), built afresh for every pass (by the
// Wiring's make_sequencer, in memory the caller owns) over the same project and configured as the live one is (seed, Tick events) except song_loop = false (the song stops at its last pattern), pattern_once = true (a pattern is played once), emit_clock = false
// and start_lead = 0. It is stepped by Engine::play_block through audio::EventSource, like the live one, on a sample clock that starts at 0.
//
// STEMS AND THE SELECTION (slice B). A request that is `masked()` (audio/offline.h) changes the graph's mixer for the render and nothing else: the mutes and solos of the tracks and the master's
// three mutes (DRY, DELAY, REVERB) are pushed as ParamChanges with the settle, and close() pushes the project's own values back (the project is what the app keeps the mixer in, and the export is
// modal: nothing changes it meanwhile). The graph's render path is not touched. A pattern request with rows plays from row_first and stops at the end of row_last (Sequencer::Config::pattern_stop).
//
// THE SLOTS (OS-SONGSHARE S5). A song request with order_first..order_last starts at play_song(order_first) and stops at the end of order_last (Sequencer::Config::order_stop, the position after it; none
// when the range runs to the end of the list, which is then the song's own end); the progress counts from order_first. A request that leaves them at their defaults is the whole song, unchanged.
//
// A PASS. open() (the first one asks the engine to stand the live render down; Wait until it has) -> SETTLE: kSettleBlocks blocks are rendered and dropped while the send effects clear their memory
// (the DelayClear and ReverbClear ParamChanges, their return silent meanwhile) and every voice has stopped -> RUN: the transport starts and every block is handed over -> the arrangement ends
// (the sequencer stops inside a block: in_tail()) and the blocks go on, the tail, for as long as the consumer asks. Two passes over the same request render the same blocks (the sequencer
// restarts from its seed, the effects from silence); what they cannot make equal is state the engine keeps across a pass: the limiter's follower after a loud pass, a module instance's own memory
// (the TB-303's oscillator phase, say): a module is rendered exactly as a live Play would render it from where the instance is. docs/formats are not touched: nothing is stored.
//
// REAL TIME. This runs in the main loop; it renders on the main loop's time and never on the audio's. Each next() renders one block at most (about 0.4 to 1.0 ms of the 1.45 ms a block lasts, by
// the governor's own budget of 60 %). No allocation, no blocking, no double.

#include <o3os/audio/engine.h>
#include <o3os/audio/events.h>
#include <o3os/audio/offline.h>
#include <o3os/audio/send_delay.h>
#include <o3os/audio/send_reverb.h>
#include <o3os/model/project.h>
#include <o3os/seq/engine_link.h>
#include <o3os/seq/sequencer.h>

#include <cstdint>

namespace o3os::seq {

namespace detail {
constexpr std::uint32_t ceil_div(std::uint32_t a, std::uint32_t b) noexcept { return (a + b - 1u) / b; }
}  // namespace detail

class OfflineRender final : public audio::OfflineRenderer {
 public:
  static constexpr std::uint32_t kBlock = hal::audio::kBlock;
  // The blocks rendered and dropped before the transport starts: the send effects clear kClearFrames / kClearSamples a block, and the slowest of them takes this many; three more for the
  // limiter's delay line and the strips' smoothers to settle on silence.
  static constexpr std::uint32_t kSettleBlocks = (detail::ceil_div(audio::SendDelay::kRingFrames, audio::SendDelay::kClearFrames) > detail::ceil_div(audio::SendReverb::kMemSamples, audio::SendReverb::kClearSamples)
                                                      ? detail::ceil_div(audio::SendDelay::kRingFrames, audio::SendDelay::kClearFrames)
                                                      : detail::ceil_div(audio::SendReverb::kMemSamples, audio::SendReverb::kClearSamples)) +
                                                 3u;
  // Blocks the sequencer has to show it is playing in, before the render gives up ("TRANSPORT DID NOT START").
  static constexpr std::uint32_t kStartBlocks = 4;

  struct Wiring {
    audio::Engine* engine = nullptr;
    const model::Project* project = nullptr;        // the document: the same the live sequencer reads
    const Sequencer* live = nullptr;                // its Config (the seed, the Tick events) is the export's; null: the defaults
    audio::SeqEvent* events = nullptr;              // the export's event buffer, `event_cap` events (audio::kMaxEventsPerBlock)
    std::uint32_t event_cap = 0;
    bool (*live_running)() noexcept = nullptr;      // the live render is running (null: it is not: a host test, a boot with no audio)
    // Builds a Sequencer over the project, in place, in memory of the caller's (SDRAM on the device), and returns it: called for every pass, over the same memory, so that a pass starts
    // from a sequencer that has never run. (A layer of this tree does not construct in place: the composition root does, and a test.)
    Sequencer* (*make_sequencer)(void* ctx) noexcept = nullptr;
    void* make_ctx = nullptr;
  };

  explicit OfflineRender(const Wiring& wiring) noexcept : w_(wiring) {}
  OfflineRender(const OfflineRender&) = delete;
  OfflineRender& operator=(const OfflineRender&) = delete;

  bool open(const audio::OfflineRequest& request) noexcept override;
  Step next(std::int16_t* out) noexcept override;
  bool in_tail() const noexcept override { return tail_; }
  unsigned progress() const noexcept override;
  void close() noexcept override;
  const char* failure() const noexcept override { return failure_; }
  const char* stopped() const noexcept override { return stopped_text_; }
  // The project's mixer is still owed to the graph (close() found the ParamChange ring too full for all of it): service() pushes it when the ring has room, once per main-loop pass.
  void service() noexcept override;
  bool restore_owed() const noexcept { return mixer_set_ && state_ == State::Closed; }

  // For tests and `diag`: where the render is.
  bool entered() const noexcept { return w_.engine != nullptr && w_.engine->offline_entered(); }
  std::uint32_t blocks() const noexcept { return blocks_; }
  const Sequencer& sequencer() const noexcept { return *seq_; }

 private:
  enum class State : std::uint8_t { Closed, Requested, Settling, Running, Failed };

  bool refuse(const char* why) noexcept {
    failure_ = why;
    return false;
  }
  void note_stopped() noexcept;  // OS-EXPNOTE: fills stopped_text_ from what Engine::offline_enter stopped
  void build_sequencer() noexcept;
  void start_pass() noexcept;
  // The engine's event source over the export's sequencer: the sequencer's own functions, with a look at the events: the Stopped event is the arrangement's end, to the sample.
  static std::uint32_t step_events(void* self, std::uint64_t block_start, std::uint32_t n, audio::SeqEvent* out, std::uint32_t cap) noexcept;
  static bool param_event(void* self, const core::ParamChange& change) noexcept;
  static std::uint32_t tempo_event(void* self) noexcept;
  void update_progress() noexcept;
  void render(std::int16_t* out) noexcept;
  std::uint32_t steps_of(std::uint32_t pattern_id) const noexcept;
  std::uint32_t last_slot() const noexcept;  // OS-SONGSHARE S5: the last order position a Song request plays
  void push_clears() noexcept;
  // OS-WAVEXP slice B: the mixer of a stem or a selection (audio::OfflineRequest), told to the graph by ParamChanges, and the project's values put back. False: the ring could not take them all.
  bool push_mixer(bool stem) noexcept;

  Wiring w_;
  State state_ = State::Closed;
  audio::OfflineRequest request_{};
  const char* failure_ = "";        // a literal, or why_
  char stopped_text_[32] = {};           // "STOPPED MIDI CH10 C-3 +2" (the longest, "STOPPED MIDI CH16 c10 +11", is 25 characters)
  Sequencer* seq_ = nullptr;
  audio::EventSource source_{};
  std::uint32_t settle_left_ = 0;
  std::uint32_t blocks_ = 0;        // blocks handed over in this pass
  std::uint32_t run_blocks_ = 0;    // blocks rendered since the transport was started
  bool saw_playing_ = false;
  bool stopped_ = false;  // the block just rendered held the Stopped event (set inside the event source, read after the render)
  bool tail_ = false;
  bool mixer_set_ = false;          // the graph's strips and master mutes are the request's, not the project's: close() puts the project's back
  std::uint32_t total_steps_ = 1;   // the arrangement, in steps (an estimate: the Order jump and Break FX move it)
  std::uint32_t last_order_ = 0xFFFFFFFFu;
  std::uint32_t prefix_ = 0;        // steps of the order positions before last_order_
  unsigned best_ = 0;               // progress is monotonic
  alignas(8) std::int16_t silent_in_[hal::audio::kBlockSamples] = {};
  hal::audio::BlockReport report_{};
};

}  // namespace o3os::seq
