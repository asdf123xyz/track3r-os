// SPDX-License-Identifier: GPL-3.0-or-later
// The WAV export's actions (OS-WAVEXP slice A; docs/specs/OS-WAVEXP.md section 5; export_view.h): the File screen's EXPORT page (F1 starts the row in view, the encoder picks the pattern of the PATTERN
// row), the export of the pattern in view from anywhere (Pattern F7 TOOLS / EXPORT PATTERN, bound by OS-H3A), and the progress overlay's F8 CANCEL.
//
// The order of a start: the transport is stopped first (the live render stands down for the render, so the sequencer must not be running: OfflineRender refuses
// "STOP THE TRANSPORT FIRST"). The sequencer follows a stop a block later, so the export is ARMED and the App's pass (App::poll_export) runs it (export.run) as soon as the published position says
// the transport has stopped. The job is a normal save (SaveService::save_export): a cancel, a failure or a pulled card leaves no file under the final name.
#include "action_handlers.h"

#include <o3os/app/key_prompt.h>
#include <o3os/app/og_export.h>
#include <o3os/app/recorder.h>
#include <o3os/app/save_service.h>

#include <cctype>
#include <cstring>

namespace o3os::app {

// The upper-case copy of `in` into `out` (cap bytes with the terminator, cut if it does not fit): the export's file names, and the firmware dialog's fault texts (action_handlers.h).
void upper_copy(char* out, std::size_t cap, const char* in) noexcept {
  std::size_t n = 0;
  for (; in[n] != '\0' && n + 1u < cap; ++n) {
    out[n] = static_cast<char>(std::toupper(static_cast<unsigned char>(in[n])));
  }
  out[n] = '\0';
}

namespace {

// OS-USBDISK 4.3: while the computer has the card the news is asked, ahead of the persistent disk strip (save_actions.cpp).
bool say(ActionContext& ctx, core::Severity severity, const char* text) noexcept {
  return ctx.view.card_on_computer ? ctx.view.notify.ask(severity, text, "export", ViewState::kMessageMs) : ctx.view.notify.upsert(severity, text, "export", ViewState::kMessageMs);
}

bool pattern_exists(const ActionContext& ctx, unsigned id) noexcept { return id >= 1u && id <= 255u && ctx.model.project != nullptr && ctx.model.project->pattern(id) != nullptr; }

// What a save needs of the card and the document, said on the strip when it is missing: false when the export may not start.
bool ready(ActionContext& ctx, const audio::OfflineRequest& rq) noexcept {
  if (ctx.view.xport.running) {
    return false;  // (cannot happen with focus: the overlay is up)
  }
  if (ctx.saves == nullptr || !ctx.saves->attached() || ctx.session == nullptr) {
    return say(ctx, core::Severity::Warn, "NO CARD WRITE LAYER") && false;
  }
  if (!ctx.saves->export_attached()) {
    return say(ctx, core::Severity::Warn, "NO EXPORT ON THIS BUILD") && false;
  }
  if (ctx.model.project == nullptr) {
    return say(ctx, core::Severity::Info, "NO PROJECT TO EXPORT") && false;
  }
  if (ctx.project != nullptr && ctx.project->busy()) {
    return say(ctx, core::Severity::Info, "A LOAD IS RUNNING") && false;
  }
  if (ctx.saves->busy()) {
    return say(ctx, core::Severity::Info, "A SAVE IS ALREADY RUNNING") && false;
  }
  if (ctx.saves->gate() != hal::storage::WriteStatus::Ok) {
    return say(ctx, core::Severity::Warn, SaveService::gate_text(ctx.saves->gate())) && false;
  }
  if (rq.what == audio::OfflineRequest::What::Song && ctx.model.project->song_length() == 0u) {
    return say(ctx, core::Severity::Info, "THE SONG IS EMPTY") && false;
  }
  if (rq.what == audio::OfflineRequest::What::Pattern && !pattern_exists(ctx, rq.pattern)) {
    return say(ctx, core::Severity::Info, "NO SUCH PATTERN") && false;
  }
  return true;
}

ExportRequest export_request(const ActionContext& ctx) noexcept {
  ExportRequest er;
  er.what = ctx.view.xport.request;
  er.loaded_folder = ctx.project != nullptr && ctx.project->loaded() ? ctx.project->folder() : "";
  er.form = ctx.view.xport.kind == ExportView::Stems ? ExportRequest::Form::Stems : (ctx.view.xport.kind == ExportView::Selection ? ExportRequest::Form::Selection : ExportRequest::Form::Mix);
  er.project = ctx.model.project;
  return er;
}

// The request becomes an armed export under the progress overlay; it starts at once when the sequencer is not running, else when it has stopped.
bool begin(ActionContext& ctx, const audio::OfflineRequest& rq, std::uint8_t kind = ExportView::Mix) noexcept {
  if (!ready(ctx, rq)) {
    return true;  // (the strip says why)
  }
  ExportView& x = ctx.view.xport;
  x.request = rq;
  x.kind = kind;
  x.stem = 0;
  x.stems = 0;
  x.part[0] = '\0';
  x.running = true;
  x.armed = true;
  x.permille = 0;
  x.pass = 0;
  x.stopped[0] = '\0';
  x.file[0] = '\0';
  ctx.view.overlay = OverlayId::ExportProgress;
  if (ctx.view.playing || ctx.view.play.playing) {
    if (ctx.transport != nullptr && ctx.view.playing) {
      (void)transport_play(ctx, 0);  // the Play key's own stop: the badge, LIVE mode and the take follow it
    }
    return true;  // armed: App::poll_export starts it when the position says the sequencer has stopped
  }
  return export_run(ctx, 0);
}

// ---- The OG PROJECT row (OS-FILES-C C2c) --------------------------------------------------------------------------------------------------------------------------------------------

// The question is up: asked, not taken back, not older than its text on the strip, and that text is still the head (another message in front of it would let a quick second F1 answer a question
// nobody has read; it then only asks again). It is posted from the File screen's source ("file", the label the strip shows) with its own text kept in FileView::og_ask_text, which is how
// file_question_sync takes it off when it is taken back.
bool og_question_open(const ActionContext& ctx) noexcept {
  return file_question(ctx.view) == Ask::Og && core::tick_elapsed(ctx.now, ctx.view.file.og_asked_at) <= core::ticks_from_ms(FileView::kNewAskMs);  // (OS-KEYROWDLG: the key row marks F1 by the same test)
}

// F1 on OG PROJECT. The checks a start needs of the card and the document (said on the strip when one is missing), the survey, the question when anything would be left out or rewritten (the second F1
// recounts: a project that changed under the question asks again), then the progress overlay: the transport is stopped first, and the App's pass (App::poll_export) starts the set when the sequencer has stopped.
bool og_export(ActionContext& ctx, bool asked) noexcept {
  ExportView& x = ctx.view.xport;
  if (x.running) {
    return false;
  }
  if (ctx.saves == nullptr || !ctx.saves->attached() || ctx.session == nullptr) {
    return say(ctx, core::Severity::Warn, "NO CARD WRITE LAYER");
  }
  if (ctx.model.project == nullptr) {
    return say(ctx, core::Severity::Info, "NO PROJECT TO EXPORT");
  }
  if (ctx.project != nullptr && ctx.project->busy()) {
    return say(ctx, core::Severity::Info, "A LOAD IS RUNNING");
  }
  if (ctx.saves->busy()) {
    return say(ctx, core::Severity::Info, "A SAVE IS ALREADY RUNNING");
  }
  if (ctx.saves->gate() != hal::storage::WriteStatus::Ok) {
    return say(ctx, core::Severity::Warn, SaveService::gate_text(ctx.saves->gate()));
  }
  if (ctx.rec != nullptr && ctx.rec->state() != Recorder::State::Idle) {
    return say(ctx, core::Severity::Info, "A RECORDING OR A TAKE IS IN PROGRESS");
  }
  // The survey is made a slice a pass by App::poll_export (the page says COUNTING until it is done): F1 waits for it, and for one that still describes the document (a change starts it again)
  const auto counting = [&ctx]() noexcept { return ctx.view.notify.upsert(core::Severity::Info, kOgCountingText, "file", ViewState::kMessageMs); };
  if (!x.fresh) {
    return counting();
  }
  if (!x.og_valid) {
    return say(ctx, core::Severity::Warn, SaveService::og_start_text(static_cast<SaveService::Start>(x.og_why), ctx.saves->gate()));
  }
  if (!ctx.saves->og_survey_current()) {
    x.fresh = false;
    return counting();
  }
  const OgSurvey& sv = x.og;
  FileView& f = ctx.view.file;
  const std::uint32_t sig = og_loss_signature(sv.drops);
  if (sv.drops.any_lost() && !(asked && f.og_sig == sig)) {
    char items[core::kNotifyTextChars];
    constexpr const char* kHead = "LOSES ";  // the strip is 48 characters: the head and the tail leave 32 for the kinds (the page lists them all)
    AskText tail;  // ": F1 AGAIN", from the question's key table
    ask_tail(Ask::Og, tail);
    (void)og_loss_line(sv.drops, items, static_cast<unsigned>(core::kNotifyTextChars - std::strlen(kHead) - tail.size()));
    core::FixedString<core::kNotifyTextChars> text;
    text.assign(kHead);
    text.append(items);
    text.append(tail.c_str());
    f.og_ask = true;
    f.og_asked_at = ctx.now;
    f.og_sig = sig;
    f.og_ask_text.assign(text.c_str());
    return ctx.view.notify.ask(core::Severity::Warn, text.c_str(), "file", FileView::kNewAskMs);
  }
  if (!f.og_ask_text.empty()) {
    ctx.view.notify.dismiss("file", f.og_ask_text.c_str());
    f.og_ask_text.clear();
  }
  x.og_done = 0;
  x.og_total = static_cast<std::uint16_t>(sv.files);
  x.kind = ExportView::Og;
  x.stem = 0;
  x.stems = 0;
  x.part[0] = '\0';
  x.running = true;
  x.armed = true;
  x.permille = 0;
  x.pass = 0;
  x.stopped[0] = '\0';
  upper_copy(x.file, sizeof x.file, "Export/");
  std::size_t n = std::strlen(x.file);
  upper_copy(x.file + n, sizeof x.file - n, sv.folder);
  ctx.view.overlay = OverlayId::ExportProgress;
  if (ctx.view.playing || ctx.view.play.playing) {
    if (ctx.transport != nullptr && ctx.view.playing) {
      (void)transport_play(ctx, 0);  // the Play key's own stop, as the WAV export's
    }
  }
  return true;  // armed: App::poll_export starts the set once the position says the sequencer has stopped
}

}  // namespace

// F1 on the EXPORT page.
bool file_export(ActionContext& ctx, core::ActionArg) noexcept {
  const bool og_asked = og_question_open(ctx);
  ctx.view.file.ask_clear();
  if (ctx.view.xport.row == ExportView::OgProject) {
    return og_export(ctx, og_asked);
  }
  audio::OfflineRequest rq;
  const unsigned row = ctx.view.xport.row;
  if (row == ExportView::Song || row == ExportView::SongStems) {
    rq.what = audio::OfflineRequest::What::Song;
  } else {
    rq.what = audio::OfflineRequest::What::Pattern;
    rq.pattern = ctx.view.xport.pattern;
  }
  return begin(ctx, rq, ExportView::stems_row(row) ? ExportView::Stems : ExportView::Mix);
}

// The pattern in view (the Pattern screen's), once, with its tail: Pattern F7 TOOLS / EXPORT PATTERN.
bool export_pattern(ActionContext& ctx, core::ActionArg) noexcept {
  audio::OfflineRequest rq;
  rq.what = audio::OfflineRequest::What::Pattern;
  rq.pattern = ctx.view.edit.pattern < 1 ? std::uint16_t{1} : static_cast<std::uint16_t>(ctx.view.edit.pattern);
  return begin(ctx, rq);
}

// The Song screen's rectangle (F6 TOOLS / EXPORT WAV, OS-SONGSHARE S5, NEW): the order positions `first..last` once, from the first slot to the end of the last, with the tail of the effects as the song's export
// has it. `track_mask` is the rectangle's tracks (a muted or soloed track in them sounds, as in the selection), kAllTracks with nothing selected in the map and on PAT: the project's own mixer, as the whole song
// is exported. The file is Export/<project>/<project>_S<first>-<last>_0001.wav (save_service.cpp). A last slot past the list is its end (the render clamps it). A mask the caller builds is checked where it is
// built (songtools_apply): here 0 is the caller's explicit kAllTracks, so an empty mask cannot be told from it.
bool export_slots(ActionContext& ctx, unsigned first, unsigned last, std::uint16_t track_mask) noexcept {
  audio::OfflineRequest rq;
  rq.what = audio::OfflineRequest::What::Song;
  rq.order_first = static_cast<std::uint16_t>(first);
  rq.order_last = static_cast<std::uint16_t>(last);
  rq.track_mask = track_mask;
  return begin(ctx, rq);
}

// The Pattern screen's block (the TOOLS list's EXPORT SELECTION, listed only while there is one; OS-WAVEXP slice B, spec section 4.3): the block's tracks only, from its first row to its last, with
// a sharp start and the tail trimmed at the first silence (PAR-151). The rows are the block's own, cut to the pattern's length; the tracks are its columns (a muted or soloed track in them sounds: the
// selection is the tracks it names). The file is Export/<project>/<project>_SEL0001.wav.
bool export_selection(ActionContext& ctx, core::ActionArg) noexcept {
  const ViewState& v = ctx.view;
  if (!v.edit.selection.active) {
    return say(ctx, core::Severity::Info, "NO BLOCK TO EXPORT");
  }
  if (ctx.model.project == nullptr) {
    return say(ctx, core::Severity::Info, "NO PROJECT TO EXPORT");
  }
  const Selection::Block b = v.edit.selection.block(v.pattern.row(), v.pattern.track());  // (OS-AUDIT-56: a column block exports as whole steps; the progress overlay covers the strip, so no notice)
  const unsigned id = v.edit.pattern < 1 ? 1u : static_cast<unsigned>(v.edit.pattern);
  unsigned steps = ctx.model.project->pattern_length(id);
  if (steps == 0u) {
    steps = ctx.model.project->default_pattern_length();
  }
  if (b.row0 < 0 || static_cast<unsigned>(b.row0) >= steps) {
    return say(ctx, core::Severity::Info, "THE BLOCK IS PAST THE PATTERN");
  }
  audio::OfflineRequest rq;
  rq.what = audio::OfflineRequest::What::Pattern;
  rq.pattern = static_cast<std::uint16_t>(id);
  rq.row_first = static_cast<std::uint16_t>(b.row0);
  rq.row_last = static_cast<std::uint16_t>(static_cast<unsigned>(b.row1) < steps ? b.row1 : steps - 1u);
  std::uint16_t mask = 0;
  for (int t = b.track0 < 0 ? 0 : b.track0; t <= b.track1 && t < 16; ++t) {
    mask = static_cast<std::uint16_t>(mask | (1u << t));
  }
  if (mask == 0u) {  // 0 is kAllTracks (no choice): a block with no track in 0..15 must not become every track
    return say(ctx, core::Severity::Info, "NO TRACK TO EXPORT");
  }
  rq.track_mask = mask;
  rq.trim_tail = true;
  return begin(ctx, rq, ExportView::Selection);
}

// The transport has stopped (or never ran): the armed export starts. Called by begin() and by the App's pass; an overlay that is not armed means there is nothing to start.
bool export_run(ActionContext& ctx, core::ActionArg) noexcept {
  ExportView& x = ctx.view.xport;
  if (!x.running || !x.armed || ctx.saves == nullptr || ctx.session == nullptr) {
    return false;
  }
  const ExportRequest er = export_request(ctx);
  char path[hal::storage::save::kPathCap] = {};
  (void)ctx.saves->export_name(er, path, sizeof path);
  const SaveService::Start start = ctx.saves->save_export(ctx.session->jobs, er);
  x.armed = false;
  if (start != SaveService::Start::Started) {
    x.running = false;
    ctx.view.overlay = OverlayId::None;
    x.fresh = false;
    (void)save_report(ctx, start);
    return true;
  }
  upper_copy(x.file, sizeof x.file, path);
  x.slot = ctx.saves->export_slot();
  if (x.kind == ExportView::Stems) {
    x.stems = static_cast<std::uint8_t>(ctx.saves->stems_total());
    x.stem = static_cast<std::uint8_t>(ctx.saves->stem_index());
    ctx.saves->stem_now(x.part, sizeof x.part);
  }
  return true;
}

// F8 of the progress overlay. Armed (the transport has not stopped yet): the export is simply not started. Running: the job is cancelled; the overlay stays until the job has ended (App::poll_export),
// and the file being written is removed by the save's own cleanup (app/atomic_save.h).
bool export_cancel(ActionContext& ctx, core::ActionArg) noexcept {
  ExportView& x = ctx.view.xport;
  if (!x.running) {
    return false;
  }
  if (x.armed) {
    x.armed = false;
    x.running = false;
    x.fresh = false;
    ctx.view.overlay = OverlayId::None;
    (void)say(ctx, core::Severity::Info, "EXPORT CANCELLED");
    return true;
  }
  if (ctx.saves != nullptr && ctx.session != nullptr) {
    if (x.kind == ExportView::Og) {
      ctx.saves->cancel_og_export(ctx.session->jobs);  // no file after the one in progress; the overlay stays until the set has ended (App::poll_og)
    } else {
      ctx.saves->cancel_export(ctx.session->jobs);
    }
  }
  return true;
}

// The encoder on the PATTERN and PATT STEMS rows: the next (or previous) pattern that exists, `arg` of them. The SONG rows have no number.
bool export_value(ActionContext& ctx, core::ActionArg arg) noexcept {
  ExportView& x = ctx.view.xport;
  if (!x.page || !ExportView::has_pattern(x.row) || arg == 0) {
    return false;
  }
  int id = x.pattern;
  int left = arg > 255 ? 255 : (arg < -255 ? -255 : arg);
  const int dir = left > 0 ? 1 : -1;
  left = left > 0 ? left : -left;
  while (left > 0) {
    int next = id + dir;
    while (next >= 1 && next <= 255 && !pattern_exists(ctx, static_cast<unsigned>(next))) {
      next += dir;
    }
    if (next < 1 || next > 255) {
      break;  // no further pattern that way: it stops there
    }
    id = next;
    --left;
  }
  if (id == x.pattern) {
    return false;
  }
  x.pattern = static_cast<std::uint8_t>(id);
  x.fresh = false;
  return true;
}

// OS-SHIFTENC (Shift + encoder): 8 existing patterns a detent; it stops at the last one that way, as the fine step does.
bool export_coarse(ActionContext& ctx, core::ActionArg arg) noexcept { return export_value(ctx, (arg > 31 ? 31 : (arg < -31 ? -31 : arg)) * 8); }

}  // namespace o3os::app
