// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/app/screens/screen.h>
#include <o3os/core/fixed_string.h>

namespace o3os::app {

// The File screen (docs/design/product-ux.md section 4: "Project new/open/save/export; pages IMPORT, MODULES"). Its first page, OPEN (OS-010a, new
// Community OS behaviour designed after PAR-108): the project folders of the card's Projects/ folder, the way the OG keeps them (an OG project
// is a folder with a project.mt in it, docs/formats/og.md section 1) and the way this OS saves them (a native project, a folder with a project.o3pj, OS-004a): each row says
// which (OG, NATIVE) and F1 opens either through the platform's one load. The listing is the App's scan of the card (dir_scan.h, a job: it appears
// while it is being read and never holds the screen), sorted, with names up to 32 characters shown whole. F1 opens the project under the cursor
// through the same load as `diag load` (ProjectSink); F2 reads the folder again. Up / Down and the encoder move, Left / Right jump to the
// previous / next first letter, Shift + F1..F5 the quarters and the end, as on every list.
//
// F4 SAVE and F5 SAVE AS write the native project through the card's save layer (OS-007n, save_service.h).
// F3 NEW replaces the document with a fresh project (OS-007o), after a second press when the project has changes no save has taken. F6 IMPORT is the second page (OS-FILES part A, docs/specs/OS-FILES-IMPORT.md): the
// card from its root, every folder and the module files (.mod .s3m .med .mmd0-3); F1 ENTER / IMPORT replaces the document with a project made from the module (the same question as NEW first), F3 UP, F7 the engine
// the commands are written for, F8 the report of the last import. OS-FILES part B (docs/specs/OS-FILES-B.md), on the PROJECTS page: the Delete key (a project folder is not renamed on the device)
// marks it deleted (an empty .deleted file in it; the list hides it, nothing is erased) after a second press (not the open project), Shift + F7 PURGEs the instruments nothing uses (one undo unit, the card untouched), and
// the last five projects opened or saved are listed first as the RECENT group (the same folders as the A-Z rows; Settings/recent.o3rc). Not here yet: the MODULES page.
inline constexpr const char* kProjectsFolder = "Projects";
inline constexpr int kFileNameChars = 32;  // PAR-108: project names up to 32 characters; longer ones are cut in the middle (OS-AUDIT-37)
inline constexpr int kFileCutTail = 6;     // OS-AUDIT-37: the last characters a cut name keeps (the extension or the suffix), after the "~"

// F1..F8 of the File screen: at most 8 characters.
// OS-FILES part A: F6 IMPORT is the page of module files (docs/specs/OS-FILES-IMPORT.md); on that page F1 is ENTER / IMPORT, F3 UP, F6 PROJECTS, F7 the engine (FX AUTO / AMIGA / MODRN) and F8 REPORT / LIST.
inline constexpr const char* kFileSoftKeys[8] = {"OPEN", "RESCAN", "NEW", "SAVE", "SAVE AS", "IMPORT", "EXPORT", "--"};
// OS-WAVEXP: the EXPORT page (F7 of the PROJECTS page; docs/specs/OS-WAVEXP.md section 3.1): F1 starts the row in view (SONG, or PATTERN nn: the encoder picks nn), F6 goes to the IMPORT page, F7 back to PROJECTS;
// F4 SAVE and F5 SAVE AS stay as on the other pages and F2, F3 and F8 have nothing to do here.
// While Shift is held (OS-SHIFTROW): the quarter jumps and the end; F6 NAME (OS-NAMES) is the open project's title, on all three pages. OS-FILES part B: the PROJECTS page adds Shift + F7 PURGE
// (kFileShiftKeys); the IMPORT and EXPORT pages have no PURGE (kFileOtherShiftKeys).
inline constexpr const char* kFileShiftKeys[8] = {"0/4", "1/4", "2/4", "3/4", "END", "NAME", "PURGE", "-"};
inline constexpr const char* kFileOtherShiftKeys[8] = {"0/4", "1/4", "2/4", "3/4", "END", "NAME", "-", "-"};
inline constexpr const char* kFileExportSoftKeys[8] = {"EXPORT", "", "", "SAVE", "SAVE AS", "IMPORT", "PROJECTS", ""};

class FileScreen final : public Screen {
 public:
  constexpr FileScreen() noexcept = default;
  void enter(const Model& model, ViewState& view) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r, ui::Rect covered) const noexcept override;
  ScreenId id() const noexcept override { return ScreenId::File; }
  Context context() const noexcept override { return Context::File; }
  const char* const* soft_keys() const noexcept override { return kFileSoftKeys; }
  Context field_context(const Model& model, const ViewState& view) const noexcept override;  // OS-WAVEXP: the EXPORT page
  const char* const* soft_keys_shifted() const noexcept override { return kFileShiftKeys; }  // OS-SHIFTROW: the quarter jumps are bound in the File context; OS-NAMES: F6 NAME; OS-FILES part B: F7 PURGE
  const char* const* shift_row(const ViewState& view) const noexcept override { return view.import.page || view.xport.page ? kFileOtherShiftKeys : kFileShiftKeys; }
  // The cursor: the listing, one axis; left / right are the letter jumps.
  int count(const Model& model, const ViewState& view) const noexcept override;
  int position(const ViewState& view) const noexcept override { return view.xport.page ? static_cast<int>(view.xport.row) : view.file.pos(); }
  int page() const noexcept override { return FileView::kVisibleRows; }
  bool accelerates() const noexcept override { return true; }  // OS-AUDIT-28
  bool set_position(const Model& model, ViewState& view, int position) const noexcept override;
  bool step(const Model& model, ViewState& view, int dx, int dy) const noexcept override;
};
extern const FileScreen kFileScreen;

// The cursor and the window back inside the listing in view, which a scan, a rescan or a load may have changed under them (the App calls it
// when the listing moves). True if the view moved.
bool file_sync(const Model& model, ViewState& view) noexcept;

// OS-NEWKEEP2: takes the text of a File question (NEW's ALL / KEEP, the unsaved-changes guard) off the strip when the question is no longer open. The App calls it once a pass; true when the strip changed.
bool file_question_sync(ViewState& view) noexcept;

// What the list shows of a name: printable ASCII as it is, a UTF-8 character as one '?' (the face has no glyph for it), a control byte as '?', at most
// `width` characters (default kFileNameChars). OS-AUDIT-37: a longer name is cut in the middle: its head, a '~', its last kFileCutTail characters, exactly `width`
// characters in all. The cut is on what is drawn only: the stored name, which the cursor and the Load memory match on, is never changed. Never empty for a non-empty name.
void file_display_name(const char* name, core::FixedString<kFileNameChars>& out, int width = kFileNameChars) noexcept;

// OS-AUDIT-19: true when `entry_name` (a folder of the PROJECTS list) is the open project: model.project_folder ("Projects/My Song", "" when none) names it.
bool file_is_open_project(const char* project_folder, const char* entry_name) noexcept;

}  // namespace o3os::app
