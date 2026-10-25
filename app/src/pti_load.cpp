// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/pti_load.h>

#include <o3os/app/import_job.h>
#include <o3os/app/play_mode.h>
#include <o3os/app/sample_bank.h>
#include <o3os/audio/voices.h>

namespace o3os::app {

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the PCM of a .pti is little-endian and is read straight into the samples");

void PtiLoadJob::begin(core::ByteSource& source, model::ModelArena& arena, std::uint32_t file_size, std::int16_t* pcm, std::uint32_t capacity, unsigned slot, const char* label) noexcept {
  source_ = &source;
  dst_ = reinterpret_cast<std::uint8_t*>(pcm);
  capacity_ = capacity;
  file_size_ = file_size;
  frames_ = 0;
  got_ = 0;
  why_ = "";
  name_.assign("LOAD ");
  name_.append(label != nullptr ? label : "");
  formats::ReadParams params;
  params.index = static_cast<std::uint16_t>(slot);
  params.size_hint = file_size;  // the reader stops after the head and says how many frames follow
  params.standalone = true;
  reader_.begin(source, arena, params);
  phase_ = pcm != nullptr ? Phase::Head : Phase::Failed;
  if (pcm == nullptr) {
    why_ = "NO SAMPLE MEMORY";
  }
}

core::JobStatus PtiLoadJob::step(std::uint32_t budget_us) noexcept {
  std::size_t bytes = budget_us > kMaxStepBytes / kBytesPerUs ? kMaxStepBytes : static_cast<std::size_t>(budget_us) * kBytesPerUs;
  bytes = bytes < kMinStepBytes ? kMinStepBytes : bytes;
  if (phase_ == Phase::Head) {
    const formats::Progress p = reader_.step(bytes);
    if (p == formats::Progress::Running) {
      return core::JobStatus::Running;
    }
    if (p == formats::Progress::Failed) {
      why_ = import_failure_text(reader_.report());
      phase_ = Phase::Failed;
      return core::JobStatus::Failed;
    }
    const formats::og::ImportReport& r = reader_.import_report();
    if (r.instruments == 0u || r.pcm_frames == 0u) {
      why_ = "NO AUDIO IN THE FILE";
      phase_ = Phase::Failed;
      return core::JobStatus::Failed;
    }
    if (r.pcm_frames > capacity_) {
      why_ = "THE SAMPLE ARENA IS FULL";
      phase_ = Phase::Failed;
      return core::JobStatus::Failed;
    }
    frames_ = r.pcm_frames;
    phase_ = Phase::Pcm;
    return core::JobStatus::Running;
  }
  if (phase_ == Phase::Pcm) {
    const std::size_t total = 2u * static_cast<std::size_t>(frames_);
    while (bytes != 0u && got_ < total) {
      const std::size_t ask = total - got_ < bytes ? total - got_ : bytes;
      const core::IoResult r = source_->read(dst_ + got_, ask);
      if (r.status == core::IoStatus::WouldBlock) {
        return core::JobStatus::Running;
      }
      if (r.status != core::IoStatus::Ok || r.count == 0u || r.count > ask) {
        why_ = r.status == core::IoStatus::End ? "TRUNCATED" : "READ ERROR";
        phase_ = Phase::Failed;
        return core::JobStatus::Failed;
      }
      got_ += r.count;
      bytes -= r.count;
    }
    if (got_ >= total) {
      phase_ = Phase::Done;
      return core::JobStatus::Done;
    }
    return core::JobStatus::Running;
  }
  return phase_ == Phase::Done ? core::JobStatus::Done : core::JobStatus::Failed;
}

unsigned PtiLoadJob::progress() const noexcept {
  if (phase_ == Phase::Done) {
    return core::kJobProgressMax;
  }
  const std::uint64_t head = formats::og::kInstrumentBytes;
  const std::uint64_t total = head + 2u * static_cast<std::uint64_t>(frames_ != 0u ? frames_ : (file_size_ > head ? (file_size_ - head) / 2u : 0u));
  const std::uint64_t done = (phase_ == Phase::Head ? 0u : head) + got_;
  return core::permille_of(done, total);
}

std::uint8_t level_1114_to_12(unsigned level, bool& clamped) noexcept {
  clamped = false;
  if (level == 0u) {
    return 0;
  }
  if (level > 100u) {
    level = 100u;
  }
  if (level < 40u) {  // under -24 dB: the 1.2+ law's quietest level is 1, -24 dB
    clamped = true;
    return 1;
  }
  // dB = 0.4 (level - 100); the 1.2+ level is 50 + dB / 0.48 = 50 + (level - 100) x 5 / 6, to the nearest (halves away from zero); level 40 is -24 dB = level 1 exactly (50 - 50 = 0 would be silence)
  const int d = (static_cast<int>(level) - 100) * 5;
  const int n = 50 + (d >= 0 ? (d + 3) / 6 : -((-d + 3) / 6));
  return static_cast<std::uint8_t>(n < 1 ? 1 : n);
}

PtiCheck pti_check(const model::Project& project, const SampleBank& bank, unsigned slot, const model::Instrument& in) noexcept {
  PtiCheck c;
  const std::uint8_t engine = bank.engine_for(slot, in);
  if (engine == audio::kEngineBits12 || engine == audio::kEngineBits8) {
    c.verdict = PtiVerdict::BitEngine;
    return c;
  }
  const std::uint32_t frames = bank.pcm(slot).frames;
  const std::uint32_t rate = in.sample_rate;
  if (engine == audio::kEngineAmiga) {
    if (in.sampler.play_mode > 1u) {  // what Paula plays: a one shot or a forward loop
      c.verdict = PtiVerdict::AmigaEngine;
      return c;
    }
    c.plan = og_pcm_plan_amiga(frames);
    c.source_hz = amiga_rate_hz(kAmigaPeriod8);
    c.fine = og_amiga_fine(in.sampler.fine_tune);
  } else if (rate != 0u && rate != kOgRateHz && in.sampler.play_mode != kPlayModeWavetable) {
    c.plan = og_pcm_plan(frames, rate);
    c.source_hz = rate;
  } else {
    c.plan = og_pcm_plan(frames, 0u);  // as it is
  }
  if (c.plan.status == OgPcmStatus::TooLong) {
    c.verdict = PtiVerdict::TooLong;
    return c;
  }
  if (project.volume_law() != 0u) {
    bool clamped = false;
    const std::uint8_t level = level_1114_to_12(in.sampler.volume, clamped);
    if (clamped) {
      c.verdict = PtiVerdict::LevelClamps;
      return c;
    }
    c.level = level;
  }
  return c;
}

}  // namespace o3os::app
