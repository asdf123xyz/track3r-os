// SPDX-License-Identifier: GPL-3.0-or-later
//
// The parts of a set of stems (app/export_stems.h).
#include <o3os/app/export_stems.h>

#include <algorithm>

namespace o3os::app {

namespace {

// What the range plays: which tracks have a note, which instruments the notes of the tracks that are heard in the mix (not muted, soloed when any is) name, and whether one names none (the track's last instrument is then in use: unknown).
struct Scan {
  std::uint16_t has_note = 0;      // bit n: the audio track n plays a note in the range
  std::uint64_t used = 0;          // bit i: instrument slot i is named by a note of a sounding track
  bool unnamed = false;            // a note of a sounding track names no instrument
};

bool is_audio_track(const model::Project& p, unsigned t) noexcept {
  const model::TrackSlot* s = p.track(t);
  return t < p.active_tracks() && s != nullptr && s->type == model::TrackType::Audio;
}

// The instrument a step plays through is audio: not a MIDI instrument (it makes no sound in a file, PAR-063).
bool instrument_is_midi(const model::Project& p, unsigned instr) noexcept {
  const model::Instrument* in = instr >= 1u ? p.instrument(instr - 1u) : nullptr;
  return in != nullptr && in->kind == model::InstrumentKind::Midi;
}

void scan_pattern(const model::Project& p, unsigned id, std::uint16_t audio_tracks, std::uint16_t heard, Scan& out) noexcept {
  const model::Pattern* pat = p.pattern(id);
  if (pat == nullptr) {
    return;
  }
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    if (((audio_tracks >> t) & 1u) == 0u) {
      continue;
    }
    const unsigned len = std::min<unsigned>(pat->track_length[t], model::kMaxPatternLength);
    for (unsigned r = 0; r < len; ++r) {
      const model::Step& s = pat->steps[t][r];
      if (s.note < model::kNoteFirst || s.note > model::kNoteLast || instrument_is_midi(p, s.instr)) {
        continue;
      }
      out.has_note = static_cast<std::uint16_t>(out.has_note | (1u << t));
      if (((heard >> t) & 1u) != 0u) {
        if (s.instr >= 1u && s.instr <= 64u) {
          out.used |= std::uint64_t{1} << (s.instr - 1u);
        } else {
          out.unnamed = true;
        }
      }
    }
  }
}

// A bus can sound when it is not shut at the master and some instrument that is played may send to it.
bool bus_can_sound(const model::Project& p, const Scan& scan, bool delay) noexcept {
  const model::MasterSettings& m = p.master();
  if ((m.flags & (delay ? model::kMasterDelayMute : model::kMasterReverbMute)) != 0u || (delay ? m.delay_volume : m.reverb_volume) == 0u) {
    return false;
  }
  if (scan.unnamed) {
    return true;
  }
  for (unsigned i = 0; i < 64u; ++i) {
    if (((scan.used >> i) & 1u) == 0u) {
      continue;
    }
    const model::Instrument* in = p.instrument(i);
    if (in == nullptr || in->kind == model::InstrumentKind::None || in->kind == model::InstrumentKind::Midi) {
      continue;
    }
    if (in->kind == model::InstrumentKind::Module || (delay ? in->sampler.delay_send : in->sampler.reverb_send) != 0u) {
      return true;  // (a module instrument's own sends are not known here: it may send)
    }
  }
  return false;
}

void put_dec2(char* out, unsigned v) noexcept {
  out[0] = static_cast<char>('0' + (v / 10u) % 10u);
  out[1] = static_cast<char>('0' + v % 10u);
}

}  // namespace

void stem_label_at(unsigned n, char* out, std::size_t cap) noexcept {
  if (cap == 0u) {
    return;
  }
  const char* text = nullptr;
  char buf[8] = {};
  if (n < model::kMaxTracks) {
    buf[0] = 'T';
    put_dec2(buf + 1, n + 1u);
    text = buf;
  } else {
    text = n == model::kMaxTracks ? "DELAY" : (n == model::kMaxTracks + 1u ? "REVERB" : "MIX");
  }
  std::size_t i = 0;
  for (; text[i] != '\0' && i + 1u < cap; ++i) {
    out[i] = text[i];
  }
  out[i] = '\0';
}

void stem_label(const StemPart& part, char* out, std::size_t cap) noexcept {
  switch (part.kind) {
    case StemPart::Kind::Track: stem_label_at(part.track, out, cap); return;
    case StemPart::Kind::Delay: stem_label_at(model::kMaxTracks, out, cap); return;
    case StemPart::Kind::Reverb: stem_label_at(model::kMaxTracks + 1u, out, cap); return;
    case StemPart::Kind::Mix: break;
  }
  stem_label_at(model::kMaxTracks + 2u, out, cap);
}

void plan_stems(const model::Project& project, const audio::OfflineRequest& base, StemPlan& out) noexcept {
  out.count = 0;
  std::uint16_t audio_tracks = 0;
  std::uint16_t solo = 0;
  std::uint16_t muted = 0;
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    if (!is_audio_track(project, t)) {
      continue;
    }
    audio_tracks = static_cast<std::uint16_t>(audio_tracks | (1u << t));
    const std::uint8_t flags = project.track(t)->flags;
    solo = static_cast<std::uint16_t>(solo | ((flags & model::kTrackSolo) != 0u ? (1u << t) : 0u));
    muted = static_cast<std::uint16_t>(muted | ((flags & model::kTrackMute) != 0u ? (1u << t) : 0u));
  }
  const std::uint16_t heard = static_cast<std::uint16_t>((solo != 0u ? solo : audio_tracks) & ~muted & audio_tracks);

  Scan scan;
  if (base.what == audio::OfflineRequest::What::Song) {
    std::uint32_t seen[8] = {};  // the patterns already looked at (a song names the same one many times)
    for (std::uint32_t pos = 0; pos < project.song_length(); ++pos) {
      const unsigned id = project.order(pos);
      if (id == 0u || ((seen[id >> 5] >> (id & 31u)) & 1u) != 0u) {
        continue;
      }
      seen[id >> 5] |= 1u << (id & 31u);
      scan_pattern(project, id, audio_tracks, heard, scan);
    }
  } else {
    scan_pattern(project, base.pattern, audio_tracks, heard, scan);
  }

  for (unsigned t = 0; t < model::kMaxTracks && out.count < StemPlan::kMax; ++t) {
    if (((scan.has_note >> t) & 1u) == 0u) {
      continue;
    }
    StemPart& p = out.part[out.count++];
    p.kind = StemPart::Kind::Track;
    p.track = static_cast<std::uint8_t>(t);
    p.rq = base;
    p.rq.track_mask = static_cast<std::uint16_t>(1u << t);
    p.rq.dry_sends = true;
  }
  for (const bool delay : {true, false}) {
    if (out.count >= StemPlan::kMax - 1u || !bus_can_sound(project, scan, delay)) {
      continue;
    }
    StemPart& p = out.part[out.count++];
    p.kind = delay ? StemPart::Kind::Delay : StemPart::Kind::Reverb;
    p.track = 0;
    p.rq = base;
    p.rq.bus_only = true;
    p.rq.buses = delay ? 1u : 2u;
  }
  StemPart& mix = out.part[out.count++];  // (kMax holds 16 tracks and both buses and the MIX)
  mix.kind = StemPart::Kind::Mix;
  mix.track = 0;
  mix.rq = base;
}

}  // namespace o3os::app
