// SPDX-License-Identifier: GPL-3.0-or-later
//
// o3sim: runs a scenario headless on a fixed tick and writes PNGs at its `snap` lines.
//
//   o3sim --scenario <file> [--out <dir>] [--goldens <file> (--check | --update-goldens)] [--twice]
//         [--display-words <n>] [--theme <name>] [--cards <dir>] [--wav <dir>] [--card-rate <bytes-per-ms>] [--card-latency <us>] [--card-write-rate <bytes-per-ms>]
//         [--fingerprints <dir> (--check-fingerprints | --update-fingerprints) [--fingerprint-diff <file>]]
//   o3sim --gallery <dir> [--goldens <file> (--check | --update-goldens)] [--twice] [--theme <name>]
//         [--only <prefix>] [--shard <i>/<n>] [--list]
//   o3sim --legibility [--sheet <file.png>] [--theme <name>] [--only <prefix>] [--shard <i>/<n>]
//
//   --out <dir>        write <dir>/<snap>.png for every snap (the directory is created)
//   --goldens <file>   SHA-256 of each framebuffer, one "<scenario>/<snap> <hex>" per line
//   --check            compare with the goldens; a missing, different or stale entry fails
//   --update-goldens   replace this scenario's entries in the goldens file (review the PNGs afterwards)
//   --twice            run the scenario a second time on a fresh simulator and require identical hashes (the pictures' SHA-256, and the SHA-256 of every `render` of the audio: the same samples twice)
//   --wav <dir>        write every `render` of the scenario as <dir>/<scenario>_<name>.wav (16-bit stereo at the engine's rate; the stems renders' parts too), for listening. The directory is created. The
//                      run also prints one line a render ("audio <scenario>/<name> <sha256> frames=<n>": the samples' hash, for comparing two builds, never a golden) and one a passed `assert audio`
//   --fingerprints <dir>   (OS-E2E-4) the folder of the audio fingerprints, tests/audio_fingerprints/ (sim/audio_fingerprint.h): <dir>/<scenario>.json holds the description of every render that the
//                      scenario's `fingerprint` lines name. With --twice the two runs must make the same text
//   --check-fingerprints   compare each with the stored one (the rules at the top of audio_fingerprint.h); a missing file, a missing or surplus render, a file for a scenario that fingerprints
//                      nothing, or a change beyond the rules is exit 1 and says what moved
//   --update-fingerprints  rewrite <dir>/<scenario>.json (tools/update_audio_fingerprints.py runs this for every scenario that has a `fingerprint` line, and the WAVs with --wav)
//   --fingerprint-diff <file>  (with --update-fingerprints) write what moved against the file as it was, as markdown
//   --display-words <n>  size of the display list the display accepts (default 2048, the chip's RAM_DL); a
//                      smaller one makes the frames not fit, which ends the run with exit 2 (used by the tests)
//   --cards <dir>      the folder a relative `card <fixture>` of the scenario is looked up in (default: the scenario file's own folder). The
//                      journeys' card is generated into the build directory (sim/CMakeLists.txt, target e2e_card_fixture), not kept in git
//   --card-rate <bytes-per-ms>, --card-latency <us>, --card-write-rate <bytes-per-ms>   (OS-QANET D2) the slow-card model on from the start of the run (platform/host/slow_card.h): a call of the app to the card
//                      charges latency plus bytes over the rate to the simulated clock. Any one of them turns it on; the others keep the PROVISIONAL defaults (257 us, 17700 bytes a ms read, 442 a ms write: the
//                      unit's block bench, until OS-QANET D4 measures the unit). The scenario's `card slow` line changes it from there. Without them nothing is charged
//   --theme <name>     the colour theme (amiga, black, white; ui/theme.h): a scenario starts in it (the default is amiga),
//                      the gallery renders only it (by default all three). The goldens cover the default theme's
//                      scenarios and every theme of the gallery, so --theme cannot be combined with --check or
//                      --update-goldens
//   --gallery <dir>    instead of a scenario: render every screen x state x theme of the registry (sim/screens.h) to
//                      <dir>/<screen>_<state>_<theme>.png, a contact sheet of everything <dir>/contact_sheet.png (a row
//                      per state, a column per theme) and one per theme <dir>/contact_sheet_<theme>.png (a frame in
//                      the colour of each verdict with --check) and, with --check, <dir>/diff_list.txt; print the
//                      display-list budget table. The goldens keys are "gallery/<screen>_<state>_<theme>" (so a
//                      scenario cannot be called "gallery"); diff_list.txt is removed by a run without --check. A
//                      budget breach is exit 1. (A --watch mode that regenerates the gallery on every build is a
//                      follow-up.)
//   --only <prefix>    (gallery) render only the screens whose registry name starts with <prefix> (non-empty, and it must
//                      name a screen: else exit 2). With --check the goldens of the other screens are neither required
//                      nor reported as missing; a golden that no screen of the registry produces is still stale. With
//                      --update-goldens only the entries of the rendered shots are replaced (the other screens' stay)
//   --shard <i>/<n>    (gallery) render shot i of every n (1 <= i <= n) of the sequence that --theme and --only leave: n
//                      runs, i = 1..n, together render each shot once (ctest `gallery_<i>`). --check, --update-goldens
//                      and the stale check behave as with --only. The display-list budget table (it costs about as much as a
//                      dozen pictures) is measured for the screens --only names, and with --shard by shard 1 only
//   --list             (gallery) print the golden keys that the run would render, one per line, and exit 0: nothing is
//                      drawn or written (ctest gallery_shards_cover adds up the shards with it)
//
//   --legibility      (OS-E2E-5) instead of a scenario or a gallery directory: draw the same shots as --gallery (the same --theme,
//                      --only and --shard), write no picture, and judge the glyphs, tiles and icons of every frame (sim/legibility.h): ink cut by
//                      the scissor or the screen edge, two bitmaps on one pixel, a colour pair outside the theme's contrast table. One line per
//                      finding; an allow-list entry (legibility.cpp) is printed with its reason. Exit 1 when there is a finding. ctest
//                      `legibility_<i>of<n>`, label e2e
//   --sheet <file>    (legibility) also write a contact sheet of every shot, nine thumbnails across (tools/ci.sh e2e: build-e2e/<date>/screens.png)
//
// Exit 0 ok; 1 a golden, determinism, budget or legibility check failed; 2 bad usage, unreadable or invalid scenario, or a
// frame the display could not take (display-list overflow, command outside the subset).
#include <o3os/eve/dl.h>
#include <o3os/platform/host/png.h>
#include <o3os/sim/audio_check.h>
#include <o3os/sim/audio_fingerprint.h>
#include <o3os/sim/budget.h>
#include <o3os/sim/gallery.h>
#include <o3os/sim/legibility.h>
#include <o3os/sim/scenario.h>
#include <o3os/sim/simulator.h>
#include <o3os/ui/assets.h>
#include <o3os/ui/theme.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace o3os;

namespace {

struct Options {
  fs::path scenario;
  fs::path gallery;
  fs::path out;
  fs::path goldens;
  fs::path sheet;        // --sheet: the legibility run's contact sheet
  fs::path cards;        // --cards: where a relative `card <fixture>` is looked up (default: the scenario file's folder)
  fs::path wav;          // --wav: where the renders of the scenario are written as WAV files
  fs::path fingerprints; // --fingerprints: the folder of the audio fingerprints (OS-E2E-4)
  fs::path fingerprint_diff;  // --fingerprint-diff: where --update-fingerprints writes what moved
  bool check_fingerprints = false;
  bool update_fingerprints = false;
  bool legibility = false;
  bool card_slow = false;  // --card-rate, --card-latency or --card-write-rate given (OS-QANET D2)
  std::uint32_t card_rate = platform::host::CardTiming::kDefaultReadBytesPerMs;
  std::uint32_t card_latency = platform::host::CardTiming::kDefaultLatencyUs;
  std::uint32_t card_write_rate = platform::host::CardTiming::kDefaultWriteBytesPerMs;
  bool check = false;
  bool update = false;
  bool twice = false;
  std::size_t display_words = eve::kMaxDisplayListWords;
  bool has_theme = false;
  ui::ThemeId theme = ui::ThemeId::Amiga;
  bool list = false;
  bool has_shard = false;
  bool has_only = false;
  std::string only;      // --only: the screens' name prefix
  unsigned shard = 0;    // --shard i/n: i - 1 (0-based) and n
  unsigned shards = 1;
};

struct Shot {
  std::string name;
  std::string hash;
  std::size_t dl_words;
};

int usage() {
  std::fputs(
      "usage: o3sim --scenario <file> [--out <dir>] [--goldens <file> (--check | --update-goldens)] [--twice]\n"
      "             [--display-words <n>] [--theme <name>] [--cards <dir>] [--wav <dir>]\n"
      "             [--card-rate <bytes-per-ms>] [--card-latency <us>] [--card-write-rate <bytes-per-ms>]\n"
      "             [--fingerprints <dir> (--check-fingerprints | --update-fingerprints) [--fingerprint-diff <file>]]\n"
      "       o3sim --gallery <dir> [--goldens <file> (--check | --update-goldens)] [--twice] [--theme <name>]\n"
      "                             [--only <prefix>] [--shard <i>/<n>] [--list]\n"
      "       o3sim --legibility [--sheet <file.png>] [--theme <name>] [--only <prefix>] [--shard <i>/<n>]\n",
      stderr);
  return 2;
}

bool read_file(const fs::path& path, std::string& text) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  text = ss.str();
  return true;
}

// What a pass heard (OS-E2E-2): every render by name with the SHA-256 of its samples and its length, and a line for every passed `assert audio`.
struct Heard {
  std::vector<std::pair<std::string, std::string>> digests;
  std::vector<std::size_t> frames;
  std::vector<std::string> log;
  sim::FpSet prints;  // OS-E2E-4: the fingerprints of the renders that the scenario's `fingerprint` lines name (only with --fingerprints)
};

// One pass over the scenario on a fresh simulator. Writes PNGs when `out` is non-empty, and the renders as WAV files when `wav` is.
bool run_once(const std::vector<sim::ScenarioCommand>& commands, const Options& opt, const fs::path& out,
              std::vector<Shot>& shots, std::string& error, Heard* heard = nullptr, const fs::path& wav = fs::path()) {
  auto simulator = std::make_unique<sim::Simulator>(eve::kMaxDisplayListWords, opt.display_words);  // about 14 KB of app state: keep it off the stack
  simulator->app().set_theme(opt.theme);  // before the first frame
  simulator->set_scenario_dir((opt.cards.empty() ? opt.scenario.parent_path() : opt.cards).string());  // a fixture of `card` is relative to --cards, else to the scenario file
  if (opt.card_slow) {
    std::string why;
    if (!simulator->set_card_timing(true, opt.card_rate, opt.card_latency, opt.card_write_rate, why)) {
      error = why;
      return false;
    }
  }
  shots.clear();
  const bool ok = sim::run_scenario(
      commands, *simulator,
      [&](const std::string& name, const sim::Simulator& s) {
        shots.push_back({name, sim::framebuffer_sha256(s.raster()), s.display().last_frame_words()});
        if (out.empty()) {
          return true;
        }
        const std::string file = (out / (name + ".png")).string();
        if (!platform::host::write_png_rgba(file.c_str(), s.raster().rgba(), sim::EveRaster::kWidth,
                                            sim::EveRaster::kHeight)) {
          std::fprintf(stderr, "o3sim: cannot write %s\n", file.c_str());
          return false;
        }
        return true;
      },
      error, heard != nullptr ? &heard->log : nullptr);
  if (simulator->has_audio()) {  // (a failed run writes its WAV files too: what the failing assertion heard is the first thing to listen to)
    if (ok && heard != nullptr) {
      heard->digests = simulator->render_digests();
      for (const auto& d : heard->digests) {
        heard->frames.push_back(simulator->render_result(d.first)->frames());
      }
    }
    if (ok && heard != nullptr && !opt.fingerprints.empty()) {
      for (const sim::ScenarioCommand& c : commands) {
        if (c.kind == sim::ScenarioCommand::Kind::Fingerprint) {
          heard->prints[c.name] = sim::fingerprint_render(*simulator->render_result(c.name), c.value != 0u);
        }
      }
    }
    if (!wav.empty()) {
      std::error_code ec;
      fs::create_directories(wav, ec);
      for (const auto& d : simulator->render_digests()) {
        const std::vector<std::uint8_t> bytes = sim::wav_bytes(*simulator->render_result(d.first));
        const fs::path file = wav / (opt.scenario.stem().string() + "_" + d.first + ".wav");
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (ec || !f) {
          std::fprintf(stderr, "o3sim: cannot write %s\n", file.string().c_str());
          error = "cannot write " + file.string();
          return false;
        }
      }
    }
  }
  return ok;
}

// OS-E2E-4: the audio fingerprint of this scenario against <dir>/<scenario>.json (--check-fingerprints), or the file rewritten from it (--update-fingerprints). 0 ok, 1 a check failed, 2 an
// error of the tool (a file that cannot be read or written). `fingerprinted`: the scenario has `fingerprint` lines.
int fingerprint_step(const Options& opt, const std::string& stem, const sim::FpSet& prints, bool fingerprinted) {
  const fs::path file = opt.fingerprints / (stem + ".json");
  std::error_code ec;
  const bool exists = fs::exists(file, ec);
  const char* hint = "run `cmake --build <build> --target update_audio_fingerprints` and read fingerprint_diff.md";
  if (!fingerprinted) {
    if (!exists) {
      return 0;
    }
    if (opt.update_fingerprints) {
      fs::remove(file, ec);
      std::printf("fingerprint %s: removed %s (the scenario has no `fingerprint` line)\n", stem.c_str(), file.string().c_str());
      return 0;
    }
    std::fprintf(stderr, "o3sim: fingerprint %s: %s exists but the scenario has no `fingerprint` line (a stale file): %s\n", stem.c_str(), file.string().c_str(), hint);
    return 1;
  }
  std::string text, error;
  sim::FpSet stored;
  bool have = false;
  if (exists) {
    if (!read_file(file, text)) {
      std::fprintf(stderr, "o3sim: cannot read %s\n", file.string().c_str());
      return 2;
    }
    have = sim::parse_fingerprint(text, stem, stored, error);
  }
  const sim::FpCompare cmp = sim::compare_fingerprints(stem, have ? stored : sim::FpSet(), prints);
  if (opt.check_fingerprints) {
    if (!exists || !have) {
      const std::string why = !exists ? "no " + file.string() + " (a new key)" : file.string() + " is not readable: " + error;
      std::fprintf(stderr, "o3sim: fingerprint %s: %s: %s\n", stem.c_str(), why.c_str(), hint);
      return 1;
    }
    for (const std::string& failure : cmp.failures) {
      std::fprintf(stderr, "o3sim: fingerprint %s/%s\n", stem.c_str(), failure.c_str());
    }
    if (!cmp.failures.empty()) {
      std::fprintf(stderr, "o3sim: fingerprint %s: the sound changed beyond the rules (audio_fingerprint.h); if it is meant, %s\n", stem.c_str(), hint);
      return 1;
    }
    std::printf("fingerprint %s: %zu renders %s\n", stem.c_str(), prints.size(), cmp.identical ? "identical" : "within the rules");
    return 0;
  }
  const std::string fresh = sim::fingerprint_json(stem, prints);
  if (!opt.fingerprint_diff.empty()) {
    std::ofstream d(opt.fingerprint_diff, std::ios::binary | std::ios::trunc);
    d << cmp.markdown;
    if (exists && !have) {
      d << "The stored file could not be read (" << error << "): every render above counts as new.\n\n";
    }
    if (!d) {
      std::fprintf(stderr, "o3sim: cannot write %s\n", opt.fingerprint_diff.string().c_str());
      return 2;
    }
  }
  if (exists && text == fresh) {
    std::printf("fingerprint %s: %s unchanged\n", stem.c_str(), file.string().c_str());
    return 0;
  }
  fs::create_directories(opt.fingerprints, ec);
  std::ofstream f(file, std::ios::binary | std::ios::trunc);
  f << fresh;
  if (!f) {
    std::fprintf(stderr, "o3sim: cannot write %s\n", file.string().c_str());
    return 2;
  }
  std::printf("fingerprint %s: %s %s (%zu renders)\n", stem.c_str(), file.string().c_str(), exists ? "rewritten" : "written", prints.size());
  return 0;
}

using Goldens = std::map<std::string, std::string>;

bool read_goldens(const fs::path& path, Goldens& goldens, bool must_exist) {
  std::string text;
  if (!read_file(path, text)) {
    if (must_exist) {
      std::fprintf(stderr, "o3sim: cannot read goldens file %s\n", path.string().c_str());
    }
    return !must_exist;
  }
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);) {
    std::istringstream words(line);
    std::string key, hash;
    if (!(words >> key) || key[0] == '#') {
      continue;
    }
    if (!(words >> hash) || hash.size() != 64) {
      std::fprintf(stderr, "o3sim: %s: malformed line '%s'\n", path.string().c_str(), line.c_str());
      return false;
    }
    goldens[key] = hash;
  }
  return true;
}

bool write_goldens(const fs::path& path, const Goldens& goldens) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out << "# o3sim scenario goldens: SHA-256 of each 800x480 RGBA framebuffer (3,072,000 bytes), one\n"
         "# \"<scenario>/<snap> <hex>\" per line. Regenerate with `--update-goldens` (cmake target update_goldens),\n"
         "# then look at the PNGs before committing the diff: a hash only says that something changed.\n";
  for (const auto& entry : goldens) {
    out << entry.first << ' ' << entry.second << '\n';
  }
  return static_cast<bool>(out);
}

// `--gallery`: see the header comment. Returns the exit code.
int run_gallery(const Options& opt) {
  std::error_code ec;
  if (opt.list) {
    sim::GalleryFilter selected;
    selected.theme = opt.has_theme ? &opt.theme : nullptr;
    selected.prefix = opt.only;
    selected.shard = opt.shard;
    selected.shards = opt.shards;
    for (const std::string& key : sim::gallery_keys(selected)) {
      std::printf("%s\n", key.c_str());
    }
    return 0;
  }
  fs::create_directories(opt.gallery, ec);
  if (ec) {
    std::fprintf(stderr, "o3sim: cannot create %s: %s\n", opt.gallery.string().c_str(), ec.message().c_str());
    return 2;
  }
  int status = 0;

  // diff_list.txt describes a --check run: one left from an earlier run must not be mistaken for this one's.
  if (!opt.check) {
    fs::remove(opt.gallery / "diff_list.txt", ec);
  }

  // The budget table costs about as much as a dozen pictures: a run of a part of the gallery measures its own share. --only: the
  // screens it names; --shard: the whole table in shard 1 only (the shards together measure every state once).
  if (opt.has_shard && opt.shard != 0) {
    std::printf("budget table: measured by shard 1/%u\n", opt.shards);
  } else {
    const std::vector<sim::BudgetRow> budget = sim::measure_all(opt.only);
    std::fputs(sim::format_table(budget).c_str(), stdout);
    if (!sim::all_within(budget)) {
      std::fputs("o3sim: a screen state exceeds its display-list budget\n", stderr);
      status = 1;
    }
  }

  sim::GalleryFilter filter;
  filter.theme = opt.has_theme ? &opt.theme : nullptr;
  filter.prefix = opt.only;
  filter.shard = opt.shard;
  filter.shards = opt.shards;
  const std::string prefix = "gallery/";
  // Every key of the registry: a golden outside it is stale, whatever part of the gallery this run renders.
  const std::vector<std::string> registry_keys = sim::gallery_keys();
  sim::GalleryFilter named;  // what --only names, before the shard and the theme narrow it
  named.prefix = opt.only;
  if (opt.has_only && sim::gallery_keys(named).empty()) {
    std::fprintf(stderr, "o3sim: --only '%s': no screen of the registry has a name that starts with it\n", opt.only.c_str());
    return 2;
  }

  std::vector<sim::GalleryShot> shots;
  std::string error;
  if (!sim::render_gallery(opt.gallery.string(), shots, error, filter)) {
    std::fprintf(stderr, "o3sim: gallery: %s\n", error.c_str());
    return 2;
  }
  if (opt.twice) {
    std::vector<sim::GalleryShot> again;
    if (!sim::render_gallery(std::string(), again, error, filter)) {
      std::fprintf(stderr, "o3sim: gallery, second run: %s\n", error.c_str());
      return 2;
    }
    for (std::size_t i = 0; i < shots.size(); ++i) {
      if (again.size() != shots.size() || again[i].hash != shots[i].hash) {
        std::fprintf(stderr, "o3sim: NOT DETERMINISTIC: %s differs between two runs\n", shots[i].key.c_str());
        status = 1;
      }
    }
  }

  Goldens goldens;
  if ((opt.check || opt.update) && !read_goldens(opt.goldens, goldens, opt.check)) {
    return opt.check ? 1 : 2;
  }
  std::string diff_list;
  for (sim::GalleryShot& shot : shots) {
    std::string verdict;
    if (opt.check) {
      const auto it = goldens.find(shot.key);
      const char* word;
      if (it == goldens.end()) {
        word = "NEW";
        verdict = "  NEW (no golden)";
        shot.verdict = sim::GalleryShot::Verdict::New;
        status = 1;
      } else if (it->second != shot.hash) {
        word = "CHANGED";
        verdict = "  CHANGED (golden " + it->second + ")";
        shot.verdict = sim::GalleryShot::Verdict::Changed;
        status = 1;
      } else {
        word = "ok";
        verdict = "  ok";
        shot.verdict = sim::GalleryShot::Verdict::Ok;
      }
      diff_list += std::string(word) + " " + shot.key + "\n";
    }
    std::printf("%s %s  dl=%zu/%zu%s\n", shot.key.c_str(), shot.hash.c_str(), shot.words, eve::kMaxDisplayListWords,
                verdict.c_str());
  }
  if (opt.check) {
    const std::set<std::string> produced(registry_keys.begin(), registry_keys.end());
    for (const auto& entry : goldens) {
      if (entry.first.compare(0, prefix.size(), prefix) == 0 && !produced.count(entry.first)) {
        std::fprintf(stderr, "o3sim: stale golden '%s': the gallery no longer produces it\n", entry.first.c_str());
        diff_list += "STALE " + entry.first + "\n";
        status = 1;
      }
    }
    std::ofstream list(opt.gallery / "diff_list.txt", std::ios::binary | std::ios::trunc);
    list << diff_list;
    if (!list) {
      std::fprintf(stderr, "o3sim: cannot write %s\n", (opt.gallery / "diff_list.txt").string().c_str());
      return 2;
    }
  }
  const fs::path sheet = opt.gallery / "contact_sheet.png";
  if (!sim::write_contact_sheet(sheet.string(), shots)) {
    std::fprintf(stderr, "o3sim: cannot write %s\n", sheet.string().c_str());
    return 2;
  }
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {  // and one sheet per theme: a column of the states in that theme
    const char* name = ui::theme(ui::theme_id_at(t)).name;
    std::vector<sim::GalleryShot> of_theme;
    for (const sim::GalleryShot& shot : shots) {
      if (shot.theme == name) {
        of_theme.push_back(shot);
      }
    }
    const fs::path file = opt.gallery / (std::string("contact_sheet_") + name + ".png");
    if (!of_theme.empty() && !sim::write_contact_sheet(file.string(), of_theme)) {
      std::fprintf(stderr, "o3sim: cannot write %s\n", file.string().c_str());
      return 2;
    }
  }
  std::printf("gallery: %zu images and contact_sheet*.png in %s\n", shots.size(), opt.gallery.string().c_str());
  if (opt.update) {
    // A run of the whole gallery drops every gallery entry first (a state that no longer exists leaves); a run of a part of it
    // (--only, --shard) replaces the entries it rendered and keeps the rest. (--theme cannot be combined with --update-goldens.)
    const bool partial = opt.has_only || opt.has_shard;
    for (auto it = goldens.begin(); it != goldens.end();) {
      it = (!partial && it->first.compare(0, prefix.size(), prefix) == 0) ? goldens.erase(it) : std::next(it);
    }
    for (const sim::GalleryShot& shot : shots) {
      goldens[shot.key] = shot.hash;
    }
    if (!write_goldens(opt.goldens, goldens)) {
      std::fprintf(stderr, "o3sim: cannot write %s\n", opt.goldens.string().c_str());
      return 2;
    }
  }
  return status;
}

// `--legibility`: see the header comment. Returns the exit code.
int run_legibility(const Options& opt) {
  sim::GalleryFilter filter;
  filter.theme = opt.has_theme ? &opt.theme : nullptr;
  filter.prefix = opt.only;
  filter.shard = opt.shard;
  filter.shards = opt.shards;
  sim::GalleryFilter named;  // what --only names, before the shard and the theme narrow it
  named.prefix = opt.only;
  if (opt.has_only && sim::gallery_keys(named).empty()) {
    std::fprintf(stderr, "o3sim: --only '%s': no screen of the registry has a name that starts with it\n", opt.only.c_str());
    return 2;
  }

  std::vector<sim::LegibilityFinding> findings;
  sim::GalleryObserver observer;
  observer.record_glyphs_below = ui::kRamGEnd;  // the faces, tiles and icons; the wave lanes above are not text
  observer.frame = [&findings](const sim::GalleryShot& shot, const sim::EveRaster& raster, const ui::Theme& theme) {
    for (sim::LegibilityFinding& f : sim::check_legibility(raster, theme, shot.key)) {
      findings.push_back(std::move(f));
    }
  };
  std::vector<sim::GalleryShot> shots;
  std::string error;
  if (!sim::render_gallery(std::string(), shots, error, filter, &observer)) {
    std::fprintf(stderr, "o3sim: legibility: %s\n", error.c_str());
    return 2;
  }

  // One line per finding, at most kPerShot of a frame (a row of 16 clipped names is one fault).
  constexpr std::size_t kPerShot = 12;
  std::size_t failed = 0, allowed = 0, shown = 0;
  std::string last;
  std::map<std::string, std::pair<std::size_t, std::set<std::string>>> by_reason;  // what the allow list let through: how many findings, in which pictures
  for (const sim::LegibilityFinding& f : findings) {
    if (f.allowed != nullptr) {
      ++allowed;
      auto& r = by_reason[f.allowed];
      ++r.first;
      r.second.insert(f.shot);
      continue;
    }
    ++failed;
    shown = f.shot == last ? shown + 1 : 1;
    last = f.shot;
    if (shown <= kPerShot) {
      std::printf("FINDING %s: %s: %s\n", f.shot.c_str(), sim::legibility_kind_name(f.kind), f.what.c_str());
    } else if (shown == kPerShot + 1) {
      std::printf("FINDING %s: ... and more\n", f.shot.c_str());
    }
  }
  for (const auto& r : by_reason) {
    std::printf("allowed: %zu findings in %zu pictures, for: %s\n", r.second.first, r.second.second.size(), r.first.c_str());
  }
  std::printf("legibility: %zu pictures, %zu findings, %zu allowed\n", shots.size(), failed, allowed);
  if (!opt.sheet.empty()) {
    constexpr int kSheetCols = 3 * sim::kSheetColumns;  // three states across, a column per theme in each
    if (!sim::write_contact_sheet(opt.sheet.string(), shots, kSheetCols)) {
      std::fprintf(stderr, "o3sim: cannot write %s\n", opt.sheet.string().c_str());
      return 2;
    }
    std::printf("legibility: contact sheet %s\n", opt.sheet.string().c_str());
  }
  return failed != 0 ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    fs::path* target = arg == "--scenario" ? &opt.scenario
                       : arg == "--gallery" ? &opt.gallery
                       : arg == "--out"    ? &opt.out
                       : arg == "--goldens" ? &opt.goldens
                       : arg == "--cards"   ? &opt.cards
                       : arg == "--wav"     ? &opt.wav
                       : arg == "--fingerprints" ? &opt.fingerprints
                       : arg == "--fingerprint-diff" ? &opt.fingerprint_diff
                                             : nullptr;
    if (target != nullptr) {
      if (i + 1 >= argc) {
        return usage();
      }
      *target = argv[++i];
    } else if (arg == "--check") {
      opt.check = true;
    } else if (arg == "--update-goldens") {
      opt.update = true;
    } else if (arg == "--check-fingerprints") {
      opt.check_fingerprints = true;
    } else if (arg == "--update-fingerprints") {
      opt.update_fingerprints = true;
    } else if (arg == "--legibility") {
      opt.legibility = true;
    } else if (arg == "--sheet") {
      if (i + 1 >= argc) {
        return usage();
      }
      opt.sheet = argv[++i];
    } else if (arg == "--list") {
      opt.list = true;
    } else if (arg == "--twice") {
      opt.twice = true;
    } else if (arg == "--theme") {
      if (i + 1 >= argc || !ui::find_theme(argv[i + 1], std::strlen(argv[i + 1]), opt.theme)) {
        std::fprintf(stderr, "o3sim: --theme needs one of: amiga, black, white\n");
        return 2;
      }
      opt.has_theme = true;
      ++i;
    } else if (arg == "--only") {
      if (i + 1 >= argc || argv[i + 1][0] == '\0') {
        std::fprintf(stderr, "o3sim: --only needs a non-empty screen name prefix\n");
        return 2;
      }
      opt.has_only = true;
      opt.only = argv[++i];
    } else if (arg == "--shard") {
      char* slash = nullptr;
      char* end = nullptr;
      const unsigned long k = (i + 1 < argc) ? std::strtoul(argv[i + 1], &slash, 10) : 0;
      const unsigned long n = (i + 1 < argc && *slash == '/') ? std::strtoul(slash + 1, &end, 10) : 0;
      if (i + 1 >= argc || *slash != '/' || *end != '\0' || k < 1 || n < 1 || k > n || n > 1000) {
        std::fprintf(stderr, "o3sim: --shard needs <i>/<n> with 1 <= i <= n\n");
        return 2;
      }
      opt.shard = static_cast<unsigned>(k - 1);
      opt.shards = static_cast<unsigned>(n);
      opt.has_shard = true;
      ++i;
    } else if (arg == "--card-rate" || arg == "--card-latency" || arg == "--card-write-rate") {
      char* end = nullptr;
      const unsigned long n = (i + 1 < argc) ? std::strtoul(argv[i + 1], &end, 10) : 0;
      const bool rate = arg != "--card-latency";
      if (i + 1 >= argc || argv[i + 1][0] == '\0' || *end != '\0' || n > 1000000000ul || (rate && n == 0)) {
        std::fprintf(stderr, "o3sim: %s needs a number (1..1000000000 for a rate, 0..1000000000 for the latency)\n", arg.c_str());
        return 2;
      }
      (arg == "--card-rate" ? opt.card_rate : arg == "--card-latency" ? opt.card_latency : opt.card_write_rate) = static_cast<std::uint32_t>(n);
      opt.card_slow = true;
      ++i;
    } else if (arg == "--display-words") {
      char* end = nullptr;
      const unsigned long n = (i + 1 < argc) ? std::strtoul(argv[i + 1], &end, 10) : 0;
      if (i + 1 >= argc || *end != '\0' || n < 2 || n > eve::kMaxDisplayListWords) {
        return usage();
      }
      opt.display_words = n;
      ++i;
    } else {
      return usage();
    }
  }
  // Exactly one of a scenario, a gallery and a legibility run; --out and --display-words belong to scenarios; a legibility run draws the
  // gallery's shots and writes nothing but its sheet, so it takes no directory, no goldens and no second pass.
  if (int(!opt.scenario.empty()) + int(!opt.gallery.empty()) + int(opt.legibility) != 1 || (opt.check && opt.update) ||
      ((opt.check || opt.update) && opt.goldens.empty()) ||
      (!opt.gallery.empty() && (!opt.out.empty() || opt.display_words != eve::kMaxDisplayListWords)) ||
      (opt.legibility && (!opt.out.empty() || opt.display_words != eve::kMaxDisplayListWords || opt.check || opt.update || opt.twice ||
                          !opt.goldens.empty() || opt.list)) ||
      (!opt.sheet.empty() && !opt.legibility) || (!opt.cards.empty() && opt.scenario.empty()) || (opt.card_slow && opt.scenario.empty()) || (!opt.wav.empty() && opt.scenario.empty()) ||
      (opt.fingerprints.empty() != !(opt.check_fingerprints || opt.update_fingerprints)) || (opt.check_fingerprints && opt.update_fingerprints) ||
      (!opt.fingerprints.empty() && opt.scenario.empty()) || (!opt.fingerprint_diff.empty() && !opt.update_fingerprints)) {
    return usage();
  }
  if ((opt.has_only || opt.has_shard) && opt.gallery.empty() && !opt.legibility) {
    std::fprintf(stderr, "o3sim: --only and --shard belong to --gallery and --legibility\n");
    return 2;
  }
  if (opt.list && opt.gallery.empty()) {
    std::fprintf(stderr, "o3sim: --list belongs to --gallery\n");
    return 2;
  }
  if (opt.has_theme && (opt.check || opt.update)) {
    std::fprintf(stderr, "o3sim: the goldens are the default theme's (scenarios) and all themes' (gallery): --theme\n"
                         "       cannot be combined with --check or --update-goldens\n");
    return 2;
  }
  if (opt.legibility) {
    return run_legibility(opt);
  }
  if (!opt.gallery.empty()) {
    return run_gallery(opt);
  }
  // Its goldens keys are "<scenario>/<snap>"; the gallery's are "gallery/<screen>_<state>", and --update-goldens of
  // a scenario of that name would erase them.
  if (opt.scenario.stem() == "gallery") {
    std::fprintf(stderr,
                 "o3sim: the scenario name 'gallery' is reserved: its goldens keys 'gallery/...' belong to --gallery\n");
    return 2;
  }

  std::string text, error;
  if (!read_file(opt.scenario, text)) {
    std::fprintf(stderr, "o3sim: cannot read %s\n", opt.scenario.string().c_str());
    return 2;
  }
  std::vector<sim::ScenarioCommand> commands;
  if (!sim::parse_scenario(text, commands, error)) {
    std::fprintf(stderr, "o3sim: %s: %s\n", opt.scenario.string().c_str(), error.c_str());
    return 2;
  }
  if (!opt.out.empty()) {
    std::error_code ec;
    fs::create_directories(opt.out, ec);
    if (ec) {
      std::fprintf(stderr, "o3sim: cannot create %s: %s\n", opt.out.string().c_str(), ec.message().c_str());
      return 2;
    }
  }

  std::vector<Shot> shots;
  Heard heard;
  if (!run_once(commands, opt, opt.out, shots, error, &heard, opt.wav)) {
    std::fprintf(stderr, "o3sim: %s: %s\n", opt.scenario.string().c_str(), error.c_str());
    return 2;
  }
  int status = 0;
  if (opt.twice) {
    std::vector<Shot> again;
    Heard heard_again;
    if (!run_once(commands, opt, fs::path(), again, error, &heard_again)) {
      std::fprintf(stderr, "o3sim: second run: %s\n", error.c_str());
      return 2;
    }
    for (std::size_t i = 0; i < shots.size(); ++i) {
      if (again.size() != shots.size() || again[i].hash != shots[i].hash) {
        std::fprintf(stderr, "o3sim: NOT DETERMINISTIC: snap '%s' differs between two runs\n", shots[i].name.c_str());
        status = 1;
      }
    }
    // The audio fingerprints: the same text twice (the analysis is deterministic too: no uninitialised read, no order of a map that changes).
    const std::string stem_again = opt.scenario.stem().string();
    if (!opt.fingerprints.empty() && sim::fingerprint_json(stem_again, heard.prints) != sim::fingerprint_json(stem_again, heard_again.prints)) {
      std::fprintf(stderr, "o3sim: NOT DETERMINISTIC: the audio fingerprint differs between two runs\n");
      status = 1;
    }
    // The audio: the same renders, sample for sample (a hash, not a golden: the samples are reproducible on one machine and compiler, the checks of `assert audio` carry the tolerances).
    if (heard_again.digests != heard.digests) {
      for (const auto& d : heard.digests) {
        const auto same = std::find(heard_again.digests.begin(), heard_again.digests.end(), d);
        if (same == heard_again.digests.end()) {
          std::fprintf(stderr, "o3sim: NOT DETERMINISTIC: render '%s' differs between two runs\n", d.first.c_str());
        }
      }
      if (heard_again.digests.size() != heard.digests.size()) {
        std::fprintf(stderr, "o3sim: NOT DETERMINISTIC: the two runs made %zu and %zu renders\n", heard.digests.size(), heard_again.digests.size());
      }
      status = 1;
    }
  }
  for (const std::string& line : heard.log) {
    std::printf("audio %s %s\n", opt.scenario.stem().string().c_str(), line.c_str());
  }
  for (std::size_t i = 0; i < heard.digests.size(); ++i) {
    std::printf("audio %s/%s %s  frames=%zu\n", opt.scenario.stem().string().c_str(), heard.digests[i].first.c_str(), heard.digests[i].second.c_str(), heard.frames[i]);
  }

  const std::string stem = opt.scenario.stem().string();
  if (!opt.fingerprints.empty() && opt.update_fingerprints && status != 0) {
    std::fprintf(stderr, "o3sim: the audio fingerprint is not rewritten: the run was not deterministic\n");
  } else if (!opt.fingerprints.empty()) {
    const int fp = fingerprint_step(opt, stem, heard.prints, std::any_of(commands.begin(), commands.end(), [](const sim::ScenarioCommand& c) { return c.kind == sim::ScenarioCommand::Kind::Fingerprint; }));
    if (fp == 2) {
      return 2;
    }
    status = std::max(status, fp);
  }
  Goldens goldens;
  if (opt.check || opt.update) {
    if (!read_goldens(opt.goldens, goldens, opt.check)) {
      return opt.check ? 1 : 2;
    }
  }
  std::map<std::string, bool> seen;
  for (const Shot& shot : shots) {
    const std::string key = stem + "/" + shot.name;
    std::string verdict;
    if (opt.check) {
      const auto it = goldens.find(key);
      if (it == goldens.end()) {
        verdict = "  NEW (no golden)";
        status = 1;
      } else if (it->second != shot.hash) {
        verdict = "  MISMATCH (golden " + it->second + ")";
        status = 1;
      } else {
        verdict = "  ok";
      }
      seen[key] = true;
    }
    std::printf("%s %s  dl=%zu/%zu%s\n", key.c_str(), shot.hash.c_str(), shot.dl_words, eve::kMaxDisplayListWords,
                verdict.c_str());
  }
  if (opt.check) {
    for (const auto& entry : goldens) {
      if (entry.first.compare(0, stem.size() + 1, stem + "/") == 0 && !seen.count(entry.first)) {
        std::fprintf(stderr, "o3sim: stale golden '%s': the scenario no longer produces it\n", entry.first.c_str());
        status = 1;
      }
    }
  }
  if (opt.update) {
    for (auto it = goldens.begin(); it != goldens.end();) {
      it = (it->first.compare(0, stem.size() + 1, stem + "/") == 0) ? goldens.erase(it) : std::next(it);
    }
    for (const Shot& shot : shots) {
      goldens[stem + "/" + shot.name] = shot.hash;
    }
    if (!write_goldens(opt.goldens, goldens)) {
      std::fprintf(stderr, "o3sim: cannot write %s\n", opt.goldens.string().c_str());
      return 2;
    }
  }
  return status;
}
