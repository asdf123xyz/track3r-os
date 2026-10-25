// SPDX-License-Identifier: GPL-3.0-or-later
// The Song screen's editing actions (OS-SONG1): slots added, deleted and copied, the pattern of a slot, the tempo and its tap, the range of slots, and F1 PLAY PAT.
// The edits are undo units through the Editor (ctx.edit, which only an undoable action is given); the model keeps the order list (model::Project::order,
// insert_order, remove_order, set_order), so no format changes. A behaviour not marked NEW below is this OS's own rule for
// the song editor (whether it matches the official firmware's is an open research question).
#include "action_handlers.h"
#include "param_access.h"

#include <o3os/app/song_edit.h>
#include <o3os/app/song_source.h>
#include <o3os/app/text_format.h>
#include <o3os/model/editor.h>
#include <o3os/model/limits.h>
#include <o3os/seq/sequencer.h>

#include <cstring>

namespace o3os::app {

namespace {

static_assert(SongView::kClipMax == static_cast<int>(model::kMaxSongPositions), "the clipboard holds a whole order list");

constexpr unsigned kTapTimeoutMs = 1500;  // 1500 ms: a longer pause starts a new run of taps
constexpr std::uint32_t kTapMaxX100 = 30000;  // OS-PATTEMPO: the tap tempo's ceiling, 300 BPM
constexpr int kMaxDetents = 1 << 16;      // an encoder fed from data is clamped before it is multiplied

bool say(ActionContext& ctx, core::Severity severity, const char* text) noexcept {
  return ctx.view.notify.post(severity, text, "song", ViewState::kMessageMs);
}

constexpr int clamp_int(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

// The cursor's slot, inside a list of `count` positions (-1 for an empty one).
int cursor_slot(const ActionContext& ctx, int count) noexcept { return count > 0 ? clamp_int(ctx.view.song.pos(), 0, count - 1) : -1; }

// The first pattern number nothing uses, 0 when all 255 are taken.
unsigned first_free_pattern(const model::Project& p) noexcept {
  for (unsigned id = 1; id <= model::kMaxPatternId; ++id) {
    if (p.pattern(id) == nullptr) {
      return id;
    }
  }
  return 0;
}

// NEW (this OS's key; whether the official firmware has an equivalent is an open research question): pattern `from` copied to the new number `to` (song_edit.h: song_copy_pattern, which UNIQUE uses too).
model::Status copy_pattern(model::Editor& e, unsigned from, unsigned to) noexcept { return song_copy_pattern(e, from, to); }

// The pattern of the cursor's slot moves by `delta`, clamped to 1..255 and not wrapping (< 255, > 1).
bool pattern_step(ActionContext& ctx, int delta) noexcept {
  const model::Project& p = ctx.edit->project();
  const int pos = cursor_slot(ctx, static_cast<int>(p.song_length()));
  if (pos < 0) {
    return say(ctx, core::Severity::Info, "SONG IS EMPTY");
  }
  const int now = static_cast<int>(p.order(static_cast<std::uint32_t>(pos)));
  const int next = clamp_int(now + clamp_int(delta, -kMaxDetents, kMaxDetents), 1, static_cast<int>(model::kMaxPatternId));
  return next != now && ctx.edit->set_order(static_cast<std::uint32_t>(pos), static_cast<std::uint32_t>(next)) == model::Status::Ok;
}

// The tempo moves by `detents`: a whole BPM each (the tempo is first cut to whole BPM when the step is a whole one), or a tenth with
// `fine` (Shift). The sequencer hears it at once (a ParamChange, ramp 0: its own smoothing); a transport start reads the project's.
bool tempo_step(ActionContext& ctx, int detents, bool fine) noexcept {
  const std::int64_t now = ctx.edit->project().tempo_x100();
  const std::int64_t base = fine ? now : now / 100 * 100;
  std::int64_t next = base + static_cast<std::int64_t>(clamp_int(detents, -kMaxDetents, kMaxDetents)) * (fine ? 10 : 100);
  next = next < static_cast<std::int64_t>(model::kMinTempoX100) ? model::kMinTempoX100 : (next > model::kMaxTempoX100 ? model::kMaxTempoX100 : next);
  if (next == now || ctx.edit->set_tempo_x100(static_cast<std::uint32_t>(next)) != model::Status::Ok) {
    return false;
  }
  send_param_change(ctx, seq::tempo_param(), static_cast<float>(next) / 100.0f);
  return true;
}

// The clock is somebody else's (CLOCK MODE: Slave): the tempo is not ours to edit. Says so on the strip (the DJ page's line) and reports true.
bool refuse_slave(ActionContext& ctx) noexcept {
  if (!clock_slave(ctx)) {
    return false;
  }
  (void)ctx.view.notify.upsert(core::Severity::Info, "MIDI SLAVE: THE CLOCK SETS THE TEMPO", "tempo", ViewState::kMessageMs);
  return true;
}

// The strip's line for a tempo: `<prefix> 120.0 BPM`, rounded to 0.1 BPM.
bool say_tempo(ActionContext& ctx, const char* prefix, std::uint32_t x100) noexcept {
  const unsigned tenths = (x100 + 5u) / 10u;
  core::FixedString<core::kNotifyTextChars> text;
  text.assign(prefix);
  text.append(" ");
  append_uint(text, tenths / 10u);
  text.append(".");
  append_uint(text, tenths % 10u);
  text.append(" BPM");
  return ctx.view.notify.upsert(core::Severity::Info, text.c_str(), "tempo", ViewState::kMessageMs);
}

bool turn(ActionContext& ctx, core::ActionArg arg, bool fine) noexcept {
  return ctx.view.song.tempo_focus() ? tempo_step(ctx, arg, fine) : pattern_step(ctx, arg);
}

}  // namespace

bool clock_slave(const ActionContext& ctx) noexcept { return ctx.model.device.get(model::device_setting::ClockMode) == 2.0f; }

// OS-PATTEMPO (NEW Community OS behaviour; whether the official firmware has an equivalent is an open research question): Pattern held + the encoder is the project tempo on any screen
// (bindings.cpp: hold_encoder). The Song screen's tempo step: a BPM a detent, Shift (held before Pattern: the recogniser ends the gesture of a key that Shift joins) a tenth; one undo unit on the
// history of the screen in view; the sequencer hears it at once. The strip says the tempo, also at the ends of the range. Not in MIDI Slave.
bool song_tempo_hold(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (ctx.edit == nullptr) {
    return false;
  }
  if (refuse_slave(ctx)) {
    return true;
  }
  merge_click(ctx, model::kTempoMergeKey);  // the taps and detents of one hold are one undo unit
  const bool changed = tempo_step(ctx, arg, ctx.view.shift);
  return say_tempo(ctx, "TEMPO", ctx.edit->project().tempo_x100()) || changed;
}

// OS-PATTEMPO: Pattern held + a pad (the App takes the pad ahead of the bindings: it plays no note) is a tempo tap. From the second tap the tempo is the mean of the last up to 4 intervals (only the
// first and the last tap of the series count), to 0.1 BPM, 40..300 (the taps' own ceiling, as the Tap Calc app's); a pause of more than 2 s starts a new series. The same edit as the encoder's.
bool song_tempo_tap(ActionContext& ctx, core::ActionArg) noexcept {
  TapTempo& t = ctx.view.tap_tempo;
  if (refuse_slave(ctx)) {
    t.n = 0;
    return true;
  }
  const core::Tick now = t.stamp;
  if (t.n != 0 && core::tick_elapsed(now, t.at[t.n - 1u]) > core::ticks_from_ms(TapTempo::kGapMs)) {
    t.n = 0;  // a new series
  }
  if (t.n == TapTempo::kTaps) {
    for (unsigned i = 1; i < TapTempo::kTaps; ++i) {
      t.at[i - 1u] = t.at[i];
    }
    --t.n;
  }
  t.at[t.n++] = now;
  const std::uint32_t span = core::tick_elapsed(now, t.at[0]);  // microseconds
  if (t.n < 2u || span == 0u) {
    return ctx.view.notify.upsert(core::Severity::Info, "TAP", "tempo", ViewState::kMessageMs);
  }
  const std::uint64_t tenths = (600000000ull * (t.n - 1u) + span / 2u) / span;  // BPM x 10 = 60 s in us x intervals / span
  std::uint64_t x100 = tenths * 10u;
  x100 = x100 > kTapMaxX100 ? kTapMaxX100 : (x100 < model::kMinTempoX100 ? model::kMinTempoX100 : x100);
  bool changed = false;
  merge_click(ctx, model::kTempoMergeKey);
  if (ctx.edit != nullptr && x100 != ctx.edit->project().tempo_x100() && ctx.edit->set_tempo_x100(static_cast<std::uint32_t>(x100)) == model::Status::Ok) {
    send_param_change(ctx, seq::tempo_param(), static_cast<float>(x100) / 100.0f);
    changed = true;
  }
  return say_tempo(ctx, "TAP", static_cast<std::uint32_t>(x100)) || changed;
}

// OS-DJ A4: the project tempo by `tenths` x 0.1 BPM, as the Song screen's Shift + encoder on the tempo field does it.
bool tempo_fine_step(ActionContext& ctx, int tenths) noexcept { return ctx.edit != nullptr && tempo_step(ctx, tenths, true); }

bool song_enabled(const ActionContext& ctx) noexcept { return ctx.view.screen == ScreenId::Song; }
bool song_play_enabled(const ActionContext& ctx) noexcept { return song_enabled(ctx) && ctx.transport != nullptr; }

// F3 / Insert: a slot after the cursor's holding the same pattern number, the cursor on it (the slot after the selected one takes the
// selected one's number, 1 for an empty one; the list is at most 255 long). arg 1 (Shift + F3) is NEW: the new slot holds a COPY of the pattern, which takes the first free number.
bool song_add_slot(ActionContext& ctx, core::ActionArg arg) noexcept {
  const model::Project& p = ctx.edit->project();
  const int count = static_cast<int>(p.song_length());
  if (count >= static_cast<int>(model::kMaxSongPositions)) {
    return say(ctx, core::Severity::Warn, "SONG FULL");
  }
  const int pos = cursor_slot(ctx, count);
  unsigned id = pos >= 0 ? p.order(static_cast<std::uint32_t>(pos)) : 0u;
  id = id != 0u ? id : 1u;
  if (arg == 1) {
    const unsigned fresh = first_free_pattern(p);
    if (fresh == 0u || copy_pattern(*ctx.edit, id, fresh) != model::Status::Ok) {
      return say(ctx, core::Severity::Warn, "NO FREE PATTERN");  // nothing was changed: a refused create records nothing
    }
    id = fresh;
  }
  if (ctx.edit->insert_order(static_cast<std::uint32_t>(pos + 1), id) != model::Status::Ok) {
    return false;
  }
  ctx.view.song.leave_edit();
  (void)ctx.view.song.set(pos + 1, count + 1);
  return true;
}

namespace {

// The slots first..first+n-1 leave the list and the cursor goes to the slot before the first one that went (not the last slot: the selection moves up one; whether the official firmware does the same is an open research question). The caller checked that a slot is left.
// True when a live jump was queued to one of the slots that went (the sequencer drops it with the slot, E-SONG-07).
bool drop_slots(ActionContext& ctx, int first, int n) noexcept {
  const unsigned queued = ctx.view.play.queued;
  const bool queue_lost = queued >= static_cast<unsigned>(first) && queued < static_cast<unsigned>(first + n);
  const model::Editor::OrderBatch batch(*ctx.edit);  // E-SONG-07: a playing song sees the range go as one change
  for (int i = 0; i < n; ++i) {
    if (ctx.edit->remove_order(static_cast<std::uint32_t>(first)) != model::Status::Ok) {
      break;
    }
  }
  ctx.view.song.leave_edit();
  (void)ctx.view.song.set(first > 0 ? first - 1 : 0, static_cast<int>(ctx.edit->project().song_length()));
  return queue_lost;
}

}  // namespace

// F4 / Delete: the cursor's slot, or the selected range, goes; the cursor to the slot before the first one that went (not the last
// slot, the selection moves up one). A range of slots is NEW.
bool song_delete_slot(ActionContext& ctx, core::ActionArg) noexcept {
  const int count = static_cast<int>(ctx.edit->project().song_length());
  if (count <= 0) {
    return say(ctx, core::Severity::Info, "SONG IS EMPTY");
  }
  const int first = clamp_int(ctx.view.song.first(), 0, count - 1);
  const int last = clamp_int(ctx.view.song.last(), first, count - 1);
  const int n = last - first + 1;
  if (n >= count) {
    return say(ctx, core::Severity::Info, "SONG NEEDS A SLOT");
  }
  if (drop_slots(ctx, first, n)) {
    (void)ctx.view.notify.upsert(core::Severity::Info, "QUEUED SLOT DELETED", "song", ViewState::kMessageMs);  // (rewrites the JUMP QUEUED notice that may still be up)
  }
  return true;
}

// Shift + Delete (OS-SONGSHARE S2, NEW): CUT. On PAT the range of slots (no range: the cursor's slot) goes into the slot clipboard, as Copy does, and out of the list, as Delete does, in one undo unit; Shift + Copy puts it back
// anywhere. The clipboard is not undone (as Copy's is not). Cutting every slot is refused, as Delete refuses it, and then the clipboard keeps what it held. In the map it is the cells' cut (song_cells_cut).
bool song_cut(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.song.in_map()) {
    return song_cells_cut(ctx);
  }
  const int count = static_cast<int>(ctx.edit->project().song_length());
  if (count <= 0) {
    return say(ctx, core::Severity::Info, "SONG IS EMPTY");
  }
  SongView& sv = ctx.view.song;
  const int first = clamp_int(sv.first(), 0, count - 1);
  const int last = clamp_int(sv.last(), first, count - 1);
  const int n = last - first + 1;
  if (n >= count) {
    return say(ctx, core::Severity::Info, "SONG NEEDS A SLOT");  // before the clipboard is touched
  }
  const SongSource src(ctx.model);
  sv.clip_count = n;
  for (int i = 0; i < n; ++i) {
    sv.clip[i] = static_cast<std::uint8_t>(src.pattern(first + i));
  }
  (void)drop_slots(ctx, first, n);  // (a queued jump to a slot that went is dropped; Delete says so, the cut's notice stays the cut's)
  core::FixedString<core::kNotifyTextChars> text;
  text.assign("CUT ");
  append_uint(text, static_cast<unsigned>(n));
  text.append(n == 1 ? " SLOT" : " SLOTS");
  (void)ctx.view.notify.upsert(core::Severity::Info, text.c_str(), "song", ViewState::kMessageMs);  // (a second cut rewrites the notice that is up)
  return true;
}

// Delete (OS-SONGMAP): in the map, every element of the rectangle's tracks in those patterns goes (the track lengths stay); on PAT it is F4's DEL SLOT. Not a repeating action: a held key does it once (QA-SNG-012).
bool song_clear(ActionContext& ctx, core::ActionArg arg) noexcept { return ctx.view.song.in_map() ? song_cells_clear(ctx) : song_delete_slot(ctx, arg); }

// Copy held + Up / Down (OS-SONGSHARE S3, NEW; arg = -1 up, +1 down): the range of slots (in the map: the rectangle's slots; no range: the cursor's slot) moves one position. The slot it steps over goes to the other
// side of the range, as one change of the list (Editor::move_order: a playing song follows the slot it plays; E-SONG-07), in the caller's one undo unit, whose records are an insert and a remove (a list of 255 slots, which cannot take
// the insert: set_order writes of the slots that change). The cursor and the range follow, and the column and the tracks of the rectangle stay. At the
// top or the bottom nothing happens. Held, the arrow repeats (the action is kActionRepeatable) and each step is a unit of its own (D2). Allowed while the song plays, as an insert and a delete are (D3): the play
// position stays a position of the list.
bool song_move(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (arg != -1 && arg != 1) {
    return false;
  }
  const model::Project& p = ctx.edit->project();
  const int count = static_cast<int>(p.song_length());
  if (count <= 0) {
    return say(ctx, core::Severity::Info, "SONG IS EMPTY");
  }
  SongView& sv = ctx.view.song;
  const int first = clamp_int(sv.first(), 0, count - 1);
  const int last = clamp_int(sv.last(), first, count - 1);
  const int over = arg < 0 ? first - 1 : last + 1;  // the slot that steps over the range
  if (over < 0 || over >= count) {
    return false;  // the top or the bottom: nothing moves
  }
  // The slot that steps over goes to the other side of the range: going down it lands at the range's first index, going up at its last.
  if (ctx.edit->move_order(static_cast<std::uint32_t>(over), static_cast<std::uint32_t>(arg < 0 ? last : first)) != model::Status::Ok) {
    return false;  // (cannot happen: the indexes are inside the list; were it ever to, nothing was written)
  }
  (void)sv.shift_range(arg, count);
  return true;
}

// Note / Shift + Note (OS-SONGSHARE S4, NEW; arg = +1 next, -1 previous): the cursor goes to the next (previous) slot that holds the cursor's pattern, wrapping round the list. The strip says where it landed, `SLOT 12 (2 OF 4)`,
// and `ONLY IN THIS SLOT` when no other slot holds the pattern (the cursor stays). A plain jump: the range collapses, as every cursor move does, and the column of the map is kept. A view change only: no undo unit.
// A pattern counts for a slot that names it whether or not it exists yet. The demo list (no project) answers too.
bool song_next_use(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (arg != 1 && arg != -1) {
    return false;
  }
  const SongSource src(ctx.model);
  const int count = src.count();
  const int pos = cursor_slot(ctx, count);
  if (pos < 0) {
    return say(ctx, core::Severity::Info, "SONG IS EMPTY");
  }
  const unsigned id = src.pattern(pos);
  int uses = 0;  // the slots that hold it, the cursor's included
  for (int i = 0; i < count; ++i) {
    uses += src.pattern(i) == id ? 1 : 0;
  }
  if (uses < 2) {
    return ctx.view.notify.upsert(core::Severity::Info, "ONLY IN THIS SLOT", "song", ViewState::kMessageMs);
  }
  int to = pos;  // (the walk ends on a holder: there are at least two, and the cursor's own slot is one of them)
  for (int k = 1; k < count; ++k) {
    to = (pos + arg * k + count) % count;
    if (src.pattern(to) == id) {
      break;
    }
  }
  int ordinal = 1;  // which of the holders it landed on, counting from the top
  for (int i = 0; i < to; ++i) {
    ordinal += src.pattern(i) == id ? 1 : 0;
  }
  ctx.view.song.leave_edit();
  (void)ctx.view.song.set(to, count);
  core::FixedString<core::kNotifyTextChars> text;
  text.assign("SLOT ");
  append_dec(text, static_cast<unsigned>(to), to > 99 ? 3u : 2u);
  text.append(" (");
  append_uint(text, static_cast<unsigned>(ordinal));
  text.append(" OF ");
  append_uint(text, static_cast<unsigned>(uses));
  text.append(")");
  (void)ctx.view.notify.upsert(core::Severity::Info, text.c_str(), "song", ViewState::kMessageMs);  // (a second press rewrites the notice that is up)
  return true;
}

// (no key since OS-SONGMAP Q1: it was F5 / F6) the pattern of the cursor's slot minus / plus one (arg = +-1). Back on the slot from the tempo field.
bool song_pattern_delta(ActionContext& ctx, core::ActionArg arg) noexcept {
  const bool left = ctx.view.song.tempo_focus();
  ctx.view.song.set_tempo_focus(false);
  return pattern_step(ctx, arg) || left;
}

// The encoder: the slot's pattern number, or the tempo when F7 put the encoder on the tempo field. Shift + encoder: the same for a slot; a tenth of a BPM for the tempo.
bool song_turn(ActionContext& ctx, core::ActionArg arg) noexcept { return turn(ctx, arg, false); }
bool song_turn_fine(ActionContext& ctx, core::ActionArg arg) noexcept { return turn(ctx, arg, true); }

// F7: the encoder goes to the tempo field and the press counts as a tap (two taps less than 1.5 s apart
// set the tempo to 60000 / the interval in ms when that is in 40..800 BPM, the next tap one interval later again. A tap after a longer pause starts a new run (the tap itself
// starts the new run: NEW, deliberately kind). Without a project the key only selects the field.
bool song_tempo(ActionContext& ctx, core::ActionArg) noexcept {
  SongView& sv = ctx.view.song;
  bool changed = !sv.tempo_focus() || sv.selecting();
  (void)sv.leave_edit();
  sv.set_tempo_focus(true);
  const std::uint32_t since = core::tick_elapsed(ctx.now, sv.tap_at);
  if (sv.taps != 0u && since != 0u && since <= core::ticks_from_ms(kTapTimeoutMs)) {
    const std::uint64_t x100 = 6000000000ull / since;  // 60 s in us x 100 (BPM x 100 = 60e6 / us x 100)
    sv.taps = sv.taps < 255u ? static_cast<std::uint8_t>(sv.taps + 1u) : sv.taps;
    if (ctx.edit != nullptr && x100 >= model::kMinTempoX100 && x100 <= model::kMaxTempoX100 && x100 != ctx.edit->project().tempo_x100() &&
        ctx.edit->set_tempo_x100(static_cast<std::uint32_t>(x100)) == model::Status::Ok) {
      send_param_change(ctx, seq::tempo_param(), static_cast<float>(x100) / 100.0f);
    }
    changed = true;
  } else {
    sv.taps = 1;
  }
  sv.tap_at = ctx.now;
  return changed;
}

// Shift + Up / Down (arg = +-1): the range of slots grows from where Shift was first used, as the pattern's block. NEW.
bool song_select(ActionContext& ctx, core::ActionArg arg) noexcept {
  ctx.view.song.set_tempo_focus(false);
  return (arg == 1 || arg == -1) && ctx.view.song.extend(arg, SongSource(ctx.model).count());
}

// Copy: the selected range (or the cursor's slot) into the clipboard, as pattern numbers (not the patterns' contents: a paste names the same patterns). NEW.
bool song_copy(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.song.in_map()) {
    return song_cells_copy(ctx);  // OS-SONGMAP: in the map Copy takes the cells of the rectangle
  }
  const SongSource src(ctx.model);
  const int count = src.count();
  if (count <= 0) {
    return say(ctx, core::Severity::Info, "SONG IS EMPTY");
  }
  SongView& sv = ctx.view.song;
  const int first = clamp_int(sv.first(), 0, count - 1);
  const int last = clamp_int(sv.last(), first, count - 1);
  sv.clip_count = last - first + 1;
  for (int i = 0; i < sv.clip_count; ++i) {
    sv.clip[i] = static_cast<std::uint8_t>(src.pattern(first + i));
  }
  core::FixedString<core::kNotifyTextChars> text;
  text.assign("COPIED ");
  append_uint(text, static_cast<unsigned>(sv.clip_count));
  text.append(sv.clip_count == 1 ? " SLOT" : " SLOTS");
  return say(ctx, core::Severity::Info, text.c_str());
}

// Shift + Copy: the clipboard's slots go into the list at the cursor (the cursor's slot and the ones after it move down), the cursor on the first of them and, for several,
// the range selected. NEW. A list the slots do not fit in is refused whole.
bool song_paste(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.song.in_map()) {
    return song_cells_paste(ctx);  // OS-SONGMAP
  }
  SongView& sv = ctx.view.song;
  const int n = sv.clip_count;
  if (n <= 0) {
    return say(ctx, core::Severity::Info, "NOTHING COPIED");
  }
  const int count = static_cast<int>(ctx.edit->project().song_length());
  if (count + n > static_cast<int>(model::kMaxSongPositions)) {
    return say(ctx, core::Severity::Warn, "SONG FULL");
  }
  const int at = count > 0 ? cursor_slot(ctx, count) : 0;
  const model::Editor::OrderBatch batch(*ctx.edit);  // E-SONG-07: a playing song sees the slots come in as one change
  for (int i = 0; i < n; ++i) {
    if (ctx.edit->insert_order(static_cast<std::uint32_t>(at + i), sv.clip[i]) != model::Status::Ok) {
      break;
    }
  }
  const int now = static_cast<int>(ctx.edit->project().song_length());
  sv.leave_edit();
  (void)sv.set(at, now);
  if (n > 1 && at + n - 1 < now) {
    sv.select(at + n - 1);
  }
  return true;
}

// F1 PLAY PAT: the pattern of the cursor's slot plays from its first step, looping, or the transport stops (the playing of the
// selected slot's pattern; again it stops). Like Play, the badge follows the request that went through.
bool song_play_pattern(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.transport == nullptr) {
    return false;
  }
  bool ok = false;
  if (ctx.view.playing) {
    ok = ctx.transport->stop();
  } else {
    const SongSource src(ctx.model);
    const int pos = cursor_slot(ctx, src.count());
    if (pos < 0) {
      return say(ctx, core::Severity::Info, "SONG IS EMPTY");
    }
    ok = ctx.transport->play(src.pattern(pos));
  }
  if (!ok) {
    return false;
  }
  ctx.view.playing = !ctx.view.playing;
  return true;
}

// Shift + F6 NAME (OS-NAMES): the keyboard on the name of the pattern in the slot under the cursor. A slot that names a pattern the project does not have yet has nothing to name.
bool song_name(ActionContext& ctx, core::ActionArg) noexcept {
  const SongSource src(ctx.model);
  const int pos = cursor_slot(ctx, src.count());
  if (pos < 0) {
    return say(ctx, core::Severity::Info, "SONG IS EMPTY");
  }
  const model::Pattern* pat = ctx.model.project != nullptr ? ctx.model.project->pattern(src.pattern(pos)) : nullptr;
  if (ctx.model.project != nullptr && pat == nullptr) {
    return say(ctx, core::Severity::Info, "NO SUCH PATTERN YET");
  }
  return name_open(ctx, TextEntryView::Target::Pattern, src.pattern(pos), pat != nullptr ? pat->name : "", model::kPatternNameMax);
}

// The live jump to slot `pos` (the transport plays): queued to play after the pattern that plays now, or taken back when that slot is the one already queued. True if it went to the sequencer
// (`cancelled` says which); false when no sequencer is behind the sink or its ring is full, nothing changed.
bool queue_slot(ActionContext& ctx, int pos, bool& cancelled) noexcept {
  cancelled = ctx.view.play.queued == static_cast<std::uint16_t>(pos);
  return ctx.transport->queue_jump(cancelled ? 0xFFFFu : static_cast<unsigned>(pos));
}

// Rec while playing (NEW): the live jump. The cursor's slot is queued to play after the pattern that plays now (TransportSink::queue_jump: a command on the sequencer's ring, applied at the pattern's
// boundary on the render's timeline); the slot blinks while it waits and Rec on the same slot cancels it. Stopped, there is nothing to jump in.
bool song_queue_jump(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.transport == nullptr) {
    return false;
  }
  if (!ctx.view.playing && !ctx.view.play.playing) {
    return say(ctx, core::Severity::Info, "NOT PLAYING");
  }
  const int count = SongSource(ctx.model).count();
  const int pos = cursor_slot(ctx, count);
  if (pos < 0) {
    return say(ctx, core::Severity::Info, "SONG IS EMPTY");
  }
  bool cancel = false;
  if (!queue_slot(ctx, pos, cancel)) {
    return false;  // no sequencer behind the sink, or the ring is full: nothing changed
  }
  return say(ctx, core::Severity::Info, cancel ? "JUMP CANCELLED" : "JUMP QUEUED");
}

// ---- the pads on Song (OS-SONGPADS; docs/specs/SONG-UX.md section 2; NEW COMMUNITY OS BEHAVIOUR; whether the official firmware's Song has a pad function is an open research question) -------------------------------------------------------------------------

// Shift + F7 PADS: TRACKS (the layer of pad_layer.h: mute, solo, mute mode and the track cursor) and SLOTS (48 pads are 48 slots) take turns. Runtime state of the view: nothing is saved (Q5).
bool song_pads(ActionContext& ctx, core::ActionArg) noexcept {
  const bool slots = ctx.view.padmode.toggle_song_slots();
  return ctx.view.notify.upsert(core::Severity::Info, slots ? "PADS: SLOTS" : "PADS: TRACKS", "song", ViewState::kMessageMs);  // (a second press rewrites the notice that is up, it does not queue behind it)
}

// A pad in SLOTS mode (arg = the pad, 0..47; the TRACKS layer takes the pads before this binding is reached). The pads show the page of 48 slots that holds the cursor (SongScreen::pad_roles), pad n of the
// page is slot 48 * page + n. Playing, a tap is the live jump to that slot (the cursor stays, Q6: as PATS mode queues without selecting) and a tap on the queued slot cancels it; stopped, the cursor goes
// there and the selection collapses. A pad past the song's end does nothing.
bool song_pad(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!ctx.view.padmode.song_slots() || arg < 0 || arg >= SongView::kPadSlots) {
    return false;
  }
  const int count = SongSource(ctx.model).count();
  const int pos = ctx.view.song.pad_base(count) + arg;
  if (pos >= count) {
    return false;
  }
  if (ctx.view.playing || ctx.view.play.playing) {
    bool cancel = false;
    return ctx.transport != nullptr && queue_slot(ctx, pos, cancel);
  }
  const bool left = ctx.view.song.leave_edit();
  return ctx.view.song.set(pos, count) || left;
}

}  // namespace o3os::app
