// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The checks behind the scenario commands `assert state`, `assert model`, `assert file` and `mark model` (OS-E2E-1a, docs/specs/OS-E2E.md). Read only: nothing here changes the
// simulator, the app, the project or the card, and nothing ticks the clock, so a scenario with assertions in it draws the same pictures as one without. Each check returns "" when it holds
// and, when it does not, the reason in words (the runner puts the scenario line in front: "line 12: assertion failed: assert state screen=SONG: ...").
//
//   assert state <key>=<value> ...   the line the unit prints for `diag state` (app/state_line.h); every pair must hold. <value> is the exact text, or `~<regex>`: a POSIX extended regular
//                                    expression searched in the value (platform/mk66/tools/o3hil.py: `state`, its "~" rule). A key the line lacks reads as "" (the regex `~^$` matches it).
//   assert model <path>=<value>      one value of the open document (a `document` is needed). A whole-number read (tempo, order, a step's bytes, a MIDI channel, an Int or Enum parameter) is compared exactly (13001 is not 13000, and the number is
//                                    written as the read prints it: 13000, not 13000.0); a float parameter as a number (1e-4 of tolerance, relative above 1); a text exactly. Paths:
//                                      tempo                    BPM x 100, as `diag state` has it (12800 is 128 BPM)
//                                      name                     the project's title
//                                      order.<n>                the pattern at song position n (0-based; 0 past the end); order.len the song's length
//                                      pat.<p>.<t>.<s>.<field>  pattern p (1..255), track t (0..15), step s (0..127), both 0-based; field is note, instr, vol (the step's bytes, model/step.h)
//                                                               or fx1 / fx2 (as "<type>:<value>", both bytes)
//                                      inst.<n>.name            instrument n (1..64, as the UI numbers them)
//                                      inst.<n>.<param>         a parameter of the instrument, by the caption of its row in lower case ("volume", "cutoff", "pb_range"); the envelope
//                                                               and modulation rows share captions, so they are env<e>.<caption> and mod<e>.<caption> (e 0..5)
//   mark model <label>               remembers the document's digest: the SHA-256 of the native project file as formats/native writes it, in a hashing sink (nothing is written anywhere), then the model's fields one by one (QANET B5: a field the writer drops is the same bytes before and after a save, not the same value)
//   assert model digest=@<label>     the document's digest now is the one marked (digest=<64 hex digits> compares with a given one)
//   assert file <path> <test>        a file of the simulated card (E2E-1b; without a card it fails with "no card"). <path> is relative to the card's root; in quotes when it holds a
//                                    space. <test>: exists, absent, size=<n>, size>n, size<n, sha=<64 hex digits> (SHA-256 of the whole file)

#include <o3os/hal/storage.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <string>
#include <vector>

namespace o3os::sim {

// ---- assert state ------------------------------------------------------------------------------------------------------------------------

struct StatePair {
  std::string key;
  std::string value;  // exact text, or "~" and a regular expression
};

// "key=value" words into pairs. false: `error` says which word is no pair, has an empty key, or holds a regular expression that is empty (`k=~`) or does not compile.
bool parse_state_pairs(const std::vector<std::string>& words, std::vector<StatePair>& out, std::string& error);
// "" when the state line holds every pair, else the first that does not ("screen is 'PATTERN', wanted 'SONG'").
std::string state_problem(const std::string& line, const std::vector<StatePair>& pairs);

// ---- assert model ------------------------------------------------------------------------------------------------------------------------

// The words after `assert model`: exactly one "<path>=<value>", both parts not empty. false: it is not.
bool split_model_args(const std::vector<std::string>& words, std::string& path, std::string& value);
// 64 hexadecimal digits: a SHA-256 as `digest=` and `sha=` take it.
bool is_sha256_hex(const std::string& text);
// Whether `path` is one this grammar has (not whether the open document has what it names). `error` says what is wrong.
bool valid_model_path(const std::string& path, std::string& error);
// "" when the document's value at `path` is `want`, else why not (the path names nothing in this document, or the value is another).
std::string model_problem(const model::Project& project, const std::string& path, const std::string& want);
// The value at `path` as text; false (with `why`) when the document has no such thing. Whole numbers print in full and the rest with %g, a step's effect as "<type>:<value>".
bool model_read(const model::Project& project, const std::string& path, std::string& text, std::string& why);
// The captions that `inst.<n>.<param>` accepts, in the order of the tables (the test of this file holds them unique).
const std::vector<std::string>& instrument_param_captions();

// Lower-case hex SHA-256 of the project as formats/native writes it and then of its fields one by one (names to their NUL, no padding); "" when the writer fails.
std::string model_digest(const model::Project& project);

// ---- assert file -------------------------------------------------------------------------------------------------------------------------

struct FileWant {
  enum class Test : std::uint8_t { Exists, Absent, SizeEq, SizeGt, SizeLt, Sha };
  Test test = Test::Exists;
  std::uint64_t size = 0;  // SizeEq, SizeGt, SizeLt
  std::string sha;         // Sha: 64 lower-case hex digits
};

// The words after `assert file`: a path (one word, or one quoted word) and a test. false: `error` says what is wrong.
bool parse_file_want(const std::vector<std::string>& words, std::string& path, FileWant& want, std::string& error);
// "" when the card's file `path` is as wanted, else why not. A null card is "no card".
std::string file_problem(hal::storage::Storage* card, const std::string& path, const FileWant& want);

}  // namespace o3os::sim
