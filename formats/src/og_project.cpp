// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/og.h>

#include "og_codec.h"

#include <cstring>

namespace o3os::formats::og {

using namespace codec;  // NOLINT: the shared field access and layouts

namespace {

constexpr std::uint8_t kFw[4] = {1, 9, 0, 255};     // firmware 1.9.0.255, as official 1.9.0 writes project.mt
constexpr std::uint8_t kFw192[4] = {1, 9, 2, 255};  // 1.9.2.255 (no file backs it: open research question, unverified; docs/formats/og.md section 2)
constexpr std::uint8_t kProjectVersion = kProjectVerified;
constexpr std::uint8_t kProjectVersion17 = kProjectObservedOnly;

// What a version's file holds, from the byte maps of PAR-006 section 3
// (versions 2-5 are the v6 layout with fields missing, the name at the end).
struct Layout {
  bool known;               // an exact layout with evidence; else only the part common to every version is read
  std::uint32_t len;        // exact file length of a known layout
  std::uint32_t name_at;    // the project name, 0 when the layout does not give one
  bool names;               // track names 1-8 (0x428), since version 4
  bool volumes;             // track, reverb and delay volumes (0x4D0, 0x4D8), since version 5
  bool mutes;               // reverb, delay, dry mutes and dry volume (0x4DA...), since version 6
  bool short_names;         // names of tracks 9-16 (0x603), versions 16 and 17
  std::uint32_t groups;     // DropGroup bits: what the file carries that the model does not
};

constexpr std::uint32_t kBase = kDropSession | kDropDelay | kDropLimiter | kDropInputs | kDropPerformance |
                                kDropPatternBitmask;

Layout layout_of(std::uint8_t version) noexcept {
  switch (version) {
    case 2: return {true, proj::kLen2, 0x418, false, false, false, false, kBase | kDropMidiVelocity};
    case 3: return {true, proj::kLen3, 0x428, false, false, false, false, kBase | kDropReverb | kDropMidiVelocity};
    case 4: return {true, proj::kLen4, 0x4D0, true, false, false, false, kBase | kDropReverb | kDropMidiVelocity};
    case 5: return {true, proj::kLen5, 0x4DC, true, true, false, false, kBase | kDropReverb | kDropMidiVelocity};
    case 6:
    case 7: return {true, proj::kLen6, proj::kNameV6, true, true, true, false,
                    kBase | kDropReverb | kDropMidiVelocity};
    case 11:
    case 12:
    case 13:
    case 14: return {true, proj::kLen11, proj::kNameV11, true, true, true, false, kBase | kDropReverb | kDropExt13};
    case 16: return {true, proj::kLen16, proj::kNameV16, true, true, true, true,
                     kBase | kDropReverb | kDropExt13 | kDropExt19};
    case 17: return {true, proj::kLen17, proj::kNameV17, true, true, true, true,  // 1.9.2: v16 with four bytes inserted
                     kBase | kDropReverb | kDropExt13 | kDropExt19};              // before the name, the new one at 0x80C
    default: return {false, 0, 0, true, true, true, false, kBase | kDropReverb | kDropExt13 | kDropExt19};
  }
}

// "<prefix>N" for slot + 1 = N in `out` (prefix "Midi " or "Track ", N up to 16), its length returned.
std::size_t numbered_name(char* out, const char* prefix, std::uint32_t slot) noexcept {
  const std::size_t len = std::strlen(prefix);
  std::memcpy(out, prefix, len);
  const unsigned n = static_cast<unsigned>(slot) + 1;
  std::size_t at = len;
  if (n >= 10) out[at++] = static_cast<char>('0' + n / 10);
  out[at++] = static_cast<char>('0' + n % 10);
  out[at] = 0;
  return at;
}

// OS-TRK912: the OG's default name of slot 9-12 is "Midi 9".."Midi 12" (the 1.9.0 workspace fixture); here they are
// ordinary tracks, "Track 9".."Track 12". `name` is `from` + the number: true when it is.
bool is_numbered(const char* name, const char* from, std::uint32_t slot) noexcept {
  char want[12];
  numbered_name(want, from, slot);
  return std::strcmp(name, want) == 0;
}

}  // namespace

bool project_frame_ok(const std::uint8_t* head, std::size_t n, std::uint32_t size, const char** why) noexcept {
  const char* reason = "";
  bool ok = true;
  if (size > kProjectImageMax) {
    reason = "longer than any project.mt layout";
    ok = false;
  } else if (size < kHeaderLen || n < kHeaderLen) {
    reason = "the file ends inside the header";
    ok = false;
  } else if (head[0] != 'M' || head[1] != 'T' || get16(head + 2) != kTypeProject) {
    reason = "not an OG project file";
    ok = false;
  } else if (head[kVersionAt] < 2) {
    reason = "unreadable project version";
    ok = false;
  } else {
    const Layout lay = layout_of(head[kVersionAt]);
    if (lay.known ? size != lay.len : size < proj::kNameV6 + 4u) {
      reason = "wrong length for its version";
      ok = false;
    }
  }
  if (why != nullptr) {
    *why = reason;
  }
  return ok;
}

// ---- reader -----------------------------------------------------------------------------------------------

void ProjectReader::begin(core::ByteSource& source, model::ModelArena& arena, const ReadParams& params) noexcept {
  (void)params;
  begin_base(source, arena);
  project_->reset();
  have_ = 0;
  probing_ = false;
}

bool ProjectReader::apply(std::uint8_t version, std::uint32_t len) noexcept {
  const Layout lay = layout_of(version);
  const std::uint8_t* b = image_;
  model::Project& p = *project_;
  std::uint32_t& sanitised = report_.sanitised;
  (void)len;

  if (lay.name_at != 0) {  // the name is only "to recognise the project"; the folder is its real name
    const std::size_t n = field_text_len(b + lay.name_at, proj::kName, proj::kName - 1);
    if (p.set_name(reinterpret_cast<const char*>(b + lay.name_at), n) != model::Status::Ok) return false;
  }

  // tempo: BPM as f32; the model holds 40..800 in hundredths
  const float tempo = get_f32(b + proj::kTempo);
  std::uint32_t x100 = model::kDefaultTempoX100;
  if (!finite_float(tempo) || tempo < 10.0f || tempo > 1000.0f) {
    ++sanitised;
  } else {
    x100 = static_cast<std::uint32_t>(tempo * 100.0f + 0.5f);
    if (x100 < model::kMinTempoX100 || x100 > model::kMaxTempoX100) {
      x100 = x100 < model::kMinTempoX100 ? model::kMinTempoX100 : model::kMaxTempoX100;
      ++sanitised;
    }
  }
  if (p.set_tempo_x100(x100) != model::Status::Ok) return false;

  std::uint32_t pattern_length = get16(b + proj::kPatternLength);
  if (pattern_length < model::kMinPatternLength || pattern_length > model::kMaxPatternLength) {
    pattern_length = model::kDefaultPatternLength;
    ++sanitised;
  }
  if (p.set_default_pattern_length(pattern_length) != model::Status::Ok) return false;
  // The pattern this OS opens an OG project on: the file's saved actual-pattern field, read as >= 1 (our choice). The
  // model has no field for it (session state): the report carries it to the app.
  report_.start_pattern = b[proj::kActualPattern] != 0 ? b[proj::kActualPattern] : std::uint8_t{1};
  if (p.set_active_tracks(version >= 16 ? kTracks : 8) != model::Status::Ok) return false;

  // song: the playlist up to its first zero
  std::uint32_t song = 0;
  while (song < proj::kPlaylistLen && b[proj::kPlaylist + song] != 0) {
    if (p.append_order(b[proj::kPlaylist + song]) != model::Status::Ok) return false;
    ++song;
  }
  for (std::uint32_t i = song; i < proj::kPlaylistLen; ++i) {
    if (b[proj::kPlaylist + i] != 0) {
      ++sanitised;  // entries behind the first zero are ignored
      break;
    }
  }

  // tracks: OG tracks 1-8 are audio, 9-12 the extra tracks of 1.9 (MIDI on the OG, inferred: docs/formats/og.md section 10; ordinary tracks here: OS-TRK912),
  // 13-16 beyond the OG (MIDI); volume and mute from the file
  for (std::uint32_t slot = 0; slot < model::kMaxTracks; ++slot) {
    model::TrackSlot t{};
    t.type = slot < kTracks ? model::TrackType::Audio : model::TrackType::Midi;
    t.volume = model::kDefaultVolume;
    t.pan = model::kDefaultPan;
    if (slot < 8) {
      if (lay.names) {
        const std::uint8_t* nm = b + proj::kTrackNames + slot * proj::kTrackName;
        std::memcpy(t.name, nm, field_text_len(nm, proj::kTrackName, model::kTrackNameMax));
      } else {  // versions 2 and 3 have no names: the default "Track n" is ours
        std::memcpy(t.name, "Track ", 6);
        t.name[6] = static_cast<char>('1' + slot);
      }
      std::uint8_t volume = lay.volumes ? b[proj::kTrackVolume + slot] : std::uint8_t{100};
      if (volume > model::kMaxPercent) {
        volume = static_cast<std::uint8_t>(model::kMaxPercent);
        ++sanitised;
      }
      t.volume = volume;
      t.flags = b[proj::kTrackMute + slot] != 0 ? model::kTrackMute : std::uint8_t{0};
    } else if (lay.short_names) {
      const std::uint8_t* nm = b + proj::kShortNames + (slot - 8) * proj::kShortName;
      std::memcpy(t.name, nm, field_text_len(nm, proj::kShortName, proj::kShortName - 1));
      // OS-TRK912: the OG's default short name of tracks 9-12 becomes the ordinary "Track N" of tracks 1-8; a name the
      // user typed is kept
      if (slot < kTracks && is_numbered(t.name, "Midi ", slot)) numbered_name(t.name, "Track ", slot);
    }
    if (p.set_track(slot, t) != model::Status::Ok) return false;
  }

  // master section: the limiter is always on (BASE-10); the master volume is a device setting
  model::MasterSettings m{model::kDefaultVolume, 100, 100, 100, model::kMasterLimiter};
  auto level = [&sanitised](std::uint8_t v) {
    if (v > model::kMaxPercent) {
      ++sanitised;
      return static_cast<std::uint8_t>(model::kMaxPercent);
    }
    return v;
  };
  if (lay.volumes) {
    m.reverb_volume = level(b[proj::kReverbVolume]);
    m.delay_volume = level(b[proj::kDelayVolume]);
  }
  if (lay.mutes) {
    m.dry_volume = level(b[proj::kDryVolume]);
    m.flags = static_cast<std::uint8_t>(m.flags | (b[proj::kReverbMute] != 0 ? model::kMasterReverbMute : 0) |
                                        (b[proj::kDelayMute] != 0 ? model::kMasterDelayMute : 0) |
                                        (b[proj::kDryMute] != 0 ? model::kMasterDryMute : 0));
  }
  if (p.set_master(m) != model::Status::Ok) return false;

  // instruments 49-64 are the 16 MIDI channels: they have no file, so they exist in every OG project
  for (std::uint32_t ch = 0; ch < 16; ++ch) {
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Midi);
    in.midi_channel = static_cast<std::uint8_t>(ch + 1);
    std::memcpy(in.name, "MIDI ", 5);
    std::size_t n = 5;
    if (ch + 1 >= 10) in.name[n++] = '1';
    in.name[n] = static_cast<char>('0' + (ch + 1) % 10);
    if (p.set_instrument(kMidiFirstSlot + ch, in) != model::Status::Ok) return false;
  }
  // The settings the model has no field for are kept verbatim in an OGPM chunk (those that are not the defaults Export
  // writes anyway); what the layout carries beyond that is dropped: the session state, the bitmask (Export rebuilds
  // it), and, for a version nobody has seen, the blocks whose layout is not known.
  const std::uint32_t stored = lay.known ? (lay.groups & codec::settings::kStoredGroups)
                                         : (kDropDelay | kDropLimiter | kDropReverb | kDropInputs | kDropPerformance);
  report_.dropped_groups = lay.groups & ~stored;
  return keep_settings(stored, version);
}

bool ProjectReader::keep_settings(std::uint32_t stored, unsigned version) noexcept {
  namespace st = codec::settings;
  std::uint8_t now[460];  // the longest group
  std::uint8_t def[460];
  // The byte at 0x80C of a v17 project rides with group 9 (OGPM 1.1). Zero is its default: nothing to keep.
  const std::uint8_t tail = ((stored >> st::kTailGroup) & 1u) != 0 ? st::tail_byte(image_, version) : std::uint8_t{0};
  std::uint32_t mask = 0;
  std::uint32_t total = st::kHeader + (tail != 0 ? st::kTailLen : 0u);
  for (unsigned bit = 0; bit < kDropGroupCount; ++bit) {
    if (((stored >> bit) & 1u) == 0) continue;
    st::extract(bit, image_, version, now);
    st::defaults(bit, def);
    if (std::memcmp(now, def, st::group_len(bit)) != 0 || (bit == st::kTailGroup && tail != 0)) {
      mask |= 1u << bit;
      total += st::group_len(bit);
    }
  }
  if (mask == 0) {
    return true;  // everything is what Export writes without being told
  }
  switch (project_->unknown_begin(kChunkSettings, total)) {
    case model::Status::Ok: break;
    case model::Status::Full:  // no room: the model counts the chunk as dropped, the groups are not kept
      report_.dropped_groups |= mask;
      return true;
    default: return false;
  }
  const std::uint16_t chunk_version = tail != 0 ? st::kVersionTail : st::kVersion;
  const std::uint8_t head[st::kHeader] = {static_cast<std::uint8_t>(chunk_version >> 8),
                                          static_cast<std::uint8_t>(chunk_version),
                                          static_cast<std::uint8_t>(mask >> 8), static_cast<std::uint8_t>(mask)};
  if (project_->unknown_append(head, st::kHeader) != model::Status::Ok) return false;
  for (unsigned bit = 0; bit < kDropGroupCount; ++bit) {
    if (((mask >> bit) & 1u) == 0) continue;
    st::extract(bit, image_, version, now);
    if (project_->unknown_append(now, st::group_len(bit)) != model::Status::Ok) return false;
  }
  if (tail != 0 && project_->unknown_append(&tail, st::kTailLen) != model::Status::Ok) return false;  // group 9 is the last
  report_.kept_groups = mask;
  return true;
}

void ProjectReader::finish() noexcept {
  const std::uint32_t len = static_cast<std::uint32_t>(have_);
  if (len < kHeaderLen) {
    fail(FormatError::Truncated, "the file ends inside the header");
    return;
  }
  if (image_[0] != 'M' || image_[1] != 'T' || get16(image_ + 2) != kTypeProject) {
    fail(FormatError::BadMagic, "not an OG project file");
    return;
  }
  const std::uint8_t version = image_[kVersionAt];
  report_.version = version;
  report_.observed_only = version == kProjectObservedOnly;
  if (version < 2) {
    fail(FormatError::UnsupportedVersion, version == 0 ? "project structure version 0"
                                                       : "project version 1 has an unreadable layout");
    return;
  }
  const Layout lay = layout_of(version);
  if (lay.known) {
    if (len != lay.len) {
      fail(len < lay.len ? FormatError::Truncated : FormatError::Malformed,
           len < lay.len ? "the file is shorter than its version's layout" : "the file is longer than its version's layout");
      return;
    }
  } else {
    // v8-10, v15 (never seen) and 18+: only the part every version shares, which ends at 0x4DE
    report_.unverified_version = true;
    if (len < proj::kNameV6 + 4) {
      fail(FormatError::Truncated, "the file is too short for any project layout");
      return;
    }
  }
  const bool crc_ok = crc32(image_, len - 4) == get32(image_ + len - 4);
  if (!crc_ok) {
    if (!lay.known) {  // nothing vouches for this layout: the CRC has to
      fail(FormatError::BadCrc, "the CRC does not match the project");
      return;
    }
    report_.crc_mismatch = true;  // the firmware only warns about a project's CRC
  }
  if (!apply(version, len) || !project_->check_invariants()) {
    fail(FormatError::Internal, "the model refused a project field");
    return;
  }
  progress_ = Progress::Done;
}

Progress ProjectReader::step(std::size_t byte_budget) noexcept {
  std::size_t budget = byte_budget;
  while (progress_ == Progress::Running) {
    if (!probing_) {
      const Pull r = pull(image_, kProjectImageMax, have_, budget, false);
      if (r == Pull::Partial) {
        return Progress::Running;
      }
      if (r == Pull::Error) {
        break;
      }
      if (r == Pull::End) {
        finish();
        break;
      }
      probing_ = true;  // the image buffer is full: the source must be over, or the file is too long
    } else {
      std::uint8_t extra;
      std::size_t got = 0;
      const Pull r = pull(&extra, 1, got, budget, false);
      if (r == Pull::Partial) {
        return Progress::Running;
      }
      if (r == Pull::Error) {
        break;
      }
      if (r == Pull::End) {
        finish();
      } else {
        fail(FormatError::LimitExceeded, "longer than any project.mt layout");
      }
      break;
    }
  }
  if (progress_ == Progress::Failed && project_ != nullptr) {
    project_->reset();  // never leave a half-loaded project behind
  }
  return progress_;
}

// ---- writer -----------------------------------------------------------------------------------------------

namespace {

// Bit n of the usage byte: track n+1 holds a note, by the rule of docs/formats/og.md (it agrees with all 104 patterns
// of the fixture survey): the first eight tracks, steps before the first track's length byte (the index of its last step, so that step is not
// looked at), note byte >= 0 (an OG note, not off, cut or fade). Returns the steps examined.
std::uint32_t usage(const model::Pattern& p, std::uint8_t& out) noexcept {
  const std::uint32_t limit = p.track_length[0] - 1u;
  std::uint32_t examined = 0;
  std::uint8_t bits = 0;
  for (std::uint32_t t = 0; t < 8; ++t) {
    for (std::uint32_t s = 0; s < limit; ++s) {
      ++examined;
      if (p.steps[t][s].note >= model::kNoteFirst && p.steps[t][s].note <= model::kNoteLast) {
        bits = static_cast<std::uint8_t>(bits | (1u << t));
        break;
      }
    }
  }
  out = bits;
  return examined;
}

}  // namespace

namespace {

// Hook for OS-005: the audio engine will keep delay, limiter and reverb settings in chunks of its own. When a project
// holds them they take precedence over the OGPM values and the defaults; this writes them into the image and
// returns the DropGroup bits it covered. Nothing yet.
std::uint32_t write_engine_settings(const model::Project& project, std::uint8_t* image) noexcept {
  (void)project;
  (void)image;
  return 0;
}

// The settings groups of the file: the defaults of a fresh project, replaced by what the project's OGPM chunk
// carries (the first valid one; any other, or an invalid one, is ignored and counted). `defaulted_groups` says
// which groups ended up at the defaults.
void write_settings(const model::Project& p, std::uint8_t target, std::uint8_t* b, ExportDrops& drops) noexcept {
  namespace st = codec::settings;
  st::Parsed parsed;
  bool have = false;
  for (std::uint32_t i = 0; i < p.unknown_count(); ++i) {
    const model::UnknownChunk* c = p.unknown(i);
    if (c->id != kChunkSettings) continue;
    st::Parsed candidate;
    if (!have && st::parse(p.unknown_data(i), c->length, candidate)) {
      parsed = candidate;
      have = true;
    } else {
      ++drops.settings_ignored;
    }
  }
  std::uint8_t payload[460];
  for (unsigned bit = 0; bit < kDropGroupCount; ++bit) {
    if (((st::kStoredGroups >> bit) & 1u) == 0 || bit == 6) continue;  // 6: version 7's velocities have no place of their own
    st::defaults(bit, payload);
    st::apply(bit, payload, b);
  }
  if (have) {
    if (parsed.at[6] != nullptr) {  // MIDI velocity of an old project: the byte of its channel's record (inferred, PAR-006 OQ-5)
      for (unsigned ch = 0; ch < 16; ++ch) b[0x4DE + st::record_velocity_at(ch)] = parsed.at[6][ch];
    }
    for (unsigned bit = 0; bit < kDropGroupCount; ++bit) {
      if (bit != 6 && parsed.at[bit] != nullptr) st::apply(bit, parsed.at[bit], b);
    }
    if (parsed.tail != nullptr && *parsed.tail != 0) {  // OGPM 1.1: a v17 project's byte at 0x80C
      if (target == kTargetOg192) {
        b[proj::kNewByteV17] = *parsed.tail;
      } else {
        ++drops.project_settings;  // a v16 file has no such field (0x80C is the name)
      }
    }
  }
  const std::uint32_t carried = (have ? parsed.groups : 0u) | write_engine_settings(p, b);
  drops.defaulted_groups |= (kDropSession | st::kStoredGroups) & ~carried;
}

}  // namespace

namespace {
std::uint32_t project_file_len(std::uint8_t target) noexcept {
  return target == kTargetOg192 ? kProjectBytes17 : kProjectBytes16;
}
}  // namespace

void ProjectWriter::begin(const model::Project& project, core::ByteSink& sink, const WriteParams& params) noexcept {
  begin_base(project, sink);
  phase_ = Phase::Done;
  if (params.target != kTargetOg190 && params.target != kTargetOg192) {
    fail(FormatError::Malformed, "unknown Export target");
    return;
  }
  target_ = params.target;
  phase_ = Phase::Scan;
  scan_id_ = 1;
  off_ = 0;
  std::memset(bitmask_, 0, sizeof bitmask_);
}

void ProjectWriter::build() noexcept {
  const model::Project& p = *project_;
  std::uint8_t* b = image_;
  const bool v17 = target_ == kTargetOg192;
  const std::uint32_t file_len = project_file_len(target_);
  std::memset(b, 0, sizeof image_);
  stamp_header(b, 'M', 'T', kTypeProject, v17 ? kFw192 : kFw, v17 ? kProjectVersion17 : kProjectVersion,
               static_cast<std::uint16_t>(file_len));

  // song: pattern 1 when there is none (our choice for an empty song)
  const std::uint32_t song = p.song_length();
  if (song == 0) {
    b[proj::kPlaylist] = 1;
  }
  for (std::uint32_t i = 0; i < song; ++i) {
    b[proj::kPlaylist + i] = p.order(i);
  }
  b[proj::kActualPattern] = song != 0 ? p.order(0) : std::uint8_t{1};
  // session state the model does not keep, at the values of a fresh 1.9.0 project (docs/formats/og.md)
  b[proj::kPadBoard + 1] = 11;
  b[proj::kPadBoard + 2] = 24;
  b[proj::kPadBoard + 3] = 8;
  b[proj::kOldVolume] = 50;
  b[proj::kEditStep] = 1;
  // tempo and pattern length
  put_f32(b + proj::kTempo, static_cast<float>(p.tempo_x100()) / 100.0f);
  put16(b + proj::kPatternLength, p.default_pattern_length());
  std::memcpy(b + proj::kBitmask, bitmask_, proj::kBitmaskLen);
  write_settings(p, target_, b, report_.drops);

  // tracks 1-8: name, volume, mute
  ExportDrops& drops = report_.drops;
  for (std::uint32_t slot = 0; slot < model::kMaxTracks; ++slot) {
    const model::TrackSlot& t = *p.track(slot);
    const std::size_t n = text_len(t.name, model::kTrackNameMax);
    if (slot < 8) {
      std::memcpy(b + proj::kTrackNames + slot * proj::kTrackName, t.name, n);
      b[proj::kTrackVolume + slot] = t.volume;
      b[proj::kTrackMute + slot] = (t.flags & model::kTrackMute) != 0 ? 1 : 0;
    } else {
      const char* shown = t.name;
      std::size_t keep = n;
      char og_default[12];  // the way back of Import: "Track 9".."Track 12" is written as the OG's "Midi 9".."Midi 12"
      if (slot < kTracks && is_numbered(t.name, "Track ", slot)) {
        keep = numbered_name(og_default, "Midi ", slot);
        shown = og_default;
      }
      if (keep > proj::kShortName - 1) {
        keep = proj::kShortName - 1;
        ++drops.track_names_cut;
      }
      std::memcpy(b + proj::kShortNames + (slot - 8) * proj::kShortName, shown, keep);
    }
    if (slot < kTracks) {  // what an OG track has no field for: pan, solo, bus, a type other than its own, and
      // (tracks 9-12) mute and volume, which the file only holds for tracks 1-8
      const bool og_audio = slot < 8;
      // OS-TRK912: an audio track in 9-12 (what Import makes of them) goes into the OG's MIDI slot as it is: reported as
      // that (a note), not as a lost setting
      const bool audio_on_midi = !og_audio && t.type == model::TrackType::Audio;
      if (audio_on_midi) ++drops.audio_on_midi_tracks;
      bool lost = t.pan != model::kDefaultPan || (t.flags & model::kTrackSolo) != 0 || t.bus != 0 ||
                  (t.type != (og_audio ? model::TrackType::Audio : model::TrackType::Midi) && !audio_on_midi);
      if (!og_audio) {
        lost = lost || (t.flags & model::kTrackMute) != 0 || t.volume != model::kDefaultVolume;
      }
      if (lost) ++drops.track_settings;
    }
  }

  // master section
  const model::MasterSettings& m = p.master();
  b[proj::kReverbVolume] = m.reverb_volume;
  b[proj::kDelayVolume] = m.delay_volume;
  b[proj::kReverbMute] = (m.flags & model::kMasterReverbMute) != 0 ? 1 : 0;
  b[proj::kDelayMute] = (m.flags & model::kMasterDelayMute) != 0 ? 1 : 0;
  b[proj::kDryMute] = (m.flags & model::kMasterDryMute) != 0 ? 1 : 0;
  b[proj::kDryVolume] = m.dry_volume;

  // name: 31 characters at most
  std::size_t n = text_len(p.name(), model::kProjectNameMax);
  if (n > proj::kName - 1) {
    n = proj::kName - 1;
    ++drops.project_settings;
  }
  std::memcpy(b + (v17 ? proj::kNameV17 : proj::kNameV16), p.name(), n);
  if (p.swing() != model::kDefaultSwing) {
    ++drops.project_settings;
  }
  for (std::uint32_t i = 0; i < p.unknown_count(); ++i) {  // the OG chunks are carried to the file, not dropped
    const std::uint32_t id = p.unknown(i)->id;
    if (id != kChunkSettings && id != kChunkNames) ++drops.unknown_chunks;
  }
  count_instrument_drops(p, drops);

  put32(b + file_len - 4, crc32(b, file_len - 4));
}

Progress ProjectWriter::step(std::size_t byte_budget) noexcept {
  std::size_t budget = byte_budget;
  while (progress_ == Progress::Running) {
    if (!unchanged()) {
      break;
    }
    switch (phase_) {
      case Phase::Scan:
        while (scan_id_ <= model::kMaxPatternId) {
          std::size_t cost = 1;
          if (const model::Pattern* pat = project_->pattern(scan_id_)) {
            cost += usage(*pat, bitmask_[scan_id_ - 1]);
          }
          ++scan_id_;
          budget = cost >= budget ? 0 : budget - cost;
          if (budget == 0 && scan_id_ <= model::kMaxPatternId) {
            return Progress::Running;
          }
        }
        phase_ = Phase::Build;
        break;
      case Phase::Build:
        build();
        phase_ = Phase::Emit;
        break;
      case Phase::Emit: {
        const Flush f = flush(image_, project_file_len(target_), off_, budget);
        if (f == Flush::Partial) {
          return Progress::Running;
        }
        if (f == Flush::Error) {
          return progress_;
        }
        phase_ = Phase::Done;
        break;
      }
      case Phase::Done:
        progress_ = Progress::Done;
        break;
    }
  }
  return progress_;
}

}  // namespace o3os::formats::og
