// SPDX-License-Identifier: GPL-3.0-or-later
// OS-APPUX S3: the `APPM` chunk (the parameter values of the SD apps, per project) at version 1.1. Every shipped app now stores its rows with set_own_param, so the chunk holds up to twelve apps instead of six.
// The rule held here (CLAUDE.md, persistent formats): a version bump (1.0 -> 1.1, the same layout, a larger count), a loader that still reads 1.0, and round trips. A project that holds six apps or fewer is
// still written as 1.0, byte for byte what it always was (an older OS opens it); more than six are written as 1.1; 1.0 never held more than six, so a 1.0 chunk that says seven is wrong, and a 1.1 chunk
// is bounded by what one writer piece holds (1,015 bytes), which the model keeps by dropping the oldest app.
#include <doctest/doctest.h>

#include "support/iff.h"
#include "support/memory_io.h"
#include "support/need.h"
#include "support/project_fixture.h"

#include <o3os/formats/native.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace o3os;
using namespace o3os::formats::native;
using namespace o3os::model;
using o3os::test::RawChunk;
using Bytes = std::vector<std::uint8_t>;

namespace {

Project& proj(int i) { return test::arena(i).project; }

Bytes build(const std::vector<RawChunk>& cs) { return test::build_file(cs, {}); }

RawChunk* appm(std::vector<RawChunk>& cs) {
  for (RawChunk& c : cs) {
    if (c.id == kChunkAppm) {
      return &c;
    }
  }
  return nullptr;
}

std::uint32_t bits(float f) {
  std::uint32_t b;
  std::memcpy(&b, &f, sizeof b);
  return b;
}

// The app `n` (1..12): its own name, ABI and as many values as `values` says (1..32); the values are distinct floats.
void store(Project& p, unsigned n, unsigned values, unsigned name_len = 0) {
  char name[32];
  std::snprintf(name, sizeof name, "app%02u", n);
  std::string stem = name;
  while (stem.size() < name_len) {
    stem += 'x';
  }
  float v[kMaxAppParams];
  for (unsigned i = 0; i < values; ++i) {
    v[i] = static_cast<float>(n) + 0.25f * static_cast<float>(i);
  }
  CHECK(p.set_app_params(stem.c_str(), stem.size(), 1, n % 5u, v, values) == Status::Ok);
}

// What the format's description says an APPM payload of these entries is (u16 version, u8 apps, then per app: u8 name length, the name, u8 abi major, u8 abi minor, u8 values, f32 big-endian each).
Bytes payload_of(const Project& p, std::uint16_t version) {
  Bytes b = {static_cast<std::uint8_t>(version >> 8), static_cast<std::uint8_t>(version & 0xFF), static_cast<std::uint8_t>(p.app_params_count())};
  for (std::uint32_t i = 0; i < p.app_params_count(); ++i) {
    const AppParams& a = *p.app_params(i);
    const std::size_t len = std::strlen(a.name);
    b.push_back(static_cast<std::uint8_t>(len));
    b.insert(b.end(), a.name, a.name + len);
    b.push_back(a.abi_major);
    b.push_back(a.abi_minor);
    b.push_back(a.count);
    for (unsigned k = 0; k < a.count; ++k) {
      const std::uint32_t w = bits(a.value[k]);
      for (int s = 24; s >= 0; s -= 8) {
        b.push_back(static_cast<std::uint8_t>((w >> s) & 0xFF));
      }
    }
  }
  return b;
}

bool same_apps(const Project& a, const Project& b) {
  if (a.app_params_count() != b.app_params_count()) {
    return false;
  }
  for (std::uint32_t i = 0; i < a.app_params_count(); ++i) {
    if (std::memcmp(a.app_params(i), b.app_params(i), sizeof(AppParams)) != 0) {
      return false;
    }
  }
  return true;
}

Bytes saved(Project& p) {
  Bytes out;
  CHECK(test::save(p, out) == Progress::Done);
  return out;
}

}  // namespace

TEST_CASE("appm 1.1: twelve apps round-trip bit for bit, in their order, and the second save is the first") {
  Project& p = proj(0);
  p.reset();
  for (unsigned n = 1; n <= kMaxAppParamSets; ++n) {
    store(p, n, 1 + (n * 3) % 24);
  }
  CHECK(p.app_params_count() == 12u);
  CHECK(p.app_params_bytes() <= kAppParamsBytesMax);
  const Bytes file = saved(p);
  std::vector<RawChunk> cs = test::split_chunks(file);
  RawChunk* c = appm(cs);
  NEED(c != nullptr);
  CHECK((c->payload[0] == 0x01 && c->payload[1] == 0x01));  // version 1.1
  CHECK(c->payload[2] == 12);
  CHECK(c->payload == payload_of(p, 0x0101));
  Report rep;
  NEED(test::load(test::arena(1), file, &rep) == Progress::Done);
  CHECK(same_apps(p, proj(1)));
  CHECK_FALSE(rep.newer_minor);
  CHECK(saved(proj(1)) == file);
}

TEST_CASE("appm 1.0: six apps or fewer are written as 1.0, byte for byte as before, and an older OS can open them") {
  for (unsigned apps : {1u, 3u, 6u}) {
    Project& p = proj(0);
    p.reset();
    for (unsigned n = 1; n <= apps; ++n) {
      store(p, n, 1 + n * 5, n % 2 == 0 ? 31 : 0);
    }
    const Bytes file = saved(p);
    std::vector<RawChunk> cs = test::split_chunks(file);
    RawChunk* c = appm(cs);
    NEED(c != nullptr);
    CAPTURE(apps);
    CHECK((c->payload[0] == 0x01 && c->payload[1] == 0x00));  // version 1.0
    CHECK(c->payload == payload_of(p, 0x0100));
    CHECK(c->payload.size() <= 981u);  // the longest 1.0
    NEED(test::load(test::arena(1), file, nullptr) == Progress::Done);
    CHECK(same_apps(p, proj(1)));
  }
}

TEST_CASE("appm: a 1.0 chunk as an older OS wrote it (six apps, every value count) loads; the 7th app makes the next save 1.1 and it reads back") {
  Project& p = proj(0);
  p.reset();
  for (unsigned n = 1; n <= 6; ++n) {
    store(p, n, n == 6 ? 32 : n, 31);
  }
  const Bytes v10 = saved(p);
  NEED(test::load(test::arena(1), v10, nullptr) == Progress::Done);
  CHECK(same_apps(p, proj(1)));
  CHECK(proj(1).app_params_count() == 6u);
  store(proj(1), 7, 2);
  CHECK(proj(1).app_params_count() == 7u);
  const Bytes v11 = saved(proj(1));
  std::vector<RawChunk> cs = test::split_chunks(v11);
  RawChunk* c = appm(cs);
  NEED(c != nullptr);
  CHECK((c->payload[0] == 0x01 && c->payload[1] == 0x01));
  NEED(test::load(test::arena(2), v11, nullptr) == Progress::Done);
  CHECK(same_apps(proj(1), proj(2)));
  CHECK(std::strcmp(proj(2).app_params(6)->name, "app07") == 0);
}

TEST_CASE("appm: the counts each version allows: 1.0 holds at most 6, 1.1 at most 12, neither none; a longer chunk than the version's longest is refused") {
  Project& p = proj(0);
  p.reset();
  for (unsigned n = 1; n <= 8; ++n) {
    store(p, n, 2);
  }
  const Bytes file = saved(p);
  std::vector<RawChunk> cs = test::split_chunks(file);
  RawChunk* c = appm(cs);
  NEED(c != nullptr);
  const Bytes good = c->payload;  // 1.1, eight apps
  NEED(test::load(test::arena(1), file, nullptr) == Progress::Done);
  // the same eight apps under 1.0: refused (1.0 never held more than six)
  c->payload[1] = 0x00;
  CHECK(test::load(test::arena(1), build(cs), nullptr) != Progress::Done);
  // 1.1 with a count of 0 or 13: refused
  c->payload = good;
  c->payload[2] = 0;
  CHECK(test::load(test::arena(1), build(cs), nullptr) != Progress::Done);
  c->payload = good;
  c->payload[2] = 13;
  CHECK(test::load(test::arena(1), build(cs), nullptr) != Progress::Done);
  // seven of the eight (a shorter list, the count says so): 1.1 reads it, the same bytes under 1.0 are refused
  c->payload = good;
  c->payload[2] = 7;
  const std::size_t entry = 4 + 5 + 8;  // name length byte + 5 + major + minor + count, 2 floats
  c->payload.resize(c->payload.size() - entry);
  NEED(test::load(test::arena(1), build(cs), nullptr) == Progress::Done);
  CHECK(proj(1).app_params_count() == 7u);
  c->payload[1] = 0x00;
  CHECK(test::load(test::arena(1), build(cs), nullptr) != Progress::Done);
  // a chunk past the longest of its version: 1.0 past 981 bytes, 1.1 past 1,015
  c->payload = good;
  c->payload.insert(c->payload.end(), 1015, 0);
  CHECK(test::load(test::arena(1), build(cs), nullptr) != Progress::Done);
  // a newer minor (1.2) appends: the 1.1 fields are read, the rest skipped, the report says so
  c->payload = good;
  c->payload[1] = 0x02;
  c->payload.insert(c->payload.end(), 40, 0xAB);
  Report rep;
  NEED(test::load(test::arena(1), build(cs), &rep) == Progress::Done);
  CHECK(rep.newer_minor);
  CHECK(proj(1).app_params_count() == 8u);
}

TEST_CASE("appm: the model keeps what one chunk piece holds: big entries push the oldest out, the newest are kept, and the file reads back") {
  Project& p = proj(0);
  p.reset();
  for (unsigned n = 1; n <= kMaxAppParamSets; ++n) {
    store(p, n, 32, 31);  // 4 + 31 + 128 = 163 bytes each
  }
  CHECK(p.app_params_bytes() <= kAppParamsBytesMax);
  CHECK(p.app_params_count() == 6u);  // 3 + 6 x 163 = 981 fits; a seventh would be 1,144 > 1,015
  CHECK(p.check_invariants());
  CHECK(std::strncmp(p.app_params(5)->name, "app12", 5) == 0);  // the newest is the last
  CHECK(std::strncmp(p.app_params(0)->name, "app07", 5) == 0);  // the oldest left is the 7th of twelve
  const Bytes file = saved(p);
  NEED(test::load(test::arena(1), file, nullptr) == Progress::Done);
  CHECK(same_apps(p, proj(1)));
  // small ones keep all twelve; replacing an app moves it to the end and keeps the count
  p.reset();
  for (unsigned n = 1; n <= kMaxAppParamSets; ++n) {
    store(p, n, 3);
  }
  CHECK(p.app_params_count() == 12u);
  store(p, 5, 4);
  CHECK(p.app_params_count() == 12u);
  CHECK(std::strncmp(p.app_params(11)->name, "app05", 5) == 0);
  // a 13th name drops the oldest
  float v[1] = {1.0f};
  CHECK(p.set_app_params("zzz", 3, 1, 0, v, 1) == Status::Ok);
  CHECK(p.app_params_count() == 12u);
  CHECK(p.find_app_params("app01", 5) == nullptr);
}

TEST_CASE("appm: a project with no app parameters has no APPM chunk; one with parameters but a stale count byte is refused") {
  Project& p = proj(0);
  p.reset();
  const Bytes none = saved(p);
  std::vector<RawChunk> cs = test::split_chunks(none);
  CHECK(appm(cs) == nullptr);
  store(p, 1, 2);
  const Bytes one = saved(p);
  cs = test::split_chunks(one);
  RawChunk* c = appm(cs);
  NEED(c != nullptr);
  c->payload[2] = 2;  // says two, holds one
  CHECK(test::load(test::arena(1), build(cs), nullptr) != Progress::Done);
}
