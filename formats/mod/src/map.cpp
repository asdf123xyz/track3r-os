// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/mod/map.h>
#include <o3os/formats/volume_level.h>

#include <algorithm>
#include <cstring>

namespace o3os::formats::mod {

namespace {

constexpr std::uint8_t kFull = 64;

inline std::uint8_t bcd(std::uint32_t v) noexcept { return static_cast<std::uint8_t>(((v / 10u) << 4) | (v % 10u)); }

inline std::uint32_t div_round(std::uint64_t n, std::uint64_t d) noexcept { return static_cast<std::uint32_t>((n + d / 2) / d); }

// A pan of 0..255 (128 the centre) as Channel pan (FX 57; Cell command 0x46, which the mapper writes on every engine): 0..100, rounded.
inline std::uint8_t pan_100(int p255) noexcept {
  const int p = std::min(255, std::max(0, p255));
  return static_cast<std::uint8_t>((p * 100 + 127) / 255);
}

}  // namespace

int memory_class(std::uint8_t effect) noexcept {
  switch (effect) {
    case 4: case 11: case 12: return 0;  // D K L
    case 5: case 6: return 1;            // E F
    case 10: return 2;                   // J
    case 17: return 3;                   // Q
    case 18: return 4;                   // R
    case 19: return 5;                   // S
    case 16: return 6;                   // P
    case 9: return 7;                    // I
    case 14: return 8;                   // N (channel volume slide)
    case 23: return 9;                   // W (global volume slide)
    default: return -1;
  }
}

// ---- the source effects --------------------------------------------------------------------------------------------------

unsigned fx_id(Kind kind, const Cell& c) noexcept {
  if (kind == Kind::Mod) {
    if (c.effect == 0 && c.param == 0) {
      return kNoFx;
    }
    return c.effect == 0x0E ? 16u + (c.param >> 4) : static_cast<unsigned>(c.effect & 15u);
  }
  if (c.effect == 0) {
    return kNoFx;
  }
  if (c.effect > 26) {
    return 80;
  }
  return c.effect == 19 ? 64u + (c.param >> 4) : 32u + c.effect;
}

FxClass fx_class(unsigned id) noexcept {
  using C = FxClass;
  static const C kClass[] = {
      // MOD 0..15: 0 arpeggio, 1 2 3 slides and portamento, 4 vibrato, 5 6 7, 8 pan (OS-COREFX: Channel pan, 0..100), 9 offset, A volume slide, B jump, C volume, D break, (E: ids 16..31), F speed and tempo
      C::Mapped, C::Mapped, C::Mapped, C::Mapped, C::Mapped, C::Mapped, C::Mapped, C::Mapped, C::Approx, C::Mapped, C::Mapped, C::Flow, C::Mapped, C::Flow, C::Drop, C::Mapped,
      // MOD E0x..EFx: filter, fine slides, glissando, vib. wave, finetune, loop, trem. wave, pan (not ProTracker's), retrigger, fine volume x2, cut, delay, pattern delay, invert loop
      C::Mapped, C::Mapped, C::Mapped, C::Drop, C::Drop, C::Mapped, C::Flow, C::Drop, C::Approx, C::Mapped, C::Mapped, C::Mapped, C::Mapped, C::Mapped, C::Flow, C::Drop,
      // S3M A..Z at 33..58 (32 is unused)
      C::Drop, C::Mapped, C::Flow, C::Flow, C::Mapped, C::Mapped, C::Mapped, C::Mapped, C::Approx, C::Drop, C::Mapped, C::Approx, C::Mapped, C::Drop, C::Drop, C::Mapped,
      C::Drop, C::Approx, C::Approx, C::Drop, C::Mapped, C::Approx, C::Drop, C::Drop, C::Approx, C::Drop, C::Drop, C::Drop, C::Drop, C::Drop, C::Drop, C::Drop,
      // S3M S0x..SFx at 64..79
      C::Drop, C::Drop, C::Approx, C::Drop, C::Drop, C::Drop, C::Drop, C::Drop, C::Approx, C::Drop, C::Drop, C::Flow, C::Mapped, C::Mapped, C::Flow, C::Drop,
      // 80: an effect byte above Z
      C::Drop, C::Drop, C::Drop, C::Drop, C::Drop, C::Drop, C::Drop, C::Drop, C::Drop, C::Drop, C::Drop, C::Drop, C::Drop, C::Drop, C::Drop, C::Drop,
  };
  static_assert(sizeof kClass / sizeof kClass[0] == kFxIds, "one class for every source effect id");
  return id < kFxIds ? kClass[id] : C::Drop;
}

const char* fx_name(unsigned id) noexcept {
  static const char* const kNames[] = {
      "MOD 0xy arpeggio", "MOD 1xx slide up", "MOD 2xx slide down", "MOD 3xx tone portamento", "MOD 4xy vibrato", "MOD 5xy porta + volume slide", "MOD 6xy vibrato + volume slide",
      "MOD 7xy tremolo", "MOD 8xx pan", "MOD 9xx sample offset", "MOD Axy volume slide", "MOD Bxx position jump", "MOD Cxx volume", "MOD Dxy pattern break", "MOD E..", "MOD Fxx speed/tempo",
      "MOD E0x filter", "MOD E1x fine slide up", "MOD E2x fine slide down", "MOD E3x glissando", "MOD E4x vibrato waveform", "MOD E5x finetune", "MOD E6x pattern loop",
      "MOD E7x tremolo waveform", "MOD E8x pan", "MOD E9x retrigger", "MOD EAx fine volume up", "MOD EBx fine volume down", "MOD ECx note cut", "MOD EDx note delay",
      "MOD EEx pattern delay", "MOD EFx invert loop",
      "S3M ..", "S3M Axx speed", "S3M Bxx position jump", "S3M Cxx pattern break", "S3M Dxy volume slide", "S3M Exx portamento down", "S3M Fxx portamento up", "S3M Gxx tone portamento",
      "S3M Hxy vibrato", "S3M Ixy tremor", "S3M Jxy arpeggio", "S3M Kxy vibrato + volume slide", "S3M Lxy porta + volume slide", "S3M Mxx channel volume", "S3M Nxy channel volume slide",
      "S3M Oxx sample offset", "S3M Pxy pan slide", "S3M Qxy retrigger + volume slide", "S3M Rxy tremolo", "S3M S..", "S3M Txx tempo", "S3M Uxy fine vibrato", "S3M Vxx global volume",
      "S3M Wxx global volume slide", "S3M Xxx pan", "S3M Yxy panbrello", "S3M Zxx sync", "", "", "", "", "",
      "S3M S0x filter", "S3M S1x glissando", "S3M S2x finetune", "S3M S3x vibrato waveform", "S3M S4x tremolo waveform", "S3M S5x panbrello waveform", "S3M S6x fine pattern delay",
      "S3M S7x NNA", "S3M S8x pan", "S3M S9x sound control", "S3M SAx stereo control", "S3M SBx pattern loop", "S3M SCx note cut", "S3M SDx note delay", "S3M SEx pattern delay",
      "S3M SFx funk repeat", "S3M effect above Z", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
  };
  static_assert(sizeof kNames / sizeof kNames[0] == kFxIds, "a name for every source effect id");
  return id < kFxIds ? kNames[id] : "";
}

// ---- a cell in the terms of the OctaMED mapping ------------------------------------------------------------------------------------

namespace {

void command(Translated& o, std::uint8_t cmd, std::uint8_t data) noexcept {
  o.cell.command = cmd;
  o.cell.data = data;
  o.med_cmd = cmd;
}

void add_extra(Translated& o, std::uint8_t cmd, std::uint8_t data) noexcept {
  if (o.extras < 2) {
    o.extra[o.extras].command = cmd;
    o.extra[o.extras].data = data;
    ++o.extras;
  }
}

// the MED commands of the flow, for the structural fallback (which has no flow): 0B jump to a position, 1D break to a line
void flow_command(Translated& o, const Cell& s, Kind kind) noexcept {
  const FlowOp op = flow_op(kind, s);
  if (op.kind == OpKind::Jump) {
    o.cell.command = 0x0B;
    o.cell.data = op.value;
  } else if (op.kind == OpKind::Break) {
    o.cell.command = 0x1D;
    o.cell.data = op.value;
  } else if (op.kind == OpKind::LoopStart || op.kind == OpKind::LoopBack) {
    o.cell.command = 0x16;
    o.cell.data = op.value;
  } else if (op.kind == OpKind::Delay) {
    o.cell.command = 0x1E;
    o.cell.data = op.value;
  }
}

void translate_mod(const Cell& s, Translated& o, bool core_fx) noexcept {
  const std::uint8_t p = s.param;
  const std::uint8_t x = p & 15u;
  switch (s.effect) {
    case 0x00: if (p != 0) command(o, 0x00, p); break;
    case 0x01: command(o, 0x01, p); break;
    case 0x02: command(o, 0x02, p); break;
    case 0x03: command(o, 0x03, p); break;
    case 0x04: command(o, 0x14, p); break;     // 14 is the vibrato at ProTracker's depth (04 is twice as deep)
    case 0x05: command(o, 0x05, p); break;
    case 0x06: command(o, 0x06, p); break;
    case 0x07: command(o, 0x07, p); break;
    case 0x08: command(o, 0x46, pan_100(p)); break;  // OS-COREFX: not a ProTracker effect, but every tracker since FastTracker and libopenmpt read it as a pan (00 left, 80 centre, FF right): Channel pan (57), as the S3M Xxx
    case 0x09: command(o, 0x19, p); break;
    case 0x0A: command(o, 0x0A, p); break;
    case 0x0C: command(o, 0x0C, bcd(std::min<unsigned>(p, kFull))); break;
    case 0x0B:
    case 0x0D:
      o.flow = true;
      flow_command(o, s, Kind::Mod);
      break;
    case 0x0F: {
      const FlowOp op = flow_op(Kind::Mod, s);
      o.timing = op.kind;
      o.timing_value = op.value;
      break;
    }
    case 0x0E:
      switch (p >> 4) {
        case 0x0: command(o, 0x0F, x == 0 ? 0xF9 : 0xF8); break;  // E00: the LED filter on, E01: off
        case 0x1: command(o, 0x11, x); break;
        case 0x2: command(o, 0x12, x); break;
        case 0x5: command(o, 0x15, static_cast<std::uint8_t>(x >= 8 ? 0xF0u | x : x)); break;
        case 0x9: if (x != 0) command(o, 0x1F, x); break;
        case 0xA: command(o, 0x1A, x); break;
        case 0xB: command(o, 0x1B, x); break;
        case 0xC: command(o, 0x18, x); break;
        case 0xD: if (x != 0) command(o, 0x1F, static_cast<std::uint8_t>(x << 4)); break;
        case 0x6:
        case 0xE:
          o.flow = true;
          flow_command(o, s, Kind::Mod);
          break;
        case 0x8: command(o, 0x46, pan_100(static_cast<int>(x) * 17)); break;  // E8x: the same pan in 16 steps (libopenmpt: x x 17)
        case 0x3: if (core_fx) command(o, 0x43, x); break;                      // OS-COREFX CF2: glissando on (x != 0) or off
        case 0x4: if (core_fx) { command(o, 0x41, static_cast<std::uint8_t>(x & 7u)); o.approx = true; } break;  // the vibrato waveform: the sine is mapped to the triangle LFO shape
        case 0x7: if (core_fx) { command(o, 0x42, static_cast<std::uint8_t>(x & 7u)); o.approx = true; } break;  // the tremolo waveform
        default: break;  // E3x E4x E7x without core_fx, EFx: nothing to map to (fx_class says Drop)
      }
      break;
    default:
      break;
  }
}

// D: the volume slide of S3M, whose parameter says up, down, fine up or fine down.
void s3m_volume_slide(Translated& o, std::uint8_t p) noexcept {
  const std::uint8_t x = p >> 4, y = p & 15u;
  if (p == 0) {
    command(o, 0x0A, 0);
  } else if (y == 0) {
    command(o, 0x0A, static_cast<std::uint8_t>(x << 4));
  } else if (x == 0) {
    command(o, 0x0A, y);
  } else if (y == 0x0F) {
    command(o, 0x1A, x);  // fine up
  } else if (x == 0x0F) {
    command(o, 0x1B, y);  // fine down
  } else {
    command(o, 0x0A, p);
    o.approx = true;      // both nibbles: a slide up and down at once, which ST3 does not define
  }
}

void translate_s3m(const Cell& s, Translated& o, bool core_fx) noexcept {
  const std::uint8_t p = s.param;
  const std::uint8_t x = p & 15u;
  switch (s.effect) {
    case 1:
    case 20: {
      const FlowOp op = flow_op(Kind::S3m, s);
      o.timing = op.kind;
      o.timing_value = op.value;
      break;
    }
    case 2:
    case 3:
      o.flow = true;
      flow_command(o, s, Kind::S3m);
      break;
    case 4: s3m_volume_slide(o, p); break;
    case 5:
    case 6: {  // E portamento down, F portamento up: 2 and 1 in MED
      const std::uint8_t slide = s.effect == 5 ? 0x02 : 0x01;
      const std::uint8_t fine = s.effect == 5 ? 0x12 : 0x11;
      if (p >= 0xF0) {
        if (x != 0) command(o, fine, x);
      } else if (p >= 0xE0) {  // extra fine: a quarter of a fine step, which the MED command cannot say below one
        if (core_fx) {         // OS-COREFX: the fine slide FX counts quarter units, the cell says them (Cell::ext) and the MED data byte is what the engine window would take
          if (x != 0) {
            command(o, fine, static_cast<std::uint8_t>(x >> 2));
            o.cell.ext = x;
          }
        } else {
          o.approx = true;
          if ((x >> 2) != 0) command(o, fine, static_cast<std::uint8_t>(x >> 2));
          else o.dropped = true;
        }
      } else {
        command(o, slide, p);
      }
      break;
    }
    case 7: command(o, 0x03, p); break;
    case 8: command(o, 0x14, p); break;
    case 10: if (p != 0) command(o, 0x00, p); break;
    case 11: command(o, 0x06, p); break;
    case 12: command(o, 0x05, p); break;
    case 9: if (core_fx) command(o, 0x45, p); break;  // OS-COREFX CF2: I, the tremor
    case 13: if (core_fx) { if (p <= 64) command(o, 0x47, p); else o.dropped = true; } break;  // OS-COREFX CF3: M, the channel volume (libopenmpt ignores a value above 64: counted as dropped)
    case 14: if (core_fx) command(o, 0x48, p); break;             // N, the channel volume slide
    case 22: if (core_fx) { if (p <= 64) command(o, 0x49, p); else o.dropped = true; } break;  // V, the global volume (same)
    case 23: if (core_fx) command(o, 0x4A, p); break;             // W, the global volume slide
    case 25: if (core_fx) command(o, 0x4B, p); break;             // Y, the panbrello (speed x, depth y; a zero nibble keeps the track's last: the engine's memory)
    case 15: command(o, 0x19, p); break;
    case 16: if (core_fx) command(o, 0x40, p); break;  // P, the pan slide
    case 17:
      if (core_fx) {
        if (x != 0) command(o, 0x44, p);  // Q: the retrigger with its volume change (a tracker FX); Qx0 never restarts
      } else if (x != 0) {
        command(o, 0x1F, x);  // the retrigger; the volume change of Qxy has no MED counterpart
      }
      break;
    case 18: command(o, 0x07, p); break;
    case 21: {  // fine vibrato: a quarter of the depth
      const std::uint8_t depth = x == 0 ? std::uint8_t{0} : std::max<std::uint8_t>(1, x >> 2);
      command(o, 0x14, static_cast<std::uint8_t>((p & 0xF0u) | depth));
      break;
    }
    case 24: command(o, 0x46, pan_100(2 * std::min<int>(p, 0x80))); break;  // Xxx is 0..0x80 (0x40 the centre) in libopenmpt, and XA4 the surround: held to the right here (S3M panning is 0..0x80)
    case 19:
      switch (p >> 4) {
        case 0x1: if (core_fx) command(o, 0x43, x); break;                      // S1x: glissando on / off
        case 0x2: command(o, 0x15, static_cast<std::uint8_t>(static_cast<std::int8_t>(static_cast<int>(x) - 8))); break;
        case 0x3: if (core_fx) { command(o, 0x41, static_cast<std::uint8_t>(x & 7u)); o.approx = true; } break;  // S3x: the vibrato waveform
        case 0x4: if (core_fx) { command(o, 0x42, static_cast<std::uint8_t>(x & 7u)); o.approx = true; } break;  // S4x: the tremolo waveform
        case 0x8: command(o, 0x46, pan_100(static_cast<int>(x) * 17)); break;  // S8x
        case 0xC: command(o, 0x18, x); break;
        case 0xD: if (x != 0) command(o, 0x1F, static_cast<std::uint8_t>(x << 4)); break;
        case 0xB:
        case 0xE:
          o.flow = true;
          flow_command(o, s, Kind::S3m);
          break;
        default: break;
      }
      break;
    default:
      break;
  }
}

}  // namespace

void translate(Kind kind, const Cell& src, std::uint32_t slots, Translated& o, bool core_fx) noexcept {
  o = Translated();
  o.cell.note = src.note;
  o.off_table = src.off_table;
  o.bad_note = src.bad_note;
  if (src.instrument != 0) {
    if (src.instrument <= slots) {
      o.cell.instrument = src.instrument;
    } else {
      o.bad_instrument = true;
    }
  }
  if (kind == Kind::S3m) {
    if (src.cut) {
      add_extra(o, 0x0F, 0xFF);  // the note off (a cut): LineMapper writes the cut where the step has no note
    }
    if (src.volume != 255) {
      if (src.volume <= kFull) {
        add_extra(o, 0x0C, bcd(src.volume));
      } else {
        o.volume_invalid = true;
      }
    }
  }
  o.id = fx_id(kind, src);
  if (o.id == kNoFx) {
    return;
  }
  if (kind == Kind::Mod) {
    translate_mod(src, o, core_fx);
  } else {
    translate_s3m(src, o, core_fx);
  }
  const FxClass cls = fx_class(o.id);
  if (cls == FxClass::Flow) {
    o.flow = true;
  } else if (o.timing != OpKind::None) {
    // a tempo or speed: LineMapper::set_tempo / set_speed
  } else if (o.cell.command != 0 || o.cell.data != 0) {  // a MED command was made (the arpeggio is command 00: its data is what says so)
    if (!(o.cell.command >= 0x46 || (core_fx && o.cell.command >= 0x40))) {
      o.approx = o.approx || cls == FxClass::Approx;  // (the tracker effects of CF2 say themselves whether they are exact)
    }
  } else if (cls == FxClass::Drop || o.dropped) {
    o.dropped = true;  // no MED command to map it to
  } else {
    o.id = kNoFx;      // a parameter that makes it a no-op (E90, SD0, J00, F00)
  }
}

// ---- instruments -----------------------------------------------------------------------------------------------------------

// The rates a sample keeps: the engine holds a sample's rate to 1,000..384,000 Hz (audio::kRateMinHz; formats may not include the audio layer) and the importer to 192,000; a C2SPD outside
// 1,000..192,000 is clamped to it and counted (ImportReport::rates_clamped), so that the instrument's rate is the one that plays (OS-RATEPITCH).
constexpr std::uint32_t kRateMinHz = 1000;
constexpr std::uint32_t kRateMaxHz = 192000;

std::uint32_t sample_rate_of(Kind kind, const SampleInfo& info, med::Clock clock) noexcept {
  if (kind == Kind::Mod) {
    return med::paula_rate(clock);
  }
  const std::uint32_t hz = info.c2spd == 0 ? 8363u : info.c2spd;
  return std::max<std::uint32_t>(std::min<std::uint32_t>(hz, kRateMaxHz), kRateMinHz);
}

std::uint8_t mod_pan_percent(std::uint32_t t) noexcept {
  const std::uint32_t k = t & 3u;  // Paula: channels 0 and 3 left, 1 and 2 right (more channels repeat it)
  return (k == 0 || k == 3) ? 0 : 100;
}

bool map_sample(Kind kind, std::uint32_t slot, const SampleInfo& info, med::Clock clock, std::uint64_t pcm_left, bool modern, model::Instrument& out, med::InstReport& rep,
                std::uint16_t& extra_flags) noexcept {
  rep = med::InstReport();
  extra_flags = 0;
  switch (info.kind) {
    case SampleKind::Empty:
      rep.result = (info.notes & kNoteTrimmed) != 0 ? med::InstResult::EmptyData : med::InstResult::None;
      return false;
    case SampleKind::AdLib:
    case SampleKind::Unsupported:
      rep.result = med::InstResult::Unsupported;
      return false;
    case SampleKind::Pcm:
      break;
  }
  const std::uint64_t pcm = static_cast<std::uint64_t>(info.frames) * 2u;
  if (pcm > med::caps::kSampleArenaBytes) {
    rep.result = med::InstResult::Oversize;
    return false;
  }
  if (pcm > pcm_left) {
    rep.result = med::InstResult::ArenaFull;
    return false;
  }
  rep.frames = info.frames;
  rep.result = med::InstResult::Sample;
  rep.src_bits = info.sixteen_bit ? 16 : 8;
  if (info.sixteen_bit) {
    rep.flags |= med::kInstSixteenBit;
  }
  if ((info.notes & kNoteStereo) != 0) {
    rep.flags |= med::kInstStereoMix;  // (the left channel only: the flag says the file was stereo)
  }
  rep.reader_notes = info.notes;

  model::init_instrument(out, model::InstrumentKind::Sample);
  std::strncpy(out.name, info.name, model::kInstrumentNameMax);
  out.name[model::kInstrumentNameMax] = '\0';
  const unsigned number = slot + 1u;
  const char path[] = {'s', 'a', 'm', 'p', 'l', 'e', 's', '/', 'i', 'n', 's', 't', 'r', static_cast<char>('0' + number / 10u), static_cast<char>('0' + number % 10u),
                       '.', 'w', 'a', 'v', '\0'};
  std::memcpy(out.sample_path, path, sizeof path);
  if (kind == Kind::S3m) {
    if (info.c2spd == 0) {
      extra_flags |= kSampleRateDefault;
    } else if (info.c2spd > kRateMaxHz || info.c2spd < kRateMinHz) {
      extra_flags |= kSampleRateClamped;
    }
  }
  out.sample_rate = sample_rate_of(kind, info, clock);
  if (modern) {
    out.engine = model::kInstEngineModern;
  }
  model::SamplerParams& sp = out.sampler;
  sp.volume = volume_level_of_tracker(info.volume);  // OS-BUG-PASTE24: 64 is level 50 (0 dB), the lower volumes at their ratio in dB
  const int ft = std::min(7, std::max(-8, static_cast<int>(info.finetune)));
  sp.fine_tune = static_cast<std::int8_t>((ft * 25 + (ft >= 0 ? 1 : -1)) / 2);  // 12.5 cents a step, to nearest (as the MED importer)
  rep.fine_tune = sp.fine_tune;
  if (info.has_loop && info.frames != 0) {
    const std::uint64_t base = info.frames;
    const std::uint64_t e = static_cast<std::uint64_t>(info.loop_start) + info.loop_len;
    const std::uint32_t ps = div_round(65535ull * info.loop_start, base);
    const std::uint32_t pe = div_round(65535ull * e, base);
    const bool exact = div_round(static_cast<std::uint64_t>(ps) * info.frames, 65535) == info.loop_start &&
                       div_round(static_cast<std::uint64_t>(pe) * info.frames, 65535) == e;
    if (pe <= ps) {
      rep.flags |= med::kInstLoopQuantised;  // the positions cannot tell the ends apart: no loop
    } else {
      sp.loop_start = static_cast<std::uint16_t>(ps);
      sp.loop_end = static_cast<std::uint16_t>(pe);
      sp.play_mode = 1;
      if (!exact) {
        rep.flags |= med::kInstLoopQuantised;
      }
    }
  }
  return true;
}

}  // namespace o3os::formats::mod
