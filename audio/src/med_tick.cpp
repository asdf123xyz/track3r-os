// SPDX-License-Identifier: GPL-3.0-or-later
// The MED tick processor: see audio/med_tick.h. The order of work in a tick is libxmp's (MIT), the arithmetic is the integer kind of it; the golden frame streams of
// tests/data/med_tick_golden.txt are libxmp's.
#include <o3os/audio/med_tick.h>
#include <o3os/audio/paula.h>

namespace o3os::audio::med {
namespace {

// ProTracker's 32-step half sine; the second half of the 64 steps is its negative.
constexpr std::int16_t kSine[32] = {0,   24,  49,  74,  97,  120, 141, 161, 180, 197, 212, 224, 235, 244, 250, 253,
                                    255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120, 97,  74,  49,  24};

int lfo(unsigned pos) noexcept { return (pos & 32u) != 0u ? -kSine[pos & 31u] : kSine[pos & 31u]; }

// The range of a period: Paula's own, 113 (the hardware and the BLEP stop there) to the 16-bit register. libxmp keeps no range for MED (a slide past 113 goes on and is
// silent below 0.25); a real Amiga cannot play it.
constexpr std::int32_t kLo = static_cast<std::int32_t>(paula::kMinPeriod);
constexpr std::int32_t kHi = static_cast<std::int32_t>(paula::kMaxPeriod);

std::int32_t clamp(std::int32_t v, std::int32_t lo, std::int32_t hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

void Track::arm_retrig(unsigned every, bool once) noexcept {
  rt_on_ = true;
  rt_once_ = once;
  rt_val_ = static_cast<std::int16_t>(every);
  rt_count_ = static_cast<std::int16_t>(every + 1u);
}

void Track::apply_note(bool porta, bool set_pitch, Out& out) noexcept {
  const std::uint16_t per = static_cast<std::uint16_t>(paula::note_period(note_.note, finetune_));
  vol_ = note_.volume > 64u ? 64 : note_.volume;
  if (porta && !set_pitch) {
    target_ = per;  // a note under a tone portamento is its target; the voice is not restarted
    if (period_ == 0u) {
      period_ = per;
    }
    return;
  }
  period_ = per;
  target_ = per;
  if (set_pitch) {
    return;  // 0F FD: the pitch changes, the voice goes on
  }
  cur_note_ = static_cast<std::int16_t>(note_.note);
  vib_pos_ = 0;
  trem_pos_ = 0;
  out.start = true;
}

void Track::begin_row(unsigned t, unsigned ticks, Out& out) noexcept {
  (void)t;
  (void)ticks;
  const bool has_note = note_pending_;
  slide_ = 0;
  porta_ = 0;
  porta_dir_ = 0;
  vslide_ = 0;
  vib_ = false;
  trem_ = false;
  arp_ = false;
  arp_i_ = 0;
  fine_vol_ = 0;
  fine_per_ = 0;
  cut_at_ = -1;
  rt_on_ = false;
  bool porta_note = false;
  bool set_pitch = false;
  bool have_hold = false;
  std::uint8_t hold_data = 0;
  std::uint32_t offset = 0;
  bool have_offset = false;
  if (has_note) {
    finetune_ = static_cast<std::int8_t>(note_.finetune < -8 ? -8 : (note_.finetune > 7 ? 7 : note_.finetune));
  }
  // The secondary column is handled first, as libxmp does, so that the primary's flags win.
  for (int col = 1; col >= 0; --col) {
    if (!has_[col]) {
      continue;
    }
    const unsigned d = data_[col];
    const unsigned x = d >> 4;
    const unsigned y = d & 15u;
    const auto vol_slide = [this](unsigned dd) {
      unsigned v = dd;
      if (v == 0u) {
        v = vol_mem_;
      }
      if (v != 0u) {
        vol_mem_ = static_cast<std::uint8_t>(v);
        vslide_ = static_cast<std::int8_t>((v >> 4) != 0u ? static_cast<int>(v >> 4) : -static_cast<int>(v & 15u));
      }
    };
    switch (cmd_[col]) {
      case 0x00:
        if (d != 0u) {
          arp_ = true;
          arp_x_ = static_cast<std::uint8_t>(x);
          arp_y_ = static_cast<std::uint8_t>(y);
        }
        break;
      case 0x01:
        if (d != 0u) {
          slide_ = static_cast<std::int16_t>(-static_cast<int>(d));
        }
        break;
      case 0x02:
        if (d != 0u) {
          slide_ = static_cast<std::int16_t>(d);
        }
        break;
      case 0x03:
        porta_note = true;
        porta_ = static_cast<std::uint16_t>(d != 0u ? d : porta_mem_);
        porta_mem_ = static_cast<std::uint8_t>(porta_);
        break;
      case 0x05:
        porta_note = true;
        porta_ = porta_mem_;
        vol_slide(d);
        break;
      case 0x04:
      case 0x14:
        if (y != 0u) {
          vib_depth_ = static_cast<std::uint8_t>(cmd_[col] == 0x04 ? y << 3 : y << 2);
        }
        if (x != 0u) {
          vib_speed_ = static_cast<std::uint8_t>(x);
        }
        vib_ = true;
        break;
      case 0x06:
        vib_ = true;
        vol_slide(d);
        break;
      case 0x07:
        if (y != 0u) {
          trem_depth_ = static_cast<std::uint8_t>(y);
        }
        if (x != 0u) {
          trem_speed_ = static_cast<std::uint8_t>(x);
        }
        trem_ = true;
        break;
      case 0x08:
        if (has_note) {
          have_hold = true;
          hold_data = static_cast<std::uint8_t>(d);
        }
        break;
      case 0x0A:
      case 0x0D:
        vol_slide(d);
        break;
      case 0x0F:
        switch (d) {
          case 0xF1: arm_retrig(3, true); break;
          case 0xF3: arm_retrig(2, false); break;
          case 0xF8: out.led = 0; break;
          case 0xF9: out.led = 1; break;
          case 0xFD: set_pitch = has_note; break;
          default: break;  // F2 was the delay, taken before the row
        }
        break;
      case 0x11:
        fine_per_ = static_cast<std::int16_t>(-static_cast<int>(d));
        break;
      case 0x12:
        fine_per_ = static_cast<std::int16_t>(d);
        break;
      case 0x15:
        finetune_ = static_cast<std::int8_t>(static_cast<int>((d + 8u) & 15u) - 8);
        break;
      case 0x18:
        cut_at_ = static_cast<std::int16_t>(d > 15u ? 15u : d);
        break;
      case 0x19: {
        const unsigned v = d != 0u ? d : off_mem_;
        off_mem_ = static_cast<std::uint8_t>(v);
        have_offset = true;
        offset = v << 8;
        break;
      }
      case 0x1A:
        fine_vol_ = static_cast<std::int16_t>(d);
        break;
      case 0x1B:
        fine_vol_ = static_cast<std::int16_t>(-static_cast<int>(d));
        break;
      case 0x1F:
        if (has_note && y != 0u) {
          arm_retrig(y, false);  // the delay x is taken before the row (tick())
        }
        break;
      default:
        break;
    }
  }
  if (has_note) {
    apply_note(porta_note, set_pitch, out);
    if (out.start) {
      out.offset = have_offset ? offset : 0u;
      if (note_.hold != 0u) {
        hold_ = note_.hold;
        decay_ = note_.decay;
        hold_active_ = true;
      } else {
        hold_ = -1;
        decay_ = 0;
        hold_active_ = false;
      }
    }
    if (have_hold) {
      const unsigned h = hold_data & 15u;
      if (h == 0u) {
        hold_ = -1;
        decay_ = 0;
        hold_active_ = false;
      } else {
        hold_ = static_cast<std::int16_t>(h);
        decay_ = static_cast<std::uint8_t>(hold_data >> 4);
        hold_active_ = true;
      }
    }
    note_pending_ = false;
  }
  if (porta_ != 0u) {
    porta_dir_ = period_ < target_ ? 1 : -1;
  }
}

void Track::tick(unsigned t, unsigned ticks, const Cfg& cfg, Out& out) noexcept {
  out = Out{};
  if (t == 0u) {
    defer_ = 0;
    unsigned delay = 0;
    for (unsigned col = 0; col < 2u && delay == 0u; ++col) {
      if (!has_[col]) {
        continue;
      }
      if (cmd_[col] == 0x0F && data_[col] == 0xF2) {
        delay = 3;
      } else if (cmd_[col] == 0x1F && (data_[col] >> 4) != 0u && note_pending_) {
        delay = data_[col] >> 4;
      }
    }
    if (delay != 0u) {
      rt_on_ = false;
      if (delay < ticks) {
        defer_ = delay;  // the previous row's commands go on until then
      } else {
        note_pending_ = false;  // a delay as long as the row: the row never happens (as in libxmp)
      }
    } else {
      begin_row(0, ticks, out);
    }
  } else if (defer_ != 0u && t == defer_) {
    defer_ = 0;
    begin_row(t, ticks, out);
  }

  // hold and decay (before the volume slide, as libxmp's extras are)
  if (hold_ == 0) {
    const int dec = decay_ != 0u ? static_cast<int>(decay_) : static_cast<int>(vol_);
    vol_ = static_cast<std::int16_t>(clamp(vol_ - dec, 0, 64));
    if (vol_ == 0) {
      hold_ = -1;
      decay_ = 0;
    }
    hold_active_ = false;
  }
  if (hold_ > 0) {
    --hold_;
  }

  // retrigger and cut
  if (rt_on_) {
    if (--rt_count_ == 0) {
      out.start = true;
      out.offset = 0;
      if (hold_active_) {
        hold_ = static_cast<std::int16_t>(hold_ + static_cast<int>(t));  // OctaMED 3.00 on: a retrigger lengthens the hold by its tick
      }
      rt_count_ = rt_val_;
      if (rt_once_) {
        rt_on_ = false;
      }
    }
  }
  if (cut_at_ >= 0 && static_cast<int>(t) == cut_at_) {
    out.cut = true;
  }

  // volume
  if (t > 0u || cfg.slide_tick0) {
    vol_ = static_cast<std::int16_t>(vol_ + vslide_);
  }
  if (t == 0u) {
    vol_ = static_cast<std::int16_t>(vol_ + fine_vol_);
  }
  vol_ = static_cast<std::int16_t>(clamp(vol_, 0, 64));

  // period
  const int ft = finetune_;
  if (t > 0u || cfg.slide_tick0) {
    if (slide_ != 0) {
      period_ = static_cast<std::uint16_t>(clamp(static_cast<std::int32_t>(period_) + slide_, kLo, kHi));
    }
    if (porta_ != 0u && target_ > 0u && porta_dir_ != 0) {
      if (porta_dir_ > 0) {
        period_ = static_cast<std::uint16_t>(clamp(static_cast<std::int32_t>(period_) + porta_, kLo, kHi));
        if (period_ >= target_) {
          period_ = target_;
          porta_dir_ = 0;
        }
      } else {
        const int v = static_cast<int>(period_) - static_cast<int>(porta_);
        period_ = static_cast<std::uint16_t>(clamp(v, kLo, kHi));
        if (period_ <= target_) {
          period_ = target_;
          porta_dir_ = 0;
        }
      }
    }
  }
  if (t == 0u && fine_per_ != 0) {
    period_ = static_cast<std::uint16_t>(clamp(static_cast<std::int32_t>(period_) + fine_per_, kLo, kHi));
  }
  if (period_ != 0u) {
    period_ = static_cast<std::uint16_t>(clamp(period_, kLo, kHi));
  }

  // what is played: the period with the arpeggio and the vibrato, the volume with the tremolo
  std::int32_t per = period_;
  if (period_ != 0u) {
    if (arp_ && cur_note_ >= 0) {
      const unsigned i = arp_i_ % 3u;
      if (i != 0u) {
        per = static_cast<std::int32_t>(paula::note_period(cur_note_ + static_cast<int>(i == 1u ? arp_x_ : arp_y_), ft));
      }
    }
    ++arp_i_;
    if (vib_) {
      per += (lfo(vib_pos_) * static_cast<int>(vib_depth_)) / 512;
      if (t > 0u) {
        vib_pos_ = static_cast<std::uint8_t>((vib_pos_ + vib_speed_) & 63u);
      }
    }
    per = clamp(per, kLo, kHi);
  }
  std::int32_t v = vol_;
  if (trem_) {
    v += (lfo(trem_pos_) * static_cast<int>(trem_depth_)) / 64;
    if (t > 0u) {
      trem_pos_ = static_cast<std::uint8_t>((trem_pos_ + trem_speed_) & 63u);
    }
  }
  out.period = static_cast<std::uint16_t>(per);
  out.volume = static_cast<std::uint8_t>(clamp(v, 0, 64));
}

}  // namespace o3os::audio::med
