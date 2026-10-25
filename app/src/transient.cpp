// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/transient.h>

#include <o3os/app/wave_math.h>

namespace o3os::app {

std::uint32_t TransientDetector::ratio_q8(unsigned sensitivity) noexcept {
  const unsigned s = sensitivity > 100u ? 100u : sensitivity;
  return 1024u - s * 576u / 100u;  // 4.0 at 0, 1.75 at 100
}

void TransientDetector::begin(const std::int16_t* pcm, std::uint32_t frames, Onset* storage, unsigned sensitivity) noexcept {
  pcm_ = pcm;
  onsets_ = storage;
  frames_ = pcm != nullptr && storage != nullptr ? frames : 0u;
  pos_ = 0;
  ratio_ = ratio_q8(sensitivity);
  prev_energy_ = 0;
  history_n_ = 0;
  history_head_ = 0;
  window_index_ = 0;
  last_window_ = 0;
  have_last_ = false;
  count_ = 0;
  done_ = frames_ == 0u;
}

// The mean of the squares of frames [first, first + n): at most 1 << 30.
std::uint32_t TransientDetector::window_energy(std::uint32_t first, std::uint32_t n) const noexcept {
  if (n == 0u) {
    return 0u;
  }
  std::uint64_t sum = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::int32_t x = pcm_[first + i];
    sum += static_cast<std::uint64_t>(static_cast<std::int64_t>(x) * x);
  }
  return static_cast<std::uint32_t>(sum / n);
}

void TransientDetector::record(std::uint32_t frame, std::uint32_t score) noexcept {
  if (count_ < kMaxOnsets) {
    onsets_[count_++] = Onset{frame, score};
    return;
  }
  unsigned weakest = 0;  // full: the weakest of them goes if this one is stronger (positions are then not in order: choose_slices sorts)
  for (unsigned i = 1; i < kMaxOnsets; ++i) {
    if (onsets_[i].score < onsets_[weakest].score) {
      weakest = i;
    }
  }
  if (score > onsets_[weakest].score) {
    onsets_[weakest] = Onset{frame, score};
  }
}

// A hit in window `w` (its energy is `energy` over `background`): where it starts, refined in blocks over the window before it and itself.
void TransientDetector::found(std::uint32_t w, std::uint32_t energy, std::uint32_t background) noexcept {
  const std::uint32_t first = w == 0u ? 0u : (w - 1u) * kWindow;
  const std::uint32_t end = (w + 1u) * kWindow < frames_ ? (w + 1u) * kWindow : frames_;
  std::uint32_t at = w * kWindow;
  for (std::uint32_t b = first; b < end; b += kBlock) {
    const std::uint32_t n = b + kBlock <= end ? kBlock : end - b;
    const std::uint64_t e = window_energy(b, n);
    if (e >= kEnergyFloor && e * 256u > static_cast<std::uint64_t>(background) * ratio_) {
      at = b;
      break;
    }
  }
  at = at > kBlock ? at - kBlock : 0u;  // a block of pre-roll: the attack is inside the slice
  const std::uint64_t score = static_cast<std::uint64_t>(energy) * 256u / (static_cast<std::uint64_t>(background) + 1u);
  record(at, score > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<std::uint32_t>(score));
}

bool TransientDetector::step(std::uint32_t max_frames) noexcept {
  if (done_) {
    return true;
  }
  std::uint32_t used = 0;
  while (pos_ < frames_ && (used == 0u || used < max_frames)) {
    const std::uint32_t n = frames_ - pos_ < kWindow ? frames_ - pos_ : kWindow;
    if (n < kWindow / 2u && window_index_ != 0u) {
      pos_ = frames_;  // a stub at the end is too short to say anything: ignored
      break;
    }
    const std::uint32_t energy = window_energy(pos_, n);
    if (history_n_ >= 2u) {
      std::uint64_t sum = 0;
      for (unsigned i = 0; i < history_n_; ++i) {
        sum += history_[i];
      }
      const std::uint32_t background = static_cast<std::uint32_t>(sum / history_n_);
      const bool loud = energy >= kEnergyFloor;
      const bool rises = energy > background && energy > prev_energy_;
      const bool spaced = !have_last_ || window_index_ - last_window_ >= kMinGapWindows;
      if (loud && rises && spaced && static_cast<std::uint64_t>(energy) * 256u > static_cast<std::uint64_t>(background) * ratio_) {
        found(window_index_, energy, background);
        last_window_ = window_index_;
        have_last_ = true;
      }
    }
    history_[history_head_] = energy;
    history_head_ = (history_head_ + 1u) % kHistory;
    history_n_ = history_n_ < kHistory ? history_n_ + 1u : kHistory;
    prev_energy_ = energy;
    ++window_index_;
    pos_ += n;
    used += n;
  }
  done_ = pos_ >= frames_;
  return done_;
}

unsigned choose_slices(const TransientDetector& d, unsigned cap, std::uint16_t* out, TransientDetector::Onset* scratch) noexcept {
  cap = cap < 1u ? 1u : (cap > kMaxSliceCount ? kMaxSliceCount : cap);
  using Cand = TransientDetector::Onset;  // frame holds the point here
  Cand* cand = scratch;
  unsigned n = 0;
  for (unsigned i = 0; i < d.count(); ++i) {
    const std::uint32_t p = frame_to_point(d.onset(i).frame, d.frames());
    if (scratch == nullptr || p == 0u || p >= kPointMax) {
      continue;  // slice 0 is point 0 already, and a start at the very end plays nothing
    }
    bool dup = false;
    for (unsigned k = 0; k < n; ++k) {
      dup = dup || cand[k].frame == p;
    }
    if (!dup) {
      cand[n++] = Cand{p, d.onset(i).score};
    }
  }
  // The strongest cap - 1 (selection by insertion sort on the score, descending: n is at most 128).
  for (unsigned i = 1; i < n; ++i) {
    const Cand c = cand[i];
    unsigned k = i;
    while (k > 0 && cand[k - 1].score < c.score) {
      cand[k] = cand[k - 1];
      --k;
    }
    cand[k] = c;
  }
  const unsigned keep = n < cap - 1u ? n : cap - 1u;
  // ... then by position.
  for (unsigned i = 1; i < keep; ++i) {
    const Cand c = cand[i];
    unsigned k = i;
    while (k > 0 && cand[k - 1].frame > c.frame) {
      cand[k] = cand[k - 1];
      --k;
    }
    cand[k] = c;
  }
  out[0] = 0;
  for (unsigned i = 0; i < keep; ++i) {
    out[1u + i] = static_cast<std::uint16_t>(cand[i].frame);
  }
  return 1u + keep;
}

}  // namespace o3os::app
