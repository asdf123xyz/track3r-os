// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-012a: the file tools/o3m_pack.py makes from a real module (modules/sine, built by arm-none-eabi-g++ with the SDK's flags) against the loader. The
// oracle of the relocation: the same module linked a second time AT the addresses the loader is asked to relocate to (hot part 0x1FFFC000, cold part
// 0x70000000, `o3m_pack.py build --hot-at --cold-at`), which is what the linker itself says every byte must be. The loader works on the file linked at 0;
// its output equals the oracle byte for byte, the hot part and the cold part. The descriptor then reads back as the module's source says.
// Needs the cross compiler to have built the fixtures (CMake target o3m_sine_tmod / o3m_sine_raw); without them the tests say so and pass, unless
// O3_CI=1 (a CI run must not go quiet).
#include <doctest/doctest.h>

// The build has no exceptions, so no REQUIRE: a failed precondition is recorded and ends the test case.
#define NEED(cond) \
  do {             \
    const bool need_ok_ = static_cast<bool>(cond); \
    CHECK(need_ok_); \
    if (!need_ok_) {  \
      return;         \
    }                 \
  } while (0)

#include "support/module_blob.h"

#include <o3os/module/loader.h>
#include <o3os/platform/host/dir_storage.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using namespace o3os::module;
using namespace o3os::module::test;
namespace fs = std::filesystem;

std::vector<std::uint8_t> slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

struct Fixtures {
  std::vector<std::uint8_t> tmod, raw;
  bool present = false;
};

Fixtures fixtures_of(const char* name) {
  Fixtures f;
#ifdef O3M_FIXTURE_DIR
  f.tmod = slurp(std::string(O3M_FIXTURE_DIR) + "/" + name + ".tmod");
  f.raw = slurp(std::string(O3M_FIXTURE_DIR) + "/" + name + ".raw");
  f.present = !f.tmod.empty() && !f.raw.empty();
#endif
  if (!f.present) {
    const char* ci = std::getenv("O3_CI");
    if (ci != nullptr && ci[0] == '1') {
      CHECK_MESSAGE(false, "the module fixtures were not built (no arm-none-eabi-g++ for the host build) and O3_CI=1");
    }
    MESSAGE("module fixtures not built (no arm-none-eabi-g++): skipped");
  }
  return f;
}
Fixtures sine_fixtures() { return fixtures_of("sine"); }

}  // namespace

TEST_CASE("module fixture: the packer's file passes the header checks and its CRC") {
  const Fixtures f = sine_fixtures();
  if (!f.present) {
    return;
  }
  NEED(f.tmod.size() >= sizeof(o3m_file_header));
  o3m_file_header h;
  std::memcpy(&h, f.tmod.data(), sizeof h);
  CHECK(check_header(h, static_cast<std::uint32_t>(f.tmod.size())) == Status::Ok);
  CHECK(h.kind == O3M_KIND_INSTRUMENT);
  CHECK(h.param_count == 2);
  CHECK(h.hot_size > 0);
  CHECK(h.hot_size + (f.raw.size() - h.hot_size) == f.raw.size());
  CHECK(h.image_size == f.raw.size());
  // the CRC: the loader's own function over the file, as the packer defines it
  std::vector<std::uint8_t> copy = f.tmod;
  std::memset(copy.data() + offsetof(o3m_file_header, image_crc), 0, 4);
  CHECK(crc32_update(0, copy.data(), copy.size()) == h.image_crc);
}

TEST_CASE("module fixture: relocated by the loader, the module equals the same module linked at those addresses") {
  const Fixtures f = sine_fixtures();
  if (!f.present) {
    return;
  }
  o3m_file_header h;
  std::memcpy(&h, f.tmod.data(), sizeof h);
  NEED(check_header(h, static_cast<std::uint32_t>(f.tmod.size())) == Status::Ok);
  alignas(64) static std::uint8_t hot[4096];
  alignas(64) static std::uint8_t cold[4096];
  NEED(h.hot_size <= sizeof hot);
  NEED(h.image_size - h.hot_size <= sizeof cold);
  Placement p{hot, kHotAddr, cold, kColdAddr};
  std::memcpy(hot, f.tmod.data() + sizeof h, h.hot_size);
  std::memcpy(cold, f.tmod.data() + sizeof h + h.hot_size, h.image_size - h.hot_size);
  const std::uint32_t* table = reinterpret_cast<const std::uint32_t*>(f.tmod.data() + sizeof h + h.image_size);
  std::vector<std::uint32_t> relocs(h.reloc_count);
  std::memcpy(relocs.data(), table, 4u * h.reloc_count);
  Relocator r(h, p);
  NEED(r.apply(relocs.data(), h.reloc_count) == Status::Ok);
  CHECK(r.complete());
  CHECK(h.reloc_count > 0);
  // byte for byte: the linker's own answer
  CHECK(std::memcmp(hot, f.raw.data(), h.hot_size) == 0);
  CHECK(std::memcmp(cold, f.raw.data() + h.hot_size, h.image_size - h.hot_size) == 0);
  Desc32 d{};
  CHECK(read_desc(h, p, d) == Status::Ok);
  CHECK(d.kind == O3M_KIND_INSTRUMENT);
  CHECK(std::string(reinterpret_cast<const char*>(resolve(h, p, d.name, 5))) == "Sine");
  Param32 q{};
  NEED(read_param(h, p, d, 0, q));
  CHECK(std::string(reinterpret_cast<const char*>(resolve(h, p, q.name, 6))) == "Level");
  CHECK(q.def == 0.5f);
  NEED(read_param(h, p, d, 1, q));
  CHECK(q.flags == O3M_PARAM_LOG);
  // the render function is in the hot part (O3M_HOT), the others in the cold part
  CHECK(d.render >= kHotAddr);
  CHECK(d.render < kHotAddr + h.hot_size);
  CHECK(d.init >= kColdAddr);
  CHECK(d.init < kColdAddr + (h.image_size - h.hot_size));
}

TEST_CASE("module fixture: loaded from a card, and loaded again after clear() into the same place") {
  const Fixtures f = sine_fixtures();
  if (!f.present) {
    return;
  }
  const fs::path root = fs::temp_directory_path() / ("o3m_fixture_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(&f)));
  fs::remove_all(root);
  fs::create_directories(root / "modules" / "instruments");
  {
    std::ofstream out(root / "modules" / "instruments" / "sine.tmod", std::ios::binary);
    out.write(reinterpret_cast<const char*>(f.tmod.data()), static_cast<std::streamsize>(f.tmod.size()));
  }
  o3os::platform::host::DirStorage card(root.string());
  NEED(card.mount() == o3os::hal::storage::Status::Ok);
  alignas(64) static std::uint8_t slab[1024];
  alignas(64) static std::uint8_t sdram[8192];
  ModuleSet set;
  set.hot().init(slab, kHotAddr, sizeof slab);
  set.cold().init(sdram, kColdAddr, sizeof sdram);
  ModuleLoader loader;
  std::vector<std::uint8_t> first;
  for (int round = 0; round < 2; ++round) {
    NEED(loader.begin(card, set, "sine", nullptr));
    while (loader.state() == ModuleLoader::State::Running) {
      loader.step(512);
    }
    NEED(loader.state() == ModuleLoader::State::Done);
    const LoadedModule* m = set.at(0);
    NEED(m != nullptr);
    CHECK(!m->hot_in_sdram);
    CHECK(m->place.hot_addr == kHotAddr);
    std::vector<std::uint8_t> image(m->place.hot_mem, m->place.hot_mem + m->header.hot_size);
    image.insert(image.end(), m->place.cold_mem, m->place.cold_mem + (m->header.image_size - m->header.hot_size));
    CHECK(std::memcmp(image.data(), f.raw.data(), image.size()) == 0);
    if (round == 0) {
      first = image;
      set.clear();
    } else {
      CHECK(image == first);
    }
  }
  fs::remove_all(root);
}

TEST_CASE("module fixture: the TB-303's file, relocated by the loader, equals the TB-303 linked at those addresses, and loads from a card") {
  const Fixtures f = fixtures_of("tb303");
  if (!f.present) {
    return;
  }
  o3m_file_header h;
  std::memcpy(&h, f.tmod.data(), sizeof h);
  NEED(check_header(h, static_cast<std::uint32_t>(f.tmod.size())) == Status::Ok);
  CHECK(h.param_count == 14);  // nine, and the five OS-SNDMOD S3 appended (ids 9..13)
  CHECK(h.hot_size > 1024);                // the voice's render loop
  CHECK(h.hot_size <= 8 * 1024);           // and it has to fit the slab (16 KiB, whose first part is the host's scratch) with room for a second module
  CHECK(h.state_bytes <= 1024);
  const fs::path root = fs::temp_directory_path() / ("o3m_tb303_fixture_" + std::to_string(reinterpret_cast<std::uintptr_t>(&f)));
  fs::remove_all(root);
  fs::create_directories(root / "modules" / "instruments");
  {
    std::ofstream out(root / "modules" / "instruments" / "tb303.tmod", std::ios::binary);
    out.write(reinterpret_cast<const char*>(f.tmod.data()), static_cast<std::streamsize>(f.tmod.size()));
  }
  o3os::platform::host::DirStorage card(root.string());
  NEED(card.mount() == o3os::hal::storage::Status::Ok);
  alignas(64) static std::uint8_t slab[16384];
  alignas(64) static std::uint8_t sdram[16384];
  ModuleSet set;
  set.hot().init(slab, kHotAddr, sizeof slab);
  set.cold().init(sdram, kColdAddr, sizeof sdram);
  ModuleLoader loader;
  NEED(loader.begin(card, set, "tb303", nullptr));
  while (loader.state() == ModuleLoader::State::Running) {
    loader.step(1024);
  }
  CHECK(loader.status() == Status::Ok);
  NEED(loader.state() == ModuleLoader::State::Done);
  const LoadedModule* m = set.at(0);
  CHECK(!m->hot_in_sdram);
  CHECK(std::memcmp(m->place.hot_mem, f.raw.data(), h.hot_size) == 0);
  CHECK(std::memcmp(m->place.cold_mem, f.raw.data() + h.hot_size, h.image_size - h.hot_size) == 0);
  CHECK(std::string(reinterpret_cast<const char*>(resolve(m->header, m->place, m->desc.name, 7))) == "TB-303");
  CHECK(m->desc.render >= kHotAddr);
  CHECK(m->desc.render < kHotAddr + h.hot_size);
  fs::remove_all(root);
}
