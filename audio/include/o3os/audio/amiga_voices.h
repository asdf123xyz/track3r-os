// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Amiga voices (OS-005h; radar card `amiga-playback-mode`, docs/formats/med.md section 5): the instruments whose playback engine is Amiga (SampleDesc::engine ==
// kEngineAmiga) play through a Paula channel (audio/paula.h) instead of the sampler voices, driven by the MED tick processor (audio/med_tick.h) from the sequencer's events.
// The Engine hands this pool the events of its instruments (audio/engine.h) and takes the track's block from it where a sampler voice would be; routing, level, pan, sends and
// the master stay the graph's (the importer's hard pan L R R L is the strip's pan, which the Engine gives the track where the note starts).
//
//   ONE VOICE A TRACK, as the sampler's: a note on a track takes that track's voice over (the channel keeps its held sample and BLEP ring, so the step to the new note is band
//   limited too); a track with none takes a free voice, and with none free the OLDEST is faded out and taken. kAmigaVoices (8) are enough for the 4 and 8 channel songs of MED.
//
//   WHAT A NOTE IS. The sequencer's NoteOn (the note 0..127, the slot, the velocity 0..127 or kVelocityDefault) becomes a med::Note: the period comes from the ProTracker table
//   (note 48 is its first, C-1; the sample at the unity note 60 plays at period 428, as the importer's native-rate WAVs expect), the finetune from the instrument's fine tune
//   (12.5 cents a step), the volume 0..64 from the step's volume column ((velocity + 1) / 2, the importer's rule) or the instrument's own (SampleDesc::amiga_vol). The step's
//   volume REPLACES the instrument's, as MED's 0C does. The instrument's gain, envelope and filter are not applied (Paula has none).
//   BEAT SLICES (OS-SLICEFIX). An instrument that is a beat slice (SampleDesc::beat_slice) plays the slice the note names, or the track's Slice FX value (VoicePool::forced_slice), one shot, at the
//   root's pitch (note 60 plus the instrument's tune): the choice is voices.h beat_slice_span, the one the sampler's voices make. The slice is a span of the sample, so the Paula channel plays
//   that span and the sample offset command counts from its start.
//   WHAT A TICK IS. A Tick event (core FX 43 Speed: ticks a step, the sequencer's Config::emit_ticks) runs med::Track::tick for every track, and the result goes to the channel:
//   the period, the volume, a restart of the sample, a cut. Tick 0 of a row with a note runs in the NoteOn itself (the sequencer sends the row's FX before its note, and the
//   Tick after both), so that a song without Tick events still plays its notes at the right pitch and volume.
//   ENGINE FX. A ModSet of the groups kModGroupEngine0 / 1 (FX type 128 + a MED command in the first or second column) hands the track its command; a ModEnd ends it.
//
//   FILTERS. A Paula filter (Amiga 1200 high-pass by default; A500's RC low-pass and the LED filter by set_filter, and by the song's 0F F8 / F9) runs on each voice's float
//   block before it is rounded to Q15 (the filters are linear: one on each voice equals one on the sum).
//
// CONTEXTS. Main loop: set_filter, set_cfg. Render context (the audio interrupt): everything else. The two settings are single bytes the render reads once a block; no
// ordering between them. Nothing here allocates, blocks or loops unboundedly. The object holds the voices' blocks and the BLEP table's copy (about 5 KB): SRAM_U on the MK66.

#include <o3os/audio/events.h>
#include <o3os/audio/hot.h>
#include <o3os/audio/med_tick.h>
#include <o3os/audio/paula.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include <cstdint>

namespace o3os::audio {

inline constexpr unsigned kAmigaVoices = 8;
inline constexpr unsigned kAmigaTracks = 16;  // the graph's kTracks

class AmigaVoices {
 public:
  static constexpr std::uint32_t kBlock = hal::audio::kBlock;

  struct Stats {
    std::uint32_t note_ons = 0;
    std::uint32_t steals = 0;
    std::uint32_t ticks = 0;
    std::uint32_t ignored = 0;  // a note on an empty slot
  };

  AmigaVoices() noexcept;
  AmigaVoices(const AmigaVoices&) = delete;
  AmigaVoices& operator=(const AmigaVoices&) = delete;

  // ---- main loop ------------------------------------------------------------------------------------------------------------------
  // The Paula filters of every voice. The LED filter is also switched by the song's 0F F8 / F9.
  void set_filter(paula::Model model, bool led) noexcept {
    model_ = static_cast<std::uint8_t>(model);
    led_ = led ? 1 : 0;
  }
  // false: the song has the SoundTracker slide flag (FLAG_STSLIDE): slides on ticks 1.. only.
  void set_slide_tick0(bool on) noexcept { slide_tick0_ = on ? 1 : 0; }

  // ---- render context -------------------------------------------------------------------------------------------------------------
  O3OS_AUDIO_INLINE void begin_block() noexcept { touched_ = 0; }
  O3OS_AUDIO_HOT void render(std::uint32_t begin, std::uint32_t end) noexcept;
  // The three-pass specification of render() (float block, filter, Q15), in flash, for the tests only: the same int16 for every model, LED setting and channel state (OS-CPU10 A2).
  O3OS_AUDIO_COLD void render_ref(std::uint32_t begin, std::uint32_t end) noexcept;
  O3OS_AUDIO_COLD void end_block() noexcept;

  // The event of the sequencer or the live notes: true when this pool took it entirely (a note of an Amiga instrument, a Tick, an engine FX); NoteOff / NoteCut / NoteFade /
  // Stopped / FxReset end or reset what is here and return false (the sampler voices see them too). A NoteOn of an instrument that is not Amiga ends this track's voice.
  O3OS_AUDIO_COLD bool event(const SeqEvent& e, const VoicePool& samples) noexcept;
  // A live note (the pads, MIDI in): the same as a NoteOn event. False: not an Amiga instrument, or nothing started.
  O3OS_AUDIO_COLD bool live_note_on(unsigned track, unsigned note, unsigned slot, unsigned velocity, const VoicePool& samples) noexcept;
  O3OS_AUDIO_COLD void note_off(unsigned track) noexcept;
  // A note of another engine starts on the track: its voice here ends and the track's block is no longer this pool's.
  O3OS_AUDIO_COLD void release_track(unsigned track) noexcept;
  O3OS_AUDIO_COLD void release_all() noexcept;
  // OS-WAVEXP: every voice (the Paula channel's held value and BLEP ring, the filters), the MED track state, the song's LED switch and the tick count back to what the constructor made (the model, the
  // LED setting and the slide flag are the app's and stay). Engine::offline_quiet calls it at the start of every pass of an export (main loop, the render parked), so that an export of an unchanged
  // project is the same file every time; release_track() only fades a voice, and the live render keeps what it has (a live Play is not touched).
  O3OS_AUDIO_COLD void reset_pass() noexcept;

  // True when a voice sounds on the track (the live notes' track search).
  bool track_busy(unsigned track) const noexcept {
    for (unsigned i = 0; i < kAmigaVoices; ++i) {
      if (v_[i].on && v_[i].track == track) {
        return true;
      }
    }
    return false;
  }
  // The track's block of this block (kBlock samples), valid until the next begin_block(); null when no voice sounded on it or the track's latest note is not an Amiga note
  // (the tail of an Amiga voice that a Modern note replaced is dropped: a cut at the block edge, rare in a song of one engine).
  O3OS_AUDIO_INLINE const std::int16_t* feed(unsigned track) const noexcept { return track < kAmigaTracks ? feed_[track] : nullptr; }

  const Stats& stats() const noexcept { return stats_; }
  // The song's own LED switch (0F F8 / F9) as the render last set it; the export determinism test reads it to prove that a live Play engaged the LED filter. Cold, not for the render.
  bool song_led() const noexcept { return song_led_ != 0u; }
  unsigned voices_in_use() const noexcept {
    unsigned n = 0;
    for (unsigned i = 0; i < kAmigaVoices; ++i) {
      n += v_[i].on ? 1u : 0u;
    }
    return n;
  }
  const med::Track& track_state(unsigned track) const noexcept { return tracks_[track < kAmigaTracks ? track : 0u]; }

 private:
  struct Voice {
    paula::Channel ch;
    paula::Filter filt;
    const std::int16_t* data = nullptr;
    std::uint32_t length = 0;
    std::uint32_t loop_start = 0;
    std::uint32_t loop_end = 0;  // loop_end - loop_start < 2: one shot
    std::uint32_t age = 0;
    std::uint8_t track = 0;
    std::uint8_t wait = 0;  // ticks a delayed note may still take to start (the voice is held while it counts)
    bool on = false;
  };

  O3OS_AUDIO_COLD bool note_on(unsigned track, unsigned note, unsigned slot, unsigned velocity, const SampleDesc& d, unsigned forced_slice) noexcept;
  O3OS_AUDIO_COLD unsigned find(unsigned track) const noexcept;
  O3OS_AUDIO_COLD unsigned allocate() noexcept;
  O3OS_AUDIO_COLD void on_tick(unsigned index, unsigned ticks) noexcept;
  O3OS_AUDIO_COLD void apply(unsigned vi, unsigned track, const med::Out& o) noexcept;
  O3OS_AUDIO_COLD void restart(Voice& v, std::uint32_t offset) noexcept;
  O3OS_AUDIO_COLD void refresh_filters() noexcept;
  O3OS_AUDIO_HOT std::int16_t* touch(unsigned i) noexcept;

  static constexpr std::uint8_t kNone = 0xFF;
  Voice v_[kAmigaVoices];
  med::Track tracks_[kAmigaTracks];
  bool ran0_[kAmigaTracks] = {};            // tick 0 of the track's row ran in its NoteOn
  const std::int16_t* feed_[kAmigaTracks] = {};
  alignas(8) std::int16_t blocks_[kAmigaVoices][kBlock] = {};
  float blep_[paula::kBlepTableLen] = {};   // the minBLEP step, copied to SRAM at construction (the table in flash is read through the flash cache)
  std::uint32_t touched_ = 0;
  std::uint32_t owner_ = 0;                 // tracks whose latest note is an Amiga note: only those take their block from here (a Modern note on a track takes it back)
  std::uint32_t age_ = 0;
  std::uint32_t ticks_ = 6;                 // ticks a row, from the last Tick event (core FX 43's default until one comes)
  volatile std::uint8_t model_ = static_cast<std::uint8_t>(paula::Model::A1200);
  volatile std::uint8_t led_ = 0;
  volatile std::uint8_t slide_tick0_ = 1;
  std::uint8_t applied_model_ = 0xFF;       // what the voices' filters were last set to (render only)
  std::uint8_t applied_led_ = 0xFF;
  std::uint8_t song_led_ = 0;               // the song's own LED switch (0F F8 / F9), render only: OR-ed with led_
  Stats stats_;
};

}  // namespace o3os::audio
