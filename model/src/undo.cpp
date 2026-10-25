// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/model/undo.h>

#include <cstring>

namespace o3os::model {

namespace {

constexpr std::uint16_t kCheck = 0xA5C3;  // in every unit header: a stray write into the region is noticed before it is applied

constexpr std::size_t align4(std::size_t n) noexcept { return (n + 3u) & ~std::size_t{3}; }

struct UnitHeader {
  std::uint32_t len;
  std::uint32_t action;
  std::int32_t arg;
  std::uint16_t records;
  std::uint16_t check;
};
static_assert(sizeof(UnitHeader) == UndoJournal::kUnitHeaderBytes, "the unit header is 16 bytes, no padding");

struct RecordHeader {
  std::uint8_t op;
  std::uint8_t zero;
  std::uint16_t len;
};
static_assert(sizeof(RecordHeader) == 4, "the record header is 4 bytes, no padding");

}  // namespace

bool UndoJournal::begin(void* region, std::size_t bytes) noexcept {
  base_ = nullptr;
  size_ = 0;
  open_ = false;
  poisoned_ = false;
  dropped_ = 0;
  lost_ = 0;
  damaged_ = 0;
  reset_ring();
  if (region == nullptr || bytes < kMinRegionBytes || bytes > kMaxRegionBytes || (bytes & 3u) != 0 ||
      (reinterpret_cast<std::uintptr_t>(region) & 3u) != 0) {
    return false;
  }
  base_ = static_cast<std::uint8_t*>(region);
  size_ = bytes;
  return true;
}

void UndoJournal::reset_ring() noexcept {
  tail_ = cur_ = head_ = 0;
  undo_bytes_ = redo_bytes_ = open_bytes_ = 0;
  undo_units_ = redo_units_ = 0;
  started_ = false;
  unit_records_ = 0;
  rec_open_ = false;
  last_key_ = 0;
}

void UndoJournal::clear() noexcept {
  const bool was_started = started_;
  reset_ring();
  if (open_ && was_started) {
    poisoned_ = true;  // what the open unit had recorded is gone: the rest of it cannot be undone either
  }
}

void UndoJournal::damage() noexcept {
  ++damaged_;
  const bool was_open = open_;
  reset_ring();
  poisoned_ = was_open;
}

void UndoJournal::poison() noexcept {
  if (!poisoned_) {
    ++lost_;
  }
  reset_ring();
  poisoned_ = true;
}

// ---- ring copies ----------------------------------------------------------------------------------------------------

void UndoJournal::put_at(std::size_t pos, const void* src, std::size_t n) noexcept {
  const std::size_t first = n < size_ - pos ? n : size_ - pos;
  std::memcpy(base_ + pos, src, first);
  if (first < n) {
    std::memcpy(base_, static_cast<const std::uint8_t*>(src) + first, n - first);
  }
}

void UndoJournal::get(std::size_t pos, void* dst, std::size_t n) const noexcept {
  const std::size_t first = n < size_ - pos ? n : size_ - pos;
  std::memcpy(dst, base_ + pos, first);
  if (first < n) {
    std::memcpy(static_cast<std::uint8_t*>(dst) + first, base_, n - first);
  }
}

// ---- writer ---------------------------------------------------------------------------------------------------------

void UndoJournal::begin_unit(core::ActionId action, core::ActionArg arg) noexcept {
  if (base_ == nullptr || open_) {
    return;
  }
  open_ = true;
  started_ = false;
  poisoned_ = false;
  action_ = action;
  arg_ = arg;
  open_bytes_ = 0;
  unit_records_ = 0;
}

bool UndoJournal::evict_oldest() noexcept {
  if (undo_units_ == 0) {
    return false;
  }
  std::uint32_t len = 0;
  get(tail_, &len, sizeof len);
  if (len < kUnitHeaderBytes + kUnitTrailerBytes || len > undo_bytes_ || (len & 3u) != 0) {
    damage();
    return false;
  }
  tail_ = advance(tail_, len);
  undo_bytes_ -= len;
  --undo_units_;
  ++dropped_;
  return true;
}

bool UndoJournal::make_room(std::size_t n) noexcept {
  while (size_ - (undo_bytes_ + redo_bytes_ + open_bytes_) < n) {
    if (!evict_oldest()) {
      return false;
    }
  }
  return true;
}

bool UndoJournal::reserve(std::uint8_t op, std::size_t payload) noexcept {
  if (!open_ || poisoned_ || rec_open_) {
    return false;
  }
  const std::size_t total = kRecordOverheadBytes + align4(payload);
  // (the record count of a unit is 16 bits: a group of 65,536 records, possible only in a ring of megabytes, is lost like one that is too big)
  if (payload > kMaxPayloadBytes || total + kUnitHeaderBytes + kUnitTrailerBytes > size_ || (started_ && unit_records_ == 0xFFFFu)) {
    poison();  // cannot be held even in an empty ring
    return false;
  }
  if (!started_) {
    // The first record of the unit: a new edit ends the redo history, and the unit needs room for its header, this record
    // and its trailer. From here on the trailer's room is counted as used, so closing the unit can never fail.
    head_ = cur_;
    redo_bytes_ = 0;
    redo_units_ = 0;
    if (!make_room(kUnitHeaderBytes + total + kUnitTrailerBytes)) {
      poison();
      return false;
    }
    unit_start_ = cur_;
    const UnitHeader provisional{0, 0, 0, 0, kCheck};
    put_at(unit_start_, &provisional, sizeof provisional);  // the real header is written when the unit closes
    head_ = advance(unit_start_, kUnitHeaderBytes);
    open_bytes_ = kUnitHeaderBytes + kUnitTrailerBytes;
    unit_records_ = 0;
    started_ = true;
  } else if (!make_room(total)) {
    poison();
    return false;
  }
  rec_start_ = head_;
  const RecordHeader h{op, 0, static_cast<std::uint16_t>(payload)};
  put_at(rec_start_, &h, sizeof h);
  rec_pos_ = advance(rec_start_, sizeof h);
  rec_len_ = payload;
  rec_put_ = 0;
  rec_open_ = true;
  return true;
}

void UndoJournal::put(const void* src, std::size_t n) noexcept {
  if (!rec_open_ || n == 0 || rec_put_ + n > rec_len_) {
    return;  // a writer that gives more than it announced is a bug: commit() then refuses the record
  }
  put_at(rec_pos_, src, n);
  rec_pos_ = advance(rec_pos_, n);
  rec_put_ += n;
}

void UndoJournal::commit() noexcept {
  if (!rec_open_) {
    return;
  }
  if (rec_put_ != rec_len_) {
    abort();
    return;
  }
  const std::size_t padded = align4(rec_len_);
  const std::uint8_t zeros[4] = {0, 0, 0, 0};
  if (padded != rec_len_) {
    put_at(rec_pos_, zeros, padded - rec_len_);
    rec_pos_ = advance(rec_pos_, padded - rec_len_);
  }
  const std::uint32_t total = static_cast<std::uint32_t>(kRecordOverheadBytes + padded);
  put_at(rec_pos_, &total, sizeof total);
  head_ = advance(rec_start_, total);
  open_bytes_ += total;
  ++unit_records_;
  rec_open_ = false;
}

void UndoJournal::abort() noexcept {
  if (!rec_open_) {
    return;
  }
  rec_open_ = false;
  if (unit_records_ == 0) {
    // Nothing of this unit is kept: it starts again, from the same place, with the next record.
    head_ = unit_start_;
    open_bytes_ = 0;
    started_ = false;
  }
}

bool UndoJournal::end_unit(std::uint32_t key) noexcept {
  if (!open_) {
    return false;
  }
  if (rec_open_) {
    abort();
  }
  bool committed = false;
  if (started_ && !poisoned_) {
    const std::uint32_t len = static_cast<std::uint32_t>(open_bytes_);
    const UnitHeader h{len, static_cast<std::uint32_t>(action_), arg_, unit_records_, kCheck};
    put_at(unit_start_, &h, sizeof h);
    put_at(head_, &len, sizeof len);
    head_ = advance(head_, kUnitTrailerBytes);
    undo_bytes_ += open_bytes_;
    ++undo_units_;
    cur_ = head_;
    last_key_ = key;
    committed = true;
  }
  open_ = false;
  started_ = false;
  poisoned_ = false;
  open_bytes_ = 0;
  unit_records_ = 0;
  return committed;
}

// ---- click groups (OS-UNDOGRP) ----------------------------------------------------------------------------------------

bool UndoJournal::reopen(std::uint32_t key) noexcept {
  if (!open_ || started_ || poisoned_ || rec_open_ || key == 0 || key != last_key_ || redo_units_ != 0) {
    return false;
  }
  last_key_ = 0;  // consumed: the unit is committed again with its key, or is gone
  UnitRef u;
  if (peek_undo(u) != Peek::Ok) {
    return false;
  }
  cur_ = u.start;
  undo_bytes_ -= u.len;
  --undo_units_;
  unit_start_ = u.start;
  unit_records_ = u.records;
  open_bytes_ = u.len;  // header, records and the trailer's room: as when the unit was open the first time
  head_ = retreat(advance(u.start, u.len), kUnitTrailerBytes);
  action_ = u.action;  // the unit stays what the first click made it
  arg_ = u.arg;
  started_ = true;
  return true;
}

bool UndoJournal::last_record(RecRef& out) const noexcept {
  if (!open_ || !started_ || poisoned_ || rec_open_ || unit_records_ == 0) {
    return false;
  }
  return record_before(head_, open_bytes_ - kUnitHeaderBytes - kUnitTrailerBytes, out);
}

void UndoJournal::patch(const RecRef& rec, std::size_t offset, const void* src, std::size_t n) noexcept {
  if (offset + n <= rec.len) {
    put_at(advance(rec.payload, offset), src, n);
  }
}

void UndoJournal::drop_last(const RecRef& rec) noexcept {
  head_ = rec.start;
  open_bytes_ -= rec.total;
  if (--unit_records_ == 0) {
    // Nothing of the unit is left: it is gone (it was taken out of the undo side when it was reopened).
    head_ = unit_start_;
    open_bytes_ = 0;
    started_ = false;
  }
}

void UndoJournal::drop_redo() noexcept {
  if (open_ || redo_units_ == 0) {
    return;
  }
  head_ = cur_;
  redo_bytes_ = 0;
  redo_units_ = 0;
}

bool UndoHistory::begin(void* region, std::size_t bytes) noexcept {
  const std::size_t pattern = ((bytes / 12u) * 5u) & ~std::size_t{3};
  const std::size_t song = ((bytes / 12u) * 3u) & ~std::size_t{3};
  const std::size_t sound = bytes - pattern - song;
  std::uint8_t* base = static_cast<std::uint8_t*>(region);
  const bool a = ring[0].begin(base, pattern);
  const bool b = ring[1].begin(base != nullptr ? base + pattern : nullptr, song);
  const bool c = ring[2].begin(base != nullptr ? base + pattern + song : nullptr, sound);
  return a && b && c;
}

// ---- reader ---------------------------------------------------------------------------------------------------------

UndoJournal::Peek UndoJournal::peek_undo(UnitRef& out) const noexcept {
  if (undo_units_ == 0) {
    return Peek::Empty;
  }
  std::uint32_t len = 0;
  get(retreat(cur_, kUnitTrailerBytes), &len, sizeof len);
  if (len < kUnitHeaderBytes + kUnitTrailerBytes || len > undo_bytes_ || (len & 3u) != 0) {
    return Peek::Damaged;
  }
  const std::size_t start = retreat(cur_, len);
  UnitHeader h;
  get(start, &h, sizeof h);
  if (h.len != len || h.check != kCheck || h.records == 0) {
    return Peek::Damaged;
  }
  out = UnitRef{start, len, h.records, static_cast<core::ActionId>(h.action), h.arg};
  return Peek::Ok;
}

UndoJournal::Peek UndoJournal::peek_redo(UnitRef& out) const noexcept {
  if (redo_units_ == 0) {
    return Peek::Empty;
  }
  UnitHeader h;
  get(cur_, &h, sizeof h);
  if (h.len < kUnitHeaderBytes + kUnitTrailerBytes || h.len > redo_bytes_ || (h.len & 3u) != 0 || h.check != kCheck ||
      h.records == 0) {
    return Peek::Damaged;
  }
  std::uint32_t trailer = 0;
  get(advance(cur_, h.len - kUnitTrailerBytes), &trailer, sizeof trailer);
  if (trailer != h.len) {
    return Peek::Damaged;
  }
  out = UnitRef{cur_, h.len, h.records, static_cast<core::ActionId>(h.action), h.arg};
  return Peek::Ok;
}

bool UndoJournal::record_before(std::size_t end, std::size_t body_left, RecRef& out) const noexcept {
  if (body_left < kRecordOverheadBytes) {
    return false;
  }
  std::uint32_t total = 0;
  get(retreat(end, sizeof total), &total, sizeof total);
  if (total < kRecordOverheadBytes || total > body_left || (total & 3u) != 0) {
    return false;
  }
  const std::size_t start = retreat(end, total);
  RecordHeader h;
  get(start, &h, sizeof h);
  if (total != kRecordOverheadBytes + align4(h.len)) {
    return false;
  }
  out = RecRef{start, total, advance(start, sizeof h), h.len, h.op};
  return true;
}

bool UndoJournal::record_at(std::size_t start, std::size_t body_left, RecRef& out) const noexcept {
  if (body_left < kRecordOverheadBytes) {
    return false;
  }
  RecordHeader h;
  get(start, &h, sizeof h);
  const std::size_t total = kRecordOverheadBytes + align4(h.len);
  if (total > body_left) {
    return false;
  }
  std::uint32_t trailer = 0;
  get(advance(start, total - sizeof trailer), &trailer, sizeof trailer);
  if (trailer != total) {
    return false;
  }
  out = RecRef{start, total, advance(start, sizeof h), h.len, h.op};
  return true;
}

bool UndoJournal::any_record(bool (*match)(std::uint8_t op, std::uint32_t head, unsigned arg), unsigned arg) const noexcept {
  if (open_) {
    return true;
  }
  std::size_t pos = tail_;
  std::size_t left = undo_bytes_ + redo_bytes_;
  while (left != 0) {
    UnitHeader h;
    if (left < kUnitHeaderBytes + kUnitTrailerBytes) {
      return true;
    }
    get(pos, &h, sizeof h);
    if (h.len < kUnitHeaderBytes + kUnitTrailerBytes || h.len > left || (h.len & 3u) != 0 || h.check != kCheck) {
      return true;
    }
    std::size_t body = h.len - kUnitHeaderBytes - kUnitTrailerBytes;
    std::size_t at = advance(pos, kUnitHeaderBytes);
    for (std::uint16_t k = 0; k < h.records; ++k) {
      RecRef rec;
      if (!record_at(at, body, rec)) {
        return true;
      }
      std::uint32_t head = 0;
      get(rec.payload, &head, rec.len < sizeof head ? rec.len : sizeof head);  // (little-endian: the first byte is the low one, as the reader's get() of an id)
      if (match(rec.op, head, arg)) {
        return true;
      }
      body -= rec.total;
      at = advance(at, rec.total);
    }
    pos = advance(pos, h.len);
    left -= h.len;
  }
  return false;
}

bool UndoJournal::newest_undo(core::ActionId& action, core::ActionArg& arg) const noexcept {
  UnitRef u;
  if (peek_undo(u) != Peek::Ok) {
    return false;
  }
  action = u.action;
  arg = u.arg;
  return true;
}

bool UndoJournal::next_redo(core::ActionId& action, core::ActionArg& arg) const noexcept {
  UnitRef u;
  if (peek_redo(u) != Peek::Ok) {
    return false;
  }
  action = u.action;
  arg = u.arg;
  return true;
}

void UndoJournal::finish_undo(const UnitRef& u) noexcept {
  cur_ = u.start;
  undo_bytes_ -= u.len;
  redo_bytes_ += u.len;
  --undo_units_;
  ++redo_units_;
}

void UndoJournal::finish_redo(const UnitRef& u) noexcept {
  cur_ = advance(cur_, u.len);
  undo_bytes_ += u.len;
  redo_bytes_ -= u.len;
  ++undo_units_;
  --redo_units_;
}

}  // namespace o3os::model
