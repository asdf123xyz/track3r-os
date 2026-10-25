// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/app_export.h (OS-H4, ADR 0014 amendment A): the export writes an SD app module is lent, against an in-memory card (support/mem_card.h) that applies the REAL path guard to every call that changes
// it and counts the calls the guard refuses (`violations`). What is proved here:
//   - the ring: wrap-around, BUSY when full and nothing taken, a begin() called twice fails the export (no restart);
//   - the calls: every code of o3module.h in the cases that make it (RANGE for names, extensions, NULL, 0 and more than the ring; STATE; LIMIT at 16 files, at 999 suffixes and at 4 MiB; NO_CARD for an
//     unmarked card, a save that runs, the firmware update path); the names (<stem>.mid, <stem>_001.mid, the stem cut to 31 bytes with the suffix);
//   - the properties: a file written in random chunks with BUSY retries is byte-identical on the card; 4 MiB is the most (exactly 4 MiB is whole, one more byte is LIMIT and FAILED with no file left);
//   - the aborts: the app leaving at EVERY step of the commit leaves no Temp and no partial final; a file already DONE stays; a user's save while an export commits wins (the export is FAILED, the
//     save completes); a power cut at EVERY mutating call leaves no partial final and the next export heals the stale Temp;
//   - a fuzz of the four calls with begin_app / end_app, random names and extensions, n from 0 to beyond the ring, faults, in and out of an open file: every code is published, nothing is created
//     outside AppExport/<app>/, nothing that existed before is changed or removed, only Temps are removed, and every final file is whole.
// The module host side (grant by capability and ABI, the confirm, TOOLS, the whole stack with SMF EXPORT) is in module_edit_test.cpp. Host only.
#include <doctest/doctest.h>

#include "support/mem_card.h"
#include "support/need.h"
#include "support/project_fixture.h"
#include "support/rng.h"

#include <o3module.h>

#include <o3os/app/app_export.h>
#include <o3os/app/save_service.h>
#include <o3os/model/settings.h>
#include <o3os/platform/host/fakes.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using o3os::test::MemCard;
using o3os::test::Rng;
namespace app = o3os::app;
namespace hs = o3os::hal::storage;
using Bytes = MemCard::Bytes;

namespace {

Bytes pattern(std::size_t n, unsigned salt) {
  Bytes b(n);
  for (std::size_t i = 0; i < n; ++i) {
    b[i] = static_cast<std::uint8_t>((i * 31u + salt * 17u + (i >> 8)) & 0xFFu);
  }
  return b;
}

struct Rig {
  MemCard card;
  app::SaveService svc;
  o3os::core::JobRunner jobs;
  o3os::platform::host::FakeClock clock;
  o3os::model::Settings settings;
  alignas(8) std::uint8_t ring[app::AppExport::kRingBytes] = {};
  std::uint64_t now_ms = 0;

  explicit Rig(bool with_ring = true) {
    svc.attach(card, card);
    if (with_ring) {
      svc.attach_app_export(ring, sizeof ring);
    }
    settings.init(o3os::core::ParamKind::Device);
    frame();  // (the first pass of the main loop: the service sees the runner)
  }
  app::AppExport& ex() { return svc.app_export(); }
  // One pass of the main loop: the jobs get a slice, the service takes the ended one in, a finished slot is released (App::report_jobs).
  void frame() {
    (void)jobs.run(clock, 2000);
    clock.advance(2000);
    now_ms += 16;
    svc.poll(jobs, settings, now_ms);
    for (unsigned slot = 0; slot < o3os::core::JobRunner::kSlots; ++slot) {
      const o3os::core::SlotState st = jobs.state(slot);
      if (st != o3os::core::SlotState::Free && st != o3os::core::SlotState::Running) {
        jobs.release(slot);
      }
    }
  }
  void frames(unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      frame();
    }
  }
  // Frames until the status leaves WRITING; the status it reached (and the name on DONE).
  std::uint32_t wait(std::string* name = nullptr, unsigned max = 200000) {
    for (unsigned i = 0; i < max; ++i) {
      frame();
      char buf[48] = {};
      const std::uint32_t st = ex().status(buf, sizeof buf);
      if (st != O3M_EXPORT_WRITING) {
        if (name != nullptr) {
          *name = buf;
        }
        return st;
      }
    }
    return 0xFFFFu;
  }
  // The whole of `data` written (BUSY retried on later frames), closed, and waited for.
  std::uint32_t write_file(const char* stem, const char* ext, const Bytes& data, std::size_t chunk, std::string* name = nullptr) {
    const std::int32_t o = ex().open(stem, ext);
    if (o != O3M_EXPORT_OK) {
      return 0x100u + static_cast<std::uint32_t>(o);
    }
    for (std::size_t at = 0; at < data.size();) {
      const std::size_t n = data.size() - at < chunk ? data.size() - at : chunk;
      const std::int32_t w = ex().write(data.data() + at, static_cast<std::uint32_t>(n));
      if (w == O3M_EXPORT_BUSY) {
        frame();
        continue;
      }
      if (w == O3M_EXPORT_STATE) {
        return wait(name);  // the file failed under the app (a card that is full, pulled ...): the status says so
      }
      if (w != O3M_EXPORT_OK) {
        return 0x200u + static_cast<std::uint32_t>(w);
      }
      at += n;
    }
    const std::int32_t c = ex().close();
    if (c == O3M_EXPORT_STATE) {
      return wait(name);
    }
    if (c != O3M_EXPORT_OK) {
      return 0x300u;
    }
    return wait(name);
  }
  bool has_temp() const {
    for (const auto& f : card.files) {
      if (f.first.size() >= 11 && f.first.compare(f.first.size() - 11, 11, "_o3save.tmp") == 0) {
        return true;
      }
    }
    return false;
  }
};

const char* kApp = "smfexport";
const char* kDir = "AppExport/smfexport";

}  // namespace

// ---- the ring ------------------------------------------------------------------------------------------------------------------------------------

namespace {

// A sink that keeps what it is given.
class VecSink final : public o3os::core::ByteSink {
 public:
  o3os::core::IoResult write(const void* src, std::size_t n) noexcept override {
    const std::uint8_t* p = static_cast<const std::uint8_t*>(src);
    data.insert(data.end(), p, p + n);
    return {n, o3os::core::IoStatus::Ok};
  }
  Bytes data;
};

}  // namespace

TEST_CASE("app export ring: wrap-around keeps every byte in order, a full ring takes nothing, and the job ends when the app has closed and the ring is empty") {
  alignas(8) std::uint8_t ring[64];
  app::AppFileSource src;
  src.attach(ring, sizeof ring);
  VecSink sink;
  src.begin(sink);
  Bytes all;
  Rng rng(7);
  unsigned wraps = 0;
  for (unsigned round = 0; round < 500; ++round) {
    const std::size_t n = 1 + rng.below(70);
    const Bytes chunk = pattern(n, round);
    const std::size_t free_before = src.space();
    const bool took = src.push(chunk.data(), n);
    CHECK(took == (n <= free_before));
    if (took) {
      all.insert(all.end(), chunk.begin(), chunk.end());
      CHECK(src.space() == free_before - n);
    } else {
      CHECK(src.space() == free_before);  // nothing was taken
    }
    const std::size_t drained = sink.data.size();
    const std::size_t head_wrap = all.size() / 64;
    wraps = static_cast<unsigned>(head_wrap);
    CHECK(src.step(1 + rng.below(40)) == o3os::formats::Progress::Running);
    CHECK(sink.data.size() >= drained);
    CHECK(sink.data.size() <= all.size());
  }
  CHECK(wraps > 100u);
  while (src.used() != 0) {
    CHECK(src.step(17) == o3os::formats::Progress::Running);  // (not closed yet: not Done)
  }
  src.finish();
  CHECK(src.step(1) == o3os::formats::Progress::Done);
  CHECK(sink.data == all);
  CHECK_FALSE(src.push(nullptr, 1));
  CHECK_FALSE(src.push(ring, 65));
}

TEST_CASE("app export ring: begin() called a second time (a retry the ring cannot do) fails the export, and an abort fails the next step") {
  alignas(8) std::uint8_t ring[64];
  app::AppFileSource src;
  src.attach(ring, sizeof ring);
  VecSink sink;
  src.begin(sink);
  CHECK(src.push(ring, 10));
  CHECK(src.step(100) == o3os::formats::Progress::Running);
  src.begin(sink);  // the atomic save wants to start over
  CHECK(src.step(100) == o3os::formats::Progress::Failed);
  CHECK(std::string(src.failure()) == "CANNOT RESTART");
  src.reset();
  src.begin(sink);
  src.abort();
  CHECK(src.step(100) == o3os::formats::Progress::Failed);
  CHECK(std::string(src.failure()) == "ABORTED");
}

// ---- the calls -----------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("app export: a file is written whole to AppExport/<app>/<name>.<ext>, atomically, and the status says DONE once with the name") {
  Rig r;
  r.ex().begin_app(kApp);
  CHECK(r.ex().status(nullptr, 0) == O3M_EXPORT_IDLE);
  const Bytes data = pattern(5000, 1);
  std::string name;
  CHECK(r.write_file("PAT001", "mid", data, 700, &name) == O3M_EXPORT_DONE);
  CHECK(name == "PAT001.mid");
  CHECK(r.card.same("AppExport/smfexport/PAT001.mid", data));
  CHECK_FALSE(r.has_temp());
  CHECK(r.card.dirs.count("appexport") == 1);
  CHECK(r.card.dirs.count("appexport/smfexport") == 1);
  CHECK(r.card.violations == 0);
  CHECK(r.card.removed().empty());
  CHECK(r.ex().status(nullptr, 0) == O3M_EXPORT_IDLE);  // DONE was read once
  // the order of what it did to the card: the folders, the Temp, the bytes, the sync, one rename
  CHECK(r.card.log.front() == "mkdir AppExport");
  CHECK(r.card.log[1] == "mkdir AppExport/smfexport");
  CHECK(r.card.log[2] == "create AppExport/smfexport/_o3save.tmp");
  CHECK(r.card.log[r.card.log.size() - 2] == "finish");
  CHECK(r.card.log.back() == "rename AppExport/smfexport/_o3save.tmp>AppExport/smfexport/PAT001.mid");
  // an empty file is a file
  CHECK(r.write_file("empty", "txt", Bytes(), 1) == O3M_EXPORT_DONE);
  CHECK(r.card.has("AppExport/smfexport/empty.txt"));
  CHECK(r.card.get("AppExport/smfexport/empty.txt")->empty());
  // every allowlisted extension
  for (const char* ext : {"mid", "syx", "txt", "csv", "json"}) {
    CHECK(r.write_file("allowed", ext, pattern(10, 2), 10) == O3M_EXPORT_DONE);
    CHECK(r.card.has((std::string(kDir) + "/allowed." + ext).c_str()));
  }
  CHECK(r.card.violations == 0);
}

TEST_CASE("app export: a name that is taken becomes _001, _002 ...; the stem is cut so that the suffix fits 31 bytes; past _999 it is LIMIT") {
  Rig r;
  r.ex().begin_app(kApp);
  std::string name;
  CHECK(r.write_file("PAT001", "mid", pattern(10, 1), 10, &name) == O3M_EXPORT_DONE);
  CHECK(name == "PAT001.mid");
  CHECK(r.write_file("PAT001", "mid", pattern(11, 2), 11, &name) == O3M_EXPORT_DONE);
  CHECK(name == "PAT001_001.mid");
  CHECK(r.write_file("PAT001", "mid", pattern(12, 3), 12, &name) == O3M_EXPORT_DONE);
  CHECK(name == "PAT001_002.mid");
  CHECK(r.card.same("AppExport/smfexport/PAT001.mid", pattern(10, 1)));  // never replaced
  CHECK(r.card.same("AppExport/smfexport/PAT001_001.mid", pattern(11, 2)));
  CHECK(r.card.same("AppExport/smfexport/PAT001_002.mid", pattern(12, 3)));
  // another extension is another name
  CHECK(r.write_file("PAT001", "txt", pattern(5, 4), 5, &name) == O3M_EXPORT_DONE);
  CHECK(name == "PAT001.txt");
  // a folder of that name counts as taken
  r.card.make_dir_now("AppExport/smfexport/DIR.mid");
  CHECK(r.write_file("DIR", "mid", pattern(5, 5), 5, &name) == O3M_EXPORT_DONE);
  CHECK(name == "DIR_001.mid");
  // a file of the user's with the name
  r.card.put("AppExport/smfexport/MINE.mid", std::string("mine"));
  CHECK(r.write_file("mine", "mid", pattern(5, 6), 5, &name) == O3M_EXPORT_DONE);  // (FAT: the case does not matter)
  CHECK(name == "mine_001.mid");
  CHECK(r.card.same("AppExport/smfexport/MINE.mid", Bytes{'m', 'i', 'n', 'e'}));
  // the longest stem: 31 bytes. Its first file is the name; the second cuts the stem to 27 and adds _001
  const std::string longest(31, 'x');
  CHECK(r.write_file(longest.c_str(), "json", pattern(5, 7), 5, &name) == O3M_EXPORT_DONE);
  CHECK(name == longest + ".json");
  CHECK(r.write_file(longest.c_str(), "json", pattern(5, 8), 5, &name) == O3M_EXPORT_DONE);
  CHECK(name == std::string(27, 'x') + "_001.json");
  CHECK(name.size() == 27 + 4 + 5);
  CHECK(r.card.violations == 0);
}

TEST_CASE("app export: when 999 suffixes are taken the open is LIMIT and nothing is made") {
  Rig r;
  r.ex().begin_app(kApp);
  r.card.make_dir_now("AppExport");
  r.card.make_dir_now("AppExport/smfexport");
  r.card.put("AppExport/smfexport/PAT001.mid", std::string("x"));
  for (unsigned i = 1; i <= 999; ++i) {
    char n[48];
    std::snprintf(n, sizeof n, "AppExport/smfexport/PAT001_%03u.mid", i);
    r.card.put(n, std::string("x"));
  }
  const std::size_t ops = r.card.ops;
  CHECK(r.ex().open("PAT001", "mid") == O3M_EXPORT_LIMIT);
  CHECK(r.card.ops == ops);
  CHECK(r.ex().opens() == 0u);  // (a refused open does not count)
  CHECK(r.ex().status(nullptr, 0) == O3M_EXPORT_IDLE);
  // a run of taken names ends at 499: the lowest free number is found
  for (unsigned i = 500; i <= 999; ++i) {
    char n[48];
    std::snprintf(n, sizeof n, "appexport/smfexport/pat001_%03u.mid", i);
    r.card.files.erase(n);
  }
  std::string name;
  CHECK(r.write_file("PAT001", "mid", pattern(3, 1), 3, &name) == O3M_EXPORT_DONE);
  CHECK(name == "PAT001_500.mid");
}

TEST_CASE("app export: the search for a free name is bounded (a directory lookup each, inside the app's call), whatever number of names are taken") {
  for (const unsigned taken : {0u, 1u, 7u, 8u, 9u, 100u, 600u, 998u}) {
    Rig r;
    r.ex().begin_app(kApp);
    r.card.make_dir_now("AppExport");
    r.card.make_dir_now("AppExport/smfexport");
    r.card.put("AppExport/smfexport/PAT001.mid", std::string("x"));
    for (unsigned i = 1; i <= taken; ++i) {
      char n[48];
      std::snprintf(n, sizeof n, "AppExport/smfexport/PAT001_%03u.mid", i);
      r.card.put(n, std::string("x"));
    }
    const std::size_t before = r.card.exists_calls;
    CHECK(r.ex().open("PAT001", "mid") == O3M_EXPORT_OK);
    const std::size_t probes = r.card.exists_calls - before;
    CAPTURE(taken);
    CHECK(probes <= 30u);
    // contiguous taken names: the lowest free one
    char want[24];
    std::snprintf(want, sizeof want, "PAT001_%03u.mid", taken + 1u);
    (void)r.ex().close();
    std::string name;
    CHECK(r.wait(&name) == O3M_EXPORT_DONE);
    CHECK(name == want);
  }
  // 999 taken: LIMIT, still bounded
  Rig r;
  r.ex().begin_app(kApp);
  r.card.make_dir_now("AppExport");
  r.card.make_dir_now("AppExport/smfexport");
  r.card.put("AppExport/smfexport/PAT001.mid", std::string("x"));
  for (unsigned i = 1; i <= 999; ++i) {
    char n[48];
    std::snprintf(n, sizeof n, "AppExport/smfexport/PAT001_%03u.mid", i);
    r.card.put(n, std::string("x"));
  }
  const std::size_t before = r.card.exists_calls;
  CHECK(r.ex().open("PAT001", "mid") == O3M_EXPORT_LIMIT);
  CHECK(r.card.exists_calls - before <= 30u);
  // holes: a free number is found (never a taken one), nothing is replaced
  Rig h;
  h.ex().begin_app(kApp);
  h.card.make_dir_now("AppExport");
  h.card.make_dir_now("AppExport/smfexport");
  h.card.put("AppExport/smfexport/PAT001.mid", std::string("x"));
  for (unsigned i = 1; i <= 999; ++i) {
    if (i % 7u != 0u) {
      char n[48];
      std::snprintf(n, sizeof n, "AppExport/smfexport/PAT001_%03u.mid", i);
      h.card.put(n, std::string("x"));
    }
  }
  std::string hname;
  CHECK(h.write_file("PAT001", "mid", pattern(3, 1), 3, &hname) == O3M_EXPORT_DONE);
  CHECK(h.card.violations == 0);
  CHECK(hname.size() == std::string("PAT001_007.mid").size());
}

TEST_CASE("app export: the 17th open of a launch is LIMIT, the count restarts with the next launch, and files already DONE stay") {
  Rig r;
  r.ex().begin_app(kApp);
  for (unsigned i = 0; i < 16; ++i) {
    CHECK(r.write_file("f", "csv", pattern(4, i), 4) == O3M_EXPORT_DONE);
  }
  CHECK(r.ex().opens() == 16u);
  CHECK(r.ex().open("f", "csv") == O3M_EXPORT_LIMIT);
  CHECK(r.ex().open("other", "csv") == O3M_EXPORT_LIMIT);
  CHECK(r.card.files.size() == 16u);
  r.ex().end_app();
  r.ex().begin_app(kApp);  // launched again
  CHECK(r.ex().opens() == 0u);
  CHECK(r.write_file("f", "csv", pattern(4, 99), 4) == O3M_EXPORT_DONE);
  CHECK(r.card.files.size() == 17u);
  CHECK(r.card.violations == 0);
  CHECK(r.card.removed().empty());
}

TEST_CASE("app export: RANGE for a bad name, an extension that is not on the list, NULL, 0 bytes and more than the ring; the card is not touched") {
  Rig r;
  r.ex().begin_app(kApp);
  const std::string long_name(32, 'x');
  for (const char* name : {"", "a/b", "a\\b", "..", ".", "a..b", "CON", "nul", "COM1", " lead", "trail ", "trail.", ".hidden", "-x", "a:b", "a*b", "a?b", "a\"b", "a<b", "a|b", "caf\xC3\xA9", "tab\tx",
                           "../x", "AppExport/x", "x.mid", "_o3save"}) {
    const std::int32_t rc = r.ex().open(name, "mid");
    INFO("name: " << name);
    if (std::string(name) == "x.mid") {
      CHECK(rc == O3M_EXPORT_OK);  // (a dot inside a name is a name; the extension is added to it: "x.mid.mid")
      CHECK(r.ex().close() == O3M_EXPORT_OK);
      CHECK(r.wait() == O3M_EXPORT_DONE);
      CHECK(r.card.has("AppExport/smfexport/x.mid.mid"));
    } else {
      CHECK(rc == O3M_EXPORT_RANGE);
    }
  }
  CHECK(r.ex().open(long_name.c_str(), "mid") == O3M_EXPORT_RANGE);
  CHECK(r.ex().open(nullptr, "mid") == O3M_EXPORT_RANGE);
  CHECK(r.ex().open("ok", nullptr) == O3M_EXPORT_RANGE);
  for (const char* ext : {"", "MID", "Mid", "ptf", "PTF", "tmod", "o3pj", "wav", "pti", "bak", "tmp", ".mid", "mid.ptf", "mid ", " mid", "midi", "jso", "jsonx", "mid/", "m\0id"}) {
    INFO("ext: " << ext);
    CHECK(r.ex().open("ok", ext) == O3M_EXPORT_RANGE);
  }
  // the card was not touched by any of the refusals but the one name that was fine
  for (const std::string& l : r.card.log) {
    CHECK((l.find("x.mid.mid") != std::string::npos || l.find("_o3save.tmp") != std::string::npos || l.rfind("mkdir", 0) == 0 || l.rfind("write", 0) == 0 || l == "finish"));
  }
  CHECK(r.card.violations == 0);
  // write
  CHECK(r.ex().open("w", "txt") == O3M_EXPORT_OK);
  const Bytes data = pattern(app::AppExport::kRingBytes + 1, 3);
  CHECK(r.ex().write(nullptr, 5) == O3M_EXPORT_RANGE);
  CHECK(r.ex().write(data.data(), 0) == O3M_EXPORT_RANGE);
  CHECK(r.ex().write(data.data(), app::AppExport::kRingBytes + 1) == O3M_EXPORT_RANGE);  // more than the ring can ever hold
  CHECK(r.ex().write(data.data(), 0xFFFFFFFFu) == O3M_EXPORT_RANGE);
  CHECK(r.ex().file_bytes() == 0u);  // none of them took a byte
  CHECK(r.ex().write(data.data(), app::AppExport::kRingBytes) == O3M_EXPORT_OK);  // exactly the ring is fine
  CHECK(r.ex().close() == O3M_EXPORT_OK);
  CHECK(r.wait() == O3M_EXPORT_DONE);
  CHECK(r.card.same("AppExport/smfexport/w.txt", Bytes(data.begin(), data.begin() + app::AppExport::kRingBytes)));
}

TEST_CASE("app export: STATE for an open while a file is open or committing, and a write or close with no file open; the status is IDLE with nothing to say") {
  Rig r;
  CHECK(r.ex().open("a", "mid") == O3M_EXPORT_STATE);  // (no app has been launched)
  r.ex().begin_app(kApp);
  const Bytes data = pattern(100, 1);
  CHECK(r.ex().write(data.data(), 10) == O3M_EXPORT_STATE);
  CHECK(r.ex().close() == O3M_EXPORT_STATE);
  CHECK(r.ex().status(nullptr, 0) == O3M_EXPORT_IDLE);
  CHECK(r.ex().open("a", "mid") == O3M_EXPORT_OK);
  CHECK(r.ex().status(nullptr, 0) == O3M_EXPORT_WRITING);
  CHECK(r.ex().open("b", "mid") == O3M_EXPORT_STATE);  // open
  CHECK(r.ex().write(data.data(), 10) == O3M_EXPORT_OK);
  CHECK(r.ex().close() == O3M_EXPORT_OK);
  CHECK(r.ex().open("b", "mid") == O3M_EXPORT_STATE);  // committing
  CHECK(r.ex().write(data.data(), 10) == O3M_EXPORT_STATE);
  CHECK(r.ex().close() == O3M_EXPORT_STATE);
  CHECK(r.ex().status(nullptr, 0) == O3M_EXPORT_WRITING);
  CHECK(r.wait() == O3M_EXPORT_DONE);
  CHECK(r.ex().write(data.data(), 10) == O3M_EXPORT_STATE);  // the file is over
  CHECK(r.ex().close() == O3M_EXPORT_STATE);
  CHECK(r.ex().open("b", "mid") == O3M_EXPORT_OK);  // the next one
  CHECK(r.card.violations == 0);
}

TEST_CASE("app export: NO_CARD for a card that may not be written, a save that runs, the firmware update path and a build with no ring; nothing is made") {
  {
    Rig r;
    r.card.gate_status = hs::WriteStatus::WriteProtected;
    r.ex().begin_app(kApp);
    CHECK(r.ex().open("a", "mid") == O3M_EXPORT_NO_CARD);
    CHECK(r.card.log.empty());
    CHECK(r.card.files.empty());
    CHECK(r.card.dirs.empty());
    CHECK(r.ex().opens() == 0u);
    CHECK(r.ex().status(nullptr, 0) == O3M_EXPORT_IDLE);
    r.card.gate_status = hs::WriteStatus::Ok;  // the card is marked: it works
    CHECK(r.write_file("a", "mid", pattern(3, 1), 3) == O3M_EXPORT_DONE);
  }
  {
    Rig r(false);  // no ring: the platform gave none
    r.ex().begin_app(kApp);
    CHECK(r.ex().open("a", "mid") == O3M_EXPORT_NO_CARD);
    CHECK(r.card.log.empty());
  }
  {
    Rig r;
    static bool in_use;
    in_use = true;
    r.svc.attach_claim([]() noexcept { return in_use; });
    r.ex().begin_app(kApp);
    CHECK(r.ex().open("a", "mid") == O3M_EXPORT_NO_CARD);
    CHECK(r.card.log.empty());
    in_use = false;
    CHECK(r.write_file("a", "mid", pattern(3, 1), 3) == O3M_EXPORT_DONE);
  }
  {
    Rig r;
    r.ex().begin_app(kApp);
    o3os::model::ModelArena& arena = o3os::test::arena(1);
    o3os::test::fill_small(arena.project);
    CHECK(r.svc.save_project(r.jobs, arena.project, "", true) == app::SaveService::Start::Started);
    CHECK(r.ex().open("a", "mid") == O3M_EXPORT_NO_CARD);  // the save layer is busy with a save it cannot share
    for (unsigned i = 0; i < 2000 && r.svc.busy(); ++i) {
      r.frame();
    }
    CHECK(r.write_file("a", "mid", pattern(3, 1), 3) == O3M_EXPORT_DONE);  // the save is over: it works
    CHECK(r.card.violations == 0);
  }
}

TEST_CASE("app export: an app folder name the guard refuses (the app's own stem) is RANGE, whatever the file") {
  Rig r;
  for (const char* stem : {"CON", "a/b", "..", "", " x", "x "}) {
    r.ex().begin_app(stem);
    CHECK(r.ex().open("ok", "mid") == O3M_EXPORT_RANGE);
    CHECK(r.card.log.empty());
  }
  r.ex().begin_app(std::string(32, 'x').c_str());
  CHECK(r.ex().open("ok", "mid") == O3M_EXPORT_RANGE);
  r.ex().begin_app(std::string(31, 'x').c_str());
  CHECK(r.ex().open("ok", "mid") == O3M_EXPORT_OK);
}

// ---- the properties ------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("app export property: a file written in random chunk sizes with BUSY retries is byte-identical on the card") {
  Rng rng(0xA11CE);
  for (unsigned round = 0; round < 24; ++round) {
    Rig r;
    r.ex().begin_app(kApp);
    const std::size_t size = round == 0 ? 0 : (round < 4 ? round : rng.below(120000));
    const Bytes data = pattern(size, round);
    CHECK(r.ex().open("rand", "json") == O3M_EXPORT_OK);
    std::size_t at = 0;
    unsigned busy = 0;
    unsigned guard = 0;
    while (at < data.size() && guard++ < 1000000) {
      std::size_t n = 1 + rng.below(round % 3 == 0 ? 64 : app::AppExport::kRingBytes);
      n = n > data.size() - at ? data.size() - at : n;
      const std::int32_t w = r.ex().write(data.data() + at, static_cast<std::uint32_t>(n));
      if (w == O3M_EXPORT_BUSY) {
        ++busy;
        if (rng.below(3) == 0) {
          r.frame();  // sometimes the main loop runs between the retries, sometimes the app asks again at once
        }
        continue;
      }
      NEED(w == O3M_EXPORT_OK);
      at += n;
    }
    CHECK(r.ex().close() == O3M_EXPORT_OK);
    std::string name;
    CHECK(r.wait(&name) == O3M_EXPORT_DONE);
    CHECK(name == "rand.json");
    CHECK(r.card.same("AppExport/smfexport/rand.json", data));
    if (size > 3 * app::AppExport::kRingBytes) {
      CHECK(busy > 0u);  // the ring was full at times: BUSY took nothing and the same bytes went later
    }
    CHECK(r.card.violations == 0);
    CHECK_FALSE(r.has_temp());
  }
}

TEST_CASE("app export property: a BUSY write takes nothing, so the same bytes again give the same file") {
  Rig r;
  r.ex().begin_app(kApp);
  CHECK(r.ex().open("busy", "txt") == O3M_EXPORT_OK);
  const Bytes a = pattern(app::AppExport::kRingBytes, 1);
  const Bytes b = pattern(100, 2);
  CHECK(r.ex().write(a.data(), static_cast<std::uint32_t>(a.size())) == O3M_EXPORT_OK);   // the ring is full
  CHECK(r.ex().write(b.data(), static_cast<std::uint32_t>(b.size())) == O3M_EXPORT_BUSY);
  CHECK(r.ex().write(b.data(), 1) == O3M_EXPORT_BUSY);
  CHECK(r.ex().file_bytes() == a.size());
  while (r.ex().write(b.data(), static_cast<std::uint32_t>(b.size())) == O3M_EXPORT_BUSY) {
    r.frame();
  }
  CHECK(r.ex().close() == O3M_EXPORT_OK);
  CHECK(r.wait() == O3M_EXPORT_DONE);
  Bytes want = a;
  want.insert(want.end(), b.begin(), b.end());
  CHECK(r.card.same("AppExport/smfexport/busy.txt", want));
}

TEST_CASE("app export property: 4 MiB is the most; exactly 4 MiB is whole, one byte more is LIMIT and FAILED with no file and no Temp") {
  constexpr std::size_t kMax = app::AppExport::kMaxFileBytes;
  {
    Rig r;
    r.ex().begin_app(kApp);
    const Bytes chunk = pattern(app::AppExport::kRingBytes, 5);
    CHECK(r.ex().open("big", "csv") == O3M_EXPORT_OK);
    for (std::size_t at = 0; at < kMax;) {
      const std::int32_t w = r.ex().write(chunk.data(), static_cast<std::uint32_t>(app::AppExport::kRingBytes));
      if (w == O3M_EXPORT_BUSY) {
        r.frame();
        continue;
      }
      NEED(w == O3M_EXPORT_OK);
      at += app::AppExport::kRingBytes;
    }
    CHECK(r.ex().file_bytes() == kMax);
    CHECK(r.ex().close() == O3M_EXPORT_OK);
    CHECK(r.wait() == O3M_EXPORT_DONE);
    const Bytes* f = r.card.get("AppExport/smfexport/big.csv");
    NEED(f != nullptr);
    CHECK(f->size() == kMax);
    CHECK(std::memcmp(f->data() + kMax - chunk.size(), chunk.data(), chunk.size()) == 0);
    CHECK(r.card.violations == 0);
  }
  {
    Rig r;
    r.ex().begin_app(kApp);
    const Bytes chunk = pattern(app::AppExport::kRingBytes, 5);
    CHECK(r.ex().open("big", "csv") == O3M_EXPORT_OK);
    for (std::size_t at = 0; at < kMax;) {
      const std::int32_t w = r.ex().write(chunk.data(), static_cast<std::uint32_t>(app::AppExport::kRingBytes));
      if (w == O3M_EXPORT_BUSY) {
        r.frame();
        continue;
      }
      NEED(w == O3M_EXPORT_OK);
      at += app::AppExport::kRingBytes;
    }
    CHECK(r.ex().write(chunk.data(), 1) == O3M_EXPORT_LIMIT);  // 4 MiB + 1
    CHECK(r.ex().status(nullptr, 0) == O3M_EXPORT_FAILED);
    CHECK(r.ex().status(nullptr, 0) == O3M_EXPORT_IDLE);
    CHECK(r.ex().write(chunk.data(), 1) == O3M_EXPORT_STATE);  // the file is gone
    CHECK(r.ex().close() == O3M_EXPORT_STATE);
    r.frames(10);
    CHECK_FALSE(r.has_temp());
    CHECK(r.card.files.empty());
    CHECK(r.card.violations == 0);
    for (const std::string& rem : r.card.removed()) {
      CHECK(rem == "AppExport/smfexport/_o3save.tmp");
    }
    // the next file is fine
    CHECK(r.write_file("small", "csv", pattern(10, 1), 10) == O3M_EXPORT_DONE);
  }
  {
    Rig r;  // one call that crosses the cap
    r.ex().begin_app(kApp);
    const Bytes chunk = pattern(app::AppExport::kRingBytes, 5);
    CHECK(r.ex().open("big", "csv") == O3M_EXPORT_OK);
    for (std::size_t at = 0; at + app::AppExport::kRingBytes < kMax;) {
      const std::int32_t w = r.ex().write(chunk.data(), static_cast<std::uint32_t>(app::AppExport::kRingBytes));
      if (w == O3M_EXPORT_BUSY) {
        r.frame();
        continue;
      }
      NEED(w == O3M_EXPORT_OK);
      at += app::AppExport::kRingBytes;
    }
    std::int32_t w2 = O3M_EXPORT_BUSY;
    while (w2 == O3M_EXPORT_BUSY) {
      w2 = r.ex().write(chunk.data(), 2);  // 4 MiB - 16 KiB + 2 ... still under
      if (w2 == O3M_EXPORT_BUSY) {
        r.frame();
      }
    }
    CHECK(w2 == O3M_EXPORT_OK);
    CHECK(r.ex().write(chunk.data(), app::AppExport::kRingBytes) == O3M_EXPORT_LIMIT);
    CHECK(r.ex().status(nullptr, 0) == O3M_EXPORT_FAILED);
    r.frames(10);
    CHECK(r.card.files.empty());
  }
}

// ---- the aborts ----------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("app export abort: the app leaving at EVERY step of an export leaves no Temp, no partial final and nothing but its own Temp removed; a file already DONE stays") {
  // how many frames the whole export takes
  unsigned total = 0;
  {
    Rig r;
    r.ex().begin_app(kApp);
    const Bytes data = pattern(40000, 1);
    CHECK(r.ex().open("gone", "mid") == O3M_EXPORT_OK);
    for (std::size_t at = 0; at < data.size();) {
      const std::size_t n = data.size() - at < 4096 ? data.size() - at : 4096;
      if (r.ex().write(data.data() + at, static_cast<std::uint32_t>(n)) == O3M_EXPORT_BUSY) {
        r.frame();
        ++total;
        continue;
      }
      at += n;
    }
    CHECK(r.ex().close() == O3M_EXPORT_OK);
    while (r.ex().status(nullptr, 0) == O3M_EXPORT_WRITING) {
      r.frame();
      ++total;
    }
  }
  CHECK(total > 10u);
  unsigned aborted = 0, finished = 0;
  for (unsigned stop = 0; stop <= total + 2; ++stop) {
    Rig r;
    r.ex().begin_app(kApp);
    CHECK(r.write_file("first", "mid", pattern(300, 7), 300) == O3M_EXPORT_DONE);  // a finished file, which must stay
    const Bytes data = pattern(40000, 1);
    unsigned frames_run = 0;
    CHECK(r.ex().open("gone", "mid") == O3M_EXPORT_OK);
    bool closed = false;
    for (std::size_t at = 0; at < data.size() && frames_run < stop;) {
      const std::size_t n = data.size() - at < 4096 ? data.size() - at : 4096;
      if (r.ex().write(data.data() + at, static_cast<std::uint32_t>(n)) == O3M_EXPORT_BUSY) {
        r.frame();
        ++frames_run;
        continue;
      }
      at += n;
      if (at >= data.size()) {
        closed = r.ex().close() == O3M_EXPORT_OK;
      }
    }
    while (closed && frames_run < stop && r.ex().status(nullptr, 0) == O3M_EXPORT_WRITING) {
      r.frame();
      ++frames_run;
    }
    r.ex().end_app();  // the app exits (or faults, or is stopped for its budget) here
    r.frames(3);
    INFO("stop after " << stop << " frames");
    CHECK_FALSE(r.has_temp());
    CHECK(r.card.violations == 0);
    CHECK(r.card.same("AppExport/smfexport/first.mid", pattern(300, 7)));
    const Bytes* gone = r.card.get("AppExport/smfexport/gone.mid");
    if (gone != nullptr) {
      CHECK(*gone == data);  // finished before the abort: whole
      ++finished;
    } else {
      ++aborted;
    }
    for (const std::string& rem : r.card.removed()) {
      CHECK(rem == "AppExport/smfexport/_o3save.tmp");
    }
    CHECK(r.ex().status(nullptr, 0) == O3M_EXPORT_IDLE);
    // and the next launch works
    r.ex().begin_app(kApp);
    CHECK(r.write_file("next", "mid", pattern(10, 1), 10) == O3M_EXPORT_DONE);
  }
  CHECK(aborted > 5u);
  CHECK(finished > 0u);
}

TEST_CASE("app export abort: an app that exits with a file open leaves nothing under AppExport but the folders; the status of the next launch starts clean") {
  Rig r;
  r.ex().begin_app(kApp);
  CHECK(r.ex().open("half", "syx") == O3M_EXPORT_OK);
  const Bytes data = pattern(1000, 1);
  CHECK(r.ex().write(data.data(), 1000) == O3M_EXPORT_OK);
  r.frames(10);
  CHECK(r.has_temp());  // the Temp exists while the app writes
  r.ex().end_app();
  CHECK_FALSE(r.has_temp());
  CHECK(r.card.files.empty());
  r.ex().begin_app("other");
  CHECK(r.ex().status(nullptr, 0) == O3M_EXPORT_IDLE);
  CHECK(r.ex().open("n", "txt") == O3M_EXPORT_OK);  // the file count and the state started again
  CHECK(r.card.violations == 0);
}

TEST_CASE("app export abort: a user's save while a file is being written is never refused: the export is FAILED, its Temp is removed and the save completes") {
  Rig r;
  r.ex().begin_app(kApp);
  const Bytes data = pattern(30000, 1);
  CHECK(r.ex().open("wait", "mid") == O3M_EXPORT_OK);
  CHECK(r.ex().write(data.data(), 8000) == O3M_EXPORT_OK);
  r.frames(10);
  CHECK(r.has_temp());
  o3os::model::ModelArena& arena = o3os::test::arena(1);
  o3os::test::fill_small(arena.project);
  CHECK(r.svc.save_project(r.jobs, arena.project, "", true) == app::SaveService::Start::Started);  // not Busy
  CHECK(r.ex().status(nullptr, 0) == O3M_EXPORT_FAILED);
  CHECK(r.ex().write(data.data(), 10) == O3M_EXPORT_STATE);
  for (unsigned i = 0; i < 4000 && r.svc.busy(); ++i) {
    r.frame();
  }
  CHECK_FALSE(r.has_temp());
  CHECK(r.card.has("Projects/Track 01/project.o3pj"));
  CHECK_FALSE(r.card.has("AppExport/smfexport/wait.mid"));
  CHECK(r.card.violations == 0);
  // the export layer works again afterwards
  CHECK(r.write_file("again", "mid", pattern(20, 2), 20) == O3M_EXPORT_DONE);
}

TEST_CASE("app export: a file left open with no write is given up after kIdleMs (FAILED, no Temp, the service is free); a file that is written to, or drained, or closed, is not") {
  {
    Rig r;
    r.ex().begin_app(kApp);
    CHECK(r.ex().open("idle", "mid") == O3M_EXPORT_OK);
    CHECK(r.svc.busy());  // an open file holds the job slot
    const Bytes data = pattern(10, 1);
    CHECK(r.ex().write(data.data(), 10) == O3M_EXPORT_OK);
    const unsigned frames_idle = app::AppExport::kIdleMs / 16u;
    r.frames(frames_idle - 100u);
    CHECK(r.ex().write(data.data(), 10) == O3M_EXPORT_OK);  // still open, and a write is progress
    r.frames(frames_idle - 100u);
    CHECK(r.ex().write(data.data(), 10) == O3M_EXPORT_OK);
    r.frames(frames_idle + 100u);  // no write for longer than kIdleMs
    CHECK(r.ex().status(nullptr, 0) == O3M_EXPORT_FAILED);
    CHECK(r.ex().write(data.data(), 10) == O3M_EXPORT_STATE);
    r.frames(50);
    CHECK_FALSE(r.svc.busy());
    CHECK_FALSE(r.has_temp());
    CHECK_FALSE(r.card.has("AppExport/smfexport/idle.mid"));
    CHECK(r.card.violations == 0);
    // the next file works
    CHECK(r.write_file("next", "mid", pattern(30, 2), 30) == O3M_EXPORT_DONE);
  }
  {
    // a closed file is not timed: a slow card (here: nothing but time) lets it end by itself
    Rig r;
    r.ex().begin_app(kApp);
    CHECK(r.write_file("slow", "mid", pattern(5000, 3), 700) == O3M_EXPORT_DONE);
  }
  {
    // a full ring that the job is draining is progress: an app that keeps retrying BUSY is not given up
    Rig r;
    r.ex().begin_app(kApp);
    const Bytes data = pattern(app::AppExport::kRingBytes, 4);
    CHECK(r.ex().open("big", "mid") == O3M_EXPORT_OK);
    unsigned accepted = 0;
    for (unsigned i = 0; i < 4000 && accepted < 40; ++i) {
      if (r.ex().write(data.data(), static_cast<std::uint32_t>(data.size())) == O3M_EXPORT_OK) {
        ++accepted;
      }
      r.frame();
    }
    CHECK(accepted == 40u);
    CHECK(r.ex().close() == O3M_EXPORT_OK);
    CHECK(r.wait() == O3M_EXPORT_DONE);
  }
}

TEST_CASE("app export abort: a request of the user that is refused or does nothing leaves the export running; one that starts cancels it; a file already committing is let finish and the request waits") {
  const Bytes data = pattern(30000, 1);
  {
    Rig r;
    r.ex().begin_app(kApp);
    CHECK(r.ex().open("keep", "mid") == O3M_EXPORT_OK);
    CHECK(r.ex().write(data.data(), 8000) == O3M_EXPORT_OK);
    r.frames(10);
    CHECK(r.has_temp());
    // a request that does not start (a WAV save of no audio)
    app::WavRequest rq;  // pcm == nullptr: Empty
    CHECK(r.svc.save_wav(r.jobs, rq) == app::SaveService::Start::Empty);
    CHECK(r.ex().write(data.data(), 100) == O3M_EXPORT_OK);  // the export is still open and takes bytes
    CHECK(r.ex().status(nullptr, 0) == O3M_EXPORT_WRITING);
    o3os::model::ModelArena& arena = o3os::test::arena(1);
    o3os::test::fill_small(arena.project);
    CHECK(r.svc.save_project(r.jobs, arena.project, "", true) == app::SaveService::Start::Started);  // one that starts does cancel it
    CHECK(r.ex().status(nullptr, 0) == O3M_EXPORT_FAILED);
  }
  {
    // the file has begun to commit: the user's save answers Busy and the export ends DONE
    Rig r;
    r.ex().begin_app(kApp);
    const Bytes small = pattern(9000, 2);
    CHECK(r.ex().open("commit", "mid") == O3M_EXPORT_OK);
    CHECK(r.ex().write(small.data(), 9000) == O3M_EXPORT_OK);
    CHECK(r.ex().close() == O3M_EXPORT_OK);
    bool seen = false;
    for (unsigned i = 0; i < 2000 && !seen && r.svc.job().running(); ++i) {
      r.frame();
      seen = r.svc.job().past_write();
    }
    NEED(seen);
    o3os::model::ModelArena& arena = o3os::test::arena(1);
    o3os::test::fill_small(arena.project);
    CHECK(r.svc.save_project(r.jobs, arena.project, "", true) == app::SaveService::Start::Busy);
    CHECK(r.wait() == O3M_EXPORT_DONE);
    CHECK(r.card.same("AppExport/smfexport/commit.mid", small));
    CHECK(r.svc.save_project(r.jobs, arena.project, "", true) == app::SaveService::Start::Started);
  }
}

TEST_CASE("app export abort: a power cut at EVERY mutating call leaves no partial final file; the next export removes the stale Temp (the heal) and completes") {
  unsigned total_ops = 0;
  {
    Rig r;
    r.ex().begin_app(kApp);
    CHECK(r.write_file("cut", "mid", pattern(20000, 3), 5000) == O3M_EXPORT_DONE);
    total_ops = static_cast<unsigned>(r.card.ops);
  }
  CHECK(total_ops > 8u);
  unsigned failed = 0, done = 0;
  for (unsigned cut = 0; cut <= total_ops + 1; ++cut) {
    Rig r;
    r.card.put("README.TXT", std::string("readme"));
    r.card.cut_at = cut;
    r.ex().begin_app(kApp);
    std::string name;
    const std::uint32_t st = r.write_file("cut", "mid", pattern(20000, 3), 5000, &name);
    INFO("cut at call " << cut);
    const Bytes* final_file = r.card.get("AppExport/smfexport/cut.mid");
    if (st == O3M_EXPORT_DONE) {
      ++done;
      NEED(final_file != nullptr);
      CHECK(*final_file == pattern(20000, 3));
    } else {
      ++failed;
      CHECK((st == O3M_EXPORT_FAILED || st >= 0x100u));  // FAILED, or the open itself failed (the card was already gone)
      CHECK(final_file == nullptr);                      // nothing partial under the final name
    }
    CHECK(r.card.violations == 0);
    CHECK(r.card.same("README.TXT", Bytes{'r', 'e', 'a', 'd', 'm', 'e'}));
    r.ex().end_app();
    // next boot: the card is back, a new launch exports again; a Temp the cut left is removed by the heal
    r.card.revive();
    r.ex().begin_app(kApp);
    CHECK(r.write_file("cut", "mid", pattern(20000, 3), 5000, &name) == O3M_EXPORT_DONE);
    CHECK_FALSE(r.has_temp());
    const Bytes* again = r.card.get("AppExport/smfexport/cut.mid");
    NEED(again != nullptr);
    CHECK(*again == pattern(20000, 3));
    CHECK(r.card.violations == 0);
    for (const std::string& rem : r.card.removed()) {
      CHECK(rem == "AppExport/smfexport/_o3save.tmp");
    }
  }
  CHECK(failed > 5u);
  CHECK(done >= 1u);
}

TEST_CASE("app export: a card that fills up or fails a read-back is FAILED, no final file, no Temp") {
  {
    Rig r;
    r.ex().begin_app(kApp);
    r.card.space_left = 3000;
    CHECK(r.write_file("full", "mid", pattern(20000, 1), 2000) == O3M_EXPORT_FAILED);
    CHECK(r.card.files.empty());
    CHECK(r.card.violations == 0);
  }
  {
    Rig r;
    r.ex().begin_app(kApp);
    r.card.corrupt_readback = true;
    CHECK(r.write_file("bad", "mid", pattern(2000, 1), 2000) == O3M_EXPORT_FAILED);
    CHECK_FALSE(r.card.has("AppExport/smfexport/bad.mid"));
    CHECK_FALSE(r.has_temp());
  }
  {
    Rig r;
    r.ex().begin_app(kApp);
    r.card.files["appexport/smfexport/_o3save.tmp"] = pattern(10, 1);  // a stale Temp of a cut export
    r.card.dirs.insert("appexport");
    r.card.dirs.insert("appexport/smfexport");
    CHECK(r.write_file("heal", "mid", pattern(2000, 2), 2000) == O3M_EXPORT_DONE);
    CHECK_FALSE(r.has_temp());
    CHECK(r.card.same("AppExport/smfexport/heal.mid", pattern(2000, 2)));
    CHECK(r.card.removed() == std::vector<std::string>{"AppExport/smfexport/_o3save.tmp"});
  }
}

// ---- the fuzz ------------------------------------------------------------------------------------------------------------------------------------

namespace {

struct FuzzResult {
  unsigned done = 0, failed = 0, opens = 0, busy = 0, limits = 0, ranges = 0, states = 0, no_cards = 0;
};

// Random calls of the four functions with begin_app and end_app, in and out of an open file, on a card that is cut at random when `faults`. Returns the counts and checks the invariants as it goes.
FuzzResult fuzz(unsigned seed, unsigned steps, bool faults) {
  FuzzResult out;
  Rng rng(seed);
  Rig r;
  const bool rare_launch = (seed & 3u) == 0u;
  // What exists before, and must never change: files of the user's in places an app might be tempted by.
  const std::map<std::string, Bytes> sentinels = {
      {"projects/song/project.o3pj", pattern(50, 1)},   {"samples/mine.wav", pattern(60, 2)},      {"settings/device.o3st", pattern(30, 3)},
      {"export/song/song_song0001.wav", pattern(70, 4)}, {"appexport/smfexport/keep.mid", pattern(40, 5)}, {"appexport/smfexport/pat001.mid", pattern(45, 6)},
      {"appexport/other/keep.txt", pattern(20, 7)},     {"readme.txt", pattern(10, 8)},            {"firmware/old.ptf", pattern(25, 9)},
      {"modules/apps/smfexport.tmod", pattern(80, 10)},
  };
  for (const auto& s : sentinels) {
    r.card.files[s.first] = s.second;
  }
  std::set<std::string> seeded_dirs;
  for (const char* d : {"projects", "projects/song", "samples", "settings", "export", "export/song", "appexport", "appexport/smfexport", "appexport/other", "firmware", "modules", "modules/apps"}) {
    r.card.dirs.insert(d);
    seeded_dirs.insert(d);
  }
  const char* stems[] = {"smfexport", "other", "CON", "a b", "smfexport"};
  const char* names[] = {"PAT001", "keep", "pat001", "x y", "My_Song-2", "", "a/b", "..", "CON", "", "ok.name", "0123456789012345678901234567890", "01234567890123456789012345678901", "trail ", nullptr, "A"};
  const char* exts[] = {"mid", "syx", "txt", "csv", "json", "mid", "mid", "MID", "ptf", "tmod", "o3pj", "wav", "", nullptr, ".mid", "mid.ptf"};
  const Bytes pool = pattern(70000, 11);
  std::vector<Bytes> recorded;     // the bytes accepted into each file ever opened
  Bytes current;                   // ... into the one that is open now
  bool open_now = false;
  std::set<std::string> apps = {"smfexport", "other", "a b"};
  std::string app_now;
  const auto check_code = [&](std::int32_t c) {
    CHECK(c >= O3M_EXPORT_OK);
    CHECK(c <= O3M_EXPORT_NO_CARD);
    out.busy += c == O3M_EXPORT_BUSY ? 1u : 0u;
    out.limits += c == O3M_EXPORT_LIMIT ? 1u : 0u;
    out.ranges += c == O3M_EXPORT_RANGE ? 1u : 0u;
    out.states += c == O3M_EXPORT_STATE ? 1u : 0u;
    out.no_cards += c == O3M_EXPORT_NO_CARD ? 1u : 0u;
  };
  const auto check_card = [&]() {
    CHECK(r.card.violations == 0);
    for (const auto& s : sentinels) {
      const Bytes* b = r.card.get(s.first.c_str());
      CHECK_MESSAGE(b != nullptr, "a file of the user's is gone: " << s.first);
      if (b != nullptr) {
        CHECK_MESSAGE(*b == s.second, "a file of the user's changed: " << s.first);
      }
    }
    for (const auto& f : r.card.files) {
      if (sentinels.count(f.first) != 0) {
        continue;
      }
      // everything new is in AppExport/<an app of this run>/ and is a Temp or a name the guard knows
      CHECK_MESSAGE(f.first.rfind("appexport/", 0) == 0, "a file outside AppExport: " << f.first);
      const std::size_t slash = f.first.find('/', 10);
      CHECK_MESSAGE((slash != std::string::npos && apps.count(f.first.substr(10, slash - 10)) == 1), "a file outside an app folder: " << f.first);
      CHECK_MESSAGE(f.first.find('/', slash + 1) == std::string::npos, "a file deeper than the app folder: " << f.first);
      CHECK(f.second.size() <= app::AppExport::kMaxFileBytes);
    }
    for (const std::string& d : r.card.dirs) {
      if (seeded_dirs.count(d) != 0) {
        continue;
      }
      CHECK_MESSAGE(d.rfind("appexport/", 0) == 0, "a folder made outside AppExport: " << d);
      CHECK_MESSAGE((d.size() > 10 && apps.count(d.substr(10)) == 1), "a folder outside the apps: " << d);
    }
    for (const std::string& rem : r.card.removed()) {
      CHECK_MESSAGE((rem.size() >= 11 && rem.compare(rem.size() - 11, 11, "_o3save.tmp") == 0), "removed: " << rem);
    }
  };
  for (unsigned i = 0; i < steps; ++i) {
    const unsigned pick = rng.below(faults ? 26 : 25);
    // open 4/25, write 7, close 3, status 3, frames 4, begin_app 1, end_app 1 (a quarter of the time it is a begin_app first), a power cut / revive 1 when the card may fail
    const unsigned op = pick == 0 ? 0 : (pick == 1 ? 2 : (pick <= 5 ? 3 : (pick <= 12 ? 6 : (pick <= 15 ? 9 : (pick <= 18 ? 10 : (pick <= 23 ? 11 : (pick == 24 ? 3 : 13)))))));
    const bool launch_op = op == 0 || op == 1 || op == 2;
    if (launch_op && rare_launch && !app_now.empty() && rng.below(30) != 0) {
      continue;  // (this run: an app that runs long enough to reach the 16 files of a launch)
    }
    switch (op) {
      case 0:
      case 1: {  // begin_app (sometimes with a stem the guard refuses)
        const char* stem = stems[rng.below(5)];
        if (open_now) {
          // a launch while a file is open is the host ending the previous app first
          r.ex().end_app();
          open_now = false;
        }
        r.ex().begin_app(stem);
        app_now = stem;
        break;
      }
      case 2:
        r.ex().end_app();
        open_now = false;
        app_now.clear();
        break;
      case 3:
      case 4:
      case 5: {  // open
        const bool good = rng.below(10) < 7;  // mostly names and extensions that are fine, so that files get made
        const char* const name = good ? names[rng.below(4)] : names[rng.below(16)];
        const char* const ext = good ? exts[rng.below(5)] : exts[rng.below(16)];
        const std::int32_t c = r.ex().open(name, ext);
        check_code(c);
        if (c == O3M_EXPORT_OK) {
          ++out.opens;
          open_now = true;
          current.clear();
        }
        break;
      }
      case 6:
      case 7:
      case 8: {  // write
        static const unsigned sizes[] = {0, 1, 2, 5, 100, 1000, 4096, 16383, 16384, 16385, 40000, 70000};
        const unsigned n = rng.below(6) == 0 ? sizes[rng.below(12)] : 1 + rng.below(3000);
        const unsigned off = rng.below(4000);
        const std::int32_t c = r.ex().write(rng.below(40) == 0 ? nullptr : pool.data() + off, n);
        check_code(c);
        if (c == O3M_EXPORT_OK) {
          current.insert(current.end(), pool.begin() + off, pool.begin() + off + n);
        }
        break;
      }
      case 9: {  // close
        const std::int32_t c = r.ex().close();
        check_code(c);
        if (c == O3M_EXPORT_OK) {
          recorded.push_back(current);
          open_now = false;
        }
        break;
      }
      case 10: {  // status
        char buf[48] = {};
        const bool use = rng.below(2) == 0;
        const std::uint32_t cap = rng.below(2) == 0 ? sizeof buf : rng.below(8);
        const std::uint32_t s = r.ex().status(use ? buf : nullptr, cap);
        CHECK(s <= O3M_EXPORT_FAILED);
        out.done += s == O3M_EXPORT_DONE ? 1u : 0u;
        out.failed += s == O3M_EXPORT_FAILED ? 1u : 0u;
        if (s == O3M_EXPORT_DONE && use && cap >= 2u) {
          CHECK(buf[0] != '\0');
          CHECK(std::strlen(buf) < cap);
        }
        break;
      }
      case 11:
      case 12:
        r.frames(1 + rng.below(6));
        break;
      default: {  // a power cut ahead, and a later revive
        if (r.card.cut_at == ~std::size_t{0}) {
          r.card.cut_at = r.card.ops + rng.below(30);
        } else {
          r.card.revive();
        }
        break;
      }
    }
    check_card();
  }
  // the end: the app leaves, the card is back, the loop settles
  r.ex().end_app();
  r.card.revive();
  r.frames(20);
  check_card();
  if (!faults) {
    CHECK_FALSE(r.has_temp());
  }
  // every final file under AppExport that is not a sentinel is whole: it is exactly the bytes some file was closed with
  for (const auto& f : r.card.files) {
    if (sentinels.count(f.first) != 0 || (f.first.size() >= 11 && f.first.compare(f.first.size() - 11, 11, "_o3save.tmp") == 0)) {
      continue;
    }
    bool matches = false;
    for (const Bytes& b : recorded) {
      matches = matches || b == f.second;
    }
    CHECK_MESSAGE(matches, "a final file that is nothing an app closed: " << f.first << " (" << f.second.size() << " bytes)");
  }
  return out;
}

}  // namespace

TEST_CASE("app export fuzz: random calls with launches and exits, in and out of an open file: every code is published, nothing outside AppExport/<app>/, nothing of the user's changed, only Temps removed, every final file whole") {
  FuzzResult total;
  for (unsigned seed = 1; seed <= 120; ++seed) {
    const FuzzResult r = fuzz(0x5EED0000u + seed, (seed & 3u) == 0u ? 1500u : 400u, false);
    total.done += r.done;
    total.failed += r.failed;
    total.opens += r.opens;
    total.busy += r.busy;
    total.limits += r.limits;
    total.ranges += r.ranges;
    total.states += r.states;
    total.no_cards += r.no_cards;
  }
  CHECK(total.opens > 300u);
  CHECK(total.done > 60u);  // (the seeds are fixed: these floors only say that the run did reach the paths it is for)
  CHECK(total.busy > 20u);
  CHECK(total.ranges > 300u);
  CHECK(total.states > 100u);
  CHECK(total.limits > 0u);
}

TEST_CASE("app export fuzz with power cuts: the same invariants on a card that is cut and revived at random") {
  FuzzResult total;
  for (unsigned seed = 1; seed <= 120; ++seed) {
    const FuzzResult r = fuzz(0xC0DE0000u + seed, 400, true);
    total.done += r.done;
    total.failed += r.failed;
    total.opens += r.opens;
  }
  CHECK(total.opens > 300u);
  CHECK(total.failed > 10u);
  CHECK(total.done > 40u);
}
