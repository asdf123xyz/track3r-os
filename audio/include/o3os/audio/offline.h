// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The offline renderer as the export asks for it (OS-WAVEXP): a pull interface, one block of the MAIN mix at a time, so that what writes the file (app/export_source.h) knows nothing of the
// engine and what drives the engine (seq/offline.h) knows nothing of files. The audio library does not include the sequencer, so this is the plain contract between the two.
//
// A render is: open(request), then next() until the consumer has what it wants, then close(). open() may be called again after a pass without a close() in between: the engine stays with
// the export (the consumer measures the length of the render in a first pass and writes it in a second, app/export_source.h).
//
// All of it is the main loop's, one call at a time; next() does at most one block of work (about a millisecond on the device), so the caller paces itself by how many it asks for in a pass.

#include <o3os/hal/audio.h>

#include <cstdint>

namespace o3os::audio {

struct OfflineRequest {
  enum class What : std::uint8_t { Song, Pattern };
  static constexpr std::uint16_t kAllTracks = 0;       // track_mask: no override, the project's own mute and solo (0 is no choice of tracks - an empty mask is never asked for - so 0xFFFF is a real one: all 16 picked)
  static constexpr std::uint16_t kToEnd = 0xFFFF;      // row_last, order_last: to the end of the pattern, of the song
  static constexpr std::uint8_t kBothBuses = 3;        // buses: bit 0 the delay's return, bit 1 the reverb's
  What what = What::Song;
  std::uint16_t pattern = 1;  // Pattern: the pattern id, 1..255 (the sequencer's); unused for a song

  // OS-WAVEXP slice B: stems and the selection. Every field defaults to the MAIN mix as it is (the slice A render), and a request that leaves them alone changes nothing in the engine.
  //   track_mask  the tracks that sound (bit n = track n). Anything but kAllTracks (0): those tracks are unmuted and every other one is muted, whatever the project's mutes and solos are (a stem is
  //               the track alone: graph.h "Solo"). The tracks' own sends follow `dry_sends`.
  //   dry_sends   the masked-in tracks feed no send: the delay and the reverb are shut (their master mutes) and every return is silent. A track stem.
  //   buses       which send returns sound; a clear bit shuts that effect's input (the return is then silent).
  //   bus_only    the tracks' dry mix is off (the master DRY MUTE): only the returns sound. A bus stem is bus_only with one bit of `buses`.
  //   row_first / row_last   Pattern only: the first row and the last row (inclusive) played; the transport starts at row_first and stops at the end of row_last, to the sample.
  //   trim_tail   the file ends at the first silent block after the last row, with no hold (the consumer's: app/export_source.h).
  // OS-SONGSHARE S5, the slots of a song:
  //   order_first / order_last   Song only: the first and the last order position (inclusive) played; the transport starts at order_first and stops at the end of order_last, to the sample (the tail
  //               rings on after it, as after the song's end). A last position past the song is its end (as row_last is a pattern's); the defaults are the whole song, and a request that leaves them
  //               alone renders exactly what it did before they existed.
  // The mixer is told by ParamChanges to the graph (no state of the graph's own) and the project's values go back to it when the render is closed (seq/offline.h).
  std::uint16_t track_mask = kAllTracks;
  std::uint16_t row_first = 0;
  std::uint16_t row_last = kToEnd;
  std::uint8_t buses = kBothBuses;
  bool dry_sends = false;
  bool bus_only = false;
  bool trim_tail = false;
  std::uint16_t order_first = 0;
  std::uint16_t order_last = kToEnd;

  // True when the mixer is overridden (the render tells the graph and puts the project's values back).
  constexpr bool masked() const noexcept { return track_mask != kAllTracks || dry_sends || buses != kBothBuses || bus_only; }
};

class OfflineRenderer {
 public:
  enum class Step : std::uint8_t {
    Wait,    // nothing to hand over: the live render has not stood down yet, or a block that is not the render's (the settle before it) was rendered and dropped. Call again.
    Block,   // `out` holds the next hal::audio::kBlockSamples samples of the render (interleaved L, R), including the tail once the arrangement is over
    Failed,  // the render cannot go on: failure() says why; close() still has to be called
  };

  // Starts (or, after a pass, starts over) the render of `request` from silence: the voices stopped, the send effects cleared, the transport at the first step. False: refused, nothing
  // started, failure() says why (the recorder is armed, the transport runs, the project is empty).
  virtual bool open(const OfflineRequest& request) noexcept = 0;
  // `out` holds hal::audio::kBlockSamples int16.
  virtual Step next(std::int16_t* out) noexcept = 0;
  // True once the arrangement is over: the block the last next() returned is the one the song's last step or the pattern ended in (the transport stopped inside it, at some sample), or a later
  // one. What rings on after the end is the tail.
  virtual bool in_tail() const noexcept = 0;
  // How far the render is, 0..1000, of an estimate of the arrangement (it is 999 in the tail and never 1000).
  virtual unsigned progress() const noexcept = 0;
  // The engine goes back to the live render. Idempotent; always safe.
  virtual void close() noexcept = 0;
  // After a refusal or Failed: why, in a few upper-case words. "" when it has nothing to say.
  virtual const char* failure() const noexcept = 0;
  // OS-EXPNOTE: what the render stopped when it took the engine, for the strip ("STOPPED MIDI CH10 C-3"): the live notes that were held end there instead of refusing the export. "" when
  // nothing was held (and until the engine is taken). Valid until the next open(). Default: "".
  virtual const char* stopped() const noexcept { return ""; }
  // Once per main-loop pass while no pass is open: finishes what close() could not (the project's mixer pushed back to a ParamChange ring that was too full: slice B). Default: nothing.
  virtual void service() noexcept {}

 protected:
  OfflineRenderer() = default;
  ~OfflineRenderer() = default;  // not deleted through the interface: no heap, so no virtual destructor slot
  OfflineRenderer(const OfflineRenderer&) = delete;
  OfflineRenderer& operator=(const OfflineRenderer&) = delete;
};

}  // namespace o3os::audio
