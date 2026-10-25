// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-QANET: the rules of docs/specs/OS-QANET.md as functions over a simulator state before and after ONE action, so that every part of the net can ask them: the fresh-state walk of rules_test.cpp (A1), and
// the random user of part B, which applies them at every step of a sequence and so catches what only shows mid-sequence.
//
//   RULE 1 (A1, amended with the undo policy of the maintainer, 2026-10-22). Undo covers the pattern data (and the song), the instruments and their parameters, and the audio of the samples. A change to one of
//                these must leave an undo unit, or have asked a question first (the prompt of app/key_prompt.h was up when it ran: it is the answer): that includes a load over a used sample slot. A destructive
//                change outside the three (on the Mixer's STEMS page Delete and Shift + Delete) asks first. Every other change is exempt (the Mixer's levels, mutes and solos, the settings, a load into a free slot, a new instrument in an empty
//                slot: what a step only adds to an empty slot is not a change of anything that was there). An action that lowers the undo depth of any area is allowed only for File NEW and OPEN, and undo and redo
//                (the sample editor's own undo included), and for a step whose changes are all of the exempt kind (a load into a free slot clears the history).
//
//   RULE 2 (A2). A key that is bound must do something or say something: if pressing it changes nothing (no document, view, selection, transport or sound change) and posts no notification, it is a silent no-op.
//                A key that does something (or says something: a refusal is an answer) has a label in the key row (F keys), and a label in the key row has a binding.
//
//   RULE 3 (A3). An operation on a selection acts on the selection: what a block, a range of slots, a slice, a sample selection or an instrument slot may change is the span of its entry in selection_ops.cpp (written from the
//                specs), the document outside the span must not change and the result inside it is the spec's. Rule 3 needs a copy of the document and the selection, not a digest: it lives in selection_ops.h (image_of(),
//                check_selection()), in the same library, and B4 calls it beside check_rules().
//
// A rule that is added later joins check_rules() and keeps its signature: the state before, the state after, the name of the action that ran.
#pragma once

#include <o3os/sim/simulator.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace qanet {

constexpr unsigned kRings = 3;  // undo rings: pattern, song, sound (model/undo.h kUndoAreas)
constexpr unsigned kInstSlots = 64;  // model::kMaxInstruments and SampleBank::kSlots (static_assert in rules.cpp)
constexpr unsigned kPcmSlots = 64;

// What a rule looks at, read from the simulator by snap_of().
struct Snap {
  std::string digest;  // the native project (the digest of `mark model`) and the PCM of every sample slot
  bool digest_error = false;  // a document is open and the native writer could not digest it: the step cannot be judged (a finding of its own)
  // The undo rings (model/undo.h). `ring` is the depth now. `added` is depth + dropped_units(): a full ring evicts its oldest unit for a new one (the depth stays or even falls) and the count still moves up by
  // the one unit that was added; only a clear, an undo or a unit too big for the ring (lost_units) lowers it. `merge` is merge_key(): the key of the unit committed last, which stays up until the next unit, an undo or a clear
  // (it says a click group MAY be joined, not that one was). A reopened click group leaves the depth and `added` as they were and changes the keyed parameter, so a merge is taken for one only when the value of that
  // parameter moved: `keyed_key` is the key whose value `keyed` holds (the ring's own merge key, or the one of the previous snapshot when the unit is gone), `keyed_ok` that it could be read.
  // The check is per parameter, not per document: a silent change of something else passes when the keyed parameter also moves in the same step, and so does a silent change of the keyed parameter itself made by another path.
  unsigned ring[kRings] = {0, 0, 0};
  unsigned added[kRings] = {0, 0, 0};
  unsigned merge[kRings] = {0, 0, 0};
  unsigned keyed_key[kRings] = {0, 0, 0};
  float keyed[kRings] = {0.0f, 0.0f, 0.0f};
  bool keyed_ok[kRings] = {false, false, false};
  // The classes of rule 1 (the undo policy, maintainer 2026-10-22): `pattern_hash` is the pattern data and the song (the patterns' generations, the song's, the order), `inst_*` each instrument slot (present, and a hash of
  // the struct, which holds its parameters), `pcm_*` the audio of each sample slot (frames, and a hash of them), `project_params` the project-level parameters (tempo, swing, default length, active tracks, the track types, the
  // note, mute-mode and scene state; NOT the Mixer's own: track volume, pan, sends, mute, solo, bus and the master, D10 of OS-UNDOGRP). What a step adds to an EMPTY slot, and every change outside these (the Mixer, the
  // settings), is not one of them.
  std::uint64_t pattern_hash = 0;
  std::uint64_t project_params = 0;
  bool inst_present[kInstSlots] = {};
  std::uint64_t inst_hash[kInstSlots] = {};
  unsigned pcm_frames[kPcmSlots] = {};
  std::uint64_t pcm_hash[kPcmSlots] = {};
  unsigned sedit = 0;  // the sample editor's own history (SampleBank::undo_depth of the slot it holds)
  bool sedit_full = false;  // that history holds SampleBank::kHistory steps: a new edit evicts the oldest and the depth does not move (the bank keeps no count of evictions: a change confined to that slot is taken for the edit, see unit_recorded)
  int sedit_slot = -1;      // the slot that history belongs to (SampleBank::undo_depth is non-zero for one slot only), -1 when it is empty
  std::string keys;    // `keys=` of the state line: a prompt is in force and these keys answer it ("" none)
  std::string overlay;
  std::string screen;
  std::string msg;
  unsigned posts = 0;
  // Rule 2: the state line as fields (`screen`, `page`, `mode`, `overlay`, `row`, `col`, `playing`, `seq`, `tempo`, `pads`, `sel`, `keys` ...), the pattern selection (Selection of pattern_edit.h) as text, and how many
  // parameter changes the audio side has heard (a sound change).
  std::map<std::string, std::string> fields;
  std::string selection;
  unsigned heard = 0;
  // What the key row and the fields do not show: the picture (a hash of the rasteriser's frame: the Wave zoom and points, the Record and Perform views, a highlight) and the notes the voice pool started (a preview, a pad, the
  // sequencer: the sound side, as `heard` is the parameters). Both are deterministic in the simulator, so a control run that waited as long has the same ones.
  std::uint64_t frame = 0;
  unsigned note_ons = 0;
  unsigned total() const { return ring[0] + ring[1] + ring[2] + sedit; }
};

struct Finding {
  std::string rule;    // "R1"
  std::string reason;  // a fixed phrase per cause: it is part of the baseline's line
};

// The value of `key=` in a state line ("" when absent).
std::string state_field(const std::string& line, const char* key);

// `prev`: the snapshot taken before the step now being looked at, when there is one (a walk): the merge key it held is read again in this state even if the unit is gone, so that a click group that returned to
// where it began is still seen as the merge it was.
Snap snap_of(o3os::sim::Simulator& s, const Snap* prev = nullptr);

// A QUESTION is a prompt whose answer is the next key and that is not a dialog (the dialogs own the key row and are forms, not questions).
bool question_up(const Snap& s);

// The findings of the rules for one action: `action` is the registry name of what ran (or, for an answer key pressed on a question, the name of the action that asked it). `answer` says that the step was a press
// of an answer key of the question that was up in `before`: that question counts as asked first. Any other action that runs while a question is up gets no such credit.
std::vector<Finding> check_rules(const Snap& before, const Snap& after, const std::string& action, bool answer = false);

// What a key press is judged by (rule 2): whether the key resolves to an action in the chain that is live (`bound`), whether it is one of F1..F8 (`fkey`: only those have a key-row label) and whether the label the
// key row shows for it (plain, or the Shift layer when Shift is part of the key) is a word and not "-" (`labelled`).
struct KeyFacts {
  bool bound = false;
  bool fkey = false;
  bool labelled = false;
  bool row_known = true;  // the labels of the state could be read (the picture's key row, or the dialog's own list): false, and no judgement about labels is made
};

// Whether the two snapshots differ in anything the user can see or hear except the notification line: the document, the view fields, the selection, the sound (the parameters heard, the notes started), and the
// picture (`picture`: false when the key said something, since the notification strip is in the picture then and cannot be told from the key's other doing). `drifting`: the sequencer is playing, and the
// fields that the playhead moves by itself (the follow cursor's row, the playing step, pattern and order), the picture and the notes are left out, since there is no control run to tell them from the key's doing.
bool differs(const Snap& a, const Snap& b, bool drifting, bool picture = true);

// What check_key calls a change: differs(), with the picture read only when the key said nothing (see differs).
bool key_acted(const Snap& ref, const Snap& after, bool drifting = false);

// RULE 2 for one key: `ref` is what the state would be without the key (a control run that waited as long, or, in a walk, the snapshot before the step with `drifting` set while the transport plays) and `after` the
// state after the key. The notification line is not a change, it is what the key said.
std::vector<Finding> check_key(const KeyFacts& key, const Snap& ref, const Snap& after, bool drifting = false);

// Runs the simulator until nothing moves for 60 ms (the document's generation, the undo depths, the notifications, the screen, the sample frames), or `limit_ms` is up: an action's job (a sample load, a scan)
// has finished when the snapshot is taken after this.
void settle(o3os::sim::Simulator& s, unsigned limit_ms = 600);

}  // namespace qanet
