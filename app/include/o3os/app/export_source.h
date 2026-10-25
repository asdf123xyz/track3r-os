// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The source of an exported WAV (OS-WAVEXP): the MAIN mix of the song or of a pattern, rendered offline by an audio::OfflineRenderer and written as a stereo 16-bit PCM WAV at 44,100 Hz.
// It is a SaveSource of the atomic save (app/atomic_save.h): the file is written to a temporary name, read back, and only then renamed to its final one, so a cancel, a failure or a pulled
// card leaves no file under the final name.
//
// TWO PASSES. The WAV header holds the length of the file, and the card's writer only appends (hal/storage_write.h): the length has to be known before the first byte is written. The length
// of a song or a pattern with its tail is not known until it has been rendered (the tail is what the delay and the reverb ring on for), so the render runs twice:
//   MEASURE  the render runs and its blocks are counted and dropped, nothing is written (the temporary file stays empty). The arrangement ends (OfflineRenderer::in_tail()) and the render goes
//            on until the output has been silent (kSilenceLsb) for kHoldBlocks blocks, longer than any gap between the repeats of the longest delay (the ring is 4.0 s); the file ends at the last
//            sound (a block whose peak is above the silence) or at the end of the arrangement, whichever is later. A tail that never dies is cut kMaxTailBlocks after the arrangement's end.
//   WRITE    the render starts over from silence (the engine stays with the export) and its first N frames are the file, N the length measured. Two passes render the same blocks (the sequencer
//            restarts from its seed, the effects from silence: tests/seq_offline_test.cpp); where they could differ (the limiter's follower, a module's own state) the file still has exactly N
//            frames, because the render goes on for as long as it is asked for blocks.
// New Community OS behaviour (an export has a measuring pass). The price is the time: a song takes about twice what one pass would; the open item is a single pass with
// the header written last (a rewrite of the first bytes of the temporary file in the card's writer), which is a change of the write contract and a decision of its own.
//
// THE RATE. The engine runs at 44,100 Hz by default (core::kRefCyclesPerSample, D-OS-8) and the converter is the identity. With -DO3OS_RATE_44100=OFF the engine is at 44,117.647 Hz and the file says 44,100: the blocks go through a cubic resampler (RateConverter) so that the file's duration, and with
// it the tempo and the pitch, are the live ones.
//
// STEMS (slice B). A set of stems is several files, one job each (app/save_service.h), and every one is the same length, the length of the MIX: the first job's measuring pass renders the plain
// MIX (`measure`) and the other jobs are told that length (`known_frames`) and only write. So a set of M files is M + 1 passes, and each job reports its share of the whole bar
// (`unit_base` passes done before it, of `units`). TRIM_TAIL (a selection, PAR-151): the measuring pass ends at the first silent block after the arrangement, with no hold.
//
// RENDERING IS PACED HERE. A pass of the main loop gives the source one step; a step renders for about kSliceUs (by the cycle counter it is given, or kMaxBlocksPerStep blocks when it has none),
// into the staging ring (SDRAM), and then writes what the ring holds, at most `byte_budget` bytes, to the card. No card I/O is done by the render and none by the renderer: the writes are the
// save job's, a chunk a step. The renderer is the main loop's too; the live render stands down for as long as the source holds it (from the first step to end()).
//
// Main loop only. The staging ring is the platform's (SDRAM); kRingBytes is its size.

#include <o3os/app/atomic_save.h>
#include <o3os/audio/offline.h>
#include <o3os/hal/audio.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

// Engine frames (44,100 Hz by default; 44,117.647 Hz with -DO3OS_RATE_44100=OFF) to the file's 44,100 Hz, or the identity when they are the same.
class RateConverter {
 public:
  static constexpr std::uint32_t kInFrames = hal::audio::kBlock;
  // The output frames of one block of kInFrames input frames, at most: the ratio is not below 1, so a block of 64 makes 64 at most.
  static constexpr std::uint32_t kMaxOutFrames = kInFrames;
  static constexpr std::uint32_t kOutRate = 44100;

  void reset() noexcept;
  // Takes `n` stereo frames (interleaved L, R) and gives the output frames they complete: `out` gets them (interleaved, kMaxOutFrames at most for n = kInFrames), or nothing is written when
  // `out` is null (counting only). Returns how many. Two frames of the input are held back (the interpolation looks two frames ahead).
  std::uint32_t feed(const std::int16_t* in, std::uint32_t n, std::int16_t* out) noexcept;
  // True when the engine's rate is the file's: feed() copies.
  static bool identity() noexcept;

 private:
  std::int16_t h_[4][2] = {};   // the last four input frames, oldest first
  std::uint64_t fed_ = 0;       // input frames taken so far
  std::uint64_t pos_ = 0;       // the next output frame's place on the input's time line, in 1/den of an input frame
};

class ExportSource final : public SaveSource {
 public:
  static constexpr std::uint32_t kBlock = hal::audio::kBlock;
  static constexpr std::size_t kRingBytes = 32u * 1024u;
  static constexpr std::int32_t kSilenceLsb = 4;                        // a block whose peak is at most this is silent (-78 dBFS)
  static constexpr std::uint32_t kHoldBlocks = 3500;                    // about 5.1 s of silence ends the tail (the delay's ring is 4.0 s)
  static constexpr std::uint32_t kMaxTailBlocks = 20700;                // a tail that does not die is cut 30 s after the arrangement's end
  static constexpr std::uint32_t kMaxFrames = 0x0FFFFFFFu;              // 100 minutes: the file stays under 1 GiB
  static constexpr std::uint32_t kMaxBlocksPerStep = 48;                // when there is no cycle counter (and as a bound on one)
  static constexpr std::uint32_t kSliceUs = 8000;                       // what one step may render for, of the main loop's 16 ms pass
  static constexpr std::uint32_t kMaxWaits = 5000;                      // consecutive "nothing yet" answers before the render is given up (the live render did not stand down)
  static constexpr std::size_t kHeaderBytes = 44;
  static constexpr std::size_t kMaxBlockOutBytes = (RateConverter::kMaxOutFrames + 1u) * 4u;

  struct Wiring {
    audio::OfflineRenderer* renderer = nullptr;
    std::uint8_t* ring = nullptr;               // kRingBytes of SDRAM, 4-byte aligned
    std::size_t ring_bytes = 0;
    std::uint32_t (*cycles)() noexcept = nullptr;  // a free-running cycle counter (null: only kMaxBlocksPerStep paces a step)
    std::uint32_t slice_cycles = 0;                // kSliceUs in its units
  };

  void attach(const Wiring& wiring) noexcept { w_ = wiring; }
  bool attached() const noexcept { return w_.renderer != nullptr && w_.ring != nullptr && w_.ring_bytes >= kMaxBlockOutBytes * 2u; }
  // The request for the next save. False when the source is not attached.
  bool set(const audio::OfflineRequest& request) noexcept;
  // One file of a set of stems: `write` is what the file holds; `measure` is the request the length is measured with (the MIX), unless `known_frames` is not 0, the length of the set (a file
  // after the first: no measuring pass). `unit_base` passes of the set's `units` are done before this job's first.
  bool set_stem(const audio::OfflineRequest& write, const audio::OfflineRequest& measure, std::uint32_t known_frames, unsigned unit_base, unsigned units) noexcept;

  void begin(core::ByteSink& sink) noexcept override;
  formats::Progress step(std::size_t byte_budget) noexcept override;
  const char* failure() const noexcept override { return failure_; }
  // OS-EXPNOTE: the live notes the render stopped when it took the engine ("STOPPED MIDI CH10 C-3"), "" when none (audio::OfflineRenderer::stopped). App::poll_export keeps it for the strip.
  const char* stopped() const noexcept { return w_.renderer != nullptr ? w_.renderer->stopped() : ""; }
  std::uint32_t size_hint() const noexcept override { return frames_known_ ? static_cast<std::uint32_t>(kHeaderBytes + static_cast<std::uint64_t>(length_frames_) * 4u) : 0u; }
  int progress_permille() const noexcept override;
  void end() noexcept override;
  // Once a pass (SaveService::poll): lets the renderer finish what its close() could not.
  void service() noexcept {
    if (w_.renderer != nullptr) {
      w_.renderer->service();
    }
  }

  // For tests and `diag`.
  enum class Phase : std::uint8_t { Idle, Measure, Write, Drain, Done, Failed };
  Phase phase() const noexcept { return phase_; }
  std::uint32_t length_frames() const noexcept { return length_frames_; }  // the file's length, known once the measuring pass has ended
  std::uint32_t engine_blocks() const noexcept { return blocks_; }

 private:
  bool open_pass(const audio::OfflineRequest& request) noexcept;
  void make_header() noexcept;
  void fail(const char* why) noexcept;
  bool render_slice(bool write) noexcept;
  void take_block(bool write) noexcept;
  bool drain(std::size_t byte_budget) noexcept;
  std::size_t ring_free() const noexcept { return w_.ring_bytes - used_; }

  Wiring w_;
  audio::OfflineRequest request_{};     // what the file holds
  audio::OfflineRequest measure_rq_{};  // what the length is measured with (the same for a plain export)
  std::uint32_t known_frames_ = 0;      // a stem after the first: the set's length, no measuring pass
  unsigned unit_base_ = 0;              // passes of the set done before this job's first, of units_
  unsigned units_ = 2;
  core::ByteSink* sink_ = nullptr;
  Phase phase_ = Phase::Idle;
  bool opened_ = false;       // the renderer is ours (open() was called and close() has not been)
  bool frames_known_ = false;
  const char* failure_ = "";
  RateConverter conv_;
  alignas(8) std::int16_t block_[hal::audio::kBlockSamples] = {};
  alignas(4) std::int16_t out_block_[RateConverter::kMaxOutFrames * 2u] = {};
  // the measuring pass
  std::uint32_t blocks_ = 0;          // engine blocks handed over in this pass
  std::uint32_t out_frames_ = 0;      // output frames the converter has made in this pass
  std::uint32_t end_frames_ = 0;      // ... when the arrangement ended
  std::uint32_t loud_frames_ = 0;     // ... after the last block that held a sound
  std::uint32_t quiet_blocks_ = 0;
  std::uint32_t tail_blocks_ = 0;
  bool tail_seen_ = false;
  std::uint32_t waits_ = 0;
  unsigned measure_pm_ = 0;
  // the writing pass
  std::uint32_t length_frames_ = 0;   // N: the file's length in frames
  std::uint32_t written_frames_ = 0;   // output frames put in the ring
  std::uint8_t header_[kHeaderBytes] = {};
  std::size_t header_sent_ = 0;
  std::size_t head_ = 0;               // the ring: where the next byte is put, where the next is taken, how many it holds
  std::size_t tail_ = 0;
  std::size_t used_ = 0;
  std::uint32_t bytes_out_ = 0;        // bytes given to the sink in the writing pass
};

}  // namespace o3os::app
