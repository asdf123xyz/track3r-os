// SPDX-License-Identifier: GPL-3.0-or-later
// OS-FILES part B (docs/specs/OS-FILES-B.md): what the File screen's PROJECTS page does to the project folders of the card besides opening and saving them: the Delete key and Shift + F7 PURGE UNUSED.
// DELETE asks the SaveService (app/save_service.h) to mark the folder with an empty file, never an erase or a rename, and never touches the project that is open; PURGE touches the document only. The words are the strip's.
#include "action_handlers.h"

#include <o3os/app/key_prompt.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/save_service.h>
#include <o3os/app/screens/file_screen.h>
#include <o3os/app/text_format.h>
#include <o3os/model/instrument_use.h>

#include <cstring>

namespace o3os::app {

namespace {

namespace guard = hal::storage::save;

// OS-USBDISK 4.3: while the computer has the card the news is asked, ahead of the persistent disk strip (save_actions.cpp).
bool say(ActionContext& ctx, core::Severity severity, const char* text) noexcept {
  return ctx.view.card_on_computer ? ctx.view.notify.ask(severity, text, "file", ViewState::kMessageMs) : ctx.view.notify.upsert(severity, text, "file", ViewState::kMessageMs);
}

// The page these keys belong to: the PROJECTS list (not the IMPORT browser, which lists the card from its root, and not the EXPORT page).
bool projects_page(const ActionContext& ctx) noexcept { return !ctx.view.import.page && !ctx.view.xport.page; }

// The folder of the open project as the loader knows it ("" when none was loaded, or the load failed: nothing is open then).
const char* loaded_folder(const ActionContext& ctx) noexcept { return ctx.project != nullptr && ctx.project->loaded() ? ctx.project->folder() : ""; }

// The project folder under the cursor (the A-Z row, or the RECENT row's folder), or a message and nullptr. `name` gets its name on the card (kNameMax + 1 bytes), `key` the entry's DirIndex key.
bool folder_under_cursor(ActionContext& ctx, char* name, std::uint16_t* key) noexcept {
  const DirScan* scan = ctx.model.files;
  if (scan == nullptr) {
    return say(ctx, core::Severity::Info, "NO SD CARD") && false;
  }
  const DirIndex& index = scan->index();
  const int entry = ctx.view.file.entry(index);
  if (entry < 0 || !index.is_dir(static_cast<unsigned>(entry))) {
    return say(ctx, core::Severity::Info, "NO PROJECT UNDER THE CURSOR") && false;
  }
  const char* listed = index.name(static_cast<unsigned>(entry));
  if (std::strlen(listed) > guard::kNameMax || !SaveService::project_name_ok(listed)) {
    return say(ctx, core::Severity::Warn, "NOT A NAME THE CARD TAKES: USE A COMPUTER") && false;  // (an OG project's name the save layer does not write: it is not touched)
  }
  std::strcpy(name, listed);
  if (key != nullptr) {
    *key = index.key(static_cast<unsigned>(entry));
  }
  return true;
}

// The refusals every card operation shares, said on the strip: false when it may not go on.
bool card_ready(ActionContext& ctx) noexcept {
  if (ctx.saves == nullptr || !ctx.saves->attached()) {
    return say(ctx, core::Severity::Warn, "NO CARD WRITE LAYER") && false;
  }
  if (ctx.project != nullptr && ctx.project->busy()) {
    return say(ctx, core::Severity::Info, "A LOAD IS ALREADY RUNNING") && false;
  }
  if (ctx.saves->busy()) {
    return say(ctx, core::Severity::Info, "A SAVE IS RUNNING: TRY AGAIN WHEN IT ENDS") && false;
  }
  if (ctx.saves->gate() != hal::storage::WriteStatus::Ok) {
    return say(ctx, core::Severity::Warn, SaveService::gate_text(ctx.saves->gate())) && false;
  }
  return true;
}

// What a refused or failed DELETE says; Done says nothing here.
bool move_report(ActionContext& ctx, SaveService::Move move) noexcept {
  core::FixedString<core::kNotifyTextChars> text;
  switch (move) {
    case SaveService::Move::Done: return true;
    case SaveService::Move::NoWriter: text.assign("NO CARD WRITE LAYER"); break;
    case SaveService::Move::NotWritable: text.assign(SaveService::gate_text(ctx.saves->gate())); break;
    case SaveService::Move::Busy: text.assign("A SAVE IS RUNNING: TRY AGAIN WHEN IT ENDS"); break;
    case SaveService::Move::CardInUse: text.assign("FIRMWARE UPDATE HAS THE CARD: TRY AGAIN"); break;
    case SaveService::Move::OpenProject: text.assign("CANNOT DELETE THE OPEN PROJECT"); break;
    case SaveService::Move::BadName: text.assign("NOT A NAME THE CARD TAKES"); break;
    case SaveService::Move::NotFound: text.assign("PROJECT NOT FOUND ON THE CARD"); break;
    case SaveService::Move::Failed: {
      text.assign("DELETE FAILED: ");
      for (const char* p = hal::storage::write_status_name(ctx.saves->move_status()); *p != '\0'; ++p) {  // the card layer's words are lower case
        const char c = (*p >= 'a' && *p <= 'z') ? static_cast<char>(*p - ('a' - 'A')) : *p;
        text.append(&c, 1);
      }
      break;
    }
  }
  return say(ctx, move == SaveService::Move::Failed ? core::Severity::Error : core::Severity::Warn, text.c_str());
}

// "<head><NAME in capitals, cut to fit><tail>" on the strip.
bool say_with_name(ActionContext& ctx, core::Severity severity, const char* head, const char* name, const char* tail) noexcept {
  core::FixedString<core::kNotifyTextChars> text;
  text.assign(head);
  const std::size_t room = core::kNotifyTextChars - std::strlen(head) - std::strlen(tail);
  core::FixedString<kFileNameChars> shown;
  file_display_name(name, shown);
  const std::size_t n = std::strlen(shown.c_str());
  for (std::size_t i = 0; i < n && i < room; ++i) {
    const char c = shown.c_str()[i];
    const char up = (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
    text.append(i + 1u == room && n > room ? "~" : &up, 1);
  }
  text.append(tail);
  return say(ctx, severity, text.c_str());
}

}  // namespace

// The Delete key (PROJECTS page): the folder under the cursor gets the empty marker file .deleted, and the File list and the RECENT group hide it (nothing is moved or erased: free the space, or bring it
// back by removing the marker, from a computer). The first press asks, in the strip, and remembers the entry it named; the second goes on only over that very entry of the same listing (a cursor move,
// another key, leaving the screen, or a listing that is still coming in takes the question back). The open project is refused.
bool file_delete(ActionContext& ctx, core::ActionArg) noexcept {
  const bool pending = ctx.view.file.confirm_delete;
  ctx.view.file.ask_clear();
  if (!projects_page(ctx)) {
    return false;
  }
  if (ctx.view.card_on_computer) {  // OS-USBDISK 4.3: the listing is empty because the card is away; the refusal is the card's, not "no project under the cursor"
    return say(ctx, core::Severity::Warn, SaveService::gate_text(hal::storage::WriteStatus::Host));
  }
  char name[guard::kNameMax + 1];
  std::uint16_t key = 0;
  if (!folder_under_cursor(ctx, name, &key) || !card_ready(ctx)) {
    return true;
  }
  char open[guard::kNameMax + 1];
  if (ctx.saves->open_project_name(loaded_folder(ctx), open, sizeof open) && std::strcmp(open, name) == 0) {
    return move_report(ctx, SaveService::Move::OpenProject);
  }
  const std::uint32_t generation = ctx.model.files->index().generation();
  const bool armed = pending && key == ctx.view.file.target_key && generation == ctx.view.file.target_generation;
  if (!armed) {
    ctx.view.file.confirm_delete = true;
    ctx.view.file.target_key = key;
    ctx.view.file.target_generation = generation;
    return say_with_name(ctx, core::Severity::Warn, "DELETE ", name, "? DELETE AGAIN");
  }
  const SaveService::Move move = ctx.saves->delete_project(loaded_folder(ctx), name);
  if (move != SaveService::Move::Done) {
    return move_report(ctx, move);
  }
  ctx.view.file.rescan = true;  // the name leaves the list (and the RECENT group, which the service has taken it out of)
  return say_with_name(ctx, core::Severity::Info, "DELETED: ", name, "");
}

// Shift + F7 PURGE (PROJECTS page, the open project): the sample and MIDI instruments that nothing names (model/instrument_use.h lists what names one) leave their slots, all in this one undo unit. The first press
// asks and says how many; the second does it. Module instruments are never purged (their count is said). Nothing happens on the card: the samples' files stay in samples/ (OS-TRASHUI, a maintainer decision, will deal with them).
bool file_purge(ActionContext& ctx, core::ActionArg) noexcept {
  const bool armed = ctx.view.file.confirm_purge;
  ctx.view.file.ask_clear();
  if (!projects_page(ctx)) {
    return false;
  }
  if (ctx.edit == nullptr || ctx.model.project == nullptr) {
    return say(ctx, core::Severity::Info, "NO PROJECT");
  }
  unsigned modules = 0;
  std::uint64_t held = 0;  // the S7 instrument clipboard holds a copy of an instrument and a reference to its audio: the slots that read that block are not purged (a paste would find them gone)
  if (ctx.samples != nullptr && ctx.samples->clip_valid() && ctx.samples->clip_pcm().data != nullptr) {
    for (unsigned slot = 0; slot < model::kMaxInstruments; ++slot) {
      held |= ctx.samples->pcm(slot).data == ctx.samples->clip_pcm().data ? std::uint64_t{1} << slot : 0u;
    }
  }
  const std::uint64_t unused = model::instruments_unused(ctx.edit->project(), &modules, held);
  unsigned n = 0;
  for (std::uint64_t m = unused; m != 0u; m &= m - 1u) {
    ++n;
  }
  core::FixedString<core::kNotifyTextChars> text;
  if (n == 0u) {
    text.assign("NO UNUSED INSTRUMENTS");
    if (modules != 0u) {
      text.append(" - ");
      append_uint(text, modules);
      text.append(modules == 1u ? " MODULE KEPT" : " MODULES KEPT");
    }
    return say(ctx, core::Severity::Info, text.c_str());
  }
  if (!armed) {
    ctx.view.file.confirm_purge = true;
    text.assign(kAskHeadPurge);
    append_uint(text, n);
    text.append(n == 1u ? " UNUSED INSTRUMENT?" : " UNUSED INSTRUMENTS?");
    AskText tail;  // " SHIFT+F7 AGAIN", from the question's key table (OS-KEYROWDLG)
    ask_tail(Ask::Purge, tail);
    text.append(tail.c_str());
    return ctx.view.notify.ask(core::Severity::Warn, text.c_str(), "file", ViewState::kMessageMs);
  }
  release_live_pads(ctx);
  unsigned purged = 0;
  for (unsigned slot = 0; slot < model::kMaxInstruments; ++slot) {
    if (((unused >> slot) & 1u) == 0u) {
      continue;
    }
    if (ctx.edit->clear_instrument(slot) == model::Status::Ok) {
      ++purged;
      if (ctx.samples != nullptr) {
        ctx.samples->request_rebind(slot);
      }
    }
  }
  if (ctx.model.modules != nullptr) {
    ctx.model.modules->sync_slots(ctx.edit->project());
  }
  text.assign("PURGED ");
  append_uint(text, purged);
  text.append(purged == 1u ? " INSTRUMENT - F8 ON PARAMS UNDOES" : " INSTRUMENTS - F8 ON PARAMS UNDOES");
  return say(ctx, core::Severity::Info, text.c_str());
}

}  // namespace o3os::app
