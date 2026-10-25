// SPDX-License-Identifier: GPL-3.0-or-later
//
// Explicit instantiation of the two rings, so that they compile (and show up in the size report) on every
// target. The members are weak, per-function sections that --gc-sections drops unless something uses them.
#include <o3os/usb/stream.h>

namespace o3os::usb {

template class OverwriteRing<StemBlock, kStemRingSlots, 1>;
template class OverwriteRing<std::uint32_t, kReturnRingFrames, kReturnMaxChunk>;

}  // namespace o3os::usb
