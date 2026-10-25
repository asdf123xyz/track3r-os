// SPDX-License-Identifier: GPL-3.0-or-later
// OS-QANET D1 (docs/specs/OS-QANET.md section 7): writes a fixture folder into a FAT32 image file, so that the simulator can mount it as a card (`card <image>`: the unit's FatFs over the image,
// platform/host/fat/include/o3os/platform/host/fat_fs_storage.h). No mkfs and no root: the volume is written from the FAT specification's byte maps by platform_host_fat's FatImage; the image is a SPARSE
// file (only the sectors that hold something are written, the rest is a hole), so a 64 MiB card is a few hundred KB on disk.
//
//   o3fatimg <folder> <image> [--size-mb N]    the folder's tree (regular files and folders only) into a new image of N MiB (default 64; at least 33). Entries of a folder are written in byte order of
//                                              their names. A name that is not a plain upper-case 8.3 one gets long-name entries and a short alias. Exit 0; 1 with the reason on stderr.
//
// Never run on anything but a generated fixture folder: the image is written to <image> (created or truncated), nothing else is touched.

#include <o3os/platform/host/fat_image.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
  using o3os::platform::host::FatImage;
  std::string folder;
  std::string image;
  std::uint32_t mb = 64;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--size-mb") == 0 && i + 1 < argc) {
      mb = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (folder.empty()) {
      folder = argv[i];
    } else if (image.empty()) {
      image = argv[i];
    } else {
      folder.clear();
      break;
    }
  }
  if (folder.empty() || image.empty() || mb == 0u || mb > 1024u) {
    std::fprintf(stderr, "usage: o3fatimg <folder> <image> [--size-mb N]   (N = 33..1024, default 64)\n");
    return 1;
  }
  FatImage fat(mb * 2048u);
  if (!fat.ok() || !fat.add_tree(folder)) {
    std::fprintf(stderr, "o3fatimg: %s\n", fat.error().c_str());
    return 1;
  }
  std::string error;
  if (!fat.write_file(image, error)) {
    std::fprintf(stderr, "o3fatimg: %s\n", error.c_str());
    return 1;
  }
  std::printf("o3fatimg: %s: FAT32, %u MiB, %u of %u clusters used\n", image.c_str(), mb, fat.data_clusters() - fat.free_clusters(), fat.data_clusters());
  return 0;
}
