// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What a test host (the PC side of the diag console, tools/o3hil.py) can read of the app in one line (OS-HIL): `diag state`. Machine readable: space separated
// `key=value` pairs, a value has no space (the notification's text has its spaces turned into `_`), the order of the keys is fixed. Read only: it takes the App by
// const reference, so it can change nothing. Main loop only (it reads what the App's pass writes).
//
//   screen=PATTERN page=0 mode=3 overlay=NONE pattern=5 track=0 row=12 col=NOTE playing=1 seq=1 tempo=12800 instrument=1 pads=NOTES shift=0
//   order=- play_pattern=5 play_step=3 posts=2 sel=- msg=SAVE_TRACK_01_DONE
//
//   screen      the registry's name of the base screen, spaces as `_` (PATTERN, SONG, MIXER, SAMPLE_LOAD, ...); page and mode are that screen's own (0-based page; mode as the screen numbers it)
//   overlay     NONE, LEARN, NAVIGATOR, NOTIFY (the strip is up), PATTERN_CHOOSER, LENGTH_CHOOSER, JUMP_CHOOSER, MODULE_PICKER, TEXT_ENTRY, STEP_POPUP
//   pattern/track/row/col   the Pattern screen's cursor, which stays where it was left when another screen is in view: pattern 1..255, track and row 0-based, col NOTE INSTR VOL FX1 FX2
//   playing     the transport's last accepted request (the badge); seq: what the sequencer published (it may lag the badge by a block)
//   tempo       BPM x 100: the sequencer's while it has published one, else the project's, else 0
//   instrument  the last used instrument 1..64
//   pads        the pads' mode (NOTES PIANO TRACKS ISO STEP PATS VU FADERS)
//   order       the song position the sequencer is on, `-` outside a song; play_pattern / play_step: where it is in the pattern
//   sel         (OS-HILNAV) the name of the file or folder under the cursor on File (PROJECTS, IMPORT) and Sample load, and the file stem of the app under the cursor on MODULES, spaces as `_`, at most 40
//               characters, `-` on any other screen or with no entry: a scenario walks to a file or an app by name
//   keys        (OS-KEYROWDLG) present only while a prompt is live: the keys its row says, in the table's order, F1..F8 and SF1..SF8 for Shift + F (a space of a label as `_`), "F3:ALL,F1:KEEP_INS": a dialog's
//               keys, a strip question's answer keys, the boot disk's. Always the last pair, at most 90 characters (the console's buffer is 384)
//   posts       notifications posted since boot (it moves on every one: a repeat of the same text is told from the old one); msg: the last one's text, `-` before any

#include <o3os/app/app.h>

#include <cstddef>

namespace o3os::app {

// Writes the line (no newline) into `out`, NUL terminated; returns its length (cut at cap - 1).
std::size_t describe_state(const App& app, char* out, std::size_t cap) noexcept;

// The code of the front-panel button `name` (app/buttons.h: the lower-case names of the simulator's scenarios, case does not matter) or a decimal code 0..32; -1 when it is neither.
int button_code(const char* name) noexcept;

}  // namespace o3os::app
