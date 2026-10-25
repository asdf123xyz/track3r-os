// SPDX-License-Identifier: GPL-3.0-or-later
// The tracker FX of the Modern engine: see audio/tracker_fx.h.
#include <o3os/audio/tracker_fx.h>

#include <o3os/core/fast_math.h>

namespace o3os::audio {

namespace trkfx {

std::int32_t offset_q16(std::int32_t note_period, std::int32_t period) noexcept {
  if (note_period == period) {
    return 0;
  }
  // The voice's pitch law is linear BETWEEN two semitones (voices.h tuned_inc_q32), so a fraction of a semitone is a fraction of the way from one increment to the next, not of the
  // exponent. The offset that gives the frequency ratio rho = note_period / period exactly is therefore the whole semitones w = floor(12 log2 rho) and the fraction g that makes
  // 1 + g (2^(1/12) - 1) equal rho / 2^(w/12): one log2 and one exp2 (core/fast_math.h), no libm. Without this a quarter-step slide of 0.33 semitone would be 0.65 cent off.
  const float x = core::log2_fast(static_cast<float>(note_period) / static_cast<float>(period)) * 12.0f;  // semitones
  std::int32_t w = static_cast<std::int32_t>(x);
  if (static_cast<float>(w) > x) {
    --w;  // floor
  }
  const float rho = core::exp2_fast((x - static_cast<float>(w)) * (1.0f / 12.0f));  // 1 .. 2^(1/12): the part of the ratio inside the semitone
  const float g = (rho - 1.0f) * 16.8179283f;                                         // 1 / (2^(1/12) - 1)
  const float frac = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g);
  return w * 65536 + static_cast<std::int32_t>(frac * 65536.0f + 0.5f);
}

}  // namespace trkfx

void TrackerFx::reset(VoicePool& voices) noexcept {
  for (unsigned track = 0; track < kTracks; ++track) {
    Trk& k = trk_[track];
    if ((k.arp_on || k.gated) && k.age != 0u) {  // what the processor holds on a voice for one tick only (an arpeggio's raised pitch, a tremor's gate) is given back, not left on a sounding note
      VoicePool::TrackerView view;
      if (voices.tracker_view(track, view) && view.age == k.age) {
        voices.tracker_set(track, trkfx::offset_q16(k.note_period, k.period), k.gated ? static_cast<int>(k.vol) : -1);
      }
    }
    k = Trk();
  }
  voices.tracker_reset();
  end_volumes(true);  // (after the states went: the tracks' channel volumes are full again)
  if (graph_ != nullptr) {
    graph_->clear_step_sends(0u);  // OS-PERFFX P3: the step FX 30 / 32 held end with the run (the sequencer's table was reset); the Perform layer's offsets stay. An export's start and end (Engine::offline_quiet) clear them with ramp 1 first, so this finds none
  }
}

// CF3: channel and global volume back to full and the panbrello's offset off, on the strips. Callers have set the states to their reset values (reset) or do so here (a stop). The Perform layer's own
// strip values (Strip::perf_gain, perf_pan; OS-PERFFX) are not the tracker's: this leaves them.
// now: the strips' gains go to full at once (the transport start). A stop (now false) leaves a gain that is below full on its strip, noted in gain_stale_: the release tails of the stopped
// voices keep ringing at the level they had (the gain snapping up would swell them), and the track's next note (note_started) gives the strip its full gain.
void TrackerFx::end_volumes(bool now) noexcept {
  const unsigned gv = gvol_;
  gvol_ = 64;
  for (unsigned track = 0; track < kTracks; ++track) {
    Trk& k = trk_[track];
    if (!now && static_cast<unsigned>(k.chvol) * gv != 4096u) {
      gain_stale_ |= 1u << track;
    }
    k.chvol = 64;
    k.pb_on = false;
    if (graph_ != nullptr) {
      if (now) {
        graph_->set_track_gain(track, 1.0f, 0u);
      }
      graph_->set_track_pan_mod(track, 0.0f, 0u);
    }
  }
  if (now) {
    gain_stale_ = 0;
  }
}

// The strip's gain of a track: channel volume x global volume, 0..1 (single precision, a multiply of two 0..64 integers and a constant).
void TrackerFx::gain_to(unsigned track, const Trk& k) noexcept {
  if (graph_ != nullptr) {
    graph_->set_track_gain(track, static_cast<float>(static_cast<unsigned>(k.chvol) * gvol_) * (1.0f / 4096.0f), 0u);
  }
}

void TrackerFx::gains_all() noexcept {
  for (unsigned track = 0; track < kTracks; ++track) {
    gain_to(track, trk_[track]);
  }
}

// The graph's pan of the track: the processor's held pan (Channel pan's 0..100 as it was given, else the slide's 0..256 scaled to 0..100).
void TrackerFx::pan_to(unsigned track, Trk& k, std::uint32_t ramp) noexcept {
  if (graph_ != nullptr) {
    graph_->set_track_pan(track, k.pan100 >= 0 ? static_cast<float>(k.pan100) : static_cast<float>(k.pan) * (100.0f / static_cast<float>(trkfx::kPanMax)), ramp);
  }
}

// The processor takes the pan over for a slide: from the Panning FX's when it is held (the slide replaces that FX), else from where the strip plays it now (the strip's own, plus the instrument's offset).
void TrackerFx::own_pan(unsigned track, Trk& k) noexcept {
  if (k.fx_on) {
    k.pan = static_cast<std::int16_t>((static_cast<unsigned>(k.fx_pan) * trkfx::kPanMax + 50u) / 100u);
    k.pan100 = static_cast<std::int8_t>(k.fx_pan);
    k.pan_owned = true;
    k.fx_on = false;
  } else if (!k.pan_owned) {
    k.pan = graph_ != nullptr ? static_cast<std::int16_t>(graph_->track_pan(track) * (static_cast<float>(trkfx::kPanMax) / 100.0f) + 0.5f) : std::int16_t{128};
    k.pan100 = -1;
    k.pan_owned = true;
  }
}

// The Panning FX ended (a ModEnd): the track plays the pan the processor holds, else the last instrument's.
void TrackerFx::panning_fx_end(unsigned track, Trk& k) noexcept {
  k.fx_on = false;
  if (graph_ != nullptr) {
    if (k.pan_owned) {
      pan_to(track, k, 0u);
    } else {
      graph_->set_voice_pan(track, k.inst_pan, 0u);
    }
  }
}

void TrackerFx::note_started(unsigned track, unsigned instrument_pan) noexcept {
  if (track >= kTracks) {
    return;
  }
  Trk& k = trk_[track];
  if (((gain_stale_ >> track) & 1u) != 0u) {  // CF3: a stop left this strip's gain below full (end_volumes): the note starts at full
    gain_stale_ &= ~(1u << track);
    if (graph_ != nullptr) {
      graph_->set_track_gain(track, 1.0f, 1u);
    }
  }
  k.inst_pan = static_cast<std::uint8_t>(instrument_pan > 100u ? 100u : instrument_pan);
  if (k.fx_on) {
    if (graph_ != nullptr) {
      graph_->set_track_pan(track, static_cast<float>(k.fx_pan), 1u);  // the note's start is instant (Graph::set_voice_pan's own ramp)
    }
  } else if (k.pan_owned) {
    pan_to(track, k, 1u);
  }
}

void TrackerFx::event(const SeqEvent& e, VoicePool& voices) noexcept {
  switch (e.kind) {
    case EventKind::TrkFx:
      if (e.track < kTracks && (e.a == 0u || (e.a >= kFxTrkFirst && e.a <= kFxTrkLast))) {
        Trk& k = trk_[e.track];
        const unsigned col = e.flags & 1u;
        std::uint8_t type = e.a;
        if (type == kFxTrkChanPan) {  // set once, held: the track's pan until another one (the column holds nothing to run); it replaces the Panning FX
          const unsigned pan100 = e.b > 100u ? 100u : e.b;
          k.pan = static_cast<std::int16_t>((pan100 * trkfx::kPanMax + 50u) / 100u);
          k.pan100 = static_cast<std::int8_t>(pan100);
          k.pan_owned = true;
          k.fx_on = false;
          pan_to(e.track, k, 0u);
          type = 0;
        } else if (type == kFxTrkChanVol) {  // CF3: set once, held (the column holds nothing to run)
          k.chvol = static_cast<std::uint8_t>(e.b > 64u ? 64u : e.b);
          gain_to(e.track, k);
          type = 0;
        } else if (type == kFxTrkGlobalVol) {
          gvol_ = static_cast<std::uint8_t>(e.b > 64u ? 64u : e.b);
          gains_all();
          type = 0;
        } else if (type == kFxTrkPanbrello) {  // the nibbles' memory (libopenmpt's Panbrello): a zero nibble keeps the track's last
          if ((e.b & 15u) != 0u) {
            k.pb_depth = e.b & 15u;
          }
          if ((e.b >> 4) != 0u) {
            k.pb_speed = e.b >> 4;
          }
        } else if (type == kFxTrkVibWave || type == kFxTrkTremWave) {  // set once, kept: the column holds nothing to run
          voices.tracker_wave(e.track, type == kFxTrkVibWave ? 0u : 1u, e.b & 3u);
          type = 0;
        } else if (type == kFxTrkGliss) {
          voices.tracker_gliss(e.track, e.b != 0u);
          type = 0;
        }
        k.cur_type[col] = type;  // 0 ends the column's effect
        k.cur_value[col] = type != 0u ? e.b : std::uint8_t{0};
      }
      break;
    case EventKind::ModSet:  // the Panning FX (31): the track's absolute pan, 0..100, while the sequencer holds the FX (again at each note: note_started)
      if (e.flags == kModGroupVolume) {  // OS-PERFFX P2: only the performance layer's VOLUME is the processor's to pass on (the group's step FX are the sequencer's)
        if (graph_ != nullptr) {
          graph_->set_perf_gain(e.track, e.c == kLayerPerf ? e.value : 0, 0u);
        }
      } else if (e.flags == kModGroupDelaySend || e.flags == kModGroupReverbSend) {  // OS-PERFFX P3: the step FX under whichever layer won, and the layer's offset
        if (graph_ != nullptr) {
          const unsigned which = e.flags == kModGroupReverbSend ? 1u : 0u;
          graph_->set_perf_send(e.track, which, e.c == kLayerPerf ? e.value : 0, 0u);
          graph_->set_step_send(e.track, which, e.a == (which == 0u ? kFxDelaySend : kFxReverbSend) ? static_cast<int>(e.b) : -1, 0u);  // (a is 0 when only the layer holds the group)
        }
      } else if (e.flags == kModGroupPanning && e.track < kTracks) {
        Trk& k = trk_[e.track];
        if (graph_ != nullptr) {
          graph_->set_perf_pan(e.track, e.c == kLayerPerf ? e.value : 0, 0u);  // the layer's PANNING: its value, or none when a step layer is the winner
        }
        if (e.a == kFxPanning) {  // the step FX applies whichever layer won: under the performance layer it is the pan the layer's value is added to
          k.fx_pan = static_cast<std::uint8_t>(e.b > 100u ? 100u : e.b);
          k.fx_on = true;
          if (graph_ != nullptr) {
            graph_->set_track_pan(e.track, static_cast<float>(k.fx_pan), 0u);
          }
        } else if (e.c == kLayerPerf && k.fx_on) {  // the layer alone: the step FX ended under it (the control table sends the winner again with nothing underneath)
          panning_fx_end(e.track, k);
        }
      }
      break;
    case EventKind::ModEnd:  // the group has no source left: the layer's value goes, and the Panning FX ended (its next note step, FX Off, or another FX in its column: the sequencer's ModEnd)
      if (e.flags == kModGroupVolume) {
        if (graph_ != nullptr) {
          graph_->set_perf_gain(e.track, 0, 0u);
        }
      } else if (e.flags == kModGroupDelaySend || e.flags == kModGroupReverbSend) {  // OS-PERFFX P3: the group has no source left: the instrument's send alone
        if (graph_ != nullptr) {
          const unsigned which = e.flags == kModGroupReverbSend ? 1u : 0u;
          graph_->set_perf_send(e.track, which, 0, 0u);
          graph_->set_step_send(e.track, which, -1, 0u);
        }
      } else if (e.flags == kModGroupPanning && e.track < kTracks) {
        if (graph_ != nullptr) {
          graph_->set_perf_pan(e.track, 0, 0u);
        }
        if (trk_[e.track].fx_on) {
          panning_fx_end(e.track, trk_[e.track]);
        }
      }
      break;
    case EventKind::Tick:
      tick(e.a, voices);
      break;
    case EventKind::FxReset:
      reset(voices);
      break;
    case EventKind::Stopped:  // CF3: a stopped song leaves no fade or channel volume on the strips (the pads would play silent after a song that ends at volume 0)
      end_volumes(false);
      break;
    default:
      break;
  }
}

void TrackerFx::tick(unsigned index, VoicePool& voices) noexcept {
  for (unsigned track = 0; track < kTracks; ++track) {
    Trk& k = trk_[track];
    if ((k.cur_type[0] | k.cur_type[1]) == 0u && !k.arp_on && !k.gated && !k.pb_on) {
      continue;  // nothing to run, nothing to take away
    }
    VoicePool::TrackerView view;
    const bool has_voice = voices.tracker_view(track, view);
    if (has_voice && view.age != k.age) {  // another note than the one the state follows: it starts over from the note
      k.age = view.age;
      k.note = view.note;
      k.vol = view.volume;
      k.vol_given = view.volume;
      k.rt_cnt = 0;
      k.note_period = trkfx::note_period_at(view.note, view.rate_hz);  // (OS-BUG-MODPLAY F4: at the sample's rate)
      k.period = k.note_period;
      k.pitch_given = 0;
      k.arp_on = false;
      k.gated = false;
    }
    // What does not need the voice: the pan, the retrigger's counter, the tremor's position.
    bool fire = false;
    unsigned fire_x = 0;
    bool gate = false;
    bool pb_active = false;
    for (unsigned col = 0; col < 2u; ++col) {
      const std::uint8_t v = k.cur_value[col];
      switch (k.cur_type[col]) {
        case kFxTrkPanSlide: {
          const int d = trkfx::pan_step(v, index);
          if (d != 0) {
            own_pan(track, k);
            k.pan100 = -1;
            const int p = k.pan + d;
            k.pan = static_cast<std::int16_t>(p < 0 ? 0 : (p > trkfx::kPanMax ? trkfx::kPanMax : p));
            pan_to(track, k, 0u);
          }
          break;
        }
        case kFxTrkChanVolSlide: {  // CF3, no voice needed: the channel volume is the strip's
          const int d = trkfx::chan_vol_step(v, index);
          if (d != 0) {
            const unsigned nv = static_cast<unsigned>(trkfx::clamp_volume(k.chvol + d));
            if (nv != k.chvol) {
              k.chvol = static_cast<std::uint8_t>(nv);
              gain_to(track, k);
            }
          }
          break;
        }
        case kFxTrkGlobalVolSlide: {
          const int d = trkfx::global_vol_step(v, index);
          if (d != 0) {
            const unsigned nv = static_cast<unsigned>(trkfx::clamp_volume(gvol_ + d));
            if (nv != gvol_) {
              gvol_ = static_cast<std::uint8_t>(nv);
              gains_all();  // (the gain of every track follows: the loop's later tracks read the new value too)
            }
          }
          break;
        }
        case kFxTrkPanbrello: {
          const int d = trkfx::panbrello_delta(k.pb_pos, k.pb_depth);
          k.pb_pos = static_cast<std::uint8_t>(k.pb_pos + k.pb_speed);
          pb_active = true;
          k.pb_on = true;
          if (graph_ != nullptr) {
            graph_->set_track_pan_mod(track, static_cast<float>(d) * (100.0f / static_cast<float>(trkfx::kPanMax)), 0u);
          }
          break;
        }
        case kFxTrkRetrig: {
          const unsigned y = v & 15u;  // libopenmpt's count: on every tick that carries the effect, a restart when it has reached y, then it counts that tick; it is kept by rows without the effect
          if (y != 0u) {
            if (k.rt_cnt >= y) {
              k.rt_cnt = 0;
              fire = true;
              fire_x = v >> 4;
            }
            if (k.rt_cnt < 255u) {
              ++k.rt_cnt;
            }
          }
          break;
        }
        case kFxTrkTremor: {
          const unsigned on = (static_cast<unsigned>(v) >> 4) + 1u;
          const unsigned period = on + (v & 15u) + 1u;
          const unsigned pos = k.trem_pos < period ? k.trem_pos : 0u;
          gate = pos >= on;
          k.trem_pos = static_cast<std::uint8_t>(pos + 1u >= period ? 0u : pos + 1u);
          break;
        }
        default:
          break;
      }
    }
    if (k.pb_on && !pb_active) {  // the row stopped carrying the panbrello: the pan is the one in force again
      k.pb_on = false;
      if (graph_ != nullptr) {
        graph_->set_track_pan_mod(track, 0.0f, 0u);
      }
    }
    if (!has_voice) {
      k.age = 0;
      k.arp_on = false;
      k.gated = false;
      continue;  // no sampler voice on the track
    }
    unsigned arp = 0;
    int vol = k.vol;
    std::int64_t period = k.period;
    for (unsigned col = 0; col < 2u; ++col) {
      const std::uint8_t v = k.cur_value[col];
      switch (k.cur_type[col]) {
        case kFxTrkArp:
          arp = trkfx::arp_semitones(v, index);
          break;
        case kFxTrkVolSlide:
          if (index != 0u) {
            vol = trkfx::clamp_volume(vol + trkfx::slide_step(v));
          }
          break;
        case kFxTrkFineVol:
          if (index == 0u) {
            vol = trkfx::clamp_volume(vol + trkfx::slide_step(v));
          }
          break;
        case kFxTrkFinePortaUp:
          if (index == 0u) {
            period -= v;
          }
          break;
        case kFxTrkFinePortaDown:
          if (index == 0u) {
            period += v;
          }
          break;
        default:
          break;
      }
    }
    if (fire) {
      vol = trkfx::retrig_volume(vol, fire_x);
    }
    k.vol = static_cast<std::uint8_t>(vol);
    k.period = trkfx::clamp_period(period);
    const std::int32_t pitch = static_cast<std::int32_t>(arp) * 65536 + trkfx::offset_q16(k.note_period, k.period);
    const bool pitch_changed = pitch != k.pitch_given;
    const unsigned out = gate ? 0u : k.vol;  // the tremor's off ticks give the voice 0 and keep the channel's volume
    const bool vol_changed = out != k.vol_given;
    k.arp_on = arp != 0u;
    k.gated = gate;
    if (pitch_changed || vol_changed) {
      voices.tracker_set(track, pitch_changed ? pitch : VoicePool::kTrackerKeep, vol_changed ? static_cast<int>(out) : -1);
      k.pitch_given = pitch;
      k.vol_given = static_cast<std::uint8_t>(out);
    }
    if (fire) {
      voices.tracker_retrigger(track);
    }
  }
}

}  // namespace o3os::audio
