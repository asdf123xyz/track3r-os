// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MK66 project region against the project model: platform/mk66 may not include the model (layer rule), so mem_map.h
// mirrors the model arena's worst-case size and this test, in a layer that sees both, keeps the mirror honest (host only:
// the QEMU suite of the platform has no model).
#include <doctest/doctest.h>

#include <o3os/model/project.h>
#include <o3os/platform/mk66/mem_map.h>

namespace mem = o3os::platform::mk66::mem;

static_assert(mem::kProjectSize >= o3os::model::kModelArenaBytes, "the SDRAM project region holds the whole model arena");

TEST_CASE("mem_map x model: the mirrored arena size is the real one, and the project region holds it") {
  CHECK(mem::kModelArenaWorstCaseBytes == o3os::model::kModelArenaBytes);
  CHECK(mem::kProjectSize >= o3os::model::kModelArenaBytes);
  CHECK(mem::kProjectBase % alignof(o3os::model::ModelArena) == 0u);
}
