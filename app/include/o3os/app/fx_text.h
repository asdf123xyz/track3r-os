// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The value of a pattern step FX in words (OS-PFX; docs/ui/ui-map.md section 4: "a value formatter each for the popup (chord names, arp patterns)"): what the Pattern screen's info line
// says about the FX under the cursor, so that a hex byte (the grid's own text) is also a chord, an arpeggio, a tuning or a distance. Only the FX whose value is not a plain number have one:
//   Chord 14   "CHORD MAJ7 47A" / "CHORD 28" / "CHORD OFF"   the 30 chords (seq/fx.h kChordMask): the intervals above the root as a label (one hex digit each: 47A is 4, 7, 10), and
//              the name where this OS gives one (11 of the 29: docs/specs/OS-PFX.md)
//   Arp 21     "ARP UP 8 STEPS" / "ARP DOWN 1/2 STEP" / "ARP RND 1 STEP" / "ARP OFF"    direction and the time between two notes (seq/fx.h kArpPeriod: 384 .. 6 microsteps of 48 a step)
//   Tune 40    "TUNE +7 ST" / "TUNE 0 ST" / "TUNE -12 ST"
//   Slide 41/42 "SLIDE UP 2.19 ST" / "SLIDE DOWN 0.50 ST"  the distance in semitones to two places (the value is sixteenths of a semitone)
//   Glide 19   "GLIDE OFF" / "GLIDE 240 MS" / "GLIDE 1.28 S"   the time the value means (audio/events.h glide_fx_ms)
//   Overdrive 38 "DRIVE 40 %" / "DRIVE OFF"   Bit depth 39 "BITS 8" / "BITS 16 (OFF)"   (OS-IFX1)
//   Tracker Arp 46 "ARPEG +3 +7" / "ARPEG OFF"   Volume slide 47 "VSLIDE +4" / "VSLIDE -4" / "VSLIDE 0"   Fine volume slide 48 "FVOL +2" / "FVOL -2" / "FVOL 0"
//   Fine porta 49 / 50 "FINE UP 1.00 PER" / "FINE DOWN 0.25 PER"   the Paula period units (the value is quarters of one)   (OS-COREFX; x is the high nibble, y the low one)
//   CF2: Pan slide 51 "PANSL R 4" / "PANSL L 4" / "PANSL FINE R 3" / "PANSL 0"   Vibrato / Tremolo waveform 52 / 53 "VIB SINE" / "VIB RAMP" / "TREM SQUARE" / "TREM RANDOM NORETRIG"
//   Glissando 54 "GLISS ON" / "GLISS OFF"   Retrigger 55 "RETRIG EVERY 3" / "RETRIG EVERY 2 VOL -4" / "RETRIG EVERY 1 VOL X1/2"   Tremor 56 "TREMOR ON 2 OFF 1"   Channel pan 57 "CHPAN 75"
//   CF3: Channel volume 58 "CHVOL 48"   Channel volume slide 59 "CHVSL -3" / "CHVSL FINE +2" / "CHVSL 0"   Global volume 60 "GVOL 64"   Global volume slide 61 "GVSL +4" / "GVSL FINE -1"   Panbrello 62 "PANBR SPD 3 DEP 5" / "PANBR SPD 3 DEP KEEP"
// The words are this OS's (the OG's own text is not reused). Pure, no heap; the value is read as the sequencer reads it (clamped to the type's range).

#include <o3os/app/text_format.h>
#include <o3os/audio/events.h>
#include <o3os/core/fixed_string.h>
#include <o3os/seq/fx.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

// The name of the FX picker's entry `index` (model/fx_table.h kPickerOrder order, 0 is "None"; an index past the list is None): "Micro-tune", "Random Instrument". The step popup's list and the Fill dialog's FX TYPE row (OS-FILL2) read the one table
// (app/src/screens/step_popup_overlay.cpp).
const char* fx_picker_name(unsigned index) noexcept;

// The name of a chord value, where this OS gives one (docs/specs/OS-PFX.md); nullptr for the other 18.
constexpr const char* chord_name(std::uint8_t value) noexcept {
  switch (value) {
    case 1: return "SUS2";
    case 4: return "MIN";
    case 5: return "MAJ";
    case 7: return "SUS4";
    case 9: return "OPEN4";
    case 11: return "OPEN5";
    case 19: return "MIN6";
    case 20: return "MIN7";
    case 22: return "MAJ6";
    case 23: return "MAJ7";
    case 24: return "MAJMAJ7";
    default: return nullptr;
  }
}
// By arp rate (seq/fx.h kArpPeriod): the steps between two notes.
inline constexpr const char* kArpRateNames[seq::kArpRates] = {"8", "6", "4", "3", "2", "1", "1/2", "1/3", "1/4", "1/6", "1/8"};

// Appends the words for (type, value); false and nothing appended for a type without a formatter.
template <std::size_t N>
bool append_fx_value(core::FixedString<N>& s, std::uint8_t type, std::uint8_t value) noexcept {
  switch (type) {
    case seq::fx::kChord: {
      const std::uint8_t v = value < seq::kChordCount ? value : static_cast<std::uint8_t>(seq::kChordCount - 1u);  // above the range reads as the last: fx_sanitize holds the sequencer's reading there
      s.append("CHORD ");
      if (v == 0u) {
        s.append("OFF");
        return true;
      }
      const char* name = chord_name(v);
      if (name != nullptr) {
        s.append(name);
        s.append(" ");
      }
      for (unsigned b = 1; b < 15u; ++b) {  // one hex digit per offset above the root
        if (((seq::kChordMask[v] >> b) & 1u) != 0u) {
          s.append(&"0123456789ABCDEF"[b], 1);
        }
      }
      return true;
    }
    case seq::fx::kArp: {
      const std::uint8_t v = value > 33u ? std::uint8_t{33} : value;
      s.append("ARP ");
      if (v == 0u) {
        s.append("OFF");
        return true;
      }
      const seq::ArpDir dir = seq::arp_dir(v);
      const std::uint32_t rate = seq::arp_index(v) % seq::kArpRates;
      s.append(dir == seq::ArpDir::Up ? "UP " : (dir == seq::ArpDir::Down ? "DOWN " : "RND "));
      s.append(kArpRateNames[rate]);
      s.append(rate < 5u ? " STEPS" : " STEP");
      return true;
    }
    case seq::fx::kTune: {
      const int st = seq::tune_semitones(value);
      s.append("TUNE ");
      if (st > 0) {
        s.append("+");
      } else if (st < 0) {
        s.append("-");
      }
      append_uint(s, static_cast<unsigned>(st < 0 ? -st : st));
      s.append(" ST");
      return true;
    }
    case seq::fx::kSlideUp:
    case seq::fx::kSlideDown: {
      const unsigned hundredths = (static_cast<unsigned>(value) * 25u + 2u) / 4u;  // value / 16 semitones, in hundredths, rounded
      s.append(type == seq::fx::kSlideUp ? "SLIDE UP " : "SLIDE DOWN ");
      append_uint(s, hundredths / 100u);
      s.append(".");
      append_dec(s, hundredths % 100u, 2);
      s.append(" ST");
      return true;
    }
    case seq::fx::kGlide: {
      const std::uint32_t ms = audio::glide_fx_ms(value);
      s.append("GLIDE ");
      if (ms == 0u) {
        s.append("OFF");
      } else if (ms < 1000u) {
        append_uint(s, ms);
        s.append(" MS");
      } else {
        append_uint(s, ms / 1000u);
        s.append(".");
        append_dec(s, (ms % 1000u) / 10u, 2);
        s.append(" S");
      }
      return true;
    }
    case seq::fx::kOverdrive: {
      const unsigned v = value > 100u ? 100u : value;
      if (v == 0u) {
        s.append("DRIVE OFF");
      } else {
        s.append("DRIVE ");
        append_uint(s, v);
        s.append(" %");
      }
      return true;
    }
    case seq::fx::kBitDepth: {
      const unsigned v = value < 1u ? 1u : (value > 16u ? 16u : value);  // fx_sanitize holds the sequencer's reading to 1..16
      s.append("BITS ");
      append_uint(s, v);
      if (v == 16u) {
        s.append(" (OFF)");
      }
      return true;
    }
    case seq::fx::kTrkArp: {
      if (value == 0u) {
        s.append("ARPEG OFF");
        return true;
      }
      s.append("ARPEG +");
      append_uint(s, value >> 4);
      s.append(" +");
      append_uint(s, value & 15u);
      return true;
    }
    case seq::fx::kTrkVolSlide:
    case seq::fx::kTrkFineVol: {
      const unsigned up = value >> 4;    // the engine reads x first: x up, else y down (audio/tracker_fx.h)
      const unsigned down = value & 15u;
      s.append(type == seq::fx::kTrkVolSlide ? "VSLIDE " : "FVOL ");
      if (up != 0u) {
        s.append("+");
        append_uint(s, up);
      } else if (down != 0u) {
        s.append("-");
        append_uint(s, down);
      } else {
        s.append("0");
      }
      return true;
    }
    case seq::fx::kTrkFinePortaUp:
    case seq::fx::kTrkFinePortaDown: {
      const unsigned hundredths = static_cast<unsigned>(value) * 25u;  // value / 4 period units
      s.append(type == seq::fx::kTrkFinePortaUp ? "FINE UP " : "FINE DOWN ");
      append_uint(s, hundredths / 100u);
      s.append(".");
      append_dec(s, hundredths % 100u, 2);
      s.append(" PER");
      return true;
    }
    case seq::fx::kTrkPanSlide: {
      const unsigned x = value >> 4;  // the engine reads y (right) first, then x (left); Fy (y < F) and xF are the fine forms, once (audio/tracker_fx.h)
      const unsigned y = value & 15u;
      s.append("PANSL ");
      if (x == 15u && y != 0u && y != 15u) {
        s.append("FINE R ");
        append_uint(s, y);
      } else if (y == 15u && x != 0u) {
        s.append("FINE L ");
        append_uint(s, x);
      } else if (y != 0u) {
        s.append("R ");
        append_uint(s, y);
      } else if (x != 0u) {
        s.append("L ");
        append_uint(s, x);
      } else {
        s.append("0");
      }
      return true;
    }
    case seq::fx::kTrkVibWave:
    case seq::fx::kTrkTremWave: {
      static constexpr const char* kWave[4] = {"SINE", "RAMP", "SQUARE", "RANDOM"};
      s.append(type == seq::fx::kTrkVibWave ? "VIB " : "TREM ");
      s.append(kWave[value & 3u]);
      if ((value & 4u) != 0u) {
        s.append(" NORETRIG");
      }
      return true;
    }
    case seq::fx::kTrkGliss:
      s.append(value != 0u ? "GLISS ON" : "GLISS OFF");
      return true;
    case seq::fx::kTrkRetrig: {
      static constexpr const char* kVol[16] = {"", " VOL -1", " VOL -2", " VOL -4", " VOL -8", " VOL -16", " VOL X2/3", " VOL X1/2",
                                                "", " VOL +1", " VOL +2", " VOL +4", " VOL +8", " VOL +16", " VOL X3/2", " VOL X2"};
      s.append("RETRIG EVERY ");
      append_uint(s, value & 15u);
      s.append(kVol[value >> 4]);
      return true;
    }
    case seq::fx::kTrkChanPan:
      s.append("CHPAN ");
      append_uint(s, value > 100u ? 100u : value);
      return true;
    case seq::fx::kTrkChanVol:
    case seq::fx::kTrkGlobalVol:
      s.append(type == seq::fx::kTrkChanVol ? "CHVOL " : "GVOL ");
      append_uint(s, value > 64u ? 64u : value);
      return true;
    case seq::fx::kTrkChanVolSlide:
    case seq::fx::kTrkGlobalVolSlide: {
      const unsigned x = value >> 4;  // audio/tracker_fx.h: both read Fy / xF as the fine forms, once; plain, the channel's slide goes down first (y), the global one up first (x), as libopenmpt's
      const unsigned y = value & 15u;
      const bool chan = type == seq::fx::kTrkChanVolSlide;
      s.append(chan ? "CHVSL " : "GVSL ");
      if ((y == 15u && x != 0u) || (x == 15u && y != 0u)) {
        s.append("FINE ");
        s.append(y == 15u ? "+" : "-");
        append_uint(s, y == 15u ? x : y);
        return true;
      }
      const unsigned up = chan ? (y == 0u ? x : 0u) : x;
      const unsigned down = chan ? y : (x == 0u ? y : 0u);
      if (up != 0u) {
        s.append("+");
        append_uint(s, up);
      } else if (down != 0u) {
        s.append("-");
        append_uint(s, down);
      } else {
        s.append("0");
      }
      return true;
    }
    case seq::fx::kTrkPanbrello:
      s.append("PANBR SPD ");
      if ((value >> 4) != 0u) {
        append_uint(s, value >> 4);
      } else {
        s.append("KEEP");
      }
      s.append(" DEP ");
      if ((value & 15u) != 0u) {
        append_uint(s, value & 15u);
      } else {
        s.append("KEEP");
      }
      return true;
    case seq::fx::kTrkTremor:
      s.append("TREMOR ON ");
      append_uint(s, (value >> 4) + 1u);
      s.append(" OFF ");
      append_uint(s, (value & 15u) + 1u);
      return true;
    default:
      return false;
  }
}

}  // namespace o3os::app
