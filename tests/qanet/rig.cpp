// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-QANET: the shared rig of rig.h.
#include "rig.h"

#include "rules.h"

#include <o3os/app/model.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/screens/screen.h>
#include <o3os/app/state_line.h>
#include <o3os/app/view_state.h>
#include <o3os/sim/scenario.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

using namespace o3os;

namespace qanet {

namespace {

void put16(std::vector<std::uint8_t>& v, unsigned x) {
  v.push_back(static_cast<std::uint8_t>(x));
  v.push_back(static_cast<std::uint8_t>(x >> 8));
}
void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
  put16(v, x & 0xFFFFu);
  put16(v, x >> 16);
}
void tag4(std::vector<std::uint8_t>& v, const char* t) {
  for (int i = 0; i < 4; ++i) {
    v.push_back(static_cast<std::uint8_t>(t[i]));
  }
}
// A 16-bit mono WAV of `frames` frames of a saw wave at 22,050 Hz.
std::vector<std::uint8_t> wav_bytes(std::uint32_t frames, unsigned step) {
  std::vector<std::uint8_t> v;
  tag4(v, "RIFF");
  put32(v, 36 + frames * 2);
  tag4(v, "WAVE");
  tag4(v, "fmt ");
  put32(v, 16);
  put16(v, 1);
  put16(v, 1);
  put32(v, 22050);
  put32(v, 22050 * 2);
  put16(v, 2);
  put16(v, 16);
  tag4(v, "data");
  put32(v, frames * 2);
  for (std::uint32_t i = 0; i < frames; ++i) {
    put16(v, (i * step) & 0x7FFFu);
  }
  return v;
}

}  // namespace

Card::Card() {

    std::string error;
    if (!dir.ok() || !dir.copy_from(O3_E2E_CARD_DIR, error)) {
      return;
    }
    const std::string root = dir.path();
    auto put = [&](const std::string& rel, const std::vector<std::uint8_t>& bytes) {
      std::ofstream f(root + "/" + rel, std::ios::binary);
      f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      return f.good();
    };
    std::string mk = "mkdir -p '" + root + "/Samples'";
    if (std::system(mk.c_str()) != 0) {
      return;
    }
    ok = put("KICK.WAV", wav_bytes(3000, 37)) && put("SNARE.WAV", wav_bytes(2000, 91)) && put("Samples/HAT.WAV", wav_bytes(1500, 211));
}

std::string trim(const std::string& s) {
  const std::size_t a = s.find_first_not_of(" \t\r");
  if (a == std::string::npos) {
    return std::string();
  }
  return s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
}

// Lines of `text`: blank lines and `#` comments are skipped. false: `error` names the first line that is not "rule | action | context | reason | id".
bool parse_lines(const std::string& text, std::vector<Line>& out, std::string& error) {
  std::istringstream in(text);
  std::string raw;
  int n = 0;
  while (std::getline(in, raw)) {
    ++n;
    const std::string t = trim(raw);
    if (t.empty() || t[0] == '#') {
      continue;
    }
    std::vector<std::string> parts;
    std::size_t from = 0;
    while (true) {
      const std::size_t bar = t.find(" | ", from);
      if (bar == std::string::npos) {
        parts.push_back(trim(t.substr(from)));
        break;
      }
      parts.push_back(trim(t.substr(from, bar - from)));
      from = bar + 3;
    }
    if (parts.size() != 5 || parts[0].empty() || parts[1].empty() || parts[2].empty() || parts[3].empty() || parts[4].empty()) {
      error = "line " + std::to_string(n) + ": not 'rule | action | context | reason | id': " + t;
      return false;
    }
    out.push_back(Line{parts[0], parts[1], parts[2], parts[3], parts[4]});
  }
  return true;
}

bool read_file(const char* path, std::string& text) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  text = ss.str();
  return true;
}


bool glob_match(const std::string& pattern, const std::string& text) {
  auto low = [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); };
  std::size_t p = 0, t = 0, star = std::string::npos, back = 0;
  while (t < text.size()) {
    if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      back = t;
    } else if (p < pattern.size() && low(pattern[p]) == low(text[t])) {
      ++p;
      ++t;
    } else if (star != std::string::npos) {
      p = star + 1;
      t = ++back;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') {
    ++p;
  }
  return p == pattern.size();
}

bool is_pattern(const Line& l) { return l.action.find('*') != std::string::npos || l.context.find('*') != std::string::npos || l.reason.find('*') != std::string::npos; }

bool Line::matches(const Line& found) const {
  return rule == found.rule && glob_match(action, found.action) && glob_match(context, found.context) && glob_match(reason, found.reason);
}

Verdict compare(const std::vector<Line>& found, const std::vector<Line>& baseline, const std::vector<Line>& allow, const std::set<std::string>* walked) {
  Verdict v;
  std::set<std::string> known, allowed;
  std::vector<bool> allow_hit(allow.size(), false), base_hit(baseline.size(), false);
  for (const Line& l : baseline) {
    if (!known.insert(l.key()).second) {
      v.bad.push_back("twice in the baseline: " + l.key());
    }
  }
  for (const Line& l : allow) {
    if (!allowed.insert(l.key()).second) {
      v.bad.push_back("twice in the allow list: " + l.key());
    }
    if (l.id == "new") {
      v.bad.push_back("an allow-list line needs the decision that allows it (an ADR or a backlog ID), not `new`: " + l.key());  // `new` is the baseline's mark for a finding nobody has triaged
    }
  }
  for (const Line& l : found) {
    bool excused = false;
    for (std::size_t i = 0; i < allow.size(); ++i) {
      if (allow[i].matches(l)) {
        allow_hit[i] = true;
        excused = true;
      }
    }
    for (std::size_t i = 0; i < baseline.size(); ++i) {
      if (baseline[i].matches(l)) {
        base_hit[i] = true;
        excused = true;
      }
    }
    if (!excused) {
      v.unknown.push_back(l.key() + " | new");
    }
  }
  auto judged = [&](const Line& l) {
    if (walked == nullptr) {
      return true;
    }
    if (is_pattern(l)) {
      return false;
    }
    return walked->count(l.context) != 0;
  };
  for (std::size_t i = 0; i < baseline.size(); ++i) {
    if (base_hit[i] && is_pattern(baseline[i])) {
      v.pattern_hits.push_back(baseline[i].text());
    }
  }
  for (std::size_t i = 0; i < allow.size(); ++i) {
    if (allow_hit[i] && is_pattern(allow[i])) {
      v.pattern_hits.push_back(allow[i].text());
    }
  }
  for (std::size_t i = 0; i < baseline.size(); ++i) {
    if (!base_hit[i] && judged(baseline[i])) {
      v.stale.push_back(baseline[i].text());
    }
  }
  for (std::size_t i = 0; i < allow.size(); ++i) {
    if (!allow_hit[i] && judged(allow[i])) {
      v.stale.push_back("(allow list) " + allow[i].text());
    }
  }
  return v;
}

std::string join(const std::vector<std::string>& v, const char* sep) {
  std::string out;
  for (const std::string& s : v) {
    out += sep + s;
  }
  return out;
}

bool run_text(sim::Simulator& s, const std::string& text, std::string& error) {
  if (text.empty()) {
    return true;
  }
  std::vector<sim::ScenarioCommand> cmds;
  if (!sim::parse_scenario(text, cmds, error)) {
    return false;
  }
  return sim::run_scenario(cmds, s, [](const std::string&, const sim::Simulator&) { return true; }, error);
}

// Whether `c` is in the context chain now: the chain App::context_chain() builds (app/src/app.cpp; keep the two in step), rebuilt from the same public parts (the modal overlay, the pad layers the state line
// reports, the screen's mode and field, the screen). A reach that does not bring the simulator to its context fails the run with this, instead of trying the action in the wrong place.
//   * a modal overlay captures: nothing below it is in reach, and the pad layers are in force only with no overlay up (App::pads_tracks_in_force / pads_mode_in_force);
//   * PadsTracks is the layer of Pattern, Perform and Song (the `pads=` field names the mode on every screen, so the screen is checked here); PadsMode is Pattern's;
//   * the screen's context may capture (Screen::captures): the walk stops there and Global is out of reach.
bool context_live(const sim::Simulator& s, app::Context c) {
  const app::ViewState& st = s.app().state();
  if (const app::Overlay* o = app::overlay_of(st.overlay); o != nullptr && o->modal()) {
    return o->context() == c;
  }
  char line[768];
  app::describe_state(s.app(), line, sizeof line);
  const std::string pads = state_field(line, "pads");
  const bool no_overlay = st.overlay == app::OverlayId::None;
  if (c == app::Context::PadsTracks) {
    return no_overlay && (st.screen == app::ScreenId::Pattern || st.screen == app::ScreenId::Perform || st.screen == app::ScreenId::Song) && pads == "TRACKS";
  }
  if (c == app::Context::PadsMode) {
    return no_overlay && st.screen == app::ScreenId::Pattern && (pads == "PIANO" || pads == "ISO" || pads == "STEP" || pads == "PATS" || pads == "VU" || pads == "FADERS");  // (FADERS: VU's second page, PadLayer::name(); its pads write the track volumes, OS-BUG-R3SEL)
  }
  const app::ScreenTable table = app::screen_registry();
  for (std::size_t i = 0; i < table.count; ++i) {
    const app::ScreenEntry& e = table.items[i];
    if (e.screen == nullptr || e.id != st.screen) {
      continue;
    }
    if (c == app::Context::Global) {
      return !e.screen->captures();
    }
    const app::ModeTable modes = e.screen->modes();
    if (modes.count != 0) {
      const unsigned mode = st.mode[app::screen_index(st.screen)];
      if (modes.items[mode < modes.count ? mode : 0u].context == c) {
        return true;
      }
    }
    return e.screen->field_context(s.app().model(), st) == c || e.screen->context() == c;
  }
  return c == app::Context::Global;
}

// The chain, innermost first, as App::context_chain(true) pushes it (app/src/app.cpp; keep the two in step): the modal overlay (which captures), the pad layer in force, the screen's mode, the field under the cursor,
// the screen (which may capture) and Global. context_live() above asks the same questions of one context.
core::ContextChain live_chain(const sim::Simulator& s) {
  core::ContextChain chain;
  const app::ViewState& st = s.app().state();
  if (const app::Overlay* o = app::overlay_of(st.overlay); o != nullptr && o->modal()) {
    (void)chain.push(app::context_id(o->context()), true);
  }
  for (app::Context c : {app::Context::PadsTracks, app::Context::PadsMode}) {
    if (st.overlay == app::OverlayId::None && context_live(s, c)) {
      (void)chain.push(app::context_id(c));
      break;  // one pad layer at most: the tracks layer is the innermost
    }
  }
  const app::ScreenTable table = app::screen_registry();
  if (const app::ScreenEntry* e = app::find_screen(table, st.screen); e != nullptr && e->screen != nullptr) {
    const app::Screen& screen = *e->screen;
    const app::ModeTable modes = screen.modes();
    if (modes.count != 0) {
      const unsigned mode = st.mode[app::screen_index(st.screen)];
      (void)chain.push(app::context_id(modes.items[mode < modes.count ? mode : 0u].context));
    }
    const app::Context field = screen.field_context(s.app().model(), st);
    if (field != app::Context::None) {
      (void)chain.push(app::context_id(field));
    }
    (void)chain.push(app::context_id(screen.context()), screen.captures());
  }
  (void)chain.push(app::context_id(app::Context::Global));
  return chain;
}

}  // namespace qanet
