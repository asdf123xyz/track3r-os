// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-QANET: what the walks of part A share. The card every run starts from, the lines of a baseline or allow file (rule | subject | context | reason | ID) and their comparison with what a run found, the scenario
// runner for a block of text, and the context chain as the App builds it. Test tooling only.
#pragma once

#include <o3os/app/bindings.h>
#include <o3os/core/binding.h>
#include <o3os/platform/host/dir_card.h>
#include <o3os/sim/simulator.h>

#include <set>
#include <string>
#include <vector>

namespace qanet {

// The card every run starts from: the generated fixture of the journeys (Projects/Small Song) with two WAVs at the root and a folder with one more. A fresh scratch copy of it is the simulator's card.
struct Card {
  o3os::platform::host::ScratchDir dir{"qanet"};
  bool ok = false;
  Card();
};

// One line of a baseline or an allow file: the finding it names and the ID of the backlog row (or decision) that owns it, `new` for a finding nobody has triaged.
struct Line {
  std::string rule, action, context, reason, id;
  std::string key() const { return rule + " | " + action + " | " + context + " | " + reason; }
  std::string text() const { return key() + " | " + id; }
  // A line of an allow file may be a pattern: `*` in the action, context and reason columns stands for any run of characters (case does not matter: a screen is `Config` in one state and `CONFIG_page_2` in another).
  // The rule column is exact. A line with no `*` matches only its own key.
  bool matches(const Line& found) const;
};

// `*` matches any run of characters, the rest must be equal, ignoring case.
bool glob_match(const std::string& pattern, const std::string& text);
bool is_pattern(const Line& l);  // a `*` in the action, context or reason column

std::string trim(const std::string& s);

// Lines of `text`: blank lines and `#` comments are skipped. false: `error` names the first line that is not "rule | action | context | reason | id".
bool parse_lines(const std::string& text, std::vector<Line>& out, std::string& error);
bool read_file(const char* path, std::string& text);

struct Verdict {
  std::vector<std::string> unknown;  // findings that the baseline and the allow list do not hold
  std::vector<std::string> stale;    // baseline lines that did not occur
  std::vector<std::string> bad;      // a line of a file that does not parse, or one that is twice in a file, or an allow line marked `new`
  std::vector<std::string> pattern_hits;  // the pattern lines (a `*` in them) that matched a finding of this run: a run that sees only some of the states cannot say that one did not
};

// The findings of a run against the two files. `found` are the keys of this run's findings. `walked` (a run that sees only some of the states: a shard): the contexts it walked. A line with no `*` is judged stale only
// when its state was walked; a pattern line is not judged at all (a shard may hold none of the states where it occurs: the pattern_hits of all the shards say, see keys_test.cpp); null: every line is judged.
Verdict compare(const std::vector<Line>& found, const std::vector<Line>& baseline, const std::vector<Line>& allow, const std::set<std::string>* walked = nullptr);

std::string join(const std::vector<std::string>& v, const char* sep = "\n  ");

// Runs the lines of `text` (the scenario grammar of sim/scenario.h) on `s`; false with `error` when one does not parse or fails.
bool run_text(o3os::sim::Simulator& s, const std::string& text, std::string& error);

// Whether `c` is in the context chain of the simulator's App now (see rig.cpp).
bool context_live(const o3os::sim::Simulator& s, o3os::app::Context c);

// The chain App::context_chain() builds in the state the simulator is in, rebuilt from the public parts (rig.cpp has the rules and points at app.cpp).
o3os::core::ContextChain live_chain(const o3os::sim::Simulator& s);

}  // namespace qanet
