// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-QANET D2: the slow-card model (platform/host/slow_card.h; PROVISIONAL, as budgets.cpp is: the numbers are the unit's block bench until OS-QANET D4 measures the unit): a wrapper over any card that charges latency plus bytes over the rate to the simulated clock, in the pass where the call happens. What these
// tests hold down: reading 1 MB through the model moves the clock by the modelled time (within 1 ms); the model off charges nothing and answers exactly as the card does; sub-microsecond parts are carried, not
// lost; the scenario line and the numbers are checked; in the simulator the clock is the ticks plus the charged time, to the microsecond, and the audio engine renders one block for each millisecond of it.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/hal/storage.h>
#include <o3os/platform/host/dir_card.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/platform/host/slow_card.h>
#include <o3os/sim/scenario.h>
#include <o3os/sim/sim_audio.h>
#include <o3os/sim/simulator.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace hs = o3os::hal::storage;
using namespace o3os;
using platform::host::CardClock;
using platform::host::CardTiming;
using platform::host::DirCard;
using platform::host::ScratchDir;
using platform::host::SlowCard;

namespace {

struct ClockSink final : CardClock {
  platform::host::FakeClock clock;
  void card_time(std::uint32_t us) noexcept override { clock.advance(us); }
};

constexpr std::size_t kMegabyte = 1024u * 1024u;

void make_file(const fs::path& path, std::size_t bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  const std::string chunk(4096, 'x');
  for (std::size_t done = 0; done < bytes; done += chunk.size()) {
    out.write(chunk.data(), static_cast<std::streamsize>(std::min(chunk.size(), bytes - done)));
  }
}

// Reads the whole file in `chunk`-byte calls; returns the bytes read.
std::size_t read_all(hs::Storage& card, const char* path, std::size_t chunk) {
  hs::FileHandle file;
  const bool opened = card.open(path, file) == hs::Status::Ok;
  CHECK(opened);
  if (!opened) {
    return 0;
  }
  std::vector<std::uint8_t> buf(chunk);
  std::size_t total = 0;
  for (;;) {
    std::size_t got = 0;
    const bool ok = card.read(file, buf.data(), buf.size(), got) == hs::Status::Ok;
    CHECK(ok);
    if (!ok || got == 0u) {
      break;
    }
    total += got;
  }
  card.close(file);
  return total;
}

bool run_on(sim::Simulator& simulator, const std::string& text) {
  std::vector<sim::ScenarioCommand> cmds;
  std::string error;
  const bool ok = sim::parse_scenario(text, cmds, error) && sim::run_scenario(cmds, simulator, [](const std::string&, const sim::Simulator&) { return true; }, error);
  CHECK_MESSAGE(ok, error);
  return ok;
}

}  // namespace

TEST_CASE("slow card: 1 MB read through the model moves the clock by the modelled time, within 1 ms") {
  ScratchDir dir("slowcard");
  NEED(dir.ok());
  make_file(fs::path(dir.path()) / "Samples" / "big.bin", kMegabyte);
  DirCard inner(dir.path());
  NEED(inner.mount() == hs::Status::Ok);
  CardTiming timing;
  timing.on = true;
  timing.read_bytes_per_ms = 20000;  // numbers of the test's own: the model, not the defaults
  timing.latency_us = 300;
  ClockSink sink;
  SlowCard card(inner, timing, sink);

  const std::size_t chunk = 16384;
  CHECK(read_all(card, "Samples/big.bin", chunk) == kMegabyte);
  // The test states the model on its own: the open walks two path components (two sectors), then 64 reads of 16 KiB, and a 65th that returns 0 bytes (the end: a call that moves nothing charges nothing).
  const double open_us = 2.0 * (300.0 + 512.0 * 1000.0 / 20000.0);
  const double reads_us = 64.0 * 300.0 + static_cast<double>(kMegabyte) * 1000.0 / 20000.0;
  const double modelled_us = open_us + reads_us;
  const double advanced_us = static_cast<double>(sink.clock.uptime_us());
  CHECK(advanced_us > modelled_us - 1.0);  // (exact to the whole microsecond that is carried; the bound the spec states is 1 ms)
  CHECK(advanced_us < modelled_us + 1.0);
  CHECK(advanced_us > modelled_us - 1000.0);
  CHECK(advanced_us < modelled_us + 1000.0);
  CHECK(card.charged_us() == sink.clock.uptime_us());  // the card's own count is the clock's
  CHECK(advanced_us > 50000.0);                        // and it is a real time: about 77 ms for the megabyte
}

TEST_CASE("slow card: with the model off nothing is charged and every answer is the card's") {
  ScratchDir dir("slowcard-off");
  NEED(dir.ok());
  make_file(fs::path(dir.path()) / "a.bin", 100000);
  DirCard inner(dir.path());
  NEED(inner.mount() == hs::Status::Ok);
  CardTiming timing;  // on is false
  ClockSink sink;
  SlowCard card(inner, timing, sink);
  CHECK(read_all(card, "a.bin", 4096) == 100000u);
  bool is_dir = true;
  CHECK(card.exists("a.bin", &is_dir) == hs::Status::Ok);
  CHECK_FALSE(is_dir);
  CHECK(card.exists("missing.bin", &is_dir) == hs::Status::NotFound);
  CHECK(card.mounted());
  CHECK(card.card_present());
  CHECK(sink.clock.uptime_us() == 0u);
  CHECK(card.charged_us() == 0u);
}

TEST_CASE("slow card: parts of a microsecond are carried, not lost, and a listing charges a sector for each sixteen slots") {
  ScratchDir dir("slowcard-carry");
  NEED(dir.ok());
  make_file(fs::path(dir.path()) / "a.bin", 1);
  for (unsigned i = 0; i < 40; ++i) {
    make_file(fs::path(dir.path()) / "List" / ("f" + std::to_string(i) + ".bin"), 1);
  }
  DirCard inner(dir.path());
  NEED(inner.mount() == hs::Status::Ok);
  CardTiming timing;
  timing.on = true;
  timing.read_bytes_per_ms = 1000000;  // 1 ns a byte: a read of 1 byte is a tenth of a microsecond with no latency, so only the carry adds it up
  timing.latency_us = 0;
  ClockSink sink;
  SlowCard card(inner, timing, sink);
  hs::FileHandle file;
  NEED(card.open("a.bin", file) == hs::Status::Ok);  // one sector: 512 ns
  std::uint8_t byte = 0;
  std::size_t got = 0;
  for (unsigned i = 0; i < 2000; ++i) {
    NEED(card.seek(file, 0) == hs::Status::Ok);  // 512 ns each
    NEED(card.read(file, &byte, 1, got) == hs::Status::Ok);  // 1 ns each
  }
  card.close(file);
  CHECK(sink.clock.uptime_us() == (512u + 2000u * (512u + 1u)) / 1000u);  // the sum, in whole microseconds: no call rounded up on its own

  CardTiming slow;
  slow.on = true;
  slow.read_bytes_per_ms = 512;  // a sector is a millisecond
  slow.latency_us = 1000;        // and a call 1 ms on top
  ClockSink sink2;
  SlowCard card2(inner, slow, sink2);
  hs::DirHandle d;
  NEED(card2.opendir("List", d) == hs::Status::Ok);  // 1 component: 2 ms
  hs::DirEntry e;
  unsigned n = 0;
  while (card2.readdir(d, e) == hs::Status::Ok) {
    ++n;
  }
  card2.closedir(d);
  CHECK(n == 40u);
  CHECK(sink2.clock.uptime_us() == (2000u + 5u * 2000u));  // the walk to "List" (one sector), then 40 entries of 2 slots each (the name "fNN.bin" is one long-name slot and the short entry): 80 slots, 5 sectors
}

TEST_CASE("slow card: long names take more slots, and a name late in a folder costs the sectors before it") {
  ScratchDir dir("slowcard-slots");
  NEED(dir.ok());
  // 20 entries of a 30-character name: ceil(30/13)+1 = 4 slots each, 80 slots, 5 sectors
  std::vector<std::string> names;
  for (unsigned i = 0; i < 20; ++i) {
    char n[48];
    std::snprintf(n, sizeof n, "long_sample_name_number_%02u.wav", i);  // 30 characters
    names.push_back(n);
    make_file(fs::path(dir.path()) / "L" / n, 1);
  }
  DirCard inner(dir.path());
  NEED(inner.mount() == hs::Status::Ok);
  CardTiming slow;
  slow.on = true;
  slow.read_bytes_per_ms = 512;  // a sector is a millisecond
  slow.latency_us = 1000;        // and a call 1 ms on top: a sector is 2 ms
  ClockSink sink;
  SlowCard card(inner, slow, sink);
  hs::DirHandle d;
  NEED(card.opendir("L", d) == hs::Status::Ok);
  hs::DirEntry e;
  unsigned n = 0;
  while (card.readdir(d, e) == hs::Status::Ok) {
    ++n;
  }
  card.closedir(d);
  CHECK(n == 20u);
  CHECK(sink.clock.uptime_us() == 2000u + 5u * 2000u);

  // `open` walks the folder to the name: the first entry of the folder's order is in sector 0, the last in sector 4 (its slots 76..79), whatever order the inner card lists in.
  std::vector<std::string> order;
  {
    hs::DirHandle h;
    NEED(inner.opendir("L", h) == hs::Status::Ok);
    while (inner.readdir(h, e) == hs::Status::Ok) {
      order.push_back(e.name);
    }
    inner.closedir(h);
  }
  NEED(order.size() == 20u);
  const std::uint64_t t0 = sink.clock.uptime_us();
  hs::FileHandle f;
  NEED(card.open(("L/" + order.front()).c_str(), f) == hs::Status::Ok);
  card.close(f);
  const std::uint64_t first_us = sink.clock.uptime_us() - t0;
  CHECK(first_us == 2000u + 2000u);  // "L" in the root (one sector), the name in the first sector of L
  const std::uint64_t t1 = sink.clock.uptime_us();
  NEED(card.open(("L/" + order.back()).c_str(), f) == hs::Status::Ok);
  card.close(f);
  CHECK(sink.clock.uptime_us() - t1 == 2000u + 5u * 2000u);  // the last name: the five sectors of L
  // a name that is not there reads the whole folder and the walk ends
  const std::uint64_t t2 = sink.clock.uptime_us();
  bool is_dir = false;
  CHECK(card.exists("L/missing.wav", &is_dir) == hs::Status::NotFound);
  CHECK(sink.clock.uptime_us() - t2 == 2000u + 5u * 2000u);
}

TEST_CASE("slow card: the scenario line parses, and refuses what it cannot take") {
  std::vector<sim::ScenarioCommand> cmds;
  std::string error;
  NEED(sim::parse_scenario("card slow 17700 257\ncard slow 500 2000 300\ncard slow off\ncard slow\n", cmds, error));
  NEED(cmds.size() == 4u);
  CHECK((cmds[0].value == 4u && cmds[0].name == "17700" && cmds[0].args[0] == "257" && cmds[0].args[1] == std::to_string(CardTiming::kDefaultWriteBytesPerMs)));
  CHECK((cmds[1].value == 4u && cmds[1].name == "500" && cmds[1].args[0] == "2000" && cmds[1].args[1] == "300"));
  CHECK(cmds[2].value == 5u);
  CHECK((cmds[3].value == 0u && cmds[3].name == "slow"));  // `card slow` alone is a fixture folder of that name
  CHECK_FALSE(sim::parse_scenario("card slow 0 5\n", cmds, error));
  CHECK(error.find("line 1: card slow needs <bytes-per-ms>") == 0u);
  CHECK_FALSE(sim::parse_scenario("card slow 5\n", cmds, error));
  CHECK_FALSE(sim::parse_scenario("card slow 5 x\n", cmds, error));
  CHECK_FALSE(sim::parse_scenario("card slow off 3\n", cmds, error));
}

TEST_CASE("slow card: in the simulator the clock is the ticks plus the charged time, and the audio renders a block for each millisecond of it") {
  ScratchDir fixture("slowcard-sim");
  NEED(fixture.ok());
  make_file(fs::path(fixture.path()) / "Samples" / "a.bin", 1000);
  const std::string card = "document sound\ncard \"" + fixture.path() + "\"\n";
  {
    // off: the clock is the ticks, to the microsecond, and nothing is charged
    auto simulator = std::make_unique<sim::Simulator>();
    NEED(run_on(*simulator, card + "wait 20\ntap file\nwait 100\n"));
    CHECK(simulator->card_charged_us() == 0u);
    CHECK(simulator->uptime_us() % sim::Simulator::kTickUs == 0u);
  }
  {
    // on, audio on: a 1 ms read latency per sector makes the File screen's listing take real time
    auto simulator = std::make_unique<sim::Simulator>();
    NEED(run_on(*simulator, "card slow 100000 1000\n" + card + "audio on\nwait 20\n"));
    CHECK(simulator->card_timing().on);
    const std::uint64_t before_us = simulator->uptime_us();
    const std::uint64_t charged_before = simulator->card_charged_us();
    const std::uint64_t blocks_before = simulator->audio()->blocks_rendered();
    NEED(run_on(*simulator, "tap file\nwait 100\n"));
    const std::uint64_t charged = simulator->card_charged_us() - charged_before;
    CHECK(charged >= 1000u);  // File reads the card's folder: at least one sector
    // the ticks the scenario ran (a whole number of them, at least the 100 waited) plus what the card took, to the microsecond
    const std::uint64_t ticks_us = simulator->uptime_us() - before_us - charged;
    CHECK(ticks_us % sim::Simulator::kTickUs == 0u);
    CHECK(ticks_us >= 102u * sim::Simulator::kTickUs);
    // the engine rendered the blocks of the whole clock (689.06 a second: 64 frames at 44,117.647 Hz), the card's time included, to within a block or two
    const std::uint64_t blocks = simulator->audio()->blocks_rendered() - blocks_before;
    const std::uint64_t ms = (simulator->uptime_us() - before_us) / sim::Simulator::kTickUs;
    CHECK(blocks + 2u >= ms * 689u / 1000u);
    CHECK(blocks <= ms * 689u / 1000u + 2u);
    CHECK(ms >= 102u + charged / sim::Simulator::kTickUs);  // (the card's time is in it)

    // `card slow off` turns it off again: the rest of the run is charged nothing
    const std::uint64_t charged_total = simulator->card_charged_us();
    NEED(run_on(*simulator, "card slow off\ntap pattern\ntap file\nwait 100\n"));
    CHECK(simulator->card_charged_us() == charged_total);
  }
}

TEST_CASE("slow card: the simulator refuses a rate of 0, and off keeps the numbers") {
  auto simulator = std::make_unique<sim::Simulator>();
  std::string error;
  CHECK_FALSE(simulator->set_card_timing(true, 0, 10, 10, error));
  CHECK(error.find("0 bytes a millisecond") != std::string::npos);
  CHECK(simulator->set_card_timing(true, 1000, 10, 10, error));
  CHECK(simulator->card_timing().read_bytes_per_ms == 1000u);
  CHECK(simulator->set_card_timing(false, 0, 0, 0, error));
  CHECK_FALSE(simulator->card_timing().on);
  CHECK(simulator->card_timing().read_bytes_per_ms == 1000u);  // off keeps the numbers; `on` is the switch
}

TEST_CASE("slow card: a full card refuses what would grow it, reads and deletes still work, off restores (OS-QANET B8)") {
  ScratchDir dir("slowcard-full");
  NEED(dir.ok());
  fs::create_directories(fs::path(dir.path()) / "Projects");  // (the guard's folder; the card makes only the project folders in it)
  DirCard inner(dir.path());
  NEED(inner.mount() == hs::Status::Ok);
  CardTiming timing;  // the timing model stays off: the full switch is independent of it
  ClockSink sink;
  SlowCard card(inner, timing, sink);
  const fs::path song = fs::path(dir.path()) / "Projects" / "Song";
  const auto save = [&](const char* text) {
    CHECK(card.create("Projects/Song/_o3save.tmp") == hs::WriteStatus::Ok);
    CHECK(card.write(text, 3) == hs::WriteStatus::Ok);
    CHECK(card.finish() == hs::WriteStatus::Ok);
  };
  NEED(card.make_dir("Projects/Song") == hs::WriteStatus::Ok);
  save("one");
  NEED(card.rename("Projects/Song/_o3save.tmp", "Projects/Song/project.o3pj") == hs::WriteStatus::Ok);

  timing.full = true;
  CHECK(card.make_dir("Projects/Other") == hs::WriteStatus::NoSpace);
  CHECK(card.create("Projects/Song/_o3save.tmp") == hs::WriteStatus::NoSpace);
  CHECK(card.create_marker("Projects/Song/_o3save.tmp") == hs::WriteStatus::NoSpace);
  CHECK_FALSE(card.writing());
  CHECK_FALSE(fs::exists(fs::path(dir.path()) / "Projects" / "Other"));
  CHECK_FALSE(fs::exists(song / "_o3save.tmp"));
  // reads work, and the old file is as it was
  CHECK(read_all(card, "Projects/Song/project.o3pj", 16) == 3u);
  // a file that was open when the card filled cannot grow
  timing.full = false;
  NEED(card.create("Projects/Song/_o3save.tmp") == hs::WriteStatus::Ok);
  NEED(card.write("ab", 2) == hs::WriteStatus::Ok);
  timing.full = true;
  CHECK(card.write("cd", 2) == hs::WriteStatus::NoSpace);
  CHECK(fs::file_size(song / "_o3save.tmp") == 2u);
  card.abandon();
  // deletes still work on a full card: that is how room is made
  CHECK(card.remove("Projects/Song/_o3save.tmp") == hs::WriteStatus::Ok);
  CHECK_FALSE(fs::exists(song / "_o3save.tmp"));

  // off: writes go through again
  timing.full = false;
  CHECK(card.make_dir("Projects/Other") == hs::WriteStatus::Ok);
  save("two");
  CHECK(card.rename("Projects/Song/project.o3pj", "Projects/Song/project.o3pj.bak") == hs::WriteStatus::Ok);  // (the replacing save: the old Target becomes its Backup)
  CHECK(card.rename("Projects/Song/_o3save.tmp", "Projects/Song/project.o3pj") == hs::WriteStatus::Ok);
  CHECK(read_all(card, "Projects/Song/project.o3pj", 16) == 3u);
}

TEST_CASE("slow card: `card full` and `card full off` parse, take nothing else, and set the simulator's switch (OS-QANET B8)") {
  std::vector<sim::ScenarioCommand> cmds;
  std::string error;
  NEED(sim::parse_scenario("card full\ncard FULL off\ncard slow 5 5\ncard full\n", cmds, error));
  NEED(cmds.size() == 4u);
  CHECK(cmds[0].value == 6u);
  CHECK(cmds[1].value == 7u);
  CHECK(cmds[2].value == 4u);  // independent of `card slow`
  CHECK_FALSE(sim::parse_scenario("card full on\n", cmds, error));
  CHECK(error.find("line 1: card full takes nothing") == 0u);
  CHECK_FALSE(sim::parse_scenario("card full off 3\n", cmds, error));
  auto simulator = std::make_unique<sim::Simulator>();
  CHECK_FALSE(simulator->card_timing().full);
  NEED(run_on(*simulator, "card full\n"));  // needs no card yet
  CHECK(simulator->card_timing().full);
  CHECK_FALSE(simulator->card_timing().on);
  NEED(run_on(*simulator, "card slow 1000 10\ncard full off\n"));
  CHECK_FALSE(simulator->card_timing().full);
  CHECK(simulator->card_timing().on);
}
