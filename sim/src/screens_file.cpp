// SPDX-License-Identifier: GPL-3.0-or-later
// The File screen's gallery and budget states (OS-010a): the screen drawn through the app's registry over a listing that the real scan job
// (app::DirScan) read from an in-memory card (platform/host FakeStorage), so what is measured and shown is what the device draws for a card like it.
#include "screens_app.h"

#include <o3os/app/dir_scan.h>
#include <o3os/app/module_import.h>
#include <o3os/app/model.h>
#include <o3os/app/og_survey.h>
#include <o3os/app/screens/file_screen.h>
#include <o3os/app/screens/scene.h>
#include <o3os/app/view_state.h>
#include <o3os/platform/host/fake_storage.h>

#include <cstring>
#include <string>

namespace o3os::sim {

namespace {

using app::DirScan;
using app::Model;
using app::ScreenId;
using app::ViewState;
using platform::host::FakeStorage;

constexpr std::uint32_t kBigSlice = 1000000;  // a budget that buys a whole step (DirScan::kMaxUnitsPerStep units)

void add_project(FakeStorage& card, const std::string& name) {
  card.add_dir("Projects/" + name);
  card.add_file("Projects/" + name + "/project.mt", 2320);
}

// A project this OS saved (OS-007o): a folder with a project.o3pj, listed with the NATIVE tag.
void add_native_project(FakeStorage& card, const std::string& name) {
  card.add_dir("Projects/" + name);
  card.add_file("Projects/" + name + "/project.o3pj", 41000);
}

// Steps the scan `steps` times (0: to the end).
void run(DirScan& scan, FakeStorage& card, unsigned steps) {
  (void)scan.begin(card, app::kProjectsFolder, app::kProjectFilter);
  for (unsigned i = 0; (steps == 0u || i < steps) && scan.step(kBigSlice) == core::JobStatus::Running; ++i) {
  }
}

// One state: the card, the scan over it, and what the screen is handed. Built once and kept (the harness is not re-entrant, like the budget harness itself).
struct Scene {
  FakeStorage card;
  DirScan scan;
  Model model;
  ViewState view;

  void show(int cursor) {
    model.files = &scan;
    view.screen = ScreenId::File;
    (void)view.file.set(cursor, scan.index());
  }
};

void draw(ui::Renderer& r, const Scene& s) { app::render_scene(s.model, s.view, r); }

// The maintainer's kind of card: projects of every habit in the naming, a folder that is not a project, a stray file, what macOS leaves behind.
Scene& typical() {
  static Scene s;
  static bool built = false;
  if (!built) {
    built = true;
    static const char* const kNames[] = {"acid-jam", "Breaks 2024", "demo song", "Dub Techno", "ElectroBass", "Hardcore_v2", "house-01",
                                         "kick-lab", "Lo-Fi Beats", "morning",     "t-v16",      "TRACK 05",    "untitled",    "Workspace backup"};
    for (const char* name : kNames) {
      add_project(s.card, name);
    }
    add_native_project(s.card, "Track 01");  // what SAVE AS makes
    add_native_project(s.card, "Track 02");
    s.card.add_dir("Projects/docs");
    s.card.add_file("Projects/readme.txt", 120);
    s.card.add_dir("Projects/.Trash");
    s.card.add_file("Projects/._acid-jam", 4096);
    run(s.scan, s.card, 0);
    s.show(2);
  }
  return s;
}

// A card of forty projects, two steps into the scan: the listing is partial, the strip says more are coming.
Scene& scanning() {
  static Scene s;
  static bool built = false;
  if (!built) {
    built = true;
    for (unsigned i = 0; i < 40; ++i) {
      add_project(s.card, "song-" + std::to_string(100 + 7 * i));
    }
    run(s.scan, s.card, 2);
    s.show(0);
  }
  return s;
}

// The dearest content: 300 projects whose names are all longer than the column (cut to 31 characters and a '~', no space to save a word), some with
// UTF-8 in them (one '?' each), so the list fills up at 256 and says so; the cursor on the last row of the last window.
Scene& worst() {
  static Scene s;
  static bool built = false;
  if (!built) {
    built = true;
    for (unsigned i = 0; i < 300; ++i) {
      std::string name = "Project-" + std::to_string(1000 + i) + "-abcdefghijklmnopqrstuvwxyz";
      if (i % 3 == 0) {
        name += "-caf\xC3\xA9";
      }
      add_project(s.card, name);
    }
    run(s.scan, s.card, 0);
    s.show(static_cast<int>(s.scan.index().count()) - 1);
  }
  return s;
}

// A card with no Projects folder.
Scene& no_folder() {
  static Scene s;
  static bool built = false;
  if (!built) {
    built = true;
    s.card.add_dir("Workspace");
    run(s.scan, s.card, 0);
    s.show(0);
  }
  return s;
}

// No card: the scan never started (nothing is attached).
Scene& no_card() {
  static Scene s;
  static bool built = false;
  if (!built) {
    built = true;
    s.show(0);
  }
  return s;
}

// OS-FILES part A: the IMPORT page in /MOD of the maintainer's test card (the list, the TYPE column, the key row with ENTER / IMPORT, UP, PROJECTS, FX AUTO, REPORT).
struct ImportScene : Scene {
  app::ModuleSummary summary;
};

ImportScene& import_scene(bool report) {
  static ImportScene lists;
  static ImportScene reports;
  static bool built = false;
  if (!built) {
    built = true;
    for (ImportScene* s : {&lists, &reports}) {
      s->card.add_dir("MOD");
      s->card.add_dir("S3M");
      s->card.add_dir("MED");
      for (const char* n : {"kc_dancinonamiga.mod", "axelf.mod", "Chiptune Sunset.MOD", "stardust memories.mod", "mod.hidden-name", "readme.txt"}) {
        s->card.add_file(std::string("MOD/") + n, 20000);
      }
      s->card.add_dir("MOD/more");
      (void)s->scan.begin(s->card, "MOD", app::kModuleFilter);
      for (unsigned i = 0; i < 100 && s->scan.step(kBigSlice) == core::JobStatus::Running; ++i) {
      }
      s->model.files = &s->scan;
      s->view.screen = ScreenId::File;
      s->view.import.page = true;
      std::strcpy(s->view.import.folder, "MOD");
      (void)s->view.file.set(2, s->scan.index());
    }
    // the report: a four-channel MOD whose slides run a tick longer and whose loop point the model cannot hold, one of its 15 samples did not fit
    app::ModuleSummary& m = reports.summary;
    m.valid = true;
    m.ok = true;
    m.format = app::ModuleSummary::Format::Mod;
    m.amiga_commands = true;
    m.channels = 4;
    m.patterns = 21;
    m.order = 32;
    m.tracks_kept = 4;
    m.instruments = 15;
    m.samples_loaded = 14;
    m.samples_no_room = 1;
    m.samples_kb = 412;
    m.cmd_approx = 12;
    m.cmd_dropped = 4;
    m.clamped = 3;
    m.dropped_groups = (1u << formats::mod::kDropSlides) | (1u << formats::mod::kDropLoopPoint);
    std::strcpy(m.name, "KC_DANCINONAMIGA.MOD");
    reports.model.import_report = &m;
    reports.view.import.report = true;
  }
  return report ? reports : lists;
}

void draw_import(ui::Renderer& r, unsigned) { draw(r, import_scene(false)); }
void draw_import_report(ui::Renderer& r, unsigned) { draw(r, import_scene(true)); }

// OS-FILES part B: the typical card with the RECENT group above the list (the last projects opened or saved, newest first, the cursor on the second row of the group);
// the newest one is the open project, so the header names it (PROJECT Track 02, by its folder: OS-NAMES).
Scene& recent_scene() {
  static Scene s;
  static bool built = false;
  if (!built) {
    built = true;
    static const char* const kNames[] = {"acid-jam", "Breaks 2024", "demo song", "Dub Techno", "ElectroBass", "Hardcore_v2", "house-01",
                                         "kick-lab", "Lo-Fi Beats", "morning",     "t-v16",      "TRACK 05",    "untitled",    "Workspace backup"};
    for (const char* name : kNames) {
      add_project(s.card, name);
    }
    add_native_project(s.card, "Track 01");
    add_native_project(s.card, "Track 02");
    run(s.scan, s.card, 0);
    s.model.files = &s.scan;
    s.model.project_folder = "Projects/Track 02";
    s.view.screen = ScreenId::File;
    std::uint16_t keys[app::FileView::kMaxLead];
    unsigned n = 0;
    for (const char* name : {"Track 02", "t-v16", "Dub Techno", "acid-jam", "morning"}) {  // newest first
      keys[n++] = s.scan.index().key(static_cast<unsigned>(s.scan.index().find_name(name)));
    }
    (void)s.view.file.set_lead(keys, n, s.scan.index());
    (void)s.view.file.set(1, s.scan.index());
  }
  return s;
}

void draw_recent(ui::Renderer& r, unsigned) { draw(r, recent_scene()); }

// OS-WAVEXP: the EXPORT page (F7 of the PROJECTS page) on the PATTERN row with pattern 3, the card's next free file name shown, and the progress overlay over it at 42 % of the second pass.
Scene& export_scene(bool progress) {
  static Scene pages;
  static Scene overlays;
  static bool built = false;
  if (!built) {
    built = true;
    for (Scene* s : {&pages, &overlays}) {
      s->show(0);
      app::ExportView& x = s->view.xport;
      x.page = true;
      x.row = app::ExportView::Pattern;
      x.pattern = 3;
      std::strcpy(x.preview, "EXPORT/T-V16/T-V16_P3_0001.WAV");
      x.fresh = true;
    }
    app::ExportView& x = overlays.view.xport;
    x.running = true;
    x.request.what = audio::OfflineRequest::What::Pattern;
    x.request.pattern = 3;
    x.pass = 1;
    x.permille = 420;
    std::strcpy(x.file, "EXPORT/T-V16/T-V16_P3_0001.WAV");
    overlays.view.overlay = app::OverlayId::ExportProgress;
    overlays.view.shift = false;
  }
  return progress ? overlays : pages;
}

void draw_export(ui::Renderer& r, unsigned) { draw(r, export_scene(false)); }
void draw_export_progress(ui::Renderer& r, unsigned) { draw(r, export_scene(true)); }

// OS-WAVEXP slice B: the EXPORT page on the PATT STEMS row, and the progress overlay of a set of stems at its third file of five (the second T-file after the MIX's measuring pass), 53 % of the set.
Scene& export_stems_scene(bool progress) {
  static Scene pages;
  static Scene overlays;
  static bool built = false;
  if (!built) {
    built = true;
    for (Scene* s : {&pages, &overlays}) {
      s->show(0);
      app::ExportView& x = s->view.xport;
      x.page = true;
      x.row = app::ExportView::PatternStems;
      x.pattern = 3;
      std::strcpy(x.preview, "EXPORT/T-V16/T-V16_P3_S0001_*.WAV");
      x.fresh = true;
    }
    app::ExportView& x = overlays.view.xport;
    x.running = true;
    x.kind = app::ExportView::Stems;
    x.request.what = audio::OfflineRequest::What::Pattern;
    x.request.pattern = 3;
    x.pass = 1;
    x.stem = 3;
    x.stems = 5;
    std::strcpy(x.part, "T06");
    x.permille = 530;
    std::strcpy(x.file, "EXPORT/T-V16/T-V16_P3_S0001_*.WAV");
    overlays.view.overlay = app::OverlayId::ExportProgress;
    overlays.view.shift = false;
  }
  return progress ? overlays : pages;
}

void draw_export_stems(ui::Renderer& r, unsigned) { draw(r, export_stems_scene(false)); }
void draw_export_stems_progress(ui::Renderer& r, unsigned) { draw(r, export_stems_scene(true)); }

// OS-FILES-C C2c: the EXPORT page on the OG PROJECT row with its survey (folder, size, what would be left out), the same with the question on the strip, the result strip after a good export, and the
// progress overlay at the 11th of 29 files. The project is the typical card's t-v16-like one: 5 sample instruments not written (one of them 12 BIT, two on the Amiga engine, one too long), 2 clamped levels,
// 3 modules and 14 effect columns left out.
enum class OgScene { Page, Ask, Done, Progress, Counting, Many };

Scene& export_og_scene(OgScene which) {
  static Scene scenes[6];
  static bool built = false;
  if (!built) {
    built = true;
    for (Scene& sc : scenes) {
      sc.show(0);
      app::ExportView& x = sc.view.xport;
      x.page = true;
      x.row = app::ExportView::OgProject;
      x.pattern = 3;
      x.fresh = true;
      x.og_valid = true;
      std::strcpy(x.og.folder, "Song OG");
      x.og.files = 29;
      x.og.bytes = 7'600'000u;
      x.og.drops.instruments_not_written = 5;
      x.og.drops.bit_engine = 1;
      x.og.drops.amiga_engine = 2;
      x.og.drops.pcm_too_long = 1;
      x.og.drops.levels_clamped = 2;
      x.og.drops.module_instruments = 3;
      x.og.drops.core_fx = 11;
      x.og.drops.engine_fx = 3;
    }
    Scene& ask = scenes[static_cast<int>(OgScene::Ask)];
    char items[32];
    (void)app::og_loss_line(ask.view.xport.og.drops, items, sizeof items);
    char line[48] = "LOSES ";
    std::strcat(line, items);
    std::strcat(line, ": F1 AGAIN");
    (void)ask.view.notify.post(core::Severity::Warn, line, "file", 4000);
    ask.view.file.og_ask = true;
    ask.view.file.og_ask_text.assign(line);
    Scene& done = scenes[static_cast<int>(OgScene::Done)];
    done.view.xport.og.files = 29;
    (void)done.view.notify.post(core::Severity::Info, "SAVED SONG OG (29 FILES)", "job", 4000);
    Scene& prog = scenes[static_cast<int>(OgScene::Progress)];
    app::ExportView& px = prog.view.xport;
    px.running = true;
    px.kind = app::ExportView::Og;
    px.og_done = 10;
    px.og_total = 29;
    px.permille = 344;
    px.pass = 1;
    std::strcpy(px.file, "EXPORT/SONG OG");
    prog.view.overlay = app::OverlayId::ExportProgress;
    prog.view.shift = false;
    // the survey is being made a slice a pass: the page says COUNTING and shows no size or kinds yet (OS-FILES-C C2c minors)
    Scene& counting = scenes[static_cast<int>(OgScene::Counting)];
    counting.view.xport.fresh = false;
    counting.view.xport.og_valid = false;
    counting.view.xport.og = app::OgSurvey{};
    // all fourteen kinds: more than the page has lines for, so each is named in its short form, run together and wrapped
    Scene& many = scenes[static_cast<int>(OgScene::Many)];
    formats::og::ExportDrops& d = many.view.xport.og.drops;
    d.instruments_not_written = 12;
    d.no_sample_memory = 2;
    d.bit_engine = 1;
    d.amiga_engine = 2;
    d.pcm_too_long = 1;
    d.levels_clamped = 2;
    d.sample_slots_beyond_48 = 1;
    d.tracks_beyond_12 = 6;
    d.track_settings = 4;
    d.midi_instruments = 2;
    d.track_names_cut = 3;
    d.project_settings = 1;
  }
  return scenes[static_cast<int>(which)];
}

void draw_export_og(ui::Renderer& r, unsigned) { draw(r, export_og_scene(OgScene::Page)); }
void draw_export_og_ask(ui::Renderer& r, unsigned) { draw(r, export_og_scene(OgScene::Ask)); }
void draw_export_og_done(ui::Renderer& r, unsigned) { draw(r, export_og_scene(OgScene::Done)); }
void draw_export_og_counting(ui::Renderer& r, unsigned) { draw(r, export_og_scene(OgScene::Counting)); }
void draw_export_og_many(ui::Renderer& r, unsigned) { draw(r, export_og_scene(OgScene::Many)); }
void draw_export_og_progress(ui::Renderer& r, unsigned) { draw(r, export_og_scene(OgScene::Progress)); }

void draw_typical(ui::Renderer& r, unsigned) { draw(r, typical()); }
void draw_scanning(ui::Renderer& r, unsigned) { draw(r, scanning()); }
void draw_worst(ui::Renderer& r, unsigned) { draw(r, worst()); }
void draw_no_folder(ui::Renderer& r, unsigned) { draw(r, no_folder()); }
void draw_no_card(ui::Renderer& r, unsigned) { draw(r, no_card()); }

}  // namespace

const ScreenState kFileStates[15] = {
    {"typical", true, 1, kBudgetWords, draw_typical},
    {"scanning", true, 1, kBudgetWords, draw_scanning},
    {"worst", true, 1, kBudgetWords, draw_worst},
    {"no_folder", true, 1, kBudgetWords, draw_no_folder},
    {"no_card", true, 1, kBudgetWords, draw_no_card},
    {"import", true, 1, kBudgetWords, draw_import},                  // OS-FILES part A
    {"import_report", true, 1, kBudgetWords, draw_import_report},
    {"export", true, 1, kBudgetWords, draw_export},                  // OS-WAVEXP
    {"export_stems", true, 1, kBudgetWords, draw_export_stems},      // OS-WAVEXP slice B
    {"recent", true, 1, kBudgetWords, draw_recent},                  // OS-FILES part B
    {"export_og", true, 1, kBudgetWords, draw_export_og},            // OS-FILES-C C2c: the OG PROJECT row
    {"export_og_ask", true, 1, kBudgetWords, draw_export_og_ask},    // ... with the question on the strip
    {"export_og_done", true, 1, kBudgetWords, draw_export_og_done},  // ... and the result strip
    {"export_og_counting", true, 1, kBudgetWords, draw_export_og_counting},  // ... while the survey is still being counted a slice a pass
    {"export_og_many", true, 1, kBudgetWords, draw_export_og_many},  // ... with more kinds than lines: all named, short, wrapped
};

// OS-WAVEXP: the progress overlay of a running export (modal, over the EXPORT page).
const ScreenState kExportStates[3] = {
    {"progress", true, 1, kBudgetWords, draw_export_progress},
    {"stems_progress", true, 1, kBudgetWords, draw_export_stems_progress},  // slice B
    {"og_progress", true, 1, kBudgetWords, draw_export_og_progress},        // OS-FILES-C C2c
};

}  // namespace o3os::sim
