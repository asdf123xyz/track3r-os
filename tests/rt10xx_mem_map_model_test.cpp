// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rt10xx project region against the project model (OS-PORT-RT10XX): platform/rt10xx may not include the model (layer rule), so its
// mem_map.h mirrors the model's worst-case arena size; this test, which may use both, checks the mirror (tests/mem_map_model_test.cpp
// does the same for the MK66).
#include <doctest/doctest.h>

#include <o3os/model/project.h>
#include <o3os/platform/rt10xx/mem_map.h>

namespace mem = o3os::platform::rt10xx::mem;

TEST_CASE("rt10xx mem_map x model: the project region holds the model's worst-case arena") {
  CHECK(mem::kModelArenaWorstCaseBytes == o3os::model::kModelArenaBytes);
  CHECK(mem::kProjectSize >= o3os::model::kModelArenaBytes);
}
