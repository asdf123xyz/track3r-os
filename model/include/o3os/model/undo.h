// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/core/action.h>

#include <cstddef>
#include <cstdint>

namespace o3os::model {

// The undo journal (ADR 0009 rev 1, OS-002c): a fixed-size byte ring, in a region the platform provides (ADR 0013 D7:
// 384 KiB of SDRAM on the MK66, a static array in the tests), of *units*. A unit is everything one undoable action changed:
// a header, one record per model mutation, a trailer. A record says what changed in model terms (set this step, remove that
// song position, delete this pattern) and keeps what is needed to undo it (the old value) and to redo it (the new one); the
// Editor (editor.h) is the only writer and the only reader, and it applies undo and redo through the Project's own mutators,
// so generations bump and screens redraw as for any edit. The journal itself knows nothing about a Project: bytes, units,
// positions and the bounded-memory policy.
//
//   unit    [header 16: u32 length, u32 action id, i32 argument, u16 record count, u16 check][records][trailer 4: u32 length]
//   record  [op u8, 0 u8, u16 payload bytes][payload, padded to 4][trailer 4: u32 record length]
//
// Everything starts on a multiple of 4; the length at the end of units and records is what lets undo walk backwards. The
// ring is circular and a unit or record may wrap round its end (copies are split), so no space is lost to padding at the
// end. Three positions chase each other round the ring: `tail` (the oldest unit), `cur` (the boundary between what undo
// takes and what redo gives back) and `head` (the end of the last redo unit); the open unit being written sits at `cur`.
//
// Policy.
//  * Capacity is bytes, not levels. When a new unit does not fit, the oldest units are dropped whole (dropped_units()
//    counts them) until it does; depth() says how many are left.
//  * A new edit discards the redo units (there is no branching history).
//  * A unit that cannot fit even in an empty ring (one huge group) cannot be undone: the whole history is cleared (it could
//    no longer be applied to a model that has changed under it) and lost_units() counts it. The edit itself stays done.
//  * Not persisted and not versioned: the journal lives in RAM and ends with the session.
//
// OS-UNDOGRP. One history is three of these rings (UndoHistory below), one per area; the Editor picks the ring a unit goes to.
//  * Click groups. A unit that holds a parameter edit can be *reopened* by the next click on the same parameter (`reopen`): the
//    unit's set_param record then takes the new value and keeps its first old value, so a sweep of the encoder is one unit. The
//    ring remembers the key (the ParamId) of the unit it committed last (`last_key_`); undo, redo, a clear, another unit and the
//    Editor's merge_break() zero it. A sweep that returns to where it started leaves no unit.
//
// Contexts: main loop only. The writer interface below is the Editor's; nothing else should call it.

// ADR 0013 D7 "Undo journal 384 KiB" (platform/mk66/mem_map.h kUndoSize is the same number: a test compares them). OS-UNDOGRP: it is
// carved into one ring per area, sized by each area's largest units (a whole-pattern delete or copy of about 14 KiB in PATTERN and
// SONG, a 64-instrument PURGE of about 94 KiB in SOUND).
inline constexpr std::size_t kUndoPatternBytes = 160u * 1024u;
inline constexpr std::size_t kUndoSongBytes = 96u * 1024u;
inline constexpr std::size_t kUndoSoundBytes = 128u * 1024u;
inline constexpr std::size_t kUndoRegionBytes = kUndoPatternBytes + kUndoSongBytes + kUndoSoundBytes;
static_assert(kUndoRegionBytes == 384u * 1024u, "ADR 0013 D7: the undo journal is 384 KiB");

// Whose history an edit belongs to (D1-D3). Auto: the first record of the unit decides. None: the unit is not journalled (the Mixer, D10).
enum class UndoArea : std::uint8_t { Auto, Pattern, Song, Sound, None };
inline constexpr unsigned kUndoAreas = 3;  // Pattern, Song, Sound

class Editor;
namespace detail {
struct PayloadReader;
}

class UndoJournal {
 public:
  static constexpr std::size_t kUnitHeaderBytes = 16;
  static constexpr std::size_t kUnitTrailerBytes = 4;
  static constexpr std::size_t kRecordOverheadBytes = 8;       // header 4 + trailer 4
  static constexpr std::size_t kMaxPayloadBytes = 0xFFFF;
  static constexpr std::size_t kMinRegionBytes = 64;
  static constexpr std::size_t kMaxRegionBytes = std::size_t{1} << 30;

  // The bytes a unit with one record of `payload` bytes takes: the capacity arithmetic in one place.
  static constexpr std::size_t unit_bytes_for(std::size_t payload) noexcept {
    return kUnitHeaderBytes + kRecordOverheadBytes + ((payload + 3u) & ~std::size_t{3}) + kUnitTrailerBytes;
  }

  // Takes the region: 4-byte aligned, a multiple of 4 bytes, kMinRegionBytes..kMaxRegionBytes. False (the journal stays
  // unusable and records nothing) otherwise. Forgets all history and counters.
  bool begin(void* region, std::size_t bytes) noexcept;
  bool ready() const noexcept { return base_ != nullptr; }
  // Forgets every unit; the counters keep counting. An open unit is poisoned: the rest of its edits are not recorded.
  void clear() noexcept;

  std::size_t capacity_bytes() const noexcept { return size_; }
  unsigned depth() const noexcept { return undo_units_; }        // units undo can take
  unsigned redo_depth() const noexcept { return redo_units_; }   // units redo can give back
  std::size_t used_bytes() const noexcept { return undo_bytes_ + redo_bytes_ + open_bytes_; }
  std::uint32_t dropped_units() const noexcept { return dropped_; }  // oldest units dropped for room since begin()
  std::uint32_t lost_units() const noexcept { return lost_; }        // units too big for the ring (history cleared)
  std::uint32_t damaged() const noexcept { return damaged_; }        // failed integrity checks (a bug; history cleared)
  bool group_open() const noexcept { return open_; }
  // True if any record of any unit (undo or redo side) satisfies `match(op, head, arg)`: `head` is the record's first four payload bytes (zero-padded when it is shorter). No unit open;
  // a damaged ring answers true (it cannot be trusted). Main loop, bounded by the ring's size: not for a hot path.
  bool any_record(bool (*match)(std::uint8_t op, std::uint32_t head, unsigned arg), unsigned arg) const noexcept;
  // The key of the unit that was committed last, while a click on the same key may still join it (0: none).
  std::uint32_t merge_key() const noexcept { return last_key_; }
  // The bytes the open unit holds so far (0 before its first record): a long group (the live take, OS-LIVEREC) is closed before it outgrows the ring, which would lose it and the history before it.
  std::size_t open_bytes() const noexcept { return open_bytes_; }
  // The name and argument the unit that undo would take (redo would give back) was begun with: what the user did. False when
  // there is none (or the journal is damaged: the next undo says so).
  bool newest_undo(core::ActionId& action, core::ActionArg& arg) const noexcept;
  bool next_redo(core::ActionId& action, core::ActionArg& arg) const noexcept;

 private:
  friend class Editor;
  friend struct detail::PayloadReader;

  // One unit and one record as found in the ring (physical positions).
  struct UnitRef {
    std::size_t start = 0;
    std::size_t len = 0;
    std::uint16_t records = 0;
    core::ActionId action = core::ActionId::None;
    core::ActionArg arg = 0;
  };
  struct RecRef {
    std::size_t start = 0;
    std::size_t total = 0;
    std::size_t payload = 0;  // position of the first payload byte
    std::uint16_t len = 0;    // payload bytes
    std::uint8_t op = 0;
  };
  enum class Peek : std::uint8_t { Empty, Ok, Damaged };

  // ---- writer (the Editor) ----
  void begin_unit(core::ActionId action, core::ActionArg arg) noexcept;  // lazily: nothing is written until a record comes
  bool end_unit(std::uint32_t key = 0) noexcept;                         // commits the unit if it has records (true); `key` becomes the merge key then
  bool reopen(std::uint32_t key) noexcept;                               // before the unit's first record: the unit committed last, if its key is `key`, is open again
  bool last_record(RecRef& out) const noexcept;                          // the open unit's newest record
  void patch(const RecRef& rec, std::size_t offset, const void* src, std::size_t n) noexcept;  // overwrites payload bytes of a record of the open unit
  void drop_last(const RecRef& rec) noexcept;                            // removes the open unit's newest record (the unit goes if it was the only one)
  void drop_redo() noexcept;                                             // forgets the units undo took (no unit open)
  void break_group() noexcept { last_key_ = 0; }
  bool reserve(std::uint8_t op, std::size_t payload) noexcept;          // false: not recorded (see the policy above)
  void put(const void* src, std::size_t n) noexcept;                     // payload bytes, in order, exactly `payload`
  void commit() noexcept;
  void abort() noexcept;

  // ---- reader (the Editor) ----
  Peek peek_undo(UnitRef& out) const noexcept;
  Peek peek_redo(UnitRef& out) const noexcept;
  bool record_before(std::size_t end, std::size_t body_left, RecRef& out) const noexcept;  // the record that ends at `end`
  bool record_at(std::size_t start, std::size_t body_left, RecRef& out) const noexcept;
  void finish_undo(const UnitRef& u) noexcept;
  void finish_redo(const UnitRef& u) noexcept;
  void get(std::size_t pos, void* dst, std::size_t n) const noexcept;
  std::size_t cur() const noexcept { return cur_; }
  std::size_t advance(std::size_t pos, std::size_t n) const noexcept { return pos + n >= size_ ? pos + n - size_ : pos + n; }
  std::size_t retreat(std::size_t pos, std::size_t n) const noexcept { return pos >= n ? pos - n : pos + size_ - n; }
  void damage() noexcept;  // an integrity check failed: forget everything

  void put_at(std::size_t pos, const void* src, std::size_t n) noexcept;
  void reset_ring() noexcept;
  bool make_room(std::size_t n) noexcept;
  bool evict_oldest() noexcept;
  void poison() noexcept;

  std::uint8_t* base_ = nullptr;
  std::size_t size_ = 0;
  std::size_t tail_ = 0;  // physical offsets into the ring
  std::size_t cur_ = 0;
  std::size_t head_ = 0;
  std::size_t undo_bytes_ = 0;  // [tail, cur)
  std::size_t redo_bytes_ = 0;  // [cur, head) when no unit is open
  unsigned undo_units_ = 0;
  unsigned redo_units_ = 0;

  bool open_ = false;       // between begin_unit and end_unit
  bool started_ = false;    // the unit's header is written
  bool poisoned_ = false;   // the unit cannot be recorded: its edits are applied and forgotten
  core::ActionId action_ = core::ActionId::None;
  core::ActionArg arg_ = 0;
  std::size_t unit_start_ = 0;
  std::size_t open_bytes_ = 0;  // header + records so far + the trailer's reserved room
  std::uint16_t unit_records_ = 0;

  bool rec_open_ = false;
  std::size_t rec_start_ = 0;
  std::size_t rec_pos_ = 0;   // where the next payload byte goes
  std::size_t rec_len_ = 0;   // payload bytes announced
  std::size_t rec_put_ = 0;   // payload bytes given

  std::uint32_t last_key_ = 0;  // the merge key of the newest committed unit (0: none)

  std::uint32_t dropped_ = 0;
  std::uint32_t lost_ = 0;
  std::uint32_t damaged_ = 0;
};

// The undo history of a document: one ring per area (OS-UNDOGRP D6) carved out of one region, so the platform still provides a single
// 384 KiB block. The Editor takes it; Editor(Project&, UndoJournal&) takes one ring for all areas instead (tests that need no areas).
struct UndoHistory {
  UndoJournal ring[kUndoAreas];  // Pattern, Song, Sound

  static constexpr unsigned index(UndoArea a) noexcept { return a == UndoArea::Song ? 1u : (a == UndoArea::Sound ? 2u : 0u); }
  // Carves `region` 5/12, 3/12 and 4/12 (160, 96 and 128 KiB for the 384 KiB of ADR 0013 D7); the same rules as UndoJournal::begin().
  // False (nothing recorded) if any ring would be unusable.
  bool begin(void* region, std::size_t bytes) noexcept;
  void clear() noexcept {
    for (UndoJournal& r : ring) {
      r.clear();
    }
  }
};

}  // namespace o3os::model
