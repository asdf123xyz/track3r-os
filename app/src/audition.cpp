// SPDX-License-Identifier: GPL-3.0-or-later
// The Sample load audition (OS-FU2): F5 held plays the first seconds of the WAV under the cursor through the engine's live notes, on a descriptor that is no instrument's
// (SampleBank, "The audition"; audio::kPreviewSlot). New Community OS behaviour: the file plays while the key is held and stops on the
// release, and (OS-BUG-PREVLAT) follows the cursor while it is held.
//
// The order of things (every step is a pass of the main loop, none waits):
//   F5 down  the action marks the key held and asks (WavView::Go::Preview); poll_wav calls start_audition(): Wait.
//   Wait     the quiet window (audition_quiet: no live note queued or held, no voice reading the descriptor) opens: the PreviewJob starts reading into the platform's buffer. Load.
//   Load     the streamed start: when enough is read and the read will stay ahead of the head, the descriptor is bound over the frames planned and the note starts while the job goes on
//            (Play; the unread tail is zeroed ahead of the head). Else the job ends (report_jobs -> finish_audition): the descriptor is bound when the window is still open, else Bind waits for it. Play.
//   stop     the key's release, leaving the screen, a lost input: a Play sends the note's release (the voice ends in its 256-sample release), a Load is cancelled.
//   follow   a move of the cursor with F5 held is a stop and a new start at Wait on the file now under it (one read at a time; a Wait only changes its file).
//            A new start begins at Wait again, so the buffer is never rewritten under a note that is still ringing out.
// The buffer's one writer is the job (main loop); the render (SWI) only reads it, aligned int16 values that cannot tear, and the descriptor is written in the quiet window only.
#include <o3os/app/app.h>

#include "sample_text.h"

#include <o3os/app/record_view.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/screens/sample_load_screen.h>
#include <o3os/app/wave_math.h>
#include <o3os/audio/voices.h>

#include <cstring>

namespace o3os::app {

namespace {
constexpr std::uint32_t kStreamMinFrames = 4096;  // the streamed start needs at least this many frames planned and written (a shorter clip starts at Done)
constexpr std::uint32_t kZeroChunk = 8192;        // the least of the tail zeroed a pass: 16 KB of SDRAM, about 0.5 ms; the head moves under 3k samples a normal 16 ms pass at 96 kHz
constexpr std::uint32_t kZeroChunkMax = 32768;    // the most a pass zeroes (64 KB, about 2 ms): a slow pass asks for more (zero_chunk), up to a 170 ms pass at 96 kHz with a 2x margin

// The samples to zero ahead of the head after `since_ms` without a zero: the head moves `rate` samples a second at most (a pitch up of the preview note is within the 2x margin).
std::uint32_t zero_chunk(std::uint64_t rate, std::uint64_t since_ms) noexcept {
  const std::uint64_t need = rate * since_ms * 2u / 1000u;
  return need < kZeroChunk ? kZeroChunk : (need > kZeroChunkMax ? kZeroChunkMax : static_cast<std::uint32_t>(need));
}
}  // namespace

bool App::audition_quiet() const noexcept {
  const bool live_idle = preview_ == nullptr || preview_->live_idle();  // asked first, as the bank's handover does
  return samples_ != nullptr && samples_->preview_quiet(live_idle);
}

void App::start_audition() noexcept {
  stop_audition();
  if (!state_.wav.audition_held) {
    return;  // the key was let go before this pass: nothing to play
  }
  aud_ = Aud::Wait;
  aud_pos_ = state_.wav.list.pos();
}

void App::stop_audition() noexcept {
  if (preview_slot_ >= 0) {
    session_.jobs.cancel(static_cast<unsigned>(preview_slot_));  // closes the file; report_jobs releases the slot and finish_audition sees it is no longer wanted
  }
  if (aud_ == Aud::Play && preview_ != nullptr) {
    preview_->audition(false, 0);
  }
  aud_ = Aud::Idle;
}

// The listing entry `pos` is a WAV the audition can read (a folder, a .wt and the other types are not: those say so at the key's press, and a cursor that moves onto one just stops).
bool App::audition_target(int pos) const noexcept {
  const DirIndex& index = files_.index();
  return pos >= 0 && static_cast<unsigned>(pos) < index.count() && !index.is_dir(static_cast<unsigned>(pos)) &&
         DirScan::ends_with_ci(index.name(static_cast<unsigned>(pos)), ".wav");
}

// The zero tail of a streamed start (OS-BUG-PREVLAT): the samples from the read's frontier to the frames planned hold zeros, not the last preview's audio, whenever the head gets there
// before the read does. At most `chunk` samples a call, from the frontier or from the zeroed mark, whichever is later: the importer writes at the frontier on, so a zero that it
// overwrites was never read. Main loop, before the job's slice of the pass; the render only reads the buffer.
void App::zero_audition_tail(std::uint32_t chunk) noexcept {
  const PreviewJob& job = samples_->preview_job();
  const std::uint32_t planned = job.planned();
  const std::uint32_t from = job.written() > aud_zeroed_ ? job.written() : aud_zeroed_;
  if (from >= planned || from >= samples_->preview_capacity()) {
    return;
  }
  const std::uint32_t to = planned - from > chunk ? from + chunk : planned;
  std::memset(samples_->preview_pcm() + from, 0, static_cast<std::size_t>(to - from) * sizeof(std::int16_t));
  aud_zeroed_ = to;
}

void App::poll_audition() noexcept {
  if (state_.screen != ScreenId::SampleLoad || files_for_ != FilesFor::Wavs || !state_.wav.audition_held) {
    if (aud_ != Aud::Idle) {
      stop_audition();  // the key's release, another screen
    }
    return;
  }
  const int pos = state_.wav.list.pos();
  if (pos != aud_pos_) {  // the preview follows the cursor while F5 is held (OS-BUG-PREVLAT)
    if (aud_ != Aud::Wait) {
      stop_audition();  // cancels the one read and lets the note go; the new file starts at Wait, when the window is open again
      aud_ = Aud::Wait;
    }
    aud_pos_ = pos;  // (in Wait nothing started: the file under the cursor now is the one)
    if (!audition_target(pos)) {
      aud_ = Aud::Idle;  // a folder, a .wt: silence until the cursor reaches a WAV
    }
  }
  if (aud_ == Aud::Idle) {
    return;
  }
  if (aud_ == Aud::Wait && preview_slot_ < 0 && audition_quiet()) {
    const auto fail = [this](const char* text) {
      (void)state_.notify.upsert(core::Severity::Warn, text, "sample", ViewState::kMessageMs);
      aud_ = Aud::Idle;
      dirty_ = true;
    };
    const DirIndex& index = files_.index();
    char path[hal::storage::kPathMax + 1];
    if (storage_ == nullptr || !audition_target(aud_pos_)) {
      aud_ = Aud::Idle;
      return;
    }
    if (!path_join(path, sizeof path, files_.folder(), index.name(static_cast<unsigned>(aud_pos_)))) {
      fail("THE PATH IS TOO LONG");
      return;
    }
    if (samples_->preview_job().begin(*storage_, path, samples_->preview_pcm(), samples_->preview_capacity(),
                                      index.stereo(static_cast<unsigned>(aud_pos_)) ? state_.wav.channel : std::uint8_t{0}) != hal::storage::Status::Ok) {
      fail("CANNOT OPEN THE FILE");
      return;
    }
    const int slot = session_.jobs.start(samples_->preview_job());
    if (slot < 0) {
      samples_->preview_job().cancel();
      fail("TOO MANY JOBS");
      return;
    }
    preview_slot_ = slot;
    shown_percent_[slot] = -1;
    aud_zeroed_ = 0;
    aud_t0_ms_ = clock_.uptime_ms();
    aud_ = Aud::Load;
  }
  if (aud_ == Aud::Load && preview_slot_ >= 0) {
    // The streamed start: once enough is read, and the read will keep ahead of the head to the end, the note starts while the job goes on. Otherwise the start waits for Done (below).
    // A read that is 8 KB of file a 16 ms pass makes at most 128k frames a second (16-bit stereo), so the start waits for max(4096, rate / 8) frames and for the projection:
    // the time the rest needs at the speed seen so far (frames written over the time since the read began) is at most 3/4 of the time the head needs for the whole clip.
    const PreviewJob& job = samples_->preview_job();
    const std::uint64_t planned = job.planned();
    const std::uint64_t written = job.written();
    const std::uint64_t rate = job.sample_rate();
    const std::uint64_t now = clock_.uptime_ms();
    const std::uint64_t e = now > aud_t0_ms_ ? now - aud_t0_ms_ : 1u;
    const std::uint64_t need = rate / 8u > kStreamMinFrames ? rate / 8u : kStreamMinFrames;
    if (planned >= kStreamMinFrames && written >= need && (planned - written) * e * rate * 4u <= planned * 1000u * written * 3u && audition_quiet()) {
      zero_audition_tail(kZeroChunk);  // the first stretch of the tail before the head can reach it
      aud_zero_ms_ = now;
      const bool live_idle = preview_ == nullptr || preview_->live_idle();
      if (samples_->preview_bind(job.planned(), job.sample_rate(), live_idle)) {
        if (preview_ != nullptr) {
          preview_->audition(true, static_cast<unsigned>(state_.pattern.track()));
        }
        aud_ = Aud::Play;
      }
    }
  } else if (aud_ == Aud::Play && preview_slot_ >= 0) {
    const std::uint64_t now = clock_.uptime_ms();  // ahead of the head, which a 16 ms pass moves 1.5k samples at 96 kHz; the chunk grows with a slower pass
    zero_audition_tail(zero_chunk(samples_->preview_job().sample_rate(), now > aud_zero_ms_ ? now - aud_zero_ms_ : 0u));
    aud_zero_ms_ = now;
  }
  if (aud_ == Aud::Bind && audition_quiet()) {
    const bool live_idle = preview_ == nullptr || preview_->live_idle();
    if (samples_->preview_bind(aud_frames_, aud_rate_, live_idle)) {
      if (preview_ != nullptr) {
        preview_->audition(true, static_cast<unsigned>(state_.pattern.track()));
      }
      aud_ = Aud::Play;
    }
  }
}

// The read ended (a Done or a Failed one; a cancelled one is no news).
void App::finish_audition(core::SlotState state) noexcept {
  if (aud_ == Aud::Play && samples_ != nullptr) {
    // The streamed start was made: the note stays, whatever the end of the read. A failed or truncated one leaves what was read to play and the rest of the planned frames silent
    // (the strip says why); a Done one has every frame in place.
    if (state != core::SlotState::Done) {
      zero_audition_tail(samples_->preview_capacity());
    }
    return;
  }
  if (aud_ != Aud::Load) {
    return;  // stopped meanwhile
  }
  if (state != core::SlotState::Done || samples_ == nullptr) {
    aud_ = Aud::Idle;
    return;
  }
  aud_frames_ = samples_->preview_job().frames();
  aud_rate_ = samples_->preview_job().sample_rate();
  aud_ = Aud::Bind;  // poll_audition binds it in this same pass or the next, when the window is open
}

// ---- The held Preview of the sample screens and the playhead (OS-FU3) ----------------------------------------------------------------------------------------------
// Preview (F1 of Sample Playback, the take's preview on Sample Recorder) is note 60 at the instrument's velocity, on at the press and off at the release, and the progress
// cursor follows the voice the screen last fired, from pads, MIDI or Preview alike. Start is exactly that, a live note; Over (new OS behaviour: Shift + Preview, "from where we are") and Take (the Record take, whose audio is
// not an instrument's) play arena audio on the audition's descriptor, which the quiet window of the Sample load audition guards the same way.

namespace {
constexpr std::uint8_t kPreviewNote = 60;  // the note of the Preview key (MIDI note 60: the sample at its own rate, voices.h kRootNote)
constexpr std::uint32_t kHeadMs = 33;      // the playhead is read about 30 times a second: it is a picture
constexpr std::uint8_t kPlayWavetable = 6;

// The play modes that show a progress cursor: not the slice ones and not the wavetable.
constexpr bool shows_progress(std::uint8_t play_mode) noexcept { return play_mode <= 3u || play_mode >= 7u; }
}  // namespace

void App::stop_hold() noexcept {
  if (hold_ == Hold::PlayStart && preview_ != nullptr) {
    preview_->hold_note(false, static_cast<unsigned>(state_.pattern.track()), hold_instrument_, kPreviewNote);
  } else if (hold_ == Hold::PlayOver && preview_ != nullptr) {
    preview_->audition(false, 0);
  }
  hold_ = Hold::Idle;
}

void App::poll_hold() noexcept {
  HeldPreview& h = state_.hold;
  const bool here = state_.screen == ScreenId::Wave || state_.screen == ScreenId::SampleEdit || state_.screen == ScreenId::Record;
  if (hold_ != Hold::Idle && (h.button == HeldPreview::kNone || !here)) {
    stop_hold();  // the key's release, another screen
  }
  if (!here || h.button == HeldPreview::kNone) {
    h.button = HeldPreview::kNone;
    h.pending = false;
    return;
  }
  if (h.pending && hold_ == Hold::Idle) {
    h.pending = false;
    hold_ = h.kind == HeldPreview::Start ? Hold::WaitStart : Hold::WaitOver;
    hold_instrument_ = static_cast<std::uint8_t>(state_.instrument < 1 ? 1 : (state_.instrument > static_cast<int>(audio::kSampleSlots) ? audio::kSampleSlots : state_.instrument));
  }
  if (hold_ == Hold::WaitStart) {
    if (preview_ != nullptr) {
      preview_->hold_note(true, static_cast<unsigned>(state_.pattern.track()), hold_instrument_, kPreviewNote);
    }
    hold_ = Hold::PlayStart;
  } else if (hold_ == Hold::WaitOver && samples_ != nullptr && audition_quiet()) {
    const std::int16_t* pcm = nullptr;
    std::uint32_t frames = 0;
    std::uint32_t rate = 0;
    if (h.kind == HeldPreview::Take) {
      const Recorder* rec = model_.rec;
      if (rec != nullptr && rec->take_data() != nullptr && rec->trim_end() > rec->trim_start()) {
        pcm = rec->take_data() + rec->trim_start();
        frames = rec->trim_end() - rec->trim_start();
      }
    } else {
      const SampleBank::Pcm whole = samples_->pcm(hold_instrument_ - 1u);
      const model::Instrument* in = model_.project != nullptr ? model_.project->instrument(hold_instrument_ - 1u) : nullptr;
      if (whole.data != nullptr && h.from < whole.frames) {
        pcm = whole.data + h.from;
        frames = whole.frames - h.from;
        rate = in != nullptr ? in->sample_rate : 0u;
      }
    }
    const bool live_idle = preview_ == nullptr || preview_->live_idle();
    if (pcm != nullptr && frames != 0u && samples_->preview_bind_over(pcm, frames, rate, live_idle)) {
      if (preview_ != nullptr) {
        preview_->audition(true, static_cast<unsigned>(state_.pattern.track()));
      }
      hold_ = Hold::PlayOver;
    } else if (pcm == nullptr || frames == 0u) {
      hold_ = Hold::Idle;  // nothing to play
    }
  }
}

// The pads of the Record screen (OS-REC2; record_view.h has the layout): a pad plays the take under review through the preview descriptor, bound over the take's trim
// window by the same quiet-window rule as the Preview key above. record_pad() starts the note at once when the descriptor already is that window; otherwise the pad waits here (`pending`)
// until no live note is queued or held and no voice reads the descriptor, then the window is bound and the waiting pads sound. A trim moved after the binding is a different window: the next
// press waits for the pads that sound to be let go, then rebinds. Leaving the screen, discarding or saving the take ends the notes (the bank lets go of a discarded take's memory at the
// next Recorder::poll in Idle, but not while a note still reads it: take_drop(live_idle), and record.toggle refuses a new take while one does).
void App::poll_take_pads() noexcept {
  TakePads& tp = state_.take_pads;
  if (!tp.any()) {
    return;
  }
  const Recorder* rec = model_.rec;
  const bool here = state_.screen == ScreenId::Record && rec != nullptr && rec->state() == Recorder::State::Review && rec->take_data() != nullptr && rec->trim_end() > rec->trim_start();
  if (!here) {
    if (preview_ != nullptr) {
      for (unsigned pad = 0; pad < kPadCount; ++pad) {
        if (((tp.down >> pad) & 1u) != 0u) {
          preview_->pad_up(pad);
        }
      }
    }
    tp = TakePads{};
    return;
  }
  if (tp.pending == 0u || samples_ == nullptr) {
    return;
  }
  const std::int16_t* pcm = rec->take_data() + rec->trim_start();
  const std::uint32_t frames = rec->trim_end() - rec->trim_start();
  if (!samples_->preview_is_over(pcm, frames)) {
    const bool live_idle = preview_ == nullptr || preview_->live_idle();
    if (!samples_->preview_quiet(live_idle) || !samples_->preview_bind_over(pcm, frames, 0, live_idle)) {
      return;  // a note still reads the old window: wait for it
    }
  }
  for (unsigned pad = 0; pad < kPadCount; ++pad) {
    if (((tp.pending >> pad) & 1u) != 0u) {
      if (preview_ != nullptr) {
        preview_->take_pad_down(pad, static_cast<unsigned>(state_.pattern.track()), rec_pad_note(pad));
      }
      tp.down |= std::uint64_t{1} << pad;
    }
  }
  tp.pending = 0;
}

// The playhead: the live voice's word, resolved to a frame of the sample in view (hidden in the slice and wavetable modes).
void App::poll_head() noexcept {
  const bool here = state_.screen == ScreenId::Wave || state_.screen == ScreenId::SampleEdit || state_.screen == ScreenId::Record;
  LiveHead head;
  if (!here) {
    if (model_.head.on) {
      model_.head = head;
      dirty_ = true;
    }
    return;
  }
  const std::uint64_t now = clock_.uptime_ms();
  if (now - head_ms_ < kHeadMs) {
    return;
  }
  head_ms_ = now;
  const std::uint32_t w = preview_ != nullptr ? preview_->live_position() : 0u;
  if ((w & audio::VoicePool::kLivePlaying) != 0u) {
    const unsigned slot = ((w >> 24) & 0x7Fu) - 1u;
    const std::uint32_t pos = w & 0xFFFFFFu;
    if (slot == audio::kPreviewSlot) {
      if (hold_ == Hold::PlayOver) {  // Shift + Preview, or the take: where the audition started, plus how far it has come
        head.on = true;
        head.slot = state_.screen == ScreenId::Record ? static_cast<std::uint8_t>(audio::kPreviewSlot) : static_cast<std::uint8_t>(hold_instrument_ - 1u);
        head.frame = state_.hold.from + pos;
      } else if (state_.screen == ScreenId::Record && state_.take_pads.down != 0u && model_.rec != nullptr) {  // a pad playing the take (OS-REC2): the window starts at the trim start
        head.on = true;
        head.slot = static_cast<std::uint8_t>(audio::kPreviewSlot);
        head.frame = model_.rec->trim_start() + pos;
      }
    } else if (state_.screen != ScreenId::Record && slot < audio::kSampleSlots && samples_ != nullptr && model_.project != nullptr) {
      const model::Instrument* in = model_.project->instrument(slot);
      const SampleBank::Pcm whole = samples_->pcm(slot);
      if (in != nullptr && whole.frames != 0u && shows_progress(in->sampler.play_mode) && in->sampler.play_mode != kPlayWavetable) {
        head.on = true;
        head.slot = static_cast<std::uint8_t>(slot);
        head.frame = point_to_frame(in->sampler.start, whole.frames) + pos;  // the descriptor's data begins at the instrument's start point
      }
    }
  }
  if (head != model_.head) {
    model_.head = head;
    dirty_ = true;
  }
}

}  // namespace o3os::app
