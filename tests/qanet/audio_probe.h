// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-QANET C4 (docs/specs/OS-QANET.md part C; new Community OS behaviour, test tooling only): the audio probe, and the measurements that the audio matrix (C1 to C3, audio_matrix_test.cpp) and the random user
// (B, random_user.cpp) share. The matrix plays one note from a pad and from a Pattern step and compares them; the probe does the same inside the random walk, after a walk step that rebinds an
// instrument's descriptor (ADD, EVEN, AUTO, slice delete, play mode, load, undo), with the tolerances of C1: pitch within 5 cents and onset within 2 ms (sim/include/o3os/sim/audio_check.h's `pitch=` and `onset=`).
#pragma once

#include <o3os/model/project.h>
#include <o3os/sim/audio_check.h>
#include <o3os/sim/simulator.h>

#include <cstdint>
#include <string>
#include <vector>

namespace o3os::qanet_audio {

constexpr double kPitchCents = 5.0;  // audio_check.h: what `pitch=<ref>±5c` allows
constexpr double kOnsetMs = 2.0;     // `onset=<ref>±2`

// ---- the measurements (moved from the matrix: one definition) ----------------------------------------------------------------------------

double cents(double hz, double ref);

constexpr double kZcCents = 25.0;  // how far the pitch reading (YIN) may lie from the zero-crossing rate before a pitch difference is not reported (OS-BUG-MODEPITCH: YIN read -44 c on a short harmonic-rich note, the crossings said 16 c at most)

struct Measure {
  double hz = 0.0;     // 0: no periodic part
  double onset = -1.0;  // ms from the start of the render; -1: never starts
  double zc_hz = 0.0;  // the rising zero-crossing rate of the loud stretch after the onset (an independent reading); 0: fewer than three crossings
  // The reading is confirmed when the crossing rate is within kZcCents of it (or there is no rate to check it by).
  bool confirmed() const;
};

// The pitch of a render: the pitch most 100 ms windows agree on, from the onset on (see audio_probe.cpp: the limit, and why a majority); `spread_cents` gets the windows' disagreement.
double steady_pitch(const sim::Signal& s, double onset, double* spread_cents = nullptr);
Measure measure(const sim::Pcm& pcm, double* spread_cents = nullptr);
// The tone track of a render of the two-pitch sample qaloop, per 20 ms bin (see audio_probe.cpp).
std::vector<int> tone_track(const sim::Pcm& pcm);

struct Finding {
  std::string cell;
  std::string paths;  // "<path> vs step"
  std::string what;   // pitch | onset | silent | nopitch
  std::string diff;   // the measured difference, for the file and the log
  double value = 0.0;
};

// One path against the reference (the Pattern step's render). Empty: they agree. `onset_matters` false: the onset is only reported. `tracks`: compare the tone tracks instead of one pitch.
std::vector<Finding> compare(const std::string& cell, const std::string& path, const sim::Pcm& got, const sim::Pcm& ref, bool onset_matters = true, bool tracks = false);
// `p` played `by` cents higher (a linear-interpolating resampler: a path with a planted detune).
sim::Pcm detuned(const sim::Pcm& p, double by);

// ---- the recipes: the keys of the Wave screen's edits (moved from the matrix, shared with the probe's edit session) --------------------------------

// The play mode list (Shift + F1): the cursor starts on the instrument's own, so go to the top and down `mode` rows. `settle` false: the F1 that makes the change is the last key.
std::string pick_mode(unsigned mode, bool settle = true);
// The CHOP page of the Wave screen from a Pattern screen (the screen's own button twice); EVEN of two slices; ADD; AUTO (a job: its answer is waited for by the caller).
extern const std::string kChop;
extern const std::string kEven;
extern const std::string kAdd;
extern const std::string kAuto;

// ---- the probe (C4) ------------------------------------------------------------------------------------------------------------------------

// What the probe watches in the walk: per instrument, what the sample bank's descriptor is made from (the sample's path, the play mode, the slices, the loop and the window). A change is a rebind to come.
struct InstWatch {
  struct One {
    std::uint64_t path = 0;
    std::uint64_t slices = 0;
    std::uint64_t loop = 0;
    std::uint8_t mode = 0;
    std::uint8_t count = 0;
    bool seen = false;
  };
  One one[model::kMaxInstruments];
};
// Compares the project's instruments with the last call (the first call only learns them); returns the 1-based number of the first that changed (0: none) and what changed in `what`:
// `load` (the path), `load_over_sliced` (the path, and the instrument was a chop), `mode`, `slices`, `loop`. `undo`: the step that made the change was an undo, which names it `undo` whatever changed.
unsigned watch_instruments(InstWatch& w, const model::Project& p, bool undo, std::string* what);

struct ProbeOptions {
  double plant_detune_cents = 0.0;  // self-test: the settled pad's render is detuned by this much (a pad path with a fault that stays) ...
  std::string plant_label;          // ... when the edit is the one of this label (`undo`); empty: never
  std::string session_edits;        // self-test: the edit session runs exactly these edits (a comma list: even,add,auto,del,undo,reload,mode<n>); empty: drawn from the seed
  unsigned session_edits_max = 3;   // the longest session (the load is probed too)
  bool session = true;              // run the session at a probe point that asks for one
};

struct ProbeReport {
  std::vector<Finding> findings;  // cell = the edit's label; paths = `pad:fresh` (the pad straight after the edit) or `pad:settled` (after the sound has settled); what and diff as compare()
  std::vector<std::string> log;   // one line a probe
  unsigned probes = 0;            // probes played
  unsigned stepped = 0;           // of those, with a Pattern step as the reference
  unsigned unstepped = 0;         // the reference was the settled pad (the pattern would not clear, a track is muted, the step was silent)
  unsigned skipped = 0;           // not played (no audio in the slot, not on a Pattern screen, the voices would not go quiet)
  unsigned diverged = 0;          // the replay did not end in the walk's document
  std::string error;              // a setup that could not run (not a finding)
};

// What the walk hands over at a probe point.
struct WalkProbe {
  std::string card_dir;              // the folder of the card as the run opened it
  std::vector<std::string> lines;    // the walk's inputs so far, one scenario line each
  unsigned instrument = 0;           // the instrument, 1-based: probed now when `probe_now`, and the one the edit session works on
  bool probe_now = true;             // probe `instrument` as it is (a rebind the walk made); false: a session point, which only runs the session
  std::string label;                 // what the step did to it (watch_instruments' `what`)
  std::string digest;                // the walk's document digest now: the replay has to end in the same document
  std::uint64_t seed = 0;            // seeds the edit session
  bool session = false;              // go on with an edit session on a sample of the probe's own
};

// The probe point: a Simulator that replays the walk's inputs with the real engine on (`audio on`; a fork: the walk itself is not touched), then plays the instrument the way the matrix does: the pad straight
// after the edit, the pad after the sound has settled, and a Pattern step, compared with the C1 tolerances (the step is the reference; when it cannot be written the settled pad is). Then, when asked, an edit session: a
// tone is put on the card, loaded into the instrument, and a few edits of the Wave screen (EVEN, ADD, AUTO, slice delete, a play mode, undo, load over the chop) are made and probed one by one.
ProbeReport probe_walk_state(const WalkProbe& in, const ProbeOptions& opt);

// Can `instrument` be probed in this state? (it has a sample, the transport is stopped, no track is muted or soloed); `why` says what not.
bool probe_possible(const sim::Simulator& s, unsigned instrument, std::string* why = nullptr);

// The same on a Simulator that is already there (the self-tests, the session): `instrument` 1-based. Plays and compares; `label` names the edit in the findings.
void probe_instrument(sim::Simulator& s, unsigned instrument, const std::string& label, const ProbeOptions& opt, ProbeReport& report);

}  // namespace o3os::qanet_audio
