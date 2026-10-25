// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The WAV export's view state (OS-WAVEXP slice A, docs/specs/OS-WAVEXP.md): the File screen's EXPORT page (the rows SONG and PATTERN nn, F7 of the PROJECTS page) and the progress overlay
// of a running export (modal, one for every home: the File page here and the Pattern screen's TOOLS entry later). NEW COMMUNITY OS BEHAVIOUR.
// The actions (export_actions.cpp) fill it, the App's pass (App::poll_export) follows the job and closes the overlay, the screens only draw it.

#include <o3os/app/og_survey.h>
#include <o3os/audio/offline.h>

#include <cstdint>

namespace o3os::app {

struct ExportView {
  static constexpr unsigned kRows = 5;  // SONG, PATTERN, and (slice B) SONG STEMS, PATT STEMS, and (OS-FILES-C C2c) OG PROJECT
  enum Row : std::uint8_t { Song = 0, Pattern = 1, SongStems = 2, PatternStems = 3, OgProject = 4 };
  enum Kind : std::uint8_t { Mix = 0, Stems = 1, Selection = 2, Og = 3 };  // what a running export makes; Og: the whole project as an OG folder (no WAV, no render: SaveService's og_ set of jobs)
  static constexpr bool has_pattern(unsigned row) noexcept { return row == Pattern || row == PatternStems; }  // the rows the encoder changes the pattern of
  static constexpr bool stems_row(unsigned row) noexcept { return row == SongStems || row == PatternStems; }
  static constexpr unsigned kFileChars = 72;  // "EXPORT/<project>/<project>_P255_0001.WAV": 6 + 1 + 31 + 1 + 31 + 4, upper case, with the terminator

  // The EXPORT page of the File screen.
  bool page = false;
  std::uint8_t row = Song;
  std::uint8_t pattern = 0;  // the pattern the PATTERN row exports, 1..255; 0: not chosen yet (the Pattern screen's pattern is taken when the page opens)
  bool fresh = false;        // `preview` is the name the row in view would make now (the actions clear it; App::poll_export looks the name up again on the card); on the OG PROJECT row `og` is the survey of it, which App::poll_export makes a slice a pass (SaveService::og_survey_step) and sets `fresh` when it is done
  // The OG PROJECT row (OS-FILES-C C2c): what the export would make and leave out, asked again whenever `fresh` is cleared; and, while one runs, the files done of the total (the overlay's "FILE n OF m").
  OgSurvey og;
  bool og_valid = false;     // `og` is a survey (false: the card or the name did not allow one, or it is not finished: `fresh` is false then and the page says COUNTING)
  std::uint8_t og_why = 0;   // when the survey could not be made: the SaveService::Start that says why (F1 words it)
  std::uint16_t og_done = 0;
  std::uint16_t og_total = 0;
  char preview[kFileChars] = "";  // that name, "EXPORT/<PROJECT>/<NAME>.WAV", upper case; "" when there is none (no card, no free number)

  // A running export (the progress overlay is up, ViewState::overlay == ExportProgress).
  bool running = false;
  int slot = -1;                   // the job runner's slot of the export's job once it has started (App::report_jobs words its end)
  bool armed = false;              // the transport was told to stop: the export starts when the sequencer's position says it has (App::poll_export)
  audio::OfflineRequest request{};  // what is exported (armed, and for the overlay's title)
  std::uint8_t kind = Mix;         // ... and in what form: one file of the mix, a set of stems, or the Pattern's selection (slice B)
  std::uint8_t stem = 0;           // stems: the file in progress, 1-based, of `stems`; both 0 until the set has started
  std::uint8_t stems = 0;
  char part[8] = "";               // ... and its name inside the file's ("T03", "MIX")
  std::uint16_t permille = 0;      // 0..1000, the export source's own account (SaveService::export_progress)
  std::uint8_t pass = 0;           // 0 the measuring pass, 1 the writing pass (the overlay says "PASS 1/2")
  char stopped[32] = "";           // OS-EXPNOTE: what the export stopped when it took the engine (the held live notes), "STOPPED MIDI CH10 C-4"; the strip says it after the result (App::finish_export)
  char file[kFileChars] = "";      // the file being made, "EXPORT/<PROJECT>/<NAME>.WAV", upper case (the overlay and the strip say it)
};

}  // namespace o3os::app
