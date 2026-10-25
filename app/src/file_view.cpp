// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/file_view.h>

namespace o3os::app {

namespace {

constexpr int clamp_int(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

int FileView::entry_at(int row, const DirIndex& index) const noexcept {
  if (row < 0) {
    return DirIndex::kNone;
  }
  if (row < lead_) {
    return index.find_key(lead_key_[row]);
  }
  const int at = row - lead_;
  return static_cast<unsigned>(at) < index.count() ? at : DirIndex::kNone;
}

bool FileView::fit(const DirIndex& index) noexcept {
  const int before_pos = pos_;
  const int before_top = top_;
  const int n = rows(index);
  if (n == 0) {
    pos_ = 0;
    top_ = 0;
    anchored_ = false;
    return pos_ != before_pos || top_ != before_top;
  }
  if (anchored_) {
    const int found = index.find_key(anchor_);
    if (found >= 0) {
      top_ += (lead_ + found) - pos_;  // the entry keeps its line of the screen
      pos_ = lead_ + found;
    }
  }
  pos_ = clamp_int(pos_, 0, n - 1);
  const int last_top = n > kVisibleRows ? n - kVisibleRows : 0;
  top_ = clamp_int(top_, 0, last_top);  // not scrolled past the end of the list
  if (pos_ < top_) {
    top_ = pos_;
  } else if (pos_ >= top_ + kVisibleRows) {
    top_ = pos_ - kVisibleRows + 1;
  }
  anchored_ = pos_ >= lead_ && pos_ != 0;
  if (pos_ >= lead_) {
    anchor_ = index.key(static_cast<unsigned>(pos_ - lead_));
  }
  return pos_ != before_pos || top_ != before_top;
}

bool FileView::set(int pos, const DirIndex& index) noexcept {
  ask_clear();
  const int n = rows(index);
  const int before_pos = pos_;
  const int before_top = top_;
  pos_ = clamp_int(pos, 0, n > 0 ? n - 1 : 0);
  if (pos_ < top_) {
    top_ = pos_;
  } else if (pos_ >= top_ + kVisibleRows) {
    top_ = pos_ - kVisibleRows + 1;
  }
  anchored_ = n > 0 && pos_ >= lead_ && pos_ != 0;
  anchor_ = pos_ >= lead_ ? index.key(static_cast<unsigned>(pos_ - lead_)) : std::uint16_t{0};
  return pos_ != before_pos || top_ != before_top;
}

bool FileView::move(int delta, const DirIndex& index) noexcept {
  return set(pos_ + clamp_int(delta, -kMaxStep, kMaxStep), index);  // a huge delta (an encoder fed from data) cannot overflow the sum
}

bool FileView::set_lead(const std::uint16_t* keys, unsigned n, const DirIndex& index) noexcept {
  const unsigned count = keys == nullptr ? 0u : (n < kMaxLead ? n : kMaxLead);
  bool group_changed = count != static_cast<unsigned>(lead_);
  for (unsigned i = 0; i < count && !group_changed; ++i) {
    group_changed = lead_key_[i] != keys[i];
  }
  if (!group_changed) {
    return false;  // (called every pass: nothing is touched, so the anchoring of a growing listing is not)
  }
  const int before_lead = lead_;
  for (unsigned i = 0; i < count; ++i) {
    lead_key_[i] = keys[i];
  }
  lead_ = static_cast<std::uint8_t>(count);
  const int shift = lead_ - before_lead;
  if (shift != 0) {
    if (pos_ == 0) {
      top_ = 0;  // the top stays the top
    } else if (pos_ >= before_lead) {
      pos_ += shift;  // a row of the listing keeps its entry
      top_ += shift;
    } else if (pos_ >= lead_) {
      pos_ = lead_ > 0 ? lead_ - 1 : 0;  // a recent row that is gone
    }
  }
  anchored_ = false;  // (fit() sets it again for the row the cursor is on)
  (void)fit(index);
  return true;
}

int FileView::letter(int dir, const DirIndex& index) const noexcept {
  if (index.count() == 0u) {
    return DirIndex::kNone;
  }
  if (pos_ < lead_) {
    return dir > 0 ? lead_ : (pos_ != 0 ? 0 : DirIndex::kNone);
  }
  if (dir < 0 && lead_ > 0 && pos_ == lead_) {
    return 0;  // left from the listing's first row: back to the group
  }
  const int at = pos_ - lead_;
  const int to = dir > 0 ? index.next_letter(at) : index.prev_letter(at);
  return to == DirIndex::kNone ? DirIndex::kNone : lead_ + to;
}

bool FileView::clear() noexcept {
  const bool moved = pos_ != 0 || top_ != 0;
  reset();
  return moved;
}

void FileView::reset() noexcept {
  ask_clear();
  pos_ = 0;
  top_ = 0;
  anchored_ = false;
  anchor_ = 0;
  lead_ = 0;
  pruned = false;
}

}  // namespace o3os::app
