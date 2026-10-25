// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/app/dir_index.h>
#include <o3os/core/fixed_string.h>
#include <o3os/core/notify.h>

#include <cstdint>

namespace o3os::app {

// The File screen's cursor and window over a DirIndex (OS-010a): one position of the listing and the first of the 16 rows shown, as the Song
// screen's SongView is for the order list. The listing's length comes from the index each time. Each move returns true if the visible state
// changed.
//
// The listing grows while it is being scanned, and the new entries land wherever they sort. So that scrolling stays calm meanwhile, a cursor
// that is not at the top is anchored to its entry (DirIndex::key): fit() finds the entry again and moves the window with it, so the highlighted
// row stays on the same line of the screen and what changes is the rows around it. A cursor at the top stays at the top (the first entry is
// whatever sorts first so far).
//
// RECENT (OS-FILES part B). The PROJECTS page may put a group of up to kMaxLead rows ABOVE the listing, the last projects opened or saved: each is a second row for an entry of the listing (the
// same folder: both open it), kept as its DirIndex key. The rows of the view are those first, then the listing's, so `pos` and `top` count rows; entry() says which entry of the listing a row is. The
// group exists once the scan has ended (the owner calls set_lead() then; a rescan or a new listing, reset(), takes it away), so the anchoring above only ever follows the listing's own rows.
class FileView {
 public:
  static constexpr int kVisibleRows = 16;
  static constexpr int kMaxStep = 1 << 16;  // larger moves are clamped before they are added
  static constexpr unsigned kMaxLead = 5;   // RecentList::kMax

  int pos() const noexcept { return pos_; }
  int top() const noexcept { return top_; }
  int lead() const noexcept { return lead_; }  // the rows of the RECENT group (0: none)
  // The rows of the view over `index`: the group's, then the listing's.
  int rows(const DirIndex& index) const noexcept { return lead_ + static_cast<int>(index.count()); }
  // The entry of the listing that `row` shows (a recent row shows its entry), DirIndex::kNone for a row outside the view or an entry no longer there; entry(): the row under the cursor.
  int entry_at(int row, const DirIndex& index) const noexcept;
  int entry(const DirIndex& index) const noexcept { return entry_at(pos_, index); }

  bool set(int pos, const DirIndex& index) noexcept;  // to row `pos`, clamped to the view; the window follows; the cursor is anchored (a move takes back the questions)
  bool move(int delta, const DirIndex& index) noexcept;
  bool fit(const DirIndex& index) noexcept;           // after the listing changed: the same entry stays selected
  void reset() noexcept;                              // a new listing: back to the top, no RECENT group (the requests below are not touched)
  bool clear() noexcept;                              // reset(), saying whether anything moved: there is no listing
  // The RECENT group: `n` (at most kMaxLead) keys of entries of the listing, newest first, or none to take the group away. A cursor on the listing's rows stays on its entry, one at the top stays at
  // the top (the first recent row, or the listing's first), one on a recent row stays on its row number if it is still there. True if the group or the view changed (a redraw).
  bool set_lead(const std::uint16_t* keys, unsigned n, const DirIndex& index) noexcept;
  // The row a letter jump (dir -1 / +1) goes to from the cursor, DirIndex::kNone when it stays: from the group to the listing's first row (and back to the group's first), inside the listing by letter.
  int letter(int dir, const DirIndex& index) const noexcept;

  // Requests from the actions to the App's pass, which owns the card and the jobs: read the folder again, and "a project open was started and
  // its result has not been told yet" (`pending` says which open it was: the result is told as an open, a recovery of an interrupted save's backup, or a new project).
  // confirm_new: F3 NEW was pressed once with unsaved changes and asked for the second press (any move, any other key of the screen and leaving the screen take it back).
  // confirm_delete, confirm_purge (OS-FILES part B): the same question for the Delete key (DELETE <NAME>?) and for Shift + F7 (PURGE n UNUSED INSTRUMENTS?), each its own flag and taken back the same ways
  // (ask_clear() takes all three back).
  enum class Pending : std::uint8_t { Open, Recover, New, Import };  // Import: OS-FILES, a module of the IMPORT page
  bool rescan = false;
  bool opening = false;
  core::FixedString<32> opening_name;  // OS-QAFIX-4: the display name (kFileNameChars) of the folder the open in progress was asked for: the strip names it when the open was refused (folder() is then the project that stays open)
  bool confirm_new = false;
  // new_choice (OS-NEWKEEP): F3 NEW was pressed and asks which new project: F3 again is ALL (the instruments go too), F1 is KEEP INSTRUMENTS; every other key, a move and leaving the screen cancel it
  // (ask_clear()). It is not confirm_new: F1 is OPEN, whose own question (confirm_new) is the unsaved-changes one.
  // The question is only good for kNewAskMs (the time its text stays on the strip): a later F1 is an OPEN again, a later F3 asks again.
  static constexpr std::uint32_t kNewAskMs = 4000;
  bool new_choice = false;
  std::uint32_t new_asked_at = 0;  // core::Tick of the press that asked
  Pending pending = Pending::Open;
  bool confirm_delete = false;
  bool confirm_purge = false;
  bool pruned = false;  // the RECENT names the listing did not have were dropped from the list (once per listing, when it is complete: a listing that is older than a save would drop a project just saved)
  std::uint16_t target_key = 0;          // the listing's key of the folder the Delete key asked about, and the listing it was (DirIndex::generation):
  std::uint32_t target_generation = 0;   // the second Delete goes on only over that very entry
  // og_ask (OS-FILES-C C2c): F1 on the EXPORT page's OG PROJECT row asked "what is left out: F1 AGAIN" and nothing took it back (same ways, same kNewAskMs); og_sig is the counters it asked about
  // (og_loss_signature), og_asked_at the tick of the press: the second F1 goes on only if the project still counts the same.
  bool og_ask = false;
  std::uint32_t og_asked_at = 0;
  std::uint32_t og_sig = 0;
  core::FixedString<core::kNotifyTextChars> og_ask_text;  // the question's text on the strip (source "file", like NEW's): how it is found again to answer or take back (file_question_sync)
  bool asking() const noexcept { return confirm_new || confirm_delete || confirm_purge || new_choice || og_ask; }  // a question is on the strip (OS-QAFIX-RESCAN: the RESCAN count does not replace it)
  void ask_clear() noexcept { confirm_new = confirm_delete = confirm_purge = new_choice = og_ask = false; }

 private:
  int pos_ = 0;
  int top_ = 0;
  std::uint16_t anchor_ = 0;  // key of the entry under the cursor (valid when anchored_)
  bool anchored_ = false;     // the cursor is on a row of the listing below the top
  std::uint8_t lead_ = 0;     // the RECENT group's rows
  std::uint16_t lead_key_[kMaxLead] = {};  // and the keys of their entries
};

}  // namespace o3os::app
